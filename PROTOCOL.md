# Tactrix OpenPort 2.0 — wire protocol (reverse engineered)

Device: `USB\VID_0403&PID_CC4D`, bus-reported name `OpenPort 2.0`.
Firmware observed: `1.17.4877`.

## 1. Transport

The OP2 exposes a **USB CDC-ACM** interface. Windows 11 binds the inbox
`usbser.sys` automatically and the device appears as a virtual COM port —
no vendor driver, no signing, nothing to install.

The vendor stack (`openport.sys`, `DriverVer=04/07/2014`, KMDF 1.9) is a
*different* access path, and it is what fails on Windows 11. `op20pt32.dll`
enumerates only through that driver's interface, which is why it reports
`ERR_DEVICE_NOT_CONNECTED` ("no devices available") on a modern machine even
though the hardware is present and healthy.

Port settings: 115200 8N1 (CDC ignores the line coding), DTR+RTS asserted.

## 2. Framing

```
command:  at<letter>[<protocol>] <arg> <arg> ... <seq>\r\n
reply:    ar<type> [<arg> ...] <seq>\r\n
```

* Commands are **CRLF** terminated. The device also accepts a bare LF; a bare
  CR is ignored entirely.
* The digit immediately after the command letter is the **J2534 protocol ID**,
  not a channel index. `%d` parsing skips whitespace, so `ato5 ...` and
  `ato 5 ...` parse identically.
* The final numeric argument of every command is a **sequence number**, echoed
  back by the device. A reply that echoes the sequence proves the command was
  parsed with the correct arity — this is the reliable oracle when probing.
* Binary payloads (transmit data) follow the CRLF immediately as raw bytes.
  If the command line is rejected the payload is **not** consumed and the
  parser desynchronises; resynchronise by sending a newline and re-issuing
  `ati`.

### Reply types

| Prefix | Meaning                                        |
|--------|------------------------------------------------|
| `aro`  | success                                        |
| `are`  | error: `are <j2534_error_code> <seq>`          |
| `ari`  | identify: `ari main code version : 1.17.4877`  |
| `arr`  | analog read: `arr <pin> <millivolts> <seq>`    |
| `arg`  | get config: `arg<proto> <param> <value> <seq>` |

### Error codes

The device returns **standard J2534 error codes**. Confirmed on hardware:

| Code | Name                     | Observed when                     |
|------|--------------------------|-----------------------------------|
| 1    | ERR_NOT_SUPPORTED        | unsupported SET_CONFIG parameter  |
| 2    | ERR_INVALID_CHANNEL_ID   | bad channel in `atg`              |
| 3    | ERR_INVALID_PROTOCOL_ID  | protocol digit invalid / not open |
| 7    | ERR_FAILED               | too few arguments                 |
| 9    | ERR_TIMEOUT              | transmit attempted, no bus ACK    |
| 10   | ERR_INVALID_MSG          | malformed message arguments       |
| 19   | ERR_PIN_INVALID          | `atr` on a non-readable pin       |
| 20   | ERR_CHANNEL_IN_USE       | opening a 4th channel             |
| 25   | ERR_INVALID_BAUDRATE     | bad baud rate for the protocol    |

## 3. Command set

Format templates were recovered from the (packed) `op20pt32.dll` by loading it
so that it self-unpacked, dumping its memory image, and matching each `at%c...`
format string to the character pushed at its call site.

| Template                   | Letter(s) | Command                                       |
|----------------------------|-----------|-----------------------------------------------|
| `at%c`                     | —         | `ati` identify (no seq)                       |
| `at%c %u`                  | a, z      | `ata <seq>` close all, `atz <seq>` reset      |
| `at%c %d %u`               | r         | `atr <pin> <seq>` read analog                 |
| `at%c %d %d %u`            | p, v      | programming voltage                           |
| `at%c%d %u`                | c, l      | `atc<proto> <seq>` close channel              |
| `at%c%d %d %u`             | k, n, w   | unmapped                                      |
| `at%c%d %lu %u`            | g         | `atg<proto> <param> <seq>` get config         |
| `at%c%d %lu %lu %u`        | s, t, y   | `ats<proto> <param> <value> <seq>` set config |
| `at%c%d %lu %lu %u %u`     | t         | `att<proto> <len> <a> <b> <seq>` transmit     |
| `at%c%d %d %d %d %u`       | f         | `atf<proto> ... <seq>` message filter         |
| `at%c%d %d %d %u`          | y         | unmapped                                      |
| `at%c%d %d %d %lu %lu %u`  | m         | periodic message                              |
| `at%c%d %lu %lu %lu %u`    | o         | `ato<proto> <baud> <flags> <chan> <seq>` open |

