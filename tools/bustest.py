"""Is this OpenPort actually on the CAN bus? One command, one verdict.

Uses a Kvaser as a reference node: it proves the bus is alive, acknowledges
the OpenPort's frames, and injects traffic so we can see whether the OpenPort
receives anything.

The decisive test is the acknowledge. A CAN frame is acknowledged at bit level
by any active node on the bus, whatever its ID and regardless of filters. So
if the OpenPort transmits and nothing acknowledges while the Kvaser can see
other modules talking, the OpenPort is not electrically on those wires - and
no amount of software configuration will change that.

    python bustest.py                 500 kbit, Kvaser channel 0
    python bustest.py --baud 250000
    python bustest.py --kv-channel 1
    python bustest.py --no-kvaser     OpenPort only (bus liveness unknown)
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

VID, PID = "0403", "CC4D"
ERRORS = {1: "ERR_NOT_SUPPORTED", 2: "ERR_INVALID_CHANNEL_ID",
          3: "ERR_INVALID_PROTOCOL_ID", 7: "ERR_FAILED", 9: "ERR_TIMEOUT",
          10: "ERR_INVALID_MSG", 12: "ERR_EXCEEDED_LIMIT", 13: "ERR_INVALID_MSG_ID",
          19: "ERR_PIN_INVALID", 20: "ERR_CHANNEL_IN_USE", 22: "ERR_INVALID_FILTER_ID",
          25: "ERR_INVALID_BAUDRATE"}


def openports():
    """Every attached OpenPort 2.0, as (COM port, USB serial)."""
    found = []
    for p in serial.tools.list_ports.comports():
        hw = (p.hwid or "").upper()
        if VID in hw and PID in hw:
            serial_no = ""
            for part in hw.replace("\\", " ").split():
                if part and part not in ("USB", "VID_0403", "PID_CC4D") \
                        and "VID" not in part and "PID" not in part \
                        and "SER=" not in part:
                    pass
            if "SER=" in hw:
                serial_no = hw.split("SER=")[1].split()[0]
            found.append((p.device, serial_no))
    return found


class OP2(object):
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.05, write_timeout=2)
        self.s.dtr = self.s.rts = True
        self.seq = 0
        time.sleep(0.15)
        self.s.reset_input_buffer()

    def _read(self, wait):
        end = time.time() + wait
        buf = b""
        while time.time() < end:
            b = self.s.read(512)
            if b:
                buf += b
                end = time.time() + 0.14
        return buf

    def cmd(self, line, payload=b"", wait=0.9):
        self.seq += 1
        self.s.reset_input_buffer()
        self.s.write(("%s %d" % (line, self.seq)).encode() + b"\r\n" + payload)
        self.s.flush()
        return self._read(wait)

    def lines(self, blob):
        return [l.decode("latin1", "replace")
                for l in blob.replace(b"\r\n", b"\n").split(b"\n") if l]

    def close(self):
        try:
            self.cmd("ata", wait=0.3)
            self.s.close()
        except Exception:
            pass


def explain(lines):
    out = []
    for l in lines:
        parts = l.split()
        if l.startswith("are") and len(parts) >= 2 and parts[1].isdigit():
            out.append("%s (%s)" % (l, ERRORS.get(int(parts[1]), "?")))
        else:
            out.append(l)
    return "; ".join(out) if out else "<no reply>"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baud", type=int, default=500000)
    ap.add_argument("--kv-channel", type=int, default=0)
    ap.add_argument("--no-kvaser", action="store_true")
    ap.add_argument("--tx-id", default="7E2", help="CAN id to transmit to")
    args = ap.parse_args()

    print("=" * 68)
    devs = openports()
    if not devs:
        sys.exit("no OpenPort 2.0 found (VID_%s&PID_%s) - is it plugged in?" % (VID, PID))
    for port, ser in devs:
        print("OpenPort found : %s   USB serial: %s" % (port, ser or "?"))
    if len(devs) > 1:
        print("NOTE: more than one attached; using the first.")
    port = devs[0][0]

    op = OP2(port)
    fw = op.lines(op.cmd("ati"))
    print("Firmware       : %s" % (fw[0] if fw else "<no reply>"))

    vb = op.lines(op.cmd("atr 16"))
    mv = 0
    try:
        mv = int(vb[0].split()[2])
    except Exception:
        pass
    print("Battery pin 16 : %d mV%s" % (mv, "" if mv > 6000 else "   <-- no vehicle power!"))

    kv = None
    bus_frames = []
    if not args.no_kvaser:
        try:
            import kvaser as kvmod
            rate = {500000: kvmod.canBITRATE_500K, 250000: kvmod.canBITRATE_250K,
                    125000: kvmod.canBITRATE_125K}.get(args.baud, kvmod.canBITRATE_500K)
            kv = kvmod.Kvaser(args.kv_channel, rate)
            print("Kvaser         : channel %d bus-on @ %d" % (args.kv_channel, args.baud))
            t0 = time.time()
            while time.time() - t0 < 1.5:
                m = kv.read(80)
                if m:
                    bus_frames.append(m)
            ids = sorted({("%03X" % f[0]) for f in bus_frames})
            print("Bus traffic    : %d frames, ids: %s"
                  % (len(bus_frames), ", ".join(ids) if ids else "(none)"))
        except Exception as e:
            print("Kvaser         : unavailable (%s)" % e)
            kv = None

    op.cmd("ata", wait=0.3)
    opened = op.lines(op.cmd("ato5 %d 0 0" % args.baud, wait=0.6))
    print("OP2 CAN open   : %s" % explain(opened))
    if not any(x.startswith("aro") for x in opened):
        op.close()
        if kv:
            kv.close()
        sys.exit("channel would not open; stopping")

    # --- the decisive test: does anything acknowledge our frame? ---
    body = struct.pack(">I", int(args.tx_id, 16)) + bytes([0x02, 0x10, 0x03, 0, 0, 0, 0, 0])
    tx = op.lines(op.cmd("att5 12 0 0", body, wait=1.4))
    acked = any(x.startswith("aro") for x in tx)
    print("OP2 transmit   : %s" % explain(tx))

    saw_ours = []
    if kv:
        t0 = time.time()
        while time.time() - t0 < 0.6:
            m = kv.read(60)
            if m and m[0] == int(args.tx_id, 16):
                saw_ours.append(m)
        print("Kvaser saw ours: %s" % ("yes" if saw_ours else "no"))

    # --- can the OpenPort receive? ---
    rxb = b""
    if kv:
        op.s.reset_input_buffer()
        for _ in range(6):
            kv.write(0x7EA, bytes([0x03, 0x7F, 0x10, 0x11, 0, 0, 0, 0]))
            time.sleep(0.08)
        time.sleep(0.5)
        rxb = op.s.read(8192)
    print("OP2 received   : %d bytes" % len(rxb))
    if rxb:
        for i in range(0, min(len(rxb), 256), 16):
            c = rxb[i:i + 16]
            print("    %04X  %-47s |%s|" % (i, " ".join("%02X" % x for x in c),
                                            "".join(chr(x) if 32 <= x < 127 else "." for x in c)))

    print("=" * 68)
    print("VERDICT")
    if mv < 6000:
        print("  No vehicle power at the OpenPort's connector (pin 16 = %d mV)." % mv)
        print("  Connect it to a powered bus before drawing any conclusion.")
    elif acked or rxb:
        print("  THIS OPENPORT IS ON THE BUS.")
        if acked:
            print("    - its transmit was acknowledged by another node")
        if rxb:
            print("    - it received %d bytes of injected traffic" % len(rxb))
        print("  The receive frame layout can now be decoded from the capture above.")
    elif bus_frames:
        print("  THIS OPENPORT IS *NOT* ON THE BUS.")
        print("    - the Kvaser sees %d frames, so the bus is alive and other" % len(bus_frames))
        print("      nodes are active")
        print("    - yet this OpenPort's frame was never acknowledged, and it")
        print("      received nothing")
        print("  Any active node acknowledges any frame, so this is electrical:")
        print("  pins 6/14 are not reaching those wires. Suspect the OBD cable")
        print("  or the harness, not the software.")
    else:
        print("  INCONCLUSIVE - no acknowledge, and no reference traffic seen.")
        print("  Check the Kvaser is on the same bus and at the same bit rate.")
    print("=" * 68)

    op.close()
    if kv:
        kv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
