// games/dissidia/runtime/menu_settings.cpp — a "Settings" entry in the game's own menus that opens the runtime's settings
// screen (psp_settings_request_open). The entries are made by the game's own menu code, so they look and behave like the
// retail ones (same bars, font, cursor, help line, sounds).
//
// Battle pause menu
//   ovl_battle FUN_08B1EBE8 builds the pause list for the battle's mode with the generic EBOOT list widget:
//   FUN_088D72E8(list, label id, help id, 0, 0, 0) adds an item (the widget holds up to 7, count at list+92+480), then
//   FUN_088D743C opens it. The wrapper adds one more item just before the open. When the list opens, every label and the
//   help line are made with FUN_0887AD78(bank, 0, id, ...) -> FUN_0887D868(text system, ..., UTF-16 string, ...); a
//   marker id makes the accessor hook create the text from a host string instead (the same call with our buffer).
//   The selection core (list + 36) registers a confirm in its update FUN_088D80AC; FUN_088D75F0(list) is the label id of
//   the selected row. A confirm of our row is taken back there and the settings screen opens over the paused game.
#include "psp_runtime.h"
#include "recomp.h"
#include "psp_settings.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_adhoc.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
extern FuncPtr ovl_menu_lookup(uint32_t vaddr);

namespace {

constexpr uint32_t OVL_BASE = 0x089DD4C0u;
constexpr uint32_t PAUSE_BUILD = 0x08B1EBE8u;   // ovl_battle: pause list for the current mode
constexpr uint32_t LIST_ADD = 0x088D72E8u;      // EBOOT list widget: add item (list, label, help, 0, 0, 0)
constexpr uint32_t LIST_OPEN = 0x088D743Cu;     // EBOOT list widget: open (list, ?, start label, 0, flag)
constexpr uint32_t CORE_UPDATE = 0x088D80ACu;   // EBOOT list widget: selection core update (pad input, confirm / cancel)
constexpr uint32_t LIST_VALUE = 0x088D75F0u;    // EBOOT list widget: label id of the selected row (-1 none)
constexpr uint32_t LIST_COUNT = 0x088D768Cu;    // EBOOT list widget: item count
constexpr uint32_t TEXT_GET = 0x0887AD78u;      // EBOOT text bank accessor: (bank, kind, id, a3, t0, t1) -> text object
constexpr uint32_t TEXT_MAKE = 0x0887D868u;     // EBOOT: text object from a UTF-16 string
constexpr uint32_t TEXT_SYS = 0x089D6998u;      // EBOOT: pointer to the text system (first argument of TEXT_MAKE)
constexpr int kMaxItems = 7;

constexpr int32_t ID_LABEL = 0x7E5E;            // marker ids: no retail bank is that large
constexpr int32_t ID_HELP = 0x7E5F;
constexpr uint32_t STR_LABEL = 0x08701D00u;     // guest UTF-16 buffers (scratch area, see roster_max.h)
constexpr uint32_t STR_HELP = 0x08701D40u;

const char* const kLabel = "Settings";
const char* const kHelp = "Display, graphics, controls and other settings of this port.";

uint8_t* g_ram = nullptr;
FuncPtr g_orig_build = nullptr, g_orig_open = nullptr, g_list_add = nullptr, g_orig_core_update = nullptr, g_list_value = nullptr,
        g_list_count = nullptr, g_orig_text = nullptr, g_text_make = nullptr;
bool g_in_pause_build = false;
uint32_t g_pause_list = 0;

uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
uint32_t overlay_id() { return r32(OVL_BASE); }

void put_utf16(uint32_t at, const char* s, size_t cap_units) {
    uint8_t* p = g_ram + (at & 0x07FFFFFFu);
    size_t i = 0;
    for (; s[i] && i + 1 < cap_units; i++) { p[2 * i] = static_cast<uint8_t>(s[i]); p[2 * i + 1] = 0; }
    p[2 * i] = p[2 * i + 1] = 0;
}

// Calls a guest function with the given arguments, leaving the caller's registers as they were (only v0 is returned).
int32_t call(FuncPtr f, uint8_t* rdram, recomp_context* ctx, std::initializer_list<int32_t> args) {
    recomp_context save = *ctx;
    int reg = 4;
    for (int32_t a : args) ctx->r[reg++] = a;
    f(rdram, ctx);
    const int32_t v0 = ctx->r[2];
    *ctx = save;
    return v0;
}

void pause_build_hook(uint8_t* rdram, recomp_context* ctx) {
    if (overlay_id() != 1) { if (FuncPtr f = ovl_menu_lookup(PAUSE_BUILD)) f(rdram, ctx); return; }
    g_in_pause_build = true;
    g_orig_build(rdram, ctx);
    g_in_pause_build = false;
}

void list_open_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_in_pause_build) {
        g_in_pause_build = false;
        const int32_t list = ctx->r[4];
        const int32_t n = call(g_list_count, rdram, ctx, {list});
        // Not in a wireless match (the settings screen gives the game back at once there) and only while the widget has room.
        if (n > 0 && n < kMaxItems && !psp_adhoc_connected() && !psp_adhoc_in_session()) {
            if (call(g_list_add, rdram, ctx, {list, ID_LABEL, ID_HELP, 0, 0, 0}) == 0) g_pause_list = static_cast<uint32_t>(list);
        }
    }
    g_orig_open(rdram, ctx);
}

