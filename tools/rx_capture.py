"""Capture and analyse OpenPort 2.0 receive frames.

This is the tool that finishes the protocol work. The device's binary receive
frame layout is the one piece that cannot be decoded on an unconnected bench:
CAN needs a second node to acknowledge a frame, so with nothing attached no
message is ever received and no transmit ever completes.

Run this with the OP2 on a live bus - a vehicle, a bench ECU with power, or a
second adapter (a Kvaser is ideal) injecting frames. It opens a channel, dumps
every unsolicited byte the device sends with timing, and tries to infer the
frame structure.

    python rx_capture.py                     listen, raw CAN @ 500k
    python rx_capture.py --proto 6           listen, ISO15765 @ 500k
    python rx_capture.py --baud 33333        SW-CAN speed (OP2 has no SW-CAN
                                             transceiver, but the bit timing
                                             is still selectable)
    python rx_capture.py --tx 7E0:0210030000000000
                                             transmit, then capture

Everything captured is written to rx_capture.log as well as the console.
"""
import argparse
import binascii
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

VID, PID = "0403", "CC4D"

ERRORS = {
    1: "ERR_NOT_SUPPORTED", 2: "ERR_INVALID_CHANNEL_ID",
    3: "ERR_INVALID_PROTOCOL_ID", 7: "ERR_FAILED", 9: "ERR_TIMEOUT",
    10: "ERR_INVALID_MSG", 12: "ERR_EXCEEDED_LIMIT", 19: "ERR_PIN_INVALID",
    20: "ERR_CHANNEL_IN_USE", 25: "ERR_INVALID_BAUDRATE",
}


def find_port():
    for p in serial.tools.list_ports.comports():
        hw = (p.hwid or "").upper()
        if VID in hw and PID in hw:
            return p.device
    return None


def hexdump(b, indent="    "):
    out = []
    for i in range(0, len(b), 16):
        chunk = b[i:i + 16]
        h = " ".join("%02X" % c for c in chunk)
        a = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        out.append("%s%04X  %-47s |%s|" % (indent, i, h, a))
    return "\n".join(out)


class Log(object):
    def __init__(self, path):
        self.f = open(path, "w")

    def __call__(self, msg=""):
        print(msg)
        self.f.write(msg + "\n")
        self.f.flush()


