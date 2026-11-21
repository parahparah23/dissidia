// games/dissidia/runtime/tagteam_menu.cpp - Communications Mode > Tag Team Battle: the Group Battle Tournament setup screen.
// See docs/tag-team.md "Communications Mode: CPU picks".
//
// The retail screen already lets each player register more than one character (the left column under the player's own: each row opens
// Player Select, the game sends the rows to the other players and puts them in the battle's party records). Every row is the player's own
// character (played by him when its turn comes); in the tag team mode Square on a row from the second on makes it a CPU (the AI fights
// it) and back. This file draws on the game layer: a "CPU" plate on the CPU rows, "Add a character" in the next free row, a Square key
// with "CPU" / "Player" in the button bar while the cursor is on a row that can be switched, and, with three or more players, the
// host's fighters-per-side choice (L on the host's screen cycles it). The plain retail Group Battle (tag team mode off) is untouched.
//
//   ovl_menu FUN_08AA8F9C(screen): the setup screen's per-frame update (runs only while it is up, not under Player Select).
//   screen + 0x7CC4: the cursor's column (0 = the left column), screen + 0x5A4: its row there (0 = the player's own).
#include "psp_runtime.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include "menu_layer.h"
#include "../../../runtime/src/hle/psp_savedata_dialog.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>

extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
bool dissidia_tagteam_group_on();
void dissidia_tagteam_gb_setup_frame(int cursor_row);
uint8_t dissidia_tagteam_local_own_mask();
int dissidia_tagteam_local_regs(uint8_t out[5]);
int dissidia_tagteam_group_size();
bool dissidia_tagteam_is_host();
uint8_t dissidia_tagteam_mode_byte();
void dissidia_tagteam_set_mode_byte(uint8_t m);
uint8_t dissidia_tagteam_local_cpu_mask();
void dissidia_tagteam_toggle_cpu_row(int row);
void dissidia_tagteam_get_cpu_cfg(int row, uint8_t out[3]);
void dissidia_tagteam_set_cpu_cfg(int row, const uint8_t in[3]);
int dissidia_tagteam_menu_cpu_row(int party, int row, uint8_t out[3]);
int dissidia_tagteam_local_party();
bool dissidia_tagteam_cpu_party_on();
int dissidia_tagteam_defect_side();
void dissidia_tagteam_add_cpu_request(bool on);
bool dissidia_tagteam_add_cpu_pending();
bool dissidia_tagteam_sp_tag_on();
bool dissidia_tagteam_sp_mode();
int dissidia_tagteam_sp_rows(uint32_t screen, uint8_t out[5]);
uint8_t dissidia_tagteam_sp_cpu_mask();
void dissidia_tagteam_sp_toggle(int row);
bool dissidia_tagteam_sp_assist_on();
void dissidia_tagteam_sp_assist_toggle();
void dissidia_tagteam_sp_get_cfg(int row, uint8_t out[3]);
void dissidia_tagteam_sp_set_cfg(int row, const uint8_t in[3]);

namespace {

using dissidia::LayerColor;
constexpr uint32_t OVL_BASE = 0x089DD4C0u;
constexpr uint32_t SETUP_TICK = 0x08AA8F9Cu;   // ovl_menu: Tournament setup screen update (a0 = the screen)
// The single player Tournament screen (Quick Battle > Party): its per-frame function and the round dialog's update. screen + 0x7CFC = M: +4 the state, +1444 the round
// the dialog is open on (0 = 1st), +1488 the dialog's cursor row (0 STAGE, 1 STRENGTH, 2 LEVEL, 3 TYPE, 4 RULES, 5 JUDGEMENT).
constexpr uint32_t SP_TICK = 0x08AAB514u, SP_DLG = 0x08AA8C78u, SCR_M = 0x7CFCu;
constexpr uint32_t PLAY_SE = 0x088D4BF4u;      // EBOOT: menu sound effect (1 cursor)
constexpr uint32_t BTN_L = 0x100u, BTN_SQUARE = 0x8000u;
constexpr uint32_t SCR_COLUMN = 0x7CC4u, SCR_ROW = 0x5A4u;
// The left column: five rows of 20 units from y 120 (row 0 = the player's own character), the job name centred at x ~110.
constexpr float kRowY0 = 120.f, kRowPitch = 20.f, kColX0 = 2.f, kColX1 = 164.f;
// The single player Tournament screen (Quick Battle > Party; Battle Mode > Tag Team Battle): its per-frame update (a0 = the screen; tree ttfb found it
// for the round dialog work). screen + 0x7CFC = M (seen = the screen itself): M + 4 the state (2 = the list), M + 1444 the cursor row (0..4 = 1st..5th, 5 = Enter Battle).

uint8_t* g_ram = nullptr;
FuncPtr g_orig_tick = nullptr, g_play_se = nullptr, g_orig_sp_tick = nullptr, g_orig_sp_dlg = nullptr;
bool g_dlg_sp = false;   // the CPU settings dialog edits a single player CPU row (else a Communications row)
dissidia::MenuLayer g_layer;
uint32_t g_prev_buttons = 0;
// The CPU settings dialog (a CPU row's STRENGTH / LEVEL / TYPE; opened with Select (Triangle is the screen's Customization) on one of the player's own CPU rows). While it is open it takes the pad
// (dissidia_tagteam_menu_pad is in the game's button filter chain), the setup screen underneath sees no buttons.
constexpr uint32_t BTN_UP = 0x10u, BTN_RIGHT = 0x20u, BTN_DOWN = 0x40u, BTN_LEFTK = 0x80u, BTN_R = 0x200u, BTN_OPEN = 0x1u, BTN_CIRCLE = 0x2000u, BTN_CROSS = 0x4000u;
std::atomic<uint32_t> g_raw_pad{0};   // the pad as the filter saw it before the dialog took it
std::atomic<bool> g_dlg_open{false};
std::atomic<int> g_inject_cross{0};   // "Add CPU" (L on the free row) presses Cross for the screen: it opens Player Select for the new row
std::atomic<uint32_t> g_pad_polls{0}, g_sq_stamp{0};   // pad polls; the poll at which the setup screen last ran with Square taken (the block ends a few polls after the screen stops ticking)
std::atomic<bool> g_sp_sq_block{false};   // single player setup list: Square is ours (Assist On / Off), the screen does not see it
int g_dlg_row = 0, g_dlg_field = 0;
uint8_t g_dlg_cfg[3] = {5, 0, 0};
float g_dlg_hold = 0.f;
std::chrono::steady_clock::time_point g_last{};
float g_appear = 0.f;

uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }

