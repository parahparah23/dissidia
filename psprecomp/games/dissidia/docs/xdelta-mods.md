# ISO patch (xdelta) mods

Many Dissidia 012 mods are shared as an `.xdelta` patch for the disc image (made with xdelta3 or Delta Patcher). The
native port installs such a patch as a mod folder: what the patch changes is kept, everything else is read from your
own disc as usual, and *Settings > Mods* turns it off and on (next start).

```
tools/xdelta_mod.py import Emendatus11.11.26.xdelta --work <workdir>     # -> <workdir>/mods/emendatus11.11.26
games/dissidia/tools/pipeline.sh <workdir>                                # only if the patch changes code (it says so)
```

In the setup program: *Options > ISO patch (xdelta)* on a new install, or *Change extras* on an installed game (the
game is then rebuilt; only the patch's code is compiled).

## Which patches work

A patch made for a retail disc: the US UMD (ULUS-10566) or the EU release (ULES-01505). The two discs hold the same
game files (the code is identical, `PACKAGE.BIN` differs in 3 sectors of region text), so a patch made for either disc
works from both. The patch's base is rebuilt from the install folder (`disc0/`, `EBOOT.enc.bin`) with
`tools/disc_layouts.json`, which records where every file of the two retail discs lies, their MD5s and the 41 KB of ISO
9660 directory sectors, `UMD_DATA.BIN` and the plaintext `~PSP` header of `EBOOT.BIN` (metadata, no game data). The
disc image is not needed (`--iso` reads it instead).

The importer checks every byte it keeps: bytes that came from a part of the base the install folder cannot supply are
tracked through the patch's copies, and a file that needs them is refused (nothing is installed, `--allow-partial`
overrides). Each patch window also carries an adler32; a window that does not match is reported. Patches whose author
started from a slightly edited disc (a rebuilt ISO header, as Emendatus did) fail the first window's checksum while every
file in it is still right; the report says so.

Supported: xdelta3 / VCDIFF with or without LZMA secondary compression (Delta Patcher's default), patches that change
`PACKAGE.BIN` entries, add data after its last entry, change other disc files, change the EBOOT (as a plain, decrypted
ELF, which is how disc patches change code) and the battle / menu / extra overlays, including code appended after an
overlay's end. Not supported: patched PRX modules, a patched EBOOT that is still encrypted, patches for other games or
releases.

## What the mod folder holds

```
<mods>/<name>/mod.json            {"name", "kind": "xdelta", "enabled", "code": [...], "code_stamp", "patch_sha256", ...}
<mods>/<name>/install/<asset>     every PACKAGE.BIN entry the patch changed or added, by path (package_paths.txt) or
                                  by_crc/<crc>.bin: the normal loose-file overrides (docs/loose-file-mods.md)
<mods>/<name>/package_tail.bin    data the patch appended to PACKAGE.BIN after the last entry; its code reads it by
                                  offset, so the runtime serves PACKAGE.BIN bytes past the retail end from this file
<mods>/<name>/usrdir/<PATH>       other changed files under PSP_GAME/USRDIR (upper case), served instead of the disc's
<mods>/<name>/code/               EBOOT.BIN / OVL_*_APP.ELF as patched, eboot.patch (the changed EBOOT bytes)
```

Left out on purpose: the firmware update, icons and `PARAM.SFO`, and movies a patch made *smaller* (patches that add
data shrink a movie to stay within the UMD's size; the PC has the room, so the original is kept).

## Code

A static recompile cannot run patched machine code as is: the patched functions have to be translated too.
`xdelta_mod.py build-code` (run by `pipeline.sh` and `psp_build.py` after the overlays, a few seconds per patched
module) does that with the recompiler the game was built with:

1. the module's retail analysis (`<work>/analysis*.json`) gets the patched bytes; an overlay whose segment grew gets
   the longer segment,
2. jump targets the patched code needs and the analysis lacks are added: a call target, or a jump into code no
   function covers, as a `force_entries` line; a jump into the middle of a function (the usual way a patch returns from
   code it placed elsewhere) as a `force_mid_entries` line,
3. the module is recompiled twice the same way the pipeline does it (`build_overlay.py`, `PSPRECOMP_CROSS_MID=1`),
   from the retail and the patched analysis, under one symbol prefix, and the functions whose C++ differs are kept,
   with their mid-entry wrappers.

All of it goes into one more function set, `<work>/overlays/xmods`, with a table (`dissidia_xmod_sets()`: mod folder,
code stamp, module, address -> function) that `games/dissidia/runtime/xdelta_mods.cpp` reads. The set always exists
(empty without such mods) because the runtime links the table; files are only rewritten when they change.

At start, for every xdelta mod folder that is on (`hooks_io.cpp` `mod_roots`):

- its EBOOT functions are registered in the dispatch table before any other hook, so a runtime hook that wraps one of
  them wraps the mod's version,
- its overlay functions come first in the game module's overlay lookups (`dissidia_ovl_*_lookup`: the resolver and
  every hook that wraps an overlay function), so such a hook wraps the mod's version too,
- its EBOOT byte changes (`code/eboot.patch`) are written to memory at boot (data the patched code reads),
- the overlay files are read from `code/` instead of the disc (`remap_disc_path`, so sizes match too).

A mod whose code is not in the build (imported after the last build, or whose `code/` changed since) is kept off
completely until the next build, with `[XMOD] ... kept off until the game is rebuilt` in the log: its data without its
code would not work. Only one mod's `package_tail.bin` can be served (the first that is on; the log names the others).

Log lines: `[XMOD] <name>: on (N function(s) in M module(s))`, `[XMOD] PACKAGE.BIN bytes 0x... served from ...`,
`[XMOD] .../OVL_MENU_APP.ELF -> <mods>/<name>/code/OVL_MENU_APP.ELF`, `[XMOD] <name>: N EBOOT byte run(s) written`.

## Maintainers

`xdelta_mod.py layout <US iso> <EU iso>` regenerates `disc_layouts.json` (and records, for each file that differs
between the two discs in a few sectors, the other disc's MD5 and those sectors). Self-test without game files:
`python3 -I tools/test_xdelta_mod.py`.
