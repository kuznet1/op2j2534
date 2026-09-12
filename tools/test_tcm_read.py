"""Read a Gen2 Saab TCM through the op2j2534 DLL's J2534 API.

Exercises the full path an application (e.g. TCMFlasher) uses:
Connect -> StartMsgFilter -> WriteMsgs -> ReadMsgs, with software ISO-TP.
Run under a 32-bit Python for the x86 DLL.
"""
import ctypes
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BITS = struct.calcsize("P") * 8
DLL = os.path.join(HERE, "..", "build", "x86" if BITS == 32 else "x64",
                   "op2j2534.dll" if BITS == 32 else "op2j2534_64.dll")

CAN = 5
PASS_FILTER = 1
TX, RX = 0x7E2, 0x7EA


class PASSTHRU_MSG(ctypes.Structure):
    _fields_ = [("ProtocolID", ctypes.c_ulong), ("RxStatus", ctypes.c_ulong),
                ("TxFlags", ctypes.c_ulong), ("Timestamp", ctypes.c_ulong),
                ("DataSize", ctypes.c_ulong), ("ExtraDataIndex", ctypes.c_ulong),
                ("Data", ctypes.c_ubyte * 4128)]


def load():
    d = ctypes.WinDLL(os.path.abspath(DLL))
    d.PassThruOpen.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    d.PassThruConnect.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
                                  ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)]
    d.PassThruStartMsgFilter.argtypes = [ctypes.c_ulong, ctypes.c_ulong,
                                         ctypes.POINTER(PASSTHRU_MSG),
                                         ctypes.POINTER(PASSTHRU_MSG),
                                         ctypes.POINTER(PASSTHRU_MSG),
                                         ctypes.POINTER(ctypes.c_ulong)]
    d.PassThruWriteMsgs.argtypes = [ctypes.c_ulong, ctypes.POINTER(PASSTHRU_MSG),
                                    ctypes.POINTER(ctypes.c_ulong), ctypes.c_ulong]
    d.PassThruReadMsgs.argtypes = [ctypes.c_ulong, ctypes.POINTER(PASSTHRU_MSG),
                                   ctypes.POINTER(ctypes.c_ulong), ctypes.c_ulong]
    d.PassThruGetLastError.argtypes = [ctypes.c_char_p]
    for fn in ("PassThruOpen", "PassThruConnect", "PassThruStartMsgFilter",
               "PassThruWriteMsgs", "PassThruReadMsgs", "PassThruClose",
               "PassThruDisconnect", "PassThruGetLastError"):
        getattr(d, fn).restype = ctypes.c_long
    return d


def mk(canid, data=b""):
    m = PASSTHRU_MSG()
    m.ProtocolID = CAN
    body = struct.pack(">I", canid) + data
    m.DataSize = len(body)
    for i, b in enumerate(body):
        m.Data[i] = b
    return m


def main():
    d = load()
    dev = ctypes.c_ulong(0)
    assert d.PassThruOpen(None, ctypes.byref(dev)) == 0, "open failed"
    ch = ctypes.c_ulong(0)
    rc = d.PassThruConnect(dev, CAN, 0, 500000, ctypes.byref(ch))
    print("Connect CAN 500k -> %d channel=%d" % (rc, ch.value))

    # Pass filter for 0x7EA (the TCM's response id): mask 0x7FF, pattern 0x7EA
    mask = mk(0x7FF)
    patt = mk(0x7EA)
    fid = ctypes.c_ulong(0)
    rc = d.PassThruStartMsgFilter(ch, PASS_FILTER, ctypes.byref(mask),
                                  ctypes.byref(patt), None, ctypes.byref(fid))
    print("StartMsgFilter   -> %d filterId=%d" % (rc, fid.value))

    def request(payload, label):
        # single-frame ISO-TP request
        frame = bytes([len(payload)]) + bytes(payload)
        frame += bytes(8 - len(frame))
        m = mk(TX, frame)
        n = ctypes.c_ulong(1)
        rc = d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 200)
        # read response(s)
        got = []
        deadline = time.time() + 1.0
        first = None
        while time.time() < deadline:
            rmsgs = (PASSTHRU_MSG * 8)()
            rn = ctypes.c_ulong(8)
            r = d.PassThruReadMsgs(ch, rmsgs, ctypes.byref(rn), 200)
            if r == 0:
                for i in range(rn.value):
                    rm = rmsgs[i]
                    data = bytes(rm.Data[:rm.DataSize])
                    cid = struct.unpack(">I", data[:4])[0]
                    payload = data[4:]
                    if cid == RX:
                        got.append(payload)
                if got:
                    break
        print("\n%s  (write rc=%d)" % (label, rc))
        if not got:
            print("   no response")
            return None
        d0 = got[0]
        pci = d0[0] >> 4
        if pci == 0:
            n = d0[0] & 0xF
            print("   single frame: %s" % d0[1:1 + n].hex().upper())
            return d0[1:1 + n]
        if pci == 1:
            total = ((d0[0] & 0xF) << 8) | d0[1]
            out = bytearray(d0[2:])
            # send flow control
            fc = mk(TX, bytes([0x30, 0x00, 0x00, 0, 0, 0, 0, 0]))
            nn = ctypes.c_ulong(1)
            d.PassThruWriteMsgs(ch, ctypes.byref(fc), ctypes.byref(nn), 200)
            deadline = time.time() + 1.0
            while len(out) < total and time.time() < deadline:
                rmsgs = (PASSTHRU_MSG * 16)()
                rn = ctypes.c_ulong(16)
                if d.PassThruReadMsgs(ch, rmsgs, ctypes.byref(rn), 200) == 0:
                    for i in range(rn.value):
                        rm = rmsgs[i]
                        data = bytes(rm.Data[:rm.DataSize])
                        cid = struct.unpack(">I", data[:4])[0]
                        p = data[4:]
                        if cid == RX and (p[0] >> 4) == 2:
                            out += p[1:]
            full = bytes(out[:total])
            txt = "".join(chr(x) if 32 <= x < 127 else "." for x in full)
            print("   multi-frame (%d bytes): %s  |%s|" % (total, full.hex().upper(), txt))
            return full
        return None

    request([0x3E, 0x00], "TesterPresent 3E00")
    request([0x1A, 0x90], "ReadDataByID 1A90 (VIN)")
    request([0x1A, 0x92], "ReadDataByID 1A92 (supplier)")
    request([0x1A, 0xB0], "ReadDataByID 1AB0 (diagnostic address)")
    request([0x1A, 0x98], "ReadDataByID 1A98")

    d.PassThruDisconnect(ch)
    d.PassThruClose(dev)
    print("\ndone.")


if __name__ == "__main__":
    main()