void play_se(uint8_t* rdram, recomp_context* ctx, int id) {
    if (!g_play_se) return;
    recomp_context save = *ctx;
    ctx->r[4] = id;
    g_play_se(rdram, ctx);
    *ctx = save;
}

// The fighters-per-side choices the host cycles through (byte 6 of the tag bytes: bits 0-1 Party A, bits 2-3 Party B).
constexpr uint8_t kModes[4] = {2 | (2 << 2), 2 | (1 << 2), 1 | (2 << 2), 1 | (1 << 2)};
std::string mode_label(uint8_t m) {
    const int a = (m & 3) == 1 ? 1 : 2, b = ((m >> 2) & 3) == 1 ? 1 : 2;
    return std::to_string(a) + " vs " + std::to_string(b);
}

// A small tag in the style of the screen's plates: a dark blue rounded band with white bold letters.
void tag(float x0, float y0, float x1, float y1, const char* s, float a) {
    g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
        const float edge = std::min({x - x0, x1 - x, 1.5f}) / 1.5f;
        const float t = (y - y0) / (y1 - y0);
        return LayerColor{50 + 30 * (1 - t), 46 + 30 * (1 - t), 120 + 40 * (1 - t), a * 0.92f * std::max(0.f, edge)};
    });
    const float em = 6.2f, w = g_layer.text(s, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(s, true, em, (x0 + x1) / 2.f - w / 2.f, y1 - (y1 - y0 - em * 0.72f) / 2.f - 0.4f, LayerColor{255, 255, 255, a});
}

