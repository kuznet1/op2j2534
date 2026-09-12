# op2j2534

A clean-room SAE J2534-1 v04.04 pass-thru driver for the **Tactrix OpenPort 2.0**
(`USB\VID_0403&PID_CC4D`), written to keep the hardware usable on Windows 11
after the vendor stack stopped installing.

**License:** Apache-2.0 (see [LICENSE](LICENSE) and [NOTICE](NOTICE)).
Copyright 2026 CMS DriveTech AB, Sweden. Published by CMS DriveTech AB (Mackanized®).

> **Not affiliated with Tactrix.** This is an independent, clean-room project
> for interoperability. "Tactrix" and "OpenPort" are used only to state which
> hardware this driver is compatible with. No vendor code or binaries are
> included here.

> ⚠️ **This software talks to vehicle control modules.** Reading, writing, or
> reflashing an ECU can render it inoperable. It is provided **as is, with no
> warranty** — you are solely responsible for how you use it.

## Why this exists

The OpenPort 2.0 ships two pieces: a kernel driver (`openport.sys`) and a
user-mode J2534 DLL (`op20pt32.dll`). The driver is dated **2014** (KMDF 1.9)
and no longer installs on Windows 11, and there are no updates.

The important discovery is that **the driver was never necessary**. The OP2
also presents a standard **USB CDC-ACM** interface, so Windows 11 binds its
own inbox `usbser.sys` and the device appears as an ordinary COM port with no
installation at all. The vendor DLL cannot use that path — it enumerates only
through its own driver's interface, so it reports `ERR_DEVICE_NOT_CONNECTED`
("no devices available") even with healthy hardware attached.

This DLL talks to the device directly over that COM port. There is **no kernel
driver, no signing requirement, and nothing that a Windows update can break
again**.

## Status

Verified against real hardware (firmware `1.17.4877`), including a full ISO-TP
read of a Gen2 Saab TCM on a live vehicle:

| Area | State |
|---|---|
| Device discovery (`VID_0403&PID_CC4D`) | working |
| `PassThruOpen` / `PassThruClose` | working |
| `PassThruReadVersion` | working — reports real firmware version |
| `PassThruConnect` / `PassThruDisconnect` | working — CAN, ISO15765, ISO9141, ISO14230 |
| Three-channel limit enforcement | working |
| `Ioctl READ_VBATT` / `READ_PROG_VOLTAGE` | working |
| `Ioctl SET_CONFIG` / `GET_CONFIG` | working — verified by round-trip |
| `PassThruWriteMsgs` | **working** — transmits and is ACKed on the bus |
| `PassThruReadMsgs` | **working** — binary RX frames decoded, validated vs Kvaser |
| `PassThruStartMsgFilter` | **working** — real `atf` filter; required for RX |
| Full request/response (software ISO-TP over raw CAN) | **working** — read TCM VIN, supplier, DIDs |
| `PassThruSetProgrammingVoltage` | implemented, not yet bench-verified |
| Periodic messages, `FAST_INIT`, `FIVE_BAUD_INIT` | report `ERR_NOT_SUPPORTED` |

Three bring-up bugs — since fixed — are worth calling out because they made a
perfectly good adapter look dead (see PROTOCOL.md §4):

1. **Open argument order** is `ato<proto> <flags> <baud> <chan>` — Flags before
   Baudrate, matching the J2534 API. Baud in the wrong slot leaves DATA_RATE at
   0 and the controller never syncs to the bus.
2. **Transmit needs a nonzero timeout.** `att … 0` returns `ERR_TIMEOUT`
   instantly without transmitting; pass `J2534_Timeout * 1000` µs.
3. **A message filter is required** before the device delivers any received
   frame. `PassThruStartMsgFilter` installs one on the wire.

Unverified calls still return honest `ERR_NOT_SUPPORTED` rather than pretending
to work.

## Performance

Measured on the live bus (Saab 9-3, Gen2 TCM), x86 DLL:

* **Request/response latency ~2.4 ms median, ~4 ms p99** (single-frame UDS read,
  request paced a few ms apart). The device itself answers in about 1 ms.
* Per-transaction overhead the DLL adds is negligible; large block transfers
  are limited by the 500 kbit CAN bus and ISO-TP flow control, not by the DLL.

A note on pacing: firing single request/response cycles back-to-back with **no**
gap loses ~half of them, because the ECU enforces a minimum time between
requests (P3). This is normal ECU behaviour, not a DLL fault — with a ~3 ms gap
(or any normal ISO-TP block transfer) loss drops to well under 1%.

The latency depends on one detail worth preserving: the serial read blocks in
the kernel (via COMMTIMEOUTS) instead of polling with `Sleep(1)`. At Windows'
default 15.6 ms timer granularity a `Sleep(1)` actually sleeps ~15.6 ms, which
had pinned every transaction at ~16 ms before this was fixed. Don't reintroduce
a `Sleep` in the read path.

## Hardware limits (not implementation gaps)

* **No SW-CAN.** The vendor's own `j2534_tactrix.h` documents `SW_CAN_NS` and
  `SW_CAN_HS` as unsupported on the OP2. GM single-wire CAN work needs a
  different interface.
* **No J1850.** This firmware rejects both VPW and PWM.
* **ISO14230 runs at 10400 baud only.**
* **Three channels maximum**, each a different protocol.

## Build

Requires Visual Studio with the C++ toolset (tested with VS 18 + Windows
SDK 10.0.26100).

```
build.bat
```

Produces `build\x86\op2j2534.dll` and `build\x64\op2j2534_64.dll`.

## Install

From an **elevated** PowerShell:

```powershell
.\register.ps1
```

This copies the DLLs to `C:\Program Files (x86)\OpenportJ2534\` and registers
them under `PassThruSupport.04.04`, where any J2534 application will find them.
32-bit applications (TCMFlasher included) read the `WOW6432Node` view; 64-bit
applications read the native view. Both are registered.

A failed Tactrix install leaves a registry entry pointing at a DLL that is not
present, which shows up in device pickers as an adapter that can never open.
To clear those:

```powershell
.\register.ps1 -RemoveStaleTactrix
```

To remove everything:

```powershell
.\register.ps1 -Uninstall
```

## Debugging

Set `OP2_J2534_LOG` to a file path and the DLL logs every API call and every
byte exchanged with the device:

```
TX: ato5 500000 0 0 5
RX: aro 5
```

## Testing

```
tools\test_dll.py      exercise the DLL against attached hardware
tools\op2probe.py      talk the raw AT protocol directly, for protocol work
```

Run `test_dll.py` under a 32-bit Python for the x86 build, 64-bit for x64.

## Layout

```
include/j2534.h     J2534 v04.04 API definitions
src/op2_port.c      COM port discovery and serial I/O
src/op2_proto.c     AT protocol: framing, sequence numbers, replies
src/j2534_api.c     the 14 exported PassThru* entry points
PROTOCOL.md         the reverse-engineered wire protocol
```

## How the protocol was recovered

Partly by probing the device, partly from the vendor DLL. `op20pt32.dll` is
packed, so its strings are not visible on disk — but a packer unpacks itself
in memory, and loading the DLL needs no device present. Dumping its memory
image yields the plaintext command templates (`at%c%d %lu %lu %lu %u` and so
on), and matching each template against the character pushed at its call site
recovers which letter each one belongs to. `PROTOCOL.md` documents the result.
