#!/usr/bin/env python3
"""Find a periodic transient in a recording: is there a tick once a second?

High-passes, rectifies, smooths into an envelope, then autocorrelates the
envelope: a periodic click shows a peak at its period, room noise does not.
"""
import struct
import sys
import wave

import numpy as np


def load(path):
    with wave.open(path) as w:
        rate = w.getframerate()
        ch = w.getnchannels()
        width = w.getsampwidth()
        raw = w.readframes(w.getnframes())
    assert width == 2, width
    x = np.frombuffer(raw, dtype="<i2").astype(np.float32)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, rate


def envelope(x, rate, win=0.002):
    n = max(1, int(rate * win))
    k = np.ones(n, dtype=np.float32) / n
    x = x - np.convolve(x, np.ones(4096, dtype=np.float32) / 4096, mode="same")  # remove DC/rumble
    return np.convolve(np.abs(x), k, mode="same")


def report(path, lo=0.3, hi=2.2):
    x, rate = load(path)
    env = envelope(x, rate)
    # Downsample the envelope to 200 Hz so lags are cheap.
    step = max(1, rate // 200)
    e = env[::step]
    fs = rate / step
    e = e - e.mean()
    ac = np.correlate(e, e, mode="full")[len(e) - 1:]
    ac /= ac[0] if ac[0] else 1.0
    lags = np.arange(len(ac)) / fs
    band = (lags >= lo) & (lags <= hi)
    # Peaks in the band, strongest first.
    idx = np.where(band & (ac > 0.05))[0]
    order = idx[np.argsort(ac[idx])[::-1]][:6]
    print(f"\n{path}: {len(x) / rate:.1f} s @ {rate} Hz")
    print(f"  envelope: median |diff| {np.median(np.abs(np.diff(env))):.0f}, max {env.max():.0f}")
    print(f"  autocorrelation peaks (lag s, r):")
    for i in order:
        print(f"    {lags[i]:.3f}  {ac[i]:.3f}")
    if not len(order):
        print("    none above 0.05: no periodic transient")
    # How many isolated transients per second above a high threshold?
    thr = np.percentile(env, 99.9)
    above = env > max(thr, 4 * np.median(env))
    edges = np.count_nonzero(above[1:] & ~above[:-1])
    print(f"  99.9th-percentile threshold {thr:.0f}: {edges} transient starts over {len(x) / rate:.0f} s"
          f" -> {edges / (len(x) / rate):.2f}/s")


if __name__ == "__main__":
    for path in sys.argv[1:]:
        report(path)
