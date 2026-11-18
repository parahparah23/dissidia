#!/usr/bin/env python3
"""Install a community xdelta / VCDIFF patch (made against a retail Dissidia 012 ISO) as a mod folder.

    xdelta_mod.py import PATCH.xdelta --work WORKDIR [--mods DIR] [--name NAME] [--iso GAME.iso] [--force]
    xdelta_mod.py build-code --work WORKDIR [--mods DIR] [--out DIR]
    xdelta_mod.py list [--mods DIR]
    xdelta_mod.py layout GAME.iso [GAME2.iso ...]          # maintainers: refresh disc_layouts.json

A patch of this kind rewrites a whole ISO. `import` applies it in memory and keeps only what changed, as a normal
installed mod folder (docs/loose-file-mods.md, "Installed mod folders"), so Settings > Mods turns it on and off:

    <mods>/<name>/mod.json             {"name": ..., "kind": "xdelta", "enabled": true, "code_stamp": ...}
    <mods>/<name>/install/<asset>      every PACKAGE.BIN entry the patch changed or added (by path, or by_crc/<crc>.bin)
    <mods>/<name>/usrdir/...           other changed files under PSP_GAME/USRDIR (BGM, movies ...), upper case
    <mods>/<name>/package_tail.bin     data the patch appends to PACKAGE.BIN past its last entry (read by offset)
    <mods>/<name>/code/                the patched EBOOT / overlays, when the patch changes game code

The base the patch needs is rebuilt from the install folder (WORKDIR: disc0/ + EBOOT.enc.bin) and a table of the two
retail discs' layouts (disc_layouts.json: file positions, sizes, MD5s and the ~40 KB of ISO 9660 directory sectors;
no game data). The EU and US discs hold the same game files, so a patch made for either disc works from both. With
--iso the disc image itself is read instead. Every patch window carries an adler32; the result is checked against it
and against which bytes could not be rebuilt.

Code. A patch that changes the EBOOT or an overlay changes compiled code. `build-code` (run by pipeline.sh /
psp_build.py before the C++ build, and cheap when there is nothing to do) recompiles each such module from the patched
bytes, keeps only the functions whose translation differs from the retail build (plus the new ones) and writes them,
renamed, into <work>/overlays/xmods with a table the runtime reads (games/dissidia/runtime/xdelta_mods.cpp). While the
mod is on, those functions replace the retail ones in the dispatch tables, the EBOOT's changed data bytes are written
at boot and the overlay files are read from code/. Turned off, the game runs the retail code. A mod whose code is not
in the build (imported after the last build) stays off until the next build.

Python 3 standard library only.
"""
import argparse
import base64
import bisect
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vcdiff  # noqa: E402

SECTOR = 2048
LAYOUTS = os.path.join(HERE, "disc_layouts.json")
PATHS = os.path.join(HERE, "..", "package_paths.txt")
EBOOT_HDR = 0x150                     # plaintext ~PSP header of the encrypted EBOOT.BIN
OVL_BASE = 0x089DD4C0
OVL_FILE_HDR = 0x54
# the modules whose code is recompiled: patch file name -> (module id, analysis, toml, retail set, symbol prefix)
CODE = {
    "EBOOT.BIN": (0, "analysis.json", "game.toml", "output", ""),
    "OVL_BATTLE_APP.ELF": (1, "analysis_battle.json", "overlay_battle.toml", "overlays/battle", "ovl_battle_"),
    "OVL_MENU_APP.ELF": (2, "analysis_menu.json", "overlay_menu.toml", "overlays/menu", "ovl_menu_"),
    "OVL_EXTRA_APP.ELF": (3, "analysis_extra.json", "overlay_extra.toml", "overlays/extra", "ovl_extra_"),
}
# files of the disc that never matter to the port (firmware update, menu icons, the dummy BOOT.BIN ...)
IGNORED = re.compile(r"^(UMD_DATA\.BIN|PSP_GAME/(PARAM\.SFO|ICON0\.PNG|ICON1\.PMF|PIC0\.PNG|PIC1\.PNG|SND0\.AT3"
                     r"|SYSDIR/(BOOT\.BIN|OPNSSMP\.BIN|UPDATE/.*)))$", re.I)


def say(msg):
    print(msg, flush=True)


def die(msg):
    print("error: " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def md5_path(path, cache=None):
    st = os.stat(path)
    key = "%s|%d|%d" % (os.path.abspath(path), st.st_size, int(st.st_mtime))
    if cache is not None and key in cache:
        return cache[key]
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(8 << 20), b""):
            h.update(b)
    if cache is not None:
        cache[key] = h.hexdigest()
    return h.hexdigest()


