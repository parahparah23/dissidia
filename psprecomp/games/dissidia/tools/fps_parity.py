#!/usr/bin/env python3
"""Frame-rate parity tests: every move of every playable character must play the same at every frame rate the game
offers as at 30 fps (the PSP's rate, the reference). See docs/testing.md "Frame-rate parity tests".

  fps_parity.py run     --work W [--chars all | squall,exdeath,10] [--rates 30,60,90,120,150,180,210,240] [--out DIR] [--jobs 4]
                        [--actions 1234,1235] [--variants plain,mash] [--repeat-base]
  fps_parity.py compare DIR [--chars ...]          (compare finished runs again, e.g. with other tolerances)
  fps_parity.py chars                              (the playable characters and their charIds)

How a run works
  * One game per (character, rate), each on its own virtual display with its own memory stick copy and debug port
    (battle_test.Game). Nothing is played through menus: the recorded Quick Battle walk (tests/replays/quick_battle_walk.txt,
    Squall vs Lightning) is replayed inside the game, and PSPRECOMP_TEST_CHAR_SWAP (runtime/direct_battle.cpp) turns Squall
    into the character under test (with his own save record and model); for Lightning the dummy becomes Squall.
  * The dummy (the CPU fighter) never presses a button (PSPRECOMP_TEST_NO_AI / _AI_OFF) but runs and reacts to hits, with a
    huge HP pool so no hit ends the fight.
  * The game lists the fighter's actions (plya records: action id, script, first / later press, ground / air, button), then
    this tool writes the trials into the running battle (test script `batch`): for each action
      - plain: both fighters back at their spots facing each other (`home`), start the action (`action <id>`), let it run;
      - mash (first presses of an attack only): the same, and the move's button tapped every 200 ms for 2.4 s, so the
        follow-up presses (combos, chains) happen too.
    Air actions start 0.4 s after a jump. Every trial starts on the 30 Hz lattice (PSPRECOMP_TEST_PARITY), so the same game
    moment exists at every rate.
  * The runtime logs a sample of both fighters on every 30 Hz frame ([PAR]: script, motion, motion time, position, facing)
    and every hit they take ([PARHIT]: attack id, reaction, damage).
  * Each rate is compared with 30 fps, trial by trial:
      - the player's script sequence and the dummy's motion sequence (same ids in the same order, each change within
        --frame-tol 30 Hz frames);
      - the hits the dummy took (same count, same attacks in the same order, each within --frame-tol frames, same damage);
      - the paths of both fighters (largest gap at the same game moment <= --pos-tol metres, or --pos-rel of the distance
        the fighter covered);
      - the length of the trial (until the player is idle again) within --frame-tol frames.
    With --repeat-base a second 30 fps run is made: trials where two 30 fps runs already disagree are reported as
    NONDETERMINISTIC instead of failing.
  * Output: <out>/<char>/report.txt (every failing trial with its reasons), <out>/summary.txt, <out>/results.json; exit code 1
    when any trial fails.

Needs what battle_test.py needs (Xvfb, a work folder with a save that has Quick Battle unlocked) and a runtime built with this
branch's test hooks. Runs are long: ~3-6 min per character and rate on a busy 20-core box; --jobs N runs N games at once."""
import argparse
import bisect
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import battle_test as bt  # noqa: E402

RATES = [30, 60, 90, 120, 150, 180, 210, 240]   # framerate.cpp k_rates: every rate the settings menu offers
WALK = os.path.join(HERE, '..', 'tests', 'replays', 'quick_battle_walk.txt')
WALK_PLAYER, WALK_CPU = 15, 31                   # who the recorded walk picks (Squall vs Lightning)

