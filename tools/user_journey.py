#!/usr/bin/env python3
"""Walk the device the way a person does, and fail loudly where it stalls.

The console can press keys and report the screen (`ui`), the queue
(`notes status`) and the radio. This drives a real journey over that console -
power on, record at once, stop, menu, back, record again, power off and on - and
marks each step against a time budget. The budgets are what a person notices: a
menu that takes more than a second feels broken, and a note that sits pending for
more than two minutes looks lost.

    tools/user_journey.py            # the whole journey
    tools/user_journey.py --no-reset # skip the power-on step

Exit code 0 only if every step passed.
"""

import glob
import subprocess
import sys
import time

import serial

ESPTOOL = '/home/jake/.platformio/packages/tool-esptool/esptool'

RESULTS = []


# --------------------------------------------------------------------------- io


def find_port(wait=25.0):
    deadline = time.time() + wait
    while time.time() < deadline:
        for name in sorted(glob.glob('/dev/ttyACM*')):
            try:
                probe = serial.Serial(name, 115200, timeout=0.2)
                probe.write(b'\r\n')
                probe.close()
                return name
            except OSError:
                continue
        time.sleep(0.5)
    raise SystemExit('no serial port answered')


class Device:
    """Talk to the console the way the console answers: a line, then a reply.

    Reads stop as soon as the reply arrives or the deadline passes, so a poll
    costs milliseconds. A fixed sleep per command - this file's first version -
    put a 3 s floor under every measurement and hid the very stalls it was meant
    to catch.
    """

    def __init__(self):
        self.port = find_port()
        self.s = serial.Serial(self.port, 115200, timeout=0.05)
        self._drain(0.6)

    def _drain(self, limit=0.3):
        deadline = time.time() + limit
        buf = b''
        while time.time() < deadline:
            try:
                chunk = self.s.read(4096)
            except serial.SerialException:
                raise ConsoleGone()
            if chunk:
                buf += chunk
                if b'\n' in buf:
                    break
            else:
                time.sleep(0.01)
        return buf.decode('utf-8', 'replace')

    def cmd(self, text, wait=1.2):
        self.s.write((text + '\r\n').encode())
        deadline = time.time() + wait
        buf = ''
        while time.time() < deadline:
            buf += self._drain(0.06)
            if any(l.strip().startswith('@') or 'screen ' in l for l in buf.splitlines()):
                break
        return buf

    def tap(self, key):
        """Press a key and do not wait for the screen: the caller times that."""
        self.s.write(('key %s\r\n' % key).encode())
        time.sleep(0.05)

    def ui(self, wait=0.8):
        out = self.cmd('ui', wait)
        for line in out.splitlines():
            i = line.find('screen ')
            if i >= 0:
                return line[i:].strip()
        return ''

    def screen(self):
        text = self.ui()
        return text.split(',')[0].replace('screen ', '').strip() if text else '?'

    def status(self):
        out = self.cmd('notes status', 2.5)
        for line in out.splitlines():
            if line.strip().startswith('@notes stored'):
                return line.strip()
        return ''

    def notes(self):
        line = self.status()
        try:
            stored = int(line.split('stored ')[1].split()[0])
            pending = int(line.split('pending ')[1].split()[0])
            failed = int(line.split('failed ')[1].split()[0])
            return stored, pending, failed
        except (IndexError, ValueError):
            return None

    def radio_duty(self):
        line = self.status()
        try:
            pair = [p for p in line.split() if '/' in p and p.replace('/', '').isdigit()][-1]
            on, total = pair.split('/')
            return float(on) / float(total) if float(total) else 0.0
        except (IndexError, ValueError):
            return None

    def reset(self):
        port = self.port
        try:
            self.s.close()
        except Exception:
            pass
        subprocess.run([ESPTOOL, '--chip', 'esp32s3', '-p', port, '-b', '115200',
                        '--after', 'hard_reset', 'chip_id'], capture_output=True)
        time.sleep(1.5)
        self.port = find_port()
        self.s = serial.Serial(self.port, 115200, timeout=0.05)
        self._drain(0.8)

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


class ConsoleGone(Exception):
    """The device powered off or reset: the port went away mid-read."""


# ------------------------------------------------------------------------ steps


def record(name, ok, detail=''):
    RESULTS.append((name, ok, detail))
    print(('  PASS  ' if ok else '  FAIL  ') + name + ('   ' + detail if detail else ''))
    sys.stdout.flush()


def wait_for(dev, want, budget, poll=0.15):
    """Screen `want` within `budget` seconds; returns the time it took or None."""
    started = time.time()
    while time.time() - started < budget:
        try:
            if dev.screen() == want:
                return time.time() - started
        except ConsoleGone:
            return None
        time.sleep(poll)
    return None


