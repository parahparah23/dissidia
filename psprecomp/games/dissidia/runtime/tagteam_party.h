// games/dissidia/runtime/tagteam_party.h - Communications Mode tag team: each party's order list (tagteam.cpp).
// See docs/tag-team.md "Communications Mode: CPU picks".
#pragma once
#include <cstdint>

// One entry of a party's order list. The list is the setup screen's order column (the battle's party record, the game's own data), so
// it is the same on every machine; owner / is_cpu come from the players' tag bytes as they were when the battle was set up.
struct TagEntry {
    uint8_t charId;   // the character
    int8_t owner;     // the player who registered the row: an index into the group's players sorted by (party, own character), the
                      // same number on every machine; -1 = no player's bytes name it (unknown, treated as a player's row)
    bool is_cpu;      // the owner set that row to CPU on the setup screen (Square): the AI fights it
    bool local;       // the owner is this machine's player (differs per machine by design: never use it for a battle decision)
    uint8_t strength = 5, level = 0, type = 0;   // a CPU row's settings (the owner's dialog, synced in the beacon): STRENGTH 1-9, LEVEL 0 = Auto / 1-100, TYPE 0 = Random / 1-8
};

// Party 0 = A, 1 = B (in a battle the party = the battle side). In a battle: the record's order; in the menus: the players' rows,
// players sorted by (party, own character). Returns the number of entries written (up to `max`).
int tagteam_party_order(int party, TagEntry* out, int max);
// The party's CPU rows in list order, without the one on the stage (in a battle). Returns the count.
int tagteam_party_cpus(int party, uint8_t* ids, int max);
// The party's players (not rows) in the group.
int tagteam_party_humans(int party);
// 1 or 2 fighters on the stage at once (two players: 2 when the party has a CPU row, else 1; three or more: the host's choice).
int tagteam_fighters_per_side(int party);
