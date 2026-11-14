# Tag team battles (2 v 2): research notes and plan

Status (14 Nov): **2v2 plays**: four fighters fight (the partner and the enemy's second fighter are clones of the existing fighters on the CPU brain), HUD panels and reserve strips, L cycles the lock between the enemies, tag swap (SELECT, `PSPRECOMP_TAGTEAM_SWAP_BUTTON=1`, clone partners only), a settings toggle (Debug > Tag Team (2v2)) and a regression test (`tools/tagteam_test.py`). Not done: KO call-ins / round end rules, partners that are different characters, the real party roster in the reserve, portraits. Branch `tag-team`, based on `origin/main` 00b9d41. The sections below start with the original research and plan, then record each step.
Everything below comes from reading the recompiled code (`psprecomp dump <analysis> 0xADDR`) and the existing
runtime hooks. "Confirmed" = read in the code; "inferred" = a guess to check in a run.

Goal: Budokai Tenkaichi style tag battles. Two fighters per side on the stage at once, the rest of the party roster
comes in when a fighter is KO'd, the existing lock-on cycling switches between the enemy fighters.

## What the battle looks like today (confirmed)

**Two fighters, hard-wired.** The battle setup `FUN_0888BA0C(bs)` (EBOOT, called from `FUN_0888C4C8`) builds the whole
cast:

| bs field | meaning |
|---|---|
| `+0x4C` (76) | battle mode (5, 6, 0xD, 0 are handled here; 6 swaps the per-side data) |
| `+0x38` / `+0x3C` (56 / 60) | the two fighter actors (slot 0 / slot 1) |
| `+0x54` (84) | the slot the human plays |
| `+0x7D8` / `+0x7DC` (2008 / 2012) | the two fighters' setup data (char info) |

Each actor is made the same way: `FUN_08831320(0x12D0)` (allocate), `FUN_088729A0(side)`, `FUN_088470B8(setup)`, then the
fighter constructor `FUN_08B39E5C(mem, a, b, side, 0)`. The code after that is all `for (i = 0; i < 2; i++)`: the
foes are cross-linked with `FUN_08836DA4(a, b)` (`a+0x300 = b`, so `+0x300` is "my enemy"), per-fighter records
(stride 0x1060 in `bs`) are copied into each, the lock-on block is initialised (`FUN_08B3B1B8`), and the camera /
HUD take the two actors from `bs+0x38`, `bs+0x3C`.

**Actor kinds** (byte `actor+0x542`, set by the object constructor `FUN_0883975C`): 0 fighter, 1 assist (master at
`+0x1108`), 3 summoned / thrown object (master at `+0x5D4`), 5 bullet, 6 the dash's fixed-point target.
`FUN_0883739C` (is fighter) is true for kinds 0 and 1. Assists are real, fully loaded second characters that
already live in the battle next to the two fighters, so the engine can run more than two character actors.

**Actor fields used for targeting:**

| offset | meaning |
|---|---|
| `+0x300` | the enemy fighter (set at setup; `FUN_08AB32B8(actor)` returns it, kind 0/1 only) |
| `+0x1108` | the "partner": an assist's master; for a fighter it is also what lock-on falls back to |
| `+0x2FC` | the current target (what `turn_to_target`, homing dashes and projectiles read; getter `FUN_08836D8C`) |
| `+0x5AC` | the lock-on block (see below) |

**The lock-on block** (battle overlay, `FUN_08B3B0B4` .. `FUN_08B3B550`). This is the "cycle lock" system:

| block field | meaning |
|---|---|
| `+0xF8` (248) | owner actor |
| `+0xFC` (252) | the locked target actor |
| `+0x108` (264) | mode (0..3); `+0x10C` (268) previous mode; `+0x110` (272) / `+0x114` (276) index into the mode table |
| `+0x118` (280) | "changing" flag |
| `+0x12C` (300) | table of mode handler objects (each has virtual functions at vtable +8 / +16 / +112: enter, update, get-target) |

`FUN_08B3B280(block)` = current target (asks the active mode handler), `FUN_08B3B0B4(block, target)` = set the target
(also copies it into `owner+0x2FC` through `FUN_08B2F17C`, which clears it again in `FUN_08B2F22C`),
`FUN_08B3B3EC(block, mode, flag)` = change mode. The target the block hands out is "any kind", so it is not limited to
the one enemy: this is the part to reuse for cycling between two enemies. The mode handlers that enumerate the
candidates have not been read yet.

## What has to change for 2 v 2

1. **More than two fighter actors.** The setup builds exactly two. We need four kind-0 actors (two per side) from
   the same constructor, with their model / motion / effect data loaded (see risks).
2. **"The enemy" stops being one actor.** `+0x300` is single-valued. Each fighter needs a current foe (the locked
   enemy of the pair) and `+0x1108` has to be right for the lock fallback.
3. **Control.** One human fighter at a time (the active one), the partner on CPU AI; a tag button swaps who the
   pad drives and who the AI drives. The pad owner flag lives on the fighter's controller object (`+0x5C4`).
4. **Lock-on candidates.** Teach the lock block's cycle to walk the live enemy fighters (and skip KO'd ones).
5. **Win / lose and roster.** KO of the active fighter brings the next roster member in (position, intro, keep the
   bravery / HP rules) and only the last KO ends the round. The party roster (Quick Battle Party "Party A")
   already exists in the menu; today each member is fought in its own scene (inferred, check).