// A button key of the bar, drawn like the screen's own: the face button as a dark disc with a white symbol (as its triangle), a shoulder
// button as a light key with a dark outline and letter (as its R). Returns the key's left edge.
float key(float right, const char* letter, bool square, float a) {
    if (square) {
        const float cx = right - 5.5f, cy = 15.4f, rad = 5.3f;
        g_layer.band(cx - rad - 1.f, cy - rad - 1.f, cx + rad + 1.f, cy + rad + 1.f, [&](float x, float y) {
            const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            return LayerColor{22, 22, 40, a * std::min(1.f, std::max(0.f, (rad - d) * 2.f))};
        });
        const float h = 2.5f, t = 0.75f;
        const LayerColor w{255, 255, 255, a};
        g_layer.band(cx - h, cy - h, cx + h, cy - h + t, [&](float, float) { return w; });
        g_layer.band(cx - h, cy + h - t, cx + h, cy + h, [&](float, float) { return w; });
        g_layer.band(cx - h, cy - h, cx - h + t, cy + h, [&](float, float) { return w; });
        g_layer.band(cx + h - t, cy - h, cx + h, cy + h, [&](float, float) { return w; });
        return cx - rad;
    }
    const float kx1 = right, kx0 = kx1 - 12.f, y0 = 9.8f, y1 = 21.f;
    g_layer.band(kx0, y0, kx1, y1, [&](float x, float y) {
        const float e = std::min({x - kx0, kx1 - x, y - y0, y1 - y});
        return e < 0.8f ? LayerColor{60, 60, 76, a} : LayerColor{236, 236, 242, a};
    });
    const float lw = g_layer.text(letter, true, 7.4f, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(letter, true, 7.4f, (kx0 + kx1) / 2.f - lw / 2.f, 18.6f, LayerColor{40, 40, 52, a});
    return kx0;
}
float bar_text(float right, const std::string& s, float a) {
    const float em = 7.0f, w = g_layer.text(s, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(s, true, em, right - w, 19.2f, LayerColor{40, 42, 65, a});
    return right - w;
}

const char* kFieldName[3] = {"STRENGTH", "LEVEL", "TYPE"};
// The TYPE names of the single player Tournament dialog (read off its screen): entry 1 = Random, entries 2-9 = builder index 0-7 = our TYPE 1-8.
const char* kTypeName[8] = {"Tactician", "Valiant", "Survivor", "Cautious", "Calm", "Extreme", "Conservative", "Vicious"};
std::string type_name(int t) { return t >= 1 && t <= 8 ? kTypeName[t - 1] : "Random"; }
// A CPU row's settings as a short tag ("S5 L-- Random": strength, level (-- = Auto), type).
std::string cfg_text(const uint8_t* c) { return "S" + std::to_string(c[0]) + " L" + (c[1] ? std::to_string(c[1]) : std::string("--")) + " " + type_name(c[2]); }
std::string field_value(int f, const uint8_t* c) {
    if (f == 0) return std::to_string(c[0]);
    if (f == 1) return c[1] ? std::to_string(c[1]) : "Auto";
    return type_name(c[2]);
}
void change_field(int f, int dir, bool big) {
    uint8_t* c = g_dlg_cfg;
    if (f == 0) c[0] = static_cast<uint8_t>(std::clamp(int(c[0]) + dir, 1, 9));
    else if (f == 1) c[1] = static_cast<uint8_t>(std::clamp(int(c[1]) + dir * (big ? 10 : 1), 0, 100));
    else c[2] = static_cast<uint8_t>((int(c[2]) + dir + 9) % 9);
    if (g_dlg_sp) dissidia_tagteam_sp_set_cfg(g_dlg_row, c); else dissidia_tagteam_set_cpu_cfg(g_dlg_row, c);
}
// The dialog: a dark blue panel in the style of the screen's plates, three rows "NAME  < value >", the selected row lit.
void dialog(uint8_t* rdram, recomp_context* ctx, uint32_t pressed, float dt, float a) {
    const float x0 = 150.f, x1 = 330.f, y0 = 78.f, y1 = 188.f;
    uint32_t b = g_raw_pad.load(std::memory_order_relaxed);
    if (pressed & (BTN_CIRCLE | BTN_CROSS | BTN_OPEN)) { g_dlg_open = false; play_se(rdram, ctx, 1); return; }
    if (pressed & BTN_UP) { g_dlg_field = (g_dlg_field + 2) % 3; play_se(rdram, ctx, 1); }
    if (pressed & BTN_DOWN) { g_dlg_field = (g_dlg_field + 1) % 3; play_se(rdram, ctx, 1); }
    const bool big = (b & (BTN_L | BTN_R)) != 0;
    int dir = 0;
    if (pressed & (BTN_RIGHT | BTN_LEFTK)) { dir = (pressed & BTN_RIGHT) ? 1 : -1; g_dlg_hold = -0.35f; }   // first repeat after 0.35 s
    else if (b & (BTN_RIGHT | BTN_LEFTK)) { g_dlg_hold += dt; if (g_dlg_hold >= 0.07f) { g_dlg_hold = 0.f; dir = (b & BTN_RIGHT) ? 1 : -1; } }
    if (dir) { const uint8_t before[3] = {g_dlg_cfg[0], g_dlg_cfg[1], g_dlg_cfg[2]}; change_field(g_dlg_field, dir, big); if (std::memcmp(before, g_dlg_cfg, 3)) play_se(rdram, ctx, 1); }
    g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
        const float edge = std::min({x - x0, x1 - x, y - y0, y1 - y, 2.f}) / 2.f;
        const float t = (y - y0) / (y1 - y0);
        return LayerColor{26 + 20 * (1 - t), 28 + 20 * (1 - t), 78 + 40 * (1 - t), 0.95f * a * std::max(0.f, edge)};
    });
    const char* title = "CPU SETTINGS";
    const float tw = g_layer.text(title, true, 8.2f, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(title, true, 8.2f, (x0 + x1) / 2.f - tw / 2.f, y0 + 13.f, LayerColor{255, 255, 255, a});
    for (int f = 0; f < 3; f++) {
        const float ry = y0 + 24.f + 24.f * float(f);
        if (f == g_dlg_field) g_layer.band(x0 + 8.f, ry, x1 - 8.f, ry + 20.f, [&](float, float y) { return LayerColor{120, 150, 230, 0.55f * a * (1.f - 0.4f * (y - ry) / 20.f)}; });
        g_layer.text(kFieldName[f], true, 7.6f, x0 + 16.f, ry + 14.f, LayerColor{235, 238, 255, a});
        const std::string v = std::string(f == g_dlg_field ? "< " : "  ") + field_value(f, g_dlg_cfg) + (f == g_dlg_field ? " >" : "");
        const float vw = g_layer.text(v, true, 7.6f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(v, true, 7.6f, x1 - 16.f - vw, ry + 14.f, LayerColor{255, 255, 255, a});
    }
    const char* hint = "Up/Down  Left/Right  (L/R: x10)  Select: close";
    const float hw = g_layer.text(hint, true, 5.6f, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(hint, true, 5.6f, (x0 + x1) / 2.f - hw / 2.f, y1 - 5.f, LayerColor{200, 205, 240, a});
}

void draw(uint8_t* rdram, recomp_context* ctx, uint32_t screen) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_last).count());
    g_last = now;
    g_appear = dt >= 0.1f ? 0.f : std::min(1.f, g_appear + dt / 0.25f);
    const float a = g_appear;
    uint8_t regs[5] = {};
    const int n = dissidia_tagteam_local_regs(regs);
    uint32_t b = g_raw_pad.load(std::memory_order_relaxed);
    const uint32_t pressed = b & ~g_prev_buttons;
    g_prev_buttons = b;
    // Square on a registered row from the second on: CPU <-> the player's own.
    const bool left = r32(screen + SCR_COLUMN) == 0u;
    const int row = static_cast<int>(r32(screen + SCR_ROW));
    const uint8_t own = dissidia_tagteam_local_own_mask();
    int first = -1; for (int i = 0; i < 5 && first < 0; i++) if ((own >> i) & 1) first = i;
    const bool switchable = left && row >= 0 && row < n && row < 5 && ((own >> row) & 1) && (row != first || dissidia_tagteam_cpu_party_on());   // the player's own rows after his first (the first too: a CPU first row makes him the keeper of a CPU party)
    const bool cpu_row = switchable && ((dissidia_tagteam_local_cpu_mask() >> row) & 1) && row != first;   // (the settings of a keeper's first row are not in the beacon bytes: default strength 5 / Auto / Random)
    if (g_dlg_open && !cpu_row) g_dlg_open = false;   // the row went away / is a player again
    const bool free_row = left && n >= 0 && n < 5 && row == n && !g_dlg_open && dissidia_tagteam_cpu_party_on();   // the cursor is on the next free row: L = Add CPU
    if (free_row && (pressed & BTN_L)) { dissidia_tagteam_add_cpu_request(true); g_inject_cross.store(3, std::memory_order_relaxed); play_se(rdram, ctx, 1); }
    bool opened_now = false;
    if (!g_dlg_open && cpu_row && (pressed & BTN_OPEN)) { opened_now = true; g_dlg_sp = false; g_dlg_row = row; g_dlg_field = 0; g_dlg_hold = 0.f; dissidia_tagteam_get_cpu_cfg(row, g_dlg_cfg); g_dlg_open = true; play_se(rdram, ctx, 1); }
    if (!g_dlg_open && switchable && (pressed & BTN_SQUARE)) { dissidia_tagteam_toggle_cpu_row(row); play_se(rdram, ctx, 1); }
    const uint8_t mask = dissidia_tagteam_local_cpu_mask();
    g_layer.begin(std::max(2, psp_sddialog::output_scale()));
    for (int i = 0; i < n && i < 5; i++) if ((mask >> i) & 1) {   // the CPU rows (the first too, a CPU party's keeper)
        const float y = kRowY0 + kRowPitch * float(i);
        tag(139.f, y + 5.f, 160.f, y + 14.f, "CPU", a);   // after the job name (the left column's hand covers x 45..80 of the row)
    }
    const int dside = dissidia_tagteam_defect_side();   // this player keeps a CPU party: his player rows fight for the other party
    if (dside >= 0) {
        for (int i = 0; i < n && i < 5; i++) if (((own >> i) & 1) && !((mask >> i) & 1)) { const float y = kRowY0 + kRowPitch * float(i); tag(121.f, y + 5.f, 160.f, y + 14.f, dside == 0 ? "-> Party A" : "-> Party B", a); }
        const std::string note = std::string("You keep the CPU party: CPU rows fight for Party ") + (dside == 0 ? "B" : "A") + ", Player rows for Party " + (dside == 0 ? "A" : "B");
        const float em = 5.6f, w = g_layer.text(note, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(note, true, em, 240.f - w / 2.f, 266.f, LayerColor{60, 60, 110, a});
    }
    // CPU rows' settings, read-only: this player's party on the left column (the player's own rows and the party mates' rows), the other party's on the right edge
    const int me_party = dissidia_tagteam_local_party();
    if (me_party == 0 || me_party == 1) for (int col = 0; col < 2; col++) for (int i = 0; i < 5; i++) {
        uint8_t c[3]; bool cpu = false;
        if (col == 0 && i < n && ((mask >> i) & 1)) { dissidia_tagteam_get_cpu_cfg(i, c); cpu = true; }   // (own rows: live, before the beacon round trip)
        else cpu = dissidia_tagteam_menu_cpu_row(col == 0 ? me_party : 1 - me_party, i, c) != 0;
        if (!cpu) continue;
        const float y = kRowY0 + kRowPitch * float(i);
        if (col == 0 && !((i < n) && ((mask >> i) & 1))) tag(139.f, y + 5.f, 160.f, y + 14.f, "CPU", a);   // a party mate's CPU row also gets the plate
        const std::string t = col == 0 ? cfg_text(c) : "CPU " + cfg_text(c);
        const float em = 4.6f, w = g_layer.text(t, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
        const float x0 = col == 0 ? kColX0 + 1.f : 478.f - w - 5.f;
        tag(x0, y + 9.f, x0 + w + 5.f, y + 16.f, "", a);
        g_layer.text(t, true, em, x0 + 2.5f, y + 14.4f, LayerColor{255, 255, 255, a});
    }
    if (n >= 1 && n < 5) {   // the next free row: it adds a character (yours; Square makes it a CPU)
        const float y = kRowY0 + kRowPitch * float(n);
        const char* hint = dissidia_tagteam_cpu_party_on() ? "Add a character   (L: Add CPU)" : "Add a character";
        const float w = g_layer.text(hint, true, 7.0f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(hint, true, 7.0f, (kColX0 + kColX1) / 2.f + 12.f - w / 2.f, y + 12.6f, LayerColor{92, 88, 128, 0.85f * a});
    }
    // The button bar, right to left from the screen's own R hint: Square (on a switchable row), then the host's choice.
    float right = 318.f;
    if (cpu_row && !g_dlg_open) right = bar_text(right, "Select: CPU Settings", a) - 8.f;
    if (switchable) {
        right = bar_text(right, (mask >> row) & 1 ? "Player" : "CPU", a);
        right = key(right - 3.f, "", true, a) - 8.f;
    }
    // Three or more players: the host's choice of fighters per side.
    const int players = dissidia_tagteam_group_size();
    if (players >= 3 || std::getenv("PSPRECOMP_TAGTEAM_SELECTOR")) {
        uint8_t m = dissidia_tagteam_mode_byte();
        const bool host = dissidia_tagteam_is_host();
        if (host && (pressed & BTN_L) && !free_row) {
            int k = 0; for (int i = 0; i < 4; i++) if (kModes[i] == m) k = i;
            m = kModes[(k + 1) % 4];
            dissidia_tagteam_set_mode_byte(m);
            play_se(rdram, ctx, 1);
        }
        right = bar_text(right, "Tag Team " + mode_label(m ? m : kModes[0]), a);
        if (host) key(right - 3.f, "L", false, a);
    }
    if (g_dlg_open) dialog(rdram, ctx, opened_now ? 0u : pressed, dt, a);
    g_layer.publish();
}


void setup_tick_hook(uint8_t* rdram, recomp_context* ctx) {
    if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(SETUP_TICK)) f(rdram, ctx); return; }
    const uint32_t screen = static_cast<uint32_t>(ctx->r[4]);
    g_orig_tick(rdram, ctx);
    if (std::getenv("PSPRECOMP_TTCPU_CURDUMP")) {   // debug: small words of the screen object that changed this frame
        static std::vector<uint32_t> prev; static uint32_t prev_scr = 0;
        std::vector<uint32_t> now(0x8000 / 4); std::memcpy(now.data(), rdram + (screen & 0x07FFFFFFu), 0x8000);
        if (prev_scr == screen && prev.size() == now.size()) {
            std::string out; char b[48];
            for (size_t i = 0; i < now.size(); i++) if (now[i] != prev[i] && now[i] < 16 && prev[i] < 16) { std::snprintf(b, sizeof b, " +%zX:%u>%u", i * 4, prev[i], now[i]); out += b; }
            if (!out.empty()) std::fprintf(stderr, "[TTCPU] screen %08X:%s\n", screen, out.c_str());
        }
        prev = now; prev_scr = screen;
    }
    if (!dissidia_tagteam_group_on()) return;
    dissidia_tagteam_gb_setup_frame(r32(screen + SCR_COLUMN) == 0u ? static_cast<int>(r32(screen + SCR_ROW)) : -1);
    { static uint32_t pc = ~0u, pr = ~0u, ps = ~0u; const uint32_t col = r32(screen + SCR_COLUMN), rw = r32(screen + SCR_ROW), M = r32(screen + SCR_M), st = (M >= 0x08800000u && M < 0x0A000000u) ? r32(M + 4u) : 0xFFFFFFFFu;
      if (col != pc || rw != pr || st != ps) { pc = col; pr = rw; ps = st; std::fprintf(stderr, "[TTGB] setup screen %08X: column %u row %u state %u\n", screen, col, rw, st); } }
    if (dissidia_tagteam_add_cpu_pending()) {   // "Add CPU": Player Select was opened for the new row; if it closes without a row (Circle) nothing stays pending
        static bool seen = false; static int calm = 0;
        const uint32_t M = r32(screen + SCR_M), st = (M >= 0x08800000u && M < 0x0A000000u) ? r32(M + 4u) : 7u;
        if (st != 7u) { seen = true; calm = 0; } else if (seen && ++calm > 20) { dissidia_tagteam_add_cpu_request(false); seen = false; calm = 0; std::fprintf(stderr, "[TTCPU] Add CPU: Player Select was closed without a character\n"); }
    }
    draw(rdram, ctx, screen);
}


// ---- single player tag mode: the round dialog of matchups 2.. ----
// The tag match is ONE continuous battle: only the first matchup's STAGE / RULES / JUDGEMENT apply. In tag mode the round dialog of the 2nd round on keeps its STRENGTH / LEVEL /
// TYPE rows (they apply per enemy member) and the others are covered with the panel's own colour; the pad filter keeps the cursor off them (Up on STRENGTH and Down on TYPE are
// swallowed, a dialog that opens on STAGE gets one Down). (Writing the cursor row itself desyncs the dialog's highlight bar.)
bool g_sp_dlg_ran = false; int g_sp_hide_round = -1; std::atomic<int> g_sp_row{-1};   // g_sp_row: the dialog cursor row while the rows are hidden, else -1 (read by the pad filter)
bool sp_hidden_row(int r) { return r == 0 || r == 4 || r == 5; }
void sp_dlg_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t screen = static_cast<uint32_t>(ctx->r[4]);
    g_orig_sp_dlg(rdram, ctx);
    if (!dissidia_tagteam_sp_tag_on()) return;
    const uint32_t M = r32(screen + SCR_M); if (M < 0x08800000u || M >= 0x0A000000u) return;
    const uint32_t round = r32(M + 1444u); const int row = static_cast<int>(r32(M + 1488u));
    g_sp_dlg_ran = true; g_sp_hide_round = round >= 1u && round < 5u ? static_cast<int>(round) : -1;
    g_sp_row.store(g_sp_hide_round >= 0 ? row : -1, std::memory_order_relaxed);
}
// ---- Single player: CPU rows on the Tournament screen (docs/tag-team.md "Single player: CPU rows") ----
// Square on one of the player's matchup rows from the 2nd on makes his member of that row a CPU partner and back; Select on a CPU row opens the CPU
// settings dialog (the same one as Communications). Drawn on the game layer: the "CPU" plate after the member's job name, the bar's hints.
bool g_sp_layer_up = false;
void sp_dump(uint32_t screen) {   // debug (PSPRECOMP_TTSP_CURDUMP=1): small words of the screen object and of M that changed this frame
    static std::vector<uint32_t> prev, prevm; static uint32_t prev_scr = 0;
    const uint32_t M = r32(screen + SCR_M);
    std::vector<uint32_t> now(0x8000 / 4), nowm(0x800 / 4, 0); std::memcpy(now.data(), g_ram + (screen & 0x07FFFFFFu), 0x8000);
    if (M >= 0x08800000u && M < 0x0A000000u) std::memcpy(nowm.data(), g_ram + (M & 0x07FFFFFFu), 0x800);
    if (prev_scr == screen && prev.size() == now.size()) {
        std::string out; char b[48];
        for (size_t i = 0; i < now.size(); i++) if (now[i] != prev[i] && now[i] < 16 && prev[i] < 16) { std::snprintf(b, sizeof b, " +%zX:%u>%u", i * 4, prev[i], now[i]); out += b; }
        for (size_t i = 0; i < nowm.size(); i++) if (nowm[i] != prevm[i] && nowm[i] < 16 && prevm[i] < 16) { std::snprintf(b, sizeof b, " M+%zu:%u>%u", i * 4, prevm[i], nowm[i]); out += b; }
        if (std::getenv("PSPRECOMP_TTSP_CURDUMP")[0] == '2') { int cnt = 0; std::string all; for (size_t i = 0; i < now.size(); i++) if (now[i] != prev[i]) { cnt++; std::snprintf(b, sizeof b, " +%zX:%X>%X", i * 4, prev[i], now[i]); all += b; } if (cnt && cnt <= 10) std::fprintf(stderr, "[TTSP] ALL%s\n", all.c_str()); }
        if (!out.empty()) std::fprintf(stderr, "[TTSP] screen %08X M %08X:%s\n", screen, M, out.c_str());
    }
    prev = now; prevm = nowm; prev_scr = screen;
}
void sp_partyscan(uint32_t screen) {   // debug (PSPRECOMP_TTSP_PARTYSCAN="c0,c1,c4"): where are the player's party members (first, second, fifth) while the screen runs
    static bool done = false; if (done) return; done = true;
    int c0 = 0, c1 = 0, c4 = 0; if (std::sscanf(std::getenv("PSPRECOMP_TTSP_PARTYSCAN"), "%d,%d,%d", &c0, &c1, &c4) != 3) return;
    for (uint32_t a = 0x08800000u; a < 0x09FF0000u; a++) {
        if (g_ram[a & 0x07FFFFFFu] != c0) continue;
        for (uint32_t st : {1u, 2u, 4u, 8u, 12u, 16u, 20u, 24u, 32u, 40u, 48u, 64u, 0x78u, 0x80u, 0xC6u, 0x100u, 0x778u})
            if (g_ram[(a + st) & 0x07FFFFFFu] == c1 && g_ram[(a + 4u * st) & 0x07FFFFFFu] == c4) std::fprintf(stderr, "[TTSP] party at %08X stride %u (screen %08X, %+d): %u %u %u %u %u\n", a, st, screen, static_cast<int>(a - screen),
                g_ram[a & 0x07FFFFFFu], g_ram[(a + st) & 0x07FFFFFFu], g_ram[(a + 2u * st) & 0x07FFFFFFu], g_ram[(a + 3u * st) & 0x07FFFFFFu], g_ram[(a + 4u * st) & 0x07FFFFFFu]);
    }
}

