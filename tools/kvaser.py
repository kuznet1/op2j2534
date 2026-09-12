"""Minimal Kvaser canlib wrapper via ctypes (no canlib Python package needed)."""
import ctypes
import os

canOPEN_ACCEPT_VIRTUAL = 0x0020
canMSG_STD = 0x0002
canMSG_EXT = 0x0004
canBITRATE_500K = -2
canBITRATE_250K = -3
canBITRATE_125K = -4

_dll = None


def _load():
    global _dll
    if _dll is None:
        _dll = ctypes.WinDLL("canlib32.dll")
        _dll.canInitializeLibrary()
        _dll.canOpenChannel.restype = ctypes.c_int
        _dll.canOpenChannel.argtypes = [ctypes.c_int, ctypes.c_int]
        _dll.canSetBusParams.argtypes = [ctypes.c_int, ctypes.c_long,
                                         ctypes.c_uint, ctypes.c_uint,
                                         ctypes.c_uint, ctypes.c_uint,
                                         ctypes.c_uint]
        _dll.canWrite.argtypes = [ctypes.c_int, ctypes.c_long, ctypes.c_void_p,
                                  ctypes.c_uint, ctypes.c_uint]
        _dll.canReadWait.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_long),
                                     ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint),
                                     ctypes.POINTER(ctypes.c_uint),
                                     ctypes.POINTER(ctypes.c_ulong),
                                     ctypes.c_ulong]
    return _dll


def channel_count():
    d = _load()
    n = ctypes.c_int(0)
    d.canGetNumberOfChannels(ctypes.byref(n))
    return n.value


def channel_name(ch):
    d = _load()
    buf = ctypes.create_string_buffer(64)
    # canCHANNELDATA_DEVDESCR_ASCII = 26
    d.canGetChannelData(ch, 26, buf, ctypes.sizeof(buf))
    return buf.value.decode("latin1", "replace")


class Kvaser(object):
    def __init__(self, channel=0, bitrate=canBITRATE_500K, silent=False):
        d = _load()
        self.d = d
        self.h = d.canOpenChannel(channel, canOPEN_ACCEPT_VIRTUAL)
        if self.h < 0:
            raise RuntimeError("canOpenChannel(%d) failed: %d" % (channel, self.h))
        st = d.canSetBusParams(self.h, bitrate, 0, 0, 0, 0, 0)
        if st != 0:
            raise RuntimeError("canSetBusParams failed: %d" % st)
        if silent:
            d.canSetBusOutputControl(self.h, 1)   # canDRIVER_SILENT
        st = d.canBusOn(self.h)
        if st != 0:
            raise RuntimeError("canBusOn failed: %d" % st)

    def write(self, can_id, data, ext=False):
        buf = (ctypes.c_ubyte * len(data))(*data)
        flag = canMSG_EXT if ext else canMSG_STD
        return self.d.canWrite(self.h, can_id, buf, len(data), flag)

    def read(self, timeout_ms=200):
        cid = ctypes.c_long(0)
        data = (ctypes.c_ubyte * 64)()
        dlc = ctypes.c_uint(0)
        flag = ctypes.c_uint(0)
        t = ctypes.c_ulong(0)
        st = self.d.canReadWait(self.h, ctypes.byref(cid), data,
                                ctypes.byref(dlc), ctypes.byref(flag),
                                ctypes.byref(t), timeout_ms)
        if st != 0:
            return None
        return cid.value, bytes(bytearray(data[:dlc.value])), flag.value, t.value

    def close(self):
        try:
            self.d.canBusOff(self.h)
            self.d.canClose(self.h)
        except Exception:
            pass
