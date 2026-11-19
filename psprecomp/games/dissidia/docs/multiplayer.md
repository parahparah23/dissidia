# Multiplayer (ad-hoc wireless over UDP)

Dissidia 012's "Communications" mode was written for two or more PSPs talking over ad-hoc WiFi. The native build
emulates that layer on top of UDP, so players on the same network, on one machine, or across the internet can play
each other. It is **experimental**: a full match has been played between two instances on one machine, and nothing
has been tested with more than two players, with real WiFi/LAN hardware between machines, or across the internet.

The code is generic runtime code (`runtime/src/hle/psp_hle_adhoc.cpp`), not Dissidia-specific. The only game-specific
part is the frame-rate rule in `runtime/framerate.cpp`.

## Playing

Open Communications mode in the menu and challenge a player as on a PSP. Nothing else needs turning on: the network
layer starts the first time the game uses it. Players need the same game version and character set (do not mix a
modded roster with an unmodded one). Packs listed below are the exception: they are sent to players who lack them.

### Internet (the default)

By default the game connects to a **party server**, `party.schindlershadow.com`, which relays the game traffic
between players. Nobody has to forward a port or know anyone's address.

- Players in the same **room** see each other in the lobby. The default is the public room, where everyone using the
  server is. To play with friends only, all of you set the same room name (any name only you know).
- The settings page is **Settings (Esc / F1 / Guide) > Network** (L / R shoulder, Q / E or Tab switch pages):

  | Row | Meaning |
  |---|---|
  | Network | **Internet** (relay server, default) or **LAN** |
  | Server | the relay: a host name or `host:port` (default `party.schindlershadow.com:27500`); Cross edits |
  | Room | blank = the public room; Cross edits |
  | Direct Peers | LAN only: extra addresses to contact directly; Cross edits |
  | Status | `Connected, 3 online`, `No answer from the server yet`, `Cannot find ...`, `LAN: 2 nearby` |

  Editing text: type on a keyboard (Enter saves, Esc cancels), or with a controller pick a letter with left / right,
  add it with Cross, delete with Circle, and save with Start or the Done row. Changes apply at once and are saved
  in `settings.ini` (`net_mode`, `net_server`, `net_room`, `net_peers`).
- Everything you send goes through the server, and the public room shows your player name to whoever is in it. The
  server never looks inside a game message and does not log them, but it has no accounts and no encryption: use a
  private room name for friends.

### LAN

Set Network to **LAN**. Players on the same network find each other by broadcast and show up in the lobby by name
(some routers and Wi-Fi isolation block broadcast; list the other player under Direct Peers then). No server is
involved, so it has the lowest latency.

Two copies on one machine (for testing) need LAN mode and separate work folders, because the game saves into its
folder:

```
cp -a ~/dissidia/work ~/dissidia/work2        # or symlink everything but ms0/, settings.ini
PSPRECOMP_ADHOC_MODE=lan PSPRECOMP_ADHOC_NICK=A tools/run.sh ~/dissidia/work  --window 960x544 &
PSPRECOMP_ADHOC_MODE=lan PSPRECOMP_ADHOC_NICK=B PSPRECOMP_DEBUG_PORT=10000 tools/run.sh ~/dissidia/work2 --window 960x544 &
```

The copies find each other over `127.0.0.1`: each takes the first free UDP port from 27312. Controllers only act on the
window that has focus (set `PSPRECOMP_PAD_BACKGROUND=1` to listen while unfocused). To test through a relay instead,
use `PSPRECOMP_ADHOC_MODE=internet PSPRECOMP_ADHOC_SERVER=127.0.0.1:27500`.

### Direct internet play without the server

In LAN mode a player can also be given an address directly (`PSPRECOMP_ADHOC_PEERS=friend.example.org`, or Direct
Peers in the menu). That needs UDP port 27312 reachable on both sides (forward it, or use a VPN such as ZeroTier,
Tailscale or WireGuard). Both players should list each other; a third and later player needs only one address,
because beacons carry the addresses of the peers each player hears (unknown addresses are probed for 15 s and dropped
if they do not answer).

**Expect latency to matter.** The game runs a lock-step battle over reliable streams, and nothing here delays or
predicts input. It was built for a few milliseconds of WiFi latency. A relay adds the trip to the server on top of the
direct path; how much round-trip time the game tolerates has not been measured, so try a nearby partner first.