// ---- Single player Tournament screen labels (19 Nov, tree ttlock): the format bar reads "Assists: ON / OFF" and each matchup row shows who plays that member instead of the stage ----
// Both are drawn over the game's own text with the bar's colours measured on screenshots (the bar interior is flat vertically, a plain horizontal gradient).
// PSPRECOMP_TAGTEAM_SPLABELS_OFF=1: the game's "Tournament" / "Random" stay.
constexpr uint32_t PLAYER_NAME_ADDR = 0x08BCEB40u;   // the save's player name, UTF-16 (same address as discord_presence.cpp)
std::string sp_player_name() {
    std::string out;
    for (int i = 0; i < 16; i++) {
        uint16_t c; std::memcpy(&c, g_ram + ((PLAYER_NAME_ADDR + 2u * i) & 0x07FFFFFFu), 2);
        if (c == 0) break;
        if (c < 0x20 || c == 0xFFFF) return "Player";
        out += c < 0x80 ? char(c) : '?';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? "Player" : out;
}
LayerColor lerp_col(float x, float x0, float x1, const float c0[3], const float c1[3], float a = 1.f) {
    const float t = std::clamp((x - x0) / (x1 - x0), 0.f, 1.f);
    return LayerColor{c0[0] + (c1[0] - c0[0]) * t, c0[1] + (c1[1] - c0[1]) * t, c0[2] + (c1[2] - c0[2]) * t, a};
}
void sp_label(float cx, float baseline, const std::string& txt, float em, LayerColor col, float maxw) {
    std::string t = txt;
    while (t.size() > 1 && g_layer.text(t.c_str(), true, em, 0, 0, LayerColor{0, 0, 0, 0}, true) > maxw) t.pop_back();
    const float w = g_layer.text(t.c_str(), true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(t.c_str(), true, em, cx - w / 2.f, baseline, col);
}
void sp_format_bar(bool assists) {   // "Tournament" / "Round-Robin" -> "Assists: ON / OFF"
    static const float a0[3] = {114, 123, 161}, a1[3] = {124, 134, 167};
    g_layer.band(198.f, 103.f, 284.f, 115.f, [&](float x, float) { return lerp_col(x, 200.f, 300.f, a0, a1); });
    sp_label(240.f, 112.f, assists ? "Assists: ON" : "Assists: OFF", 8.f, LayerColor{255, 255, 255, 1.f}, 80.f);
}
// The enemy side's CPU strength of round i: the round dialog keeps its STRENGTH in the screen object, one block of 0x16DC bytes per round (found with PSPRECOMP_TTSP_CURDUMP=2 as the
// word at +0x2134 / +0x3810 whose top byte was 4 > 3 > 2 / 4 > 5 while the dialog's STRENGTH of round 2 / 3 moved; tier = screen value - 1).
int sp_enemy_strength(uint32_t screen, int i) {
    const int tier = g_ram[(screen + 0xA5Bu + 0x16DCu * uint32_t(i)) & 0x07FFFFFFu];
    return tier >= 0 && tier <= 8 ? tier + 1 : 0;
}
void sp_row_label(int i, bool hi, const std::string& ours, const std::string& foe) {   // the stage text of matchup row i: our side on the left, the enemy's on the right
    static const float n0[3] = {194, 200, 216}, n1[3] = {160, 166, 179}, h0[3] = {64, 49, 211}, h1[3] = {37, 19, 158};
    const float y0 = 123.f + 20.f * float(i), y1 = y0 + 11.f;
    g_layer.band(246.f, y0, 342.f, y1, [&](float x, float) { return hi ? lerp_col(x, 276.f, 380.f, h0, h1) : lerp_col(x, 276.f, 380.f, n0, n1); });
    const LayerColor col = hi ? LayerColor{255, 255, 255, 1.f} : LayerColor{58, 60, 76, 1.f};
    sp_label(293.f, y0 + 9.f, ours, 7.5f, col, 62.f);
    if (!foe.empty()) {
        g_layer.band(336.f, y0 + 2.f, 337.f, y1 - 1.f, [&](float, float) { return LayerColor{col.r, col.g, col.b, 0.5f}; });
        sp_label(363.f, y0 + 9.f, foe, 7.5f, col, 46.f);
    }
}
void sp_hide_bands() {   // the round dialog of matchups 2..: STAGE / RULES / JUDGEMENT covered
    for (const auto& r : {std::pair<float, float>{103.f, 125.f}, std::pair<float, float>{183.f, 229.f}})
        g_layer.band(66.f, r.first, 414.f, r.second, [&](float, float) { return LayerColor{208, 210, 219, 1.f}; });
}
void sp_draw(uint8_t* rdram, recomp_context* ctx, uint32_t screen, bool hide) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_last).count());
    g_last = now;
    g_appear = dt >= 0.1f ? 0.f : std::min(1.f, g_appear + dt / 0.25f);
    const float a = g_appear;
    uint8_t rows[5] = {};
    const int n = dissidia_tagteam_sp_rows(screen, rows);
    const uint32_t b = g_raw_pad.load(std::memory_order_relaxed);
    const uint32_t pressed = b & ~g_prev_buttons;
    g_prev_buttons = b;
    const uint32_t M = r32(screen + SCR_M);
    const bool mok = M >= 0x08800000u && M < 0x0A000000u;
    const uint32_t state = mok ? r32(M + 4u) : 0xFFFFFFFFu;
    static const uint32_t kListState = std::getenv("PSPRECOMP_TTSP_LISTSTATE") ? static_cast<uint32_t>(std::atoi(std::getenv("PSPRECOMP_TTSP_LISTSTATE"))) : 2u;   // M + 4: 2 = the matchup list, 3 / 4 = the round dialog opening / open (seen with PSPRECOMP_TTSP_CURDUMP)
    const bool list = mok && state == kListState;   // the matchup list has the cursor (not the round dialog, Player Select, ...)
    const int row = mok ? static_cast<int>(r32(M + 1444u)) : -1;
    const bool switchable = list && row >= 1 && row < n && row < 5;
    { static int pn = -1, prow = -9; static uint32_t pst = 0; if (n != pn || row != prow || state != pst) { pn = n; prow = row; pst = state;
        std::fprintf(stderr, "[TTSP] setup screen: %d rows (%d %d %d %d %d), state %u, cursor row %d\n", n, rows[0], rows[1], rows[2], rows[3], rows[4], state, row); } }
    if (std::getenv("PSPRECOMP_TTSP_GNODES")) {   // debug: the group struct's type-5 commands (CPU side) while the screen runs
        static std::string prev; const uint32_t base = r32(0x089D8508u); std::string cur; char bb[64];
        if (base >= 0x08800000u && base < 0x0A000000u) for (int i = 0; i < 10; i++) for (int j = 0; j < 23; j++) {
            const uint32_t c = base + 104u + 744u * uint32_t(i) + 0x20u * uint32_t(j); uint16_t t; std::memcpy(&t, g_ram + (c & 0x07FFFFFFu), 2);
            if (t == 5 || t == 0) { std::snprintf(bb, sizeof bb, " n%d.%d t%u:", i, j, t); cur += bb; for (int k = 0; k < 8; k++) { std::snprintf(bb, sizeof bb, "%d,", int(int8_t(g_ram[(c + 0x10u + k) & 0x07FFFFFFu]))); cur += bb; } }
        }
        if (cur != prev) { prev = cur; std::fprintf(stderr, "[TTGN] state %u cursor %d base %08X:%s\n", state, row, base, cur.c_str()); }
    }
    const uint8_t mask0 = dissidia_tagteam_sp_cpu_mask();
    const bool cpu_row = switchable && ((mask0 >> row) & 1);
    if (g_dlg_open && (!cpu_row || row != g_dlg_row)) g_dlg_open = false;
    bool opened_now = false;
    if (!g_dlg_open && cpu_row && (pressed & BTN_OPEN)) { opened_now = true; g_dlg_sp = true; g_dlg_row = row; g_dlg_field = 0; g_dlg_hold = 0.f; dissidia_tagteam_sp_get_cfg(row, g_dlg_cfg); g_dlg_open = true; play_se(rdram, ctx, 1); }
    if (!g_dlg_open && switchable && (pressed & BTN_L)) { dissidia_tagteam_sp_toggle(row); play_se(rdram, ctx, 1); }   // L (Square: Assist On / Off, below)
    const bool sq_ours = list && !std::getenv("PSPRECOMP_TAGTEAM_ASSISTTOGGLE_OFF") && !(std::getenv("PSPRECOMP_TAGTEAM_ASSISTTOGGLE") && std::getenv("PSPRECOMP_TAGTEAM_ASSISTTOGGLE")[0] == '0');
    g_sp_sq_block.store(sq_ours, std::memory_order_relaxed); g_sq_stamp.store(g_pad_polls.load(std::memory_order_relaxed), std::memory_order_relaxed);   // the pad filter hides Square from the screen (no "Swap format" = no Round-Robin) while the list has the cursor
    if (!g_dlg_open && sq_ours && (pressed & BTN_SQUARE)) { dissidia_tagteam_sp_assist_toggle(); play_se(rdram, ctx, 1); }
    const uint8_t mask = dissidia_tagteam_sp_cpu_mask();
    const bool labels = mok && (list || g_dlg_open) && !std::getenv("PSPRECOMP_TAGTEAM_SPLABELS_OFF");
    if (!mask && !switchable && !sq_ours && !g_sp_layer_up && !hide && !labels) return;   // nothing to draw (the layer is left to the screen's other users)
    g_layer.begin(std::max(2, psp_sddialog::output_scale()));
    if (hide) sp_hide_bands();
    if (labels) {
        sp_format_bar(dissidia_tagteam_sp_assist_on());
        const std::string me = sp_player_name();
        for (int i = 0; i < n && i < 5; i++) {
            std::string t = me;
            if ((mask >> i) & 1) { uint8_t cf[3]; dissidia_tagteam_sp_get_cfg(i, cf); t = "CPU STR " + std::to_string(cf[0]); }
            const int es = sp_enemy_strength(screen, i);
            sp_row_label(i, row == i, t, es ? "CPU STR " + std::to_string(es) : std::string());
        }
    }
    if (mok && (list || g_dlg_open)) {
        for (int i = 1; i < n && i < 5; i++) if ((mask >> i) & 1) {
            const float y = kRowY0 + kRowPitch * float(i);
            tag(141.f, y + 5.f, 162.f, y + 14.f, "CPU", a);   // after the member's job name, before the matchup's "2nd"
        }
        float right = 334.f;   // the bar at the top, left of the screen's own hints ("Swap format" from x ~341, "R Job Settings"; user screenshot 18 Nov: at 370 ours covered "Swap format")
        if (sq_ours && !g_dlg_open) { right = bar_text(right, dissidia_tagteam_sp_assist_on() ? "Assist: On" : "Assist: Off", a); key(right - 3.f, "", true, a); right -= 20.f; }   // Square (not drawn by the game: its "Swap format" is blocked); host-unverified placement
        if (cpu_row && !g_dlg_open) right = bar_text(right, "Select: CPU Settings", a) - 8.f;
        if (switchable && !g_dlg_open) { right = bar_text(right, (mask >> row) & 1 ? "Player" : "CPU", a); key(right - 3.f, "L", false, a); }
        if (g_dlg_open) dialog(rdram, ctx, opened_now ? 0u : pressed, dt, a);
    }
    g_layer.publish(); g_sp_layer_up = mask || switchable || sq_ours || g_dlg_open || hide || labels;
}
void sp_tick_hook(uint8_t* rdram, recomp_context* ctx) {
    if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(SP_TICK)) f(rdram, ctx); return; }
    const uint32_t screen = static_cast<uint32_t>(ctx->r[4]);
    g_sp_dlg_ran = false; g_sp_hide_round = -1;
    g_orig_sp_tick(rdram, ctx);
    if (!g_sp_dlg_ran) g_sp_row.store(-1, std::memory_order_relaxed);
    const bool hide = g_sp_dlg_ran && g_sp_hide_round >= 0;
    if (std::getenv("PSPRECOMP_TTSP_CURDUMP")) sp_dump(screen);
    if (std::getenv("PSPRECOMP_TTSP_PARTYSCAN")) sp_partyscan(screen);
    if (std::getenv("PSPRECOMP_TAGTEAM_NOSPCPU") || !dissidia_tagteam_sp_mode()) {
        if (g_dlg_open && g_dlg_sp) g_dlg_open = false;
        if (hide || g_sp_layer_up) { g_layer.begin(std::max(2, psp_sddialog::output_scale())); if (hide) sp_hide_bands(); g_layer.publish(); g_sp_layer_up = hide; }
        return;
    }
    sp_draw(rdram, ctx, screen, hide);
}

}  // namespace