# charId -> (name, code). The 31 playable characters of Dissidia 012 (code strings at 0x089D6CA8; 23 = Chaos, the boss, is not
# playable, 33 = Aerith is an assist only). https://dissidia.wiki/Dissidia_012_Final_Fantasy
CHARS = {
    1: ('Warrior of Light', 'one100'), 2: ('Garland', 'one200'), 3: ('Firion', 'two100'), 4: ('The Emperor', 'two200'),
    5: ('Onion Knight', 'thr100'), 6: ('Cloud of Darkness', 'thr200'), 7: ('Cecil', 'for100'), 8: ('Golbez', 'for200'),
    9: ('Bartz', 'fiv100'), 10: ('Exdeath', 'fiv200'), 11: ('Terra', 'six100'), 12: ('Kefka', 'six200'),
    13: ('Cloud', 'sev100'), 14: ('Sephiroth', 'sev200'), 15: ('Squall', 'eht100'), 16: ('Ultimecia', 'eht200'),
    17: ('Zidane', 'nin100'), 18: ('Kuja', 'nin200'), 19: ('Tidus', 'ten100'), 20: ('Jecht', 'ten200'),
    21: ('Shantotto', 'gst100'), 22: ('Gabranth', 'gst200'), 24: ('Kain', 'for110'), 25: ('Gilgamesh', 'fiv210'),
    26: ('Tifa', 'sev110'), 27: ('Laguna', 'eht110'), 28: ('Yuna', 'ten110'), 29: ('Prishe', 'gst110'), 30: ('Vaan', 'gst120'),
    31: ('Lightning', 'gst130'), 32: ('Feral Chaos', 'org210'),
}
BUTTON = {1: 'CIRCLE', 2: 'SQUARE'}              # plya +0x0F: 1 brave (Circle), 2 HP (Square)
# Movement every fighter has (pad input, not plya actions; default controls: Cross jump, R + Cross dodge, R + Triangle dash,
# R block). Each is a list of test script lines with times relative to the trial start (input begins at +0).
MOVES = {
    'jump':        ['+0 tap CROSS 67'],
    'double_jump': ['+0 tap CROSS 67', '+400 tap CROSS 67'],
    'dodge':       ['+0 tap R+CROSS 67'],
    'air_dodge':   ['+0 tap CROSS 67', '+400 tap R+CROSS 67'],
    'dash':        ['+0 set R+TRIANGLE', '+1200 set -'],
    'air_dash':    ['+0 tap CROSS 67', '+400 set R+TRIANGLE', '+1400 set -'],
    'block':       ['+0 set R', '+1000 set -'],
    'run':         ['+0 set - 128 0', '+1500 set -'],
    'run_jump':    ['+0 set - 128 0', '+600 set CROSS 128 0', '+667 set - 128 0', '+1500 set -'],
}
FRAME_MS = 1000.0 / 30.0
HOME_DIST, HOME_X, HOME_Z = 3.5, 0.0, 0.0       # every trial: the dummy at stage point (x, z), the player HOME_DIST m from it, facing each other


def slug(cid):
    return CHARS[cid][0].lower().replace(' ', '_')


def parse_chars(spec):
    if spec in ('all', ''):
        return sorted(CHARS)
    if spec == 'none':                                     # e.g. only the Jecht combo test: --chars none --jecht-combos
        return []
    out = []
    for t in spec.split(','):
        t = t.strip().lower()
        if t.isdigit():
            out.append(int(t))
            continue
        hit = [c for c, (n, code) in CHARS.items() if t in (n.lower(), n.lower().replace(' ', '_'), n.lower().replace(' ', ''), code)]
        if not hit:
            raise SystemExit('unknown character %r (fps_parity.py chars lists them)' % t)
        out.append(hit[0])
    return out


# ---- one game: one character at one rate ------------------------------------------------------------------------------

_start_lock = threading.Lock()


def trial_lines(acts, variants, only=None, moves=True):
    """the batch of trials for a fighter's action list [(id, script, later, side, button)] (the same text at every rate)"""
    lines = ['+0 waitidle 8000', '+0 home %.2f %.2f %.2f' % (HOME_DIST, HOME_X, HOME_Z), '+3000 mark SETTLE']   # the first seconds after the intro are not
    for name, steps in (MOVES.items() if moves and not only else []):                                       # repeatable (a first trial gave 0 or 19 hits)
        lines.append('+0 waitidle 8000')
        lines.append('+0 home %.2f %.2f %.2f' % (HOME_DIST, HOME_X, HOME_Z))
        lines.append('+1100 mark T 0 %s' % name)               # 1 s to settle first: running is camera-relative, and the camera swings after `home`
        for st in steps:
            t, rest = st.split(' ', 1)
            lines.append('+%d %s' % (1200 + int(t[1:]), rest))
        lines.append('+4500 mark E 0 %s' % name)
    for (aid, scr, later, side, button) in acts:
        if only and aid not in only:
            continue
        for v in variants:
            if v == 'mash' and (later or button not in BUTTON):
                continue
            lines.append('+0 waitidle 8000')
            lines.append('+0 home %.2f %.2f %.2f' % (HOME_DIST, HOME_X, HOME_Z))
            lines.append('+100 mark T %d %s' % (aid, v))
            t = 700                                         # 0.6 s after `home`: right after the teleport a jump was sometimes not taken and the
            if side == 1:                                   # dummy sometimes not hittable (still getting up); an air action: jump first
                lines.append('+700 tap CROSS 100')
                t = 1100
            lines.append('+%d action %d player' % (t, aid))
            if v == 'mash':
                for k in range(12):
                    lines.append('+%d tap %s 67' % (t + 300 + 200 * k, BUTTON[button]))
            lines.append('+%d mark E %d %s' % (t + 3300, aid, v))
    lines.append('+0 waitidle 8000')
    lines.append('+500 mark DONE')
    lines.append('+600 quit')
    return lines


