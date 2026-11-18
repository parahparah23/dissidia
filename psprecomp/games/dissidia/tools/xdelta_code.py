#!/usr/bin/env python3
"""`xdelta_mod.py build-code`: recompile the code that installed xdelta mods change (see xdelta_mod.py).

For every mod folder with "kind": "xdelta" and a code/ folder, and every module it patches (EBOOT, battle / menu /
extra overlay):

1. the module's analysis (<work>/analysis*.json, the retail one) gets the patched bytes; an overlay that grew (code
   appended after its end, a common trick of ISO patches) gets a longer segment,
2. jump targets the patched code needs that the retail analysis does not have become force_entries (a call target or
   a jump into code no function covers) and force_mid_entries (a jump into the middle of a function),
3. the module is recompiled twice the way the pipeline does it (build_overlay.py, PSPRECOMP_CROSS_MID=1), from the
   retail and from the patched analysis, under one symbol prefix (a few seconds each), and only the functions whose
   C++ differs are kept, plus the mid-entry wrappers of those functions.

Everything goes into one function set, <work>/overlays/xmods (generated/batch_*.cpp, funcs.h, mid_entries.cpp and
dispatch.cpp with the table `dissidia_xmod_sets()` that games/dissidia/runtime/xdelta_mods.cpp reads). The set always
exists (empty without mods) because the runtime links against it. Files are only rewritten when their content changes,
so an unchanged set does not recompile.
"""
import base64
import bisect
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xdelta_mod as X  # noqa: E402

FUNC_RE = re.compile(r'^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{ /\* 0x([0-9A-Fa-f]{8}) \*/', re.M)
MID_RE = re.compile(r'RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{\n    ctx->entry_point = 0x([0-9A-F]{8})U;\n'
                    r'    (\w+)\(rdram, ctx\);\n    ctx->entry_point = 0;\n\}\n')
SYM_RE = re.compile(r'\b(?:[A-Za-z0-9]+_)*?((?:FUN|func)_[0-9A-Fa-f]{8}\w*)')


def say(msg):
    print(msg, flush=True)


def functions_of(gen_dir):
    """{addr: (name, text)} of every function in the batch files of a recompiled set."""
    out = {}
    if not os.path.isdir(gen_dir):
        return out
    for fn in sorted(os.listdir(gen_dir)):
        if not (fn.startswith("batch_") and fn.endswith(".cpp")):
            continue
        s = open(os.path.join(gen_dir, fn)).read()
        ms = list(FUNC_RE.finditer(s))
        for i, m in enumerate(ms):
            end = ms[i + 1].start() if i + 1 < len(ms) else len(s)
            body = s[m.start():end].rstrip() + "\n"
            out[int(m.group(2), 16)] = (m.group(1), body)
    return out


