#!/usr/bin/env python3
"""Record the resting device with the amplifier forced on and off, then look for
a once-a-second transient in every take.

The camera's ALSA capture returns about 6 s per take whatever it is asked for, so
this records many takes and analyses each one on its own: a 1 Hz tick puts five
intervals inside a 6 s take, which is enough for the envelope autocorrelation to
show it.

  usage: amp_click_bench.py [--takes 10] [--port /dev/ttyACM0]
"""
import argparse
import glob
import os
import subprocess
import sys
import time
import wave

import numpy as np
import serial

ALSA = "plughw:2,0"


def reset_chip():
    ports = glob.glob("/dev/ttyACM*")
    port = ports[0] if ports else "/dev/ttyACM0"
    exe = os.path.expanduser("~/.hermes/cache/scratch/venv-esptool/bin/esptool")
    r = subprocess.run([exe, "--chip", "esp32s3", "--port", port, "--before", "default_reset",
                        "--after", "hard-reset", "chip_id"], capture_output=True, text=True, timeout=90)
    return r.returncode == 0


class Console:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.1)

    def cmd(self, line, wait=6.0):
        try:
            self.ser.reset_input_buffer()
            self.ser.write(line.encode() + b"\r\n")
        except serial.SerialException:
            return None
        end = time.time() + wait
        out = b""
        while time.time() < end:
            try:
                c = self.ser.read(4096)
            except serial.SerialException:
                return None
            if c:
                out += c
                if b"@ok" in out or b"@error" in out:
                    time.sleep(0.2)
                    out += self.ser.read(8192)
                    break
        return out.decode(errors="replace") if out else None

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


def take(path):
    subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-f", "alsa", "-i", ALSA,
                    "-t", "8", "-ac", "1", "-ar", "16000", "-y", path],
                   capture_output=True, timeout=60)


def load(path):
    with wave.open(path) as w:
        rate, ch = w.getframerate(), w.getnchannels()
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, dtype="<i2").astype(np.float32)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, rate


def periodicity(x, rate, lo=0.5, hi=1.6):
    """Strongest envelope-autocorrelation peak in the band, as (lag_s, r)."""
    x = x - np.convolve(x, np.ones(4096, np.float32) / 4096, mode="same")
    env = np.convolve(np.abs(x), np.ones(32, np.float32) / 32, mode="same")
    step = max(1, rate // 200)
    e = env[::step]
    fs = rate / step
    e = e - e.mean()
    ac = np.correlate(e, e, mode="full")[len(e) - 1:]
    if ac[0] <= 0:
        return 0.0, 0.0
    ac /= ac[0]
    lags = np.arange(len(ac)) / fs
    band = (lags >= lo) & (lags <= hi)
    if not band.any():
        return 0.0, 0.0
    i = np.argmax(np.where(band, ac, -1))
    return float(lags[i]), float(ac[i])


def run(prefix, amp, port, takes):
    reset_chip()
    time.sleep(2)
    c = Console(port)
    time.sleep(8)
    c.cmd("wakes reset")
    c.cmd("epd idle on")
    if amp:
        print("   ", (c.cmd("amp on") or "").strip().splitlines()[0])
    time.sleep(4)
    label = "amp_on" if amp else "amp_off"
    results = []
    for i in range(takes):
        path = f"{prefix}_{label}_{i:02d}.wav"
        take(path)
        x, rate = load(path)
        lag, r = periodicity(x, rate)
        results.append((len(x) / rate, lag, r))
        os.unlink(path)
    c.close()
    return label, results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--takes", type=int, default=10)
    ap.add_argument("--out-prefix", default="amp_click")
    args = ap.parse_args()
    summary = {}
    for amp in (True, False):
        label, results = run(args.out_prefix, amp, args.port, args.takes)
        summary[label] = results
        secs = [r[0] for r in results]
        lags = [r[1] for r in results]
        rs = [r[2] for r in results]
        at_one = sum(1 for lag, r in zip(lags, rs) if 0.9 <= lag <= 1.1 and r > 0.15)
        print(f"  {label}: {len(results)} takes, {np.mean(secs):.1f} s each; "
              f"median peak lag {np.median(lags):.2f} s, median r {np.median(rs):.2f}; "
              f"{at_one}/{len(results)} takes with a 1 s peak above r=0.15")
    for label, results in summary.items():
        print(f"\n{label}:")
        for secs, lag, r in results:
            print(f"  {secs:4.1f} s  lag {lag:.3f} s  r {r:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