// The selection core's per-frame update (list + 36; +12 state, 1 = taking input; +16 event, 1 = confirmed; +20 cursor) is
// where a confirm is registered (and the game's confirm sound played). Right after it, before the list widget reacts
// (its update starts the closing animation on a confirm), a confirm of our row is taken back and the settings screen opens.
void core_update_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t core = static_cast<uint32_t>(ctx->r[4]);
    g_orig_core_update(rdram, ctx);
    if (g_pause_list == 0 || core != g_pause_list + 36u || r32(core + 16) != 1) return;
    if (call(g_list_value, rdram, ctx, {static_cast<int32_t>(g_pause_list)}) != ID_LABEL) return;
    const uint32_t active = 1, none = 0;
    std::memcpy(rdram + (core & 0x07FFFFFFu) + 12, &active, 4);
    std::memcpy(rdram + (core & 0x07FFFFFFu) + 16, &none, 4);
    if (!psp_settings_menu_open()) {
        std::fprintf(stderr, "[MENUSET] Settings chosen in the pause menu\n");
        psp_settings_request_open();
    }
}

void text_get_hook(uint8_t* rdram, recomp_context* ctx) {
    const int32_t id = ctx->r[6];
    if ((id == ID_LABEL || id == ID_HELP) && g_text_make) {
        const uint32_t buf = id == ID_LABEL ? STR_LABEL : STR_HELP;
        put_utf16(buf, id == ID_LABEL ? kLabel : kHelp, id == ID_LABEL ? 32 : 96);
        // as FUN_0887AD78 does after decoding the record: FUN_0887D868(text system, kind, string, 0, 0, a3, t0, 0)
        const int32_t kind = ctx->r[5], a3 = ctx->r[7], t0 = ctx->r[8];
        ctx->r[4] = static_cast<int32_t>(r32(TEXT_SYS)); ctx->r[5] = kind; ctx->r[6] = static_cast<int32_t>(buf);
        ctx->r[7] = 0; ctx->r[8] = 0; ctx->r[9] = a3; ctx->r[10] = t0; ctx->r[11] = 0;
        g_text_make(rdram, ctx);
        return;
    }
    g_orig_text(rdram, ctx);
}

}  // namespace

void dissidia_menu_settings_install(uint8_t* rdram) {
    g_ram = rdram;
    if (const char* e = std::getenv("PSPRECOMP_MENU_SETTINGS")) if (e[0] == '0') return;
    g_orig_build = ovl_battle_lookup(PAUSE_BUILD);
    g_orig_open = psp_dispatch_probe_lookup(LIST_OPEN);
    g_list_add = psp_dispatch_probe_lookup(LIST_ADD);
    g_orig_core_update = psp_dispatch_probe_lookup(CORE_UPDATE);
    g_list_value = psp_dispatch_probe_lookup(LIST_VALUE);
    g_list_count = psp_dispatch_probe_lookup(LIST_COUNT);
    g_orig_text = psp_dispatch_probe_lookup(TEXT_GET);
    g_text_make = psp_dispatch_probe_lookup(TEXT_MAKE);
    if (!g_orig_build || !g_orig_open || !g_list_add || !g_orig_core_update || !g_list_value || !g_list_count || !g_orig_text || !g_text_make) {
        std::fprintf(stderr, "[MENUSET] pause menu hooks not installed (a function is missing)\n");
        return;
    }
    psp_dispatch_register(TEXT_GET, text_get_hook);
    psp_dispatch_register(PAUSE_BUILD, pause_build_hook);
    psp_dispatch_register(LIST_OPEN, list_open_hook);
    psp_dispatch_register(CORE_UPDATE, core_update_hook);
}
