#!/usr/bin/env python3
"""Tap BOOT after the chip has been dozing: does it record, or open the menu?

This is the battery symptom, made reproducible with the console alive: light sleep
is allowed even on USB, the console's `key virtual` drives the real poll path, and
the test idles long enough for the chip to sleep between samples.

    tools/doze_tap.py [rounds]
"""

import glob
import sys
import time

import serial


def find_port():
    for name in sorted(glob.glob('/dev/ttyACM*')):
        try:
            p = serial.Serial(name, 115200, timeout=0.2)
            p.write(b'\r\n')
            p.close()
            return name
        except OSError:
            continue
    raise SystemExit('no port')


class Dev:
    def __init__(self):
        self.s = serial.Serial(find_port(), 115200, timeout=0.05)
        self.drain(0.5)

    def drain(self, limit=0.25):
        end = time.time() + limit
        buf = b''
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                buf += chunk
                if b'\n' in buf:
                    break
            else:
                time.sleep(0.005)
        return buf.decode('utf-8', 'replace')

    def cmd(self, text, wait=1.0):
        self.s.write((text + '\r\n').encode())
        end = time.time() + wait
        buf = ''
        while time.time() < end:
            buf += self.drain(0.05)
            if any(l.strip().startswith('@') or 'screen ' in l for l in buf.splitlines()):
                break
        return buf

    def screen(self, wait=0.9):
        for line in self.cmd('ui', wait).splitlines():
            i = line.find('screen ')
            if i >= 0:
                return line[i:].split(',')[0].replace('screen ', '').strip()
        return '?'

    def press(self, key, hold_s=0.15):
        """A finger: down, wait, up - through the poll path."""
        self.s.write(('key virtual %s down\r\n' % key).encode())
        time.sleep(hold_s)
        self.s.write(('key virtual %s up\r\n' % key).encode())
        time.sleep(0.05)

    def wait_for(self, want, budget):
        start = time.time()
        while time.time() - start < budget:
            if self.screen() == want:
                return time.time() - start
            time.sleep(0.1)
        return None

    def home(self):
        for _ in range(4):
            if self.screen() == 'home':
                return True
            self.drain(0.1)
            self.press('boot', 2.6)  # hold = back / open the menu
            self.drain(0.2)
        return self.screen() == 'home'


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    dev = Dev()
    dev.home()
    print('start:', dev.screen())

    taps, holds = [], []
    for i in range(rounds):
        # Doze: long enough that the chip has certainly slept between samples.
        time.sleep(20)
        took = dev.wait_for('recording', 4) if False else None
        dev.press('boot', 0.15)
        took = dev.wait_for('recording', 4)
        seen = dev.screen()
        taps.append((seen, took))
        print('  tap %d: %s%s' % (i + 1, seen, '  %.2f s' % took if took else ''))
        if seen == 'recording':
            dev.press('boot', 0.15)
            dev.wait_for('home', 5)
        else:
            dev.home()

    # A real hold must still open the menu, even after a doze.
    time.sleep(20)
    dev.press('boot', 2.6)
    took = dev.wait_for('menu', 4)
    holds.append(dev.screen())
    print('  hold: %s%s' % (dev.screen(), '  %.2f s' % took if took else ''))

    ok_taps = sum(1 for s, _ in taps if s == 'recording')
    print('\ntaps that recorded: %d/%d, hold opened the menu: %s'
          % (ok_taps, rounds, holds == ['menu']))
    return 0 if ok_taps == rounds and holds == ['menu'] else 1


if __name__ == '__main__':
    sys.exit(main())