## Settings menu

The in-game settings menu (Esc / F1, Guide) pauses the game, which would stall the other players, so it does not open while you are in a group with others: a message says so and the game carries on. Change settings before joining a room.

## Battles: our pad link

A 1-on-1 fight nobody watches runs on our own pad link (`games/dissidia/runtime/comm_battle.cpp`) instead of the game's exchange. The
game's exchange (`FUN_0890D664`) sends a 16-bit word a frame (12 buttons and 4 stick directions after its dead zone), so online
fighters only moved 8 ways; its input delay grows to 2 frames and never shrinks, and every late word freezes the fight. The link
replaces that function while the fight runs: each frame this copy's whole pad (buttons, stick at full resolution, turned by this copy's
free camera before sending) for frame f + delay goes over the ad-hoc side channel (type byte `B`), with this copy's last 8 frames in
every packet, so a lost packet is covered by the next. The other copy's pad for frame f is injected next to ours, and the analogue walk
works online. Every tenth frame the game's own check value (`FUN_0890CD90`) is compared; a difference ends the fight with the game's
error. When a pad is late the game waits that frame, as before, so the game's own timeout still ends a fight whose peer is gone (and
the link ends it after 10 s with nothing heard). The lobby, setup, results and Group Battle fights with watchers stay the game's.

Both copies announce the link at the fight's first frame; one that hears nothing within 5 s (an older build, or `PSPRECOMP_NET_LINK=0`)
keeps the game's exchange for that fight, so mixed versions still play. Injected sticks are never turned by the receiving copy's camera
and, while the link runs, are processed with the default stick constants whatever the local settings, so both copies compute the same
fight.

| Variable | Meaning |
|---|---|
| `PSPRECOMP_NET_LINK=0` | the game's own exchange |
| `PSPRECOMP_NET_DELAY=<frames>` | input delay, default 2 (about 67 ms; covers a round trip up to about 130 ms without waiting), 1 to 8 |
| `PSPRECOMP_NET_LINK_LOG=1` | log the link (linked, every 900 frames: checks agreed, frames waited, how a fight ended) |
| `PSPRECOMP_NET_LINK_DROP=<percent>` | drop that share of the pad packets sent (tests) |

Tested 19 Nov 2026, two copies on one machine (LAN, Online Lobby 1-on-1, Lightning vs Lightning): linked, fights in sync with both
attacking (same Bravery and HP on both screens), 1,800 frames with no wait; copy A at 60 fps and copy B at 240 with 30 % of the pad
packets dropped: 79 checks agreed by frame 900, 16 frames waited; a copy with `PSPRECOMP_NET_LINK=0` against one with the link: the
link gave up after 5 s and the fight ran on the game's exchange. Not tested yet: two machines, through the relay, real latency.
Design from VCKing101's port (https://github.com/VCKing101/Dissidia012-pc, `docs/port/communication-mode.md`, MIT); our own code.

## Frame rate

Every player's wanted frame rate (from the settings menu) is shared in the beacons. When every player in a group asked for more
than 30, each keeps their own rate: above 30 a fight's logic runs at 30 Hz (30 Hz logic) and only the frames between are drawn
(tested: 60 against 240 fps, in sync). If anyone asked for 30 the game runs at the **lowest rate any of them asked for**, so a
60 fps player and a 30 fps player both run at 30 (plain 30 fps is not bit-identical to 30 Hz logic in every move).
`PSPRECOMP_NET_SAME_FPS=1` always uses the lowest rate. It returns to the player's own setting when the group ends. A message at the top of the screen tells you the rate for five seconds ("Online: 30 fps (lowest player's setting)"), and again when the group ends. This logic is in
`runtime/framerate.cpp` (`psp_adhoc_group_fps_cap()`).

