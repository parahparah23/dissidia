#!/usr/bin/env python3
"""Two-copy multiplayer test (issue #66): boot two copies headless in LAN mode on this machine, walk both into a
Communications > Group Battle fight (A creates the room, B joins, Party A / Party B, Tournament, both Lightning), press a
fixed list of moves on both through the debug socket, then report the lock-step statistics and the desync check.

  mp_test.py --work-a DIR --work-b DIR [--secs 60] [--env 'K=V ...'] [--env-a ...] [--env-b ...] [--out DIR]

Examples:
  mp_test.py --work-a ~/dissidia/work --work-b ~/dissidia/work2                                    # plain match
  mp_test.py ... --env 'PSPRECOMP_NET_LAG_MS=100'                                                  # 200 ms round trip
  mp_test.py ... --env 'PSPRECOMP_NET_LAG_MS=100 PSPRECOMP_NET_INPUT_DELAY=3'                      # with input delay
  mp_test.py ... --env-a 'PSPRECOMP_NET_PERTURB=600'                                               # must report frame 601
  mp_test.py ... --rstick                                                                          # views turned while moving

Each work folder needs a save in ms0/ (copied for the run, the folder's own is not touched) and a runtime built with
games/dissidia/runtime/netplay.cpp. The copies get a private settings file (30 fps, LAN), debug ports 9311 / 9312 and
PSPRECOMP_NET_TRACE=1. Rendering goes to the GPU without a window (SDL offscreen + the NVIDIA EGL vendor file when present).
Prints, per copy: frames, frames the battle waited for input ("stalled"), the run-ahead (lead = local input delay) and
the desync check's summary. Exit 1 when the walk fails or the desync check found a mismatch (unless --expect-desync)."""
import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
BTN = {'up': 0x10, 'down': 0x40, 'cross': 0x4000, 'circle': 0x2000, 'right': 0x20, 'left': 0x80}
PORT = {'A': 9311, 'B': 9312}
# the moves pressed on both sides: (stick x y ms) or (buttons ms); B runs the list three entries later
MOVES = [('A', '255 128 900'), ('B', '8000 150'), ('A', '128 0 700'), ('B', '2000 150'), ('B', '0 128 600'), ('B', '4000 150'),
         ('B', '1000 150'), ('A', '128 255 800'), ('B', '8000 150'), ('B', '2000 150'), ('A', '255 255 500'), ('B', '2000 150')]
# --rstick: also turn each player's view with the right stick ('R': x y ms) while the left stick moves (the camera must not
# leak into the simulation: docs/multiplayer.md "Camera and stick")
RSTICK_MOVES = [('R', '255 128 500'), ('A', '128 0 900'), ('R', '0 100 300')]