def decode_jumps(seg_va, data, lo, hi):
    """[(at, kind, target)] for j / jal in [lo, hi) of a segment."""
    out = []
    for va in range(lo & ~3, hi, 4):
        o = va - seg_va
        if o < 0 or o + 4 > len(data):
            continue
        w = struct.unpack_from("<I", data, o)[0]
        op = w >> 26
        if op in (2, 3):
            t = ((va + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            out.append((va, "jal" if op == 3 else "j", t))
    return out


def patched_segments(module, analysis, blob):
    """New analysis segments (list of dicts) with the patched bytes."""
    segs = []
    if module == 0:
        phs = X.elf_segments(blob)
        for s in analysis["segments"]:
            va = int(s["p_vaddr"], 16)
            old = base64.b64decode(s["data_b64"])
            ph = [p for p in phs if p[1] == va]
            if not ph:
                raise ValueError("patched EBOOT has no segment at 0x%08X" % va)
            off, _, fs, ms = ph[0]
            data = blob[off:off + fs].ljust(len(old), b"\0")[:max(len(old), fs)]
            n = dict(s)
            n["data_b64"] = base64.b64encode(data).decode()
            segs.append(n)
    else:
        if len(analysis["segments"]) != 1:
            raise ValueError("overlay analysis with %d segments" % len(analysis["segments"]))
        s = analysis["segments"][0]
        old = base64.b64decode(s["data_b64"])
        _, va, fs, ms = X.elf_segments(blob)[0]
        body = blob[X.OVL_FILE_HDR:X.OVL_FILE_HDR + fs]       # filesz may exceed memsz: code appended in a patch
        data = body.ljust(len(old), b"\0")
        n = dict(s)
        n["data_b64"] = base64.b64encode(data).decode()
        n["p_filesz"] = len(data) if isinstance(s.get("p_filesz"), int) else s.get("p_filesz")
        n["p_memsz"] = len(data) if isinstance(s.get("p_memsz"), int) else s.get("p_memsz")
        segs.append(n)
    return segs


def changed_ranges(old, new, va):
    out, i, n = [], 0, max(len(old), len(new))
    old, new = old.ljust(n, b"\0"), new.ljust(n, b"\0")
    while i < n:
        if old[i] != new[i]:
            j = i
            while j < n and old[j] != new[j]:
                j += 1
            out.append((va + i, va + j))
            i = j
        else:
            i += 1
    return out


def entries_for(analysis, segs_old, segs_new):
    """force_entries / force_mid_entries the patched code needs."""
    funcs = sorted((int(f["address"], 16), int(f["size"])) for f in analysis["functions"] if f.get("size"))
    starts = {a for a, _ in funcs}
    mids = {int(m.get("addr", "0"), 16) for m in analysis.get("mid_entries", []) if m.get("addr")}

    def owner(t):
        best = None
        for a, sz in funcs:
            if a < t < a + sz:
                best = (a, sz) if best is None or a > best[0] else best
        return best

    force, mid = set(), {}
    for so, sn in zip(segs_old, segs_new):
        va = int(sn["p_vaddr"], 16)
        old, new = base64.b64decode(so["data_b64"]), base64.b64decode(sn["data_b64"])
        for lo, hi in changed_ranges(old, new, va):
            # the whole words around a change, and everything appended after the retail end
            for at, kind, t in decode_jumps(va, new, lo - 4, hi + 4):
                if t in starts or t in mids or not (va <= t < va + len(new)):
                    continue
                o = owner(t)
                if kind == "j" and o:
                    mid[t] = o[0]
                else:
                    force.add(t)
        if len(new) > len(old):                       # code past the retail end: jumps inside it too
            for at, kind, t in decode_jumps(va, new, va + len(old), va + len(new)):
                if t in starts or t in mids or not (va <= t < va + len(new)):
                    continue
                o = owner(t)
                if kind == "j" and o:
                    mid[t] = o[0]
                else:
                    force.add(t)
    for t in list(mid):
        if t in force:
            del mid[t]
    return sorted(force), sorted(mid.items())


def new_code_ranges(segs_old, segs_new):
    """[(lo, hi)] vaddr ranges the patch wrote: changed bytes, and everything past the retail end of a segment."""
    out = []
    for so, sn in zip(segs_old, segs_new):
        va = int(sn["p_vaddr"], 16)
        old, new = base64.b64decode(so["data_b64"]), base64.b64decode(sn["data_b64"])
        out += changed_ranges(old[:len(new)], new[:len(old)], va)
        if len(new) > len(old):
            out.append((va + len(old), va + len(new)))
    return out


def pointer_targets(segs_new, ranges):
    """Words of the module that point into code the patch wrote: function-pointer tables, the patch's own or the
    game's (a patch may overwrite a function the analysis missed and that only a table points to). The code reaches
    them with jalr / jr, which no jump decoding sees."""
    starts = sorted(lo for lo, hi in ranges)
    out = set()
    for sn in segs_new:
        data = base64.b64decode(sn["data_b64"])
        n = len(data) // 4
        for v in struct.unpack_from("<%dI" % n, data, 0):
            if v & 3:
                continue
            i = bisect.bisect_right(starts, v) - 1
            if i >= 0:
                lo = starts[i]
                hi = max(h for l, h in ranges if l == lo)
                if lo <= v < hi:
                    out.add(v)
    return out


def missing_targets(set_dir):
    """Static RECOMP_LOOKUP targets of a recompiled set that are not in its dispatch table (recompile_report.json)."""
    try:
        r = json.load(open(os.path.join(set_dir, "recompile_report.json")))
        return {int(x, 16) for x in r.get("dispatch_audit", {}).get("missing_targets", [])}
    except (OSError, ValueError):
        return set()


def toml_with(base_toml, force, mid):
    s = open(base_toml).read() if base_toml and os.path.isfile(base_toml) else "[recompile]\n"
    if force:
        extra = ", ".join('"0x%08X"' % a for a in force)
        m = re.search(r'^force_entries\s*=\s*\[', s, re.M)
        if m:
            s = s[:m.end()] + extra + ", " + s[m.end():]
        else:
            m = re.search(r'^\[recompile\]\s*$', s, re.M)
            if m:
                s = s[:m.end()] + "\nforce_entries = [%s]" % extra + s[m.end():]
            else:
                s = "[recompile]\nforce_entries = [%s]\n" % extra + s
    for e, p in mid:
        s += '\n[[recompile.force_mid_entries]]\nentry  = "0x%08X"\nparent = "0x%08X"\n' % (e, p)
    return s


def c_ident(s):
    s = re.sub(r"[^A-Za-z0-9]", "_", s)
    return ("m" + s) if not s or s[0].isdigit() else s


def build(a):
    work = os.path.abspath(a.work)
    mods = os.path.abspath(a.mods or X.default_mods(work))
    out = os.path.abspath(a.out or os.path.join(work, "overlays", "xmods"))
    game = os.path.abspath(a.game or os.path.join(HERE, ".."))
    env = dict(os.environ)
    if a.psprecomp:
        env["PSPRECOMP_BIN"] = os.path.abspath(a.psprecomp)
    sets, batches, decls, mids = [], {}, [], []
    for idx, (folder, path, meta) in enumerate(X.xdelta_folders(mods)):
        code = os.path.join(path, "code")
        mods_in = [k for k in X.CODE if os.path.isfile(os.path.join(code, k))]
        if not mods_in:
            continue
        stamp = X.code_stamp(path)
        if meta.get("code_stamp") and meta["code_stamp"] != stamp:
            say("xmods: %s: code/ changed since the import; using it as it is now" % folder)
        say("xmods: %s (%s)" % (folder, ", ".join(mods_in)))
        for key in mods_in:
            module, an_name, toml_name, _retail_dir, _retail_prefix = X.CODE[key]
            an_path = os.path.join(work, an_name)
            if not os.path.isfile(an_path):
                say("  %s: %s missing (run the pipeline first); skipped" % (key, an_name))
                continue
            analysis = json.load(open(an_path))
            blob = open(os.path.join(code, key), "rb").read()
            try:
                new_segs = patched_segments(module, analysis, blob)
            except ValueError as e:
                say("  %s: %s; skipped" % (key, e))
                continue
            force, mid = entries_for(analysis, analysis["segments"], new_segs)
            patched = dict(analysis)
            patched["segments"] = new_segs
            prefix = "xm%d_%s_%s" % (idx, c_ident(folder)[:24], ["eboot", "battle", "menu", "extra"][module])
            tmp = tempfile.mkdtemp(prefix="xmod_", dir=work)
            try:
                pa = os.path.join(tmp, "analysis.json")
                json.dump(patched, open(pa, "w"))
                tt = os.path.join(tmp, "recompile.toml")
                ranges = new_code_ranges(analysis["segments"], new_segs)
                funcs = sorted((int(f["address"], 16), int(f["size"])) for f in analysis["functions"] if f.get("size"))
                starts = {a for a, _ in funcs}
                force = sorted(set(force) | {t for t in pointer_targets(new_segs, ranges) if t not in starts})
                lo_seg = min(int(sg["p_vaddr"], 16) for sg in new_segs)
                hi_seg = max(int(sg["p_vaddr"], 16) + len(base64.b64decode(sg["data_b64"])) for sg in new_segs)

                def recompile(analysis_path, toml, od):
                    r = subprocess.run([sys.executable, os.path.join(HERE, "build_overlay.py"), str(module), prefix,
                                        analysis_path, os.path.join(work, "analysis.json"), od, toml],
                                       env=env, capture_output=True, text=True)
                    if r.returncode != 0:
                        say("  %s: recompile failed:\n%s%s" % (key, r.stdout[-2000:], r.stderr[-2000:]))
                        raise SystemExit(1)
                    return functions_of(os.path.join(od, "generated"))
                # the retail module through the very same command and prefix: what differs is what the patch changes
                retail = recompile(an_path, os.path.join(game, toml_name), os.path.join(tmp, "retail"))
                retail_missing = missing_targets(os.path.join(tmp, "retail"))
                od = os.path.join(tmp, "set")
                # the patched module, then again with every lookup target the patch's code needs and the set lacks (a
                # branch out of a function into code the patch wrote, a jump back into the middle of a function ...)
                for _round in range(6):
                    open(tt, "w").write(toml_with(os.path.join(game, toml_name), force, mid))
                    shutil.rmtree(od, ignore_errors=True)
                    variant = recompile(pa, tt, od)
                    known = set(force) | {e for e, _ in mid}
                    extra = sorted(t for t in missing_targets(od) - retail_missing
                                   if lo_seg <= t < hi_seg and t % 4 == 0 and t not in known)
                    if not extra:
                        break
                    for t in extra:
                        own = [a for a, sz in funcs if a < t < a + sz]
                        if own and not any(lo <= t < hi for lo, hi in ranges):
                            mid.append((t, max(own)))       # into unchanged code of a function: continue its frame
                        else:
                            force.append(t)
                    force, mid = sorted(set(force)), sorted(set(mid))
                else:
                    say("  %s: lookup targets still missing after 6 rounds: %s" % (key, ", ".join("0x%08X" % t for t in extra[:8])))
                keep = {}
                for addr, (name, body) in variant.items():
                    r0 = retail.get(addr)
                    if r0 is None or r0[1] != body:
                        keep[addr] = (name, body)
                kept_names = {n for n, _ in keep.values()}
                mtext = open(os.path.join(od, "mid_entries.cpp")).read()
                mk = [(int(m.group(2), 16), m.group(1), m.group(0)) for m in MID_RE.finditer(mtext) if m.group(3) in kept_names]
                fns = sorted((addr, name) for addr, (name, _) in keep.items()) + sorted((addr, n) for addr, n, _ in mk)
                batches["batch_%s.cpp" % prefix] = "".join(b for _, (_, b) in sorted(keep.items()))
                mids.extend(t for _, _, t in mk)
                decls.extend("RECOMP_FUNC void %s(uint8_t* rdram, recomp_context* ctx);" % n for _, n in fns)
                sets.append((folder, meta.get("code_stamp") or stamp, module, prefix, fns))
                say("  %s: %d function(s) and %d mid-entr%s replaced (%d new entr%s added)"
                    % (key, len(keep), len(mk), "y" if len(mk) == 1 else "ies", len(force) + len(mid),
                       "y" if len(force) + len(mid) == 1 else "ies"))
            finally:
                shutil.rmtree(tmp, ignore_errors=True)
    write_set(out, sets, batches, decls, mids)
    say("xmods: %d function set(s) -> %s" % (len(sets), out))
    return 0


def write_set(out, sets, batches, decls, mids):
    gen = os.path.join(out, "generated")
    os.makedirs(gen, exist_ok=True)
    head = '// Generated by xdelta_mod.py build-code — do not edit\n#include "recomp.h"\n#include "funcs.h"\n\n'
    for fn in os.listdir(gen):
        if fn.startswith("batch_") and fn not in batches:
            os.remove(os.path.join(gen, fn))
    for fn, body in batches.items():
        X.write_if_changed(os.path.join(gen, fn), (head + body).encode())
    X.write_if_changed(os.path.join(out, "funcs.h"), ("#pragma once\n#include \"recomp.h\"\n" + "\n".join(decls) + "\n").encode())
    X.write_if_changed(os.path.join(out, "mid_entries.cpp"), (head + "\n".join(mids)).encode())
    d = [head, "#include <cstdint>\n\n",
         "// layout shared with games/dissidia/runtime/xdelta_mods.cpp\n",
         "struct DissidiaXmodFn { uint32_t addr; void (*fn)(uint8_t*, recomp_context*); };\n",
         "struct DissidiaXmodSet { const char* folder; const char* stamp; int module; const DissidiaXmodFn* fns; int count; };\n\n"]
    for i, (folder, stamp, module, prefix, fns) in enumerate(sets):
        d.append("static const DissidiaXmodFn s_fns%d[] = {\n" % i)
        d.extend("    {0x%08XU, %s},\n" % (addr, name) for addr, name in fns)
        d.append("};\n")
    if sets:
        d.append("static const DissidiaXmodSet s_sets[] = {\n")
        for i, (folder, stamp, module, prefix, fns) in enumerate(sets):
            d.append("    {%s, \"%s\", %d, s_fns%d, %d},\n" % (json.dumps(folder), stamp, module, i, len(fns)))
        d.append("};\n")
    d.append("\nconst DissidiaXmodSet* dissidia_xmod_sets(int* count) {\n")
    d.append("    *count = %d;\n    return %s;\n}\n" % (len(sets), "s_sets" if sets else "nullptr"))
    X.write_if_changed(os.path.join(out, "dispatch.cpp"), "".join(d).encode())
    X.write_if_changed(os.path.join(out, "overlay.txt"), b"4 xmods\n")