6. **HUD and camera.** Both the HUD (bravery / HP gauges, portraits) and the camera frame two fighters; the camera
   hook in `runtime/camera.cpp` already pivots on the nearest fighter and asks the player's lock state.
7. **Assist / summons / EX Revenge / break logic** read "the" opponent in many places (`+0x300`, `+0x1108`,
   `FUN_08AB32B8`, the EX Revenge manager); each needs a check for the 4-actor case.

## Risks, most likely first

- **Asset loading.** Two more characters means their models, motions, effects, sounds and `plya` tables have to be
  loaded in the same scene budget. Assists show it can be done, but how the loader picks what to load is unread.
- **The 0x12D0-byte actor and shared singletons** (HUD, camera, AI "brain" per fighter, replay recorder) may
  assume exactly slots 0 and 1.
- **Frame-rate patches** (`runtime/framerate.cpp`) track a fixed `g_fighters[4]`, one human and one CPU; a third
  and fourth fighter needs the same time step.
- **Online play** (the adhoc layer) syncs two fighters; tag battles are offline only to begin with.

## Plan (each step is checked in the game before the next)

1. **Spike: a third actor.** Hook the setup, create a third kind-0 fighter on the CPU side with the same
   constructor, link it, and see whether it loads and stands in the battle. Learn what the loader needs.
2. **Foe selection.** Make each fighter's foe the locked enemy; cycle the lock between the two enemies with the
   existing system.
3. **Fourth actor and partner AI.** Player team of two, only one human, the partner runs the CPU brain.
4. **Tag swap button** and active-fighter HUD.
5. **KO flow and roster call-ins**, then round end rules.
6. **Camera and HUD polish**, settings menu option, docs, tests.

## Spike results (14 Nov, `runtime/tagteam.cpp`, `PSPRECOMP_TAGTEAM=1`)

A third kind-0 fighter (a copy of the CPU fighter's setup data, so no new assets) is made after the setup and the
engine accepts it: it is updated every frame, stands where it is placed, becomes "ready" and is started with the others,
and the fight, HUD and result screen carry on. It does nothing yet (idle action, no AI choices, not on the camera).

What it takes (all confirmed in a run):
1. The construction: `FUN_08831320(0x12D0)`, `FUN_088729A0(side)`, `FUN_088470B8(bs + 0x7DC)`, `FUN_08B39E5C(mem, siderec, info, side, 0)`.
2. The setup's per-fighter init for that side: `FUN_08B01090(buf, side)`, then the actor's init through
   `*(actor + 0x544)`-style vtable slot at `*(actor + 1348) + 256` with `(actor + off, G + 0x1060 * side, buf)` where
   `G = 0x09AFB8A0` (per-side records), the two flag bits (`actor + 1456` bytes 7 / 8), and the lock block init
   `FUN_08B3B1B8(actor +0x5AC block, 3 words)`. Without it the actor ticks but never gets a script.
3. Placement `FUN_088359A8(actor, vec4, 1)` and the brain `FUN_08A64078(brain, 2)` / `FUN_08A64100(brain, 15)`
   (`brain = actor + 0x5C4`).
4. **The battle flow step `FUN_088887A4(bs)`** drives the start: it is called every frame until both fighters in
   `bs+56` / `bs+60` are ready (`FUN_08B2F9E0(actor)` = bit 0x10 of byte `actor+1456`), then calls
   `FUN_0882FA10(actor + 304, 1, 0)` (start action 1) for each and sets `bs+72 = 1`. The spike wraps it: it holds the
   flow until the third is ready too, lets the original run, then starts the third.

Test notes: actor addresses differ between runs (do not watch fixed addresses; `PSPRECOMP_TAGTEAM_WATCH=<hex off>` arms
the write watch on the human actor + offset from inside the hook). `PSPRECOMP_TAGTEAM_LOG=1` logs the steps.
Menu fast-forward above 1x misnavigated the menu walk (landed in the story), so the walk runs at normal speed.

5. **AI.** The brain (`actor + 0x5C4`) is given the setup's CPU settings: `FUN_08B2F2E4(actor, 0)` and, for control types 1
   and 2, `FUN_08A640CC` / `FUN_08A64100` with record halfwords +3894 / +3896. Two more things were needed: the target
   (`FUN_08B2F17C(actor)` copies the lock block's result into `+0x2FC`; the lock block target is set with
   `FUN_08B3B0B4(block, foe)`), and clearing **bit 2 of `actor+0x380`**, which the CPU brain tick `FUN_08A64400`
   treats as "skip this actor" (set at construction; the retail two fighters lose it after the intro; what clears it
   there is not found yet, the spike clears it every tick).

With all five the third fighter fights: in a run Squall faced two Lightnings and the new one dashed and attacked
(script ids 400 / 460 / 1 / 2 repeating). The brain's per-fighter `partner +0x1108` of a retail fighter is its **assist
actor** (kind 1), which is why `+0x1108` is not a team mate pointer.

Still open: the camera does not frame the third fighter, the lock block's cycle is not wired to it, the human has no
partner yet.

## HUD for the extra fighters and the reserve (14 Nov)

The retail HUD only knows the two original fighters (it reads them by slot through `FUN_089E95B0(side)` = the actor in `bs+56+4*side`).
The extras get their own: **`runtime/tagteam_hud.cpp`** draws a panel for each extra (name, HP bar, KO state; HP is the live word
`actor+0x10B0`, its maximum is the largest value seen) above the retail HUD, and a **reserve strip** per side under the top row
(the waiting fighters, next one highlighted). It needed a small engine feature: `ge_draw_set_game_overlay(rgba, w, h)`
(`runtime/include/psp_ge_draw.h`), a game module's own 480x272 canvas that the presenter blends over the picture (centred on the
480-pixel area in widescreen) and that screenshots include.