### Verified on hardware

```
ati                     -> ari main code version : 1.17.4877
ata 1                   -> aro 1
ato5 500000 0 0 2       -> aro 2         (raw CAN, 500 kbit, channel 0)
ato6 500000 0 1 3       -> aro 3         (ISO15765, channel 1)
ato3 10400  0 2 4       -> aro 4         (ISO9141, channel 2)
ato5 500000 0 3 5       -> are 20 5      (only three channels exist)
atc5 6                  -> aro 6
ats5 3 1 7              -> aro 7         (LOOPBACK = 1)
atg5 3 8                -> arg5 3 1 8    (reads it back)
atr 16 9                -> arr 16 108 9  (pin 16 = VBAT, millivolts)
atr 17 10               -> arr 17 4204 10
```

### Channels

Three concurrent channels (0, 1, 2), each a distinct protocol. The channel
number is argument 4 of `ato`. Per-protocol commands address the channel by
its **protocol ID**, not by the channel number.

### Protocol support (this firmware)

| ID | Protocol | Status                     |
|----|----------|----------------------------|
| 1  | J1850VPW | rejected (`are 3`)         |
| 2  | J1850PWM | rejected (`are 3`)         |
| 3  | ISO9141  | opens                      |
| 4  | ISO14230 | opens, **10400 baud only** |
| 5  | CAN      | opens                      |
| 6  | ISO15765 | opens                      |

`j2534_tactrix.h`, shipped in the vendor installer, confirms SW-CAN is **not
supported** by this hardware, and that SCI is unsupported.

### SET_CONFIG parameters accepted on a CAN channel

Sweeping `ats5 <param> 0 <seq>` over 0x00–0x2F returns `aro` for exactly
0x01, 0x03, 0x17 and 0x18 — `DATA_RATE`, `LOOPBACK`, `BIT_SAMPLE_POINT`,
`SYNC_JUMP_WIDTH`. Every other parameter returns `are 1` (`ERR_NOT_SUPPORTED`).
That is precisely the J2534 set for a CAN channel, which independently confirms
the device implements J2534 semantics directly.

### Analog pins (`atr <pin>`)

Readable: 8, 12, 16 (battery) and 17 (internal VADJ). All others return
`ERR_PIN_INVALID`. Values are in millivolts.

## 4. Transmit, receive, and filters (all confirmed on a live bus)

Validated on a real car (Saab 9-3, Gen2 Aisin TCM) with a Kvaser as an
independent reference node. A full ISO-TP read of the TCM works through the
DLL: VIN, supplier `AISIN0200`, diagnostic address `0x18`.

### Transmit

    att<proto> <DataSize> <TxFlags> <tx_timeout_us> <seq>\r\n<DataSize bytes>

* `DataSize` counts the whole J2534 message body. For CAN/ISO15765 the body is
  a **4-byte big-endian CAN ID** followed by the data, so an 8-byte CAN frame
  is `DataSize = 12`.
* `TxFlags` is the J2534 TxFlags (0 for a normal CAN frame).
* **`tx_timeout_us` must be nonzero.** It is a per-frame transmit timeout in
  microseconds. A value of 0 makes the firmware return `ERR_TIMEOUT`
  immediately without ever putting the frame on the bus — this single detail
  is what made transmit look completely dead during bring-up. The genuine DLL
  sends `J2534_Timeout * 1000`; do the same, with a floor of ~100 ms.
* On success the device replies `aro <seq>`; for CAN that means the frame was
  transmitted and acknowledged by another node.

