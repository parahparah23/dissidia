# Related projects: what we can learn from them

Surveyed 18 November 2026. Three other Dissidia 012 projects, what they have that this project does not, and what is
worth taking. Addresses in their notes are for the USA release (ULUS10566), but the main executable and the overlays
load at the same addresses as ours, so they can be used directly (checked: their `0x08872138` is our
`FUN_08872138`, the pad dead-zone routine; their `0x0889F234` is our BGM playback `FUN_0889F234`).

## Credits and licences

- **[VCKing101/Dissidia012-pc](https://github.com/VCKing101/Dissidia012-pc)**, by VCKing101. A native port built
  on [PortableKit](https://github.com/TeamGDB/PortableKit), plus a matching decompilation. MIT licence (copyright
  2026 VCKing101) for its port code, tools and docs. Its `NOTICE.md` says the MIT licence does **not** cover the
  decompiled C in `src/`, `include/` and `port/decomp/`. So: research facts and designs from its docs can be
  used with credit. Code from `port/host/` or `tools/` may be copied only with the MIT notice kept. Do not copy the
  decompiled C. Write our own from their notes.
- **[RocherBrockas/DissidiaDecompProject](https://github.com/RocherBrockas/DissidiaDecompProject)**, by Pierre
  Brossy. An early decompilation setup (splat, Ghidra scripts, a `PACKAGE.BIN` extractor, per-format notes). MIT.
  Some format notes in it are credited to Sky Noxt.
- **[RecompLauncher](https://recomplauncher.com/game/dissidia012-pc)**: a launcher that lists recomps from their
  GitHub repos, installs them, and runs their setup from the player's disc image. VCKing101's port is listed there.

Anything added to this project from these sources must name the source in the commit message and in the doc or code
comment it lands in. Use a link to the repo and the file it came from.

## Worth taking, most useful first

### 1. Analogue walking: the direction is still 8-way (bug in ours)

**Done** for the world map and battle (18-19 November 2026, `runtime/input.cpp`, [native-build.md](native-build.md)); battle replays
may play back a little differently. The camera follow dead zone below is done too (same file).

Source: [docs/port/analog-movement.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/analog-movement.md).

Our `input.cpp` replaces the stick vector that `FUN_08872138` writes. Their notes show that **nothing in the walk
reads that vector**. The walker's input is `ovl_battle 0x08B38A0C`, called once a frame from `0x08B36D2C` with
`(out, walker, pad, camera)`. It builds a camera-space step from the four "walk" bits only (+-1 on x and z), so every
walk direction is one of eight. Lowering the per-axis dead zone (`PSPRECOMP_ANALOG_DPAD_ZONE`) changes when a bit
appears, not the angle. This applies on the world map, and in battle too: the fighter under the player's pad calls the
same function.

Their fix: override `0x08B38A0C` and, when the walk bits come from the stick (the "move with analog stick" option, pad
table B at `0x08B7F380`), use the real stick direction in camera space. Set the walker's direction flags at
`walker+0x5B0` as the 8-way bits would be (each axis within 67.5 degrees). Leave the rest as the game does it. The walk
speed stays the same, because the output is a unit direction.

Two cautions from them:
- **Replays.** A battle replay records only the 8-way bits (`0x08889628` -> `0x088CA680`, 15 bits a frame). Analogue
  steering in battle makes a saved replay play back differently. They keep battle 8-way by default. Netplay is safe
  if both machines are fed the same bytes.
- **World map camera follow.** The free camera's follow (`ovl_battle 0x08B3F7F4`, the end at `0x08B3FAA4..0x08B3FC08`)
  turns toward the walker's facing by up to 0.05 rad per frame, with no dead zone. With true angles, a push slightly
  off straight up makes the camera turn and the walker veer. They added a 22.5 degree dead zone to the follow.

### 2. Online battles: replacing the game's per-frame exchange

Source: [docs/port/communication-mode.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/communication-mode.md)
(sections 4 and 11 onward) and [online-next.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/online-next.md) section 5.

Our multiplayer runs the game's own lock-step over our ad-hoc emulation. Their notes on that lock-step:
- All of it is one function, the role-0 handler `0x0890D664` (frame pump `0x088EC010 -> 0x0890E794 -> 0x08907F14 ->
  0x0890C5BC`). Spectators use `0x0890DF58`.
- It sends a 16-bit word a frame: 12 buttons plus 4 stick-direction bits. So online fighters get only the 8-way stick
  and no right stick. Input delay is 0 to 2 frames, it grows and never shrinks, and the battle freezes on every
  late frame past about 100 ms round trip. After 900 waiting frames the session ends.
- Packet: 0x24 bytes, the last 8 pad words, a desync hash every 10 frames (`0x0890CD90`; the hash callback is battle
  `0x08B136D4`).
- Both machines simulate both fighters and both cameras from the two pads, so full analogue input can go in the raw
  pads' injected samples (`+0x40..+0x4F`) as long as both machines inject the same bytes.

They replaced `0x0890D664` during battles with their own UDP lock-step: full analogue, both right sticks (bytes 10 and
11 of the sample), a set input delay, redundancy, and the game's own check value compared every 10 frames. They then
added **rollback**. Their measurements: dirty-page snapshots are about 2 MiB a frame, and 2 to 3 frames of rollback on
top of 1 frame of delay covers about 100 to 130 ms without a stall. This is the main gap in our
[multiplayer.md](multiplayer.md) ("input delay or rollback for high latency" is listed there as missing).

### 3. Japanese voices: story scene scripts

Source: [docs/port/japanese-voices.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/japanese-voices.md), "Story scenes".

Our `jp_audio.py` swaps audio files only. They found that **302 voiced story scenes** (`talkevent/*.evex`, ARC
archives) have different scripts on the English and Japanese discs. 55 have other voice lists and 247 have other
waits, because each line waits a fixed time tuned to its own recording. With English scripts and Japanese audio, lines
get cut off or the wrong line plays (their example: Vaan and Terra, 012 Chapter 2, `up02_103`). Their fix is to read
each of those scenes as the Japanese disc has it, keeping the English text. The English game knows every script
command of the Japanese scripts except `0x157` and `0x162`. They also keep the English movie picture with the
Japanese sound, where we swap the whole PMF.

Their trick for pairing files without names: the `PACKAGE_INFO.BIN` hash is CRC-32/BZIP2, which is linear. Two names
of the same length that differ only in `jp`/`en` differ by a constant that depends only on how many characters follow,
so `hash XOR delta(n)` finds the English twin of every Japanese file (6,222 of 6,354 sound files pair one to one).

### 4. Features we do not have

| Feature | Their doc | Notes for us |
| --- | --- | --- |
| BGM picker on the loading screen (series left/right, track wheel up/down) and music mods in MP3/Ogg/Opus/FLAC/WAV/BRSTM with loop points, no disc change | [bgm-playlist-design.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/bgm-playlist-design.md), [mod-folder.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/mod-folder.md) | Ours needs AT3 encoding and a `main.bin` rewrite ([audio.md](audio.md)). Their facts: the table object `*0x089D6C30` has room for 256 rows; DLC rows are added at run time by `0x0889F040`; the selector is battle `0x08AF8CA4`, struct at loading screen `+0x6AC`. |
| Local split-screen versus (1v1 and party), each player with a controller and camera | [local-versus.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/local-versus.md) | Large job (one game drawn twice). Their notes on player 2's input path also serve netplay. |
| Cheats page: built-in cheats plus PPSSPP CWCheat files (`PSP/Cheats/ULUS10566.ini`) | [cheats.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/cheats.md) | Their CWCheat engine is in PortableKit (MIT). |
| Button prompts drawn as Xbox / PlayStation / Nintendo / keyboard, following the controller in use | [button-prompts.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/button-prompts.md) | Also changes the prompts inside hint text. |
| L/R on the triggers option; per-player controller choice; one key each for dodge, dash, EX Mode, summon and assists | [keyboard-controls.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/keyboard-controls.md), [controller-picker.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/controller-picker.md) | Small. |
| Save, message and keyboard dialogs drawn in the game's own menu style with its sounds | [native-dialogs.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/native-dialogs.md) | Cosmetic. |
| Listing on RecompLauncher | [recomplauncher.json](https://github.com/VCKing101/Dissidia012-pc/blob/main/recomplauncher.json) | Needs a portable zip and a no-window `--prepare --game {game}` step; the manifest describes the ISO check, an ini settings file, and the save, mod and log folders. |

### 5. Event names

Source: [docs/research/moveset-format.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/research/moveset-format.md), section 6.

They list all 125 ranged and 56 keyed event handlers. Compared with the names in `tools/charmod_objx.py`, these codes
have names there and not here (uses / fighters in brackets):
- Ranged: `0x3C brave_regen` (Gabranth), `0x51 armor` (Kain, Chaos), `0x17 swing_reset` (Sephiroth, Bartz, Firion),
  `0x6B sound_ramp`, `0x1A actionable_ex` (Feral Chaos).
- Unused by any retail move, but working handlers that new moves could use: `0x0D freeze_state`, `0x0E hold_target`,
  `0x41 break_lock_ons`, `0x44 tint`, `0x4E shape_off`, plus camera paths `0x5C`/`0x6F` (partly understood).
- Keyed: `0x17 cart_effect` (Laguna, Squall).

Check each in the game code before adding it to the table: their codes are the event type, ours are
`(arg count << 8) | type`.

### 6. Smaller facts

- `PACKAGE_INFO.BIN` is read into a fixed buffer of `0x20F8C` bytes (`0x08871938`), room for only 210 more records
  ([japanese-voices.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/port/japanese-voices.md)).
- The pad manager object map (`0x08C15B90`), its three pad arrays, the remap tables and the injected / replay /
  network input paths: [research/pad-module.md](https://github.com/VCKing101/Dissidia012-pc/blob/main/docs/research/pad-module.md).
- `DES4` files are stage geometry: collision meshes, transforms of breakable objects, EX core positions and fighter
  start and end positions (Sky Noxt, in DissidiaDecompProject `Extract/des4/README.md`).

## Already covered here

- The text cipher (DissidiaDecompProject `Tools/MiscTools`): our `tools/mess_text.py` already reads and writes it,
  including the OFF4 layout.
- The `PACKAGE.BIN` extractor and builder: `tools/package_extract.py` and the loose-file layer do more.
- Right-stick camera, analogue dead zone, 60+ fps, HD textures, Discord status, Japanese voices, internet play,
  cloud saves: all here already. Our frame rates (up to 240) and character modding go further than theirs.
- Their quit crash (a descriptor pool destroyed before sets were freed from it) does not apply. Our Vulkan code
  never frees single descriptor sets.
