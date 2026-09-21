"""Watch what the OP2's standalone SD-card logger puts on the bus.

The standalone logger runs entirely in OP2 firmware: it reads logcfg.txt from
the microSD card, polls the ECU by itself and writes log####.csv back to the
card. The config file has no way to express a CAN request or response ID, so
the only way to learn which addressing it uses is to watch it work.

This harness does that on a desk - no car and no ECU needed. A Kvaser sits on
the bus and plays ECU: it acknowledges the OP2's frames (without a second node
nothing is ever ACKed and the OP2 gives up) and answers every OBD request with
a plausible positive response, so the logger keeps polling. The script prints
every request with timing and ends with a summary of which IDs were used.

Wiring: OP2 on 12 V at OBD pin 16 with ground on pin 4/5, CAN-H pin 6 and
CAN-L pin 14 to the Kvaser, 120 ohm at each end. The OP2 must NOT be plugged
into USB - it will not log standalone while it is.

    python sniff_logcfg.py                  listen and answer for 60 s
    python sniff_logcfg.py --seconds 300
    python sniff_logcfg.py --passive        listen only, answer nothing
    python sniff_logcfg.py --channel 1 --bitrate 250
"""
import argparse
import sys
import time

import kvaser

# A request the logger sends arrives on one of these; a physical response goes
# back on the request id + 8, which is the ISO15765-4 convention, and a
# functional request (0x7DF) is answered by the ECM at 0x7E8.
FUNCTIONAL_ID = 0x7DF
PHYSICAL_LOW, PHYSICAL_HIGH = 0x7E0, 0x7E7
RESPONSE_OFFSET = 8

CAN_TX_ACK = 0x0040

# What we hand back for any parameter the logger asks for. The values are
# arbitrary - the point of the test is the addressing and the poll rate, not
# the data - but they are non-zero so an RPN scaling that divides still works.
FILLER = bytes([0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80])


def bitrate_of(khz):
    return {500: kvaser.canBITRATE_500K,
            250: kvaser.canBITRATE_250K,
            125: kvaser.canBITRATE_125K}[khz]


def is_request(can_id):
    return can_id == FUNCTIONAL_ID or PHYSICAL_LOW <= can_id <= PHYSICAL_HIGH


def response_id(can_id):
    # Everything on the bus that is not us comes from the OP2, so a functional
    # request is answered as if we were the engine controller.
    if can_id == FUNCTIONAL_ID:
        return PHYSICAL_LOW + RESPONSE_OFFSET
    return can_id + RESPONSE_OFFSET


def build_response(payload):
    """Turn a service payload into ISO-TP frames (single or first+consecutive)."""
    if len(payload) <= 7:
        frame = bytes([len(payload)]) + payload
        return [frame + b"\x00" * (8 - len(frame))], []
    first = bytes([0x10 | (len(payload) >> 8), len(payload) & 0xFF]) + payload[:6]
    rest, i, sn = [], 6, 1
    while i < len(payload):
        chunk = payload[i:i + 7]
        frame = bytes([0x20 | (sn & 0x0F)]) + chunk
        rest.append(frame + b"\x00" * (8 - len(frame)))
        i += 7
        sn += 1
    return [first], rest


def answer_for(req):
    """Craft a positive response to a single-frame OBD request, or None."""
    if not req or (req[0] & 0xF0) != 0x00:      # only single frames are requests
        return None
    length = req[0] & 0x0F
    body = req[1:1 + length]
    if not body:
        return None
    mode = body[0]
    if mode == 0x01:                            # mode $01: echo each PID
        pids = body[1:]
        out = bytearray([0x41])
        for pid in pids:
            out += bytes([pid]) + FILLER[:2]
        return bytes(out)
    if mode == 0x22:                            # mode $22: echo each 2-byte DID
        dids = body[1:]
        out = bytearray([0x62])
        for i in range(0, len(dids) - 1, 2):
            out += dids[i:i + 2] + FILLER[:2]
        return bytes(out)
    if mode == 0x23:                            # mode $23: read memory by address
        return bytes([0x63]) + FILLER[:4]
    if mode == 0x3E:                            # TesterPresent, if it sends one
        return bytes([0x7E, 0x00])
    if mode == 0x10:                            # session control, if it sends one
        return bytes([0x50]) + body[1:2]
    return bytes([0x7F, mode, 0x11])            # serviceNotSupported


def describe(req):
    if not req:
        return ""
    pci = req[0] & 0xF0
    if pci == 0x30:
        return "FLOW CONTROL"
    if pci != 0x00:
        return "multi-frame"
    body = req[1:1 + (req[0] & 0x0F)]
    if not body:
        return ""
    mode = body[0]
    if mode == 0x01:
        return "mode $01 PID " + " ".join("%02X" % b for b in body[1:])
    if mode == 0x22:
        return "mode $22 DID " + " ".join("%02X" % b for b in body[1:])
    if mode == 0x23:
        return "mode $23 addr " + " ".join("%02X" % b for b in body[1:])
    return "service $%02X" % mode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", type=int, default=0)
    ap.add_argument("--bitrate", type=int, default=500, choices=[500, 250, 125])
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--passive", action="store_true",
                    help="listen only; do not answer (the OP2 will time out)")
    args = ap.parse_args()

    bus = kvaser.Kvaser(args.channel, bitrate_of(args.bitrate),
                        silent=args.passive)
    print("listening on Kvaser channel %d at %d kbit%s" %
          (args.channel, args.bitrate,
           ", passive" if args.passive else ", answering as ECU"))
    print("plug the OP2 into 12 V now; Ctrl-C to stop early\n")

    seen = {}
    first_seen = {}
    pending = []            # consecutive frames waiting for a flow control
    start = time.time()
    last = None
    try:
        while time.time() - start < args.seconds:
            msg = bus.read(timeout_ms=200)
            if msg is None:
                continue
            can_id, data, flags, _ = msg
            if flags & CAN_TX_ACK:
                continue

            now = time.time() - start
            gap = "" if last is None else "  +%6.1f ms" % ((now - last) * 1000)
            last = now
            seen[can_id] = seen.get(can_id, 0) + 1
            first_seen.setdefault(can_id, now)
            print("%8.3f  %03X  %-23s %s%s" %
                  (now, can_id, " ".join("%02X" % b for b in data),
                   describe(data), gap))

            if args.passive or not data:
                continue

            if (data[0] & 0xF0) == 0x30 and pending:   # flow control for our FF
                for frame in pending:
                    bus.write(response_id(can_id), frame)
                pending = []
                continue

            if is_request(can_id):
                payload = answer_for(data)
                if payload:
                    head, pending = build_response(payload)
                    for frame in head:
                        bus.write(response_id(can_id), frame)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        bus.close()

    print("\n--- summary ---")
    if not seen:
        print("nothing on the bus. The OP2 logs only when it is NOT plugged")
        print("into USB, needs 12 V on OBD pin 16, and needs a logcfg.txt in")
        print("the root of the microSD card - check all three.")
        return 1
    elapsed = time.time() - start
    for can_id in sorted(seen):
        n = seen[can_id]
        rate = n / elapsed if elapsed else 0
        print("  %03X  %5d frames  %6.1f/s  first at %.3f s%s" %
              (can_id, n, rate, first_seen[can_id],
               "   <- REQUESTS" if is_request(can_id) else ""))
    requests = [i for i in seen if is_request(i)]
    if requests:
        print("\nthe logger transmits on: %s" %
              ", ".join("0x%03X" % i for i in sorted(requests)))
        print("that is %s addressing" %
              ("functional (broadcast)" if FUNCTIONAL_ID in requests
               else "physical"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
