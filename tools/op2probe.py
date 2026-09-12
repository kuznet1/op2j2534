"""Talk the raw OpenPort 2.0 AT protocol over its CDC serial port.

Useful for protocol work and for sanity-checking hardware without involving
the DLL at all. Requires pyserial.

    python op2probe.py                 interactive shell
    python op2probe.py ati "atr 16"    run commands and exit

Commands are sent with an automatic trailing sequence number, which the device
echoes back; a reply carrying that sequence proves the command was parsed with
the right arity. Prefix a command with '!' to send it verbatim with no
sequence number appended.
"""
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

VID_PID = ("0403", "CC4D")

# J2534 error codes, which is what this device actually returns.
ERRORS = {
    1: "ERR_NOT_SUPPORTED", 2: "ERR_INVALID_CHANNEL_ID",
    3: "ERR_INVALID_PROTOCOL_ID", 4: "ERR_NULL_PARAMETER",
    5: "ERR_INVALID_IOCTL_VALUE", 6: "ERR_INVALID_FLAGS", 7: "ERR_FAILED",
    8: "ERR_DEVICE_NOT_CONNECTED", 9: "ERR_TIMEOUT", 10: "ERR_INVALID_MSG",
    11: "ERR_INVALID_TIME_INTERVAL", 12: "ERR_EXCEEDED_LIMIT",
    13: "ERR_INVALID_MSG_ID", 14: "ERR_DEVICE_IN_USE",
    15: "ERR_INVALID_IOCTL_ID", 16: "ERR_BUFFER_EMPTY", 17: "ERR_BUFFER_FULL",
    18: "ERR_BUFFER_OVERFLOW", 19: "ERR_PIN_INVALID", 20: "ERR_CHANNEL_IN_USE",
    21: "ERR_MSG_PROTOCOL_ID", 22: "ERR_INVALID_FILTER_ID",
    23: "ERR_NO_FLOW_CONTROL", 24: "ERR_NOT_UNIQUE", 25: "ERR_INVALID_BAUDRATE",
    26: "ERR_INVALID_DEVICE_ID",
}


def find_port():
    for p in serial.tools.list_ports.comports():
        hw = (p.hwid or "").upper()
        if VID_PID[0] in hw and VID_PID[1] in hw:
            return p.device
    return None


class OpenPort(object):
    def __init__(self, port=None, reset=False):
        port = port or find_port()
        if not port:
            raise SystemExit("no OpenPort 2.0 found (VID_%s&PID_%s)" % VID_PID)
        self.port = port
        self.s = serial.Serial(port, 115200, timeout=0.05, write_timeout=2)
        self.s.dtr = self.s.rts = True
        self.seq = 0
        time.sleep(0.15)
        self.s.reset_input_buffer()
        if reset:
            self.raw("atz")
            self.s.close()
            time.sleep(1.6)
            self.s = serial.Serial(port, 115200, timeout=0.05, write_timeout=2)
            self.s.dtr = self.s.rts = True

    def read_lines(self, wait=0.45):
        end = time.time() + wait
        buf = b""
        while time.time() < end:
            b = self.s.read(256)
            if b:
                buf += b
                end = time.time() + 0.12
        return [l.decode("latin1", "replace")
                for l in buf.replace(b"\r\n", b"\n").split(b"\n") if l]

    def raw(self, line, payload=b"", wait=0.45):
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\r\n" + payload)
        self.s.flush()
        return self.read_lines(wait)

    def cmd(self, line, payload=b"", wait=0.45):
        self.seq += 1
        return self.raw("%s %d" % (line, self.seq), payload, wait)

    def resync(self):
        """Recover after a rejected command left a binary payload unread."""
        for _ in range(4):
            self.s.reset_input_buffer()
            self.s.write(b"\r\n")
            self.s.flush()
            time.sleep(0.1)
            if any(r.startswith("ari") for r in self.raw("ati", wait=0.3)):
                return True
        return False


def explain(lines):
    out = []
    for l in lines:
        if l.startswith("are"):
            parts = l.split()
            if len(parts) >= 2 and parts[1].isdigit():
                code = int(parts[1])
                out.append("%s   <- %s" % (l, ERRORS.get(code, "?")))
                continue
        out.append(l)
    return out


def main():
    dev = OpenPort()
    print("OpenPort 2.0 on %s" % dev.port)
    print("  %s" % "; ".join(dev.raw("ati")))

    args = [a for a in sys.argv[1:]]
    if args:
        for a in args:
            verbatim = a.startswith("!")
            line = a[1:] if verbatim else a
            rep = dev.raw(line) if verbatim else dev.cmd(line)
            print("  %-28s -> %s" % (a, "; ".join(explain(rep)) or "<no reply>"))
        return

    print("type commands ('!cmd' sends verbatim, 'quit' exits)")
    while True:
        try:
            line = input("op2> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not line:
            continue
        if line in ("quit", "exit"):
            break
        if line == "resync":
            print("  sync:", dev.resync())
            continue
        verbatim = line.startswith("!")
        rep = dev.raw(line[1:]) if verbatim else dev.cmd(line)
        print("  %s" % ("; ".join(explain(rep)) or "<no reply>"))


if __name__ == "__main__":
    main()