Above 30 fps a fight runs on 30 Hz logic ([frame-rate.md](frame-rate.md)): the game advances on one displayed frame in two, three or
four. Its network manager task (`FUN_0890E794`, which runs the battle's per-frame pad exchange `FUN_0890D664`) only runs on those logic
frames. Until 19 November 2026 it ran on every displayed frame; on the in-between frames the other copy's pad for the next frame was
not there yet, the exchange counted a waiting frame, and after the game's 900 waiting frames it ended the session without a message:
about 10 s into every online fight at 120 fps (two copies on one machine, LAN; 30 fps was fine). `PSPRECOMP_L30_NET_EVERY_FRAME=1`
brings the old behaviour back for tests.

## Options

| Variable | Meaning |
|---|---|
| `PSPRECOMP_ADHOC=0` | turn the emulation off (the old no-op stubs return, Communications mode then cannot connect) |
| `PSPRECOMP_ADHOC_MODE=lan\|internet` | how players find each other; default `internet` (`lan` if `PSPRECOMP_ADHOC_PEERS` is set and no mode is given). Saved settings win |
| `PSPRECOMP_ADHOC_SERVER=host[:port]` | the relay for Internet mode (default `party.schindlershadow.com:27500`) |
| `PSPRECOMP_ADHOC_ROOM=<name>` | room on the relay; empty = public |
| `PSPRECOMP_ADHOC_PORT=<n>` | LAN mode: first UDP port (default 27312) |
| `PSPRECOMP_ADHOC_SPAN=<n>` | how many consecutive ports are tried and probed (default 16, up to 64) |
| `PSPRECOMP_ADHOC_PEERS=a,b` | LAN mode: extra peers to contact directly: `host` or `host:port` (a bare host probes the whole port span) |
| `PSPRECOMP_ADHOC_NICK=<name>` | nickname other adhocctl clients see (default: the save's player name once a save is loaded, else "Player"; Discord status names players by it) |
| `PSPRECOMP_ADHOC_MAC=aa:bb:cc:dd:ee:ff` | virtual MAC; default is random per launch (a host-name hash collides: every Steam Deck is called `steamdeck`) |
| `PSPRECOMP_ADHOC_LOG=1` | log every call and event with arguments, results and the first bytes of each message |
| `PSPRECOMP_PAD_BACKGROUND=1` | let controllers act while the window is unfocused |

Log lines start with `[ADHOC]` in `last_run.log`.

## How it works

The game uses four layers of the PSP network API; each is emulated:

| Layer | Calls | Emulation |
|---|---|---|
| Wlan / net helpers | `sceWlanGetEtherAddr`, `sceNetGetLocalEtherAddr`, `sceNetEtherNtostr` | a virtual MAC per instance |
| Ad-hoc control (adhocctl) | `Init/Term/AddHandler/DelHandler`, `Create/Connect/Join/Disconnect/Scan`, `GetScanInfo/GetPeerList/GetPeerInfo` | a "group" is a name; peers advertise their group in beacons. Events (connect, disconnect, scan) are queued and delivered to the game's handlers |
| Matching | `MatchingInit/Create/Start/Stop/Delete`, `SelectTarget/CancelTarget/SendData/SetHelloOpt` | the lobby state machine: hello, request, accept, established, leave, cancel, data and ping/timeout, as events on the game's callback |
| PDP / PTP | `PdpCreate/Send/Recv`, `PtpListen/Open/Connect/Accept/Send/Recv/Flush/Close` | PDP is a datagram per call; PTP is a TCP-like reliable stream (connection ids, go-back-N retransmission, partial sends) |

Messages larger than 1.3 KB (the lobby hello is 4.7 KB) are split into 1.1 KB `T_FRAG` packets and reassembled by the receiver, because IP-fragmented datagrams are dropped by many routers (a home router's loop-back NAT did exactly that). Transport: one UDP socket and one network thread. In LAN mode the socket takes the first free port from 27312 and sends to the LAN, loopback and listed peers; in Internet mode every packet goes to the relay server. Packets start with `DSAH`, a type, the sender's MAC and a body
length. Types: beacon (nickname, group, wanted frame rate, known peers), PDP, matching, PTP. Beacons go out twice a
second to the local ports, the LAN broadcast address and any listed peers; a peer is dropped after 4 s of silence.

Guest callbacks never run on the network thread. They are queued and run on a game thread, from
`sceKernelCheckCallback` and the vblank wait (`psp_adhoc_pump`), because they call back into recompiled code.

Details worth knowing when changing it:

- The game checks every peer with `sceNetAdhocctlGetPeerInfo` and drops anyone whose `last_recv` (game clock) is older
  than 20 s, so the field is filled from the beacon time.
- After the PTP handshake the game keeps opening new sockets on the same ports whenever a connect fails. A SYN with no
  listener must be refused (RST) so it retries, and every packet carries a connection id so replies reach the right
  socket.
- `sceNetAdhocPtpSend` is `send()`-like: it queues what fits in the socket's buffer and writes the count into `*size`
  (0 on would-block). The game adds that count to what it has sent.
- Struct layouts follow PPSSPP: scan info 28 bytes, peer info 152 bytes, PDP stat 20, PTP stat 36 (the last two are
  unchecked against the game).

## Mods that change a character (verified packs)

A player who uses a known pack (today: Bartz's Dissidia 1 moves, `mods/bartz-dff`) can play online with players who
never installed it: as soon as both are in the same lobby, the other game fetches what it needs from that player
(about 760 KB: the moveset, the models and the voice bank, as deltas against the retail 012 files every player has),
caches it in `<mods>/.net_cache/`, and uses it for that session. A player's own characters are not changed by it.
Only the player who wants the moves needs the Dissidia 1 disc.

Nothing a peer sends is trusted. The packs a game accepts are compiled into it (`games/dissidia/net_packs.json` ->
`runtime/net_packs.inc`, written by `tools/netpack.py record`): every file with its size and SHA-256, the SHA-256 of the
retail file the delta applies to, and the abilities. The receiving game applies the delta to its own retail file
(`runtime/net_delta.h`: every copy bounds-checked, output capped at the listed size, inflate capped; host test
`tests/run_net_delta_test.sh`, run under AddressSanitizer with corrupted and hostile deltas), keeps the result only if
size and SHA-256 match, stores it under that hash (never a name a peer sent), and builds the ability records itself
from the list. So a peer can make another game use exactly the listed files and abilities, or nothing.

Who may start a match (`net_packs.cpp` `allow_match`, through the ad-hoc layer's match filter): a player who uses a
pack this game does not know (another version, an edited mod, a build that is not the listed one) or has not been
received and verified yet is refused, and a player who uses a pack only plays with players who confirmed having it. A
refused player is not shown in the lobby. Two players without packs play as before.

Transport: a side channel of the ad-hoc layer (PDP to the reserved port 0xD1D1, `psp_adhoc_side_*`), so the party
server needs no change. Messages: a manifest every second (packs in use, packs ready), requests and 1 KB pieces of a
delta (24 in flight, resent after 0.4 s, given up after 90 s). A sender serves only the deltas `netpack.py deltas`
wrote (run by `charmod.py build`) for packs whose files it has installed byte for byte (`<mods>/.net/<sha256>.dlt`).

Tested 16 Nov 2026 with two instances in LAN mode: A with the pack and Ascendant Lance / Soul Eater equipped, B with an
empty mods folder. B fetched and verified all six files in the lobby within seconds, the match started, A picked Bartz
and B Lightning, and both screens showed the same Brave and HP values through Ascendant Lance and Soul Eater. With
one of A's deltas altered (valid format, different bytes), B threw that file away and the two players no longer saw
each other in the lobby. Not done yet: an on-screen note telling a refused player why.

Adding a pack: build the mod, `netpack.py record <mods dir> <folder> --version N` (updates the JSON and the .inc), rebuild
the game. The recorded hashes must be what `d1_moves.py` / the mod build makes on every machine (they are deterministic
for the same discs: rebuilding from scratch gave identical files).

## The party server

`tools/adhoc_relay/` ([README](../../../../tools/adhoc_relay/README.md)) is a small C++ UDP relay. A client registers into a
room with a cookie handshake (so a forged source address cannot make the server flood someone), then the server
copies its beacons to the rest of the room and its PDP / matching / PTP packets to the player whose MAC is the
destination (everyone else for the broadcast MAC). Packets are forwarded untouched; the client sends one copy and
the server fans out, so a 10-player room costs each player the same upstream as two. Per-client rate limits and a
32-player room size protect it. The client keeps the registration alive with a hello every second and treats 5 s of
silence as a lost server. `party.schindlershadow.com` runs it as a user service on a home server, with UDP 27500
forwarded; run your own with the README's two lines and point Server at it.

## Not implemented

- `sceNetAdhocDiscover*` and game mode (`sceNetAdhocctlCreateEnterGameMode` and friends): the game imports Discover but
  nothing in Communications mode was seen to need it.
- Input delay or rollback for high latency, and any form of authentication or encryption. A single relay host is a
  single point of failure.
- Infrastructure (WiFi access point) mode: the game does not use it.
