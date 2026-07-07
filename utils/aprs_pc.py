#!/usr/bin/env python3
"""PC control for UV-K5 APRS firmware (normal mode, radio powered on).

  aprs_pc.py <port> msg <DEST> <text...>   send an APRS message
  aprs_pc.py <port> beacon                  transmit a position beacon
  aprs_pc.py <port> monitor                 print decoded packets as they arrive

The obfuscated command protocol matches egzumer/K5TOOL; the monitor simply
watches for the plain "APRS:" lines the firmware emits on every decode.
"""
import sys
import struct
import serial

XOR = bytes([0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40,
             0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80])


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def obf(data):
    return bytes(c ^ XOR[i % 16] for i, c in enumerate(data))


def frame(payload):
    body = obf(payload + struct.pack("<H", crc16(payload)))
    return b"\xAB\xCD" + struct.pack("<H", len(payload)) + body + b"\xDC\xBA"


def send_cmd(ser, cmd_id, data=b""):
    payload = struct.pack("<HH", cmd_id, len(data)) + data
    ser.write(frame(payload))
    ser.flush()


def read_reply(ser, timeout=3.0):
    ser.timeout = timeout
    if ser.read(2) != b"\xAB\xCD":
        return None
    hdr = ser.read(2)
    if len(hdr) != 2:
        return None
    size = struct.unpack("<H", hdr)[0]
    rest = ser.read(size + 4)
    if len(rest) < size + 4:
        return None
    return obf(rest[: size + 2])[:size]


def cmd_msg(ser, dest, text):
    dest = dest.upper().encode()[:9].ljust(10, b"\x00")
    text = text.encode()[:30].ljust(30, b"\x00")
    send_cmd(ser, 0x0700, dest + text)
    r = read_reply(ser)
    if r and len(r) >= 5 and r[0] | (r[1] << 8) == 0x0701 and r[4] == 1:
        print(f"sent: {dest.rstrip(chr(0).encode()).decode()} <- {text.rstrip(chr(0).encode()).decode()}")
    else:
        print("no/!ack from radio (is APRS enabled and callsign set?)")


def cmd_beacon(ser):
    send_cmd(ser, 0x0702)
    r = read_reply(ser)
    print("beacon sent" if (r and (r[0] | (r[1] << 8)) == 0x0703) else "no ack from radio")


def cmd_monitor(ser):
    print("monitoring 144.800 decodes (Ctrl-C to stop)...")
    ser.timeout = 1.0
    buf = b""
    while True:
        chunk = ser.read(256)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if line.startswith(b"APRS:"):
                print(line[5:].decode(errors="replace"))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    port, cmd = sys.argv[1], sys.argv[2]
    ser = serial.Serial(port, 38400, timeout=1.0)
    if cmd == "msg" and len(sys.argv) >= 5:
        cmd_msg(ser, sys.argv[3], " ".join(sys.argv[4:]))
    elif cmd == "beacon":
        cmd_beacon(ser)
    elif cmd == "monitor":
        try:
            cmd_monitor(ser)
        except KeyboardInterrupt:
            print()
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
