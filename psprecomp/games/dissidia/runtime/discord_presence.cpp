// games/dissidia/runtime/discord_presence.cpp — what Discord shows while Dissidia 012 runs (Rich Presence, runtime/src/psp_discord.cpp).
//
//   Dissidia 012 Duodecim                 <- the Discord application's name
//   Quick Battle (1-on-1) · Order's Sanctuary
//   Squall vs. Kefka                      (2 of 2 when in a Communications Mode group)
//   03:12 elapsed                         <- since the fight started; outside fights since the game started
//
// Where the player is comes from the start menu's own cursors (menu_settings.cpp reports them every frame the start menu or its
// sub-list updates): leaving the start menu with the cursor on Story Mode, Battle Mode > Arcade Mode, ... sets the mode until the
// start menu shows again. The title screen reports itself the same way. In a fight (battle overlay with fighters, framerate.cpp
// keeps the list) the fighters' characters (actor -> object record charId / type, 5 = manikin) and the stage (psp_stage_id) are
// added. All of it is read on the presence thread once a second: guest RAM words and atomics only.
//
// Names: the game's own English text (text/en/mess_pk_loading_0.bin entries 1 and 3). charId order = the EBOOT code table at
// 0x089D6CA8 (one100 .. sev120); characters added with roster.txt use their nm= label. Stage ids (the battle setup word read by
// hooks_main.cpp's dissidia_stage_id) were matched to maps in the issue #2 stage runs (docs/rendering notes); ids without a
// confirmed map (2, 13, 14, labyrinth floors) show no stage.
#include "psp_discord.h"
#include "psp_screenfx.h"
#include "psp_settings.h"
#include "hle/psp_hle_adhoc.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

int dissidia_live_fighters(uint32_t* out, int max);   // framerate.cpp
int dissidia_fighter_char_id(uint32_t actor);
int dissidia_fighter_model_type(uint32_t actor);
bool dissidia_fighter_human(uint32_t actor);
std::string dissidia_roster_name(int char_id);        // roster.cpp
std::string psp_release_version();                   // runtime/src/psp_ge_draw.cpp

namespace {

// The Discord application ("Dissidia 012 Duodecim", developer portal). Its name is the first line Discord shows.
// PSPRECOMP_DISCORD_APP_ID overrides it.
constexpr const char* kAppId = "";
constexpr const char* kIcon = "https://raw.githubusercontent.com/schindlershadow/dissidia/main/psprecomp/games/dissidia/icon/icon.png";
constexpr const char* kProjectUrl = "https://github.com/schindlershadow/dissidia";

constexpr uint32_t OVL_BASE = 0x089DD4C0u;   // loaded overlay id: 1 battle, 2 menu
uint8_t* g_ram = nullptr;
uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }

// charId (1..33) -> name; the manikin ("type 5" model) of the same character
const char* const kChars[34] = {nullptr,
    "Warrior of Light", "Garland", "Firion", "The Emperor", "Onion Knight", "Cloud of Darkness", "Cecil", "Golbez", "Bartz", "Exdeath",
    "Terra", "Kefka", "Cloud", "Sephiroth", "Squall", "Ultimecia", "Zidane", "Kuja", "Tidus", "Jecht", "Shantotto", "Gabranth", "Chaos",
    "Kain", "Gilgamesh", "Tifa", "Laguna", "Yuna", "Prishe", "Vaan", "Lightning", "Feral Chaos", "Aerith"};
const char* const kManikins[34] = {nullptr,
    "False Hero", "False Stalwart", "Imitation Liegeman", "Imitation Despot", "Counterfeit Youth", "Counterfeit Wraith", "Delusory Knight",
    "Delusory Warlock", "Fallacious Wanderer", "Fallacious Tree", "Phantasmal Girl", "Phantasmal Harlequin", "Imaginary Soldier",
    "Imaginary Champion", "Transient Lion", "Transient Witch", "Capricious Thief", "Capricious Reaper", "Ephemeral Vision",
    "Ephemeral Phantom", "Lady of Antiquity", "Warrior of Antiquity", nullptr, "Delusory Dragoon", "Fallacious Giant", "Imaginary Brawler",
    "Transient Gunner", "Ephemeral Summoner", "Horror of Antiquity", "Idle Sky Pirate", "Fleeting Flash", nullptr, nullptr};

// battle setup stage id -> stage
const char* stage_name(int id) {
    switch (id) {
    case 0: return "Old Chaos Shrine";
    case 1: return "Pandaemonium";
    case 3: return "World of Darkness";
    case 4: return "Lunar Subterrane";
    case 5: return "The Rift";
    case 6: return "Kefka's Tower";
    case 7: return "Planet's Core";
    case 8: return "Ultimecia's Castle";
    case 9: return "Crystal World";
    case 10: return "Dream's End";
    case 11: return "Edge of Madness";
    case 12: return "Order's Sanctuary";
    case 15: return "Empyreal Paradox";
    case 16: return "Sky Fortress Bahamut";
    case 17: return "Orphan's Cradle";
    case 18: return "Phantom Train";
    case 19: return "M.S. Prima Vista";
    case 20: return "Crystal Tower";
    case 21: return "Pandaemonium - Top Floor";
    default: return nullptr;
    }
}