def run_one(a, cid, rate, out, tag=None):
    """boot, list the actions, play every trial, quit. Returns the log path (raises on failure)."""
    tag = tag or str(rate)
    os.makedirs(out, exist_ok=True)
    log = os.path.join(out, 'run_%d.log' % rate)
    batch = os.path.join(out, 'batch_%s.txt' % tag)
    script = os.path.join(out, 'script_%s.txt' % tag)
    for p in (batch, log):
        if os.path.exists(p):
            os.remove(p)
    with open(script, 'w') as f:
        f.write('1000 action -1 player\n1500 batch %s\n' % batch)
    swap = '%d=%d' % (WALK_PLAYER, cid) if cid != WALK_PLAYER else ''
    if cid == WALK_CPU:
        swap = '%d=%d,%d=%d' % (WALK_PLAYER, WALK_CPU, WALK_CPU, WALK_PLAYER)   # Lightning under test: the dummy becomes Squall
    env = {'PSPRECOMP_TEST_PLAYER_CHAR': str(cid), 'PSPRECOMP_TEST_PARITY': '1', 'PSPRECOMP_TEST_DIRECT_BATTLE': os.path.abspath(WALK),
           'PSPRECOMP_FIGHTER_LOG': '0', 'PSPRECOMP_TEST_NO_AI': '0', 'PSPRECOMP_TEST_AI_OFF': '1',   # brain runs, its presses blocked: with
                                                                                                    # the brain stopped (NO_AI) a CPU caught mid-dash at the start dashed for the whole run 'PSPRECOMP_TEST_FREEZE_CPU': '0',
           'LP_NUM_THREADS': os.environ.get('LP_NUM_THREADS', '2'), 'PSPRECOMP_TEST_NO_DRAW': '0' if (a.draw or bt.gpu_headless()) and not a.no_draw else '2',
           'PSPRECOMP_TEST_INPUT_DELAY': str(a.input_delay),
           'PSPRECOMP_CAM_FREE': '0'}   # the analogue camera runs on the wall clock (not repeatable under fast-forward) and turns the stick
    if swap:
        env['PSPRECOMP_TEST_CHAR_SWAP'] = swap
    with _start_lock:                                      # two games started in the same second pick the same X display
        xp, disp = bt.start_xvfb()
        g = bt.Game(os.path.abspath(a.work), rate, out, disp, bt.free_port(), script=script, extra_env=env)
        g.log = log
        g.start()
        time.sleep(3)
    t0 = time.time()
    try:
        bt.walk_direct(g, ff_menus=4)
        g.cmd('FF 120000')
        last_ff = time.time()
        acts, wrote, want, last_check = [], False, None, 0.0
        deadline = time.time() + a.timeout
        while g.alive():
            if time.time() > deadline:
                raise RuntimeError('timeout (%d s)' % a.timeout)
            if time.time() - last_ff > 50:
                try:
                    g.cmd('FF 120000')
                except OSError:
                    pass
                last_ff = time.time()
            if not wrote:
                acts, want = read_listing(log)                 # from the file: the harness's stream reader may have passed the lines already
                if want is not None and len(acts) >= want:
                    only = set(int(x) for x in a.actions.split(',')) if a.actions else None
                    if a.max_actions:
                        acts = acts[:a.max_actions]
                    if a.moves_only:
                        acts = []
                    with open(batch + '.tmp', 'w') as f:
                        f.write('\n'.join(trial_lines(acts, a.variants.split(','), only, moves=not a.no_moves)) + '\n')
                    os.replace(batch + '.tmp', batch)
                    wrote = True
            if wrote and time.time() - last_check > 5:      # a battle that ends after the trials began restarts the script: give up, the worker retries
                last_check = time.time()
                with open(log, errors='replace') as f:
                    txt = f.read()
                first = txt.find('mark T ')
                if first >= 0 and '[TEST] battle ended' in txt[first:]:
                    raise RuntimeError('the battle ended during the trials')
            try:
                size = os.path.getsize(log)
            except OSError:
                size = 0
            if size > a.max_log_mb * 1e6:
                raise RuntimeError('log over %d MB (runaway output)' % a.max_log_mb)
            time.sleep(1.0)
        if not wrote:
            raise RuntimeError('the game exited before listing the actions')
        with open(log, errors='replace') as f:
            txt = f.read()
        if 'mark DONE' not in txt:
            raise RuntimeError('the trials did not finish (no DONE mark)')
        if re.search(r'\[TEST\] battle ended.*', txt[txt.find('mark T '):] if 'mark T ' in txt else ''):
            raise RuntimeError('the battle ended during the trials')
        n_trials, n_stuck = txt.count('mark T '), len(re.findall(r'waitidle TIMEOUT', txt))
        if n_trials and n_stuck > 0.25 * n_trials:         # the fighters were rarely idle between trials: a stuck dummy / player, the run says nothing
            raise RuntimeError('%d of %d waits for idle fighters timed out' % (n_stuck, n_trials))
        return log, time.time() - t0
    finally:
        g.stop()
        xp.terminate()


