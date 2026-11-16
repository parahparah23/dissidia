#!/usr/bin/env python3
"""Dissidia (2008, the first game, "D1") character moveset -> the Dissidia 012 layout.

D1 keeps its movesets in the same ARC container (obj/p_<code>.objx) with the same section tags, but four sections
have older record layouts. This rewrites those four so that charmod_objx.unpack (and the Move Studio's copy-a-move)
read the result like any 012 moveset:

  plya (actions)   header 20 -> 22, records 112 -> 116 bytes. 012 moved D1's kind byte (+0x15: 0x20 brave,
                   0x40/0x42 HP, 0x80 EX) to +0x0F (1 brave, 2 HP, 0x44 EX), widened +0x10 to two halfwords
                   (0xFFFF, 0xFFFF) and inserted 4 zero bytes at +0x14; D1 +0x14.. is 012 +0x18.. (script id at
                   +0x1C, HP banner byte at +0x21).
  comb (commands)  D1: u32 version 4, u32 count, fixed 72-byte records {u16 id, u16 0, u16 prereq[2], 8 steps of
                   {u16 action, b, c, flags}} with action 0xFFFF = unused step. 012: version 6, u16 count, u32
                   offsets, records {u16 id, u16 steps, u16 prereq[2], steps}.
  atkp (hits)      header 9 -> 12, records 60 -> 68 bytes: D1 byte +6 = the 012 flags word (+0x08; +6/+7 become
                   0xFF/0x00), and 012 added the hit-stop / shake bytes (+0x14) that D1 does not have. Those are
                   filled the way 012 fills the same kind of hit (HP hits 0a 14 4b 4b, launchers / finishers
                   00 0a 0f 0f, others 0). kb_bone (+0x2C) is 0xFFFF for the stagger / knock reactions, as in 012.
  magi (projectiles) origin / target modes renumbered (ORIGIN_MAP, TARGET_MAP). D1 stores each of the 46 parameters as three floats (lo, hi, step) in a 616-byte record;
                   012 stores byte indices into a float pool (204-byte records). The first 0x40 bytes are the same.

Everything else (timelines, motion map, hit volumes, effects, sounds, text) has the same layout and is copied.
Also: effe (the effect package: FEPA placements 248 -> 260 bytes) and timelines (six event ops whose value count grew).
Only verified for Bartz (fiv100); other characters should work the same way but are not checked.

    d1_objx.py <D1 p_<code>.objx> <out 012-layout objx> [--only-projectiles id,id,...]
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import charmod_objx as co  # noqa: E402
import charmod_magi as magi  # noqa: E402

D1_PLYA, D1_ATKP, D1_MAGI = 112, 60, 616
STAGGER_REACTIONS = {2, 11, 12, 13, 14}


def plya(d):
    hdr, n = struct.unpack_from('<II', d, 0)
    if hdr != 20 or len(d) != 8 + n * D1_PLYA:
        raise SystemExit('plya: not a D1 action table (header %d, %d bytes for %d records)' % (hdr, len(d), n))
    out = bytearray(struct.pack('<II', 22, n))
    for k in range(n):
        r = d[8 + k * D1_PLYA: 8 + (k + 1) * D1_PLYA]
        kind = r[0x15]
        nr = bytearray(116)
        nr[0:0x0F] = r[0:0x0F]
        nr[0x0F] = 0x44 if kind & 0x80 else 2 if kind & 0x40 else 1 if kind & 0x20 else 0
        nr[0x10:0x14] = b'\xff\xff\xff\xff'
        nr[0x18:] = r[0x14:]
        nr[0x19] = 0
        out += nr
    return bytes(out)


def comb(d):
    ver, n = struct.unpack_from('<II', d, 0)
    if ver != 4 or len(d) != 8 + n * 72:
        raise SystemExit('comb: not a D1 command table (version %d, %d bytes for %d records)' % (ver, len(d), n))
    cmds = []
    for k in range(n):
        o = 8 + k * 72
        cid, _, pa, pb = struct.unpack_from('<4H', d, o)
        steps = []
        for s in range(8):
            a, b, c, f = struct.unpack_from('<4H', d, o + 8 + 8 * s)
            if a == 0xFFFF:
                break
            steps.append({'action': a, 'b': b, 'c': c, 'flags': f})
        cmds.append({'id': '0x%02X' % cid, 'prereq': [pa, pb], 'steps': steps})
    return co.comb_build({'version': 6, 'commands': cmds})


def shake_for(flags, reaction):
    if flags & 0x02:                        # HP attack
        return bytes.fromhex('0a144b4b')
    if reaction not in (0, 1, 13):          # launches, blows, knocks: the combo finishers
        return bytes.fromhex('000a0f0f')
    return bytes(4)


def atkp(d):
    hdr, n = struct.unpack_from('<II', d, 0)
    if hdr != 9 or len(d) != 8 + n * D1_ATKP:
        raise SystemExit('atkp: not a D1 hit table (header %d, %d bytes for %d records)' % (hdr, len(d), n))
    out = bytearray(struct.pack('<II', 12, n))
    for k in range(n):
        r = d[8 + k * D1_ATKP: 8 + (k + 1) * D1_ATKP]
        nr = bytearray(68)
        nr[0:6] = r[0:6]
        nr[6], nr[7] = 0xFF, 0
        struct.pack_into('<I', nr, 0x08, r[6])
        nr[0x0C:0x14] = r[8:16]
        nr[0x14:0x18] = shake_for(r[6], r[5])
        nr[0x18:0x44] = r[16:60]
        if struct.unpack_from('<H', nr, 0x2C)[0] == 0 and r[5] in STAGGER_REACTIONS:
            struct.pack_into('<H', nr, 0x2C, 0xFFFF)
        out += nr
    return bytes(out)


# 012 added spawn-origin and target modes, so D1's numbers moved. Learnt from the 790 projectile records both games share
# (same id and hit, every character): D1 origin 5 (at an effect placement, arg = its id; Paladin Force's light) is 012's
# 6, 4 (the emitter) is 5, 2 / 3 are 11 / 12, 7 is 8, 8 is 12; target 2 is 3. Direction, motion, kind and flags are unchanged.
ORIGIN_MAP = {0: 0, 1: 1, 2: 11, 3: 12, 4: 5, 5: 6, 6: 7, 7: 8, 8: 12}
TARGET_MAP = {0: 0, 1: 1, 2: 3}


def magi_conv(d, only=None):
    tag, ver, n, nf = struct.unpack_from('<4sIII', d, 0)
    if tag != b'MAGI' or nf != 0 or len(d) != 16 + n * D1_MAGI:
        raise SystemExit('magi: not a D1 projectile table')
    m = magi.Magi(version=magi.VERSION)
    for k in range(n):
        r = d[16 + k * D1_MAGI: 16 + (k + 1) * D1_MAGI]
        rid = struct.unpack_from('<H', r, 0)[0]
        if only is not None and rid not in only:
            continue
        rec = magi.Record(bytes(r[:0x40]) + bytes(magi.REC_SIZE - 0x40))
        rec.origin = ORIGIN_MAP.get(rec.origin, rec.origin)
        rec.target = TARGET_MAP.get(rec.target, rec.target)
        vals = struct.unpack_from('<138f', r, 0x40)
        for p in range(magi.NPARAM):
            lo, hi, st = vals[3 * p: 3 * p + 3]
            rec.set_trip(p, m.pool_index(lo), m.pool_index(hi), m.pool_index(st))
        m.records.append(rec)
    m.records.sort(key=lambda r: r.id)
    return m.to_bytes()


def magi_needed(d):
    """Unique floats the whole D1 table needs (the 012 pool holds 128)."""
    n = struct.unpack_from('<I', d, 8)[0]
    vals = set()
    for k in range(n):
        vals.update(struct.pack('<f', v) for v in struct.unpack_from('<138f', d, 16 + k * D1_MAGI + 0x40))
    return len(vals)


def _tail_homing(a):
    # D1 0x0C10 (12 values) -> 012 homing_dash 0x0D10 (13): the 8 named values, then 012 always carries
    # (x, x, 1, 0, 0) where D1 had a flag in one of its last two values (Bartz's shared air moves, both games)
    return a[:10] + [1 if (a[10] or a[11]) else 0, 0, 0]


# D1 event op -> (012 op, args from D1 args). The op's high byte is its value count; 012 grew some events.
# Learnt from the moves both games share (same motion, events side by side).
OP_MAP = {
    0x0115: (0x0315, lambda a: a + [0, 0]),          # voice: + only_if_alt_off, only_if_alt_on
    0x0C10: (0x0D10, _tail_homing),                  # homing_dash
    0x0825: (0x0925, lambda a: a + [0]),             # ground_homing_dash
    0x0211: (0x0811, lambda a: a + [0] * 6),         # turn_or_swivel (deg_per_tick, away): no swivel
    0x0443: (0x0743, lambda a: a + [0] * 3),         # hit_window_fire (hit, group, projectile)
    0x0418: (0x0518, lambda a: a + [0]),             # steer_during_move
}


def timelines(d, log=None):
    ver, n = struct.unpack_from('<II', d, 0)
    offs = list(struct.unpack_from(f'<{n}I', d, 8)) + [len(d)]
    blobs, unknown = [], {}
    for k in range(n):
        b = d[offs[k]:offs[k + 1]]
        t = co.ev_decode(b)
        if t is None:
            blobs.append(b)
            continue
        for e in t['ranged'] + t['keyed']:
            op = int(e['op'], 16)
            if op in OP_MAP:
                nop, f = OP_MAP[op]
                e['op'] = '0x%04X' % nop
                e['args'] = f(list(e['args']))
            elif op not in co.NAMED and op not in co.OPS:
                unknown[op] = unknown.get(op, 0) + 1
        blobs.append(co.ev_encode(t))
    if unknown and log:
        log('timelines: D1 events with no 012 meaning kept as they are: %s' % ', '.join(
            '0x%04X x%d' % kv for kv in sorted(unknown.items())))
    o = 8 + 4 * n
    out_offs = []
    for b in blobs:
        out_offs.append(o)
        o += len(b)
    return struct.pack('<II', ver, n) + struct.pack(f'<{n}I', *out_offs) + b''.join(blobs)


D1_FEPA_VER, FEPA_VER, D1_FEPA, FEPA = 0x20080711, 0x20100205, 248, 260


def effe(d):
    """The effect package (MPK: 0x40 FEP effects, 0x4D FEPA placements, 0x5A FXSE). Only FEPA changed: 248 -> 260-byte
    records, 4 zero bytes inserted at +0x68 (D1 +0x54..+0x6B is all zero, 012 +0x54..+0x6F likewise; D1 +0x6C = 012 +0x70,
    the motion / bone / name strings follow) and 8 zero bytes at the end."""
    if d[:4] != b'MPK ':
        return d
    n = struct.unpack_from('<I', d, 8)[0]
    ents = [list(struct.unpack_from('<4I', d, 16 + 16 * i)) for i in range(n)]
    blobs = [d[o:o + s] for _, o, s, _ in ents]
    for i, (t, _, _, _) in enumerate(ents):
        b = blobs[i]
        if t != 0x4D or b[:4] != b'FEPA':
            continue
        ver, cnt = struct.unpack_from('<II', b, 4)[0], struct.unpack_from('<I', b, 12)[0]
        if ver == FEPA_VER:
            continue
        if ver != D1_FEPA_VER or len(b) != 16 + cnt * D1_FEPA:
            raise SystemExit('effe: FEPA version %08X / size %d not known' % (ver, len(b)))
        out = bytearray(b[:16])
        struct.pack_into('<II', out, 4, FEPA_VER, 16 + cnt * FEPA)
        for k in range(cnt):
            r = b[16 + k * D1_FEPA: 16 + (k + 1) * D1_FEPA]
            out += r[:0x68] + bytes(4) + r[0x68:] + bytes(8)
        blobs[i] = bytes(out)
    # rebuild: same entry order, data at the old alignment (16)
    pos = 16 + 16 * n
    pos = (pos + 15) & ~15
    table, body = bytearray(), bytearray()
    first = min(o for _, o, _, _ in ents)
    pos = first
    for (t, _, _, x), b in zip(ents, blobs):
        pos = (pos + 15) & ~15
        table += struct.pack('<4I', t, pos, len(b), x)
        body += bytes(pos - first - len(body)) + b
        pos += len(b)
    hdr = bytearray(d[:16 + 16 * n])
    hdr[16:16 + 16 * n] = table
    return bytes(hdr) + bytes(first - len(hdr)) + bytes(body) + bytes((-(first + len(body))) % 16)


def convert(objx, only_proj=None, log=print):
    extra, ents = co.arc_read(objx)
    out = []
    for t, tag, data in ents:
        key = tag.rstrip(b'\0')
        if t == 8:
            data = timelines(data, log)
        elif t == 4 and key == b'effe':
            data = effe(data)
        elif t == 1 and key == b'plya':
            data = plya(data)
        elif t == 1 and key == b'comb':
            data = comb(data)
        elif t == 1 and key == b'atkp':
            data = atkp(data)
        elif t == 1 and key == b'magi':
            data = magi_conv(data, only_proj)
        out.append((t, tag, data))
    return co.arc_write(extra, out)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('d1_objx')
    ap.add_argument('out')
    ap.add_argument('--only-projectiles', help='keep only these projectile ids (hex or dec, comma separated)')
    a = ap.parse_args(argv)
    src = open(a.d1_objx, 'rb').read()
    only = {int(x, 0) for x in a.only_projectiles.split(',')} if a.only_projectiles else None
    data = convert(src, only)
    with open(a.out, 'wb') as f:
        f.write(data)
    print('%s: %d -> %d bytes' % (a.out, len(src), len(data)))


if __name__ == '__main__':
    main(sys.argv[1:])
