#!/usr/bin/env python3
"""Dump the e-paper panel's image over the board's serial console.

`screen` answers with the 1-bit 200x200 image as hex, but one console write
carries about 4,000 characters and the image is 10,000, so this walks it with
`screen <first> <count>` and stitches the slices. The console is the same one
`pio device monitor` opens: quit that first, only one reader per port.

Writes two files next to `--out`:
  <out>.txt   the image as ASCII, '#' = ink, '.' = paper (200x200 characters)
  <out>.pbm   the same image as a PBM P4, which any image viewer opens at 1:1

Needs pyserial (the PlatformIO virtualenv has it):

    PYTHONPATH= ~/.platformio/penv/bin/python firmware/tools/panel_dump.py \
        --port /dev/ttyACM0 --out /tmp/panel

Exit status: 0 when every slice came back, 1 on a missing reply or a short
image (a screen that has not drawn yet answers nothing).
"""
import argparse
import re
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - the message is the point
    sys.exit("panel_dump.py needs pyserial (pip install pyserial)")

WIDTH, HEIGHT = 200, 200
IMAGE_BYTES = WIDTH * HEIGHT // 8  # 5,000
SLICE_BYTES = 2000  # 4,000 hex characters: under one console write
TIMEOUT_S = 6.0


def read_slice(ser, first, count, expect_chars):
    ser.reset_input_buffer()
    ser.write(f"screen {first} {count}\n".encode())
    deadline = time.time() + TIMEOUT_S
    text = ""
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            text += chunk.decode("utf-8", "replace")
            if any(len(run) >= expect_chars for run in re.findall(r"[0-9a-f]+", text)):
                break
    runs = [run for run in re.findall(r"[0-9a-f]+", text) if len(run) >= expect_chars]
    if not runs:
        return None
    return bytes.fromhex(max(runs, key=len)[:expect_chars])


def main():
    ap = argparse.ArgumentParser(description="Dump the e-paper panel's image over the board's serial console.")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--out", default="panel")
    ap.add_argument("--baud", type=int, default=115200, help="ignored on USB CDC, kept for UART boards")
    args = ap.parse_args()

    # No DTR/RTS games: the board is running, and a reset would blank the panel
    # (or, before the first draw, answer nothing).
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dsrdtr = False
    ser.rtscts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    time.sleep(0.3)

    # Opening the port resets this board, so wait for it to be back before
    # reading the panel: the image before Wi-Fi associates is the offline frame.
    deadline = time.time() + 30
    boot = b""
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            boot += chunk
            if b"hg: online" in boot or b"online (paired)" in boot:
                break
    if b"online" not in boot:
        print("no 'online' in the boot log yet: dumping whatever is on the panel", file=sys.stderr)
    time.sleep(1.0)  # let the frame after the hand-over land

    image = bytearray()
    first = 0
    while first < IMAGE_BYTES:
        count = min(SLICE_BYTES, IMAGE_BYTES - first)
        part = read_slice(ser, first, count, count * 2)
        if part is None or len(part) != count:
            print(f"no answer for bytes {first}..{first + count}", file=sys.stderr)
            return 1
        image += part
        first += count
    ser.close()

    rows = []
    for y in range(HEIGHT):
        line = []
        for x in range(WIDTH):
            byte = image[y * (WIDTH // 8) + x // 8]
            white = (byte >> (7 - x % 8)) & 1  # EpdDisplay::convert: 1 = white dot
            line.append("." if white else "#")
        rows.append("".join(line))
    with open(f"{args.out}.txt", "w") as f:
        f.write("\n".join(rows) + "\n")

    # PBM P4 packs bit 1 as black, the opposite of the panel's encoding.
    with open(f"{args.out}.pbm", "wb") as f:
        f.write(f"P4\n{WIDTH} {HEIGHT}\n".encode())
        for y in range(HEIGHT):
            for xb in range(WIDTH // 8):
                byte = 0
                for bit in range(8):
                    white = (image[y * 25 + xb] >> (7 - bit)) & 1
                    if not white:
                        byte |= 0x80 >> bit
                f.write(bytes([byte]))

    ink = sum(row.count("#") for row in rows)
    print(f"{args.out}.txt and {args.out}.pbm written: {ink} ink pixels of {WIDTH * HEIGHT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