def read_listing(log):
    """the fighter's actions from the last `action -1` listing in the log: ([(id, script, later, side, button)], record count or None)"""
    try:
        with open(log, errors='replace') as f:
            txt = f.read()
    except OSError:
        return [], None
    i = txt.rfind('[TEST] plya actor ')
    if i < 0:
        return [], None
    m = re.match(r'\[TEST\] plya actor \w+: (\d+) records', txt[i:])
    acts = [tuple(int(x) for x in m2.groups()) for m2 in
            re.finditer(r'\[TEST\] plya action (\d+) script (-?\d+) later (\d+) side (\d+) button (\d+)', txt[i:])]
    return acts, (int(m.group(1)) if m else None)


def worker(a, jobs, results, lock):
    while True:
        with lock:
            if not jobs:
                return
            cid, rate, tag = jobs.pop(0)
        out = os.path.join(a.out, slug(cid), 'r%s' % tag)
        msg = ''
        for attempt in range(2):                           # a busy box sometimes loses a boot ("the battle never started"): once more
            try:
                log, secs = run_one(a, cid, rate, out, tag)
                msg = 'ok %.0f s' % secs
                with lock:
                    results[(cid, tag)] = log
                break
            except Exception as e:                          # noqa: BLE001
                msg = 'FAILED: %s' % e
        print('[fps_parity] %-18s %4s fps: %s' % (CHARS[cid][0], tag, msg), file=sys.stderr, flush=True)


# ---- reading a log ----------------------------------------------------------------------------------------------------

class Trial:
    def __init__(self, aid, variant, t0):
        self.aid, self.variant, self.t0 = aid, variant, t0
        self.start = None          # gt of the action start
        self.result = None         # 'started' / 'NOT started' / ...
        self.end = None            # gt of the E mark
        self.idle = None           # gt the player was idle again (the next waitidle), or None on a timeout
        self.samples = []          # on the 30 Hz lattice: (gt, pscr, pcur, pmt, px, py, pz, dscr, dcur, dx, dy, dz)
        self.every = []            # every update (same tuples): paths are aligned by whole updates
        self.hits = []             # (gt, atkp, react, dmg, by)
        self.scr = []              # (gt, new script) of the player, every update


def keyed(trials):
    """{(action, variant, n-th occurrence): trial}"""
    out, seen = {}, {}
    for t in trials:
        k = (t.aid, t.variant)
        seen[k] = seen.get(k, 0) + 1
        out[(t.aid, t.variant, seen[k])] = t
    return out


def parse(path):
    trials, cur = [], None
    rx_par = re.compile(r'^\[PAR\] ([\d.]+)(~?) \| (.*?) \| (.*?) \| (\d+) (\d+)')
    pending_idle = None
    with open(path, errors='replace') as f:
        for l in f:
            if l.startswith('[PAR] '):
                if cur is None:
                    continue
                m = rx_par.match(l)
                if not m:
                    continue
                p, d = m.group(3).split(), m.group(4).split()
                if len(p) < 6 or len(d) < 6:
                    continue
                smp = (float(m.group(1)), int(p[0]), int(p[1]), float(p[2]), float(p[3]), float(p[4]), float(p[5]),
                       int(d[0]), int(d[1]), float(d[3]), float(d[4]), float(d[5]))
                cur.every.append(smp)
                if not m.group(2):                          # '~' marks an update between two 30 Hz frames
                    cur.samples.append(smp)
            elif l.startswith('[PARHIT] '):
                if cur is None:
                    continue
                m = re.match(r'\[PARHIT\] (\d+) (\w+) atkp=(-?\d+) react=(\d+) flags=\w+ dmg=(-?\d+) by=(\w+)', l)
                if m and m.group(2) == 'dummy':
                    cur.hits.append((int(m.group(1)), int(m.group(3)), int(m.group(4)), int(m.group(5)), m.group(6)))
            elif l.startswith('[TEST] gt='):
                m = re.match(r'\[TEST\] gt=(\d+) mark T (\d+) (\w+)', l)
                if m:
                    if cur is not None and cur.idle is None and pending_idle is not None:
                        cur.idle = pending_idle
                    cur = Trial(int(m.group(2)), m.group(3), int(m.group(1)))
                    if cur.aid == 0:                        # a movement trial: no action, the input starts 100 ms after the mark
                        cur.start, cur.result = cur.t0 + 100, 'started'
                    trials.append(cur)
                    pending_idle = None
                    continue
                m = re.match(r'\[TEST\] gt=(\d+) mark E', l)
                if m and cur is not None:
                    cur.end = int(m.group(1))
                    continue
                m = re.match(r'\[TEST\] gt=(\d+) mark DONE', l)
                if m and cur is not None:
                    continue
                m = re.match(r'\[TEST\] gt=(\d+) action (-?\d+) on \S+ \(\w+\): (.*)', l)
                if m and cur is not None and int(m.group(2)) == cur.aid:
                    cur.start, cur.result = int(m.group(1)), m.group(3).strip()
                    continue
                m = re.match(r'\[TEST\] gt=(\d+) waitidle (done|TIMEOUT)', l)
                if m and cur is not None and cur.end is not None and cur.idle is None:
                    cur.idle = int(m.group(1)) if m.group(2) == 'done' else -1
                    continue
                m = re.match(r'\[TEST\] gt=(\d+) scr (-?\d+) -> (-?\d+)', l)
                if m and cur is not None:
                    cur.scr.append((int(m.group(1)), int(m.group(3))))
            elif l.startswith('[TEST] battle ended') and cur is not None:
                cur.result = (cur.result or '') + ' (battle ended)'
    return trials