- Names: retail charIds 1..33 come from a built-in name table (the roster order; 15 Squall and 31 Lightning are confirmed in runs, the
  other ids are from the roster order and the series codes, so check them in game); mods fall back to their code string.
- The reserve lists are a **placeholder** (`PSPRECOMP_TAGTEAM_RESERVE="id,id;id,id"`, default `5,9;20,24`) until the KO call-ins
  read the real party. `PSPRECOMP_TAGTEAM_HUD=0` hides the HUD.
- Not done yet: bravery (the live BRV is not in the actor; the status object at `actor+0x52C` has floats at +0x18 / +0x1C that may be
  it, unverified), the "active" arrow for the pad-driven fighter, portraits.

## Camera and side-0 extras (tag-camera fork, 14 Nov)

Symptom: with a second fighter on the human's side the game's camera manager (`FUN_08B3CA24`, `[CAMX]` logs) computed
eye = target = (0, 0, 0) (later garbage), so the 3D scene came out grey / white. An extra on the enemy side looked fine.
Reproduced by the constructor alone (`PSPRECOMP_TAGTEAM_STOP=2`), with either character.

Cause (confirmed with RAM diffs around the constructor, `PSPRECOMP_TAGTEAM_REGDUMP/PTRDIFF=1`): `FUN_08B39E5C` registers the
new fighter in shared tables, replacing the retail pair's entries:
- the **camera manager's slot table at `mgr + 0x6D0 + 4 * side`** holds the fighters' lock blocks (slot 0 the human's,
  slot 1 the CPU's, 2 / 3 the assists'); the new fighter's lock block (`actor + 0x5AC`) takes the slot of its side, so the camera
  follows the new, not yet placed fighter. The camera uses side 0 only, which is why the enemy side looked fine;
- it resets four words of the old lock block (`+0x274 +0x278 +0x2A0 +0x314`) and links a new node into the old block's list
  (head at `old_lb - 0x30`);