uint32_t dissidia_tagteam_menu_pad(uint32_t raw) {
    g_raw_pad.store(raw, std::memory_order_relaxed);
    const int sr = g_sp_row.load(std::memory_order_relaxed);
    if (sr >= 0) {   // single player tag mode, round dialog of the 2nd round on: the cursor stays on STRENGTH / LEVEL / TYPE
        static bool inj = false;
        if (sr == 0) { inj = !inj; raw = inj ? (raw | BTN_DOWN) : (raw & ~BTN_DOWN); }   // opened on STAGE: one Down (pressed on alternate frames until the cursor moved)
        else if (sr == 1) raw &= ~BTN_UP;
        else if (sr == 3) raw &= ~BTN_DOWN;
    }
    if (int ij = g_inject_cross.load(std::memory_order_relaxed); ij > 0) { g_inject_cross.store(ij - 1, std::memory_order_relaxed); raw = (ij == 2) ? (raw | BTN_CROSS) : (raw & ~BTN_CROSS); }   // Add CPU: one Cross press (edge) for the screen
    if (g_sp_sq_block.load(std::memory_order_relaxed) && g_pad_polls.fetch_add(1, std::memory_order_relaxed) - g_sq_stamp.load(std::memory_order_relaxed) < 40u) raw &= ~BTN_SQUARE;
    else g_pad_polls.fetch_add(1, std::memory_order_relaxed);
    return g_dlg_open.load(std::memory_order_relaxed) ? 0u : raw;
}