def segments(seq):
    """[(value, first index, last index)] of a list of values"""
    out = []
    for i, v in enumerate(seq):
        if out and out[-1][0] == v:
            out[-1][2] = i
        else:
            out.append([v, i, i])
    return out


def compare_trials(base, other, a, info=None):
    """reasons why `other` does not play like `base` (both Trial), [] = the same. `info` (a list) gets notes that are not failures."""
    why = []
    info = [] if info is None else info
    if base.result != other.result:
        return ['start: %s vs %s at 30 fps' % (other.result, base.result)]
    if base.result != 'started':
        return []
    tb, to = base.start, other.start
    sb = [s for s in base.samples if s[0] >= tb]
    so = [s for s in other.samples if s[0] >= to]
    n = min(len(sb), len(so))
    if n < 3:
        return ['too few samples (%d / %d)' % (len(sb), len(so))]
    # samples are on the 30 Hz lattice from the action start on: index k = the same game moment at both rates
    for label, col in (('player script', 1), ('dummy motion', 8)):
        a_seg = segments([s[col] for s in sb[:n]])
        o_seg = segments([s[col] for s in so[:n]])
        ids_a = [x[0] for x in a_seg]
        ids_o = [x[0] for x in o_seg]
        if ids_a != ids_o:
            # first difference
            i = next((i for i in range(min(len(ids_a), len(ids_o))) if ids_a[i] != ids_o[i]), min(len(ids_a), len(ids_o)))
            why.append('%s sequence differs at change %d: %s vs %s at 30 fps (%.0f ms into the move)' % (
                label, i, ids_o[i:i + 4], ids_a[i:i + 4], (a_seg[i][1] if i < len(a_seg) else n) * FRAME_MS))
        else:
            worst = max((abs(x[1] - y[1]) for x, y in zip(a_seg, o_seg)), default=0)
            if worst > a.frame_tol:
                k = next(i for i, (x, y) in enumerate(zip(a_seg, o_seg)) if abs(x[1] - y[1]) == worst)
                why.append('%s %d starts %+d frames off (%.0f ms vs %.0f ms into the move)' % (
                    label, a_seg[k][0], o_seg[k][1] - a_seg[k][1], o_seg[k][1] * FRAME_MS, a_seg[k][1] * FRAME_MS))
    # hits on the dummy
    hb = [h for h in base.hits if h[0] >= tb and (base.end is None or h[0] <= base.end + 2000)]
    ho = [h for h in other.hits if h[0] >= to and (other.end is None or h[0] <= other.end + 2000)]
    if len(hb) != len(ho):
        why.append('dummy took %d hits vs %d at 30 fps (attacks %s vs %s)' % (len(ho), len(hb), [h[1] for h in ho][:10], [h[1] for h in hb][:10]))
    else:
        for i, (x, y) in enumerate(zip(hb, ho)):
            if x[1] != y[1] or x[2] != y[2]:
                why.append('hit %d: attack %d reaction %d vs attack %d reaction %d at 30 fps' % (i + 1, y[1], y[2], x[1], x[2]))
                break
            dt = (y[0] - to) - (x[0] - tb)
            if abs(dt) > a.frame_tol * FRAME_MS + 1:
                why.append('hit %d (attack %d) lands %+.0f ms off (%.0f vs %.0f ms into the move)' % (i + 1, x[1], dt, y[0] - to, x[0] - tb))
                break
            if abs(x[3] - y[3]) > max(5, a.dmg_tol * abs(x[3])):   # HP damage = the attacker's bravery, which varies a little between runs
                why.append('hit %d (attack %d) damage %d vs %d at 30 fps' % (i + 1, x[1], y[3], x[3]))
                break
    # paths: judged at the best alignment within +-a.shift 30 Hz frames. A move starts at the end of the first update after it was asked
    # for: 33 ms after the press at 30 fps, 17 ms at 60, 8 ms at 120, so a high rate can lead by up to a frame (a phase, reported as info)
    for label, cols in (('player', (4, 5, 6)), ('dummy', (9, 10, 11))):
        travel = sum(math.sqrt(sum((sb[k][c] - sb[k - 1][c]) ** 2 for c in cols)) for k in range(1, n))
        ev = [x for x in other.every if x[0] >= to - FRAME_MS * a.shift - 1] or so
        times = [x[0] for x in ev]
        step = min((b - a_) for a_, b in zip(times, times[1:]) if b - a_ > 0.5) if len(times) > 2 else FRAME_MS
        m_ = int(round(a.shift * FRAME_MS / step))
        best = None
        for j in range(-m_, m_ + 1):
            sh_ms = j * step
            gap, at_k = 0.0, 0
            for k in range(n):
                t = so[0][0] + (sb[k][0] - sb[0][0]) + sh_ms
                i = bisect.bisect_left(times, t - step / 2)
                if i >= len(times) or abs(times[i] - t) > step / 2:
                    continue
                d = math.sqrt(sum((sb[k][c] - ev[i][c]) ** 2 for c in cols))
                if d > gap:
                    gap, at_k = d, k
            if best is None or gap < best[0] - 1e-6 or (abs(gap - best[0]) <= 1e-6 and abs(j) < abs(best[2])):
                best = (gap, at_k, j)
        gap, at_k, j = best
        sh = -1 if j < 0 else (1 if j > 0 else 0)
        shift_ms = j * step
        lim = max(a.pos_tol, a.pos_rel * travel)
        if gap > lim:
            why.append('%s path %.2f m off at %.0f ms into the move (moved %.2f m at 30 fps; limit %.2f m; best alignment %+.0f ms)' % (
                label, gap, at_k * FRAME_MS, travel, lim, shift_ms))
        elif sh and label == 'player':
            info.append('player %.0f ms %s' % (abs(shift_ms), 'ahead' if sh < 0 else 'behind'))
        # peak height (jumps, launches): a lower or higher arc is a difference even when the path stays within the tolerance
        hb, ho = max(s[cols[1]] for s in sb[:n]), max(s[cols[1]] for s in so[:n])
        if abs(hb - ho) > a.height_tol and max(hb, ho) - min(sb[0][cols[1]], so[0][cols[1]]) > 0.5:
            why.append('%s peak height %.2f m vs %.2f m at 30 fps' % (label, ho, hb))
    # length
    if base.idle and other.idle and base.idle > 0 and other.idle > 0:
        lb, lo = base.idle - tb, other.idle - to
        if abs(lb - lo) > a.frame_tol * FRAME_MS + 1:
            why.append('ends %+.0f ms off (player idle after %.0f ms vs %.0f ms at 30 fps)' % (lo - lb, lo, lb))
    elif (base.idle == -1) != (other.idle == -1):
        why.append('the player %s idle again within 8 s (at 30 fps: %s)' % ('was not' if other.idle == -1 else 'was', 'not' if base.idle == -1 else 'yes'))
    return why