- a pair table at `0x08BAE9FC / 0x08BAEA08` (CPU, human, human, CPU) gets the new actor instead of the CPU;
- EBOOT global `0x089D4774` (the last created fighter's controller-side object) and a node in the resource pool at `0x08C00xxx`.

The registration has to stay while the fighter loads (without it the actor's ready bit, `FUN_08B2F9E0`, never sets and the flow
waits forever), so `spawn_fighter` records an undo list (snapshot of 0x08800000..0x0A000000 before the constructor, diff after)
and `apply_undo` writes the old values back when the extra is released, in reverse spawn order: before the original flow step
for extras made at setup, in `start_extra` for late ones. Result: the gameplay camera is the retail one again.

Remaining: the resource node (`HE_D001T` / `HE_P001T_P1` for the human's side, `SG_D...` for the enemy's) holds the side's
**intro camera data**. A second fighter on a side re-loads it (the old node is released by `FUN_0886CAA0` / `FUN_0886A140`
and reused, even for a different character), so that side's intro cut shows eye = target = 0 for ~80 frames (and a second
~70 frames for the human's side). Skipping the two releases does not help (the node is reset anyway). Two ways around it:
make the extras after the intro (`PSPRECOMP_TAGTEAM_LATE=2`: the partner is made once the human's script ids >= 1000 have
run and it is idle again: 1 zero camera frame of 622, the frame it loads) or give the extras their own slot objects
(not done). Measured, `[CAMX]` zero frames in the first 400 updates: baseline 0; partner at setup, no fix 400 (all);
with the undo 151; enemy extra with the undo 80; partner late after the intro 1.

Debug switches: `PSPRECOMP_TAGTEAM_NOFIX=1` (no undo), `_KEEP_LAST=1`, `_FREE=1` (do not skip the releases), `_STOP=n`,
`_LATE=1|2`, `_REGDUMP=1`, `_PTRDIFF=1`, `_SCAN=1`, `_WATCHRES=<hex>`.

## 2v2 works (14 Nov, `PSPRECOMP_TAGTEAM=1`)

State of `runtime/tagteam.cpp`: both extras (the enemy team's second fighter and the human's partner, each a copy of the existing
fighter's setup data on a CPU brain) are made **after the intro** (the intro cut's camera data is loaded per fighter, and a second
fighter spawned during it zeroes the camera for ~80 frames; `PSPRECOMP_TAGTEAM_LATE=0` spawns at setup, `=1` right when the battle
starts), started together once both are loaded, and fight: in a 30 s run camera garbage is 1 frame in 500 (the load frame) and the four
fighters change script 22-41 times each. HUD panels and reserve strips show (see the HUD section).

- The fork's undo of the constructor's registrations (camera slot table, lock block, "last fighter" global) stays, **except the pair
  table at 0x08BAE9FC / 0x08BAEA08**: the CPU brain tick of an extra needs its place in it (with it undone the extras idle, only
  the first tick runs). `PSPRECOMP_TAGTEAM_UNDO_PAIR=1` restores the old behaviour for experiments.
- **Lock-on.** L toggles the human's lock through `FUN_08B3B3EC(block, mode, flag)` (mode 1 locked, 0 released; the retail cycle selector
  at mode handler [1] +28 is reset by the press itself). In a tag battle the hook `lock_mode_hook` turns the press that would release
  the lock into a cycle first: foe -> the other living enemy (`FUN_08B40994`, retail "the foe's assist", is hooked to return it, and
  `FUN_08B2F17C` is hooked to write the locked fighter into `+0x2FC`) -> release -> foe. Seen in a run: `lock cycle: foe -> other
  enemy`, `+0x2FC` changed to the extra. Not verified yet: that attacks and the camera really follow the second target in play.

## Tag swap (first attempt, 14 Nov: not working yet, off by default)

`PSPRECOMP_TAGTEAM_SWAP_BUTTON=1` (hex PSP button mask, 1 = SELECT) turns it on; the press is taken out of the pad input by the pad filter
(`dissidia_tagteam_pad`, called from framerate.cpp) and served by the next tick of the human's brain (`swap_roles`). The swap trades the
two fighters' brains (+0x5C4, brain +4 owner) and their places in the battle slot table (`bs+56+4*slot`, `[0x089D6A10]` is the same
`bs`), and re-points the camera manager's lock block slot (`mgr+0x6D0+4*side`). `PSPRECOMP_TAGTEAM_SWAP_MODE` = 1 brains only, 2 slots only,
3 both. Result: the pad does go to the other fighter (the swap logs `pad H -> P`), a second press swaps back and everything recovers, but
- brains only: HUD fine, **the game camera produces garbage** (eye 0 / grey world, ~46 of 700 logged updates);
- slots only: camera garbage too and **the retail HUD breaks** (left portrait / bars missing, right side shows the wrong fighter);
- so the camera and the HUD keep references to the human fighter (cached actor pointers in camera mode objects, HUD widgets) beyond the
  slot table, brain and camera slot.

## Tag swap: findings of the tag-swap fork (14 Nov; the swap still does not work)

**Fixed on the way: the extras' bravery was 0.** The retail HUD reads a fighter's current bravery as the halfword at
`*(actor + 0x52C) + 0x14` (`FUN_089E978C(side)`; the word is `<initial><current>`, both halves = the character's base bravery
`actor + 0x10AC` at battle start: human 0x0434 = 1076, CPU 0x0406 = 1030). The extras were left at 0, i.e. permanently broken
(`FUN_089E9A4C` flips to 1). `start_extra` now writes `(base << 16) | base` when the word is 0. (`status + 0x1C` is not bravery:
it is the assist gauge's draining bar; HP is `status + 0xC` = `actor + 0x10B0`.) A 2v2 run is unchanged otherwise (camera 1 bad
frame in 500, all four fighters act).

