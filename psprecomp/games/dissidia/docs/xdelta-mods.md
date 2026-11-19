# ISO patch (xdelta) mods

Many Dissidia 012 mods are shared as an `.xdelta` patch for the disc image (made with xdelta3 or Delta Patcher). The
native port installs such a patch as a mod folder: what the patch changes is kept, everything else is read from your
own disc as usual, and *Settings > Mods* turns it off and on (next start).

**In the game:** *Settings > Mods > Install ISO Patch (.xdelta)* opens the system's file picker (it starts in Downloads;
on Android the system document picker), applies the chosen patch to your own game files and installs the result as a
mod folder; it shows up below, on, from the next start. An update of an installed mod replaces it (and keeps it off if
you had turned it off); the same patch twice is recognised. A Dissidia 012 install folder with an extracted disc is all
it needs (no disc image, no PC, no Python). Where no file picker exists (Steam Deck game mode, or
`PSPRECOMP_FILE_DIALOG=none`), put the `.xdelta` in the mods folder and choose the row: the newest one there is
installed. Code the patch changes runs in the interpreter until the game is rebuilt (below), unless the build already
has it (a rebuild with the same patch: the code stamps match).

`games/dissidia/runtime/xdelta_import.cpp` is the C++ twin of the tool below and writes the same folder (same entries,
`package_tail.bin`, `code/`, `eboot.patch`, `code_stamp`): the patch is decoded by `runtime/src/psp_vcdiff.cpp`
(xdelta3's LZMA sections by `runtime/third_party/xz-embedded`), the base disc is rebuilt from `disc0/` (+
`EBOOT.enc.bin` when the folder has it; a phone's does not, and a patched EBOOT needs none) with
`runtime/disc_layouts.inc` (generated from `tools/disc_layouts.json`: `xdelta_mod.py layout-table`), PACKAGE.BIN entries
go to `install/by_crc/`, and `eboot.patch` is computed against the retail EBOOT image compiled into the game. A big
patched file is not copied: only the 64 KB pieces that differ from your own file are kept while the patch runs (Emendatus:
~330 MB for PACKAGE.BIN's appended data instead of a second 1.3 GB file). Emendatus installs in about 7 s on a desktop.
`PSPRECOMP_XMOD_IMPORT=<patch>` installs a patch at start the same way, prints the result and quits (tests).

**From a PC / the build tools:**

```
tools/xdelta_mod.py import Emendatus11.11.26.xdelta --work <workdir>     # -> <workdir>/mods/emendatus
games/dissidia/tools/pipeline.sh <workdir>                                # compiles the patch's code (optional)
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
releases. Code the patch loads at run time is supported as described below (it needs one rebuild after the first run).

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

A mod whose code is not in the build (imported after the last build, or whose `code/` changed since) runs its changed
code in the interpreter (`[XMOD] <name>: its code changes are not compiled in this build: interpreted until the next
build`): the game compares memory with what its compiled code was made from and interprets every function that differs. Only one mod's `package_tail.bin` can be served (the first that is on; the log names the others).

Log lines: `[XMOD] <name>: on (N function(s) in M module(s))`, `[XMOD] PACKAGE.BIN bytes 0x... served from ...`,
`[XMOD] .../OVL_MENU_APP.ELF -> <mods>/<name>/code/OVL_MENU_APP.ELF`, `[XMOD] <name>: N EBOOT byte run(s) written`.

## Code a mod loads while the game runs

Some patches go further: they carry extra machine code in their data, read it into the heap while the game runs and
then rewrite game instructions to jump into it. Emendatus does: the EBOOT reads 16 KB from its appended data at boot
(save-data hooks), the menu overlay reads 32 KB when it loads (costumes and more) and that code rewrites 63 game
instructions across the EBOOT and the three overlays, some of them only for as long as it needs them. A static
recompile has seen none of it. It is handled in two steps:

1. **First run (learning).** `hooks_io.cpp` reports every read from `package_tail.bin` and where it went in memory. A
   call into such a range that nothing compiled answers runs in a small interpreter (`runtime/xmod_interp.cpp`: the
   integer and FPU instructions with delay slots; calls go through the dispatch table, so the loaded code calls the
   game's compiled functions). When that first call returns, the code as it is then and every game `j`/`jal` that now
   leads into it are written to `<mod>/code/dynamic.txt` + `dyn_<addr>_<offset>.bin`; while the code keeps running
   the game is rescanned every 2 s and when another overlay is loaded, for instructions rewritten later. The mod works
   in this run, except that the game does not yet follow the instructions the code rewrote. Settings > Mods shows
   "rebuild once" for it (`<mod>/status.txt`).
2. **Next build.** `xdelta_code.py` compiles each recorded block at its address, every function and mid-entry of it
   behind a check that memory still holds the bytes it was compiled from (else it is interpreted: code that rewrites
   itself, like its trampolines). Every rewritten game instruction gets a guard in the compiled game function: when the
   word in memory is no longer the original, the jump that is there now is followed, delay slot included
   (`dissidia_xmod_site`), exactly as the PSP would; when the code puts the original back, the original runs. The
   block's jumps back into the game (into the middle of a hooked function) become mid-entries. The compiled block
   switches on when the code is read to the recorded address again (the game's allocations are deterministic); read
   anywhere else it is interpreted and recorded again.

`PSPRECOMP_XMOD_DYN_SKIP=4` (or `4@0B330000` for one block) keeps the compiled blocks off, `PSPRECOMP_XMOD_TRACE=N`
logs the first N calls into them, `PSPRECOMP_SD_TRACE=1` logs every save-data call, `PSPRECOMP_XMOD_ABI=1` logs every
interpreted function that returns with a callee-saved register or `$sp` changed, and every followed rewritten jump.

Recompiler fixes this needed (the retail game's output does not change):

- a link branch (`bal`, `bltzal`, `bgezal`) to another function was emitted as a tail call followed by `return`,
  skipping the rest of the caller. It is a call now (psp-emitter `emit_link_branch`).
- a call (`jal`, `jalr`, a link branch to another function) now writes the return address to `$ra` (`jalr`: its rd)
  as the CPU does (psp-emitter `emit_link_write`). Compiled code returns by C++ return and never needed it, but a
  mod's hook may test who called it: Emendatus's menu hook at 0x0889F290 compares `$ra` with 0x088D53FC.
- a `jr` through another register to the address the function was called from is a return (`move s4, ra` ...
  `jr s4`, as Emendatus's battle code does); it was a lookup of a return address, which no function answers. The
  interpreter does the same. 80 retail functions get the check; it only fires where the old code missed.
- a mid-entry at a delay slot's address (code a patch adds jumps back to the word after the `j` it wrote over a
  function's first instruction) is decoded like a branch into a delay slot (issue #56): entering there runs that
  instruction and goes on, instead of re-running the jump (endless recursion into the hook). One retail function
  changes the same way (0x08882FF4, entry 0x08883018 in the delay slot of its `jr ra`).
- `bal` to the next instruction but one (reading the PC) is not a function entry for code loaded at run time, and no
  entry of such code is put in a delay slot.
- a link branch whose target is a mid-entry of the same function (the start of a subroutine the sibling coalescer
  merged into it, `PSPRECOMP_CROSS_MID`) is a call, not a branch: as a branch the subroutine's `jr ra` returned from
  the whole function. Emendatus's timeline wrapper (plugin block + 0x3FD8, which the patch puts in front of the
  game's timeline runner 0x08828E94) calls its helper at + 0x42C4 with `bal`; the helper's return ended the wrapper,
  so no timeline of Aerith's ever ran (no "may move" flag, no hit, no recovery: she stood still and ignored the pad
  and the AI, while her forced actions played).
- after a link branch's callee comes back, the caller resumes only if `$ra` still holds the link address; otherwise
  it returns too. `bal` is also used to jump over a table and read its address (Emendatus block + 0x54F0:
  `bal 1f; nop; .word ids...; 1: ... (ra)`): that target leaves with its caller's return address and the code after
  the delay slot is data. The interpreter does the same.
- the prologue scan no longer starts a function in a branch's delay slot (`addiu sp, sp, -N` right after a branch).
  Such a split cut the delay slot off the branch at the end of the previous function: code a patch adds is not in the
  retail analysis, so the scan is what divides it into functions.

## Saves and cloud sync

Code a mod runs can write a save of its own. Emendatus keeps the entries it adds (which the game's own save cannot
hold) in a second save next to the game's: `<disc id>Ext<slot>` (e.g. `ULUS10566ExtGameData00`) with `EMSX.BIN`,
saved and loaded together with the game's save by its hooks on `sceUtilitySavedataInitStart` / `ShutdownStart`. It is
an ordinary encrypted save: the save list, the save manager and cloud sync treat it like the game's. Save scans
(`psp_savemgr::scan`, used by cloud sync and the import screen) take every save of a disc ID that has a Dissidia 012
save, whatever its title, so a mod's save travels between devices and regions with the game's (renamed to the
installed disc ID and re-signed); `test_cloudsync` covers it.

The plugin derives its save's name from the name the game's save request holds after the list dialog. The PSP writes
the chosen name back into the request; the runtime does that now also when no list is shown
(`PSPRECOMP_SAVEDATA_AUTO`, the test harness).

## Maintainers

`xdelta_mod.py layout <US iso> <EU iso>` regenerates `disc_layouts.json` (and records, for each file that differs
between the two discs in a few sectors, the other disc's MD5 and those sectors). Self-test without game files:
`python3 -I tools/test_xdelta_mod.py`; cloud sync of a mod's save: `test_cloudsync`.
