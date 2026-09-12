"""Exercise the op2j2534 DLL against a real OpenPort 2.0.

Run with a 32-bit Python for the x86 build, 64-bit for the x64 build.
Set OP2_J2534_LOG to capture the wire traffic.
"""
import ctypes
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BITS = struct.calcsize("P") * 8
DEFAULT = os.path.join(HERE, "..", "build",
                       "x86" if BITS == 32 else "x64",
                       "op2j2534.dll" if BITS == 32 else "op2j2534_64.dll")

PROTO = {"J1850VPW": 1, "J1850PWM": 2, "ISO9141": 3,
         "ISO14230": 4, "CAN": 5, "ISO15765": 6}
READ_VBATT, READ_PROG_VOLTAGE = 0x03, 0x0E
GET_CONFIG, SET_CONFIG = 0x01, 0x02
LOOPBACK = 0x03


class SCONFIG(ctypes.Structure):
    _fields_ = [("Parameter", ctypes.c_ulong), ("Value", ctypes.c_ulong)]


class SCONFIG_LIST(ctypes.Structure):
    _fields_ = [("NumOfParams", ctypes.c_ulong),
                ("ConfigPtr", ctypes.POINTER(SCONFIG))]


def load(path):
    dll = ctypes.WinDLL(path)
    dll.PassThruOpen.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    dll.PassThruClose.argtypes = [ctypes.c_ulong]
    dll.PassThruConnect.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
                                    ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)]
    dll.PassThruDisconnect.argtypes = [ctypes.c_ulong]
    dll.PassThruReadVersion.argtypes = [ctypes.c_ulong, ctypes.c_char_p,
                                        ctypes.c_char_p, ctypes.c_char_p]
    dll.PassThruIoctl.argtypes = [ctypes.c_ulong, ctypes.c_ulong,
                                  ctypes.c_void_p, ctypes.c_void_p]
    dll.PassThruGetLastError.argtypes = [ctypes.c_char_p]
    for fn in ("PassThruOpen", "PassThruClose", "PassThruConnect",
               "PassThruDisconnect", "PassThruReadVersion", "PassThruIoctl",
               "PassThruGetLastError"):
        getattr(dll, fn).restype = ctypes.c_long
    return dll


def err(dll):
    b = ctypes.create_string_buffer(128)
    dll.PassThruGetLastError(b)
    return b.value.decode("latin1", "replace")


def main():
    path = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else DEFAULT)
    print("DLL    : %s" % path)
    print("Python : %d-bit\n" % BITS)
    dll = load(path)

    dev = ctypes.c_ulong(0)
    rc = dll.PassThruOpen(None, ctypes.byref(dev))
    print("PassThruOpen              -> %d  deviceId=%d  (%s)" % (rc, dev.value, err(dll)))
    if rc != 0:
        return 1

    fw = ctypes.create_string_buffer(96)
    dl = ctypes.create_string_buffer(96)
    ap = ctypes.create_string_buffer(96)
    rc = dll.PassThruReadVersion(dev, fw, dl, ap)
    print("PassThruReadVersion       -> %d  fw=%s dll=%s api=%s"
          % (rc, fw.value.decode(), dl.value.decode(), ap.value.decode()))

    volts = ctypes.c_ulong(0)
    rc = dll.PassThruIoctl(dev, READ_VBATT, None, ctypes.byref(volts))
    print("Ioctl READ_VBATT          -> %d  %d mV (%.2f V)"
          % (rc, volts.value, volts.value / 1000.0))

    rc = dll.PassThruIoctl(dev, READ_PROG_VOLTAGE, None, ctypes.byref(volts))
    print("Ioctl READ_PROG_VOLTAGE   -> %d  %d mV" % (rc, volts.value))

    for name in ("CAN", "ISO15765", "ISO9141"):
        ch = ctypes.c_ulong(0)
        baud = 10400 if name == "ISO9141" else 500000
        rc = dll.PassThruConnect(dev, PROTO[name], 0, baud, ctypes.byref(ch))
        print("Connect %-9s baud=%-6d -> %d  channelId=%d  (%s)"
              % (name, baud, rc, ch.value, err(dll) if rc else "ok"))
        if rc == 0 and name == "CAN":
            cfg = (SCONFIG * 1)(SCONFIG(LOOPBACK, 1))
            lst = SCONFIG_LIST(1, cfg)
            rc2 = dll.PassThruIoctl(ch, SET_CONFIG, ctypes.byref(lst), None)
            print("   Ioctl SET_CONFIG LOOPBACK=1 -> %d (%s)"
                  % (rc2, err(dll) if rc2 else "ok"))
            cfg2 = (SCONFIG * 1)(SCONFIG(LOOPBACK, 0))
            lst2 = SCONFIG_LIST(1, cfg2)
            rc3 = dll.PassThruIoctl(ch, GET_CONFIG, ctypes.byref(lst2), None)
            print("   Ioctl GET_CONFIG LOOPBACK   -> %d  value=%d"
                  % (rc3, cfg2[0].Value))
        if rc == 0:
            print("   Disconnect -> %d" % dll.PassThruDisconnect(ch))

    # A fourth simultaneous channel must be refused: the device has three.
    chans = []
    for name in ("CAN", "ISO15765", "ISO9141", "ISO14230"):
        ch = ctypes.c_ulong(0)
        baud = 10400 if name in ("ISO9141", "ISO14230") else 500000
        rc = dll.PassThruConnect(dev, PROTO[name], 0, baud, ctypes.byref(ch))
        print("Connect %-9s (limit test) -> %d %s"
              % (name, rc, "" if rc == 0 else "(%s)" % err(dll)))
        if rc == 0:
            chans.append(ch)
    for ch in chans:
        dll.PassThruDisconnect(ch)

    print("PassThruClose             -> %d" % dll.PassThruClose(dev))
    return 0


if __name__ == "__main__":
    sys.exit(main())