def compare_char(a, cid, logs):
    """logs: {tag: path}. Writes <char>/report.txt, returns {rate: {'trials': n, 'fail': [...], 'nondet': [...]}}"""
    parsed = {t: parse(p) for t, p in logs.items() if p and os.path.exists(p)}
    res = {}
    lines = ['%s (charId %d)' % (CHARS[cid][0], cid)]
    if '30' not in parsed:
        lines.append('no 30 fps run: nothing to compare')
        res['error'] = 'no 30 fps run'
        _write(a, cid, lines)
        return res
    base = keyed(parsed['30'])
    nondet = set()
    if '30b' in parsed:
        for k, t in keyed(parsed['30b']).items():
            b = base.get(k)
            if b and compare_trials(b, t, a):
                nondet.add(k)
        lines.append('30 fps twice: %d of %d trials differ between two 30 fps runs (NONDETERMINISTIC, not counted as failures)' % (len(nondet), len(base)))
    started = sum(1 for t in base.values() if t.result == 'started')
    lines.append('%d trials at 30 fps (%d actions started)' % (len(base), started))
    for tag in sorted((t for t in parsed if t not in ('30', '30b')), key=int):
        fails, nd, n, ahead = [], [], 0, 0
        seen = keyed(parsed[tag])
        for key, b in base.items():
            o = seen.get(key)
            if o is None:
                fails.append((key, ['missing at this rate (run cut short?)']))
                continue
            n += 1
            inf = []
            why = compare_trials(b, o, a, inf)
            ahead += any('ahead' in x for x in inf)
            if why:
                (nd if key in nondet else fails).append((key, why))
        res[tag] = {'trials': n, 'fail': [[k[0], k[1], w] for k, w in fails], 'nondet': [[k[0], k[1], w] for k, w in nd]}
        lines.append('')
        lines.append('== %s fps vs 30 fps: %d trials, %d FAIL, %d nondeterministic; the player ran ahead (up to a frame: phase, not judged) in %d trials' % (tag, n, len(fails), len(nd), ahead))
        for (aid, v, occ), why in fails:
            b = base[(aid, v, occ)]
            scr = next((s[1] for s in b.samples if b.start and s[0] >= b.start), '?')
            lines.append(('  FAIL %s:' % v) if aid == 0 else ('  FAIL action %d (%s, script %s):' % (aid, v, scr)))
            for w in why:
                lines.append('      ' + w)
    _write(a, cid, lines)
    return res


