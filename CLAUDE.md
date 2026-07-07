# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Custom firmware for the Quansheng UV-K5/K6/5R handheld radios — a fork of F4HWN's firmware (itself based on Egzumer → OneOfEleven → DualTachyon). This fork turns the radio into a self-contained **APRS station** (AX.25 / Bell 202 1200 baud, no external TNC), all verified on-air on 2026-07-07 between two UV-K5s and against the live Tekirdağ/Marmara network:

- **TX** — position beacons; digipeated by YM3KZD, igated by YM1KTC-11, seen on aprs.fi.
- **RX** — decodes packets off the air (uncompressed, Mic-E, base-91 compressed) and shows the position on screen.
- **Messaging** — send/receive APRS text messages; an incoming message addressed to your callsign pops a framed overlay box over any screen, dismissed by any key.
- **PC control** — send beacon/message and monitor decoded traffic over USB (`utils/aprs_pc.py`).
- **Config** — callsign/SSID/location/message-target persist in EEPROM; location is entered as a 15-digit code produced by `utils/aprs-location.html` (phone GPS → code).

## Build System

```bash
make ENABLE_APRS=1            # local build (arm-none-eabi-gcc 10.3.1 required, brew works)
make clean                    # note: clean misses APRS objects unless ENABLE_APRS=1 is passed
```

- `ENABLE_APRS` defaults to **0** in the Makefile — a plain `make` produces a firmware without APRS.
- Output: `f4hwn` (ELF), `f4hwn.bin` (raw, what the flasher needs), `f4hwn.packed.bin` (needs python `crcmod`; if pip is blocked by PEP 668, use a venv and run `python fw-pack.py f4hwn.bin F4HWN v4.3 f4hwn.packed.bin`).
- Docker build also available: `./compile-with-docker.sh <edition>` → `compiled-firmware/`.
- If make fails with "No rule to make target .../printf_config.h" after the repo was moved, delete stale `*.d` files (they hold absolute paths).

## Flash Constraints

- Flash: 60KB (61440 bytes); RAM ~4KB. Current build with the full APRS suite: ~58.9KB — size-check every change with `arm-none-eabi-size f4hwn`.
- To make room, the APRS build turns several stock features OFF by default in the Makefile: VOX, TX1750, mic audio bar, small-bold font, RX/TX timer, sleep. Re-enable per build if you free space elsewhere.
- No floating point (soft-float libs cost ~15KB), no sprintf in hot paths, prefer integer arithmetic. See `app/aprs_minimal.c` for the patterns.

## Flashing (macOS)

**Apple's built-in CH34x serial driver is broken on current macOS** (Darwin 25.5+): every `tcsetattr` fails with errno 22, which kills K5TOOL (mono), k5prog, pyserial, and even `stty`. Fix once per machine:

```bash
brew install --cask wch-ch34x-usb-serial-driver   # approve cn.wch.CH34xVCPDriver in System Settings, replug
```

