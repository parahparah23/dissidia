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
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ctime>
#include <string>
#include <vector>

int dissidia_live_fighters(uint32_t* out, int max);   // framerate.cpp
int dissidia_fighter_char_id(uint32_t actor);
int dissidia_fighter_model_type(uint32_t actor);
bool dissidia_fighter_human(uint32_t actor);
uint32_t dissidia_fighter_brain_mode(uint32_t actor);
bool dissidia_fighter_local(uint32_t actor);
std::string dissidia_roster_name(int char_id);        // roster.cpp
std::string psp_release_version();                   // runtime/src/psp_ge_draw.cpp

namespace {

// The Discord application ("Dissidia 012 Duodecim", developer portal). Its name is the first line Discord shows.
// PSPRECOMP_DISCORD_APP_ID overrides it.
constexpr const char* kAppId = "1571969529272213605";
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

// ---- the player's name: the save's player name (Data Config > Player Name), UTF-16 at a fixed EBOOT BSS address; found by
// changing it in RAM and watching Data Config follow (the copy at 0x08BDC4BC is a stale duplicate). Empty before a save is loaded.
constexpr uint32_t PLAYER_NAME = 0x08BCEB40u;
constexpr int kNameUnits = 16;
std::string player_name() {
    std::string out;
    for (int i = 0; i < kNameUnits; i++) {
        uint16_t c;
        std::memcpy(&c, g_ram + ((PLAYER_NAME + 2u * i) & 0x07FFFFFFu), 2);
        if (c == 0) break;
        if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF) || c == 0xFFFF) return "";   // not text: no save loaded (or not this layout)
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
// Other players see this name (the ad-hoc nickname, "Player" until a save is loaded). Called on the game thread from the start menu
// (every Communications Mode session starts there, and a name changed in Data Config comes back through it).
void update_nickname() {
    static std::string last = "\x01";
    const std::string n = player_name();
    if (n == last) return;
    last = n;
    psp_adhoc_set_nickname(n.c_str());
}

// "Bob", "Bob and Carl", "Bob, Carl and Dan", ... cut to fit `max` bytes ("and 2 others").
std::string name_list(const std::vector<std::string>& names, size_t max) {
    for (size_t shown = names.size(); shown > 0; shown--) {
        std::string s;
        for (size_t i = 0; i < shown; i++) {
            if (i) s += (i + 1 == shown && shown == names.size()) ? " and " : ", ";
            s += names[i];
        }
        if (shown < names.size()) s += " and " + std::to_string(names.size() - shown) + (names.size() - shown == 1 ? " other" : " others");
        if (s.size() <= max) return s;
    }
    return std::to_string(names.size()) + " players";
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
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
struct Fight { bool on = false; int64_t start = 0; std::string a, b; int stage = -1; std::string log; };
Fight g_fight;

bool provide(psp_discord::Activity* out) {
    if (!g_ram) return false;
    const int64_t t = now_ms();
    out->large_image = kIcon;
    out->large_text = "Dissidia Recomp " + psp_release_version();
    out->buttons = {{"Dissidia Recomp on GitHub", kProjectUrl}};
    const int group = psp_adhoc_group_size();
    std::vector<std::string> others;
    if (group >= 2) {
        others = psp_adhoc_group_nicks();
        char id[32];
        std::snprintf(id, sizeof id, "dissidia-%016llx", static_cast<unsigned long long>(fnv1a(psp_adhoc_group_key())));
        out->party_id = id;   // the same for everyone in the room: Discord shows them as one party
        out->party_size = group; out->party_max = group;
        std::string me = player_name();
        std::vector<std::string> all{me.empty() ? std::string("You") : me};
        all.insert(all.end(), others.begin(), others.end());
        out->large_text = "Players: " + name_list(all, 118);
    }
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
            // The player's side: the fighter this copy's player drives (online the other players' fighters are pad-driven too), else the
            // first human-driven one. Both are set during the intro, so the names wait for them (the intro at normal speed takes over 6 s;
            // a CPU-only fight is named after 30 s in list order).
            int me = -1;
            for (int i = 0; i < n && me < 0; i++) if (dissidia_fighter_local(f[i])) me = i;
            for (int i = 0; i < n && me < 0; i++) if (dissidia_fighter_human(f[i])) me = i;
            static const bool log = std::getenv("PSPRECOMP_DISCORD_LOG") != nullptr;
            if (log) {   // which fighter is whose (online: is the remote player's fighter "human" too?)
                std::string l;
                char b[96];
                for (int i = 0; i < n; i++) {
                    std::snprintf(b, sizeof b, " [%08X char %d type %d brain %X local %d]", f[i], dissidia_fighter_char_id(f[i]), dissidia_fighter_model_type(f[i]),
                                  dissidia_fighter_brain_mode(f[i]), dissidia_fighter_local(f[i]) ? 1 : 0);
                    l += b;
                }
                if (l != g_fight.log) { g_fight.log = l; std::fprintf(stderr, "[DISCORD] fighters:%s me %d\n", l.c_str(), me); }
            }
            if (me < 0 && std::time(nullptr) - g_fight.start >= 30) me = 0;
            if (me >= 0) {
                const std::string a = fighter_name(f[me]), b = fighter_name(f[me == 0 ? 1 : 0]);
                if (!a.empty()) g_fight.a = a;
                if (!b.empty()) g_fight.b = b;
            }
            g_fight.stage = psp_stage_id();
        }
        if (g_fight.on) {
            if (const char* st = stage_name(g_fight.stage)) details += (details.empty() ? "" : " \xC2\xB7 ") + std::string(st);   // " · "
            if (!g_fight.a.empty() && !g_fight.b.empty()) {
                out->state = g_fight.a + " vs. " + g_fight.b;
                const std::string me = player_name();
                if (others.size() == 1 && !me.empty()) {   // one on one online: who is who
                    const std::string named = me + " (" + g_fight.a + ") vs. " + others[0] + " (" + g_fight.b + ")";
                    if (named.size() <= 128) out->state = named;
                }
            }
            out->start = g_fight.start;
        }
    }
    if (details.empty()) details = "In the menus";
    if (out->state.empty() && !others.empty()) out->state = "With " + name_list(others, 123);
    out->details = details;
    return true;
}

}  // namespace

// menu_settings.cpp: the title screen / the start menu's top level / its sub-list updated this frame.
void dissidia_presence_title() { g_title_ms.store(now_ms()); }
void dissidia_presence_main(uint32_t state) {
    if (!g_ram || !state) return;
    g_top.store(int(r32(state + 0xA28u + 4u)));
    update_nickname();
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
