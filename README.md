# UV-K5 APRS — a self-contained APRS station for the Quansheng UV-K5/K6/5R

This firmware turns a Quansheng UV-K5/K6/5R handheld into a standalone **APRS
station** — 1200 baud Bell 202 (AX.25) transmit *and* receive using only the
radio's BK4819 chip. No sound card, no external TNC, no phone app required to
be on the air.

Verified on-air between two UV-K5s and against the live 144.800 MHz network
(beacons digipeated and igated onto aprs.fi; packets from other stations
decoded and shown on screen).

It is a fork of [F4HWN's custom firmware](https://github.com/armel/uv-k5-firmware-custom),
which builds on [Egzumer](https://github.com/egzumer/uv-k5-firmware-custom),
[OneOfEleven](https://github.com/OneOfEleven/uv-k5-firmware-custom),
[fagci's spectrum analyzer](https://github.com/fagci/uv-k5-firmware-fagci-mod),
and ultimately [DualTachyon's open firmware](https://github.com/DualTachyon/uv-k5-firmware).
All the stock radio features from those projects are still present — this fork
adds the APRS layer on top.

> [!IMPORTANT]
> **Licensed amateurs only.** APRS transmits on amateur frequencies. You must
> hold a valid amateur licence and set **your own callsign** before the radio
> will key up. A fresh or reset radio ships as `N0CALL` and refuses to transmit
> until a real callsign is configured. Transmit is limited to amateur bands by
> default.

> [!WARNING]
> This firmware comes with no warranty of any kind. Use it entirely at your own
> risk. There is no guarantee it will work on your radio, and flashing
> third-party firmware can in principle brick a radio. You accept that risk when
> you flash.

> [!CAUTION]
> Back up your EEPROM before flashing any alternative firmware. `utils/eeprom_tool.py`
> can take a full 8 KB dump, or use [k5prog](https://github.com/sq5bpf/k5prog).
> It is a good habit and can save you if something goes wrong.

## Contents

* [What it does](#what-it-does)
* [How it works](#how-it-works)
* [Building](#building)
* [Flashing](#flashing)
* [Configuring your station](#configuring-your-station)
* [Companion tools](#companion-tools)
* [Documentation](#documentation)
* [Credits](#credits)
* [License](#license)

## What it does

- **Beacon your position** — manually, or automatically on an interval
  (1–60 min keep-alive). Position, callsign/SSID and comment are all
  menu-configured; nothing is hardcoded.
- **Receive and decode** packets off the air — standard uncompressed, Mic-E,
  and base-91 compressed positions — and show the sender and its **distance
  from your saved location** on screen.
- **Messaging** — send short APRS text messages. An incoming message addressed
  to your callsign pops up in a framed overlay box; it auto-clears after 30 s
  or is dismissed by any key.
- **PC / phone control over USB** — send a beacon or message and monitor
  decoded traffic from a computer, or beacon a live GPS fix from the companion
  web tool.
- **Persistent config** — callsign, SSID, location and message target are
  stored in EEPROM and survive a reboot.
- **Amateur-band restriction** — VFO and TX are limited to the amateur
  allocations reachable by the radio (optional; on by default).

Everything the base F4HWN firmware does (multiple screen layouts, improved
S-meter, power levels, scan lists, and so on) still works alongside APRS.

## How it works

APRS is generated and decoded entirely in software on the radio, driving the
BK4819's hardware FSK engine — there is no external modem.

- **TX** — an AX.25 UI frame is built in software with a CRC-16/X.25 FCS,
  HDLC-encoded (bit stuffing + NRZI), and clocked out by the BK4819's FSK
  engine configured for Bell 202 1200/2200 Hz tones. No software bit-banging.
- **RX** — while APRS is on, the FSK engine is armed to hunt for AX.25 flags; a
  streaming software decoder does NRZI decode, bit destuffing and FCS checking,
  then parses the position and displays it.

The full register recipe, frame format and decoder details are documented in
[`docs/APRS.md`](docs/APRS.md) and the reference PDFs under `docs/bk4819/`.

## Building

`arm-none-eabi-gcc` **10.3.1** is recommended (other versions may produce a
binary that is too large for the 60 KB flash). On macOS, `brew install
arm-none-eabi-gcc` works.

```bash
make                     # APRS edition — this is the default build
make ENABLE_APRS=0       # plain radio, no APRS
```

To fit the flash budget, the APRS edition disables several stock features by
default (VOX, TX1750, mic audio bar, small-bold font, RX/TX timer, sleep, FM
radio, spectrum analyzer). Re-enable any of them per build if you free up space.

Key flags:

| Flag | Default | Meaning |
|---|---|---|
| `ENABLE_APRS` | `1` | The whole APRS suite. |
| `ENABLE_AMATEUR_BAND_ONLY` | `1` | Restrict VFO/TX to amateur allocations. Set `=0` to remove. |

Output files: `f4hwn` (ELF), `f4hwn.bin` (raw — this is what the flasher
needs), and `f4hwn.packed.bin` (needs the Python `crcmod` package). Check the
size of every change with `arm-none-eabi-size f4hwn`.

A Docker build is also available: `./compile-with-docker.sh <edition>` writes
to `compiled-firmware/`.

## Flashing

Put the radio into flash mode (hold **PTT** while powering on — the LED lights),
then flash the **raw** `f4hwn.bin`.

On macOS, use the bundled pyserial flasher:

```bash
python utils/k5flash.py /dev/cu.wchusbserial1110 f4hwn.bin '*YOURCALL v1.0'
```

The version string must start with `*` (the wildcard bypasses the bootloader's
version check).

> [!NOTE]
> **macOS driver caveat:** Apple's built-in CH34x serial driver is broken on
> current macOS (Darwin 25.5+) — `tcsetattr` fails, which breaks most flashers.
> Install the vendor driver once per machine:
> ```bash
> brew install --cask wch-ch34x-usb-serial-driver
> ```
> Approve it in System Settings and replug. The adapter then appears as
> `/dev/cu.wchusbserial*`.

## Configuring your station

Set your callsign, SSID and location from the on-radio APRS menu group
(**APRS, Intv, Call, SSID, Loc, Cmnt, MsgTo, Msg, Send, TX**). Text fields use
two-digit-per-character entry; location is a 15-digit code.

The easiest way to enter your location is the companion tool
`utils/aprs-location.html` — open it on a phone, let it read GPS, and it prints
the 15-digit code (and encodes text fields for you) to key into the radio.

## Companion tools

Under `utils/`:

- **`aprs-location.html`** — phone GPS → 15-digit location code and text-field encoder.
- **`aprs-web-beacon.html`** — beacon a live GPS fix over USB from a browser.
- **`aprs_pc.py`** — send a beacon/message and monitor decoded traffic from a PC:
  `aprs_pc.py <port> msg <DEST> <text>` / `beacon` / `monitor`.
- **`k5flash.py`** — the flasher described above.
- **`eeprom_tool.py`** — full EEPROM backup / probe / settings wipe (calibration preserved).
- **`aprs_hdlc_test.c`** — host-side copy of the encoder and decoders, checked
  against spec vectors, for verifying changes without a radio:
  ```bash
  cc -Wall -o /tmp/hdlc_test utils/aprs_hdlc_test.c && /tmp/hdlc_test
  ```

## Documentation

- **[`docs/APRS.md`](docs/APRS.md)** — user guide plus the full technical
  writeup (frame format, BK4819 register recipe, RX decoder, EEPROM layout).
- **`docs/bk4819/`** — the official Beken datasheet, technical reference manual,
  register list and application note.

## Credits

This firmware stands on a long chain of open-source work. Thanks to everyone in
it, and to the many contributors who came before:

* [F4HWN (Armel)](https://github.com/armel) — the immediate upstream this fork is based on
* [Egzumer](https://github.com/egzumer)
* [OneOfEleven](https://github.com/OneOfEleven)
* [DualTachyon](https://github.com/DualTachyon) — the original open firmware
* [Mikhail (fagci)](https://github.com/fagci) — spectrum analyzer
* [Andrej](https://github.com/Tunas1337)
* [Manuel](https://github.com/manujedi)
* [@Matoz](https://github.com/spm81)
* @wagner, @Lohtse Shar, @Davide, @Ismo OH2FTG, @d1ced95
* and others who contributed along the way

The APRS position decoders are ported from the F4JTV `aprs_decoder` work.

## License

Copyright 2023 Dual Tachyon
https://github.com/DualTachyon

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
