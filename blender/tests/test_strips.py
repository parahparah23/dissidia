"""Self-test of dfx.strips (triangle list -> one strip): no Blender needed.  python3 tests/test_strips.py"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'io_scene_dissidia'))
from dfx import strips as S  # noqa: E402


def canon(t):
    """a triangle as a winding-aware key: its rotation starting at the smallest index"""
    i = t.index(min(t))
    return t[i:] + t[:i]


def check(tris, label, max_ratio=None):
    idx = [v for t in tris for v in t]
    s = S.join(S.stripify(tris))
    want = sorted(canon(tuple(t)) for t in tris if len(set(t)) == 3)
    got = sorted(canon(t) for t in S.strip_triangles(s))
    assert got == want, '%s: the strip draws other triangles (%d vs %d)' % (label, len(got), len(want))
    ratio = len(s) / max(1, len(idx))
    if max_ratio is not None:
        assert ratio <= max_ratio, '%s: strip %d indices for a %d-index list (%.2f)' % (label, len(s), len(idx), ratio)
    return ratio


# a 60 x 40 grid of quads, two triangles each, counter-clockwise
W, H = 60, 40
grid = []
for y in range(H):
    for x in range(W):
        a, b, c, d = y * (W + 1) + x, y * (W + 1) + x + 1, (y + 1) * (W + 1) + x + 1, (y + 1) * (W + 1) + x
        grid += [(a, b, c), (a, c, d)]
r = check(grid, 'grid', max_ratio=0.6)
print('grid: %d triangles, strip / list = %.2f' % (len(grid), r))

# the same grid in random order, some triangles with the other winding (a double-sided card), plus degenerate ones
random.seed(1)
soup = grid[:]
random.shuffle(soup)
soup += [(t[0], t[2], t[1]) for t in random.sample(grid, 200)]
soup += [(5, 5, 6), (7, 8, 7)]
r = check(soup, 'shuffled + reversed + degenerate', max_ratio=0.75)
print('soup: %d triangles, strip / list = %.2f' % (len(soup), r))

# disconnected single triangles: no gain, list_to_strip keeps the list
lone = [(3 * i, 3 * i + 1, 3 * i + 2) for i in range(50)]
check(lone, 'lone triangles')
assert S.list_to_strip([v for t in lone for v in t]) is None
print('test_strips: OK')
