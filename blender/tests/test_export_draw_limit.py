"""The exporter keeps every draw within the GE's 16-bit vertex count (a draw over 65,535 indices wraps and loses most of
its triangles). Run:  blender -b -P tests/test_export_draw_limit.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
for k in [k for k in sys.modules if k == 'io_scene_dissidia' or k.startswith('io_scene_dissidia.')]:
    del sys.modules[k]                     # an installed copy of the add-on Blender loaded at start: test this checkout's
from io_scene_dissidia import exporter as E
assert os.path.dirname(os.path.dirname(os.path.abspath(E.__file__))) == os.path.abspath(os.path.join(HERE, '..')), E.__file__


def check(tri_bones, label):
    batches = E._batch_triangles(tri_bones)
    tris = sorted(t for b in batches for t in b['tris'])
    assert tris == list(range(len(tri_bones))), label + ': triangles lost or repeated'
    assert all(len(b['tris']) * 3 <= 65535 for b in batches), label + ': a draw over 65,535 indices'
    assert all(len(b['bones']) <= E.MAX_SUBSET for b in batches), label + ': a draw over the bone limit'
    return batches


# a head of 70,000 triangles all on one bone: one draw before, now split
b = check([{5}] * 70000, 'one bone')
assert len(b) == 4, len(b)
# many bones, more than one draw's worth of triangles each
b = check([{i % 12, (i + 1) % 12} for i in range(100000)], 'many bones')
# small models keep one draw per bone set (no change for retail-sized bodies)
b = check([{1, 2}] * 3000 + [{3}] * 500, 'small')
assert len(b) == 1, len(b)
print('test_export_draw_limit: OK')
