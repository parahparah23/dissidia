"""Triangle lists -> one triangle strip (GE PRIM 4) per draw.

The runtime decodes every index of a draw as its own vertex, so a triangle list costs 3 vertex decodes per triangle and a
strip about 1: for a model of tens of thousands of triangles that is most of its per-frame cost on a phone. Greedy strips
along shared edges, joined into one strip with degenerate triangles. Winding is kept: strip triangle k is
(s[k], s[k+1], s[k+2]) for even k and (s[k+1], s[k], s[k+2]) for odd k, as on the GE, and a triangle only extends a strip
when that order turns the same way as the list's triangle (the game culls back faces).
"""
from collections import defaultdict


def _same_turn(a, b):
    """same triangle, same winding (cyclic order)"""
    return a in (b, (b[1], b[2], b[0]), (b[2], b[0], b[1]))


def stripify(tris):
    """tris: [(a, b, c)] -> list of strips (each a list of vertex indices)"""
    tris = [tuple(t) for t in tris]
    edge_tris = defaultdict(list)
    used = [len(set(t)) < 3 for t in tris]          # degenerate triangles of the list: nothing to draw
    for t, (a, b, c) in enumerate(tris):
        if used[t]:
            continue
        for e in ((a, b), (b, c), (c, a)):
            edge_tris[frozenset(e)].append(t)

    def free_neighbours(t):
        a, b, c = tris[t]
        return sum(1 for e in ((a, b), (b, c), (c, a)) for u in edge_tris[frozenset(e)] if u != t and not used[u])

    strips = []
    for start in range(len(tris)):
        if used[start]:
            continue
        a, b, c = tris[start]
        # start on the rotation whose last edge has an unused neighbour (a longer strip)
        best = (a, b, c)
        for r in ((a, b, c), (b, c, a), (c, a, b)):
            if any(not used[u] and u != start for u in edge_tris[frozenset((r[1], r[2]))]):
                best = r
                break
        strip = list(best)
        used[start] = True
        while True:
            e = frozenset((strip[-2], strip[-1]))
            cands = [u for u in edge_tris[e] if not used[u]]
            if not cands:
                break
            nxt = min(cands, key=free_neighbours)      # the one with fewest ways out first (fewer stranded triangles)
            x = next(v for v in tris[nxt] if v not in e)
            k = len(strip) - 2
            cand = (strip[-2], strip[-1], x) if k % 2 == 0 else (strip[-1], strip[-2], x)
            if not _same_turn(cand, tris[nxt]):
                break
            strip.append(x)
            used[nxt] = True
        strips.append(strip)
    return strips


def join(strips):
    """one strip: each next strip starts at an even position (same winding), joined by degenerate triangles"""
    out = []
    for s in strips:
        if out:
            out.append(out[-1])
            out.append(s[0])
            if len(out) % 2 == 1:
                out.append(s[0])
        out.extend(s)
    return out


def strip_triangles(s):
    """the triangles a strip draws (degenerates left out), each as the list order would give it"""
    out = []
    for k in range(len(s) - 2):
        t = (s[k], s[k + 1], s[k + 2]) if k % 2 == 0 else (s[k + 1], s[k], s[k + 2])
        if len(set(t)) == 3:
            out.append(t)
    return out


def list_to_strip(idx):
    """triangle-list indices -> strip indices, or None when the strip would not be shorter"""
    tris = [tuple(idx[i:i + 3]) for i in range(0, len(idx) - 2, 3)]
    s = join(stripify(tris))
    return s if len(s) < len(idx) else None
