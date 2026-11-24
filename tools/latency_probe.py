#!/usr/bin/env python3
"""Press a key and film the panel: how long until the glass changes?

The console can press keys (`key boot|hold|pwr`), and the camera watches the
panel. The video is the clock: the frame where the panel's pixels move, minus the
moment the key was sent, is the response time a person feels.

usage: tools/latency_probe.py <key> [seconds] [crop=W:H:X:Y]   (bench camera on /dev/video0)

A hand-over is only partial for the first three moves of a session: the fourth
`key hold` in a row spends the ghost budget and draws the full 1.4-2 s waveform.
A number taken without resetting the board is therefore the fourth move's, not
the one a person waiting on the menu sees - reset first, or count the moves.
"""
import re
import subprocess
import sys
import time

import glob

import serial


def find_port():
    """The board's CDC port, wherever it enumerated.

    A reset can move the device from ttyACM0 to ttyACM1, and a script that pins
    the old name talks to a dead port: the flash then fails at 13% with "no more
    data to read" (bench, 2026-11-23).
    """
    for name in sorted(glob.glob('/dev/ttyACM*')):
        try:
            port = serial.Serial(name, 115200, timeout=0.3)
            port.write(b'\r\n')
            port.close()
            return name
        except OSError:
            continue
    raise SystemExit('no serial port answered')


key = sys.argv[1] if len(sys.argv) > 1 else "hold"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith('crop=') else 8.0
video = '/tmp/latency_%s.mkv' % key
# The panel's text area. The device gets moved on the bench, so this is an
# argument, not a constant: measure once, look at the frame, adjust.
crop_arg = next((a for a in sys.argv[2:] if a.startswith('crop=')), None)
crop = 'crop=' + crop_arg[5:] if crop_arg else 'crop=240:290:400:430'

s = serial.Serial(find_port(), 115200, timeout=0.3)
s.write(b'\r\n')
time.sleep(1.5)
s.read(30000)

rec = subprocess.Popen(['ffmpeg', '-hide_banner', '-loglevel', 'error',
                        '-f', 'v4l2', '-framerate', '30', '-video_size', '1280x720',
                        '-i', '/dev/video0', '-t', str(seconds), '-y', video])
t0 = time.time()
time.sleep(2.0)
s.write(('key %s\r\n' % key).encode())
t_send = time.time()
rec.wait()
s.close()

# Scene detection on the panel only: each hit is a frame whose pixels moved.
out = subprocess.run(['ffmpeg', '-hide_banner', '-i', video,
                      '-vf', "%s,select='gt(scene,0.010)',showinfo" % crop,
                      '-f', 'null', '-'], capture_output=True, text=True).stderr
all_stamps = [float(m) for m in re.findall(r'pts_time:([0-9.]+)', out)]
send_at = t_send - t0
# Anything before the key is the room: the meter's digits and the camera's own
# exposure drift move pixels too, and only what happens after the press is the
# device's answer.
stamps = [t for t in all_stamps if t >= send_at]
print('video      %s (%.1f s)' % (video, seconds))
print('key sent   at %.2f s' % send_at)
print('pre-press motion: %d frames' % (len(all_stamps) - len(stamps)))
if not stamps:
    print('no panel change after the key')
else:
    first = min(stamps)
    print('first change at %.2f s  ->  %.0f ms after the key' % (first, (first - send_at) * 1000))
    print('changes at %s' % ', '.join('%.2f' % t for t in stamps[:6]))
    print('settled by %.2f s  ->  %.0f ms of drawing' % (stamps[-1], (stamps[-1] - first) * 1000))