std::string fighter_name(uint32_t actor) {
    const int id = dissidia_fighter_char_id(actor);
    if (id < 1) return "";
    const std::string mod = dissidia_roster_name(id);
    if (!mod.empty()) return mod;
    if (id >= 34) return "";
    if (dissidia_fighter_model_type(actor) == 5 && kManikins[id]) return kManikins[id];
    return kChars[id] ? kChars[id] : "";
}

// ---- where the player is (menu_settings.cpp reports, the presence thread reads) ----
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<int64_t> g_title_ms{0}, g_main_ms{0}, g_sub_ms{0};
std::atomic<int> g_top{-1}, g_group{0}, g_sub{-1};
int64_t g_session_start = 0;   // unix seconds

// Top-level rows of the start menu (mess_pk_start_menu entries; row 7 is this port's Settings) and Battle Mode's sub-list.
const char* const kTop[7] = {"Story Mode", "Battle Mode", "Communications Mode", "PP Catalog", "Collection", "Data Config", "Options"};
const char* const kBattleRows[4] = {"Quick Battle (1-on-1)", "Quick Battle (Party)", "Arcade Mode", "Labyrinth"};

// The mode chosen when the start menu was left (nullptr: none / unknown).
const char* chosen_mode() {
    const int top = g_top.load();
    if (top == 1 && g_group.load() == 1 && g_sub_ms.load() + 500 >= g_main_ms.load()) {
        const int s = g_sub.load();
        if (s >= 0 && s < 4) return kBattleRows[s];
    }
    return top >= 0 && top < 7 ? kTop[top] : nullptr;
}

// The current fight, kept while the battle overlay stays loaded (the fighter list goes stale while the game's pause menu is up).
struct Fight { bool on = false; int64_t start = 0; std::string a, b; int stage = -1; };
Fight g_fight;

bool provide(psp_discord::Activity* out) {
    if (!g_ram) return false;
    const int64_t t = now_ms();
    out->large_image = kIcon;
    out->large_text = "Dissidia Recomp " + psp_release_version();
    out->buttons = {{"Dissidia Recomp on GitHub", kProjectUrl}};
    const int group = psp_adhoc_group_size();
    if (group >= 2) { out->party_id = "dissidia-adhoc"; out->party_size = group; out->party_max = group; }
    out->start = g_session_start;

    const uint32_t ovl = r32(OVL_BASE);
    if (ovl != 1) g_fight = Fight{};
    if (t - g_title_ms.load() < 1500) { out->details = "Title Screen"; return true; }
    if (ovl == 2 && (t - g_main_ms.load() < 1500 || t - g_sub_ms.load() < 1500)) { out->details = "Main Menu"; return true; }

    const char* mode = chosen_mode();
    std::string details = mode ? mode : (group >= 2 ? "Communications Mode" : "");
    if (ovl == 1) {
        uint32_t f[4];
        const int n = dissidia_live_fighters(f, 4);
        if (n >= 2) {
            if (!g_fight.on) { g_fight.on = true; g_fight.start = std::time(nullptr); }
            // The player's side: the fighter a person drives. Its brain is switched to human mode during the intro, so the names
            // wait for it (a CPU-only fight, e.g. a demo, is named after 6 s in list order).
            int me = -1;
            for (int i = 0; i < n; i++) if (dissidia_fighter_human(f[i])) { me = i; break; }
            if (me < 0 && std::time(nullptr) - g_fight.start >= 6) me = 0;
            if (me >= 0) {
                const std::string a = fighter_name(f[me]), b = fighter_name(f[me == 0 ? 1 : 0]);
                if (!a.empty()) g_fight.a = a;
                if (!b.empty()) g_fight.b = b;
            }
            g_fight.stage = psp_stage_id();
        }
        if (g_fight.on) {
            if (const char* st = stage_name(g_fight.stage)) details += (details.empty() ? "" : " \xC2\xB7 ") + std::string(st);   // " · "
            if (!g_fight.a.empty() && !g_fight.b.empty()) out->state = g_fight.a + " vs. " + g_fight.b;
            out->start = g_fight.start;
        }
    }
    if (details.empty()) details = "In the menus";
    out->details = details;
    return true;
}

}  // namespace

// menu_settings.cpp: the title screen / the start menu's top level / its sub-list updated this frame.
void dissidia_presence_title() { g_title_ms.store(now_ms()); }
void dissidia_presence_main(uint32_t state) {
    if (!g_ram || !state) return;
    g_top.store(int(r32(state + 0xA28u + 4u)));
    g_main_ms.store(now_ms());
}
void dissidia_presence_sub(uint32_t state) {
    if (!g_ram || !state) return;
    g_group.store(int(r32(state + 0xA88u)));
    g_sub.store(int(r32(state + 0xA58u + 4u)));
    g_sub_ms.store(now_ms());
}

void dissidia_discord_install(uint8_t* rdram) {
    g_ram = rdram;
    g_session_start = std::time(nullptr);
    psp_discord::start(kAppId, provide);
}