**HUD accessors** (`PSPRECOMP_TAGTEAM_HUDPROBE=1` calls the retail HUD getters `0x089E95B0..0x089E9EF4(side)` before and after the
swap): `FUN_089E95B0(side)` = `bs[56 + 4*side]`; `FUN_089E95E0 / 089E9628` = "side is CPU-controlled" (reads the brain: slots-only
swap makes both sides CPU, brains-only keeps them right); `089E96DC / 973C` = HP, `089E978C` = bravery. So the HUD does read the slot
actors live; with brains and slots both swapped and the bravery fixed the getters return the right numbers for both sides, yet the
left HUD still disappears and the right portrait shows the wrong character, so the **portraits / widgets are bound at setup** and not
re-read.

**Shared references to the human** (`PSPRECOMP_TAGTEAM_SWAPSCAN=1`, scans RAM for words equal to H, P, their lock blocks and brains
at swap time): H is held by the slot table (`bs+56`), a pair table at `0x08BAE9FC..0x08BAEA08`, a resource node near `0x08C00720`,
`0x08B63310`, a word near the camera manager (`mgr + 0x6360`, listed as `[CPU, human]`), `0x089D4890`, and ~50 words inside H's own
sub-objects, the CPU's, the extras' and a per-fighter record array at `0x09744xxx..0x09747xxx` (which holds P once per record, so
it is an interaction matrix, not a list to swap). The lock block of H is held only by the camera manager slot (`mgr + 0x6D0 + 4*side`)
and two words in a pool. `swap_roles` now also swaps `mgr + 0x6360`, `0x089D4890`, `0x08B63310` (`PSPRECOMP_TAGTEAM_SWAP_LISTS=<hex mask>`);
the addresses that hold H vary between runs once the extras spawn late (heap layout), so fixed addresses are unreliable and the
camera still shows garbage in 36 of 700 updates after a swap (46 before this change; baseline 1).