The adapter then appears as `/dev/cu.wchusbserial110` / `wchusbserial1110` (suffix follows the USB port). Flash with the pyserial-based flasher in this repo (K5TOOL's V2 bootloader protocol ported to Python):

```bash
python utils/k5flash.py /dev/cu.wchusbserial1110 f4hwn.bin '*F4HWN v4.3'
```

- Takes the **raw** `f4hwn.bin` (not packed). The version string must start with `*` (wildcard bypasses the bootloader version check).
- Radio must be in flash mode: hold PTT while powering on (LED lights).
- Protocol notes: bootloader beacons packet 0x0518; host sends version (0x0530), then 256-byte chunks (0x0519) acked by 0x051A. Envelope: `AB CD len | XOR(payload+crc16-xmodem) | DC BA`.

## APRS Implementation (ENABLE_APRS)

### Architecture — what actually works

Live code is **`app/aprs_minimal.c`** only. TX pipeline:

1. Build AX.25 UI frame in software: `TA1JS>APRS,WIDE1-1,WIDE2-1: !4059.60N/02735.98E>UV-K5 APRS` + CRC-16/X.25 FCS (bit-by-bit, no table).
2. Software HDLC encode (`HDLC_PutBit/PutByte`): 32 lead flags, bit stuffing (frame only), NRZI (0 = transition), AX.25 bits LSB-first, then packed **MSB-first** into bytes because the BK4819 FIFO serializes MSB-first.
3. **BK4819 hardware FSK engine clocks the bits out** — no software bit timing at all. Bell 202 is obtained from FFSK 1200/1800 TX mode with Tone1 forced to 2200 Hz (undocumented but spectrum-verified).

Key register recipe (all confirmed against the official docs in `docs/bk4819/`):

| Register | Value | Why |
|---|---|---|
| REG_58 | mode 001 (FFSK 1200/1800 TX), **bits <7:6> = 11** (FSK enable — gvJaime's fork misses this), enable bit 0 | modem mode |
| REG_70 | Tone1 + Tone2 enabled, gain 96 | both tone generators |
| REG_71 | 22714 (2200 Hz) | space tone override |
| REG_72 | 12389 (1200 Hz) | mark tone = bit clock (freq × 10.32444 for 26M XTAL) |
| REG_40 | keep <15:12>, deviation = 1200 | ~3 kHz tone deviation (kamilsss655 measurements) |
| REG_2B | (1<<2)\|(1<<0) | TX 300Hz HPF + pre-emphasis OFF during TX (restore after) |
| REG_51 | 0 | CTCSS off during TX (restore after) |
| REG_5C | 0xAA30 | engine CRC off (AX.25 FCS is done in software) |
| REG_5D | (nbytes−1)<<8 | length field is N−1 |
| REG_59 | 0x8068→0x0068, fill FIFO, 0x0868 | clear FIFO, preamble 6 + sync 4, TX enable |

TX FIFO is 128 words (256 bytes) — the whole packet fits in one fill. TX duration is timed with a fixed delay: `(10 + nbytes) × 6.67 ms + margin`.

Beacon and message frames are built from the menu-configured callsign/SSID/location/comment (`APRS_BuildHeader` + `APRS_TransmitBeacon` / `APRS_SendMessage`). Nothing is hardcoded anymore.

### RX — receiving and decoding

While the "APRS" menu item is ON the FSK engine is armed for RX (`APRS_RxArm`): sync detector loaded with `0xFEFE` (the NRZI encoding of a run of AX.25 `0x7E` flags), raw bits captured through the FIFO. A software streaming decoder (`APRS_DecodeCapture`) does NRZI decode → flag hunt → bit destuff → FCS check, then `APRS_ShowFrame` parses the position (uncompressed / Mic-E / base-91, ported from F4JTV/aprs_decoder) and shows `SRC 41.15N 27.84E` on the centre line for 30 s.

Three RX gotchas, all fixed and easy to regress:
1. **Power save must be disabled while listening** — the sleep cycle reprograms the chip mid-capture (guarded in `app/app.c`).
2. **`RADIO_SetupRegisters` must preserve the FSK interrupt bits in REG_3F** — other paths overwrite the mask.
3. **The engine's byte counter stalls when the carrier drops, so RX_FINISHED rarely fires for real packets** — `APRS_Task` decodes the capture after ~1.5 s of FIFO inactivity (drain the FIFO tail first).

### Messaging

`:ADDRESSEE:text` frames. An incoming message whose addressee matches this radio's callsign-SSID sets `gAPRS_RxSticky` and is drawn as a framed overlay box in `GUI_DisplayScreen` (`ui/ui.c`) over whatever screen is showing; **any key dismisses it** (`APRS_DismissMessage` in `ProcessKey`, PTT passes through so it still keys up). Compose via menu MsgTo + Msg + Send.

### Text entry (menu string fields)

Call / Cmnt / MsgTo / Msg use **two-digit-per-character entry** (not T9): `00`–`09` = digits, `10`–`35` = A–Z, `36`+ = punctuation — same table as `utils/aprs-location.html`, which encodes text for you. First digit shows tentatively, second commits and advances; **EXIT backspaces** (cancels at position 0). Location (`Loc`) is a 15-digit code: `(lat+90)*1e4 [7] + (lon+180)*1e4 [7] + checksum [1]`; a wrong checksum is rejected so a mistyped code leaves the old position intact.

### EEPROM persistence

APRS settings live in two 16-byte rows that are unused across the whole tree and outside every reset-protected range: **0x0E30 and 0x0F20** (see `SETTINGS_SaveAPRS`/`LoadAPRS`). Critical: `EEPROM_WriteBuffer` only ever burns **8 bytes** regardless of the buffer size, so each region is written as two rows. Persisted: ON, interval, SSID, callsign(≤6), lat, lon, MsgTo. **Comment and message text are not persisted** (no room in 32 bytes) — they revert to defaults on boot. `MENU_AcceptSetting` calls `SETTINGS_SaveAPRS()` after any APRS field changes (the generic `SETTINGS_SaveSettings` does NOT cover this block). Menu numbers shift with feature flags; group order: APRS, Intv, Call, SSID, Loc, Cmnt, MsgTo, Msg, Send, TX.

### PC control (UART, `utils/aprs_pc.py`)

Custom UART commands (normal mode, radio on): `0x0700` send message (dest[10]+text[30], acks 0x0701), `0x0702` beacon (acks 0x0703). On every decode the firmware also emits a plain `APRS:<text>\r\n` line so any serial reader can monitor. Tool: `aprs_pc.py <port> msg <DEST> <text>` / `beacon` / `monitor`. **Hardware limit:** the shared 2-pin connector means TX can't radiate while a plain 3-wire programming cable is attached (the adapter's idle-high TX line back-feeds the radio, tripping the over-voltage lockout — see cable note below). The `monitor` (RX) direction works fine while cabled.

### Cable back-feed / VOLT HIGH

A connected programming cable back-feeds ~11.55V into the battery sense, tripping `VFO_STATE_VOLTAGE_HIGH` and disabling TX. `helper/battery.c` treats only the narrow **11.00–12.00V** band as a normal full battery (so TX works while cabled) while a genuine over-voltage (e.g. a 3S pack ~12.6V) is still caught. Proper fix is electrical (drop the adapter-TX→radio-RX wire), but the firmware band-exception makes the cable usable as-is.

### EEPROM tooling

`utils/eeprom_tool.py <port> backup <prefix>|wipe|probe` — full 8KB dump, settings wipe (0x0000–0x1DFF, calibration at 0x1E00+ preserved), or probe. Backups + generated maps go under `backups/` (git-ignored). Calibration at 0x1F40 (6× u16) is factory per-radio; never wipe 0x1E00+.

### History — why the old approach failed

The previous implementation bit-banged tones by rewriting REG_71 per bit with `SYSTICK_DelayUs(833)`. It never decoded. Known fatal bugs found on the way: 1 ms bit time instead of 833 µs (old old version), MSB-first instead of LSB-first AX.25 bit order, an out-of-bounds read in `AX25_EncodeAddress` for callsigns shorter than 6 chars, TX pre-emphasis/HPF left enabled, and REG_58<7:6> left 0. Dead experiments (`app/aprs.c`, `app/aprs_tx.c`, `ui/aprs.c`, `driver/bk4819-afsk.c.bak`) are untracked leftovers, not in the build — safe to delete.

### Verifying changes without a radio

`utils/aprs_hdlc_test.c` is a host-side copy of the encoder + the streaming RX decoder + the three position parsers, checked against spec vectors:

```bash
cc -Wall -o /tmp/hdlc_test utils/aprs_hdlc_test.c && /tmp/hdlc_test
# expect: ALL CHECKS PASSED / RX STREAMING DECODER PASSED / POSITION PARSERS PASSED
```

Keep it in sync with `app/aprs_minimal.c` when changing the frame, encoder, RX decoder, or the Mic-E/compressed/uncompressed parsers.

### On-air verification with RTL-SDR

```bash
brew install librtlsdr direwolf
cd <dir with direwolf.conf containing at least "MYCALL TA1JS">   # direwolf truncates long -c paths
rtl_fm -f 144.8M -s 24000 -g 40 - | direwolf -n 1 -r 24000 -b 16 -
```

Notes: only one process can own the dongle; a decoded frame prints as `TA1JS>APRS,WIDE1-1,WIDE2-1:!4059.60N/02735.98E>UV-K5 APRS`. Regional RF infrastructure (Tekirdağ/Marmara): digipeater YM3KZD, iGates YM1KTC-11 / YM3KC-8. Much aprs.fi traffic in the region arrives via DMR/BrandMeister (`qAS`), which proves nothing about 144.800 RF coverage — look for `qAR`/`qAO` paths.

## Reference Documents

`docs/bk4819/` contains the official Beken PDFs (Datasheet, Technical Reference Manual, Registers V1.1, Application Note). The TRM's FSK chapter (pages 16–21) and the Registers doc (pages 14–23) cover everything the APRS modem uses. Beken's official sample code (fsk.c/mdc1200.c/noaa.c) lives in `~/Downloads/Reference Code` (not committed).

## Architecture (general)

- **`app/`** application layer, **`driver/`** hardware (BK4819 RF, ST7565 LCD, EEPROM…), **`ui/`** rendering/menus, **`bsp/`** DP32G030 MCU, **`helper/`**, **`external/`** (CMSIS, printf).
- `syscalls.c` provides `_sbrk()` for newlib-nano; `firmware.ld` defines `__HeapBase`/`__HeapLimit`. Both required — removing them breaks linking.
- Function naming `Module_Function()`, no dynamic allocation, feature flags via `#define`/Makefile.
