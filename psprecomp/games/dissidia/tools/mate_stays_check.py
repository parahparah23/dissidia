#!/usr/bin/env python3
"""Host check: when a slot fighter falls and its living team mate takes over the slot, the mate must not be relocated.
Parses the [MATE] lines of a PSPRECOMP_TAGTEAM_LOG=1 run (mate position before, new body position after) and fails when they differ by more than 0.05 m
or when a [TELEPORT] line moves a mate other than the parking of its old body.
Run a battle with CPU KOs of the enemy slot (e.g. PSPRECOMP_TAGTEAM_KOLIST=cpu:9,cpu:17 and a reserve), then: mate_stays_check.py <run log>"""
import re, sys
num = r'\(?(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)?'
bad = n = 0
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'\[MATE\] .*mate at ' + num + r' foe .* -> new body at ' + num, l)
    if m:
        n += 1
        a = [float(x) for x in m.groups()]
        d = max(abs(a[i] - a[i + 3]) for i in range(3))
        if d > 0.05: bad += 1; print('FAIL mate moved %.2f m: %s' % (d, l.strip()[:200]))
print('%s  %d mate takeovers, %d moved' % ('PASS' if n and not bad else 'FAIL', n, bad))
sys.exit(0 if n and not bad else 1)