class Copy:
    def __init__(self, side, work, out, env):
        self.side, self.work, self.out, self.env = side, work, out, env
        self.log = os.path.join(out, 'run_%s.log' % side)
        self.proc = None

    def start(self):
        ms0 = os.path.join(self.out, 'ms0_' + self.side)
        shutil.rmtree(ms0, ignore_errors=True)
        shutil.copytree(os.path.join(self.work, 'ms0'), ms0)
        ini = os.path.join(self.out, 'settings_%s.ini' % self.side)
        with open(ini, 'w') as f:
            f.write('display=window\nfps=30\nrender_scale=1\nwidescreen=0\nvoice=en\ncheck_updates=0\nhd_textures=0\nnet_mode=lan\n')
        env = dict(os.environ, PSPRECOMP_SETTINGS=ini, PSPRECOMP_MS0=ms0, PSPRECOMP_ADHOC_MODE='lan', PSPRECOMP_ADHOC_NICK=self.side,
                   PSPRECOMP_DEBUG_PORT=str(PORT[self.side]), PSPRECOMP_CHARMOD='0', PSPRECOMP_AUDIO='0', PSPRECOMP_SAVEDATA_AUTO='0',
                   PSPRECOMP_NET_TRACE='1', SDL_VIDEODRIVER='offscreen')
        egl = '/usr/share/glvnd/egl_vendor.d/10_nvidia.json'
        if os.path.exists(egl):
            env['__EGL_VENDOR_LIBRARY_FILENAMES'] = egl
        env.update(self.env)
        if os.path.exists(self.log):
            os.remove(self.log)
        self.proc = subprocess.Popen([os.path.join(HERE, 'run.sh'), self.work, '--window', '480x272', '--log', self.log], env=env,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
                self.proc.wait(10)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except OSError:
                    pass

    def text(self):
        try:
            with open(self.log, 'rb') as f:
                return f.read().decode(errors='replace')
        except OSError:
            return ''

    def cmd(self, line):
        with socket.create_connection(('127.0.0.1', PORT[self.side]), timeout=15) as s:
            s.sendall(line.encode() + b'\n')
            return s.makefile('rb').readline().decode(errors='replace').strip()

    def press(self, *names, gap=0.8):
        for n in names:
            self.cmd('B %X 120' % BTN[n])
            time.sleep(gap)


def wait(pred, timeout, what):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return
        time.sleep(0.5)
    raise RuntimeError('timeout: ' + what)


def walk(a, b):
    both = (a, b)
    for c in both:
        wait(lambda: 'Debug socket listening' in c.text(), 90, 'debug socket ' + c.side)
    for c in both:   # title: CROSS until the save list opens, CROSS loads the save
        end = time.time() + 150
        while not ('list dialog opened' in c.text() or 'complete: mode=4' in c.text()):
            if time.time() > end:
                raise RuntimeError('the save list never opened (%s)' % c.side)
            c.press('cross', gap=1.0)
        time.sleep(0.6)
        c.press('cross')
    for c in both:
        wait(lambda: 'complete: mode=4 result=0x00000000' in c.text(), 60, 'save load ' + c.side)
    time.sleep(15)
    for c in both:
        c.press('down', 'down', 'cross', gap=1.2)          # Communications Mode
    time.sleep(2)
    for c in both:
        c.press('down', 'cross', gap=1.2)                  # Group Battle
    time.sleep(4)
    a.press('cross'); time.sleep(4)                        # A: Create Room (Custom)
    b.press('down', 'down', 'cross'); time.sleep(6)        # B: Search for a Room
    b.press('cross'); time.sleep(5)                        # B: request entry
    a.press('up', 'cross'); time.sleep(2)                  # A: Start Group Battle -> YES
    a.press('cross'); time.sleep(8)
    a.press('cross'); b.press('right', 'cross')            # Party A / Party B
    time.sleep(12)
    a.press('cross'); time.sleep(5)                        # A: Tournament
    for c in both:
        c.press('cross', gap=2.0)                          # the 1st slot -> Player Select
    for c in both:
        c.press('cross', gap=1.5)                          # Lightning
    for c in both:
        c.press('cross', gap=3.0)                          # YES
    for c in both:
        c.press('down', 'down', 'down', 'down', 'down', gap=0.5)
    for c in both:
        c.press('cross', gap=0.2)                          # Enter Battle
    end = time.time() + 90
    while time.time() < end:                               # "Next" on the opponent pages until the lock-step runs
        busy = [c for c in both if 'lock-step battle: input delay' not in c.text()]
        if not busy:
            return
        time.sleep(3)
        for c in busy:
            c.press('cross', gap=0.1)
    raise RuntimeError('the battle never started')


def exercise(a, b, secs, moves=MOVES):
    end, i = time.time() + secs, 0
    while time.time() < end:
        for c, off in ((a, 0), (b, 3)):
            kind, arg = moves[(i + off) % len(moves)]
            c.cmd(('A ' if kind == 'A' else 'A2 ' if kind == 'R' else 'B ') + arg)
        i += 1
        time.sleep(0.7)


def stats(c):
    rows = []
    for l in c.text().splitlines():
        m = re.match(r'\[NETLS\] f=(\d+) rc=(\d+) frame=(\d+) seq=(\d+) lead=(-?\d+)', l)
        if m and m.group(2) in ('0', '1'):
            rows.append(tuple(int(x) for x in m.groups()))
    rows = [r for r in rows if r[2] > 60]   # past the first 2 s
    if not rows:
        return '%s: no lock-step frames' % c.side
    stalls = sum(1 for r in rows if r[1] == 1)
    leads = Counter(r[4] for r in rows)
    sync = [l for l in c.text().splitlines() if l.startswith('[NETSYNC]')]
    summ = [l for l in sync if 'so far' in l or 'battle over' in l]
    first = [l for l in sync if 'DESYNC' in l]
    return '%s: update calls %d, stalled %d (%.1f%%), lead %s\n   %s%s' % (
        c.side, len(rows), stalls, 100.0 * stalls / len(rows), ' '.join('%d:%d' % kv for kv in sorted(leads.items())),
        summ[-1] if summ else '(no desync summary yet)', ('\n   ' + first[0]) if first else '')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work-a', required=True)
    ap.add_argument('--work-b', required=True)
    ap.add_argument('--secs', type=float, default=60)
    ap.add_argument('--env', default='')
    ap.add_argument('--env-a', default='')
    ap.add_argument('--env-b', default='')
    ap.add_argument('--out', default=os.path.join(os.getcwd(), 'mp_test_out'))
    ap.add_argument('--rstick', action='store_true', help='also turn both views with the right stick while moving')
    ap.add_argument('--keep', action='store_true', help='leave both copies running afterwards')
    ap.add_argument('--expect-desync', action='store_true', help='exit 0 only when a desync was reported (the perturbation test)')
    a = ap.parse_args(argv)
    a.out = os.path.abspath(os.path.expanduser(a.out))
    os.makedirs(a.out, exist_ok=True)
    kv = lambda s: dict(x.split('=', 1) for x in s.split() if '=' in x)
    A = Copy('A', os.path.abspath(os.path.expanduser(a.work_a)), a.out, {**kv(a.env), **kv(a.env_a)})
    B = Copy('B', os.path.abspath(os.path.expanduser(a.work_b)), a.out, {**kv(a.env), **kv(a.env_b)})
    try:
        A.start()
        B.start()
        t0 = time.time()
        walk(A, B)
        print('[mp_test] in battle after %.0f s' % (time.time() - t0), file=sys.stderr)
        exercise(A, B, a.secs, RSTICK_MOVES + MOVES if a.rstick else MOVES)
        time.sleep(11)   # one more desync summary
        for c in (A, B):
            print(stats(c))
        desync = any('DESYNC' in c.text() for c in (A, B))
        return 0 if desync == a.expect_desync else 1
    finally:
        if not a.keep:
            A.stop()
            B.stop()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
