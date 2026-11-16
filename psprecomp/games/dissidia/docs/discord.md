# Discord Rich Presence

With the Discord desktop app running on the same computer, your Discord profile shows what you are doing in the game:

```
Playing Dissidia 012 Duodecim
Quick Battle (1-on-1) · Order's Sanctuary
Squall vs. Kefka                (2 of 2)
03:12 elapsed
[ Dissidia Recomp on GitHub ]
```

| Where you are | First line | Second line | Timer |
|---|---|---|---|
| Title screen | Title Screen | | since the game started |
| Start menu (and its Battle Mode / Communications lists) | Main Menu | | since the game started |
| After choosing a mode | the mode: Story Mode, Quick Battle (1-on-1), Quick Battle (Party), Arcade Mode, Labyrinth, Communications Mode, PP Catalog, Collection, Data Config, Options | | since the game started |
| In a fight | the mode · the stage | your fighter vs. the opponent (manikins by their own names: Transient Lion, ...) | since the fight started |
| In a Communications Mode group (lobby, room) | Communications Mode | With Zack (or "With Zack, Cloud and 2 others") plus "(n of n)" | as above |
| One-on-one against another player | Communications Mode · the stage | Ender (Lightning) vs. Zack (Laguna) | since the fight started |

The large picture is the game's icon; hovering it shows the Dissidia Recomp version, or in a group every player's name ("Players: Ender
and Zack"). Players are named by the player name of their save (Data Config > Player Name): each copy of the game sends it to the
others as its ad-hoc nickname (it was "Player" before), so the game's own lists of nearby players show it too. Everyone in one room gets
the same Discord party, so Discord shows them together. Your Discord name is never sent to other players. Characters added with `roster.txt` show their
`nm=` name (else their code). Stages without a confirmed name (labyrinth floors, a few story-only arenas) are left out.

## Turning it off

Settings > Network > **Discord Status** (On / Off, saved as `discord=` in `settings.ini`). The **Discord** row under it says whether the
app was found ("Connected as ...", "Discord is not running", ...). Turning it off clears what Discord shows at once. Nothing is sent
anywhere but to the Discord app on this computer, and Discord only shows it while the game runs.

| Option | Effect |
|---|---|
| `PSPRECOMP_DISCORD=0` | off (the saved setting wins) |
| `PSPRECOMP_DISCORD_APP_ID=<id>` | another Discord application (its name is the "Playing ..." line) |
| `PSPRECOMP_DISCORD_LOG=1` | log every message to and from Discord |
| `PSPRECOMP_DISCORD_IPC=<path>` | use this socket / pipe instead of searching (tests) |

Not on Android: the Discord phone app has no local connection for games. On a Steam Deck it works in Desktop Mode with Discord running;
in Game Mode only if Discord runs in the background.

## How it works

`runtime/src/psp_discord.cpp` (generic, no Discord SDK) talks to the desktop app over its local IPC: a Unix socket `discord-ipc-0..9`
in `$XDG_RUNTIME_DIR`, `$TMPDIR` or `/tmp` (also the Flatpak, Snap and Vesktop sub-folders), or the named pipe
`\\.\pipe\discord-ipc-0..9` on Windows. Frames are `{u32 opcode, u32 length, JSON}`: a handshake with the application id, then
`SET_ACTIVITY` commands; pings are answered. One background thread owns the connection. Once a second it asks the game module what to
show and sends it when it changed, at most once every 4 s (Discord drops more than 5 updates in 20 s; the newest state always goes out).
If Discord is not running, quits or restarts, the thread looks again after 5 s, doubling up to 60 s; the game never waits for it. On quit
and when turned off the activity is cleared.

`games/dissidia/runtime/discord_presence.cpp` decides what to show:

- **Menus.** `menu_settings.cpp` reports every frame the title screen, the start menu (`ovl_menu` FUN_089E45E8; cursor at state +0xA28
  +4) or its sub-list (FUN_089E4A60; group at state +0xA88, cursor at +0xA58 +4) updates. The rows they were on when the start menu was
  left are the chosen mode, kept until the start menu shows again.
- **Fights.** `framerate.cpp` keeps the fighters on stage; each fighter's object record (actor +0x2F8) gives its charId (+6) and model
  type (+8, 5 = manikin), and the brain (actor +0x5C4, +0x54 != 0) tells which one a person drives. charIds follow the EBOOT code
  table at 0x089D6CA8 (one100 .. sev120). The stage is `psp_stage_id()` (the battle setup word, hooks_main.cpp), named from the game's
  own text (`text/en/mess_pk_loading_0.bin` entry 3).
- **Online.** The save's player name is UTF-16 at 0x08BCEB40 (EBOOT BSS; found by changing it in RAM and watching Data Config follow,
  the copy at 0x08BDC4BC is a stale duplicate). The start menu hands it to `psp_adhoc_set_nickname` (every Communications session starts
  there; `PSPRECOMP_ADHOC_NICK` still wins). `psp_adhoc_group_size()` / `psp_adhoc_group_nicks()` give the other players, and
  `psp_adhoc_group_key()` (room + the game's group name) a party id everyone in the room shares. Which fighter is yours: actor +0x5B0
  bit 0x04 (…35 against …31; found by diffing two copies in one Group Battle, the same in single player). The brain's human mode is set
  for the other players' fighters too, so it is only the fallback.

Test: `runtime/tests/test_discord.cpp` (ctest `discord`) checks the JSON and runs a whole session against a fake Discord app on a Unix
socket: handshake, first activity, rate limit, ping, off / on, Discord restarting, clearing on quit.