Conclusion: moving the retail slot between two actors needs the same bookkeeping the constructor does for a new fighter (camera
slot table, lock block list, pair table, "last fighter" global, the HUD's setup-time bindings), found by RAM diffs at the right moment, not by
patching a fixed list. Suggested next step: instead of swapping existing actors, **re-run the retail registrations** for the pair:
snapshot RAM before and after constructing a fighter into slot 0 (the fork's undo machinery does exactly this) and, at swap time,
write P's "after" values for the retail registrations and H's "before" values for the extra, or give the HUD and camera a single
fixed "player" actor and swap the character data under it (clones only).

## Tag swap works: content exchange (14 Nov, branch tag-swap2)

`PSPRECOMP_TAGTEAM_SWAP_BUTTON=1` (1 = SELECT) now swaps for real (default `PSPRECOMP_TAGTEAM_SWAP_MODE=4`; 1..3 are the old, broken rebind modes).
The earlier forks tried to re-point the human's references (slot table, brains, camera slot) to the partner and always left some camera / HUD
binding behind. The working idea is the opposite: **never move a reference, exchange the character instead**. Everything the game binds
to "the human" (HUD, camera, pad brain, lock block, pair table, enemies' targets) refers to the fighter's *address*, so after the swap the
actor at the human's address H holds what used to be the partner P's character, and P holds the old human.

- **Ownership recording.** `alloc_hook` wraps the heap allocator core `FUN_0885C6CC` and records every block allocated while a fighter is
  built (the actor block, the constructor `FUN_08B39E5C` via `ctor_hook`, the per-fighter init, the vtable slot at `FIGHTER_VT+260` via
  `init_hook`; for extras `spawn_fighter` sets the context itself). Clones allocate the same sizes in the same order (57 blocks per
  fighter), so block k of H pairs with block k of P.
- **Exchange** (`content_swap`): for each pair (except the human brain and the lock block, which stay whole at their address) every
  word that differs is exchanged; **numbers** swap, **pointers into the owned blocks** are translated to the counterpart block (same offset), and
  **pointers to anything else** (shared resources, list neighbours, registrations, targets: 359 words the first time, 21 the second) stay with
  their address. About 24,000 word writes per swap.
- **Safe point**: `dissidia_tagteam_frame`, called at the start of the camera update (`camera.cpp`, once per frame, outside any fighter's update;
  the scene tick turned out not to run per frame in battle). The pad filter (`dissidia_tagteam_pad`) takes the button press out of the input.
- **Result** (headless run, SELECT at 12 s and 19 s, CIRCLE taps after each): camera garbage 1 frame in 700 (baseline 1 in 500), screenshots after each
  swap show the normal 3D scene with both retail HUD portraits and bars; at 12 s the actor at H took over P's script 10016 and position
  (1.2, -13) and P took H's script 1 / position (0, 33); each CIRCLE tap then starts script 10008 on H (the pad drives the new character) while P runs
  its own AI actions; the second swap restores positions exactly; the regression test (`tools/tagteam_test.py --baseline`) still passes.
- **Limits.** Works for *clones*: the two fighters must own the same block list (the swap logs and aborts if the counts or sizes differ),
  which is the case for the partner (a copy of the human). A different character as partner owns different blocks, so it needs a
  per-character exchange of the visible parts only (model, moveset, stats) on top of this; the HUD portraits are bound at setup and stay
  those of the human's original character. Blocks allocated after construction (effects, projectiles) are not owned and stay with the address.
  The human's lock mode is re-engaged after a swap (`FUN_08B3B3EC`). Not tried: swapping mid-attack with projectiles out, KO call-ins, the lock
  cycle (L) after a swap (it reads the human by address, so it should work).

## Partners of a different character (14 Nov: not working, needs resource loading)

`PSPRECOMP_TAGTEAM_PARTNER_CHAR=<charId>` / `_ENEMY2_CHAR` make an extra of another character: the character info is found by id in the table
`FUN_08846C98` searches (`*(*0x089D4968)`: header with the array at +8 and the count at +4, 0x44-byte records, charId = signed byte +6; the
record also holds the model file stems, e.g. `P_EHT100_1P` Squall, `P_SEV100_1P` Cloud). Result with Cloud as the partner: the actor is built,
becomes "ready", gets a panel in the HUD, but never starts (script id stays -1) and nothing is drawn: **retail only loads the data of the characters
in the battle during the loading screen** (the model slots 0x54E, 0x134, 0x408, 0x548 of `FUN_08B45310`, the 0x780000-byte model pool of
hooks_models.cpp holds the built models of the fighters and their assists; a model larger than the budget fails). A different character
therefore needs its model / motion / effect / sound files streamed in at the loading stage (extend the retail loader to include the extras
with room in the pool), or reuse of an already loaded character (the fighters, and the assists' characters). Characters that are already loaded
(Squall, Lightning here) work, and the swap needs a clone (it exchanges the block lists of two fighters of one character).
`PSPRECOMP_TAGTEAM_INFODUMP=1` prints the records.

## Play-test round 1 (user, RP6)
- Fixed: reserve strips are empty by default (they showed a placeholder roster outside a group battle; `PSPRECOMP_TAGTEAM_RESERVE` sets one);
  the locked enemy now also becomes the foe (`actor+0x300`) so damage / follow-up attacks use it (`PSPRECOMP_TAGTEAM_KEEPFOE=1` restores the old
  behaviour); extras stop fighting once a slot fighter is down (victory scene); `alive()` uses live HP (status+0xC).
- Camera: the camera manager keeps a foe word at `mgr+0x8B4`; it is written every frame by the lock blocks' mode handlers (`FUN_08B3B280`)
  and flips between the human's target and the partner's, so a single write before the update is not enough (`[TAGTEAM] camera foe` lines).
  Open: the camera still frames the original opponent, and chase cameras. Debug: `_FOESCAN=1`, `_WATCH_CAM=<hex mgr offset>`.
- Open from the report: L skips an off-screen second enemy and the EX core is in the cycle (retail candidates), HP/BRV damage to the second enemy
  needs a play test with the foe fix.
## KO rules (14 Nov, branch tag-ko; partly working)

How retail handles a KO (found with `PSPRECOMP_TAGTEAM_KO=<who>:<s>`, `_KO_DUMP`, `_WATCH`):
- Every fighter has a **stock counter**, the halfword `actor+0x10CE` (1 in a Quick Battle). A KO runs the EBOOT rules chain
  `FUN_08891894 -> FUN_08891CA4 -> FUN_088C6D0C -> FUN_088C6F44 -> ovl FUN_08B33914(actor)`, which takes one stock and sets the
  **revive word `actor+0x10D0` = 1**. With stock left the battle does not end: the KO presentation runs (the harness' "battle ended" is
  only the fighters not being stepped for a second), then the round restarts (battle time starts over, same actors). At 0 the battle ends the
  retail way. The rules engine keeps per-slot records around `bs+0x7A4` (`FUN_08A07DFC(obj, slot, ...)`, 40 bytes per slot, `FUN_088912D0` /
  `FUN_08891684` / `FUN_088916A4`); `bs+72` is a per-frame flow phase, not the result, and the end handlers `FUN_0888A434..0888ACA0`
  (`PSPRECOMP_TAGTEAM_ENDLOG=1`) are not what ends a Quick Battle round.
- Live HP is `*(actor+0x52C)+0xC`; `actor+0x10B0` is the maximum (`alive()` read the maximum before this branch). Live HP 0 set by hand
  only plays the KO motion (script 1210), no stock loss; the rules chain runs after a real KO, which the test recipe (live HP 1, bravery
  halfword `status+0x14` = 0) triggers within 2 frames for the human slot fighter.
- `ko_watch()` (tagteam.cpp, once per frame from `dissidia_tagteam_frame`): each slot fighter gets **one more stock per living team mate**
  (`start_extra`), so the first KO restarts the round instead of ending the battle. When a slot fighter's stock drops and a team mate
  lives, the team mate is exchanged into the slot with `content_swap` when the round restarts (battle time goes back; fallback 8 s); the slot
  keeps its stock and revive word, the KO'd character stays down at the team mate's address (live HP 0, its brain no longer ticks, its HUD
  panel shows KO). The camera stays sane (1 bad frame in 1000). `PSPRECOMP_TAGTEAM_TAGIN=0` turns it off, `_KO_NOSWAP=1` keeps the stock
  rule without the swap.

Not working / not verified:
- After the tag-in the **retail HUD stays in its KO / Break display** (left bars and portrait missing, "BREAK" text, right portrait wrong)
  although the swap itself is clean; without the swap the HUD recovers after the revive. Swapping at the KO event or at the round restart
  gives the same picture, so the HUD widgets keep a KO state that only the retail revive path clears (find what the revive resets; the revive
  word `+0x10D0` and the status object are the first suspects).
- The test recipe makes the human KO twice (live HP stays 1 after the revive), so "both KO'd -> the round ends" is untested; the CPU slot
  fighter was not KO'd by the recipe (nothing hit it), so the enemy side is untested (the code is symmetric over both slots).
- The KO of a non-slot extra is not handled beyond "a dead extra stays down".

## KO tag-in, round 2 (HUD bisect; tag-in is OFF by default, `PSPRECOMP_TAGTEAM_TAGIN=1`)
- Merged the tag-ko work onto tag-team. The swap now waits until the revived slot fighter is controllable (script 1/2 for 45 frames after the
  restart) instead of swapping in the restart frame. The retail stock decrement is seen twice (KO, then again at the round restart).
- Writing live HP 0 into the KO'd character's status (`T+0x52C`+0xC) after the swap breaks the retail HUD (left bars/portrait gone, right portrait
  wrong) - the retail rules/HUD read the team's status. Not writing it (`PSPRECOMP_TAGTEAM_KOFIX=4`) keeps the HUD on some runs, but the KO'd
  character then fights on as the AI partner; marking it down with `g_down` + the AI gate bit (+0x380 bit 2) breaks the HUD / froze the fight in
  others (`_KOFIX` 1 = no stock restore, 2 = no stock zero on T, 4 = no down mark, 8 = down but no AI gate; `_TAGIN_SLOTS` bitmask of slots).
  Results are timing dependent (the same flags gave intact and broken HUD in different runs) so the HUD state depends on what the slot
  fighter is doing at the swap. Open: what the retail HUD keeps per fighter that the content exchange does not carry.

## 2v1 mode
Settings > Debug > Tag Team 2v1 (or `PSPRECOMP_TAGTEAM_2V1=1`, same as `PSPRECOMP_TAGTEAM_EXTRAS=2`): with Tag Team on, only the human's partner is added (you + partner against the one enemy).
Headless run: 3 fighters acting, camera 1 bad frame in 500, no lock cycle (no second enemy).

## 1v2 mode
Settings > Debug > Tag Team 1v2 (or `PSPRECOMP_TAGTEAM_1V2=1`, = `EXTRAS=1`): only the enemy's second fighter is added; you have no partner. 2v1 and 1v2 both on = 2v2.
Headless run: 3 fighters acting, camera 0 bad frames in 500, lock cycle goes foe -> second enemy.
## KO tag-in, round 3 (HUD fixed; tag-in ON again, `PSPRECOMP_TAGTEAM_TAGIN=0` turns it off)
**Root cause of the broken HUD:** the retail HUD asks `FUN_089E968C(side)` = "is the slot actor the player?" = `slot == FUN_08842164()`, and that
function returns the actor at `*(0x089D4958)+20` only if its class check `FUN_08B2EF74` passes: **`actor+0x5B0` bit 2** ("this actor is the
controlled one"). `content_swap` exchanged that byte between the two fighters, so after the swap the slot fighter was no longer "the player": the HUD
treated side 0 as CPU-controlled (`FUN_089E95E0`/`9628` = 1), hid the left bars/portrait and mis-bound the right one. (The plain SELECT swap
showed the same getters change in a probe but nobody looked at the player flag.) The flag belongs to the address, like the brain and the lock
block: `content_swap` now keeps bit 2 of `+0x5B0` with the address. Found with `PSPRECOMP_TAGTEAM_HUDWATCH=<every N frames>` (prints the retail HUD
getters for both sides, the slot fighters' HP / BRV / stock / revive word / script and `bs+72` whenever one changes).
- Not the timing, not the status HP write: the earlier "timing dependent" results were that flag plus the status HP-0 write (which also hurts, so a
  KO'd team mate is marked down with `g_down` + the AI gate bit instead of writing HP).
- KO detection: the rules chain takes **one** stock per KO (`FUN_08B33914`); the round restart then drains more (the KO'd slot 1 -> 0, the other
  slot 2 -> 0 as well). `ko_watch` treats a drop of exactly one stock as the KO, and after the swap restores the other slot's stock to what it was
  before (`snap`). The KO'd slot ends with stock 0, so the next KO of the slot fighter ends the battle: the round ends when both team mates are down.
- Test recipe caveat: `PSPRECOMP_TAGTEAM_KO=<who>:<s>` (HP 1, BRV 0) makes retail drain the stock within 2 frames and the fighter stays at HP 1; it is
  not a real KO. Debug: `_KO_IDLE=<frames>` (idle frames after the restart before the swap, default 45), `_KO_HPVAL`, `_STOCK=<n>`, `_KOFIX` bits.
- Verified (headless, screenshots after the swap): human KO and enemy KO (`cpu`) both keep the retail HUD (portraits, bars, BRV numbers) and the
  fight goes on; the KO'd character shows `KO` in its panel; a later KO of the slot fighter ends the battle (DEFEATED). `tagteam_test.py --baseline`
  PASS. Not verified: a real lethal hit (the recipe never produces one), the KO fall animation of the replaced character (it stands), swapping
  mid-projectile.
## Party (group) battle roster (tag-party branch)
- **Reaching a party battle headless**: `tests/replays/party_battle_walk.txt` (use `battle_test.py --direct <file>` / a copy of `.scratch/tt_run.py` with `direct=` pointing at it). Same boot as the Quick Battle walk, then Down + Cross on Battle Mode > Quick Battle (Party), Cross (Party A: Jecht, Squall, Cloud of Darkness, Terra, Yuna), the CPU party is picked on the CPU Select screen (Lightning, Vaan, Laguna), OK, Enter Battle, VS, Opponent Info. Battle in ~40 s. (Menus drop presses during transitions: keep >= 120 polls between steps.)
- **Where the party lives**: the battle overlay keeps a team record per side at `bs + 0x13C21 - 6` (stride 0x38): a **count byte**, then `count` charIds in party order (1st, 2nd, ...), 0xFF filled, then the jobs (`01 01 07 06 08` = WR WR BM WM SM). A 1 v 1 battle rewrites it with a count of 1 (so it is never stale). Two more copies exist: the menu overlay's team struct (`0x09AFDFD5` / `0x09AFE00D`, same layout, plus round-result bytes that change when a round ends: `I-7`, `I-5` of the player's list, `J-4` and `J+10` of the CPU's) and a word-per-entry copy (`0x09B6F2B4`, flags + jobs). Retail plays the party sequentially, one battle per member (VS screen "Round N", 1st/2nd labels), so the members behind the one on the stage are the reserve.
- **Implemented**: `update_party_reserve()` (tagteam.cpp) reads the two lists every HUD frame; the member on the stage is the actor's charId (first match from the last index, so a repeated charId works), the reserve strip lists the ones after him; a count of 1 (1 v 1) shows nothing. Verified in the Party A battle: `team0: SQUA COD TERR YUNA | team1: VAAN LAGU`; the 1 v 1 baseline shows nothing. `PSPRECOMP_TAGTEAM_RESERVE` still overrides. Name table fix: charId 29 is Prishe, 30 is Vaan (checked against the CPU party pick).
- **Not decoded**: the exact "which member is next / who is KO'd" progress word of the sequential flow (only the round-result bytes above were seen to change after a round); members who already lost are inferred from the list order.
- **What a real roster tag-in needs**: the next party member is a different character, and retail loads model, motion and sound data only for the two (plus their assists') characters at the loading screen into fixed slots (0x780000 model pool), so a call-in of Squall for Jecht needs those resources streamed in (or loaded for the whole party up front by the loading step, `FUN_0888BA0C` / the character info table), and the actor built through the same spawn path as the extras (`spawn_fighter` with that character's info record). The retail party flow itself restarts the battle with a fresh setup for the next member, which is a cheaper route to a real "next member" (let the round end the retail way and keep the other side's fighter state) than loading everyone at once.
