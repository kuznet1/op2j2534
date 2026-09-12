# Contributing to op2j2534

Thanks for helping keep the OpenPort 2.0 usable. Bug reports, protocol findings,
and pull requests are all welcome.

## Legal / provenance

- Contributions are accepted under the project's **Apache-2.0** license.
- Keep it **clean-room**. This project was built by observing the device and by
  lawful reverse-engineering for interoperability. **Do not paste vendor source,
  headers, or decompiler output** into the repo, and don't commit any vendor
  binaries. Describe *observed behaviour* and *wire bytes* — those are facts, not
  someone's copyrighted code.
- Not affiliated with Tactrix; only nominative references to the hardware.

## Building

Requires Visual Studio with the C++ toolset and the Windows SDK (CI uses
`windows-latest`; developed against VS 2022/18 + SDK 10.0.26100).

```
build.bat
```

Produces `build\x86\op2j2534.dll` (32-bit) and `build\x64\op2j2534_64.dll`
(64-bit). Every push and PR is built by CI (`.github/workflows/build.yml`), and
tagging `vX.Y.Z` publishes a release with both DLLs attached.

## Testing (needs the hardware)

The Python tools under `tools/` talk to a real OpenPort 2.0. Use a **32-bit**
Python for the x86 DLL, 64-bit for the x64 DLL.

| Tool | What it does |
|---|---|
| `tools/op2probe.py` | raw AT-protocol shell — the fastest way to poke the device |
| `tools/test_dll.py` | exercises the DLL API (open, connect, voltage, config) |
| `tools/test_tcm_read.py` | full ISO-TP read of a Gen2 TCM through the DLL |
| `tools/bustest.py` | one-command "is this adapter actually on the bus?" verdict |
| `tools/rx_capture.py` | capture and decode received frames |
| `tools/jbench.py` | latency benchmark; works against any J2534 DLL for comparison |

Set `OP2_J2534_LOG=<path>` to have the DLL log every API call and every byte on
the wire — invaluable when debugging.

## Code layout

```
include/j2534.h   SAE J2534 v04.04 API definitions
src/op2_port.c    COM-port discovery and serial I/O
src/op2_proto.c   AT protocol: framing, sequence numbers, RX stream tokenizer
src/j2534_api.c   the 14 exported PassThru* entry points
PROTOCOL.md       the reverse-engineered wire protocol (read this first)
```

## Things that will bite you (learned the hard way)

- **Never `Sleep(1)` in the read path.** At Windows' default 15.6 ms timer
  granularity that sleeps ~15.6 ms and pins every transaction at ~16 ms. The
  serial reads block in the kernel via `COMMTIMEOUTS` instead — keep it that way.
- **The channel-open argument order is `ato<proto> <flags> <baud> <chan>`** —
  flags *before* baud. Wrong order silently leaves DATA_RATE at 0 and the
  controller never syncs.
- **Transmit needs a nonzero timeout** (`att … <tx_timeout_us>`), or the
  firmware reports `ERR_TIMEOUT` instantly without sending.
- **A filter must be installed** before the device delivers any received frame.

## Good first / high-value contributions

- **Hardware ISO-TP (J2534 protocol 6):** the firmware supports it; the DLL
  currently does software ISO-TP over raw CAN. Needs the `atf` flow-control
  filter form validated on a car.
- **Periodic messages** (`PassThruStartPeriodicMsg`): the device has an `atm`
  command; its argument layout is not yet confirmed.
- **K-line init** (`FAST_INIT` / `FIVE_BAUD_INIT`) for ISO9141/ISO14230.
- **Map the remaining commands:** `atw` and `aty` are still unidentified (see
  PROTOCOL.md §4).
- **Confirm behaviour on other buses/vehicles** and file what you see.

When you add a protocol finding, please update `PROTOCOL.md` with the observed
bytes and how you verified them.
