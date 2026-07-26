#!/usr/bin/env python3
"""Can the radio hear sound well enough to carry data? (on-off keying probe)

The BK4819's DTMF/FSK decoders sit on the RX DSP and are fed from the RF chain,
so they cannot hear the microphone at all. But REG_64 ("Voice amplitude out",
what VOX uses) is readable, and an amplitude reading is enough for on-off
keying even though it can never give us frequency.

This probe answers the one unknown before any of that is worth writing: does
REG_64 track the microphone while the radio is NOT transmitting? REG_30 has
separate bits for the MIC ADC (<2>) and the TX DSP (<1>) from the PA (<3>) and
the PLL/VCO (<7:4>), so in principle the audio front end can run with nothing
going on the air. "In principle" is why we measure.

Needs a firmware built with ENABLE_UART_RW_BK_REGS=1 (0x0601 read, 0x0602
write); the shipped images compile those out.

    python3 utils/mic_ook_probe.py [port]

Be quiet for the first few seconds, then talk at the radio or play a tone.
"""
import glob
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aprs_pc import send_cmd, read_reply  # noqa: E402
import serial  # noqa: E402

REG_30, REG_64 = 0x30, 0x64
MIC_ADC, TX_DSP = 1 << 2, 1 << 1


# 0x0601: request is a single register byte; the reply echoes 0x0601 with
# { uint8 reg, uint16 value }, so the value sits at payload offset 5.
def read_reg(ser, reg):
    send_cmd(ser, 0x0601, bytes([reg]))
    r = read_reply(ser)
    if not r or struct.unpack("<H", r[0:2])[0] != 0x0601 or len(r) < 7:
        return None
    return struct.unpack("<H", r[5:7])[0]


# 0x0602 takes { uint8 reg, uint16 value } and sends no reply at all.
def write_reg(ser, reg, val):
    send_cmd(ser, 0x0602, struct.pack("<BH", reg, val))


def main():
    ports = sys.argv[1:2] or glob.glob("/dev/cu.wchusbserial*")
    if not ports:
        sys.exit("no adapter found")
    ser = serial.Serial(ports[0], 38400, timeout=1.0, exclusive=True)

    send_cmd(ser, 0x0514, bytes([0x46, 0x9C, 0x6F, 0x64]))
    r = read_reply(ser)
    if not r:
        sys.exit("radio did not answer hello")
    print("firmware:", r[4:r.index(0, 4)].decode())

    original = read_reg(ser, REG_30)
    if original is None:
        sys.exit("no answer to 0x0601 — this firmware lacks ENABLE_UART_RW_BK_REGS")
    print(f"REG_30 before: 0x{original:04X}")

    # audio front end on, PA and PLL untouched, so nothing is radiated
    write_reg(ser, REG_30, original | MIC_ADC | TX_DSP)
    now = read_reg(ser, REG_30)
    print(f"REG_30 after:  0x{now:04X}  (mic adc {'on' if now & MIC_ADC else 'OFF'})")
    if now == original:
        print("!! the radio rewrote REG_30 immediately — it reprograms the chip in its "
              "main loop, so this needs doing from inside the firmware instead")

    print("\nstay quiet ~4 s, then make noise at the radio (Ctrl-C to stop)\n")
    quiet, loud, t0 = [], [], time.time()
    try:
        while time.time() - t0 < 20:
            v = read_reg(ser, REG_64)
            if v is None:
                continue
            el = time.time() - t0
            (quiet if el < 4 else loud).append(v)
            bar = "#" * min(60, v // 16)
            print(f"  {el:5.1f}s  REG_64={v:5d} {bar}")
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass

    write_reg(ser, REG_30, original)   # leave the radio as we found it
    ser.close()

    if quiet and loud:
        qa, qm = sum(quiet) / len(quiet), max(quiet)
        la, lm = sum(loud) / len(loud), max(loud)
        print(f"\nquiet: avg {qa:.0f} peak {qm}")
        print(f"sound: avg {la:.0f} peak {lm}")
        if lm > qm * 2 and lm > 50:
            print("=> the microphone IS readable while not transmitting: on-off keying is on the table")
        else:
            print("=> no usable difference: REG_64 does not follow the mic here, so an "
                  "acoustic link would need hardware, not firmware")


if __name__ == "__main__":
    main()