void dissidia_tagteam_menu_install(uint8_t* rdram) {
    g_ram = rdram;
    if (std::getenv("PSPRECOMP_MENU_NOTAGTEAM")) return;
    g_orig_tick = ovl_menu_lookup(SETUP_TICK);
    g_play_se = psp_dispatch_probe_lookup(PLAY_SE);
    if (!g_orig_tick) { std::fprintf(stderr, "[TTCPU] setup screen hook not installed\n"); return; }
    psp_dispatch_register(SETUP_TICK, setup_tick_hook);
    // single player Tournament screen: the round dialog of matchups 2.. (tag mode) and the CPU rows (docs/tag-team.md "Single player: CPU rows"), one tick hook
    g_orig_sp_tick = ovl_menu_lookup(SP_TICK); g_orig_sp_dlg = ovl_menu_lookup(SP_DLG);
    if (g_orig_sp_tick) psp_dispatch_register(SP_TICK, sp_tick_hook);
    else std::fprintf(stderr, "[TTSP] Tournament screen hook not installed\n");
    if (g_orig_sp_dlg && !std::getenv("PSPRECOMP_TAGTEAM_SPDLG_OFF"))
        psp_dispatch_register(SP_DLG, [](uint8_t* rdram, recomp_context* ctx) { if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(SP_DLG)) f(rdram, ctx); return; } sp_dlg_hook(rdram, ctx); });
}