def go_home(dev, tries=4):
    """A person gets out of wherever they are: hold is back on every screen."""
    for _ in range(tries):
        if dev.screen() == 'home':
            return True
        dev.tap('hold')
        time.sleep(0.4)
    return dev.screen() == 'home'


# ---------------------------------------------------------------------- journey


def main():
    reset = '--no-reset' not in sys.argv
    dev = Device()
    print('  port:', dev.port)

    base = dev.notes()
    if base is None:
        record('console answers `notes status`', False)
        return 1
    record('console answers `notes status`', True, str(base))
    stored0 = base[0]

    if reset:
        # 1. Power on. A person presses the key and expects Home, and expects the
        #    device to stay quiet until they ask for something.
        dev.reset()
        took = wait_for(dev, 'home', 30)
        record('power on shows Home', took is not None, '%.1f s' % took if took else 'never')
        time.sleep(6)  # a boot that records by itself does it early
        after = dev.notes()[0]
        record('power on records nothing by itself', after == stored0,
               '%d -> %d notes' % (stored0, after))

    # 2. Record immediately: the first thing a person does.
    go_home(dev)
    stored0 = dev.notes()[0]
    dev.tap('boot')
    took = wait_for(dev, 'recording', 3)
    record('record starts from Home within 3 s', took is not None, '%.2f s' % took if took else 'never')
    time.sleep(3)

    # 3. Stop, and the note is kept.
    dev.tap('boot')
    took = wait_for(dev, 'home', 5)
    record('stop returns Home within 5 s', took is not None, '%.2f s' % took if took else 'never')
    counts = dev.notes()
    record('the note was kept', counts is not None and counts[0] == stored0 + 1,
           '%s -> %s' % (stored0, counts[0] if counts else '?'))

    # 4. Menu, walk it, come back.
    dev.tap('hold')
    took = wait_for(dev, 'menu', 3)
    record('menu opens within 3 s', took is not None, '%.2f s' % took if took else 'never')
    # On the menu the hold is the way back (ui_keys hold_boot: Menu -> Home); PWR
    # selects a row, which is how a first pass walked into the note list and
    # called the menu stuck.
    dev.tap('hold')
    took = wait_for(dev, 'home', 4)
    record('back from the menu returns Home', took is not None, '%.2f s' % took if took else 'never')

    # 5. Record again after the menu.
    go_home(dev)
    stored1 = dev.notes()[0]
    dev.tap('boot')
    took = wait_for(dev, 'recording', 3)
    record('record starts a second time', took is not None, '%.2f s' % took if took else 'never')
    time.sleep(2)
    dev.tap('boot')
    wait_for(dev, 'home', 5)
    counts = dev.notes()
    record('the second note was kept', counts is not None and counts[0] == stored1 + 1,
           '%s -> %s' % (stored1, counts[0] if counts else '?'))

    # 6. The queue drains on its own, on whatever power it has.
    started = time.time()
    drained = None
    while time.time() - started < 120:
        counts = dev.notes()
        if counts and counts[1] == 0:
            drained = time.time() - started
            break
        time.sleep(1.0)
    record('the queue drains within 120 s', drained is not None,
           '%.0f s' % drained if drained else 'still %s' % (counts,))

    # 7. The UI answers while the radio is busy: the one Jake hit on battery.
    duty = dev.radio_duty()
    go_home(dev)
    dev.tap('hold')
    took = wait_for(dev, 'menu', 3)
    record('menu opens with the radio up', took is not None,
           ('%.2f s, radio %.0f%%' % (took, 100 * duty)) if took and duty is not None else 'never')
    go_home(dev)
    dev.tap('boot')
    took = wait_for(dev, 'recording', 3)
    record('record starts with the radio up', took is not None, '%.2f s' % took if took else 'never')
    if dev.screen() == 'recording':
        dev.tap('boot')
        wait_for(dev, 'home', 5)

    # 8. Power off and back on: the cycle a person actually does.
    try:
        dev.cmd('notes poweroff', 1.0)
    except ConsoleGone:
        pass
    time.sleep(3)
    dev.reset()
    took = wait_for(dev, 'home', 30)
    record('power off and on returns to Home', took is not None, '%.1f s' % took if took else 'never')

    dev.close()

    failed = [r for r in RESULTS if not r[1]]
    print('\n%d steps, %d failed' % (len(RESULTS), len(failed)))
    for name, _, detail in failed:
        print('  FAILED:', name, detail)
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
