#!/usr/bin/env python3
"""Minimal VCDIFF (RFC 3284 / xdelta3) decoder that works WITHOUT the source file.

    vcdiff.py PATCH.xdelta [--json OUT.json]

A delta applied to a source file S gives a target T. Most of T is copied from S unchanged; what the patch
actually *adds* is spelled out in the delta itself (ADD / RUN instructions).  Without S we can therefore
recover every byte the patch wrote, and where, but not the bytes it left alone:

    decode(patch) -> Delta(target_size, ranges=[(target_offset, bytes), ...], moved=[...], notes=[...])

* `ranges`  literal bytes written by ADD/RUN (and by COPYs out of earlier literal bytes of the same window),
            merged when adjacent, sorted by target offset.  Positions the patch writes with bytes that happen
            to equal the source are included (a delta cannot tell), so a range may be a no-op.
* `moved`   COPY instructions out of the source at an address different from the target position
            (source_offset, target_offset, length).  A rebalance patch should not have any; if it does, the
            ranges alone do not describe the change and the caller must not trust them.
* identity COPYs (source address == target position) are the "unchanged" part and are not reported.

Supported: the default code table, SELF/HERE/NEAR/SAME address modes, VCD_SOURCE windows, the xdelta3
adler32 flag and application header. Not supported (raises ValueError): secondary compression, custom code
tables, VCD_TARGET windows, delta-compressed sections.  xdelta3 -S none output (what DeltaPatcher writes by
default) uses none of those.

`apply()` is the full decoder for when the source IS available (or partly available): it streams the target window by
window, decodes xdelta3's LZMA secondary compression (header indicator bit 1, compressor id 2: one xz stream per section
type that runs on across windows, flushed per window) and checks each window's adler32. The source is read through a
callback that also says which source bytes are unknown; unknown bytes travel through COPYs, so the caller learns exactly
which target bytes it could not reconstruct.

Gotcha worth remembering: the SAME address modes (6, 7, 8) take ONE byte from the address stream, not a
varint; SELF/HERE/NEAR take a varint.
"""
import json
import lzma
import struct
import sys
import zlib
from dataclasses import dataclass, field

RUN, ADD, COPY = 1, 2, 3        # instruction types (0 = NOOP)
NEAR_SIZE, SAME_SIZE = 4, 3


def default_code_table():
    """RFC 3284 section 5.6: 256 entries (type1, size1, mode1, type2, size2, mode2)."""
    t = [(RUN, 0, 0, 0, 0, 0)]
    t += [(ADD, s, 0, 0, 0, 0) for s in range(18)]                       # 1..18
    for mode in range(9):                                                # 19..162
        t += [(COPY, s, mode, 0, 0, 0) for s in [0] + list(range(4, 19))]
    for mode in range(6):                                                # 163..234
        for a in range(1, 5):
            for c in (4, 5, 6):
                t.append((ADD, a, 0, COPY, c, mode))
    for mode in range(6, 9):                                             # 235..246
        for a in range(1, 5):
            t.append((ADD, a, 0, COPY, 4, mode))
    t += [(COPY, 4, mode, ADD, 1, 0) for mode in range(9)]               # 247..255
    assert len(t) == 256
    return t


CODE_TABLE = default_code_table()


