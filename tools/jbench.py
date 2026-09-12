"""Generic J2534 latency benchmark. Same UDS request/response test against the
same TCM (0x7E2/0x7EA) through whatever J2534 DLL is given on the command line.

    python jbench.py <label> <dll_path>
"""
import ctypes, struct, sys, time

CAN, PASS_FILTER, TX, RX = 5, 1, 0x7E2, 0x7EA


class MSG(ctypes.Structure):
    _fields_ = [("ProtocolID", ctypes.c_ulong), ("RxStatus", ctypes.c_ulong),
                ("TxFlags", ctypes.c_ulong), ("Timestamp", ctypes.c_ulong),
                ("DataSize", ctypes.c_ulong), ("ExtraDataIndex", ctypes.c_ulong),
                ("Data", ctypes.c_ubyte * 4128)]


def main():
    label, path = sys.argv[1], sys.argv[2]
    try:
        d = ctypes.WinDLL(path)
    except OSError as e:
        print("%-10s LOAD FAILED: %s" % (label, e)); return
    for fn, args in [
        ("PassThruOpen", [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]),
        ("PassThruConnect", [ctypes.c_ulong]*4 + [ctypes.POINTER(ctypes.c_ulong)]),
        ("PassThruStartMsgFilter", [ctypes.c_ulong, ctypes.c_ulong, ctypes.POINTER(MSG),
                                    ctypes.POINTER(MSG), ctypes.POINTER(MSG), ctypes.POINTER(ctypes.c_ulong)]),
        ("PassThruWriteMsgs", [ctypes.c_ulong, ctypes.POINTER(MSG), ctypes.POINTER(ctypes.c_ulong), ctypes.c_ulong]),
        ("PassThruReadMsgs", [ctypes.c_ulong, ctypes.POINTER(MSG), ctypes.POINTER(ctypes.c_ulong), ctypes.c_ulong]),
        ("PassThruDisconnect", [ctypes.c_ulong]), ("PassThruClose", [ctypes.c_ulong]),
        ("PassThruGetLastError", [ctypes.c_char_p])]:
        try:
            f = getattr(d, fn); f.argtypes = args; f.restype = ctypes.c_long
        except AttributeError:
            print("%-10s missing %s" % (label, fn)); return

    def err():
        b = ctypes.create_string_buffer(128)
        try: d.PassThruGetLastError(b)
        except Exception: pass
        return b.value.decode("latin1", "replace")

    def mk(cid, data=b""):
        m = MSG(); m.ProtocolID = CAN
        body = struct.pack(">I", cid) + data; m.DataSize = len(body)
        for i, b in enumerate(body): m.Data[i] = b
        return m

    dev = ctypes.c_ulong(0)
    rc = d.PassThruOpen(None, ctypes.byref(dev))
    if rc != 0:
        print("%-10s Open failed rc=%d (%s)" % (label, rc, err())); return
    ch = ctypes.c_ulong(0)
    rc = d.PassThruConnect(dev, CAN, 0, 500000, ctypes.byref(ch))
    if rc != 0:
        print("%-10s Connect failed rc=%d (%s)" % (label, rc, err()))
        d.PassThruClose(dev); return
    fid = ctypes.c_ulong(0)
    d.PassThruStartMsgFilter(ch, PASS_FILTER, ctypes.byref(mk(0x7FF)),
                             ctypes.byref(mk(0x7EA)), None, ctypes.byref(fid))

    def txn(gap):
        m = mk(TX, bytes([0x02, 0x1A, 0xB0, 0, 0, 0, 0, 0])); n = ctypes.c_ulong(1)
        t0 = time.perf_counter()
        d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 100)
        rm = (MSG*1)(); rn = ctypes.c_ulong(1)
        ok = d.PassThruReadMsgs(ch, rm, ctypes.byref(rn), 100) == 0 and rn.value
        dt = time.perf_counter() - t0
        if gap: time.sleep(gap)
        return dt, bool(ok)

    for _ in range(8): txn(0.003)             # warmup
    lat, to = [], 0
    for _ in range(150):
        dt, ok = txn(0.003)
        if ok: lat.append(dt)
        else: to += 1
    lat.sort(); n = len(lat)
    if n:
        print("%-10s median=%.2f ms  p90=%.2f  p99=%.2f  max=%.2f  timeouts=%d/150" % (
            label, lat[n//2]*1000, lat[int(n*.9)]*1000, lat[min(n-1,int(n*.99))]*1000,
            lat[-1]*1000, to))
    else:
        print("%-10s no successful transactions (timeouts=%d)" % (label, to))
    d.PassThruDisconnect(ch); d.PassThruClose(dev)


if __name__ == "__main__":
    main()
