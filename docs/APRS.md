# APRS for the Quansheng UV-K5 (no external TNC)

This firmware turns a UV-K5/K6/5R into a self-contained **APRS station** — it
transmits and receives 1200 baud Bell 202 (AX.25) packets using only the radio's
BK4819 chip. No sound card, no TNC, no phone app required to be on the air.

Verified on-air between two UV-K5s and against the live 144.800 MHz network.

> **Licensed amateurs only.** APRS transmits on amateur frequencies. You must
> hold a valid amateur licence and set **your own callsign** before the radio
> will transmit — a fresh/reset radio ships as `N0CALL` and refuses to key up
> until you configure a real callsign. Transmitting is limited to amateur bands
> by default (see *Band restriction* below).

## What it does

- **Beacon** your position (manual, or automatic on an interval — "keep-alive").
- **Receive & decode** packets off the air (standard, Mic-E, and base-91
  compressed positions) and show the sender + position on screen.
- **Messaging** — send short APRS text messages; an incoming message to your
  callsign pops a box over any screen, dismissed by any key.
- **PC / phone control** over USB (see *Companion tools*).

## Building

APRS is on by default in this edition:

```bash
make                     # APRS edition (arm-none-eabi-gcc 10.3.1)
```

To fit the 60 KB flash, this edition disables VOX, TX1750, the mic audio bar,
the small-bold font, the RX/TX timer, sleep, FM radio and the spectrum analyzer.
Build a plain radio without APRS with `make ENABLE_APRS=0` (re-enable those
features as desired).

Flags:
- `ENABLE_APRS` (default 1) — the whole APRS suite.
- `ENABLE_AMATEUR_BAND_ONLY` (default 1) — restrict the VFO/TX to the Turkish
  amateur allocations reachable by the radio (17 m, 15 m, 12 m, 10 m, 6 m, 2 m,
  430–440 MHz, 23 cm). Set `=0` to remove the restriction.

## Menu items

The APRS group is the **first ten items** of the menu, so it is reachable without scrolling:

| Item | Meaning |
|------|---------|
| APRS | Enable RX listening (and, with Intv > 0, auto-beacon) |
| Intv | Auto-beacon interval, minutes. **OFF** = no auto-beacon (RX only) |
| Call | Your callsign (≤ 6 chars) |
| SSID | 0–15 |
| Loc  | 15-digit location code (generate it with the web tool below) |
| Cmnt | Beacon comment |
| MsgTo| Message destination, **callsign *and* SSID** (e.g. `VA3EMQ-7`) |
| Msg  | Message text |
| Send | Sends the message to MsgTo **the moment you press MENU** (the value area shows `SEND MESSAGE`) |
| BEACON | Transmits one position beacon **the moment you press MENU** (shows `SEND BEACON`) |

**Text entry** (Call/Cmnt/MsgTo/Msg): two digits per character —
`00`–`09` digits, `10`–`35` = A–Z, `36` space, `37`–`45` punctuation
(`-./?!@:,'`). The web tool encodes text for you. **MENU** saves the field as
soon as you press it, however few characters you typed; **EXIT** backspaces
(and leaves the field unchanged at position 0).

Callsign/SSID/location/message-target persist in EEPROM; comment and message
text reset to defaults on power-up.

### Messaging details

- **MsgTo must carry the other station's SSID.** A message to `VA3EMQ` is not a
  message to `VA3EMQ-7`: the receiving radio compares the address against its
  own callsign *including* the SSID and silently ignores anything else — the
  packet is received and decoded, it just never appears as a message. The `-`
  is code `37` in the two-digit entry above.
- **Replying is automatic:** when a message addressed to you arrives, MsgTo is
  set to the sender's full callsign-SSID (exactly as shown in the message box),
  so a reply only needs Msg + Send. This is RAM only — the stored MsgTo comes
  back on the next power-up.
- **Line numbers and acknowledgements:** outgoing messages carry a `{nn` line
  number, so the receiving station acknowledges them. The ack arrives as
  `THEIRCALL-n>ackNN` in the message box — that is the delivery confirmation.
  Incoming numbered messages are acked automatically within ~0.5 s.

## Companion tools (open source, in `utils/`)

Hosted (Chrome; iOS unsupported — its browsers lack Web Serial/WebUSB):

- **Code generator** — phone GPS → 15-digit `Loc` code, plus callsign/message
  codes. Works on any phone, no cable.
- **Web beacon** — beacons your live GPS position through the radio over USB
  (Web Serial on desktop, WebUSB/OTG on Android), with SmartBeaconing
  (speed-scaled rate + corner pegging) for live tracking while moving.

Local scripts (Python + pyserial):

- `utils/aprs_pc.py <port> msg|beacon|monitor` — send a message/beacon or watch
  decoded packets from a PC.
- `utils/k5flash.py <port> f4hwn.bin '*F4HWN <ver>'` — flash over USB.
- `utils/aprs_hdlc_test.c` — host-side self-test of the encoder, RX decoder and
  position parsers (`cc -o t utils/aprs_hdlc_test.c && ./t`).

## How it works (short version)

TX builds an AX.25 UI frame, software-encodes HDLC (flags, bit stuffing, NRZI,
FCS), and clocks it out via the BK4819 hardware FSK engine as Bell 202 tones.
RX arms the FSK engine on the NRZI flag pattern, captures raw bits, then
software-decodes and parses. See `CLAUDE.md` for the register-level detail and
the on-air verification notes.

## Credits

Bell 202 hardware-FSK approach informed by gvJaime/uv-k5-firmware-aprs; position
parsers ported from F4JTV/aprs_decoder; built on the F4HWN / egzumer /
OneOfEleven / DualTachyon firmware lineage.
