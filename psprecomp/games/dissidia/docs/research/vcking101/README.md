# Research notes from VCKing101's port

Six static-analysis notes on Dissidia 012's code and data, written for VCKing101's native port
([VCKing101/Dissidia012-pc](https://github.com/VCKing101/Dissidia012-pc), `docs/research/`, commit
`e7c622dbe223edb77b2ae45e5f8a4e604258baf9`, 17 November 2026). Copied here unchanged except for a header naming the source;
MIT licence, copyright (c) 2026 VCKing101 ([LICENSE](LICENSE)). Our own findings stay in the docs one folder up; these are a
second, independent reading of the same game to check them against. Survey of that project and what we took from it:
[../../related-projects.md](../../related-projects.md).

## Addresses

The notes use the USA release (ULUS10566 v1.00). The European release (ULES01505), the one this project was built against, has
the same decrypted executable and the same three overlays (their `regions.md`; our builds of both run the same recompiled code), so
every `main:` and `battle:` / `menu:` / `extra:` address in the notes is ours too. `main:0x0882...` is our `FUN_0882...`;
`battle:0x08B3...` is our `ovl_battle FUN_08B3...` (the battle overlay loads at `0x089DD4C0`, as here). Addresses we have used from
these notes and seen behave as described in our runs: `0x08872138` (pad dead zone, `input.cpp`), `0x08B38A0C` (walker input),
`0x08B3F7F4` (free camera input), `0x0890D664` / `0x0890CD90` (Communications battle exchange and check value, `comm_battle.cpp`),
`0x089D6A10` +0x38/+0x3C (battle object fighters).

The notes mark what they verified ("V", "verified") and what is inference; keep that distinction when using them. Where one of our
docs disagrees, trust the one with the run behind it, and note the disagreement in our doc.

## The notes and where they meet ours

| Note | What it covers | Our docs on the same ground |
|---|---|---|
| [pad-module.md](pad-module.md) | the pad manager object at `0x08C15B90` (raw, logical, null and system pads), the remap tables, virtual bits, the injected / replay / network input paths | [native-build.md](../../native-build.md) (input, analogue stick), [multiplayer.md](../../multiplayer.md) (battle pad link) |
| [battle-camera.md](battle-camera.md) | the battle camera's objects, the two input functions, parameter table and speeds, camera modes, hook points | [native-build.md](../../native-build.md) (right-stick camera), `runtime/camera.cpp` |
| [battle-moveset.md](battle-moveset.md) | how a fighter's attacks are defined and run, character-specific code (Lightning, Bartz), EX Mode, EX Burst and assists | [moves-and-events.md](../../moves-and-events.md), [character-mods.md](../../character-mods.md) |
| [moveset-format.md](moveset-format.md) | the moveset archive's chunks and record fields, all 125 ranged and 56 instant events with handlers, warp-strike, making a new attack | [moves-and-events.md](../../moves-and-events.md), [move-studio.md](../../move-studio.md); event names not in `tools/charmod_objx.py` yet are listed in [related-projects.md](../../related-projects.md) section 5 |
| [character-assets.md](character-assets.md) | the package archive and its name hash, the files of one playable character, model budgets, how loading is keyed, DLC | [formats.md](../../formats.md), [loose-file-mods.md](../../loose-file-mods.md), [roster.md](../../roster.md) |
| [roster-and-save.md](roster-and-save.md) | character ids, object entries, master tables, hard-coded counts, the select screen, the save data, what a new character would touch | [roster.md](../../roster.md), [savedata.md](../../savedata.md) |