def _write(a, cid, lines):
    d = os.path.join(a.out, slug(cid))
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'report.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')


def find_logs(a, cid):
    d = os.path.join(a.out, slug(cid))
    logs = {}
    if not os.path.isdir(d):
        return logs
    for sub in os.listdir(d):
        m = re.match(r'r(\d+b?)$', sub)
        if m:
            tag = m.group(1)
            p = os.path.join(d, sub, 'run_%d.log' % int(tag.rstrip('b')))
            if os.path.exists(p):
                logs[tag] = p
    return logs


def summarize(a, chars):
    allres, bad = {}, 0
    rows = []
    for cid in chars:
        r = compare_char(a, cid, find_logs(a, cid))
        allres[CHARS[cid][0]] = r
        if 'error' in r:
            rows.append('%-18s %s' % (CHARS[cid][0], r['error']))
            bad += 1
            continue
        cells = []
        for tag in sorted((t for t in r), key=int):
            nf = len(r[tag]['fail'])
            bad += nf
            cells.append('%s:%s' % (tag, 'ok' if nf == 0 else '%d FAIL' % nf))
        rows.append('%-18s %s' % (CHARS[cid][0], '  '.join(cells)))
    text = 'Frame-rate parity vs 30 fps (per rate: failing trials; details in <char>/report.txt)\n' + '\n'.join(rows) + '\n'
    with open(os.path.join(a.out, 'summary.txt'), 'w') as f:
        f.write(text)
    with open(os.path.join(a.out, 'results.json'), 'w') as f:
        json.dump(allres, f, indent=1)
    print(text)
    return 1 if bad else 0


