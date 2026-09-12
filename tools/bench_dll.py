"""Benchmark the op2j2534 DLL on a live bus: latency and ISO-TP throughput."""
import ctypes, os, struct, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
BITS = struct.calcsize("P") * 8
DLL = os.path.join(HERE, "..", "build", "x86" if BITS == 32 else "x64",
                   "op2j2534.dll" if BITS == 32 else "op2j2534_64.dll")
CAN, PASS_FILTER, TX, RX = 5, 1, 0x7E2, 0x7EA


class MSG(ctypes.Structure):
    _fields_ = [("ProtocolID", ctypes.c_ulong), ("RxStatus", ctypes.c_ulong),
                ("TxFlags", ctypes.c_ulong), ("Timestamp", ctypes.c_ulong),
                ("DataSize", ctypes.c_ulong), ("ExtraDataIndex", ctypes.c_ulong),
                ("Data", ctypes.c_ubyte * 4128)]


def load():
    d = ctypes.WinDLL(os.path.abspath(DLL))
    d.PassThruOpen.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    d.PassThruConnect.argtypes = [ctypes.c_ulong]*4 + [ctypes.POINTER(ctypes.c_ulong)]
    d.PassThruStartMsgFilter.argtypes = [ctypes.c_ulong, ctypes.c_ulong,
        ctypes.POINTER(MSG), ctypes.POINTER(MSG), ctypes.POINTER(MSG),
        ctypes.POINTER(ctypes.c_ulong)]
    d.PassThruWriteMsgs.argtypes = [ctypes.c_ulong, ctypes.POINTER(MSG),
        ctypes.POINTER(ctypes.c_ulong), ctypes.c_ulong]
    d.PassThruReadMsgs.argtypes = [ctypes.c_ulong, ctypes.POINTER(MSG),
        ctypes.POINTER(ctypes.c_ulong), ctypes.c_ulong]
    return d


def mk(cid, data=b""):
    m = MSG(); m.ProtocolID = CAN
    body = struct.pack(">I", cid) + data
    m.DataSize = len(body)
    for i, b in enumerate(body):
        m.Data[i] = b
    return m


def main():
    d = load()
    dev = ctypes.c_ulong(0); d.PassThruOpen(None, ctypes.byref(dev))
    ch = ctypes.c_ulong(0); d.PassThruConnect(dev, CAN, 0, 500000, ctypes.byref(ch))
    fid = ctypes.c_ulong(0)
    d.PassThruStartMsgFilter(ch, PASS_FILTER, ctypes.byref(mk(0x7FF)),
                             ctypes.byref(mk(0x7EA)), None, ctypes.byref(fid))

    def txn(payload):
        frame = bytes([len(payload)]) + bytes(payload); frame += bytes(8-len(frame))
        m = mk(TX, frame); n = ctypes.c_ulong(1)
        t0 = time.perf_counter()
        d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 100)
        rm = (MSG*1)(); rn = ctypes.c_ulong(1)   # single-frame response: want 1
        if d.PassThruReadMsgs(ch, rm, ctypes.byref(rn), 100) == 0 and rn.value:
            data = bytes(rm[0].Data[:rm[0].DataSize])
            return time.perf_counter()-t0, data[4:]
        return time.perf_counter()-t0, None

    for _ in range(5): txn([0x1A, 0xB0])   # warmup

    lat = sorted(txn([0x1A, 0xB0])[0] for _ in range(100))
    print("single request/response (n=100):")
    print("  min=%.2f  median=%.2f  p90=%.2f  max=%.2f ms  -> %.0f txn/s" % (
        lat[0]*1000, lat[50]*1000, lat[90]*1000, lat[-1]*1000, 1/lat[50]))

    # ISO-TP multiframe read timing (VIN, ~19 bytes)
    def isotp(did):
        t0 = time.perf_counter()
        m = mk(TX, bytes([2, 0x1A, did]) + bytes(5)); n = ctypes.c_ulong(1)
        d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 100)
        total, out, sent_fc = None, bytearray(), False
        while time.perf_counter()-t0 < 0.5:
            rm = (MSG*1)(); rn = ctypes.c_ulong(1)
            if d.PassThruReadMsgs(ch, rm, ctypes.byref(rn), 100) == 0:
                for i in range(rn.value):
                    data = bytes(rm[i].Data[:rm[i].DataSize])
                    if struct.unpack(">I", data[:4])[0] != RX: continue
                    p = data[4:]
                    if (p[0] >> 4) == 1:
                        total = ((p[0] & 0xF) << 8) | p[1]; out += p[2:]
                        fc = mk(TX, bytes([0x30,0,0,0,0,0,0,0])); nn = ctypes.c_ulong(1)
                        d.PassThruWriteMsgs(ch, ctypes.byref(fc), ctypes.byref(nn), 100)
                    elif (p[0] >> 4) == 2:
                        out += p[1:]
                    elif (p[0] >> 4) == 0:
                        return time.perf_counter()-t0
                if total and len(out) >= total:
                    return time.perf_counter()-t0
        return time.perf_counter()-t0

    tt = sorted(isotp(0x90) for _ in range(30))
    print("\nISO-TP multiframe read (19-byte VIN, incl flow control), n=30:")
    print("  median=%.2f ms  -> %.0f reads/s" % (tt[15]*1000, 1/tt[15]))

    d.PassThruDisconnect(ch); d.PassThruClose(dev)


if __name__ == "__main__":
    main()