class _Reader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def byte(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def varint(self):
        v = 0
        while True:
            c = self.byte()
            v = (v << 7) | (c & 0x7F)
            if not c & 0x80:
                return v

    def take(self, n):
        v = self.b[self.p:self.p + n]
        if len(v) != n:
            raise ValueError('truncated delta')
        self.p += n
        return v

    def eof(self):
        return self.p >= len(self.b)


@dataclass
class Delta:
    target_size: int = 0
    ranges: list = field(default_factory=list)       # [(target_offset, bytes)]
    moved: list = field(default_factory=list)        # [(source_offset, target_offset, length)]
    notes: list = field(default_factory=list)
    windows: int = 0
    app_header: bytes = b''


class _Window:
    """The target window as a list of segments: ('lit', bytes) | ('src', source_offset, length)."""

    def __init__(self):
        self.segs, self.n = [], 0

    def add_lit(self, b):
        if b:
            self.segs.append(('lit', bytes(b)))
            self.n += len(b)

    def add_src(self, off, ln):
        if ln:
            self.segs.append(('src', off, ln))
            self.n += ln

    def slice(self, start, ln):
        out, pos = [], 0
        for s in self.segs:
            sl = len(s[1]) if s[0] == 'lit' else s[2]
            lo, hi = max(start, pos), min(start + ln, pos + sl)
            if lo < hi:
                out.append(('lit', s[1][lo - pos:hi - pos]) if s[0] == 'lit' else ('src', s[1] + lo - pos, hi - lo))
            pos += sl
        return out

    def copy_self(self, addr, ln):
        # overlapping copies repeat the bytes written so far, like memmove byte by byte
        while ln:
            chunk = min(ln, self.n - addr)
            if chunk <= 0:
                raise ValueError('COPY from beyond the written window')
            for s in self.slice(addr, chunk):
                if s[0] == 'lit':
                    self.add_lit(s[1])
                else:
                    self.add_src(s[1], s[2])
            addr += chunk
            ln -= chunk


def decode(patch: bytes) -> Delta:
    r = _Reader(patch)
    if r.take(3) != b'\xd6\xc3\xc4':
        raise ValueError('not a VCDIFF file')
    r.byte()                                            # version
    hind = r.byte()
    if hind & 1:
        raise ValueError('secondary compression is not supported')
    if hind & 2:
        raise ValueError('custom code table is not supported')
    d = Delta()
    if hind & 4:
        d.app_header = r.take(r.varint())
    tpos = 0
    while not r.eof():
        wind = r.byte()
        if wind & 2:
            raise ValueError('VCD_TARGET windows are not supported')
        src_len = src_pos = 0
        if wind & 1:
            src_len, src_pos = r.varint(), r.varint()
        dl = r.varint()
        body = _Reader(r.take(dl))
        tlen = body.varint()
        dind = body.byte()
        if dind:
            raise ValueError('delta-compressed sections are not supported')
        n_add, n_inst, n_addr = body.varint(), body.varint(), body.varint()
        if wind & 4:
            body.take(4)                                # adler32 of the target window
        add = _Reader(body.take(n_add))
        inst = _Reader(body.take(n_inst))
        addr = _Reader(body.take(n_addr))
        near, same, slot = [0] * NEAR_SIZE, [0] * (SAME_SIZE * 256), 0
        w = _Window()

        def do_copy(size, mode):
            nonlocal slot
            here = src_len + w.n
            if mode == 0:
                a = addr.varint()
            elif mode == 1:
                a = here - addr.varint()
            elif mode < 2 + NEAR_SIZE:
                a = near[mode - 2] + addr.varint()
            else:
                a = same[(mode - 2 - NEAR_SIZE) * 256 + addr.byte()]       # SAME: one byte
            near[slot] = a
            slot = (slot + 1) % NEAR_SIZE
            same[a % (SAME_SIZE * 256)] = a
            if a < src_len:
                if a + size > src_len:
                    raise ValueError('COPY crosses the end of the source segment')
                w.add_src(src_pos + a, size)
            else:
                w.copy_self(a - src_len, size)

        while not inst.eof():
            code = CODE_TABLE[inst.byte()]
            for k in (0, 3):
                typ, size, mode = code[k:k + 3]
                if not typ:
                    continue
                if size == 0:
                    size = inst.varint()
                if typ == ADD:
                    w.add_lit(add.take(size))
                elif typ == RUN:
                    w.add_lit(add.take(1) * size)
                else:
                    do_copy(size, mode)
        if w.n != tlen:
            raise ValueError('window decoded to %d bytes, header says %d' % (w.n, tlen))
        pos = tpos
        for s in w.segs:
            if s[0] == 'lit':
                d.ranges.append((pos, s[1]))
                pos += len(s[1])
            else:
                if s[1] != pos:
                    d.moved.append((s[1], pos, s[2]))
                pos += s[2]
        tpos += tlen
        d.windows += 1
    d.target_size = tpos
    # merge adjacent literal runs
    merged = []
    for off, b in d.ranges:
        if merged and merged[-1][0] + len(merged[-1][1]) == off:
            merged[-1] = (merged[-1][0], merged[-1][1] + b)
        else:
            merged.append((off, b))
    d.ranges = merged
    return d


class Unknown:
    """Sorted, merged list of [lo, hi) byte ranges."""

    def __init__(self, ranges=()):
        self.r = []
        for lo, hi in sorted(ranges):
            self.add(lo, hi)

    def add(self, lo, hi):
        if lo >= hi:
            return
        if not self.r or lo > self.r[-1][1]:          # the common case: in order
            self.r.append([lo, hi])
            return
        if lo >= self.r[-1][0]:
            self.r[-1][1] = max(self.r[-1][1], hi)
            return
        out, placed = [], False                       # out of order: insert and merge
        for a, b in self.r:
            if b < lo or a > hi:
                if a > hi and not placed:
                    out.append([lo, hi])
                    placed = True
                out.append([a, b])
            else:
                lo, hi = min(lo, a), max(hi, b)
        if not placed:
            out.append([lo, hi])
        self.r = out

    def __bool__(self):
        return bool(self.r)

    def overlaps(self, lo, hi):
        return [(max(a, lo), min(b, hi)) for a, b in self.r if a < hi and b > lo]

    def total(self):
        return sum(b - a for a, b in self.r)


def header(patch: bytes):
    """(application header bytes, offset of the first window, secondary compressor id or None)."""
    r = _Reader(patch)
    if r.take(3) != b'\xd6\xc3\xc4':
        raise ValueError('not a VCDIFF / xdelta3 file')
    r.byte()
    hind = r.byte()
    sec = r.byte() if hind & 1 else None
    if hind & 2:
        raise ValueError('custom code tables are not supported')
    app = r.take(r.varint()) if hind & 4 else b''
    return app, r.p, sec


@dataclass
class WindowResult:
    index: int
    target_offset: int
    data: bytes
    unknown: list          # [(lo, hi)] target-window-relative byte ranges that depend on unknown source bytes
    checksum_ok: object    # True / False / None (no checksum in the patch)
    source: tuple          # (source position, source length)


def apply(patch: bytes, read_source):
    """Yield one WindowResult per target window.

    read_source(pos, n) -> (bytes of length n, [(lo, hi), ...] absolute source ranges that are unknown).
    Unknown source bytes are best supplied as zeros; the target bytes copied from them are reported."""
    _, start, sec = header(patch)
    if sec not in (None, 2):
        raise ValueError('secondary compressor %d is not supported (only LZMA, id 2)' % sec)
    r = _Reader(patch)
    r.p = start
    dec = [lzma.LZMADecompressor(), lzma.LZMADecompressor(), lzma.LZMADecompressor()]
    tpos, wi = 0, 0
    while not r.eof():
        wind = r.byte()
        if wind & 2:
            raise ValueError('VCD_TARGET windows are not supported')
        src_len = src_pos = 0
        if wind & 1:
            src_len, src_pos = r.varint(), r.varint()
        body = _Reader(r.take(r.varint()))
        tlen = body.varint()
        dind = body.byte()
        sizes = body.varint(), body.varint(), body.varint()
        cksum = struct.unpack('>I', body.take(4))[0] if wind & 4 else None
        sect = []
        for k in range(3):
            b = body.take(sizes[k])
            if dind & (1 << k):
                if sec is None:
                    raise ValueError('compressed section without a secondary compressor')
                q = _Reader(b)
                n = q.varint()
                b = dec[k].decompress(b[q.p:])
                if len(b) != n:
                    raise ValueError('window %d: LZMA section %d gave %d bytes, header says %d' % (wi, k, len(b), n))
            sect.append(_Reader(b))
        add, inst, addr = sect
        S, s_unk = read_source(src_pos, src_len) if src_len else (b'', [])
        su = Unknown((lo - src_pos, hi - src_pos) for lo, hi in s_unk)
        T = bytearray()
        tu = Unknown()
        near, same, slot = [0] * NEAR_SIZE, [0] * (SAME_SIZE * 256), 0
        while not inst.eof():
            code = CODE_TABLE[inst.byte()]
            for k in (0, 3):
                typ, size, mode = code[k:k + 3]
                if not typ:
                    continue
                if size == 0:
                    size = inst.varint()
                if typ == ADD:
                    T += add.take(size)
                    continue
                if typ == RUN:
                    T += add.take(1) * size
                    continue
                here = src_len + len(T)
                if mode == 0:
                    a = addr.varint()
                elif mode == 1:
                    a = here - addr.varint()
                elif mode < 2 + NEAR_SIZE:
                    a = near[mode - 2] + addr.varint()
                else:
                    a = same[(mode - 2 - NEAR_SIZE) * 256 + addr.byte()]
                near[slot] = a
                slot = (slot + 1) % NEAR_SIZE
                same[a % (SAME_SIZE * 256)] = a
                at = len(T)
                if a < src_len:
                    if a + size > src_len:
                        raise ValueError('COPY crosses the end of the source segment')
                    T += S[a:a + size]
                    for lo, hi in su.overlaps(a, a + size):
                        tu.add(at + lo - a, at + hi - a)
                else:
                    a -= src_len
                    unk = tu.overlaps(a, a + size) if tu else []
                    if a + size <= at:
                        T += T[a:a + size]
                    else:                       # overlapping: repeats the bytes written so far
                        left = size
                        while left:
                            chunk = min(left, len(T) - a)
                            T += T[a:a + chunk]
                            a += chunk
                            left -= chunk
                    if unk:                     # conservative for overlapping copies: the whole run
                        tu.add(at, at + size)
        if len(T) != tlen:
            raise ValueError('window %d decoded to %d bytes, header says %d' % (wi, len(T), tlen))
        ok = None if cksum is None else (zlib.adler32(bytes(T)) & 0xFFFFFFFF) == cksum
        yield WindowResult(wi, tpos, bytes(T), [tuple(x) for x in tu.r], ok, (src_pos, src_len))
        tpos += tlen
        wi += 1


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    d = decode(open(argv[0], 'rb').read())
    print('%d window(s), target %d B, %d literal ranges (%d B), %d relocated copies'
          % (d.windows, d.target_size, len(d.ranges), sum(len(b) for _, b in d.ranges), len(d.moved)))
    if '--json' in argv:
        out = argv[argv.index('--json') + 1]
        json.dump({'target_size': d.target_size, 'ranges': [[o, b.hex()] for o, b in d.ranges],
                   'moved': d.moved}, open(out, 'w'), indent=0)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