There is also a 3-argument form `att<proto> <DataSize> <TxFlags> <seq>` (no
timeout); prefer the 4-argument form with an explicit timeout.

### Receive

Received frames arrive as **binary records, not CRLF terminated**, interleaved
with ASCII replies on the same stream:

    'a' 'r' <proto-digit> <len> <RxStatus:1> <Timestamp:4 BE us> <CAN-ID:4 BE> <data...>

`len` counts every byte after it, so `len = 9 + datalen`. Disambiguate a binary
record from an ASCII reply by the **third byte**: a digit (`'1'`..`'6'`) is a
binary frame for that protocol id; a letter is a reply (`aro`/`are`/`arr`/
`arf`/`arg`/`ari`). Read binary records by their length byte, so data bytes
that spell `"ar"` never cause a mis-parse.

Validated field-by-field against a Kvaser: injected id `0x123` /
`AABBCCDDEEF00102` and every static broadcast frame (`0x0F9`, `0x199`, `0x1AF`,
`0x1F5`, `0x4C9`, `0x77F`) decoded to the exact id and data the Kvaser saw.

### Filters — REQUIRED before any frame is delivered

The device forwards **no** received frames to the host until a filter exists.

    atf<proto> <type> <mask_len> <pattern_len> <seq>\r\n<mask bytes><pattern bytes>

* `type` is the J2534 filter type: `1`=PASS, `2`=BLOCK, `3`=FLOW_CONTROL.
* mask and pattern follow the CRLF as a binary payload, each a 4-byte
  big-endian CAN-ID selector, so the lengths are `4 4`.
* reply is `arf<proto> <filter_id> <seq>`.
* A PASS filter with mask `0` / pattern `0` accepts every id.

### A lesson worth keeping

During bring-up, transmit returned `ERR_TIMEOUT` and receive returned nothing,
and this was misread as "the adapter is not electrically on the bus." That was
wrong — the adapter was fine. Three *software* bugs had stacked up: baud in the
wrong `ato` slot (DATA_RATE stayed 0), a zero transmit timeout, and no filter
installed. A CAN ACK really is emitted by any active node, but that reasoning
only applies once the controller is correctly configured and actually driving,
which it was not. Verify the local configuration — read back DATA_RATE, confirm
a filter is set — before concluding the hardware is at fault.

### Further letters identified by their error codes

Because the device returns standard J2534 error codes, the code a command
rejects with identifies what it operates on:

| Command          | Rejects with             | Therefore              |
|------------------|--------------------------|------------------------|
| `atk<proto> <n>` | `ERR_INVALID_FILTER_ID`  | stop / remove a filter |
| `atn<proto> <n>` | `ERR_INVALID_MSG_ID`     | stop a periodic message|
| `atv <pin> <n>`  | `ERR_PIN_INVALID`        | pin control, like `atp`|
| `atl<proto>`     | returns `aro`            | accepted, effect unknown |

`atw` and `aty` remain unmapped.

### `atx` — enter the bootloader. Do not send it.

The vendor DLL's firmware updater sends `atx %d` immediately before it starts
"waiting for bootloader" and then talks a different, terse protocol to it
(`tre %X` to erase, and a flash write command, with replies checked as
"invalid erase response" / "invalid flash response"). Its own text warns that
"you must connect to the device again after a bootloader recovery".

So `atx` drops the device out of the J2534 firmware into the bootloader, and
the commands that follow erase and rewrite flash. Never send it while probing,
and never sweep the alphabet blind on a device you care about — this is the one
letter that can leave a unit needing recovery. Recovered from the unpacked
`op20pt32.dll` image, not from the device.

## 5. Standalone SD-card logging

The OP2 also has a microSD slot and a logger that runs without a host: it reads
`logcfg.txt` from the card root and writes `log####.csv` back to it. That is
entirely firmware — none of it is reachable through the commands above, and
none of it appears in the vendor DLL, whose unpacked image contains no logging
keywords at all. USB and standalone logging are mutually exclusive: the device
does not log while it is plugged into a PC.