def default_mods(work):
    return os.environ.get("PSPRECOMP_MODS") or os.path.join(work, "mods")


# --------------------------------------------------------------------------------------------- ISO 9660 in memory

def iso_files(read):
    """{path: (offset, size)} of an ISO 9660 image given read(offset, n) -> bytes."""
    pvd = read(16 * SECTOR, SECTOR)
    if pvd[1:6] != b"CD001":
        raise ValueError("no ISO 9660 volume descriptor")
    sec = 16
    while pvd[0] != 1:
        sec += 1
        pvd = read(sec * SECTOR, SECTOR)
        if pvd[1:6] != b"CD001" or pvd[0] == 255:
            raise ValueError("no primary volume descriptor")
    out = {}

    def rec(buf, off):
        ln = buf[off]
        if not ln:
            return None, 0
        ext, size = struct.unpack_from("<I", buf, off + 2)[0], struct.unpack_from("<I", buf, off + 10)[0]
        nl = buf[off + 32]
        raw = bytes(buf[off + 33: off + 33 + nl])
        name = raw.decode("ascii", "replace").split(";")[0]
        if name.endswith(".") and len(name) > 1:
            name = name[:-1]
        return (raw, name, ext, size, bool(buf[off + 25] & 2)), ln

    def walk(ext, size, prefix, depth):
        if depth > 16:
            return
        data = read(ext * SECTOR, size)
        off = 0
        while off < len(data):
            r, ln = rec(data, off)
            if not ln:
                off = (off // SECTOR + 1) * SECTOR
                continue
            off += ln
            raw, name, e, s, is_dir = r
            if raw in (b"\x00", b"\x01"):
                continue
            if is_dir:
                walk(e, s, prefix + name + "/", depth + 1)
            else:
                out[prefix + name] = (e * SECTOR, s)

    root, _ = rec(pvd, 156)
    walk(root[2], root[3], "", 0)
    return out


# --------------------------------------------------------------------------------------------- the patch's base

class IsoSource:
    """The base read straight from a disc image (.iso / .cso)."""

    def __init__(self, path):
        import setup_from_iso as sfi
        self.f = sfi.open_image(path)
        self.name = os.path.basename(path)

    def read(self, pos, n):
        self.f.seek(pos)
        b = self.f.read(n)
        return b + bytes(n - len(b)), ([(pos + len(b), pos + n)] if len(b) < n else [])

    def files(self):
        return iso_files(lambda o, n: self.read(o, n)[0])


class WorkSource:
    """A retail disc rebuilt from an install folder and one entry of disc_layouts.json. Files whose MD5 does not match
    the table (or that the install folder does not keep) are unknown bytes."""

    def __init__(self, work, layout, md5cache):
        self.layout = layout
        self.name = "%s (rebuilt from %s)" % (layout["id"], work)
        self.size = layout["size"]
        self.blobs = [(b["offset"], zlib.decompress(base64.b64decode(b["data"]))) for b in layout["blobs"]]
        disc = os.path.join(work, "disc0")
        self.spans = []          # (offset, size, host path or None=zeros, file offset)
        self.missing = []
        self.holes = []          # (lo, hi) absolute: bytes of a file taken from the other disc that differ there
        for path, f in layout["files"].items():
            off, size = f["offset"], f["size"]
            if f.get("zero"):
                self.spans.append((off, size, None, 0))
                continue
            host = None
            if path.upper() == "PSP_GAME/SYSDIR/EBOOT.BIN":
                cand = os.path.join(work, "EBOOT.enc.bin")
            else:
                cand = os.path.join(disc, *path.split("/"))
            if os.path.isfile(cand) and os.path.getsize(cand) == size:
                m = md5_path(cand, md5cache)
                if m == f["md5"]:
                    host = cand
                for v in f.get("variants", ()):
                    if m == v["md5"]:
                        host = cand
                        self.holes += [(off + lo, off + hi) for lo, hi in v["differs"]]
            if host:
                self.spans.append((off, size, host, 0))
            else:
                self.missing.append(path)
        self.spans.sort()
        self.starts = [s[0] for s in self.spans]
        self._fh = {}

    def _open(self, p):
        if p not in self._fh:
            self._fh[p] = open(p, "rb")
        return self._fh[p]

    def read(self, pos, n):
        buf = bytearray(n)
        known = vcdiff.Unknown()
        for off, data in self.blobs:
            lo, hi = max(pos, off), min(pos + n, off + len(data))
            if lo < hi:
                buf[lo - pos:hi - pos] = data[lo - off:hi - off]
                known.add(lo, hi)
        i = max(0, bisect.bisect_right(self.starts, pos) - 1)
        while i < len(self.spans) and self.spans[i][0] < pos + n:
            off, size, host, _ = self.spans[i]
            lo, hi = max(pos, off), min(pos + n, off + size)
            if lo < hi:
                if host:
                    f = self._open(host)
                    f.seek(lo - off)
                    buf[lo - pos:hi - pos] = f.read(hi - lo)
                known.add(lo, hi)
            i += 1
        # outside the files the retail discs are zero, except the blobs (`layout` checks that)
        missing = []
        k = vcdiff.Unknown(known.r)          # file bytes that are not available are unknown
        for lo, hi in self._file_ranges(pos, n):
            cur = lo
            for a, b in k.overlaps(lo, hi):
                if a > cur:
                    missing.append((cur, a))
                cur = max(cur, b)
            if cur < hi:
                missing.append((cur, hi))
        for lo, hi in self.holes:
            if lo < pos + n and hi > pos:
                missing.append((max(lo, pos), min(hi, pos + n)))
        return bytes(buf), missing

    def _file_ranges(self, pos, n):
        out = []
        for path, f in self.layout["files"].items():
            lo, hi = max(pos, f["offset"]), min(pos + n, f["offset"] + f["size"])
            if lo < hi:
                out.append((lo, hi))
        return out


def load_layouts():
    with open(LAYOUTS) as f:
        return json.load(f)["discs"]


# --------------------------------------------------------------------------------------------- package index

def parse_index(b):
    magic, tag, count, _ = struct.unpack_from("<I4sII", b, 0)
    if tag != b"pack":
        raise ValueError("not a PACKAGE_INFO.BIN")
    out = {}
    for i in range(count):
        crc, sector, sf = struct.unpack_from("<III", b, 16 + 12 * i)
        out[crc] = (sector * SECTOR, sf & 0x0FFFFFFF, sf >> 28)
    return out


def path_names():
    import package_extract as pe
    names = {}
    try:
        with open(PATHS) as f:
            for line in f:
                p = line.strip()
                if p:
                    names[pe.package_crc(p)] = p
    except OSError:
        pass
    return names


# --------------------------------------------------------------------------------------------- import

class Sink:
    """Collects the target bytes of one file (in memory, or in a temp file for big ones)."""

    def __init__(self, size, path=None):
        self.size = size
        self.unknown = vcdiff.Unknown()
        self.path = path
        self.f = open(path, "w+b") if path else None
        self.buf = None if path else bytearray(size)

    def put(self, at, data):
        if self.f:
            self.f.seek(at)
            self.f.write(data)
        else:
            self.buf[at:at + len(data)] = data

    def read(self, at, n):
        if self.f:
            self.f.seek(at)
            return self.f.read(n)
        return bytes(self.buf[at:at + n])

    def close(self):
        if self.f:
            self.f.close()
            self.f = None


def run_patch(patch, src, wanted, tmpdir, log=True):
    """Apply the patch against src. wanted(path, size) -> 'mem' | 'file' | None decides which target files are kept.
    Returns (target files {path: (offset, size)}, sinks {path: Sink}, bad windows [(index, target range, files)])."""
    files, sinks, spans, bad = None, {}, [], []
    pending = []                  # windows decoded before the target directory could be read
    head = bytearray()

    def route(w):
        lo, hi = w.target_offset, w.target_offset + len(w.data)
        for off, size, path in spans:
            if off >= hi:
                break
            a, b = max(lo, off), min(hi, off + size)
            if a < b and path in sinks:
                s = sinks[path]
                s.put(a - off, w.data[a - lo:b - lo])
                for ua, ub in w.unknown:
                    ua, ub = max(ua + lo, a), min(ub + lo, b)
                    if ua < ub:
                        s.unknown.add(ua - off, ub - off)
        if w.checksum_ok is False:
            touched = [p for off, size, p in spans if off < hi and off + size > lo and size]
            bad.append((w.index, (lo, hi), touched))

    for w in vcdiff.apply(patch, src.read):
        if files is None:
            pending.append(w)
            head += w.data
            try:
                files = iso_files(lambda o, n: bytes(head[o:o + n]) if o + n <= len(head) else _short())
            except _Short:
                files = None
                if len(head) > (256 << 20):
                    raise ValueError("the patched disc's directory could not be read")
                continue
            except ValueError as e:
                raise ValueError("the patched disc has no readable ISO 9660 directory (%s): is this a Dissidia 012 ISO patch?" % e)
            spans[:] = sorted((off, size, p) for p, (off, size) in files.items())
            for p, (off, size) in files.items():
                how = wanted(p, size)
                if how:
                    sinks[p] = Sink(size, os.path.join(tmpdir, "t%04d.bin" % len(sinks)) if how == "file" else None)
            for pw in pending:
                route(pw)
            pending = None
            head = None
            continue
        route(w)
        if log and w.index % 20 == 0:
            say("  window %d (%d MB)" % (w.index, (w.target_offset + len(w.data)) >> 20))
    if files is None:
        raise ValueError("the patch ended before the disc directory")
    return files, sinks, bad


class _Short(Exception):
    pass


def _short():
    raise _Short()


def score(src, patch):
    """How well src fits the patch: (windows that fail their checksum, unknown target bytes)."""
    fails = unk = 0
    for w in vcdiff.apply(patch, src.read):
        if w.checksum_ok is False:
            fails += 1
        unk += sum(b - a for a, b in w.unknown)
    return fails, unk


def elf_segments(elf):
    phoff = struct.unpack_from("<I", elf, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 42)
    out = []
    for i in range(phnum):
        t, off, va, _pa, fs, ms, _f, _a = struct.unpack_from("<8I", elf, phoff + i * phentsize)
        if t == 1:
            out.append((off, va, fs, ms))
    return out


def eboot_patch_records(retail, patched):
    """[(vaddr, bytes)] where the loadable bytes of two EBOOT ELFs differ (same program headers)."""
    rs, ps = elf_segments(retail), elf_segments(patched)
    if [(s[1], s[3]) for s in rs] != [(s[1], s[3]) for s in ps]:
        raise ValueError("the patched EBOOT has a different memory layout; not supported")
    out = []
    for (ro, va, rfs, _), (po, _va, pfs, _ms) in zip(rs, ps):
        n = max(rfs, pfs)
        a = retail[ro:ro + rfs].ljust(n, b"\0")
        b = patched[po:po + pfs].ljust(n, b"\0")
        i = 0
        while i < n:
            if a[i] != b[i]:
                j = i
                while j < n and (a[j] != b[j] or (j + 8 < n and a[j:j + 8] != b[j:j + 8])):
                    j += 1
                out.append((va + i, b[i:j]))
                i = j
            else:
                i += 1
    return out


def write_if_changed(path, data):
    if os.path.isfile(path):
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return True


def slug(name):
    s = re.sub(r"[^A-Za-z0-9._-]+", "-", name).strip("-.").lower()
    return s or "xdelta-mod"


def cmd_import(a):
    work = os.path.abspath(a.work)
    if not os.path.isdir(os.path.join(work, "disc0")):
        die("%s is not an install folder (no disc0/; run setup_from_iso.py first)" % work)
    mods = os.path.abspath(a.mods or default_mods(work))
    if a.patch.startswith("fd:"):          # Android's file picker hands over an open descriptor
        with os.fdopen(int(a.patch[3:]), "rb", closefd=False) as f:
            f.seek(0)
            patch = f.read()
        shown = "the chosen patch"
    else:
        with open(a.patch, "rb") as f:
            patch = f.read()
        shown = os.path.basename(a.patch)
    try:
        app, _, sec = vcdiff.header(patch)
    except ValueError as e:
        die("%s: %s" % (shown, e))
    # the name: the patch file's, else the patched image's name in the patch header ("target//source/")
    target = re.split(r"[\\/]", app.decode("latin-1").split("//")[0])[-1] if app else ""
    base = os.path.splitext(os.path.basename(a.patch))[0] if not a.patch.startswith("fd:") else os.path.splitext(target)[0]
    name = a.name or base or "xdelta-patch"
    folder = os.path.join(mods, slug(name))
    patch_sha = hashlib.sha256(patch).hexdigest()
    mj = os.path.join(folder, "mod.json")
    if os.path.isfile(mj) and not a.force:
        try:
            old = json.load(open(mj))
        except ValueError:
            old = {}
        if old.get("patch_sha256") == patch_sha:
            say("%s is already installed as %s (--force to redo)" % (shown, folder))
            return 0
    src_name = re.split(r"[\\/]", app.decode("latin-1").rstrip("/").split("//")[-1])[-1] if b"//" in app else ""
    say("Patch: %s (%.1f MB)%s" % (shown, len(patch) / 1048576.0, ", made for " + src_name if src_name else ""))

    md5cache_path = os.path.join(work, ".md5cache.json")
    try:
        md5cache = json.load(open(md5cache_path))
    except (OSError, ValueError):
        md5cache = {}
    if a.iso:
        cands = [IsoSource(a.iso)]
    else:
        cands = [WorkSource(work, l, md5cache) for l in load_layouts()]
    try:
        json.dump(md5cache, open(md5cache_path, "w"))
    except OSError:
        pass
    if len(cands) > 1:
        say("Finding the disc the patch was made for ...")
        scored = []
        for c in cands:
            s = score(c, patch)
            say("  %s: %d window(s) fail, %d byte(s) unknown" % (c.layout["id"], s[0], s[1]))
            scored.append((s, c))
        scored.sort(key=lambda x: x[0])
        src = scored[0][1]
    else:
        src = cands[0]
    say("Base: %s" % src.name)

    # what to keep from the target disc
    tmp = os.path.join(work, ".xdelta_tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)

    def wanted(path, size):
        if IGNORED.match(path) or not path.upper().startswith(("PSP_GAME/USRDIR/", "PSP_GAME/SYSDIR/EBOOT.BIN")):
            return None
        return "file" if size > (64 << 20) else "mem"

    say("Applying the patch ...")
    try:
        files, sinks, bad = run_patch(patch, src, wanted, tmp)
    except ValueError as e:
        shutil.rmtree(tmp, ignore_errors=True)
        die(str(e))
    if bad:   # expected where the base differs from the patch author's own disc (ISO header sectors, region files)
        say("  %d of the patch's blocks differ from its own checksum (%s); the files below are checked one by one"
            % (len(bad), ", ".join("#%d" % b[0] for b in bad)))

    disc = os.path.join(work, "disc0")
    usr = os.path.join(disc, "PSP_GAME", "USRDIR")
    staging = folder + ".new"
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(os.path.join(staging, "install"))
    problems, summary = [], []

    def need_known(path, sink):
        if sink.unknown:
            problems.append("%s: %d byte(s) could not be rebuilt (the install folder lacks part of the base disc)"
                            % (path, sink.unknown.total()))
            return False
        return True

    # ---- PACKAGE.BIN entries
    up = {p.upper(): p for p in files}
    pinfo, pbin = up.get("PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN"), up.get("PSP_GAME/USRDIR/DATA/PACKAGE.BIN")
    if pinfo in sinks and pbin in sinks and need_known(pinfo, sinks[pinfo]):
        tidx = parse_index(sinks[pinfo].read(0, files[pinfo][1]))
        bidx = parse_index(open(os.path.join(usr, "DATA", "PACKAGE_INFO.BIN"), "rb").read())
        names = path_names()
        bpkg = open(os.path.join(usr, "DATA", "PACKAGE.BIN"), "rb")
        tp = sinks[pbin]
        changed = added = bad_e = 0
        nbytes = 0
        for crc, (off, size, flags) in sorted(tidx.items(), key=lambda x: x[1][0]):
            data = tp.read(off, size)
            if len(data) != size:
                problems.append("PACKAGE.BIN entry %08x lies past the end of the patched file" % crc)
                continue
            old = bidx.get(crc)
            mine = None
            if old:
                bpkg.seek(old[0])
                mine = bpkg.read(old[1])
            holes = tp.unknown.overlaps(off, off + size)
            if holes and mine is not None and len(mine) == size:
                # bytes the base could not supply (the other region's copy of a sector): the player's own entry stands in
                data = bytearray(data)
                for lo, hi in holes:
                    data[lo - off:hi - off] = mine[lo - off:hi - off]
                data = bytes(data)
                if data != mine:
                    say("  note: entry %s: %d byte(s) taken from your disc's own copy" % (names.get(crc, "%08x" % crc), sum(h - l for l, h in holes)))
                holes = []
            if mine is not None and mine == data:
                continue
            if holes:
                bad_e += 1
                problems.append("PACKAGE.BIN entry %s could not be rebuilt" % names.get(crc, "%08x" % crc))
                continue
            if old and flags != old[2]:
                say("  note: entry %s changes its flags %d -> %d (the loose-file layer keeps the retail flags)"
                    % (names.get(crc, "%08x" % crc), old[2], flags))
            rel = names.get(crc) or "by_crc/%08x.bin" % crc
            out = os.path.join(staging, "install", *rel.split("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as f:
                f.write(data)
            nbytes += size
            if old:
                changed += 1
            else:
                added += 1
        # data the patch appends past the last entry, which its code reads by offset (no index entry points there)
        base_size = os.path.getsize(os.path.join(usr, "DATA", "PACKAGE.BIN"))
        tsize = files[pbin][1]
        if tsize > base_size:
            if base_size % SECTOR:
                problems.append("PACKAGE.BIN is not a whole number of sectors; the appended data cannot be served")
            elif tp.unknown.overlaps(base_size, tsize):
                problems.append("PACKAGE.BIN: the data appended by the patch could not be rebuilt")
            else:
                with open(os.path.join(staging, "package_tail.bin"), "wb") as f:
                    at = base_size
                    while at < tsize:
                        n = min(8 << 20, tsize - at)
                        f.write(tp.read(at, n))
                        at += n
                summary.append("PACKAGE.BIN: %.1f MB appended after the last entry (package_tail.bin)" % ((tsize - base_size) / 1048576.0))
        elif tsize < base_size:
            summary.append("PACKAGE.BIN: the patched file is shorter than yours; entries past its end are unchanged")
        removed = len(set(bidx) - set(tidx))
        summary.append("PACKAGE.BIN: %d entr%s changed, %d added (%.1f MB)%s" % (
            changed, "y" if changed == 1 else "ies", added, nbytes / 1048576.0,
            ", %d no longer listed (left as they are)" % removed if removed else ""))

    # ---- other USRDIR files and the code
    code = {}
    for p, (off, size) in sorted(files.items()):
        if p not in sinks or p in (pinfo, pbin):
            continue
        s = sinks[p]
        u = p.upper()
        if u == "PSP_GAME/SYSDIR/EBOOT.BIN":
            enc = os.path.join(work, "EBOOT.enc.bin")
            if not s.unknown and os.path.isfile(enc) and os.path.getsize(enc) == size and open(enc, "rb").read() == s.read(0, size):
                continue                           # the disc's own (encrypted) EBOOT: unchanged
            local = os.path.join(work, "EBOOT.dec.elf")
            key = "EBOOT.BIN"
        else:
            rel = p.split("/", 2)[2]               # under USRDIR
            local = os.path.join(usr, *rel.split("/"))
            key = os.path.basename(u) if os.path.basename(u) in CODE else None
        try:
            mine = open(local, "rb").read() if size <= (64 << 20) else None
        except OSError:
            mine = b""
        if mine is not None:
            data = s.read(0, size)
            if data == mine:
                continue
        else:                                      # big file: compare in chunks
            same = os.path.getsize(local) == size if os.path.isfile(local) else False
            if same:
                with open(local, "rb") as f:
                    at = 0
                    while at < size and same:
                        n = min(8 << 20, size - at)
                        same = f.read(n) == s.read(at, n)
                        at += n
            if same:
                continue
            data = None
        if not need_known(p, s):
            continue
        if key:
            if key == "EBOOT.BIN" and data[:4] != b"\x7fELF":
                problems.append("the patched EBOOT.BIN is encrypted; only patches that put a plain (decrypted) EBOOT on the disc are supported")
                continue
            code[key] = data
            continue
        if u.endswith(".PRX"):
            problems.append("%s: patched PRX modules are not supported (left as it is)" % p)
            continue
        if u.endswith(".PMF") and os.path.isfile(local) and size < os.path.getsize(local):
            # ISO patches that add data shrink a movie to stay within the UMD size; the PC has the room
            summary.append("%s: made smaller by the patch (%d -> %d MB) to make room on the disc; the original is kept"
                           % (rel, os.path.getsize(local) >> 20, size >> 20))
            continue
        out = os.path.join(staging, "usrdir", *rel.upper().split("/"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if data is not None:
            with open(out, "wb") as f:
                f.write(data)
        else:
            with open(out, "wb") as f:
                at = 0
                while at < size:
                    n = min(8 << 20, size - at)
                    f.write(s.read(at, n))
                    at += n
        summary.append("%s: replaced (%d B)" % (rel, size))
    for s in sinks.values():
        s.close()
    shutil.rmtree(tmp, ignore_errors=True)

    if code:
        os.makedirs(os.path.join(staging, "code"))
        for key, data in sorted(code.items()):
            with open(os.path.join(staging, "code", key), "wb") as f:
                f.write(data)
            if key == "EBOOT.BIN":
                recs = eboot_patch_records(open(os.path.join(work, "EBOOT.dec.elf"), "rb").read(), data)
                with open(os.path.join(staging, "code", "eboot.patch"), "wb") as f:
                    for va, b in recs:
                        f.write(struct.pack("<II", va, len(b)) + b)
                summary.append("EBOOT: %d changed byte run(s)" % len(recs))
            else:
                summary.append("%s: patched (code)" % key)
    stamp = code_stamp(staging)
    meta = {"name": name, "kind": "xdelta", "enabled": True, "patch": shown, "patch_sha256": patch_sha,
            "base": getattr(src, "layout", {}).get("id", src.name), "code": sorted(code), "code_stamp": stamp,
            "description": "Installed from %s. Settings > Mods turns it on and off (next start)." % shown}
    if os.path.isfile(mj):                     # keep the user's on/off choice across a re-import
        try:
            meta["enabled"] = json.load(open(mj)).get("enabled", True) is not False
        except ValueError:
            pass
    with open(os.path.join(staging, "mod.json"), "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    if problems:
        for p in problems:
            say("  problem: " + p)
        if any("could not be rebuilt" in p or "encrypted" in p for p in problems) and not a.allow_partial:
            shutil.rmtree(staging, ignore_errors=True)
            die("the patch could not be applied completely (above); nothing was installed (--allow-partial to install anyway)")
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    os.replace(staging, folder)
    for line in summary:
        say("  " + line)
    say("Installed as %s" % folder)
    if code:
        say("This patch changes the game's code: it takes effect after the next build (pipeline.sh / the setup's Rebuild).")
    return 0


def code_stamp(folder):
    h = hashlib.sha256()
    d = os.path.join(folder, "code")
    if os.path.isdir(d):
        for n in sorted(os.listdir(d)):
            if n in CODE:
                h.update(n.encode() + b"\0")
                with open(os.path.join(d, n), "rb") as f:
                    h.update(hashlib.sha256(f.read()).digest())
    return h.hexdigest()[:16]


# --------------------------------------------------------------------------------------------- list

def xdelta_folders(mods):
    out = []
    if not os.path.isdir(mods):
        return out
    for n in sorted(os.listdir(mods)):
        p = os.path.join(mods, n, "mod.json")
        if n.startswith((".", "_")) or not os.path.isfile(p):
            continue
        try:
            m = json.load(open(p))
        except ValueError:
            continue
        if m.get("kind") == "xdelta":
            out.append((n, os.path.join(mods, n), m))
    return out


def cmd_list(a):
    mods = os.path.abspath(a.mods or default_mods(os.getcwd()))
    for n, p, m in xdelta_folders(mods):
        say("%-24s %-3s %s%s" % (n, "on" if m.get("enabled", True) is not False else "off", m.get("name", n),
                                 "  [code: %s]" % ", ".join(m["code"]) if m.get("code") else ""))
    return 0


# --------------------------------------------------------------------------------------------- layout (maintainers)

def cmd_layout(a):
    import setup_from_iso as sfi
    try:
        doc = json.load(open(LAYOUTS))
    except (OSError, ValueError):
        doc = {"discs": []}
    for path in a.iso:
        iso = IsoSource(path)
        files = iso.files()
        size = os.path.getsize(path)
        sfo = files.get("PSP_GAME/PARAM.SFO")
        disc_id = sfi.parse_sfo(iso.read(sfo[0], sfo[1])[0]).get("DISC_ID") if sfo else "?"
        out = {}
        for p, (off, sz) in sorted(files.items()):
            data = iso.read(off, sz)[0]
            e = {"offset": off, "size": sz, "md5": hashlib.md5(data).hexdigest()}
            if not data.strip(b"\0"):
                e["zero"] = True
            out[p] = e
        spans = vcdiff.Unknown((o, o + s) for o, s in files.values())
        blobs = []
        pos = 0
        for lo, hi in spans.r + [[size, size]]:
            if pos < lo:                           # a gap between files: keep its non-zero bytes
                at = pos
                while at < lo:
                    n = min(lo - at, 4 << 20)
                    b = iso.read(at, n)[0]
                    if b.strip(b"\0"):
                        i = len(b) - len(b.lstrip(b"\0"))
                        j = len(b.rstrip(b"\0"))
                        i -= i % SECTOR
                        blobs.append((at + i, b[i:j]))
                    at += n
            pos = max(pos, hi)
        for small in ("UMD_DATA.BIN",):
            if small in files:
                off, sz = files[small]
                blobs.append((off, iso.read(off, sz)[0]))
        if "PSP_GAME/SYSDIR/EBOOT.BIN" in files:
            off, _ = files["PSP_GAME/SYSDIR/EBOOT.BIN"]
            blobs.append((off, iso.read(off, EBOOT_HDR)[0]))
        if sum(len(b) for _, b in blobs) > (256 << 10):
            die("%s: %d KB outside the files; not a retail disc?" % (path, sum(len(b) for _, b in blobs) >> 10))
        entry = {"id": disc_id, "size": size, "files": out,
                 "blobs": [{"offset": o, "data": base64.b64encode(zlib.compress(b, 9)).decode()} for o, b in blobs]}
        doc["discs"] = [d for d in doc["discs"] if d["id"] != disc_id] + [entry]
        say("%s: %s, %d files, %d B of metadata" % (path, disc_id, len(out), sum(len(b) for _, b in blobs)))
    # a file that is the same size on two discs and differs in a few sectors: the other disc's copy serves this one with
    # those sectors unknown (EU and US PACKAGE.BIN differ in 3 sectors)
    isos = [IsoSource(p) for p in a.iso]
    ids = {}
    for iso in isos:
        sfo = iso.files().get("PSP_GAME/PARAM.SFO")
        ids[iso] = sfi.parse_sfo(iso.read(sfo[0], sfo[1])[0]).get("DISC_ID")
    by_id = {d["id"]: d for d in doc["discs"]}
    for x in isos:
        for y in isos:
            if x is y:
                continue
            dx, dy = by_id[ids[x]], by_id[ids[y]]
            for p, fx in dx["files"].items():
                fy = dy["files"].get(p)
                if not fy or fy["size"] != fx["size"] or fy["md5"] == fx["md5"] or fx.get("zero"):
                    continue
                diff = vcdiff.Unknown()
                at = 0
                while at < fx["size"]:
                    n = min(8 << 20, fx["size"] - at)
                    bx, by = x.read(fx["offset"] + at, n)[0], y.read(fy["offset"] + at, n)[0]
                    if bx != by:
                        for i in range(0, n, SECTOR):
                            if bx[i:i + SECTOR] != by[i:i + SECTOR]:
                                diff.add(at + i, at + min(i + SECTOR, n))
                    at += n
                if diff.total() * 8 < fx["size"]:
                    fx.setdefault("variants", [])
                    fx["variants"] = [v for v in fx["variants"] if v["md5"] != fy["md5"]] + [
                        {"md5": fy["md5"], "disc": ids[y], "differs": diff.r}]
    doc["discs"].sort(key=lambda d: d["id"])
    doc["about"] = ("Layouts of the retail Dissidia 012 discs for xdelta_mod.py: file positions, sizes and MD5s plus the "
                    "ISO 9660 descriptor/directory sectors, UMD_DATA.BIN and the plaintext ~PSP header of EBOOT.BIN "
                    "(metadata only, no game data). Regenerate with: xdelta_mod.py layout <iso>...")
    with open(LAYOUTS, "w") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")
    return 0


# --------------------------------------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description="Install xdelta ISO patches as Dissidia 012 mod folders.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("import", help="apply a patch and install what it changes as a mod folder")
    p.add_argument("patch")
    p.add_argument("--work", required=True, help="the install folder (holds disc0/)")
    p.add_argument("--mods", help="mods folder (default: $PSPRECOMP_MODS or <work>/mods)")
    p.add_argument("--name", help="mod name (default: the patch's file name)")
    p.add_argument("--iso", help="read the base from this disc image instead of the install folder")
    p.add_argument("--force", action="store_true", help="re-import even when the same patch is installed")
    p.add_argument("--allow-partial", action="store_true", help="install even if some files could not be rebuilt")
    p = sub.add_parser("build-code", help="recompile the code of the installed xdelta mods (before the C++ build)")
    p.add_argument("--work", required=True)
    p.add_argument("--mods")
    p.add_argument("--out", help="default <work>/overlays/xmods")
    p.add_argument("--psprecomp", help="recompiler binary (default $PSPRECOMP_BIN or target/release/psprecomp)")
    p.add_argument("--game", help="games/dissidia folder (game.toml, overlay_*.toml)")
    p = sub.add_parser("list", help="list the installed xdelta mods")
    p.add_argument("--mods")
    p = sub.add_parser("layout", help="(maintainers) record the layout of retail discs in disc_layouts.json")
    p.add_argument("iso", nargs="+")
    a = ap.parse_args(argv)
    if a.cmd == "import":
        return cmd_import(a)
    if a.cmd == "build-code":
        import xdelta_code
        return xdelta_code.build(a)
    if a.cmd == "list":
        return cmd_list(a)
    return cmd_layout(a)


if __name__ == "__main__":
    sys.exit(main())
