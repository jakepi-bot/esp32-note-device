#!/usr/bin/env python3
"""Press a key and film the panel: how long until the glass changes?

The console can press keys (`key boot|hold|pwr`), and the camera watches the
panel. The video is the clock: the frame where the panel's pixels move, minus the
moment the key was sent, is the response time a person feels.

usage: tools/latency_probe.py <key> [seconds]   (bench camera on /dev/video0)
"""
import re
import subprocess
import sys
import time

import serial

key = sys.argv[1] if len(sys.argv) > 1 else "hold"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
video = '/tmp/latency_%s.mkv' % key
crop = 'crop=250:320:555:230'  # the panel's text area, from the bench framing

s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3)
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
