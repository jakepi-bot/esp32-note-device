#!/usr/bin/env python3
"""Measure how often a resting notes device actually sleeps - and what wakes it.

The click is a load step on the codec's supply, and the codec and its amplifier
are fed from the same rails as the CPU: a device that leaves light sleep once a
second clicks once a second. So the number worth having is wakes per second at
rest, and the device cannot answer for it while it is asleep (the USB port is not
a wake source, and a light-sleeping chip's CDC link is dead).

The firmware keeps two counters in RTC memory, which survives a software reset:

  * `wakes` -> "@wakes <light-sleep exits>, <speaker idle waits>, <s uptime>, <ms asleep>"
  * `wakes reset` zeroes both.

So this probe:

  1. opens the console (which resets the chip - expected), zeroes the counters
     and puts the device into the resting state with `epd idle on`;
  2. polls `wakes` until the console stops answering, which is the moment the
     chip genuinely went to sleep (the boot Bluetooth window holds light sleep
     off for ~142 s, so that wait is part of the run);
  3. leaves it alone for --rest seconds;
  4. reboots it over USB (the only way back) and reads the counters.

Exits per second is the result. A resting device should leave light sleep about
once per 10 s idle wait; anything near 1 a second is a task polling the CPU
awake, which is exactly the class this probe exists to catch.

  usage: idle_wake_probe.py [--rest 120] [--port /dev/ttyACM0]

Needs pyserial (the platformio venv has it) and `sudo -n` for the USB
re-enumeration that brings a sleeping chip's port back.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

import serial

# The board on this bench is a hub-less root port; find it rather than pinning it.
USB_ID = "303a"


def usb_device_dir():
    out = subprocess.run(["grep", "-l", USB_ID, *__import__("glob").glob("/sys/bus/usb/devices/*/idVendor")],
                         capture_output=True, text=True).stdout.split()
    return os.path.dirname(out[0]) if out else None


def recover_port(timeout=90):
    """Re-enumerate the board's USB port until ttyACM comes back.

    A light-sleeping ESP32-S3 stops answering USB control transfers, so the host
    drops the device and any esptool/pyserial open fails with EPROTO. Toggling
    `authorized` catches the chip in one of its brief awake windows; the chip
    keeps running (its RTC counters survive), and the next esptool/pyserial open
    resets it into the app.
    """
    dev = usb_device_dir()
    if not dev:
        return False
    deadline = time.time() + timeout
    while time.time() < deadline:
        subprocess.run(["sudo", "-n", "sh", "-c",
                        f"echo 0 > {dev}/authorized 2>/dev/null; sleep 1; echo 1 > {dev}/authorized 2>/dev/null"],
                       check=False, capture_output=True)
        time.sleep(2)
        if os.path.exists("/dev/ttyACM0"):
            return True
    return False


def reset_chip(esptool):
    """Hard-reset the chip over the USB-JTAG interface.

    A light-sleeping chip answers no console, so the counters are read from the
    boot that follows a reset - and only a reset can be counted on, since the
    chip keeps its RTC counters but loses its awake state.
    """
    exe = esptool or shutil.which("esptool") or os.path.expanduser(
        "~/.hermes/cache/scratch/venv-esptool/bin/esptool")
    # `default_reset`, not `usb_reset`: the reset sequence over DTR/RTS is handled
    # by the USB-Serial-JTAG peripheral in hardware, so it works on a chip whose
    # firmware is asleep. `usb_reset` needs the firmware itself to answer a
    # control transfer, which a light-sleeping chip cannot do.
    out = subprocess.run([exe, "--chip", "esp32s3", "--port", "/dev/ttyACM0",
                          "--before", "default_reset", "--after", "hard-reset", "chip_id"],
                         capture_output=True, text=True)
    return out.returncode == 0


class Console:
    def __init__(self, port, baud=115200):
        self.ser = serial.Serial(port, baud, timeout=0.2)

    def cmd(self, line, wait=6.0):
        try:
            self.ser.reset_input_buffer()
            self.ser.write(line.encode() + b"\r\n")
        except (serial.SerialException, OSError):
            return None
        deadline, out = time.time() + wait, b""
        while time.time() < deadline:
            try:
                chunk = self.ser.read(256)
            except (serial.SerialException, OSError):
                return None
            if chunk:
                out += chunk
                if b"@wakes " in out or b"@ok counters" in out or b"@error" in out:
                    break
        text = out.decode("utf-8", "replace")
        return text if "@" in text else None

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


def read_counters(c, tries=3):
    for _ in range(tries):
        answer = c.cmd("wakes")
        if answer:
            m = re.search(r"@wakes (\d+) light-sleep exits(?: \([^)]*\))?, (\d+) speaker idle waits, (\d+) s since boot",
                          answer)
            if m:
                return int(m.group(1)), int(m.group(2))
        time.sleep(2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("HG_PORT", "/dev/ttyACM0"))
    ap.add_argument("--settle", type=float, default=8.0)
    ap.add_argument("--rest", type=float, default=120.0)
    ap.add_argument("--gate", type=float, default=0.5,
                    help="fail if the rest exceeded this many wakes per second")
    ap.add_argument("--esptool", default="", help="esptool binary, if not on PATH")
    args = ap.parse_args()

    if not recover_port(20):
        print("no ttyACM: the board is not on USB", file=sys.stderr)
        return 2
    reset_chip(args.esptool)  # a known boot, whatever state the last run left
    time.sleep(2)
    c = Console(args.port)
    print(f"console open (this resets the chip); settling {args.settle:.0f}s", flush=True)
    time.sleep(args.settle)
    # The open itself resets the chip, and a boot that has not reached
    # `console::begin()` yet answers nothing: ask again rather than call a good
    # image broken.
    for _ in range(5):
        if c.cmd("wakes reset"):
            break
        time.sleep(2)
    else:
        print("the build on the chip has no `wakes` verb", file=sys.stderr)
        c.close()
        return 2
    print("counters zeroed; `epd idle on`", flush=True)
    c.cmd("epd idle on")

    # Poll until the console stops answering: that is the chip actually sleeping
    # (the boot Bluetooth window holds light sleep off for ~142 s).
    quiet_since, last_ok = None, time.time()
    while True:
        answer = read_counters(c)
        if answer:
            last_ok = time.time()
            quiet_since = None
        else:
            quiet_since = quiet_since or time.time()
            if time.time() - quiet_since > 20:
                break
        if time.time() - last_ok > 240:
            break
        time.sleep(4)
    t_sleep = last_ok
    c.close()
    print(f"asleep at +{time.time() - t_sleep:.0f}s of quiet; resting {args.rest:.0f}s more")

    time.sleep(args.rest)
    if not recover_port(120):
        print("the port did not come back; press BOOT on the device", file=sys.stderr)
        return 2
    if not reset_chip(args.esptool):
        print("the chip did not reset over USB; press BOOT on the device", file=sys.stderr)
        return 2
    c = Console(args.port)
    time.sleep(args.settle + 4)  # the reboot the reset started
    count = read_counters(c, tries=8)
    t_read = time.time()
    c.close()
    if not count:
        print("no answer after the reboot", file=sys.stderr)
        return 2

    exits, waits = count
    elapsed = t_read - t_sleep
    rate = exits / elapsed if elapsed > 0 else 0.0
    print(f"@wakes {exits} light-sleep exits, {waits} speaker idle waits over {elapsed:.0f}s")
    print(f"-> {rate:.2f} wakes/second at rest")
    if exits == 0:
        print("the counters did not survive the reset (power cycle, or RTC memory cleared)")
        return 2
    verdict = "PASS" if rate <= args.gate else "FAIL"
    print(f"{verdict}: a resting device must wake about once per 10 s idle wait, not once a second")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