def analyse(log, blob):
    """Look for the structure the vendor parser implies.

    A third-party reimplementation of this protocol reports parse failures as
    BadStartMarker / InvalidLen / UnknownDiscriminator, which says receive
    frames carry a start marker, a length and a type byte. Replies we already
    understand are ASCII and begin 'ar'.
    """
    if not blob:
        return
    log("\n--- structural analysis of %d captured bytes ---" % len(blob))

    ar = blob.count(b"ar")
    log("  occurrences of ASCII 'ar' : %d" % ar)

    # Split on CRLF; anything that is not a printable 'ar...' line is binary.
    lines = blob.replace(b"\r\n", b"\n").split(b"\n")
    ascii_lines, binary_chunks = [], []
    for l in lines:
        if not l:
            continue
        if l[:2] == b"ar" and all(32 <= c < 127 for c in l):
            ascii_lines.append(l.decode("latin1"))
        else:
            binary_chunks.append(l)

    if ascii_lines:
        log("  ASCII replies:")
        for l in ascii_lines[:40]:
            note = ""
            parts = l.split()
            if l.startswith("are") and len(parts) >= 2 and parts[1].isdigit():
                note = "   <- %s" % ERRORS.get(int(parts[1]), "?")
            log("    %s%s" % (l, note))

    if binary_chunks:
        log("  binary chunks: %d" % len(binary_chunks))
        for c in binary_chunks[:10]:
            log("    len=%d  first bytes: %s" %
                (len(c), " ".join("%02X" % x for x in c[:12])))
        firsts = {}
        for c in binary_chunks:
            if c:
                firsts[c[0]] = firsts.get(c[0], 0) + 1
        log("  leading-byte histogram (candidate start marker / type):")
        for k in sorted(firsts, key=lambda x: -firsts[x])[:10]:
            log("    0x%02X : %d" % (k, firsts[k]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proto", type=int, default=5,
                    help="5=CAN (default), 6=ISO15765, 3=ISO9141, 4=ISO14230")
    ap.add_argument("--baud", type=int, default=500000)
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--tx", default=None,
                    help="transmit before listening, as ID:HEXBYTES e.g. 7E0:0210030000000000")
    ap.add_argument("--loopback", action="store_true",
                    help="set LOOPBACK=1 on the channel first")
    args = ap.parse_args()

    port = find_port()
    if not port:
        sys.exit("no OpenPort 2.0 found (VID_%s&PID_%s)" % (VID, PID))

    log = Log("rx_capture.log")
    s = serial.Serial(port, 115200, timeout=0.05, write_timeout=2)
    s.dtr = s.rts = True
    time.sleep(0.15)
    s.reset_input_buffer()

    seq = [0]

    def cmd(line, payload=b"", wait=0.5):
        seq[0] += 1
        s.reset_input_buffer()
        s.write(("%s %d" % (line, seq[0])).encode() + b"\r\n" + payload)
        s.flush()
        end = time.time() + wait
        buf = b""
        while time.time() < end:
            b = s.read(256)
            if b:
                buf += b
                end = time.time() + 0.12
        return buf

    log("OpenPort 2.0 on %s" % port)
    log("ati -> %s" % cmd("ati").decode("latin1", "replace").strip())

    vb = cmd("atr 16").decode("latin1", "replace").strip()
    log("battery (pin 16) -> %s" % vb)
    try:
        mv = int(vb.split()[2])
        if mv < 6000:
            log("")
            log("  WARNING: battery pin reads %d mV." % mv)
            log("  Nothing appears to be connected. On an unpowered bench no")
            log("  frame can be received and every transmit will time out,")
            log("  because CAN needs a second node to acknowledge a frame.")
            log("")
    except (IndexError, ValueError):
        pass

    cmd("ata")                                   # release stale channels
    r = cmd("ato%d %d 0 0" % (args.proto, args.baud)).decode("latin1", "replace").strip()
    log("open proto=%d baud=%d -> %s" % (args.proto, args.baud, r))
    if "aro" not in r:
        log("channel did not open; stopping")
        return 1

    if args.loopback:
        log("loopback -> %s" % cmd("ats%d 3 1" % args.proto)
            .decode("latin1", "replace").strip())

    if args.tx:
        can_id, _, data_hex = args.tx.partition(":")
        data = binascii.unhexlify(data_hex)
        body = int(can_id, 16).to_bytes(4, "big") + data
        log("\ntransmitting id=%s data=%s (body %d bytes)"
            % (can_id, data_hex, len(body)))
        # att<proto> <len> <txflags> <extradataindex> <seq> + raw body.
        # Argument 1 is confirmed to be the byte count; the other two are the
        # fields still to be confirmed here.
        out = cmd("att%d %d 0 0" % (args.proto, len(body)), body, wait=1.2)
        log("  reply: %r" % out)
        log(hexdump(out))

    log("\nlistening for %.1f s ..." % args.seconds)
    blob = b""
    t0 = time.time()
    last = t0
    while time.time() - t0 < args.seconds:
        b = s.read(512)
        if b:
            now = time.time()
            log("  +%7.3f s  %d bytes" % (now - t0, len(b)))
            log(hexdump(b))
            blob += b
            last = now
        else:
            time.sleep(0.01)

    log("\ntotal captured: %d bytes" % len(blob))
    analyse(log, blob)

    cmd("ata")
    s.close()
    log("\nwritten to rx_capture.log")
    return 0


if __name__ == "__main__":
    sys.exit(main())