def show(a):
    """one trial at every rate, side by side on the 30 Hz lattice"""
    cid = parse_chars(a.char)[0]
    logs = find_logs(a, cid)
    tr = {}
    for tag, p in logs.items():
        k = keyed(parse(p))
        t = k.get((a.action, a.variant, a.nth))
        if t:
            tr[tag] = t
    tags = sorted(tr, key=lambda x: (int(x.rstrip('b')), x))
    if not tags:
        print('no such trial')
        return 1
    for tag in tags:
        t = tr[tag]
        print('%4s fps: %s, hits %s' % (tag, t.result, [(h[0] - t.start, h[1], h[2], h[3]) for h in t.hits if t.start and h[0] >= t.start][:30]))
    print('%6s  ' % 'ms' + ' | '.join('%-46s' % ('%s fps: scr cur  x y z / dummy cur x z' % tg) for tg in tags))
    rows = {tg: [s for s in tr[tg].samples if tr[tg].start is not None and s[0] >= tr[tg].start] for tg in tags}
    n = max(len(r) for r in rows.values())
    for k in range(0, n, a.step):
        cells = []
        for tg in tags:
            r = rows[tg]
            if k < len(r):
                s = r[k]
                cells.append('%5d %4d %6.2f %5.2f %6.2f / %4d %6.2f %6.2f' % (s[1], s[2], s[4], s[5], s[6], s[8], s[9], s[11]))
            else:
                cells.append(' ' * 46)
        print('%6.0f  ' % (k * FRAME_MS) + ' | '.join(cells))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='what', required=True)
    p = sub.add_parser('run')
    p.add_argument('--work', required=True)
    p.add_argument('--out', default=None)
    p.add_argument('--chars', default='all')
    p.add_argument('--rates', default=','.join(map(str, RATES)))
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--actions', default='', help='only these action ids (default: every action the fighter has)')
    p.add_argument('--max-actions', dest='max_actions', type=int, default=0, help='only the first N actions (quick checks)')
    p.add_argument('--jecht-combos', dest='jecht_combos', action='store_true',
                   help='also run the Jecht combo window test (tools/combo_timing.py combos) after the parity games; off by default')
    p.add_argument('--combo-rates', dest='combo_rates', default='', help='rates for --jecht-combos (default: --rates, i.e. every rate 30..240)')
    p.add_argument('--moves-only', dest='moves_only', action='store_true', help='only the movement trials (jump, dodge, dash, block, run)')
    p.add_argument('--no-moves', dest='no_moves', action='store_true', help='leave out the movement trials (jump, dodge, dash, block, run)')
    p.add_argument('--variants', default='plain,mash')
    p.add_argument('--repeat-base', dest='repeat_base', action='store_true', help='a second 30 fps run: trials that differ between two 30 fps runs are NONDETERMINISTIC')
    p.add_argument('--timeout', type=int, default=3600, help='seconds per game')
    p.add_argument('--input-delay', dest='input_delay', type=float, default=0.0, help='experiments: scripted presses reach the game this many ms later above 30 fps')
    p.add_argument('--gpu', default='auto', choices=['auto', 'on', 'off'],
                   help='render on the NVIDIA GPU with no X server (battle_test BT_GPU; auto = when the NVIDIA EGL driver is installed); off = Xvfb + llvmpipe')
    p.add_argument('--draw', action='store_true', help='keep drawing during the trials with Xvfb too (on the GPU it is always kept: it costs nothing there)')
    p.add_argument('--no-draw', dest='no_draw', action='store_true', help='stop drawing once the fight runs (Xvfb + llvmpipe default: ~2.5x faster there)')
    p.add_argument('--max-log-mb', dest='max_log_mb', type=int, default=1500)
    q = sub.add_parser('compare')
    q.add_argument('out')
    q.add_argument('--chars', default='all')
    sub.add_parser('chars')
    w = sub.add_parser('show', help='one trial at every rate, side by side')
    w.add_argument('out')
    w.add_argument('char')
    w.add_argument('action', type=int)
    w.add_argument('variant', nargs='?', default='plain')
    w.add_argument('--nth', type=int, default=1)
    w.add_argument('--step', type=int, default=1, help='every n-th 30 Hz frame')
    for x in (p, q):
        x.add_argument('--frame-tol', dest='frame_tol', type=int, default=1, help='30 Hz frames a change / hit may be off (default 1)')
        x.add_argument('--pos-tol', dest='pos_tol', type=float, default=0.25, help='metres a path may be off (default 0.25)')
        x.add_argument('--pos-rel', dest='pos_rel', type=float, default=0.05, help='or this share of the distance covered (default 0.05)')
        x.add_argument('--shift', type=int, default=1, help='30 Hz frames a path may be shifted in time (phase) before it is compared (default 1)')
        x.add_argument('--height-tol', dest='height_tol', type=float, default=0.10, help='metres a peak height may differ (default 0.10)')
        x.add_argument('--dmg-tol', dest='dmg_tol', type=float, default=0.03, help='share an HP hit\'s damage may differ (default 0.03; two 30 fps runs differ by up to 2%%)')
    a = ap.parse_args(argv)
    if a.what == 'chars':
        for c, (n, code) in sorted(CHARS.items()):
            print('%2d  %-18s %s' % (c, n, code))
        return 0
    if a.what == 'show':
        return show(a)
    if a.what == 'compare':
        chars = [c for c in parse_chars(a.chars) if os.path.isdir(os.path.join(a.out, slug(c)))]
        return summarize(a, chars)
    os.environ['BT_GPU'] = {'auto': 'auto', 'on': '1', 'off': '0'}[a.gpu]
    a.out = os.path.abspath(a.out or tempfile.mkdtemp(prefix='fps_parity_'))
    os.makedirs(a.out, exist_ok=True)
    chars = parse_chars(a.chars)
    rates = [int(x) for x in a.rates.split(',')]
    if 30 not in rates:
        rates = [30] + rates
    jobs = []
    for cid in chars:                                      # character by character, all rates of one character together
        for r in rates:
            jobs.append((cid, r, str(r)))
        if a.repeat_base:
            jobs.append((cid, 30, '30b'))
    print('[fps_parity] %d characters x %d rates = %d games, %d at once, output %s' % (len(chars), len(rates), len(jobs), a.jobs, a.out), file=sys.stderr)
    results, lock = {}, threading.Lock()
    th = [threading.Thread(target=worker, args=(a, jobs, results, lock)) for _ in range(max(1, a.jobs))]
    for t in th:
        t.start()
    for t in th:
        t.join()
    rc = summarize(a, chars) if jobs is not None and chars else 0
    if a.jecht_combos:                                     # optional: Jecht's timed chain windows (tools/combo_timing.py), one game per rate
        import combo_timing
        crates = [int(x) for x in (a.combo_rates or a.rates).split(',')]
        crates = [30] + [r for r in crates if r != 30]
        print('[fps_parity] Jecht combo windows at %s fps' % ','.join(map(str, crates)), file=sys.stderr)
        crc = combo_timing.run_combos(os.path.abspath(a.work), crates, os.path.join(a.out, 'jecht_combos'), jobs=a.jobs)
        with open(os.path.join(a.out, 'summary.txt'), 'a') as f:
            f.write('\nJecht combo windows (jecht_combos/combos.txt): %s\n' % ('PASS' if crc == 0 else 'FAIL'))
        rc = rc or crc
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
