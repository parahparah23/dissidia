#!/usr/bin/env python3
"""Tag team accessory audit (host, headless): every fighter must fight with ITS OWN member's accessories.

Runs the party walk (tests/replays/party_battle_walk.txt, 1 v 2, the player's party has members with different accessory sets) with the
tag team on, a partner (SP_PARTNER) and KOs that rebuild the player's slot (KOLIST human:12,partner:25,human:40) and reads the
`[ACC]` lines PSPRECOMP_TAGTEAM_ACCLOG=1 prints (docs/tag-team.md "Accessories per fighter"): for each fighter after its init, the accessory ids of
the record copy the fighter keeps (actor +0x30C), the side record's and whether the actor's equals the character's own save record's (`own-save=SAME`).

  tagteam_acc_test.py --work DIR [--out DIR]

Checks: every human-side fighter (partner, rebuilt slot) is own-save=SAME; at least two different accessory sets were seen among the
human-side fighters and a rebuilt one differs from the side record's (the first member's); no actor record is 0 after its init.
Exit status 0 = pass. Needs Xvfb (battle_test.py)."""
import argparse, os, re, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import battle_test as bt  # noqa: E402

def run(work, out, seconds):
    os.makedirs(out, exist_ok=True)
    script = os.path.join(out, 'script.txt')
    open(script, 'w').write('%d quit\n' % (seconds * 1000))
    os.environ['BT_NO_FREEZE'] = '1'
    env = {'PSPRECOMP_CLOUD': '0', 'PSPRECOMP_TAGTEAM_BREAKSHOT': '0', 'PSPRECOMP_TAGTEAM': '1', 'PSPRECOMP_TAGTEAM_LOG': '1', 'PSPRECOMP_FIGHTER_LOG': '1',
           'PSPRECOMP_TAGTEAM_ACCLOG': '1', 'PSPRECOMP_TAGTEAM_SP_PARTNER': '1', 'PSPRECOMP_TAGTEAM_KOLIST': 'human:12,partner:25,human:40', 'PSPRECOMP_TAGTEAM_KOLIST_NAT': '1'}
    a = argparse.Namespace(work=work, script=script, fps=30, out=out, char='Squall', timeout=400, ff=True, ff_menus=4,
                           direct=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'replays', 'party_battle_walk.txt'),
                           env=env, stage=12, record=None, watch=None, shots=None)
    bt.run(a)
    return os.path.join(out, 'run_30.log')

def parse(log):
    rows = []
    for l in open(log, errors='replace'):
        m = re.match(r'\[ACC\] (.*?) actor (\w+) side (\d+): own-save=(\w+) actor rec (\w+)(?: char (\d+) L(\d+) set (\d+) nacc (-?\d+) acc\[([^\]]*)\])?.*?\| side-record rec \w+ char (\d+) .*?acc\[([^\]]*)\]', l)
        if m:
            rows.append(dict(tag=m[1], actor=m[2], side=int(m[3]), own=m[4], char=int(m[6] or -1), nacc=m[9], acc=m[10].split() if m[10] else [], schar=int(m[11]), sacc=m[12].split()))
    return rows

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', required=True); ap.add_argument('--out', default=None); ap.add_argument('--seconds', type=int, default=90)
    ap.add_argument('--log', default=None, help='parse an existing run_30.log instead of running')
    a = ap.parse_args()
    log = a.log or run(os.path.abspath(a.work), os.path.abspath(a.out or tempfile.mkdtemp(prefix='tagteam_acc_')), a.seconds)
    rows = parse(log); ok = True
    def check(c, m):
        nonlocal ok; print(('PASS  ' if c else 'FAIL  ') + m); ok = ok and c
    final = [r for r in rows if 'after init' in r['tag'] and r['side'] == 0]
    for r in rows:
        print('%-26s side %d char %2d own-save=%-6s acc %s | side record char %d acc %s' % (r['tag'], r['side'], r['char'], r['own'], ' '.join(r['acc']), r['schar'], ' '.join(r['sacc'])))
    check(len(final) >= 3, '%d human-side fighters were made after the start (partner + rebuilds), need >= 3' % len(final))
    check(all(r['char'] > 0 for r in final), 'every human-side fighter has its own record copy after its init')
    check(all(r['own'] == 'SAME' for r in final), 'human-side fighters carry their own save record accessories (own-save=SAME)')
    check(any(r['acc'] != r['sacc'] for r in final), 'a rebuilt / partner fighter differs from the side record (the first member): per-member sets')
    check(len({tuple(r['acc']) for r in final}) >= 2, 'different members have different accessory sets')
    print('RESULT: ' + ('PASS' if ok else 'FAIL') + '  (log %s)' % log)
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
