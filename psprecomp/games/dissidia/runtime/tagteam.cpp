// games/dissidia/runtime/tagteam.cpp - tag team battles (2 v 2). See docs/tag-team.md.
//
// Spike: after the battle setup FUN_0888BA0C has built the two fighters, make more with the same constructor, cloning
// the existing fighters' setup data (so no new assets are needed): a second enemy and a partner for the human.
//   PSPRECOMP_TAGTEAM=1        turn the spike on
//   PSPRECOMP_TAGTEAM_EXTRAS=n 1 = the enemy's second fighter, 2 = the human's partner, 3 = both (default)
//   PSPRECOMP_TAGTEAM_LOG=1    log what the hook does
#include "psp_game_module.h"
#include "psp_runtime.h"
#include "recomp.h"
#include "psp_settings.h"
#include "tagteam_hud.h"
#include "hle/psp_hle.h"
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <map>
#include <utility>
#include <vector>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
void psp_dump_func_ring(const char* tag);   // runtime/src/psp_dispatch.cpp
void dissidia_model_pool_request_extra(uint32_t bytes);   // hooks_models.cpp: a bigger model pool for the extra characters
float dissidia_battle_age();   // framerate.cpp: ticks (1/60 s) since the battle started running
bool dissidia_cpu_ai_off();    // framerate.cpp: the debug menu's Enemy AI option / the test harness switched the CPU brains off
uint32_t dissidia_camera_manager();   // camera.cpp
void psp_mem_watch_set(uint32_t addr);   // runtime/src/psp_dispatch.cpp

extern "C" __attribute__((noinline)) void tagteam_hpm_marker(uint32_t att, uint32_t vic, int pi) { asm volatile("" :: "r"(att), "r"(vic), "r"(pi) : "memory"); }   // gdb breakpoint anchor (HPM windows)

namespace {

constexpr uint32_t BATTLE_SETUP = 0x0888BA0Cu;   // EBOOT: FUN_0888BA0C(bs), builds the fighters
constexpr uint32_t ALLOC = 0x08831320u;          // EBOOT: guest allocator (a0 = size)
constexpr uint32_t SIDE_REC = 0x088729A0u;       // EBOOT: per-side record (a0 = side)
constexpr uint32_t CHAR_INFO = 0x088470B8u;      // EBOOT: character info from a setup record (a0 = bs+0x7D8 / +0x7DC)
constexpr uint32_t FIGHTER_CTOR = 0x08B39E5Cu;   // battle overlay: fighter constructor (mem, siderec, info, side, 0)
constexpr uint32_t SET_FOE = 0x08836DA4u;        // EBOOT: a->+0x300 = b
constexpr uint32_t CPU_MODE = 0x08B2F2E4u;       // battle overlay: FUN_08B2F2E4(actor, 0), the setup's CPU-control call
constexpr uint32_t BRAIN_MODE = 0x08A640CCu;     // battle overlay: brain (actor +0x5C4) parameter 1 (record +3894)
constexpr uint32_t BRAIN_LEVEL = 0x08A64100u;    // battle overlay: brain parameter 2 (record +3896)
constexpr uint32_t ACT_BRAIN = 0x5C4u;
constexpr uint32_t FLOW_STEP = 0x088887A4u;      // EBOOT: battle flow step (a0 = bs): waits for both fighters, starts action 1 on each, bs+72 = 1
constexpr uint32_t FIGHTER_READY = 0x08B2F9E0u;  // battle overlay: bit 0x10 of byte actor+1456 (the fighter is ready)
constexpr uint32_t LOCK_SET = 0x08B3B0B4u;       // battle overlay: lock block: set the target (a0 = block, a1 = actor)
constexpr uint32_t BRAIN_TICK = 0x08A64400u;     // battle overlay: the CPU brain tick (a0 = brain, actor +0x5C4)
constexpr uint32_t TARGET_REFRESH = 0x08B2F17Cu; // battle overlay: copy the lock block's result into actor +0x2FC (a0 = actor)
constexpr uint32_t LOCK_OTHER = 0x08B40994u;     // battle overlay: the lock cycle's 2nd candidate, retail = the foe's assist (a0 = handler, a1 = lock block)
constexpr uint32_t LOCK_GET = 0x08B3B280u;       // battle overlay: the lock block's current target (a0 = block)
constexpr uint32_t LOCK_MODE = 0x08B3B3ECu;      // battle overlay: change the lock block's mode (a0 = block, a1 = mode, a2 = flag): L toggles 1 <-> 0 through it
constexpr uint32_t START_ACTION = 0x0882FA10u;   // EBOOT: start an action (a0 = actor + 304, a1 = id, a2 = 0, f12 = f13 = 0)
constexpr uint32_t REC_INIT = 0x08B01090u;       // battle overlay: fills the 40-byte init block (a0 = buf, a1 = side)
constexpr uint32_t LOCK_INIT = 0x08B3B1B8u;      // battle overlay: lock-on block init (a0 = *(actor + 0x5AC), a1 = 3 words)
constexpr uint32_t PLACE = 0x088359A8u;          // EBOOT: place an actor (a0 = actor, a1 = vec4, a2 = 1)
constexpr uint32_t REC_BASE = 0x09AFB8A0u;       // global per-side records (stride 0x1060), 0x09B00000 - 18272
constexpr uint32_t REG_PAIR0 = 0x08BAE9FCu, REG_PAIR1 = 0x08BAEA08u;   // ctor writes the new fighter here (see spawn_fighter)
constexpr uint32_t LAST_FIGHTER_OBJ = 0x089D4774u;   // EBOOT global the fighter constructor sets to its own controller-side object
constexpr uint32_t ACT_VT = 0x544u, ACT_FLAGS = 0x5B0u, ACT_LOCKBLK = 0x5ACu;

FuncPtr g_orig_setup = nullptr, g_orig_flow = nullptr, g_orig_tick = nullptr, g_orig_lock_other = nullptr, g_orig_refresh = nullptr, g_orig_lock_mode = nullptr, g_orig_stock = nullptr, g_orig_rel1 = nullptr, g_orig_rel2 = nullptr;
bool g_in_ctor = false;   // inside the extra's constructor: the resource releases are skipped (see rel_hook)
bool g_oneround = true;   // KO rules, one continuous round: a KO'd slot fighter is swapped with its living team mate at once, no stock hack / round restart (PSPRECOMP_TAGTEAM_ONEROUND=0: the old restart flow)
bool g_tagin = true;      // KO rules: a KO'd slot fighter with a living team mate is replaced by him (PSPRECOMP_TAGTEAM_TAGIN=0 turns it off)
uint32_t side_of(uint32_t a);
bool g_enabled = false;  // PSPRECOMP_TAGTEAM=1 or Settings > Debug > Tag Team (2v2): the next battle is a tag battle
bool g_log = false;
uint8_t* g_ram = nullptr;
recomp_context* g_ctx = nullptr;

// The extra fighters of the battle being set up. side 1 = the CPU's team (the "third"), side 0 = the human's (the partner).
struct Extra { uint32_t actor = 0, side = 0, foe = 0, slot_actor = 0; bool started = false; int hp_max = 0; std::vector<std::pair<uint32_t, uint32_t>> undo; };   // undo: (address, value) writes that take back what the constructor registered
Extra g_extra[2];
std::vector<std::pair<uint32_t, uint32_t>> g_last_undo;
int g_nextra = 0;
uint32_t g_bs = 0;
int g_mask = 3;
bool g_two_v_one = false;   // Settings > Debug > Tag Team 2v1: only the partner is added (you and a partner against one enemy)
bool g_one_v_two = false;   // Settings > Debug > Tag Team 1v2: only the enemy's second fighter is added (you alone against two); both 2v1 and 1v2 on = 2v2
int eff_mask() { return (g_two_v_one == g_one_v_two) ? g_mask : (g_two_v_one ? (g_mask & 2) : (g_mask & 1)); }
int g_stop = 99;   // PSPRECOMP_TAGTEAM_STOP=n: debugging, spawn steps up to n only (1 alloc, 2 ctor, 3 foe, 4 init, 5 brain, 6 start/place)
bool g_late_wait = false;   // PSPRECOMP_TAGTEAM_LATE=2: the late partner waits until the intro is over
bool g_late = false;   // PSPRECOMP_TAGTEAM_LATE=1: the partner is made once the battle has started (not during the intro setup)
struct Late { uint32_t side = 0, foe = 0, cpu_side = 0, info_slot = 0, slot_actor = 0; };
std::vector<Late> g_late_q;   // extras still to be made (after the intro)
bool g_late_ready = false;    // the intro is over, the queue may spawn   // PSPRECOMP_TAGTEAM_EXTRAS: 1 = enemy side fighter, 2 = partner on the human's side

uint32_t rd(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
uint32_t rdh(uint32_t a) { return static_cast<uint32_t>(g_ram[a & 0x07FFFFFFu]) | (static_cast<uint32_t>(g_ram[(a + 1) & 0x07FFFFFFu]) << 8); }
void wrh(uint32_t a, uint16_t v) { g_ram[a & 0x07FFFFFFu] = static_cast<uint8_t>(v & 0xFF); g_ram[(a + 1) & 0x07FFFFFFu] = static_cast<uint8_t>(v >> 8); }
float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void wr(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }
bool ok(uint32_t a, uint32_t n) { return a >= 0x08800000u && a + n <= 0x0C000000u; }

FuncPtr fn(uint32_t addr) { return addr >= 0x089DD4C0u ? ovl_battle_lookup(addr) : psp_dispatch_probe_lookup(addr); }

// Calls a guest function with up to 5 integer arguments and returns v0; the registers are restored afterwards.
uint32_t call(uint32_t addr, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0) {
    FuncPtr f = fn(addr);
    if (!f) { std::fprintf(stderr, "[TAGTEAM] no function at %08X\n", addr); return 0; }
    const recomp_context saved = *g_ctx;
    g_ctx->r[4] = static_cast<int32_t>(a0); g_ctx->r[5] = static_cast<int32_t>(a1); g_ctx->r[6] = static_cast<int32_t>(a2);
    g_ctx->r[7] = static_cast<int32_t>(a3); g_ctx->r[8] = static_cast<int32_t>(a4);
    f(g_ram, g_ctx);
    const uint32_t v = static_cast<uint32_t>(g_ctx->r[2]);
    *g_ctx = saved;
    return v;
}

void tick_hook(uint8_t* rdram, recomp_context* ctx);
const char* role_of(uint32_t a);
void fix_assist();
void stock_hook(uint8_t* rdram, recomp_context* ctx);
void lock_other_hook(uint8_t* rdram, recomp_context* ctx);
void refresh_hook(uint8_t* rdram, recomp_context* ctx);
void lock_mode_hook(uint8_t* rdram, recomp_context* ctx);
int g_cyc = 0; uint32_t g_cyc_target = 0;   // the human's lock cycle: 0 foe, 1 the other enemy (g_cyc_target), 2 EX core

// The fighter constructor first releases the resource handles of the side's slot object (FUN_0886CAA0: two handles at +288,
// FUN_0886A140: one at +268), the intro / outro camera data the retail fighter of that side is still using, and loads its own.
// For an extra on a side that already has a fighter that frees the retail fighter's data (its intro camera then reads
// nothing: eye and target 0). While an extra is being constructed the releases are skipped.
void rel1_hook(uint8_t* rdram, recomp_context* ctx) { if (g_in_ctor) { if (g_log) std::fprintf(stderr, "[TAGTEAM] release (two handles) of %08X skipped\n", static_cast<uint32_t>(ctx->r[4])); return; } g_orig_rel1(rdram, ctx); }
void rel2_hook(uint8_t* rdram, recomp_context* ctx) { if (g_in_ctor) { if (g_log) std::fprintf(stderr, "[TAGTEAM] release (one handle) of %08X skipped\n", static_cast<uint32_t>(ctx->r[4])); return; } g_orig_rel2(rdram, ctx); }

// Debug: every guest word outside the actor itself that holds the actor's address (who registered it).
void scan_refs(uint32_t actor, const char* tag) {
    int n = 0;
    for (uint32_t a = 0x08800000u; a < 0x0BF00000u && n < 60; a += 4) {
        if (a >= actor && a < actor + 0x12D0u) continue;
        if (rd(a) == actor) {
            std::fprintf(stderr, "[TAGTEAM] %s: %08X holds the actor\n", tag, a); n++;
            if (a < 0x09000000u) { std::fprintf(stderr, "[TAGTEAM]   around:"); for (int k = -6; k <= 6; k++) std::fprintf(stderr, " %08X", rd(a + 4 * k)); std::fprintf(stderr, "\n"); }
        }
    }
}

// One more fighter, the same steps as the setup's own for `side` (its character is the one in that side's setup data).
// `cpu_side` is the side whose record holds the CPU brain settings (record +3892 control type, +3894 / +3896 parameters).
// ---- ownership of a fighter's heap objects (for the content exchange of the tag swap) ----
// Every guest allocation made while a fighter is being built (its constructor FUN_08B39E5C, its per-fighter init, the actor block
// itself) is recorded against that actor, in order: clones allocate the same sizes in the same order, so the k-th block of one is the
// counterpart of the k-th block of the other.
struct Blk { uint32_t addr, size; };
std::map<uint32_t, std::vector<Blk>> g_owned;
uint32_t g_own_ctx = 0;
FuncPtr g_orig_alloc = nullptr, g_orig_ctor = nullptr, g_orig_init = nullptr;
constexpr uint32_t ALLOC_CORE = 0x0885C6CCu;       // EBOOT: the heap allocator core (a0 = heap, a1 = size), v0 = block
constexpr uint32_t FIGHTER_VT = 0x08B3A9A8u;       // the fighter class's vtable (the constructor stores it in actor+1348)
void alloc_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t size = static_cast<uint32_t>(ctx->r[5]);
    g_orig_alloc(rdram, ctx);
    if (g_own_ctx) { const uint32_t p = static_cast<uint32_t>(ctx->r[2]); if (p) g_owned[g_own_ctx].push_back({p, size}); }
}
void ctor_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mem = static_cast<uint32_t>(ctx->r[4]), prev = g_own_ctx;
    g_ram = rdram;
    if (!g_owned.count(mem)) g_owned[mem].push_back({mem, 0x12D0u});   // the actor block (allocated by the caller)
    g_own_ctx = mem;
    g_orig_ctor(rdram, ctx);
    g_own_ctx = prev;
}
void init_hook(uint8_t* rdram, recomp_context* ctx) {   // the per-fighter init: a0 = actor + a small offset
    const uint32_t a0 = static_cast<uint32_t>(ctx->r[4]), prev = g_own_ctx;
    for (auto& kv : g_owned) if (a0 >= kv.first && a0 < kv.first + 0x12D0u) { g_own_ctx = kv.first; break; }
    g_orig_init(rdram, ctx);
    g_own_ctx = prev;
}

// The character info record (0x44 bytes, charId at +6) of a charId: the table FUN_08846C98 searches (*(0x089D4968) -> header: +0 array, +4 count... see the
// function: base = *(*table) + 8, count = *(*table + 4)). The first record of that charId (the default costume).
uint32_t info_for_char(int charId) {
    const uint32_t t = rd(0x089D4968u), h = ok(t, 8) ? rd(t) : 0u;
    if (!ok(h, 16)) return 0;
    const uint32_t base = h + 8u, n = rd(h + 4u);
    for (uint32_t i = 0; i < n && i < 4000; i++) { const uint32_t r = base + 0x44u * i; if (!ok(r, 0x44)) break; if (static_cast<int8_t>(g_ram[(r + 6u) & 0x07FFFFFFu]) == charId) return r; }
    return 0;
}
int g_partner_char = 0, g_enemy2_char = 0;   // PSPRECOMP_TAGTEAM_PARTNER_CHAR / _ENEMY2_CHAR: a different character for the extra (0 = a copy of the fighter it stands beside)

// Resources of a character that is not in the battle's loading list. Retail registers the file slots (staging heap, model / motion
// slots) of every character of the battle record at the loading screen (FUN_08B45310 -> FUN_089121F4(charId)); calling it again for the
// extra's character does the same late.
constexpr uint32_t LOAD_FILE_GROUP = 0x08844C14u;   // EBOOT: load file id + its companions (a0 = file id, a1 = model heap, a2 = flag)
constexpr uint32_t FILE_FIND = 0x088444D4u;         // EBOOT: registered file object of an id (a0 = file manager), 0 = not loaded
constexpr uint32_t MODEL_HEAP = 0x088908E0u;        // EBOOT: the heap the battle's character files are loaded into
constexpr uint32_t FILE_MGR_PTR = 0x089D0000u + 18784u;
// The retail staging heap (0x095CD080, FUN_0888204C(0x270800)) is given back to the main heap after the loading screen and reused, so a
// load during the battle needs a heap of its own: a block of the host heap, initialised like retail (FUN_0885C40C(block, size, name, 0x40)).
uint32_t g_load_heap = 0;
uint32_t load_heap() {
    if (g_load_heap) return g_load_heap;
    constexpr uint32_t kSize = 0x290000u;
    const uint32_t block = psp_kmem_bump_alloc_named(kSize, "TAGTEAM_LOAD_HEAP");
    if (!block) return 0;
    call(0x0885C40Cu, block, kSize, 0x089E076Cu, 0x40);
    g_load_heap = block;
    if (g_log) std::fprintf(stderr, "[TAGTEAM] staging heap for character loads: %08X (%X bytes)\n", block, kSize);
    return block;
}
void dump_file_obj(const char* tag, uint32_t mgr, uint32_t id) {
    const uint32_t o = call(FILE_FIND, mgr, id);
    std::fprintf(stderr, "[FILEOBJ] %s id %u obj %08X:", tag, id, o);
    if (ok(o, 0xF0)) for (uint32_t k = 0; k < 0xEC; k += 4) std::fprintf(stderr, " %08X", rd(o + k));
    std::fprintf(stderr, "\n");
}
void load_character(int charId) {
    if (g_log) for (uint32_t a : {0x089C88B8u, 0x089E076Cu, 0x089C93E0u, 0x089CAC10u, 0x089CAF0Cu, 0x089C9ED0u, 0x089CBA28u, 0x089CBDD4u, 0x089C9564u, 0x089C9574u}) { char b[24] = {}; for (int i = 0; i < 23; i++) b[i] = static_cast<char>(g_ram[(a + i) & 0x07FFFFFFu]); std::fprintf(stderr, "[TAGTEAM] heap name %08X: %s\n", a, b); }
    if (charId <= 0 || std::getenv("PSPRECOMP_TAGTEAM_NOLOAD")) return;
    const uint32_t rec = info_for_char(charId);
    if (!rec) { std::fprintf(stderr, "[TAGTEAM] load_character(%d): no file record\n", charId); return; }
    const uint32_t mgr = rd(FILE_MGR_PTR), heap = load_heap();
    if (!heap) { std::fprintf(stderr, "[TAGTEAM] load_character(%d): no staging heap\n", charId); return; }
    const uint32_t ids[2] = {rdh(rec + 0u), rdh(rec + 4u)};   // the model file and its D_ data file (retail loads both at the loading screen)
    for (uint32_t id : ids) {
        if (id == 0xFFFFu) continue;
        const uint32_t have = call(FILE_FIND, mgr, id);
        if (!have) call(LOAD_FILE_GROUP, id, heap, 0);
        if (g_log && std::getenv("PSPRECOMP_TAGTEAM_FILEOBJ")) { dump_file_obj("cloud-after", mgr, id); dump_file_obj("squall", mgr, 2); dump_file_obj("squallD", mgr, 189); }
        if (g_log) std::fprintf(stderr, "[TAGTEAM] load_character(%d): file id %u %s -> object %08X\n", charId, id, have ? "already loaded" : "loaded", call(FILE_FIND, mgr, id));
    }
}

uint32_t spawn_fighter(uint32_t bs, uint32_t side, uint32_t foe, uint32_t cpu_side, uint32_t info_slot, uint32_t slot_actor) {
    const uint32_t sp0 = static_cast<uint32_t>(g_ctx->r[29]);
    g_ctx->r[29] = static_cast<int32_t>(sp0 - 0x200u);   // scratch below the stack pointer, clear of the callees' frames
    const uint32_t buf = sp0 - 0x1C0u, lockargs = sp0 - 0x180u;
    { const int w0 = static_cast<int>(info_slot) >= 100 ? static_cast<int>(info_slot) - 100 : 0; if (w0) load_character(w0); }
    uint32_t mem = call(ALLOC, 0x12D0);
    if (!ok(mem, 0x12D0)) { std::fprintf(stderr, "[TAGTEAM] alloc failed\n"); g_ctx->r[29] = static_cast<int32_t>(sp0); return 0; }
    g_owned[mem].clear(); g_owned[mem].push_back({mem, 0x12D0u}); g_own_ctx = mem;   // everything allocated from here on belongs to this fighter
    if (g_stop < 2) { g_ctx->r[29] = static_cast<int32_t>(sp0); return mem; }
    const uint32_t rec = call(SIDE_REC, side);
    uint32_t before[24]; if (g_log) for (int k = 0; k < 24; k++) before[k] = rd(rec + 4u * k);
    const int want = static_cast<int>(info_slot) >= 100 ? static_cast<int>(info_slot) - 100 : 0;   // info_slot >= 100: a charId + 100 (an extra of its own character)
    uint32_t info = want ? info_for_char(want) : 0u;
    if (want && !info) std::fprintf(stderr, "[TAGTEAM] no info record for charId %d, using the copy\n", want);
    if (!info) info = call(CHAR_INFO, rd(bs + 2008u + 4u * (want ? 0u : info_slot)));
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_INFODUMP")) {
        for (int id : {15, 31, 13}) { const uint32_t r = info_for_char(id); std::fprintf(stderr, "[TAGTEAM] info record charId %d at %08X:", id, r); if (r) for (uint32_t o = 0; o < 0x44; o += 4) std::fprintf(stderr, " %08X", rd(r + o)); std::fprintf(stderr, "\n"); }
        std::fprintf(stderr, "[TAGTEAM] info used %08X:", info); if (ok(info, 0x44)) for (uint32_t o = 0; o < 0x44; o += 4) std::fprintf(stderr, " %08X", rd(info + o)); std::fprintf(stderr, "\n");
        std::fprintf(stderr, "[TAGTEAM] setup records: slot0 %08X slot1 %08X:", rd(bs + 2008u), rd(bs + 2012u)); for (int k = 0; k < 2; k++) { const uint32_t r = rd(bs + 2008u + 4u * k); if (ok(r, 0x40)) { std::fprintf(stderr, " ["); for (uint32_t o = 0; o < 0x40; o += 4) std::fprintf(stderr, " %08X", rd(r + o)); std::fprintf(stderr, "]"); } } std::fprintf(stderr, "\n");
    }
    std::vector<uint32_t> reg;
    const uint32_t RB = 0x08B64000u, RN = (0x09000000u - RB) / 4;
    const bool regdump = g_log && std::getenv("PSPRECOMP_TAGTEAM_REGDUMP");
    if (regdump) { reg.resize(RN); std::memcpy(reg.data(), g_ram + (RB & 0x07FFFFFFu), RN * 4); }
    // The constructor stores itself in two words of a table of the retail pair (0x08BAE9FC / 0x08BAEA08: the CPU fighter,
    // twice, with the human between), overwriting the CPU's entries. The game camera reads them, so they are put back.
    const uint32_t t0 = rd(REG_PAIR0), t1 = rd(REG_PAIR1);
    if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCHRES")) { const uint32_t a = static_cast<uint32_t>(std::strtoul(w, nullptr, 16)); psp_mem_watch_set(a); std::fprintf(stderr, "[TAGTEAM] watching %08X (before the constructor)\n", a); }
    // The constructor also registers the new fighter's lock block in the camera manager's slot for its side (mgr + 0x6D0 +
    // 4 * side: the camera follows the lock blocks of the retail pair) and resets a few words of the old one. The camera
    // then loses the human (eye and target 0). Snapshot the guest RAM so exactly those writes can be taken back.
    const uint32_t old_lb = rd(slot_actor + ACT_LOCKBLK);
    constexpr uint32_t SB = 0x08800000u, SN = (0x0A000000u - SB) / 4;
    std::vector<uint32_t> snap;
    std::vector<std::pair<uint32_t, uint32_t>> undo;   // handed to the caller (g_last_undo)
    const bool guard = !std::getenv("PSPRECOMP_TAGTEAM_NOFIX");
    if (guard) { snap.resize(SN); std::memcpy(snap.data(), g_ram + (SB & 0x07FFFFFFu), SN * 4); }
    std::vector<uint32_t> cam;   // debug: the camera manager and its mode objects around the constructor
    const uint32_t mgr = dissidia_camera_manager();
    std::vector<uint32_t> modes;
    if (regdump && ok(mgr, 0x2400)) { cam.resize(0x2400 / 4); std::memcpy(cam.data(), g_ram + (mgr & 0x07FFFFFFu), 0x2400); for (int k = 0; k < 8; k++) modes.push_back(rd(mgr + 1744u + 4u * k)); }
    std::vector<std::vector<uint32_t>> msnap;
    for (uint32_t m : modes) { std::vector<uint32_t> v(0x400 / 4, 0); if (ok(m, 0x400)) std::memcpy(v.data(), g_ram + (m & 0x07FFFFFFu), 0x400); msnap.push_back(v); }
    g_in_ctor = !std::getenv("PSPRECOMP_TAGTEAM_FREE");
    call(FIGHTER_CTOR, mem, rec, info, side, 0);
    g_in_ctor = false;
    if (!cam.empty()) {
        std::fprintf(stderr, "[TAGTEAM] camera manager %08X, modes:", mgr); for (uint32_t m : modes) std::fprintf(stderr, " %08X", m); std::fprintf(stderr, "\n");
        int n = 0;
        for (uint32_t i = 0; i < cam.size() && n < 120; i++) if (rd(mgr + 4 * i) != cam[i]) { std::fprintf(stderr, "[TAGTEAM] mgr +%04X: %08X -> %08X\n", 4 * i, cam[i], rd(mgr + 4 * i)); n++; }
        for (size_t k = 0; k < modes.size(); k++) for (uint32_t i = 0; i < 0x100 && ok(modes[k], 0x400); i++) if (rd(modes[k] + 4 * i) != msnap[k][i]) std::fprintf(stderr, "[TAGTEAM] mode%zu %08X +%03X: %08X -> %08X\n", k, modes[k], 4 * i, msnap[k][i], rd(modes[k] + 4 * i));
    }
    if (guard && g_log && std::getenv("PSPRECOMP_TAGTEAM_PTRDIFF")) {   // debug: every overwritten pointer (old value a live-looking pointer, not zero)
        int n = 0;
        for (uint32_t i = 0; i < SN && n < 400; i++) {
            const uint32_t a = SB + 4 * i, ov = snap[i], nv = rd(a);
            if (ov != nv && ov >= 0x08800000u && ov < 0x0A000000u && !(a >= mem && a < mem + 0x12D0u)) { std::fprintf(stderr, "[TAGTEAM] ptr %08X: %08X -> %08X\n", a, ov, nv); n++; }
        }
    }
    if (guard) {
        if (std::getenv("PSPRECOMP_TAGTEAM_UNDO_PAIR")) { undo.push_back({REG_PAIR0, t0}); undo.push_back({REG_PAIR1, t1}); }   // (kept by default: the CPU brain tick of the extra needs its place in this table)
        if (!std::getenv("PSPRECOMP_TAGTEAM_KEEP_LAST")) undo.push_back({LAST_FIGHTER_OBJ, snap[(LAST_FIGHTER_OBJ - SB) / 4]});   // global: the last created fighter's object
        const uint32_t new_lb = rd(mem + ACT_LOCKBLK);
        uint32_t slot_at = 0;
        for (uint32_t i = 0; i < SN; i++) {
            const uint32_t a = SB + 4 * i;
            if (snap[i] == old_lb && rd(a) == new_lb && !(a >= mem && a < mem + 0x12D0u)) { undo.push_back({a, old_lb}); slot_at = a; if (g_log) std::fprintf(stderr, "[TAGTEAM] camera slot %08X: %08X to go back to the retail lock block %08X when the extra starts\n", a, new_lb, old_lb); }
        }
        if (slot_at) {
            const uint32_t m = slot_at - 0x6D0u - 4u * side;   // the camera manager
            int n = 0;
            for (uint32_t o = 0; o < 0xD00u; o += 4) { const uint32_t a = m + o, ov = snap[(a - SB) / 4]; if (rd(a) != ov) { undo.push_back({a, ov}); n++; } }
            int k = 0;
            for (uint32_t o = 0; o < 0x440u; o += 4) { const uint32_t a = old_lb - 0x40u + o, ov = snap[(a - SB) / 4]; if (rd(a) != ov) { undo.push_back({a, ov}); k++; if (g_log) std::fprintf(stderr, "[TAGTEAM]   lock block %+d: now %08X, was %08X\n", int(o) - 0x40, rd(a), ov); } }
            if (g_log) std::fprintf(stderr, "[TAGTEAM] camera manager %08X: %d words and %d words of the old lock block to put back when the extra starts\n", m, n, k);
        } else if (g_log) std::fprintf(stderr, "[TAGTEAM] no camera slot found for lock block %08X\n", new_lb);
    }
    if (regdump) {
        int n = 0;
        for (uint32_t i = 0; i < RN && n < 300; i++) {
            const uint32_t a = RB + 4 * i, v = rd(a);
            if (v != reg[i] && !(a >= mem && a < mem + 0x12D0u)) { std::fprintf(stderr, "[TAGTEAM] reg side %u %08X: %08X -> %08X\n", side, a, reg[i], v); n++; }
        }
    }
    if (g_stop < 3) { g_ctx->r[29] = static_cast<int32_t>(sp0); return mem; }
    call(SET_FOE, mem, foe);
    if (g_stop < 4) { g_ctx->r[29] = static_cast<int32_t>(sp0); return mem; }
    if (g_log) for (int k = 0; k < 24; k++) if (rd(rec + 4u * k) != before[k]) std::fprintf(stderr, "[TAGTEAM] side %u record %08X +%02X: %08X -> %08X (after ctor)\n", side, rec, 4 * k, before[k], rd(rec + 4u * k));

    // The setup's per-fighter init: the actor's own init through its vtable, two flag bits, the lock block.
    const uint32_t G = REC_BASE + 4192u * side;
    call(REC_INIT, buf, side);
    const uint32_t vt = rd(mem + 1348) + 256u;
    const uint32_t voff = static_cast<uint32_t>(static_cast<int16_t>(rd(vt) & 0xFFFFu)), vfn = rd(vt + 4);
    if (FuncPtr f = fn(vfn)) { const recomp_context sv = *g_ctx; g_ctx->r[4] = static_cast<int32_t>(mem + voff); g_ctx->r[5] = static_cast<int32_t>(G); g_ctx->r[6] = static_cast<int32_t>(buf); f(g_ram, g_ctx); *g_ctx = sv; }
    if (rd(REC_BASE + 10208u + 4u * side) & 1u) g_ram[(mem + 1456u + 7u) & 0x07FFFFFFu] |= 0x10;
    if (rdh(REC_BASE + 10216u + 2u * side) & 1u) g_ram[(mem + 1456u + 8u) & 0x07FFFFFFu] |= 0x20;
    {
        const uint32_t lr = REC_BASE + 8384u + 4u * side, w = rdh(lr);
        wr(lockargs, static_cast<uint32_t>(static_cast<int8_t>(g_ram[(lr + 2u) & 0x07FFFFFFu])));
        g_ram[(lockargs + 4u) & 0x07FFFFFFu] = (w & 4u) ? 1 : 0;
        g_ram[(lockargs + 5u) & 0x07FFFFFFu] = (w & 0x10u) ? 1 : 0;
        call(LOCK_INIT, rd(mem + ACT_LOCKBLK), lockargs);
    }
    if (g_log) for (int k = 0; k < 24; k++) if (rd(rec + 4u * k) != before[k]) std::fprintf(stderr, "[TAGTEAM] side %u record +%02X now %08X (after init)\n", side, 4 * k, rd(rec + 4u * k));
    if (g_stop < 5) { g_ctx->r[29] = static_cast<int32_t>(sp0); return mem; }
    // The CPU brain, as the setup gives it to a fighter whose control type is not 0: FUN_08B2F2E4(actor, 0), then, for
    // types 1 and 2, its two parameters.
    const uint32_t brain = rd(mem + ACT_BRAIN), cr = REC_BASE + 4192u * cpu_side, type = rdh(cr + 3892u);
    if (g_log) std::fprintf(stderr, "[TAGTEAM] spawn side %u: actor %08X (info %08X brain %08X) foe %08X, cpu record side %u type %u params %u %u\n", side, mem, info, brain, foe, cpu_side, type, rdh(cr + 3894u), rdh(cr + 3896u));
    if (type != 0) {
        call(CPU_MODE, mem, 0);
        if (type < 3 && ok(brain, 0x40)) { call(BRAIN_MODE, brain, rdh(cr + 3894u)); call(BRAIN_LEVEL, brain, rdh(cr + 3896u)); }
    }
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_SCAN")) { std::fprintf(stderr, "[TAGTEAM] slots: %08X %08X human slot %u\n", rd(bs + 56), rd(bs + 60), rd(bs + 84)); scan_refs(mem, side ? "extra side 1" : "extra side 0"); }
    g_last_undo = undo;
    g_own_ctx = 0;
    g_ctx->r[29] = static_cast<int32_t>(sp0);
    return mem;
}

struct Snap { uint32_t base = 0, n = 0; std::vector<uint32_t> w; };
Snap take(uint32_t base, uint32_t bytes) { Snap s; s.base = base; s.n = bytes / 4; for (uint32_t i = 0; i < s.n; i++) s.w.push_back(rd(base + 4 * i)); return s; }
void diff(const char* tag, const Snap& s) {
    for (uint32_t i = 0; i < s.n; i++) { const uint32_t v = rd(s.base + 4 * i); if (v != s.w[i]) std::fprintf(stderr, "[TAGTEAM] %s %08X+%04X: %08X -> %08X\n", tag, s.base, 4 * i, s.w[i], v); }
}

void setup_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t bs = static_cast<uint32_t>(ctx->r[4]);
    g_ram = rdram; g_ctx = ctx;
    if (!g_orig_ctor) {   // ownership recording for the retail pair (the constructor and the init run inside the original setup)
        g_orig_ctor = fn(FIGHTER_CTOR);
        const uint32_t vfn0 = rd(FIGHTER_VT + 256u + 4u);
        g_orig_init = fn(vfn0);
        if (g_orig_ctor && g_orig_init) { psp_dispatch_register(FIGHTER_CTOR, ctor_hook); psp_dispatch_register(vfn0, init_hook); if (g_log) std::fprintf(stderr, "[TAGTEAM] ownership hooks: ctor %08X init %08X\n", FIGHTER_CTOR, vfn0); }
        else std::fprintf(stderr, "[TAGTEAM] ownership hooks NOT installed\n");
    }
    g_owned.clear(); g_own_ctx = 0;
    g_orig_setup(rdram, ctx);
    g_ram = rdram; g_ctx = ctx;
    g_nextra = 0; g_bs = 0; g_late_q.clear(); g_late_ready = false;
    if (!g_enabled) return;
    if (!ok(bs, 0x2800)) return;
    const uint32_t a0 = rd(bs + 56), a1 = rd(bs + 60), mode = rd(bs + 76), human = rd(bs + 84);
    if (g_log) std::fprintf(stderr, "[TAGTEAM] setup bs=%08X mode=%u slots %08X %08X human slot %u\n", bs, mode, a0, a1, human);
    if (!ok(a0, 0x12D0) || !ok(a1, 0x12D0)) return;
    if (!g_orig_lock_other) {
        g_orig_lock_other = fn(LOCK_OTHER); g_orig_refresh = fn(TARGET_REFRESH); g_orig_lock_mode = fn(LOCK_MODE);
        if (g_orig_lock_other && g_orig_refresh && g_orig_lock_mode) { psp_dispatch_register(LOCK_OTHER, lock_other_hook); psp_dispatch_register(TARGET_REFRESH, refresh_hook); psp_dispatch_register(LOCK_MODE, lock_mode_hook); }
        else std::fprintf(stderr, "[TAGTEAM] lock cycle hooks not installed\n");
    }
    if (!g_orig_stock) { g_orig_stock = fn(0x08B33914u); if (g_orig_stock) psp_dispatch_register(0x08B33914u, stock_hook); }
    if (!g_orig_tick) { g_orig_tick = fn(BRAIN_TICK); if (g_orig_tick) psp_dispatch_register(BRAIN_TICK, tick_hook); else std::fprintf(stderr, "[TAGTEAM] brain tick not found\n"); }
    const uint32_t hum = human == 0 ? a0 : a1, cpu = human == 0 ? a1 : a0;
    if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCH_BS")) {   // arm the write watch on bs + hex offset
        const uint32_t off = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        psp_mem_watch_set(bs + off);
        std::fprintf(stderr, "[TAGTEAM] watching bs+%X = %08X\n", off, bs + off);
    }
    if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCH")) {   // arm the write watch on <human actor> + hex offset
        const uint32_t off = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        psp_mem_watch_set(hum + off);
        std::fprintf(stderr, "[TAGTEAM] watching %08X\n", hum + off);
    }
    if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCH_STATUS")) {   // watch the human's status object (actor+0x52C) + hex offset
        const uint32_t st = rd(hum + 0x52Cu), off = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        psp_mem_watch_set(st + off); std::fprintf(stderr, "[TAGTEAM] watching status %08X + %X\n", st, off);
    }
    if (std::getenv("PSPRECOMP_TAGTEAM_WATCH_SEL")) {   // watch the human's lock cycle selector (retail mode handler [1] + 28)
        const uint32_t L = rd(hum + 0x5ACu), h1 = ok(L, 0x200) ? rd(L + 304u) : 0u;
        if (ok(h1, 0x40)) { psp_mem_watch_set(h1 + 28u); std::fprintf(stderr, "[TAGTEAM] watching the lock selector at %08X\n", h1 + 28u); }
    }
    // Slot 0 is side 0 and slot 1 side 1 in the setup, whoever the human is; the CPU brain settings are the CPU slot's.
    const uint32_t cpu_side = human == 0 ? 1 : 0, hum_side = human;
    if ((eff_mask() & 1) && g_late) { g_late_q.push_back({cpu_side, hum, cpu_side, g_enemy2_char ? static_cast<uint32_t>(100 + g_enemy2_char) : cpu_side, cpu}); g_bs = bs; }
    else if (eff_mask() & 1) {   // the enemy team's second fighter: a copy of the CPU fighter, foe = the human
        Extra& e = g_extra[g_nextra];
        e.actor = spawn_fighter(bs, cpu_side, hum, cpu_side, cpu_side, cpu); e.side = cpu_side; e.foe = hum; e.slot_actor = cpu; e.started = false;
        if (e.actor) { e.undo = g_last_undo; g_nextra++; }
    }
    if (eff_mask() & 2) {   // the human's partner: a copy of the human fighter on the CPU brain, foe = the CPU fighter
        const uint32_t P = rd(0x089D6A10u);
        Snap sb, sp, sg;
        if (g_log) { sb = take(bs, 0x2800); if (ok(P, 0x400)) sp = take(P, 0x400); sg = take(0x089D6000u, 0x1000); }
        if (g_late) { g_late_q.push_back({hum_side, cpu, cpu_side, std::getenv("PSPRECOMP_TAGTEAM_PARTNER_CPU_CHAR") ? cpu_side : (g_partner_char ? static_cast<uint32_t>(100 + g_partner_char) : hum_side), hum}); g_bs = bs; goto after_partner; }
        Extra& e = g_extra[g_nextra];
        e.actor = spawn_fighter(bs, hum_side, cpu, cpu_side, std::getenv("PSPRECOMP_TAGTEAM_PARTNER_CPU_CHAR") ? cpu_side : hum_side, hum);
        if (g_log) { diff("bs", sb); if (!sp.w.empty()) diff("P", sp); diff("glob", sg); } e.side = hum_side; e.foe = cpu; e.slot_actor = hum; e.started = false;
        if (e.actor) { e.undo = g_last_undo; g_nextra++; }
    }
    after_partner:
    if (g_nextra) g_bs = bs;
}

// Take back the constructor's registrations (camera slot, lock block list, pair table). They are needed while the fighter
// loads (it never becomes ready without them), but with them in place the game camera follows the new fighter, not the human.
void apply_undo(Extra& e) {
    for (auto& u : e.undo) wr(u.first, u.second);
    if (g_log && !e.undo.empty()) std::fprintf(stderr, "[TAGTEAM] extra %08X: %zu registration words taken back\n", e.actor, e.undo.size());
    e.undo.clear();
}

void start_extra(Extra& e) {
    if (g_stop < 6) { e.started = true; return; }
    apply_undo(e);
    if (g_tagin && !g_oneround) {   // one more stock for the slot fighter of this extra's team: the retail KO then restarts the round instead of ending the battle (the KO watcher swaps the partner in)
        for (int k = 0; k < 2; k++) { const uint32_t a = rd(g_bs + 56u + 4u * k); if (ok(a, 0x12D0) && side_of(a) == e.side) g_ram[(a + 0x10CEu) & 0x07FFFFFFu] = static_cast<uint8_t>(g_ram[(a + 0x10CEu) & 0x07FFFFFFu] + 1); }
    }
    const uint32_t sp0 = static_cast<uint32_t>(g_ctx->r[29]);
    g_ctx->r[29] = static_cast<int32_t>(sp0 - 0x100u);
    const uint32_t vec = sp0 - 0xC0u;
    // Stand 3 m beside the fighter it copies (the retail pair is placed by the stage at this point).
    float pdx = 3.0f, pdz = 0.0f; if (const char* pe = std::getenv("PSPRECOMP_TAGTEAM_PLACE")) std::sscanf(pe, "%f,%f", &pdx, &pdz);   // debug: offset from the fighter it copies (x, z)
    wrf(vec, rdf(e.slot_actor + 0x80u) + pdx); wrf(vec + 4, rdf(e.slot_actor + 0x84u)); wrf(vec + 8, rdf(e.slot_actor + 0x88u) + pdz); wrf(vec + 12, 1.0f);
    call(PLACE, e.actor, vec, 1);
    g_ctx->f[12] = 0.0f; g_ctx->f[13] = 0.0f;
    { const uint32_t rv = call(START_ACTION, e.actor + 304u, 1, 0); if (g_log) std::fprintf(stderr, "[TAGTEAM] START_ACTION(%08X, 1) -> %u, script now %d\n", e.actor + 304u, rv, static_cast<int16_t>(rd(e.actor + 0x180u) & 0xFFFFu)); }
    call(LOCK_SET, rd(e.actor + 0x5ACu), e.foe);          // lock on to the foe: the AI and the turn-to-target ops read it
    {   // Bravery: the retail pair gets its base bravery (actor +0x10AC) in both halves of status +0x14 at battle start; the extras start at 0 (broken).
        const uint32_t st = rd(e.actor + 0x52Cu), b = rd(e.actor + 0x10ACu) & 0xFFFFu;
        if (ok(st, 0x40) && rd(st + 0x14u) == 0) wr(st + 0x14u, (b << 16) | b);
    }
    e.started = true;
    if (g_log) for (uint32_t a : {e.slot_actor, e.actor}) { const uint32_t st = rd(a + 0x52Cu); std::fprintf(stderr, "[TAGTEAM] stats %08X: +10A8 %u %u %u %u %u status %08X +0x14 %08X +0x18 %08X +0x1C %08X\n", a, rd(a + 0x10A8u), rd(a + 0x10ACu), rd(a + 0x10B0u), rd(a + 0x10B4u), rd(a + 0x10B8u), st, rd(st + 0x14u), rd(st + 0x18u), rd(st + 0x1Cu)); }
    if (g_log) std::fprintf(stderr, "[TAGTEAM] extra %08X (side %u) started, target %08X foe %08X\n", e.actor, e.side, rd(e.actor + 0x2FCu), e.foe);
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_ACTORDUMP")) {   // debug: every word of the extra's actor (diff a clone against another character)
        for (uint32_t o = 0; o < 0x12D0; o += 4) std::fprintf(stderr, "[ACTORDUMP] %04X %08X\n", o, rd(e.actor + o));
    }
    g_ctx->r[29] = static_cast<int32_t>(sp0);
}

// The flow step runs every frame until both fighters are ready, then starts them (action 1) and sets bs+72. The extras
// are held to the same rule and started right after the original has started the others.
void flow_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t bs = static_cast<uint32_t>(ctx->r[4]);
    g_ram = rdram; g_ctx = ctx;
    bool pending = false;
    for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started) pending = true;
    if (!pending || bs != g_bs || rd(bs + 72) != 0) { g_orig_flow(rdram, ctx); return; }
    static int n = 0;
    bool ready = true;
    for (int i = 0; i < g_nextra; i++) if (!call(FIGHTER_READY, g_extra[i].actor)) ready = false;
    if (g_log && (n++ % 60) == 0) std::fprintf(stderr, "[TAGTEAM] flow: extras ready=%d\n", ready);
    if (!ready) return;                                      // wait: the original would start the others without them
    for (int i = g_nextra - 1; i >= 0; i--) apply_undo(g_extra[i]);   // (last spawned first) before the original starts the retail pair and the camera
    g_orig_flow(rdram, ctx);
    if (rd(bs + 72) == 0) return;                            // the others were not ready yet
    for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started) start_extra(g_extra[i]);
}

// ---- HUD (tagteam_hud.cpp) ----
std::vector<uint32_t> g_down;   // KO'd characters that stay down (tag-in): their live HP is left alone, writing 0 to it makes the retail rules treat the team as lost
bool is_down(uint32_t a) { for (uint32_t d : g_down) if (d == a) return true; return false; }
int g_called[2] = {0, 0};   // call-ins made per team (0 = the human's), the reserve strip hides that many names
std::vector<TagHudReserve> g_reserve;   // waiting fighters per side (placeholder list until the roster call-ins exist)
bool g_hud_on = true;

// A character's label. Retail charIds 1..33 have a name table (the order of the roster; 15 = Squall and 31 = Lightning are confirmed
// in runs, the rest follows the roster order and the series codes of the table at 0x089D6CA8); other ids (mods) show the first
// letters of their code string ("fiv100" = Bartz -> "FIV1"), else "#id".
struct CharName { const char* full; const char* brief; };
const CharName kNames[34] = {
    {"", ""}, {"WARRIOR OF LIGHT", "WOL"}, {"GARLAND", "GARL"}, {"FIRION", "FIRI"}, {"EMPEROR", "EMPE"}, {"ONION KNIGHT", "ONIO"},
    {"CLOUD OF DARKNESS", "COD"}, {"CECIL", "CECI"}, {"GOLBEZ", "GOLB"}, {"BARTZ", "BART"}, {"EXDEATH", "EXDE"}, {"TERRA", "TERR"},
    {"KEFKA", "KEFK"}, {"CLOUD", "CLOU"}, {"SEPHIROTH", "SEPH"}, {"SQUALL", "SQUA"}, {"ULTIMECIA", "ULTI"}, {"ZIDANE", "ZIDA"},
    {"KUJA", "KUJA"}, {"TIDUS", "TIDU"}, {"JECHT", "JECH"}, {"SHANTOTTO", "SHAN"}, {"GABRANTH", "GABR"}, {"CHAOS", "CHAO"},
    {"KAIN", "KAIN"}, {"GILGAMESH", "GILG"}, {"TIFA", "TIFA"}, {"LAGUNA", "LAGU"}, {"YUNA", "YUNA"}, {"PRISHE", "PRIS"},
    {"VAAN", "VAAN"}, {"LIGHTNING", "LIGH"}, {"FERAL CHAOS", "FCHA"}, {"YUFFIE", "YUFF"},
};
std::string char_label(int id, bool full) {
    if (id >= 1 && id <= 33) return full ? kNames[id].full : kNames[id].brief;
    if (id <= 0 || id > 60) return "#" + std::to_string(id);
    const uint32_t p = rd(0x089D6CA8u + 4u * static_cast<uint32_t>(id));
    std::string out;
    if (ok(p, 8)) for (int i = 0; i < 4; i++) { const char c = static_cast<char>(g_ram[(p + i) & 0x07FFFFFFu]); if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) break; out += c; }
    return out.size() >= 2 ? out : "#" + std::to_string(id);
}
int actor_char(uint32_t actor) {
    const uint32_t info = rd(actor + 0x2F8u);
    return ok(info, 8) ? static_cast<int8_t>(g_ram[(info + 6u) & 0x07FFFFFFu]) : -1;
}

// ---- the party lists of a group (Quick Battle Party) battle ----
// The battle overlay keeps each side's party in a team record (stride 0x38) at bs+0x13C21 - 6: a count byte, then the charIds in order
// (1st, 2nd, ...), 0xFF filled. A 1 v 1 battle rewrites it with a count of 1, so a reserve only shows in a real party battle. Retail plays
// the party one member after the other, so the members behind the one on the stage are the reserve; the ones before him are out.
constexpr uint32_t PARTY_IDS_FROM_BS = 0x13C21u, PARTY_STRIDE = 0x38u;
struct PartyList { int n = 0; uint8_t ids[8] = {}; };
PartyList read_party(int side) {
    PartyList p;
    if (!g_bs) return p;
    const uint32_t ids = g_bs + PARTY_IDS_FROM_BS + PARTY_STRIDE * static_cast<uint32_t>(side);
    if (!ok(ids - 6u, 0x10)) return p;
    const int n = g_ram[(ids - 6u) & 0x07FFFFFFu];
    if (n < 1 || n > 8) return p;
    for (int i = 0; i < n; i++) { const uint8_t c = g_ram[(ids + static_cast<uint32_t>(i)) & 0x07FFFFFFu]; if (c == 0 || c > 34) return PartyList{}; p.ids[i] = c; }
    p.n = n;
    return p;
}
int g_party_last[2] = {0, 0};   // index of the member on the stage per team (only moves forward while the party list stays the same)
void update_party_reserve() {
    if (std::getenv("PSPRECOMP_TAGTEAM_RESERVE") || !g_bs) return;   // an env list overrides
    const uint32_t hs = rd(g_bs + 84u);
    const uint32_t slot[2] = {rd(g_bs + 56u + 4u * (hs & 1u)), rd(g_bs + 56u + 4u * (1u - (hs & 1u)))};   // [0] = the human's fighter, [1] = the other team's
    const PartyList lists[2] = {read_party(0), read_party(1)};
    g_reserve.resize(2); g_reserve[0].side = 0; g_reserve[1].side = 1;
    g_reserve[0].names.clear(); g_reserve[1].names.clear();
    auto has = [](const PartyList& l, int c) { for (int i = 0; i < l.n; i++) if (l.ids[i] == c) return true; return false; };
    const int hc = ok(slot[0], 0x12D0) ? actor_char(slot[0]) : -1;
    const int ho = has(lists[0], hc) || !has(lists[1], hc) ? 0 : 1;   // which list is the human's team (the first one unless only the second holds him)
    const int idx_list[2] = {ho, 1 - ho};
    static PartyList prev[2];
    for (int t = 0; t < 2; t++) {
        const PartyList& l = lists[idx_list[t]];
        if (l.n <= 1) continue;
        if (prev[t].n != l.n || std::memcmp(prev[t].ids, l.ids, 8) != 0) { prev[t] = l; g_party_last[t] = 0; }
        const int cur = ok(slot[t], 0x12D0) ? actor_char(slot[t]) : -1;
        int at = -1;
        for (int i = g_party_last[t]; i < l.n && at < 0; i++) if (l.ids[i] == cur) at = i;
        for (int i = 0; i < g_party_last[t] && at < 0; i++) if (l.ids[i] == cur) at = i;   // a new battle of the same party starts over at the first member
        if (at < 0) continue;
        g_party_last[t] = at;
        for (int i = at + 1; i < l.n; i++) g_reserve[t].names.push_back(char_label(l.ids[i], false));
    }
    if (g_log) {
        static std::string last;
        std::string now;
        for (int t = 0; t < 2; t++) { now += t ? " | " : ""; now += "team" + std::to_string(t) + ":"; for (const std::string& n : g_reserve[t].names) now += " " + n; }
        if (now != last) { last = now; std::fprintf(stderr, "[TAGTEAM] party reserve (human char %d, lists %d/%d members): %s\n", hc, lists[0].n, lists[1].n, now.c_str()); }
    }
}

void init_reserve() {
    g_reserve.clear();
    g_reserve.resize(2);
    g_reserve[0].side = 0; g_reserve[1].side = 1;
    const char* e = std::getenv("PSPRECOMP_TAGTEAM_RESERVE");   // "id,id;id,id": charIds waiting on the human's side ; the enemy's side
    std::string spec = e ? e : ";";                      // placeholder roster for the HUD until the party call-ins exist
    int side = 0; int v = -1;
    auto flush = [&]() { if (v >= 0) g_reserve[side].names.push_back(char_label(v, false)); v = -1; };
    for (char c : spec) { if (c >= '0' && c <= '9') v = (v < 0 ? 0 : v * 10) + (c - '0'); else if (c == ',') flush(); else if (c == ';') { flush(); side = 1; } }
    flush();
}

void hud_update() {
    if (!g_hud_on) return;
    static bool inited = false;
    if (!inited) { inited = true; init_reserve(); if (g_log) { std::fprintf(stderr, "[TAGTEAM] labels:"); for (int i = 0; i <= 36; i++) { const uint32_t p = rd(0x089D6CA8u + 4u * static_cast<uint32_t>(i)); char b[9] = {}; if (ok(p, 8)) for (int k = 0; k < 8; k++) b[k] = static_cast<char>(g_ram[(p + k) & 0x07FFFFFFu]); std::fprintf(stderr, " %d=%s", i, b); } std::fprintf(stderr, "\n"); } }
    update_party_reserve();
    std::vector<TagHudFighter> fs;
    for (int i = 0; i < g_nextra; i++) {
        Extra& e = g_extra[i];
        TagHudFighter f;
        f.name = char_label(actor_char(e.actor), true); f.side = static_cast<int>(e.side == g_extra[0].side ? e.side : e.side);
        { const uint32_t st = rd(e.actor + 0x52Cu); f.hp = (ok(st, 0x40) && !is_down(e.actor)) ? static_cast<int>(static_cast<int32_t>(rd(st + 0xCu))) : 0; }   // live HP (the status object word the retail HUD reads); +0x10B0 is the maximum
        f.hp_max = std::max(static_cast<int>(rd(e.actor + 0x10B0u)), std::max(f.hp, 1));
        fs.push_back(f);
    }
    // sides in the HUD: the human's team on the left (side value of the human's slot), the other on the right
    const uint32_t hum_side = g_bs ? rd(g_bs + 84u) : 0u;
    for (TagHudFighter& f : fs) f.side = (static_cast<uint32_t>(f.side) == hum_side) ? 0 : 1;
    {   // members already called in are not in reserve any more
        std::vector<TagHudReserve> shown = g_reserve;
        for (size_t t = 0; t < shown.size() && t < 2; t++) { const size_t n = std::min(static_cast<size_t>(g_called[t]), shown[t].names.size()); shown[t].names.erase(shown[t].names.begin(), shown[t].names.begin() + static_cast<long>(n)); }
        tagteam_hud_draw(fs, shown);
        return;
    }
    tagteam_hud_draw(fs, g_reserve);
}

// ---- lock-on cycling between the enemy team's fighters ----
// Every fighter of the battle: the two retail slots and the extras. Side = halfword actor+0x10CC (the ctor's side argument).
int all_fighters(uint32_t out[6]) {
    int n = 0;
    if (g_bs) { for (int k = 0; k < 2; k++) { const uint32_t a = rd(g_bs + 56u + 4u * k); if (ok(a, 0x12D0)) out[n++] = a; } }
    for (int i = 0; i < g_nextra && n < 6; i++) if (g_extra[i].started) out[n++] = g_extra[i].actor;
    return n;
}
uint32_t side_of(uint32_t a) { return rdh(a + 0x10CCu); }
// Alive = live HP above 0: the word the retail HUD reads, *(actor+0x52C)+0xC (actor+0x10B0 is the maximum).
bool alive(uint32_t a) { const uint32_t st = rd(a + 0x52Cu); return !is_down(a) && ok(st, 0x40) && static_cast<int32_t>(rd(st + 0xCu)) > 0; }

// The enemy fighter to cycle to from `foe` for `owner`: another living fighter of the other team, 0 if none.
uint32_t other_enemy(uint32_t owner, uint32_t foe) {
    uint32_t f[6];
    const int n = all_fighters(f);
    for (int i = 0; i < n; i++) if (f[i] != owner && f[i] != foe && side_of(f[i]) != side_of(owner) && alive(f[i])) return f[i];
    return 0;
}

uint32_t g_ld_a[2] = {}, g_ld_snap[2][0x12D0 / 4]; int g_ld_n = 0; float g_ld_due = 0;   // LOCKDIFF debug

// FUN_08B40994: the cycle's second candidate (retail: the foe's assist). In a tag battle it is the foe's team mate, so the retail cycle
// (FUN_08B3FE3C advances through the candidates, asking this function whether each exists) walks foe -> other enemy -> ...
void lock_other_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram; g_ctx = ctx;
    const uint32_t L = static_cast<uint32_t>(ctx->r[5]);
    if (g_nextra && ok(L, 0x200)) {
        const uint32_t owner = rd(L + 248u);
        if (ok(owner, 0x12D0) && g_ram[(owner + 0x542u) & 0x07FFFFFFu] == 0) {
            const uint32_t o = other_enemy(owner, rd(owner + 0x300u));
            if (o) { ctx->r[2] = static_cast<int32_t>(o); return; }
        }
    }
    g_orig_lock_other(rdram, ctx);
}

// FUN_08B2F17C copies the lock result into actor+0x2FC but, for a fighter that is not an assist, always writes the foe (+0x300). Afterwards
// a locked enemy fighter other than the foe is written back.
void refresh_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    g_ram = rdram; g_ctx = ctx;
    g_orig_refresh(rdram, ctx);
    if (!g_nextra || !ok(actor, 0x12D0)) return;
    const uint32_t L = rd(actor + 0x5ACu);
    if (!ok(L, 0x200)) return;
    const uint32_t t = call(LOCK_GET, L);
    if (t && t != actor && ok(t, 0x12D0) && g_ram[(t + 0x542u) & 0x07FFFFFFu] == 0 && side_of(t) != side_of(actor) && alive(t)) {
        if (g_log && rd(actor + 0x2FCu) != t) std::fprintf(stderr, "[TAGTEAM] %08X targets %08X (foe %08X)\n", actor, t, rd(actor + 0x300u));
        wr(actor + 0x2FCu, t);
        if (!std::getenv("PSPRECOMP_TAGTEAM_KEEPFOE")) wr(actor + 0x300u, t);   // the foe too: homing follow-ups, damage and the camera read it
    }
}

// FUN_08B3B3EC(block, mode, flag): L toggles the human's lock through it. Tag battle: with a second enemy alive, the press that would
// release the lock first moves the lock to that enemy (the retail cycle selector, handler [1] +28 = 1); the next press releases it.
void lock_mode_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram; g_ctx = ctx;
    const uint32_t L = static_cast<uint32_t>(ctx->r[4]), mode = static_cast<uint32_t>(ctx->r[5]);
    if (g_nextra && ok(L, 0x200)) {
        const uint32_t owner = rd(L + 248u), h1 = rd(L + 304u);
        if (g_log && (owner == rd(g_bs + 56u + 4u * rd(g_bs + 84u)))) { static int n = 0; if (n++ < 40) std::fprintf(stderr, "[TAGTEAM] t=%.1fs lock mode call: block %08X mode %u -> %u flag %u (sel %d)\n", dissidia_battle_age() / 60.0f, L, rd(L + 264u), mode, static_cast<uint32_t>(ctx->r[6]), ok(h1, 0x40) ? static_cast<int>(rd(h1 + 28u)) : -99); }
        if (mode == 0 && rd(L + 264u) == 1 && ok(h1, 0x40) && ok(owner, 0x12D0) && g_ram[(owner + 0x542u) & 0x07FFFFFFu] == 0 && owner == rd(g_bs + 56u + 4u * rd(g_bs + 84u))) {
            int& cyc = g_cyc;   // 0: locked on the foe, 1: on the other enemy, 2: on the EX core (the retail selector is reset by the press itself, so the state is kept here)
            const uint32_t other = other_enemy(owner, rd(owner + 0x300u));
            const bool core = call(0x08B40A50u, h1, L) != 0;   // retail's third candidate: the EX core, when one is out
            if (cyc == 0 && other) {
                cyc = 1; g_cyc_target = other; wr(h1 + 28u, 1);           // cycle to the other enemy instead of releasing the lock
                if (g_log) std::fprintf(stderr, "[TAGTEAM] lock cycle: %08X -> the other enemy %08X\n", rd(owner + 0x300u), other);
                ctx->r[2] = 1; return;
            }
            if (cyc < 2 && core) {
                cyc = 2; wr(h1 + 28u, 2);           // ... then the EX core
                if (g_log) std::fprintf(stderr, "[TAGTEAM] lock cycle: -> the EX core\n");
                ctx->r[2] = 1; return;
            }
            cyc = 0; g_cyc_target = 0; wr(h1 + 28u, 0);               // released: the next lock starts on the foe again
            for (int k = 0; k < 2; k++) { const uint32_t sa = rd(g_bs + 56u + 4u * k); if (sa != owner && ok(sa, 0x12D0)) wr(owner + 0x300u, sa); }   // the foe is the slot opponent again
        }
    }
    g_orig_lock_mode(rdram, ctx);
}

// ---- tag swap: SELECT (PSPRECOMP_TAGTEAM_SWAP_BUTTON=<hex psp mask>) hands the pad to the partner ----
uint32_t g_swap_mask = 0;         // off until the swap keeps the camera and HUD intact; PSPRECOMP_TAGTEAM_SWAP_BUTTON=1 (hex PSP mask, 1 = SELECT) turns it on
bool g_swap_req = false;
Extra* extra_of(uint32_t actor) {
    for (int i = 0; i < g_nextra; i++) if (g_extra[i].actor == actor) return &g_extra[i];
    return nullptr;
}

// Hand the pad to the partner: the two fighters trade brains (the pad is read by the human brain, +0x54 != 0) and trade places in
// the battle's slot table, which the retail HUD, the camera and the lock cycle read. The partner becomes an extra on the CPU brain.
// Debug (PSPRECOMP_TAGTEAM_SWAPSCAN=1): every word in guest RAM equal to `v`, outside the two actors' own blocks.
void scan_val(uint32_t v, const char* tag, uint32_t a0, uint32_t a1) {
    int n = 0;
    for (uint32_t a = 0x08800000u; a < 0x0BF00000u && n < 150; a += 4) {
        if ((a >= a0 && a < a0 + 0x12D0u) || (a >= a1 && a < a1 + 0x12D0u)) continue;
        if (rd(a) == v) { std::fprintf(stderr, "[SWAPSCAN] %s %08X held at %08X\n", tag, v, a); n++; }
    }
}

void dump_win(uint32_t at, const char* tag, uint32_t H, uint32_t P, uint32_t C, uint32_t lbH, uint32_t lbP) {
    std::fprintf(stderr, "[SWAPSCAN] window %s @%08X:", tag, at);
    for (int k = -6; k <= 6; k++) {
        const uint32_t v = rd(at + 4 * k);
        const char* n = v == H ? "H" : v == P ? "P" : v == C ? "C" : v == lbH ? "lbH" : v == lbP ? "lbP" : nullptr;
        if (n) std::fprintf(stderr, " [%s]", n); else std::fprintf(stderr, " %X", v);
    }
    std::fprintf(stderr, "\n");
}

// Debug: the retail HUD's accessor functions (battle overlay 0x089E95E0..0x089E9EF4, a0 = side) before / after the swap.
void hud_probe(const char* tag) {
    static const uint32_t fns[] = {0x089E95B0u, 0x089E95E0u, 0x089E9628u, 0x089E968Cu, 0x089E96DCu, 0x089E973Cu, 0x089E978Cu, 0x089E97FCu, 0x089E9858u, 0x089E98F0u, 0x089E9964u, 0x089E99C0u, 0x089E9A4Cu, 0x089E9AA8u, 0x089E9B18u, 0x089E9C24u, 0x089E9C64u, 0x089E9D84u, 0x089E9E10u, 0x089E9E68u, 0x089E9EF4u};
    for (uint32_t f : fns) {
        std::fprintf(stderr, "[HUDPROBE] %s %08X:", tag, f);
        for (uint32_t side = 0; side < 2; side++) {
            FuncPtr fp = fn(f); if (!fp) continue;
            const recomp_context sv = *g_ctx; g_ctx->r[4] = static_cast<int32_t>(side); g_ctx->r[5] = 0;
            fp(g_ram, g_ctx);
            std::fprintf(stderr, "  s%u v0=%08X f0=%g", side, static_cast<uint32_t>(g_ctx->r[2]), static_cast<double>(g_ctx->f[0]));
            *g_ctx = sv;
        }
        std::fprintf(stderr, "\n");
    }
}

// ---- tag swap by content exchange ----
// The retail HUD, camera, pad, lock block, pair table and the enemies' targets all refer to the two slot fighters by ADDRESS. So the swap
// leaves every address alone and exchanges the CONTENT of the human's fighter (H) and the partner (P): the actor block and the heap
// objects each owns (recorded by alloc_hook), word by word, with
//   - plain numbers exchanged (position, hp, bravery, action state, animation ...),
//   - pointers into either fighter's owned blocks translated to the counterpart block (offset kept), so the sub-objects of the content
//     that moved point at the content's new home,
//   - pointers to anything else (shared resources, list neighbours, registrations, targets) left where they are: they belong to the
//     ADDRESS (identity), not to the character,
//   - the human brain and the lock block left in place whole (the pad and the camera slot belong to the address).
bool isptr(uint32_t w) { return (w & 3u) == 0 && w >= 0x08800000u && w < 0x0C000000u; }
struct BlkPair { Blk h, p; bool keep; };
std::vector<BlkPair> g_pairs;
uint32_t trans(uint32_t v) {
    for (const BlkPair& b : g_pairs) {
        if (v >= b.h.addr && v < b.h.addr + b.h.size) return v - b.h.addr + b.p.addr;
        if (v >= b.p.addr && v < b.p.addr + b.p.size) return v - b.p.addr + b.h.addr;
    }
    return v;
}
// Extras and the combat modules. The battle controller (FUN_08886C88, once per frame) runs the combat modules (hit results -> bravery / HP damage,
// HUD accounting) for the two slot fighters, each with that fighter's assist actor (+0x1108): FUN_08AFD424 walks [slot actor, its assist] and sends
// them the hit messages. An extra is neither, so the victim of its attacks reacted but nobody took damage. The extra therefore takes the place of
// its slot fighter's assist pointer (the retail assist actor is left alone otherwise; PSPRECOMP_TAGTEAM_KEEPASSIST=1 keeps the old behaviour).
void fix_assist() {
    if (!std::getenv("PSPRECOMP_TAGTEAM_SWAPASSIST")) return;   // old behaviour (the extra replaces the slot fighter's assist): the retail assist then stops working and the extra is treated as an invisible assist; off by default
    for (int i = 0; i < g_nextra; i++) {
        Extra& e = g_extra[i];
        if (!e.started || !ok(e.actor, 0x12D0) || !ok(e.slot_actor, 0x12D0)) continue;
        wr(e.slot_actor + 0x1108u, e.actor);
        wr(e.actor + 0x1108u, 0);
    }
}

bool content_swap(uint32_t H, uint32_t P) {
    auto ih = g_owned.find(H), ip = g_owned.find(P);
    if (ih == g_owned.end() || ip == g_owned.end()) { std::fprintf(stderr, "[TAGTEAM] swap: no ownership record (H %zu, P %zu)\n", ih == g_owned.end() ? 0u : ih->second.size(), ip == g_owned.end() ? 0u : ip->second.size()); return false; }
    const std::vector<Blk>& bh = ih->second; const std::vector<Blk>& bp = ip->second;
    if (bh.size() != bp.size()) { std::fprintf(stderr, "[TAGTEAM] swap: H owns %zu blocks, P %zu: not clones\n", bh.size(), bp.size()); return false; }
    for (size_t k = 0; k < bh.size(); k++) if (bh[k].size != bp[k].size) { std::fprintf(stderr, "[TAGTEAM] swap: block %zu size %u vs %u\n", k, bh[k].size, bp[k].size); return false; }
    const uint32_t keepH[2] = {rd(H + ACT_BRAIN), rd(H + ACT_LOCKBLK)}, keepP[2] = {rd(P + ACT_BRAIN), rd(P + ACT_LOCKBLK)};
    const uint8_t flagH = g_ram[(H + 0x5B0u) & 0x07FFFFFFu], flagP = g_ram[(P + 0x5B0u) & 0x07FFFFFFu];   // +0x5B0 bit 2: "this actor is the player" (the HUD's FUN_08842164 asks it): belongs to the address
    g_pairs.clear();
    int kept = 0;
    for (size_t k = 0; k < bh.size(); k++) {
        bool keep = false;
        for (int i = 0; i < 2; i++) if (bh[k].addr == keepH[i]) { keep = true; if (bp[k].addr != keepP[i]) { std::fprintf(stderr, "[TAGTEAM] swap: kept block %zu pairs %08X with %08X, not %08X\n", k, bh[k].addr, bp[k].addr, keepP[i]); return false; } }
        if (keep) kept++;
        g_pairs.push_back({bh[k], bp[k], keep});
    }
    if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: %zu block pairs (%d kept: brain %08X/%08X, lock block %08X/%08X)\n", g_pairs.size(), kept, keepH[0], keepP[0], keepH[1], keepP[1]);
    struct W { uint32_t a, v; };
    std::vector<W> writes;
    size_t nnum = 0, ntrans = 0, next = 0;
    auto f = [&](uint32_t w, bool* ext) { *ext = false; if (!isptr(w)) return w; const uint32_t t = trans(w); if (t != w) return t; *ext = true; return w; };
    for (const BlkPair& b : g_pairs) {
        if (b.keep) continue;
        for (uint32_t o = 0; o < b.h.size; o += 4) {
            const uint32_t a = b.h.addr + o, c = b.p.addr + o, x = rd(a), y = rd(c);
            if (x == y) continue;
            bool ex = false, ey = false;
            const uint32_t fx = f(x, &ex), fy = f(y, &ey);   // what x / y become after moving to the other block
            const uint32_t na = ey ? x : fy;                  // an external pointer stays with its address
            const uint32_t nc = ex ? y : fx;
            if (ex || ey) next++; else if (isptr(x) || isptr(y)) ntrans++; else nnum++;
            if (na != x) writes.push_back({a, na});
            if (nc != y) writes.push_back({c, nc});
        }
    }
    for (const W& w : writes) wr(w.a, w.v);
    g_ram[(H + 0x5B0u) & 0x07FFFFFFu] = static_cast<uint8_t>((g_ram[(H + 0x5B0u) & 0x07FFFFFFu] & ~4u) | (flagH & 4u));
    g_ram[(P + 0x5B0u) & 0x07FFFFFFu] = static_cast<uint8_t>((g_ram[(P + 0x5B0u) & 0x07FFFFFFu] & ~4u) | (flagP & 4u));
    if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: content exchanged: %zu numeric, %zu translated pointer, %zu external-pointer words left with their address, %zu writes\n", nnum, ntrans, next, writes.size());
    fix_assist();
    return true;
}

void swap_content() {
    if (!g_bs) return;
    const uint32_t hs = rd(g_bs + 84u), H = rd(g_bs + 56u + 4u * hs);
    Extra* pe = nullptr;
    for (int i = 0; i < g_nextra; i++) if (g_extra[i].started && g_extra[i].side == side_of(H) && alive(g_extra[i].actor)) pe = &g_extra[i];
    if (!pe || !ok(H, 0x12D0) || !alive(H)) { if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: no living partner\n"); return; }
    const uint32_t P = pe->actor;
    if (content_swap(H, P)) {
        if (rd(rd(H + ACT_LOCKBLK) + 264u) != 1) call(LOCK_MODE, rd(H + ACT_LOCKBLK), 1, 0);
        if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: pad character is now the one that stood at %08X (H %08X, P %08X)\n", P, H, P);
    }
}

void swap_roles() {
    static const int mode0 = std::getenv("PSPRECOMP_TAGTEAM_SWAP_MODE") ? std::atoi(std::getenv("PSPRECOMP_TAGTEAM_SWAP_MODE")) : 4;
    if (mode0 == 4) { swap_content(); return; }
    if (!g_bs) return;
    const uint32_t hs = rd(g_bs + 84u), slot = g_bs + 56u + 4u * hs, H = rd(slot);
    Extra* pe = nullptr;
    for (int i = 0; i < g_nextra; i++) if (g_extra[i].started && g_extra[i].side == side_of(H) && alive(g_extra[i].actor)) pe = &g_extra[i];
    if (!pe || !ok(H, 0x12D0) || !alive(H)) { if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: no living partner\n"); return; }
    const uint32_t P = pe->actor, bh = rd(H + ACT_BRAIN), bp = rd(P + ACT_BRAIN);
    if (g_log) { const uint32_t T = rd(0x089D6A10u); std::fprintf(stderr, "[TAGTEAM] swap: bs %08X slots %08X %08X human slot %u; [0x089D6A10] = %08X slots %08X %08X\n", g_bs, rd(g_bs + 56u), rd(g_bs + 60u), hs, T, ok(T, 0x80) ? rd(T + 56u) : 0u, ok(T, 0x80) ? rd(T + 60u) : 0u); }
    if (!ok(bh, 0x80) || !ok(bp, 0x80)) return;
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_SWAPSCAN")) {
        std::fprintf(stderr, "[SWAPSCAN] mgr %08X bs %08X H %08X P %08X\n", dissidia_camera_manager(), g_bs, H, P);
        scan_val(H, "H", H, P); scan_val(P, "P", H, P);
        scan_val(rd(H + ACT_LOCKBLK), "lbH", H, P); scan_val(rd(P + ACT_LOCKBLK), "lbP", H, P);
        scan_val(bh, "brainH", H, P); scan_val(bp, "brainP", H, P);
        const uint32_t C = rd(g_bs + 56u + 4u * (1u - hs)), lbH_ = rd(H + ACT_LOCKBLK), lbP_ = rd(P + ACT_LOCKBLK);
        const uint32_t ws[] = {0x08B63310u, 0x08B876A0u, 0x08BAEA04u, 0x08C00720u, 0x09AEEFD0u, 0x089D4890u, 0x08BAE204u, 0x08BAE9FCu, 0x08C00AD4u, 0x09785AECu, 0x09785AFCu, 0x09AE9340u};
        for (uint32_t w : ws) dump_win(w, "w", H, P, C, lbH_, lbP_);
    }
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_HUDPROBE")) hud_probe("before");
    static const int mode = std::getenv("PSPRECOMP_TAGTEAM_SWAP_MODE") ? std::atoi(std::getenv("PSPRECOMP_TAGTEAM_SWAP_MODE")) : 3;   // 1 brains only, 2 slots only, 3 both (debugging)
    if (mode & 1) { wr(H + ACT_BRAIN, bp); wr(P + ACT_BRAIN, bh); wr(bh + 4u, P); wr(bp + 4u, H); }   // the pad (human brain) goes to the partner, the AI brain to the old human, each brain's owner
    if (mode & 2) { wr(slot, P); pe->actor = H; pe->slot_actor = P; }   // slot table: the retail HUD / camera / lock cycle follow the new human
    // the camera manager keeps one lock block per side (mgr + 0x6D0 + 4 * side)
    const uint32_t mgr = dissidia_camera_manager(), side = side_of(H), lbH = rd(H + ACT_LOCKBLK), lbP = rd(P + ACT_LOCKBLK);
    if (mgr) { const uint32_t at = mgr + 0x6D0u + 4u * side; if (rd(at) == lbH) wr(at, lbP); else if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: camera slot %08X holds %08X, expected %08X\n", at, rd(at), lbH); }
    // Other shared references to the human fighter (found by scanning RAM for H at swap time): the camera manager's fighter list
    // (mgr + 0x6360: [CPU, human]), an EBOOT fighter list (0x089D4890: [CPU, human]) and 0x08B63310. H is replaced by P there.
    static const uint32_t mask = std::getenv("PSPRECOMP_TAGTEAM_SWAP_LISTS") ? static_cast<uint32_t>(std::strtoul(std::getenv("PSPRECOMP_TAGTEAM_SWAP_LISTS"), nullptr, 16)) : 7u;
    const uint32_t lists[3] = {mgr ? mgr + 0x6360u : 0u, 0x089D4890u, 0x08B63310u};
    for (int i = 0; i < 3; i++) if ((mask & (1u << i)) && lists[i]) { const uint32_t v = rd(lists[i]); if (v == H) wr(lists[i], P); else if (v == P) wr(lists[i], H); else if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: list %08X holds %08X (H %08X P %08X), left alone\n", lists[i], v, H, P); }
    // the new human locks on to the foe, the old one keeps the foe he had
    if (rd(lbP + 264u) != 1) call(LOCK_MODE, lbP, 1, 0);
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_HUDPROBE")) hud_probe("after");
    if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: pad %08X -> %08X (brains %08X / %08X)\n", H, P, bh, bp);
}

void tick_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t brain = static_cast<uint32_t>(ctx->r[4]);
    g_ram = rdram; g_ctx = ctx;
    const uint32_t owner = rd(brain + 4);
    if (g_nextra && g_extra[0].started && owner == g_extra[0].actor) hud_update();
    if (g_log && g_bs) {   // the human's lock cycle selector (retail mode handler [1], +28) and its target
        static int last_sel = -99; static uint32_t last_t = 0, last_mode = 99;
        const uint32_t hum = rd(g_bs + 56u + 4u * rd(g_bs + 84u)), L = ok(hum, 0x12D0) ? rd(hum + 0x5ACu) : 0u;
        if (ok(L, 0x200) && owner == rd(hum + 0x5C4u + 0u) + 0u) {}
        if (ok(L, 0x200)) {
            const uint32_t h1 = rd(L + 304u);
            const int sel = ok(h1, 0x40) ? static_cast<int>(rd(h1 + 28u)) : -99; const uint32_t t = rd(L + 252u);
            if (sel != last_sel || t != last_t || rd(L + 264u) != last_mode) { last_mode = rd(L + 264u); std::fprintf(stderr, "[TAGTEAM] t=%.1fs human lock: sel %d -> %d, mode %u, target %08X -> %08X (+0x2FC %08X foe %08X)\n", dissidia_battle_age() / 60.0f, last_sel, sel, rd(L + 264u), last_t, t, rd(hum + 0x2FCu), rd(hum + 0x300u)); last_sel = sel; last_t = t; }
        }
    }
    if (g_log && g_bs && std::getenv("PSPRECOMP_TAGTEAM_KO")) {   // debugging: "<who>:<seconds>" puts a fighter on 1 HP and 0 bravery so the next hit KOs it (who = human, cpu, enemy2, partner)
        static bool done = false; static uint32_t target = 0; static int lasthp = -1;
        char who[16] = {}; float at = 0; std::sscanf(std::getenv("PSPRECOMP_TAGTEAM_KO"), "%15[a-z0-9]:%f", who, &at);
        if (!done && dissidia_battle_age() / 60.0f >= at && g_nextra) {
            uint32_t a = 0;
            const uint32_t hs = rd(g_bs + 84u);
            if (!std::strcmp(who, "human")) a = rd(g_bs + 56u + 4u * hs);
            else if (!std::strcmp(who, "cpu")) a = rd(g_bs + 56u + 4u * (1u - hs));
            else for (int i = 0; i < g_nextra; i++) if (g_extra[i].started && ((!std::strcmp(who, "enemy2") && g_extra[i].side != side_of(rd(g_bs + 56u + 4u * hs))) || (!std::strcmp(who, "partner") && g_extra[i].side == side_of(rd(g_bs + 56u + 4u * hs))))) a = g_extra[i].actor;
            if (ok(a, 0x12D0)) {
                const uint32_t st = rd(a + 0x52Cu);
                const uint32_t hp0 = std::getenv("PSPRECOMP_TAGTEAM_KO_HPVAL") ? static_cast<uint32_t>(std::atoi(std::getenv("PSPRECOMP_TAGTEAM_KO_HPVAL"))) : (std::getenv("PSPRECOMP_TAGTEAM_KO_HP0") ? 0u : 1u);   // HP0=1: the live HP goes to 0 at once (the game's own update has to notice)
                if (ok(st, 0x40)) { wr(st + 0xCu, hp0); if (!std::getenv("PSPRECOMP_TAGTEAM_KO_NOBRV")) { g_ram[(st + 0x14u) & 0x07FFFFFFu] = 0; g_ram[(st + 0x15u) & 0x07FFFFFFu] = 0; } }
                target = a; done = true;
                std::fprintf(stderr, "[TAGTEAM] t=%.1fs KO prepared on %s %08X (hp 1, brv 0)\n", dissidia_battle_age() / 60.0f, who, a);
            }
        }
        if (target && owner == target && std::getenv("PSPRECOMP_TAGTEAM_KO_DUMP")) {   // every ~1 s: the actor's HP words and the status object
            // every frame: print the words of the actor (0x100..0x1100 region of interest) and of its status object that CHANGED since the last frame
            static std::vector<uint32_t> prev; static uint32_t prev_t = 0;
            const uint32_t st = rd(target + 0x52Cu);
            std::vector<uint32_t> cur;
            for (uint32_t o = 0x370; o < 0x3C0; o += 4) cur.push_back(rd(target + o));      // flags
            for (uint32_t o = 0x10A0; o < 0x10D0; o += 4) cur.push_back(rd(target + o));    // stats
            for (uint32_t o = 0; o < 0x40; o += 4) cur.push_back(ok(st, 0x80) ? rd(st + o) : 0);   // status object
            if (prev_t != target || prev.size() != cur.size()) { prev = cur; prev_t = target; }
            else {
                bool any = false; char buf[1200]; int n = std::snprintf(buf, sizeof buf, "[TAGTEAM] t=%.2fs script %d changed:", dissidia_battle_age() / 60.0f, static_cast<int16_t>(rd(target + 0x180u) & 0xFFFF));
                for (size_t i = 0; i < cur.size() && n < 1100; i++) if (cur[i] != prev[i]) {
                    any = true;
                    const char* area = i < 20 ? "flags+0x370" : i < 32 ? "stat+0x10A0" : "status";
                    const uint32_t off = i < 20 ? 0x370 + 4 * i : i < 32 ? 0x10A0 + 4 * (i - 20) : 4 * (i - 32);
                    n += std::snprintf(buf + n, sizeof buf - n, " %s[%X] %08X>%08X", area, off, prev[i], cur[i]);
                }
                if (any) std::fprintf(stderr, "%s\n", buf);
                prev = cur;
            }
        }
        if (target && owner == target) { const uint32_t st2 = rd(target + 0x52Cu); const int hp = ok(st2, 0x40) ? static_cast<int32_t>(rd(st2 + 0xCu)) : -1; if (hp != lasthp) { std::fprintf(stderr, "[TAGTEAM] t=%.1fs KO target %08X live hp %d (max %u) script %d flags %08X\n", dissidia_battle_age() / 60.0f, target, hp, rd(target + 0x10B0u), static_cast<int16_t>(rd(target + 0x180u) & 0xFFFF), rd(target + 0x380u)); lasthp = hp; } }
    }
    if (g_log && g_bs && std::getenv("PSPRECOMP_TAGTEAM_FOESCAN")) {   // debug: who holds the two enemy fighters' addresses, before and after the lock cycle
        static bool d1 = false, d2 = false;
        const uint32_t hum = rd(g_bs + 56u + 4u * rd(g_bs + 84u));
        const float tt = dissidia_battle_age() / 60.0f;
        if (owner == hum && ((!d1 && tt > 8.0f) || (!d2 && tt > 11.0f))) {
            const bool first = !d1; (first ? d1 : d2) = true;
            uint32_t es[2] = {0, 0}; int k = 0;
            uint32_t f[6]; const int n = all_fighters(f);
            for (int i = 0; i < n; i++) if (side_of(f[i]) != side_of(hum) && k < 2) es[k++] = f[i];
            std::fprintf(stderr, "[FOESCAN] t=%.1f pass %d human %08X foe %08X enemies %08X %08X\n", tt, first ? 1 : 2, hum, rd(hum + 0x300u), es[0], es[1]);
            for (int j = 0; j < 2; j++) if (es[j]) scan_val(es[j], j == 0 ? "enemyA" : "enemyB", hum, es[j]);
        }
    }
    if (g_log && g_bs && std::getenv("PSPRECOMP_TAGTEAM_HPLOG") && g_nextra) {   // debug: live HP / BRV changes of every fighter, tagged by role
        static const bool boost = std::getenv("PSPRECOMP_TAGTEAM_HPBOOST") != nullptr;
        struct Last { uint32_t a; int hp, brv; }; static Last last[8] = {}; static int nl = 0; static uint32_t tk = 0;
        const uint32_t st = rd(owner + 0x52Cu);
        if (ok(st, 0x40)) {
            int i = 0; while (i < nl && last[i].a != owner) i++;
            if (i == nl && nl < 8) { last[nl++] = {owner, 0x7FFFFFFF, 0x7FFFFFFF}; }
            const uint32_t hs = rd(g_bs + 84u);
            const char* role = owner == rd(g_bs + 56u + 4u * hs) ? "slotH" : owner == rd(g_bs + 56u + 4u * (1u - hs)) ? "slotC" : (extra_of(owner) && extra_of(owner)->side == side_of(rd(g_bs + 56u + 4u * hs))) ? "extraP" : "extraE";
            const int hp = static_cast<int32_t>(rd(st + 0xCu)), brv = static_cast<int>(rdh(st + 0x14u));
            if (boost && (++tk % 120) == 0) { wr(st + 0x14u, (4000u << 16) | 4000u); }   // keep every fighter's bravery high so the AI's HP attacks do real damage
            if (hp != last[i].hp || (brv != last[i].brv && std::getenv("PSPRECOMP_TAGTEAM_HPLOG")[0] == '2'))
                std::fprintf(stderr, "[HP] t=%.2fs %s %08X hp %d -> %d brv %d -> %d foe %08X tgt %08X script %d\n", dissidia_battle_age() / 60.0f, role, owner, last[i].hp, hp, last[i].brv, brv, rd(owner + 0x300u), rd(owner + 0x2FCu), static_cast<int16_t>(rd(owner + 0x180u) & 0xFFFF));
            last[i].hp = hp; last[i].brv = brv;
        }
    }
    if (g_log && g_bs && g_nextra && std::getenv("PSPRECOMP_TAGTEAM_HPSCAN")) {   // debug: who holds each fighter's address (outside the fighter's own blocks), once after 8 s
        static bool done = false;
        if (!done && owner == rd(g_bs + 56u) && dissidia_battle_age() / 60.0f > 8.0f && g_extra[0].started && g_extra[g_nextra - 1].started) {
            done = true;
            uint32_t f[6]; const int n = all_fighters(f);
            const uint32_t hs = rd(g_bs + 84u);
            for (int pr = 0; pr < 2; pr++) {   // actor diff of each slot fighter against its clone: words where exactly one is 0, or both are small numbers that differ
                const uint32_t A = (pr == 0) ? rd(g_bs + 56u + 4u * hs) : rd(g_bs + 56u + 4u * (1u - hs));
                uint32_t B = 0; for (int i = 0; i < g_nextra; i++) if (g_extra[i].slot_actor == A) B = g_extra[i].actor;
                for (uint32_t o = 0; o < 0x12D0u && B; o += 4) {
                    const uint32_t x = rd(A + o), y = rd(B + o);
                    if (x == y) continue;
                    const bool px = x >= 0x08800000u && x < 0x0C000000u, py = y >= 0x08800000u && y < 0x0C000000u;
                    if (px && py) continue;   // two different pointers: expected
                    std::fprintf(stderr, "[HPDIFF] %s +%04X slot %08X clone %08X\n", pr == 0 ? "H/P" : "C/E", o, x, y);
                }
            }
            for (int i = 0; i < n; i++) for (uint32_t o = 0; o < 0x12D0u; o += 4) { const uint32_t w = rd(f[i] + o); if (w >= 0x08800000u && w < 0x0BF00000u && (w & 3) == 0 && w != f[i] && ok(w, 0x40) && rd(w + 16u) == f[i]) std::fprintf(stderr, "[HPCOMP] %s %08X +%04X -> %08X (obj+16 == actor; obj words: %08X %08X %08X %08X %08X)\n", role_of(f[i]), f[i], o, w, rd(w), rd(w + 4), rd(w + 8), rd(w + 12), rd(w + 20)); }
            for (int i = 0; i < n; i++) {   // objects X (anywhere) with X+16 == actor: the per-fighter modules; then who points to X
                int c = 0;
                for (uint32_t x = 0x08800000u; x < 0x0BF00000u && c < 12; x += 4) {
                    if (x >= f[i] && x < f[i] + 0x12D0u) continue;
                    if (rd(x + 16u) != f[i]) continue;
                    std::fprintf(stderr, "[HPMOD] %s %08X: obj %08X (words %08X %08X %08X %08X | %08X %08X)\n", role_of(f[i]), f[i], x, rd(x), rd(x + 4), rd(x + 8), rd(x + 12), rd(x + 20), rd(x + 24)); c++;
                    int h = 0;
                    for (uint32_t a = 0x08800000u; a < 0x0BF00000u && h < 4; a += 4) if (rd(a) == x && !(a >= f[i] && a < f[i] + 0x12D0u)) { std::fprintf(stderr, "[HPMOD]    held at %08X (%08X %08X)\n", a, rd(a - 4), rd(a + 4)); h++; }
                }
            }
            for (int i = 0; i < n; i++) { const uint32_t vt = rd(f[i] + 1348u), e = vt + 304u; std::fprintf(stderr, "[HPVT] %s %08X vtable %08X +304: adj %d fn %08X\n", role_of(f[i]), f[i], vt, static_cast<int16_t>(rdh(e)), rd(e + 4u)); }
            for (int i = 0; i < n; i++) {
                const char* r3 = f[i] == rd(g_bs + 56u + 4u * hs) ? "slotH" : f[i] == rd(g_bs + 56u + 4u * (1u - hs)) ? "slotC" : (side_of(f[i]) == side_of(rd(g_bs + 56u + 4u * hs))) ? "extraP" : "extraE";
                std::fprintf(stderr, "[HPWORDS] %s %08X +4A0:", r3, f[i]); for (uint32_t o = 0x4A0; o < 0x4C0; o += 4) std::fprintf(stderr, " %08X", rd(f[i] + o));
                std::fprintf(stderr, " | +1100:"); for (uint32_t o = 0x1100; o < 0x1130; o += 4) std::fprintf(stderr, " %08X", rd(f[i] + o));
                std::fprintf(stderr, " | +10C0:"); for (uint32_t o = 0x10C0; o < 0x10E0; o += 4) std::fprintf(stderr, " %08X", rd(f[i] + o)); std::fprintf(stderr, "\n");
            }
            for (int i = 0; i < n; i++) {   // fighter addresses stored inside each actor
                const char* r2 = f[i] == rd(g_bs + 56u + 4u * hs) ? "slotH" : f[i] == rd(g_bs + 56u + 4u * (1u - hs)) ? "slotC" : (side_of(f[i]) == side_of(rd(g_bs + 56u + 4u * hs))) ? "extraP" : "extraE";
                for (uint32_t o = 0; o < 0x12D0u; o += 4) for (int j = 0; j < n; j++) if (rd(f[i] + o) == f[j]) std::fprintf(stderr, "[HPREF] %s +%04X -> %08X\n", r2, o, f[j]);
            }
            for (int i = 0; i < n; i++) {
                const char* role = f[i] == rd(g_bs + 56u + 4u * hs) ? "slotH" : f[i] == rd(g_bs + 56u + 4u * (1u - hs)) ? "slotC" : (side_of(f[i]) == side_of(rd(g_bs + 56u + 4u * hs))) ? "extraP" : "extraE";
                int c = 0;
                for (uint32_t a = 0x08800000u; a < 0x0BF00000u && c < 400; a += 4) {
                    if (a >= f[i] && a < f[i] + 0x12D0u) continue;
                    if (rd(a) == f[i]) {
                        std::fprintf(stderr, "[HPSCAN] %s %08X held at %08X\n", role, f[i], a); c++;
                        if (a < 0x09000000u) { std::fprintf(stderr, "[HPDUMP] %s at %08X:", role, a); for (int k = -6; k <= 9; k++) std::fprintf(stderr, " %08X", rd(a + 4u * static_cast<uint32_t>(k + 0))); std::fprintf(stderr, "\n"); }
                    }
                }
            }
        }
    }
    if (g_log && g_bs && g_nextra >= 2 && g_extra[0].started && g_extra[1].started && std::getenv("PSPRECOMP_TAGTEAM_HPMATRIX") && owner == rd(g_bs + 56u)) {
        // debug: every 5 s one attacker/victim pair of the four fighters: the victim is put in front of the attacker, the attacker starts an attack action
        // (PSPRECOMP_TAGTEAM_HPMATRIX=<first action id>), and the victim's HP / BRV are logged before and 2 s after.
        static int step = -1; static float t0 = 0; static uint32_t att = 0, vic = 0; static int hp0 = 0, brv0 = 0, act = 0; static const char* ra = ""; static const char* rv = ""; static std::vector<int> scr, vscr; static int ah[4] = {}, ab[4] = {};
        const float tt = dissidia_battle_age() / 60.0f;
        const uint32_t hs = rd(g_bs + 84u);
        const uint32_t F[4] = {rd(g_bs + 56u + 4u * hs), rd(g_bs + 56u + 4u * (1u - hs)), g_extra[0].side == side_of(rd(g_bs + 56u + 4u * hs)) ? g_extra[0].actor : g_extra[1].actor, g_extra[0].side == side_of(rd(g_bs + 56u + 4u * hs)) ? g_extra[1].actor : g_extra[0].actor};
        const char* R[4] = {"slotH", "slotC", "extraP", "extraE"};
        auto stv = [&](uint32_t a, int& hp, int& brv) { const uint32_t st = rd(a + 0x52Cu); hp = static_cast<int32_t>(rd(st + 0xCu)); brv = static_cast<int>(rdh(st + 0x14u)); };
        if (step >= 0 && att && tt - t0 >= (std::getenv("PSPRECOMP_TAGTEAM_HPM_SWEEP") ? 1.6f : 4.0f)) {
            int hp, brv; stv(vic, hp, brv);
            { std::fprintf(stderr, "[HPM2] %s -> %s changed:", ra, rv); for (int q = 0; q < 4; q++) { int h2, b2; stv(F[q], h2, b2); if (h2 != ah[q] || b2 != ab[q]) std::fprintf(stderr, " %s(hp %d->%d brv %d->%d)", R[q], ah[q], h2, ab[q], b2); } std::fprintf(stderr, "\n"); }
            std::fprintf(stderr, "[HPM] %s -> %s action %d: victim hp %d -> %d  brv %d -> %d  att scripts:", ra, rv, act, hp0, hp, brv0, brv); for (int x : scr) std::fprintf(stderr, " %d", x); std::fprintf(stderr, " | victim scripts:"); for (int x : vscr) std::fprintf(stderr, " %d", x); std::fprintf(stderr, "\n"); scr.clear(); vscr.clear();
            att = 0; g_down.clear(); g_ram[0x0BFFF000u & 0x07FFFFFFu] = 0;   // call-count gate off (PSPRECOMP_FIRSTCALL=0BFFF000:<file>)
        }
        if (att) { for (int q = 0; q < 4; q++) { if (F[q] == F[0]) continue; wr(F[q] + 0x380u, (F[q] == att && !std::getenv("PSPRECOMP_TAGTEAM_HPM_AI")) ? (rd(F[q] + 0x380u) | 2u) : (F[q] == att ? (rd(F[q] + 0x380u) & ~2u) : (rd(F[q] + 0x380u) | 2u))); } }
        if (att) { const int a1 = static_cast<int16_t>(rd(att + 0x180u) & 0xFFFF), v1 = static_cast<int16_t>(rd(vic + 0x180u) & 0xFFFF); if (scr.empty() || scr.back() != a1) scr.push_back(a1); if (vscr.empty() || vscr.back() != v1) vscr.push_back(v1); }
        if (att && att != F[0] && tt - t0 < (std::getenv("PSPRECOMP_TAGTEAM_HPM_SWEEP") ? 1.6f : 4.0f)) {   // AI attacker: keep the victim in front of it
            const uint32_t sp0 = static_cast<uint32_t>(g_ctx->r[29]);
            g_ctx->r[29] = static_cast<int32_t>(sp0 - 0x100u);
            const uint32_t vec = sp0 - 0xC0u;
            wrf(vec, rdf(att + 0x80u) + 1.6f); wrf(vec + 4, rdf(att + 0x84u)); wrf(vec + 8, rdf(att + 0x88u)); wrf(vec + 12, 1.0f);
            call(PLACE, vic, vec, 1); g_ctx->f[12] = 0.0f; g_ctx->f[13] = 0.0f;
            g_ctx->r[29] = static_cast<int32_t>(sp0);
        }
        if (!att && tt >= 9.0f && tt - t0 >= (std::getenv("PSPRECOMP_TAGTEAM_HPM_SWEEP") ? 2.5f : 6.0f)) {
            step++; t0 = tt;
            const int base = std::atoi(std::getenv("PSPRECOMP_TAGTEAM_HPMATRIX"));
            // pairs: opposite sides only (0,2 = human team, 1,3 = enemy team)
            static const int P[8][2] = {{0, 1}, {0, 3}, {2, 1}, {2, 3}, {1, 0}, {3, 0}, {1, 2}, {3, 2}};
            int pi = step % 8; const int round = step / 8;
            const int ids[3] = {base, base + 1, base + 70};
            act = ids[round % 3];
            if (std::getenv("PSPRECOMP_TAGTEAM_HPM_SWEEP")) { pi = std::atoi(std::getenv("PSPRECOMP_TAGTEAM_HPM_SWEEP")); act = base + step; }   // one pair, action ids base, base+1, ...
            const uint32_t a = F[P[pi][0]], v = F[P[pi][1]];
            if (ok(a, 0x12D0) && ok(v, 0x12D0)) {
                att = a; vic = v; ra = R[P[pi][0]]; rv = R[P[pi][1]];
                g_ram[0x0BFFF000u & 0x07FFFFFFu] = (round == 0) ? static_cast<uint8_t>(pi + 1) : 0;   // call-count phase of this pair
                g_down.clear(); for (int q = 0; q < 4; q++) if (F[q] != F[0] && (F[q] != a || !std::getenv("PSPRECOMP_TAGTEAM_HPM_AI"))) g_down.push_back(F[q]);   // everybody else stands still (AI gated)
                stv(v, hp0, brv0); for (int q = 0; q < 4; q++) stv(F[q], ah[q], ab[q]);
                const uint32_t sp0 = static_cast<uint32_t>(g_ctx->r[29]);
                g_ctx->r[29] = static_cast<int32_t>(sp0 - 0x100u);
                const uint32_t vec = sp0 - 0xC0u;
                wrf(vec, rdf(a + 0x80u) + 1.6f); wrf(vec + 4, rdf(a + 0x84u)); wrf(vec + 8, rdf(a + 0x88u)); wrf(vec + 12, 1.0f);
                call(PLACE, v, vec, 1);
                g_ctx->f[12] = 0.0f; g_ctx->f[13] = 0.0f;
                tagteam_hpm_marker(a, v, step);
                if (std::getenv("PSPRECOMP_TAGTEAM_HPM_GDB") && false) {   // stall so that a debugger can be attached and a hardware watchpoint set on the victim's BRV word
                    const uint32_t wa = rd(v + 0x52Cu) + 0x14u;
                    std::fprintf(stderr, "[GDBWAIT] victim BRV guest %08X host %p\n", wa, static_cast<void*>(g_ram + (wa & 0x07FFFFFFu))); std::fflush(stderr);
                    if (FILE* f = std::fopen("/home/schindler/dissidia/tt/wa.txt", "w")) { std::fprintf(f, "%p\n", static_cast<void*>(g_ram + (wa & 0x07FFFFFFu))); std::fclose(f); }
                    std::raise(SIGTRAP);   // under gdb (tt/gdbwrap.sh): stops here, the gdb script sets the watchpoint
                }
                if (std::getenv("PSPRECOMP_TAGTEAM_HPM_WATCH")) { psp_mem_watch_set(rd(v + 0x52Cu) + 0x14u); std::fprintf(stderr, "[HPM] window %s -> %s: watching victim BRV\n", ra, rv); }
                call(SET_FOE, a, v);
                call(LOCK_SET, rd(a + 0x5ACu), v);
                { const uint32_t st = rd(a + 0x52Cu); wr(st + 0x14u, (3000u << 16) | 3000u); }
                if (std::getenv("PSPRECOMP_TAGTEAM_HPM_VBRV0")) { const uint32_t sv = rd(v + 0x52Cu); wr(sv + 0x14u, rd(sv + 0x14u) & 0xFFFF0000u); }   // victim's bravery 0: an HP attack does HP damage
                g_ctx->f[12] = 0.0f; g_ctx->f[13] = 0.0f;
                if (!std::getenv("PSPRECOMP_TAGTEAM_HPM_PAD") && (a == F[0] || !std::getenv("PSPRECOMP_TAGTEAM_HPM_AI"))) call(START_ACTION, a + 304u, static_cast<uint32_t>(act), 0);
                g_ctx->r[29] = static_cast<int32_t>(sp0);
            }
        }
    }
    static bool intro_seen = false;
    if (!g_late_q.empty() && g_bs && rd(g_bs + 72) != 0 && !g_late_ready) {   // the intro scripts (ids >= 1000) have run and the human is idle again
        const int cur = static_cast<int16_t>(rd(g_late_q[0].slot_actor + 0x180u) & 0xFFFFu);
        if (cur >= 1000) intro_seen = true;
        else if (intro_seen && (cur == 1 || cur == 2)) g_late_ready = true;
        if (!g_late_wait) g_late_ready = true;
    }
    if (!g_late_q.empty() && g_late_ready) {   // the intro is over: make the late extras now
        for (const Late& l : g_late_q) {
            Extra& e = g_extra[g_nextra];
            e.actor = spawn_fighter(g_bs, l.side, l.foe, l.cpu_side, l.info_slot, l.slot_actor); e.side = l.side; e.foe = l.foe; e.slot_actor = l.slot_actor; e.started = false;
            if (e.actor) { e.undo = g_last_undo; g_nextra++; }
        }
        g_late_q.clear(); intro_seen = false;
    }
    {   // start the extras once all of them are loaded; the registrations are taken back in reverse spawn order
        bool pending = false, all_ready = true;
        for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started) { pending = true; if (!call(FIGHTER_READY, g_extra[i].actor)) all_ready = false; }
        if (pending && all_ready && g_bs && rd(g_bs + 72) != 0) {
            for (int i = g_nextra - 1; i >= 0; i--) if (!g_extra[i].started) apply_undo(g_extra[i]);
            for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started) start_extra(g_extra[i]);
        }
    }
    if (Extra* e = extra_of(owner); e && e->started) {
        if (g_tagin && !alive(owner)) return;   // a KO'd extra stays down: its brain does not think
        if (rd(owner + 0x2FCu) == 0 && g_orig_refresh) { const recomp_context sv = *g_ctx; g_ctx->r[4] = static_cast<int32_t>(owner); g_orig_refresh(g_ram, g_ctx); *g_ctx = sv; }
        { static const int kf = std::getenv("PSPRECOMP_TAGTEAM_KOFIX") ? std::atoi(std::getenv("PSPRECOMP_TAGTEAM_KOFIX")) : 0; wr(owner + 0x380u, (is_down(owner) && !(kf & 8)) ? (rd(owner + 0x380u) | 2u) : (rd(owner + 0x380u) & ~2u)); }   // bit 2 of +0x380 makes the CPU brain skip the actor; the retail two lose it after the intro
    }
    static uint32_t seen[8] = {}; static int cnt[8] = {}; static int n = 0;
    int i = 0; while (i < n && seen[i] != brain) i++;
    if (i == n && n < 8) { seen[n++] = brain; }
    if (g_log && i < 8 && (cnt[i]++ % 300) == 0 && ok(brain, 0x40))
        std::fprintf(stderr, "[TAGTEAM] brain tick %08X (owner %08X) enabled=%u state=%u call#%d tgt2FC=%08X foe300=%08X partner1108=%08X lockT=%08X act380=%08X%s\n", brain, owner, g_ram[(brain) & 0x07FFFFFFu], g_ram[(brain + 12) & 0x07FFFFFFu], cnt[i], rd(owner + 0x2FCu), rd(owner + 0x300u), rd(owner + 0x1108u), rd(rd(owner + 0x5ACu) + 252u), rd(owner + 0x380u), extra_of(owner) ? " <- EXTRA" : "");
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_STATUS") && i < 8 && (cnt[i] % 300) == 2) {
        const uint32_t st = rd(owner + 0x52Cu);
        std::fprintf(stderr, "[TAGTEAM] status %08X owner %08X%s hp10B0=%u:", st, owner, extra_of(owner) ? " EXTRA" : "", rd(owner + 0x10B0u));
        if (ok(st, 0x80)) for (uint32_t o = 0; o < 0x60; o += 4) std::fprintf(stderr, " %08X", rd(st + o));
        std::fprintf(stderr, "\n");
    }
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_LOCKDUMP") && i < 8 && cnt[i] == 302 && g_bs) {
        for (int side = 0; side < 2; side++) {
            const uint32_t act = rd(g_bs + 56u + 4u * side), L = rd(act + 0x5ACu);
            if (!ok(L, 0x200)) continue;
            std::fprintf(stderr, "[TAGTEAM] lock block %08X of %08X (slot %d): owner=%08X target=%08X mode=%u prev=%u idx=%u idx2=%u\n", L, act, side, rd(L + 248), rd(L + 252), rd(L + 264), rd(L + 268), rd(L + 272), rd(L + 276));
            for (uint32_t o = 240; o < 400; o += 4) { const uint32_t v = rd(L + o); if (v) std::fprintf(stderr, "[TAGTEAM]   +%u = %08X\n", o, v); }
            for (uint32_t k = 0; k < 6; k++) {
                const uint32_t h = rd(L + 300 + 4 * k);
                if (!ok(h, 16)) continue;
                const uint32_t vt = rd(h), e = vt + 112;
                std::fprintf(stderr, "[TAGTEAM]   handler[%u] %08X vtable %08X getter(+112) off=%d fn=%08X  +8: fn=%08X +16: fn=%08X\n", k, h, vt, static_cast<int16_t>(rd(e) & 0xFFFF), rd(e + 4), rd(vt + 8 + 4), rd(vt + 16 + 4));
            }
        }
    }
    if (g_bs && extra_of(owner)) {   // a slot fighter is down: the battle is over (victory scene), the extras stop fighting
        for (int k = 0; k < 2; k++) { const uint32_t a = rd(g_bs + 56u + 4u * k); if (ok(a, 0x12D0) && !alive(a)) return; }
    }
    if (dissidia_cpu_ai_off() && ok(brain, 0x60) && static_cast<int32_t>(rd(brain + 0x54u)) == 0) return;   // Enemy AI off (debug menu): CPU brains do not think; this hook replaces framerate.cpp's
    g_orig_tick(rdram, ctx);
}

// FUN_08B33914(actor): the round-end handler's "take one stock, ask for a revive" for the loser (experiment ONEROUND=1: skipped).
void stock_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram; g_ctx = ctx;
    const uint32_t a = static_cast<uint32_t>(ctx->r[4]);
    if (g_oneround && g_tagin && g_bs && ok(a, 0x12D0) && (a == rd(g_bs + 56u) || a == rd(g_bs + 60u)) && alive(a)) {   // the retail finish effect of a KO that the tag swap took away: nothing is lost
        if (g_log) std::fprintf(stderr, "[TAGTEAM] t=%.1fs retail KO finish for %08X skipped (the slot holds a living fighter)\n", dissidia_battle_age() / 60.0f, a);
        return;
    }
    g_orig_stock(rdram, ctx);
}

// ---- KO rules ----
// Retail: every fighter has a stock count (actor halfword +0x10CE, 1 in a quick battle). A KO takes one (FUN_08B33914 also sets the
// revive word +0x10D0); at 0 the battle ends, otherwise the round restarts about 2 s later (the KO presentation runs first).
// A tag team gives each slot fighter one more stock per living team mate; when a slot fighter's stock drops, the team mate is
// exchanged into the slot (content swap: the KO'd character moves to the team mate's address and stays down there) while the
// retail revive / round restart go on with the slot. The slot keeps its own stock and revive word. A team loses when the slot
// fighter is KO'd with no stock left, i.e. when both of its fighters are down.
void ko_watch() {
    if (g_oneround || !g_tagin || !g_bs || g_nextra == 0) return;
    static uint32_t prev[2] = {99, 99}, prev_rev[2] = {0, 0}, snap[2] = {99, 99}, prev_bs = 0, pend_T[2] = {0, 0};
    static float pend_since[2] = {0, 0}, last_age = 0;
    if (prev_bs != g_bs) { prev[0] = prev[1] = 99; prev_bs = g_bs; pend_T[0] = pend_T[1] = 0; }
    if (const char* e = std::getenv("PSPRECOMP_TAGTEAM_STOCK")) { static bool set = false; if (!set && g_bs) { set = true; for (int k = 0; k < 2; k++) { const uint32_t S0 = rd(g_bs + 56u + 4u * k); if (ok(S0, 0x12D0)) g_ram[(S0 + 0x10CEu) & 0x07FFFFFFu] = static_cast<uint8_t>(std::atoi(e)); } } }   // debug: give the slot fighters this many stocks
    for (uint32_t k = 0; k < 2; k++) {
        const uint32_t S = rd(g_bs + 56u + 4u * k);
        if (!ok(S, 0x12D0)) continue;
        const uint32_t stock = g_ram[(S + 0x10CEu) & 0x07FFFFFFu];
        const uint32_t rev = rd(S + 0x10D0u);
        if (prev[k] != 99 && stock + 1 == prev[k] && !pend_T[k]) {   // a KO: the rules chain took one stock (the round restart then drains more: those drops are bigger or come while the tag-in is pending)
            snap[0] = prev[0]; snap[1] = prev[1];   // both slots' stocks before it (the restart drains the other slot's too)
            Extra* te = nullptr;
            for (int i = 0; i < g_nextra; i++) if (g_extra[i].started && g_extra[i].actor != S && side_of(g_extra[i].actor) == side_of(S) && alive(g_extra[i].actor)) te = &g_extra[i];
            if (g_log) std::fprintf(stderr, "[TAGTEAM] t=%.1fs KO of slot %u fighter %08X: stock %u -> %u, team mate %08X%s\n", dissidia_battle_age() / 60.0f, k, S, prev[k], stock, te ? te->actor : 0u, te ? "" : " (none alive: the retail rules decide)");
            // Not at once: the retail KO sequence (finish cut, HUD KO state, revive) wants the slot fighter itself to go through it. The team
            // mate is swapped in when the round restarts (battle time starts over), see below.
            static const int slots = std::getenv("PSPRECOMP_TAGTEAM_TAGIN_SLOTS") ? std::atoi(std::getenv("PSPRECOMP_TAGTEAM_TAGIN_SLOTS")) : 3;
            if (te && stock >= 1 && (slots & (1 << k)) && !std::getenv("PSPRECOMP_TAGTEAM_KO_NOSWAP")) { pend_T[k] = te->actor; pend_since[k] = dissidia_battle_age(); }
        }
        prev[k] = g_ram[(S + 0x10CEu) & 0x07FFFFFFu];
        prev_rev[k] = rev;
        if (pend_T[k]) {
            const float age = dissidia_battle_age();
            const bool restarted = age + 30.0f < last_age;                 // battle time went back: the round restarted
            const bool timeout = age - pend_since[k] > 60.0f * 8.0f;       // fallback: 8 s after the KO
            static bool rs[2] = {false, false}; static int idle[2] = {0, 0};
            if (restarted) rs[k] = true;
            const int cur = static_cast<int16_t>(rd(S + 0x180u) & 0xFFFFu);
            idle[k] = (rs[k] && (cur == 1 || cur == 2)) ? idle[k] + 1 : 0;   // after the restart: wait until the revived fighter is controllable (its HUD is back)
            static const int need = std::getenv("PSPRECOMP_TAGTEAM_KO_IDLE") ? std::atoi(std::getenv("PSPRECOMP_TAGTEAM_KO_IDLE")) : 45;
            if (idle[k] >= need || timeout) { rs[k] = false; idle[k] = 0;
                const uint32_t T = pend_T[k]; pend_T[k] = 0;
                if (alive(T) && ok(T, 0x12D0)) {
                    const uint32_t flag = rd(S + 0x10D0u), keep = g_ram[(S + 0x10CEu) & 0x07FFFFFFu];
                    static const int kofix = std::getenv("PSPRECOMP_TAGTEAM_KOFIX") ? std::atoi(std::getenv("PSPRECOMP_TAGTEAM_KOFIX")) : 0;
                    if (content_swap(S, T)) {
                        if (!(kofix & 1)) { wr(S + 0x10D0u, flag); g_ram[(S + 0x10CEu) & 0x07FFFFFFu] = static_cast<uint8_t>(keep); }   // the slot keeps its stock and revive word
                        { const uint32_t j = 1u - k, J = rd(g_bs + 56u + 4u * j); if (snap[j] != 99 && ok(J, 0x12D0) && !pend_T[j] && g_ram[(J + 0x10CEu) & 0x07FFFFFFu] < snap[j]) g_ram[(J + 0x10CEu) & 0x07FFFFFFu] = static_cast<uint8_t>(snap[j]); }   // the other slot did not fall: its stock is as before
                        if (!(kofix & 2)) { wr(T + 0x10D0u, 0); g_ram[(T + 0x10CEu) & 0x07FFFFFFu] = 0;  }                               // the KO'd character stays down
                        if (!(kofix & 4)) g_down.push_back(T);                // (live HP 0: the AI and the lock cycle skip it)
                        if (g_log) for (uint32_t A : {S, T}) { const uint32_t vt = rd(A + 1348u), e = vt + 320u, f = rd(e + 4u); const int16_t off = static_cast<int16_t>(rdh(e)); std::fprintf(stderr, "[TAGTEAM] actor %08X vt %08X +320: off %d fn %08X -> %u\n", A, vt, off, f, call(f, A + static_cast<uint32_t>(off))); }
                        if (g_log) { const uint32_t g = rd(0x089D4958u), pl = ok(g, 0x40) ? rd(g + 20u) : 0u; std::fprintf(stderr, "[TAGTEAM] player global %08X +20 = %08X (S %08X T %08X) getter(S) = %08X\n", g, pl, S, T, call(0x08842164u, 0)); }
                        if (g_log) std::fprintf(stderr, "[TAGTEAM] t=%.1fs tag in (%s): %08X now holds the team mate, the KO'd character is at %08X (hp %d / %d)\n", age / 60.0f, timeout ? "timeout" : "revived and idle", S, T, static_cast<int32_t>(rd(rd(S + 0x52Cu) + 0xCu)), static_cast<int32_t>(rd(rd(T + 0x52Cu) + 0xCu)));
                    }
                }
            }
        }
    }
    last_age = dissidia_battle_age();
}


// ---- one-round KO flow ----
// Retail has no "a fighter is down, the fight goes on": the KO of a slot fighter ends the round (finish effect, stock, restart; skipping
// the stock step only loops the round end). So the KO is taken away from the slot: the moment a slot fighter's live HP reaches 0
// (its KO motion has just started, the retail round end is seconds away) the content of the slot and of a living team mate are
// exchanged. The slot (HUD, camera, pad) now holds the living fighter; the KO'd one plays out its fall at the other address and stays down
// (AI gate, g_down). A down fighter comes back after kCallInDelay with the next member of the side's party (the reserve strips) when one is
// left: it is healed, placed beside its team mate and started like an extra (call_in).
constexpr float kCallInDelaySec = 20.0f;   // seconds between a KO and the next member coming in (15-30 s; PSPRECOMP_TAGTEAM_CALLIN_DELAY overrides)
float call_in_delay() { static const float d = (std::getenv("PSPRECOMP_TAGTEAM_CALLIN_DELAY") ? static_cast<float>(std::atof(std::getenv("PSPRECOMP_TAGTEAM_CALLIN_DELAY"))) : kCallInDelaySec) * 60.0f; return d; }
struct DownRec { uint32_t actor; float since; uint32_t side; };
std::vector<DownRec> g_downrec;

int team_of(uint32_t side) { return g_bs && side == side_of(rd(g_bs + 56u + 4u * (rd(g_bs + 84u) & 1u))) ? 0 : 1; }
int reserve_left(int team) { const int n = static_cast<int>(g_reserve.size() > static_cast<size_t>(team) ? g_reserve[team].names.size() : 0u); return n - g_called[team]; }

// Bring a down extra back as the side's next member (charId reserved for the day other characters can be loaded: today a clone of the existing fighters).
void call_in(DownRec& d, int charId) {
    const uint32_t A = d.actor;
    uint32_t anchor = 0;
    for (int k = 0; k < 2; k++) { const uint32_t a = rd(g_bs + 56u + 4u * k); if (ok(a, 0x12D0) && side_of(a) == d.side) anchor = a; }
    const uint32_t st = rd(A + 0x52Cu);
    if (ok(st, 0x40)) { wr(st + 0xCu, rd(A + 0x10B0u)); const uint32_t b = rd(A + 0x10ACu) & 0xFFFFu; wr(st + 0x14u, (b << 16) | b); }
    for (size_t i = 0; i < g_down.size(); i++) if (g_down[i] == A) { g_down.erase(g_down.begin() + static_cast<long>(i)); break; }
    wr(A + 0x380u, rd(A + 0x380u) & ~2u);
    const uint32_t sp0 = static_cast<uint32_t>(g_ctx->r[29]);
    g_ctx->r[29] = static_cast<int32_t>(sp0 - 0x100u);
    const uint32_t vec = sp0 - 0xC0u;
    const uint32_t ref = ok(anchor, 0x12D0) ? anchor : A;
    wrf(vec, rdf(ref + 0x80u) + 3.0f); wrf(vec + 4, rdf(ref + 0x84u)); wrf(vec + 8, rdf(ref + 0x88u)); wrf(vec + 12, 1.0f);
    call(PLACE, A, vec, 1);
    g_ctx->f[12] = 0.0f; g_ctx->f[13] = 0.0f;
    call(START_ACTION, A + 304u, 1, 0);
    g_ctx->r[29] = static_cast<int32_t>(sp0);
    g_called[team_of(d.side)]++;
    if (g_log) std::fprintf(stderr, "[TAGTEAM] t=%.1fs CALLIN actor %08X (side %u, char %d) is back with full HP\n", dissidia_battle_age() / 60.0f, A, d.side, charId);
}

void ko_oneround() {
    if (!g_oneround || !g_tagin || !g_bs || g_nextra == 0) return;
    if (std::getenv("PSPRECOMP_TAGTEAM_BSDIFF")) {   // debug: the setup object's words that changed during the last frames (at the KO and after)
        static uint32_t snap[0x1400 / 4]; static int shown = 0; static bool have = false; static float lastage = -1;
        const float a = dissidia_battle_age();
        const uint32_t S0 = rd(g_bs + 56u); const bool dead0 = ok(S0, 0x12D0) && !alive(S0);
        static int since_dead = -1; if (dead0 && since_dead < 0) since_dead = 0; if (since_dead >= 0) since_dead++;
        if (have && since_dead >= 0 && since_dead < 1500 && shown < 80) for (uint32_t o = 0; o < 0x1400; o += 4) { const uint32_t v = rd(g_bs + o); if (v != snap[o / 4] && o != 0x48 && o != 0x60 && shown < 80) { std::fprintf(stderr, "[BSDIFF] f+%d t=%.2f bs+%04X: %08X -> %08X\n", since_dead, a / 60.0f, o, snap[o / 4], v); shown++; } }
        for (uint32_t o = 0; o < 0x1400; o += 4) snap[o / 4] = rd(g_bs + o); have = true; (void)lastage;
    }
    static uint32_t prev_bs = 0; static bool handled[2] = {false, false}; static uint8_t lastStock[2] = {1, 1};
    if (prev_bs != g_bs) { prev_bs = g_bs; handled[0] = handled[1] = false; g_downrec.clear(); g_called[0] = g_called[1] = 0; g_down.clear(); }
    const float age = dissidia_battle_age();
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_KOLOG")) { static int n = 0; if ((n++ % 120) == 0) { std::fprintf(stderr, "[KOLOG] t=%.1f down %zu rec %zu called %d/%d reserve %d/%d bs %08X:", age / 60.0f, g_down.size(), g_downrec.size(), g_called[0], g_called[1], reserve_left(0), reserve_left(1), g_bs); for (int i = 0; i < g_nextra; i++) std::fprintf(stderr, " extra%d %08X side %u started %d hp %d down %d", i, g_extra[i].actor, g_extra[i].side, g_extra[i].started, ok(rd(g_extra[i].actor + 0x52Cu), 0x40) ? static_cast<int>(rd(rd(g_extra[i].actor + 0x52Cu) + 0xCu)) : -1, is_down(g_extra[i].actor)); std::fprintf(stderr, "\n"); } }
    for (uint32_t k = 0; k < 2; k++) {
        const uint32_t S = rd(g_bs + 56u + 4u * k);
        if (!ok(S, 0x12D0)) continue;
        if (alive(S)) { handled[k] = false; lastStock[k] = g_ram[(S + 0x10CEu) & 0x07FFFFFFu]; continue; }
        if (handled[k]) continue;
        handled[k] = true;
        Extra* te = nullptr;
        for (int i = 0; i < g_nextra; i++) if (g_extra[i].started && g_extra[i].actor != S && side_of(g_extra[i].actor) == side_of(S) && alive(g_extra[i].actor)) te = &g_extra[i];
        if (g_log) std::fprintf(stderr, "[TAGTEAM] t=%.1fs KO of slot %u fighter %08X, team mate %08X, reserve %d\n", age / 60.0f, k, S, te ? te->actor : 0u, reserve_left(team_of(side_of(S))));
        if (!te) continue;                                    // nobody to take over: the retail rules end the round
        const uint32_t T = te->actor;
        const uint32_t keepRev = 0; const uint8_t keepStock = lastStock[k];   // the stock / revive words belong to the slot, like the player flag: back to what they were before the retail KO chain took a stock and asked for a revive
        if (!content_swap(S, T)) continue;
        wr(S + 0x10D0u, keepRev); g_ram[(S + 0x10CEu) & 0x07FFFFFFu] = keepStock; wr(T + 0x10D0u, 0); g_ram[(T + 0x10CEu) & 0x07FFFFFFu] = 0;
        g_down.push_back(T);                                   // the KO'd content now at T: it plays out its fall and stays down
        g_downrec.push_back({T, age, side_of(T)});
        if (g_log) std::fprintf(stderr, "[TAGTEAM] t=%.1fs slot %u now holds the team mate; the KO'd character is down at %08X\n", age / 60.0f, k, T);
    }
    for (size_t i = 0; i < g_downrec.size();) {
        DownRec& d = g_downrec[i];
        if (age - d.since >= call_in_delay() && reserve_left(team_of(d.side)) > 0) { call_in(d, 0); g_downrec.erase(g_downrec.begin() + static_cast<long>(i)); }
        else i++;
    }
}

// debug (PSPRECOMP_TAGTEAM_HUDWATCH=1): the retail HUD getters for the human's side every 10 frames, printed when one changes
void hud_watch() {
    static const uint32_t fns[] = {0x089E95E0u, 0x089E9628u, 0x089E968Cu, 0x089E96DCu, 0x089E973Cu, 0x089E978Cu, 0x089E97FCu, 0x089E9858u, 0x089E98F0u, 0x089E9964u, 0x089E99C0u, 0x089E9A4Cu, 0x089E9AA8u, 0x089E9B18u, 0x089E9C24u, 0x089E9C64u, 0x089E9D84u, 0x089E9E10u, 0x089E9E68u, 0x089E9EF4u};
    static uint32_t last[2][32] = {}; static int n = 0;
    static const int every = std::atoi(std::getenv("PSPRECOMP_TAGTEAM_HUDWATCH")); if ((n++ % every) != 0 || !g_bs) return;
    for (uint32_t side = 0; side < 2; side++) {
        char line[512]; int len = 0; bool changed = false;
        for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
            FuncPtr fp = fn(fns[i]); if (!fp) continue;
            const recomp_context sv = *g_ctx; g_ctx->r[4] = static_cast<int32_t>(side); g_ctx->r[5] = 0;
            fp(g_ram, g_ctx);
            const uint32_t v = static_cast<uint32_t>(g_ctx->r[2]);
            *g_ctx = sv;
            if (v != last[side][i]) { changed = true; len += std::snprintf(line + len, sizeof(line) - len, " %X=%X", fns[i] & 0xFFFu, v); last[side][i] = v; }
        }
        const uint32_t S = rd(g_bs + 56u + 4u * side), st = ok(S, 0x12D0) ? rd(S + 0x52Cu) : 0;
        static uint32_t lastS[2][5] = {};
        const uint32_t sv4[5] = {ok(st, 0x40) ? rd(st + 0xCu) : 0u, ok(st, 0x40) ? (rdh(st + 0x14u)) : 0u, ok(S, 0x12D0) ? (static_cast<uint32_t>(g_ram[(S + 0x10CEu) & 0x07FFFFFFu]) | (rd(S + 0x10D0u) << 8)) : 0u, ok(S, 0x12D0) ? (rd(S + 0x180u) & 0xFFFFu) : 0u, rd(g_bs + 72u)};
        for (int q = 0; q < 5; q++) if (sv4[q] != lastS[side][q]) { changed = true; lastS[side][q] = sv4[q]; }
        if (changed) std::fprintf(stderr, "[HUDWATCH] f=%d t=%.1fs side %u: hp=%d brv=%u stock|rev<<8=%X script=%u bs72=%u |%s\n", n, dissidia_battle_age() / 60.0f, side, static_cast<int>(sv4[0]), sv4[1], sv4[2], sv4[3], sv4[4], line);
    }
}

// ---- debugging: which retail end-of-battle state handler runs (PSPRECOMP_TAGTEAM_ENDLOG=1) ----
// The EBOOT functions that write bs+72 (the battle phase): FUN_0888A434 .. FUN_0888ACA0 set the result phases (7 = finished).
constexpr uint32_t kEndFns[16] = {0x0888A434u, 0x0888A578u, 0x0888A5F4u, 0x0888A688u, 0x0888A70Cu, 0x0888A778u, 0x0888A7E0u, 0x0888A884u,
                                  0x0888A8ECu, 0x0888A958u, 0x0888AA78u, 0x0888AAE4u, 0x0888AB6Cu, 0x0888AC14u, 0x0888ACA0u, 0x08889138u};
FuncPtr g_end_orig[16] = {};
template <int N> void end_log_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram;
    std::fprintf(stderr, "[TAGTEAM] t=%.1fs END handler %08X called from %08X (bs+72 = %u)\n", dissidia_battle_age() / 60.0f, kEndFns[N], static_cast<uint32_t>(ctx->r[31]), g_bs ? rd(g_bs + 72u) : 99u);
    g_end_orig[N](rdram, ctx);
}
template <int... I> void install_end_log(std::integer_sequence<int, I...>) {
    ((g_end_orig[I] = psp_dispatch_probe_lookup(kEndFns[I]), g_end_orig[I] ? psp_dispatch_register(kEndFns[I], end_log_hook<I>) : void()), ...);
}

// debug (PSPRECOMP_TAGTEAM_HITLOG=1): FUN_08AEF284(event, attacker, victim) builds a hit event
FuncPtr g_orig_hit = nullptr;
const char* role_of(uint32_t a) {
    if (!g_bs) return "?";
    const uint32_t hs = rd(g_bs + 84u);
    if (a == rd(g_bs + 56u + 4u * hs)) return "slotH";
    if (a == rd(g_bs + 56u + 4u * (1u - hs))) return "slotC";
    for (int i = 0; i < g_nextra; i++) if (g_extra[i].actor == a) return side_of(a) == side_of(rd(g_bs + 56u + 4u * hs)) ? "extraP" : "extraE";
    return "other";
}
FuncPtr g_orig_msg = nullptr;
void msg_hook(uint8_t* rdram, recomp_context* ctx) {   // FUN_08AFD170(obj, msg, data): message dispatch; log the messages with 16 < id < 39 (table of 23 handlers)
    g_ram = rdram; g_ctx = ctx;
    const uint32_t a0 = static_cast<uint32_t>(ctx->r[4]), id = static_cast<uint32_t>(ctx->r[5]), d = static_cast<uint32_t>(ctx->r[6]);
    static int n = 0;
    if (id - 16u < 23u && n++ < 3000) std::fprintf(stderr, "[MSG] t=%.2fs id %u obj %08X(%s) data %08X handler %08X\n", dissidia_battle_age() / 60.0f, id, a0, role_of(a0), d, rd(0x08AFDDB8u + 4u * (id - 16u)));
    g_orig_msg(rdram, ctx);
}
FuncPtr g_orig_world = nullptr;
void world_hook(uint8_t* rdram, recomp_context* ctx) {   // FUN_089DEDB0(world): per-frame hit processing of the collision groups (+36, +120, +204)
    g_ram = rdram; g_ctx = ctx;
    static int n = 0;
    const uint32_t w = static_cast<uint32_t>(ctx->r[4]);
    if (n++ == 400) {
        std::fprintf(stderr, "[WORLD] a0 %08X mask(0x08AE7CA4) %08X\n", w, rd(0x08AE7CA4u));
        for (uint32_t g : {36u, 120u, 204u}) { std::fprintf(stderr, "[WORLD] group +%u:", g); for (uint32_t o = 0; o < 84; o += 4) std::fprintf(stderr, " %08X", rd(w + g + o)); std::fprintf(stderr, "\n"); }
        for (uint32_t g : {36u, 120u, 204u}) for (uint32_t k = 0; k < 5; k++) { const uint32_t rec = rd(w + g + 44u + 4u * k); if (!rec) continue; std::fprintf(stderr, "[WORLD] group +%u rec %u @%08X:", g, k, rec); for (uint32_t o = 0; o < 68; o += 4) std::fprintf(stderr, " %08X", rd(rec + o)); std::fprintf(stderr, "\n"); }
        uint32_t f[6]; const int nf = all_fighters(f); for (int i = 0; i < nf; i++) std::fprintf(stderr, "[WORLD] fighter %d %s %08X\n", i, role_of(f[i]), f[i]);
    }
    g_orig_world(rdram, ctx);
}
void hit_hook(uint8_t* rdram, recomp_context* ctx) { g_orig_hit(rdram, ctx); }

// The combat modules (battle controller FUN_08886C88, once per frame) damage the attacker's foe (+0x300); while the lock is on the other enemy the
// human's foe is put back to it just before they run (the retail target refresh can rewrite it from the lock block, whose target is 0 then).
FuncPtr g_orig_ctrl = nullptr;
void ctrl_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram; g_ctx = ctx;
    if (g_bs && g_nextra && g_cyc == 1 && g_cyc_target && alive(g_cyc_target)) {
        const uint32_t hum = rd(g_bs + 56u + 4u * rd(g_bs + 84u));
        if (ok(hum, 0x12D0)) { if (g_log && rd(hum + 0x300u) != g_cyc_target) { static int n = 0; if (n++ < 20) std::fprintf(stderr, "[TAGTEAM] controller: human foe %08X -> %08X\n", rd(hum + 0x300u), g_cyc_target); } wr(hum + 0x300u, g_cyc_target); wr(hum + 0x2FCu, g_cyc_target); }
    }
    g_orig_ctrl(rdram, ctx);
}

// FUN_08AFD424(m): the per-slot combat module (m+0 = the slot fighter) walks [slot actor, its assist] and runs FUN_08AFD170(m, is_main, &actor) for each (hit
// results -> bravery / HP damage). The extras get the same call as if they were that slot fighter's second assist, without touching the real assist pointer.
FuncPtr g_orig_afd424 = nullptr;
void afd424_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram; g_ctx = ctx;
    const uint32_t m = static_cast<uint32_t>(ctx->r[4]);
    g_orig_afd424(rdram, ctx);
    if (!g_nextra || !ok(m, 0x100)) return;
    const uint32_t slot = rd(m);
    static const int mode = std::getenv("PSPRECOMP_TAGTEAM_AFDMODE") ? std::atoi(std::getenv("PSPRECOMP_TAGTEAM_AFDMODE")) : 0;   // 0 off (no effect found), 1 module clone, 2 second assist call
    static uint32_t clone_of[8] = {}, clone_src[8] = {};
    if (mode == 0) return;
    for (int i = 0; i < g_nextra; i++) {
        Extra& e = g_extra[i];
        if (!e.started || e.slot_actor != slot || !alive(e.actor) || !ok(e.actor, 0x12D0)) continue;
        if (mode == 2) {   // the extra as the module's second assist: FUN_08AFD170(m, 0, &actor)
            const uint32_t scratch = static_cast<uint32_t>(ctx->r[29]) - 128u;
            wr(scratch, e.actor);
            call(0x08AFD170u, m, 0u, scratch);
        } else {           // the extra as a slot fighter of its own: a copy of the slot's module whose actor is the extra (no assist)
            if (clone_src[i] != m || !clone_of[i]) {
                clone_of[i] = call(ALLOC, 0x400u); clone_src[i] = m;
                for (uint32_t o = 0; o < 0x400u; o += 4) wr(clone_of[i] + o, ok(m + o, 4) ? rd(m + o) : 0u);
                if (g_log) std::fprintf(stderr, "[TAGTEAM] combat module of extra %08X cloned from %08X at %08X\n", e.actor, m, clone_of[i]);
            }
            wr(clone_of[i], e.actor);
            const uint32_t keep = rd(e.actor + 0x1108u); wr(e.actor + 0x1108u, 0);
            { const recomp_context sv = *ctx; ctx->r[4] = static_cast<int32_t>(clone_of[i]); g_orig_afd424(rdram, ctx); *ctx = sv; }
            wr(e.actor + 0x1108u, keep);
        }
    }
}

// ---- debugging: the game's resource lookups (PSPRECOMP_TAGTEAM_LOADLOG=1): FUN_088470B8(fileId) returns the file record (name at +36) ----
FuncPtr g_orig_filerec = nullptr;
void filerec_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = static_cast<uint32_t>(ctx->r[4]), ra = static_cast<uint32_t>(ctx->r[31]);
    g_ram = rdram;
    g_orig_filerec(rdram, ctx);
    static int n = 0;
    const uint32_t rec = static_cast<uint32_t>(ctx->r[2]);
    if (n++ < 6000 && ok(rec, 0x60)) {
        char nm[28] = {}; for (int i = 0; i < 24; i++) nm[i] = static_cast<char>(g_ram[(rec + 36 + i) & 0x07FFFFFFu]);
        std::fprintf(stderr, "[LOAD] age=%.1f id=%u rec=%08X %s ra=%08X\n", dissidia_battle_age() / 60.0f, id, rec, nm, ra);
    }
}

FuncPtr g_orig_ld1 = nullptr, g_orig_ld2 = nullptr;
void ld1_hook(uint8_t* rdram, recomp_context* ctx) {   // FUN_08843014(out, fileId, heap, size, flag)
    g_ram = rdram; const uint32_t a1 = static_cast<uint32_t>(ctx->r[5]), a2 = static_cast<uint32_t>(ctx->r[6]), a3 = static_cast<uint32_t>(ctx->r[7]), ra = static_cast<uint32_t>(ctx->r[31]);
    std::fprintf(stderr, "[LOAD1] age=%.1f FUN_08843014 id=%u heap=%08X size=%X ra=%08X\n", dissidia_battle_age() / 60.0f, a1, a2, a3, ra);
    psp_dump_func_ring("LOAD1-RING");
    g_orig_ld1(rdram, ctx);
}
void ld2_hook(uint8_t* rdram, recomp_context* ctx) {   // FUN_08845230(out, fileId, buf, size, flag)
    g_ram = rdram; const uint32_t a1 = static_cast<uint32_t>(ctx->r[5]), a2 = static_cast<uint32_t>(ctx->r[6]), a3 = static_cast<uint32_t>(ctx->r[7]), ra = static_cast<uint32_t>(ctx->r[31]);
    std::fprintf(stderr, "[LOAD2] age=%.1f FUN_08845230 id=%u buf=%08X size=%X ra=%08X\n", dissidia_battle_age() / 60.0f, a1, a2, a3, ra);
    g_orig_ld2(rdram, ctx);
}

// ---- preload: characters of the extras are added to the battle's loading list ----
// FUN_08B45310 (battle overlay, the loading screen) takes the battle's record (FUN_089132A4) whose halfwords at +72 are the characters
// (6 slots, negative = end) and registers + loads their model files (FUN_089121F4 -> FUN_0884417C -> FUN_08843014). The character table the
// lookup uses is freed after the loading screen, so a character has to be in this list to be loaded: the extras' characters are appended.
FuncPtr g_orig_batrec = nullptr;
int g_preload[4] = {0, 0, 0, 0};   // charIds to append (set from the settings before the loading screen)
int g_npreload = 0;
void batrec_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t ra = static_cast<uint32_t>(ctx->r[31]);
    g_ram = rdram;
    g_orig_batrec(rdram, ctx);
    const uint32_t rec = static_cast<uint32_t>(ctx->r[2]);
    if (g_log) std::fprintf(stderr, "[TAGTEAM] FUN_089132A4(%08X, %08X) -> %08X\n", static_cast<uint32_t>(ctx->r[4]), static_cast<uint32_t>(ctx->r[5]), rec);
    const bool from_loader = true;   // (the recompiled calls leave $ra unset: every record with a character list is patched)
    (void)ra;
    if (!ok(rec, 0x60) || !from_loader) return;
    int16_t list[6]; for (int i = 0; i < 6; i++) list[i] = static_cast<int16_t>(rdh(rec + 72u + 2u * i));
    if (g_log) std::fprintf(stderr, "[TAGTEAM] battle record %08X characters: %d %d %d %d %d %d (ra %08X)\n", rec, list[0], list[1], list[2], list[3], list[4], list[5], ra);
    if (!g_enabled) return;
    for (int k = 0; k < g_npreload; k++) {
        const int id = g_preload[k]; bool have = false; int free_slot = -1;
        for (int i = 0; i < 6; i++) { if (list[i] == id) have = true; if (free_slot < 0 && list[i] < 0) free_slot = i; }
        if (have || free_slot < 0) { if (!have && g_log) std::fprintf(stderr, "[TAGTEAM] no free slot in the loading list for charId %d\n", id); continue; }
        wrh(rec + 72u + 2u * free_slot, static_cast<uint16_t>(id)); list[free_slot] = static_cast<int16_t>(id);
        if (g_log) std::fprintf(stderr, "[TAGTEAM] charId %d appended to the loading list (slot %d)\n", id, free_slot);
    }
}

// generic call logger for the loading chain (PSPRECOMP_TAGTEAM_LOADLOG=1)
constexpr uint32_t kChain[8] = {0x08844C14u, 0x0884417Cu, 0x088444D4u, 0x0888204Cu, 0x08844DE0u, 0x08844F50u, 0x08844B58u, 0x08842308u};
FuncPtr g_chain_orig[8] = {};
template <int N> void chain_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram;
    const uint32_t a0 = static_cast<uint32_t>(ctx->r[4]), a1 = static_cast<uint32_t>(ctx->r[5]);
    const bool big = (N == 2 && dissidia_battle_age() > 100.f) || N < 2;
    if (big) std::fprintf(stderr, "[CHAIN] age=%.1f FUN_%08X(%08X, %08X, %08X, %08X)\n", dissidia_battle_age() / 60.0f, kChain[N], a0, a1, static_cast<uint32_t>(ctx->r[6]), static_cast<uint32_t>(ctx->r[7]));
    g_chain_orig[N](rdram, ctx);
    if (N == 0 || N == 2) std::fprintf(stderr, "[CHAIN]   -> %08X\n", static_cast<uint32_t>(ctx->r[2]));   // (N == 2: FUN_088444D4 lookup result)
}
template <int... I> void install_chain(std::integer_sequence<int, I...>) {
    ((g_chain_orig[I] = psp_dispatch_probe_lookup(kChain[I]), g_chain_orig[I] ? psp_dispatch_register(kChain[I], chain_hook<I>) : void()), ...);
}

}  // namespace

// Called by the scene tick (hooks_main.cpp) before every frame's update: between frames no fighter is mid-update, the safe point for the swap.
uint32_t dissidia_camera_manager();
void dissidia_tagteam_frame(uint8_t* rdram, recomp_context* ctx) {
    if (g_nextra && g_bs && !std::getenv("PSPRECOMP_TAGTEAM_NOCAMFOE")) {   // the camera manager keeps the human's foe at mgr+0x8B4: follow the locked enemy
        g_ram = rdram; g_ctx = ctx;
        const uint32_t mgr = dissidia_camera_manager(), hum = rd(g_bs + 56u + 4u * rd(g_bs + 84u));
        static bool armed = false;
        if (mgr && !armed) if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCH_CAM")) { armed = true; psp_mem_watch_set(mgr + static_cast<uint32_t>(std::strtoul(w, nullptr, 16))); std::fprintf(stderr, "[TAGTEAM] watching camera mgr %08X + %s\n", mgr, w); }
        if (mgr && ok(hum, 0x12D0)) {
            const uint32_t foe = rd(hum + 0x300u), cur = rd(mgr + 0x8B4u);
            uint32_t f[6]; const int n = all_fighters(f);
            bool is_enemy = false; for (int i = 0; i < n; i++) if (f[i] == cur && side_of(f[i]) != side_of(hum)) is_enemy = true;
            if (is_enemy && foe && foe != cur && alive(foe)) { if (g_log) std::fprintf(stderr, "[TAGTEAM] camera foe %08X -> %08X\n", cur, foe); wr(mgr + 0x8B4u, foe); }
        }
    }
    if (g_nextra && g_bs && std::getenv("PSPRECOMP_TAGTEAM_RETICLE")) {   // experiment: the overlay globals that hold the human's foe (reticle?)
        g_ram = rdram; g_ctx = ctx;
        const uint32_t hum = rd(g_bs + 56u + 4u * rd(g_bs + 84u)), foe = ok(hum, 0x12D0) ? rd(hum + 0x300u) : 0;
        static const uint32_t g[2] = {0x08B876A4u, 0x08C00998u};
        for (uint32_t a : g) { const uint32_t cur = rd(a); uint32_t f[6]; const int n = all_fighters(f); bool en = false; for (int i = 0; i < n; i++) if (f[i] == cur && side_of(f[i]) != side_of(hum)) en = true; if (en && foe && foe != cur) wr(a, foe); }
    }
    g_ram = rdram; g_ctx = ctx;
    if (g_nextra && g_bs && dissidia_battle_age() / 60.0f > 3.0f) fix_assist();   // (not at once: the retail setup of the assist actors has to run first)
    ko_watch();
    ko_oneround();
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_HUDWATCH")) hud_watch();
    if (!g_swap_req) return;
    g_swap_req = false;
    swap_roles();
}

// Pad filter (framerate.cpp calls it with the buttons of every poll): a rising edge of the swap button becomes a swap request and is
// taken out of the input.
uint32_t dissidia_tagteam_pad(uint32_t raw) {
    static uint32_t prev = 0;
    const uint32_t edge = raw & ~prev & g_swap_mask;
    prev = raw;
    if (g_nextra == 0 || g_swap_mask == 0) return raw;
    if (edge) g_swap_req = true;
    return raw & ~g_swap_mask;
}

void dissidia_tagteam_install(uint8_t* rdram) {
    const char* e = std::getenv("PSPRECOMP_TAGTEAM");
    g_enabled = e && e[0] && e[0] != '0';
    g_log = std::getenv("PSPRECOMP_TAGTEAM_LOG") != nullptr;
    if (const char* m = std::getenv("PSPRECOMP_TAGTEAM_EXTRAS")) g_mask = std::atoi(m);
    g_one_v_two = std::getenv("PSPRECOMP_TAGTEAM_1V2") && std::getenv("PSPRECOMP_TAGTEAM_1V2")[0] == '1';
    g_two_v_one = std::getenv("PSPRECOMP_TAGTEAM_2V1") && std::getenv("PSPRECOMP_TAGTEAM_2V1")[0] == '1';
    { const char* l = std::getenv("PSPRECOMP_TAGTEAM_LATE"); g_late = !(l && l[0] == '0'); g_late_wait = !(l && l[0] == '1'); }   // default: spawn after the intro; LATE=1 right when the battle starts, LATE=0 during the intro setup
    g_oneround = !(std::getenv("PSPRECOMP_TAGTEAM_ONEROUND") && std::getenv("PSPRECOMP_TAGTEAM_ONEROUND")[0] == '0');
    g_tagin = !(std::getenv("PSPRECOMP_TAGTEAM_TAGIN") && std::getenv("PSPRECOMP_TAGTEAM_TAGIN")[0] == '0');
    if (std::getenv("PSPRECOMP_TAGTEAM_LOADLOG") && (g_orig_filerec = psp_dispatch_probe_lookup(0x088470B8u))) { psp_dispatch_register(0x088470B8u, filerec_hook); psp_mem_watch_set(0x0880FFF0u); install_chain(std::make_integer_sequence<int, 8>{}); if ((g_orig_ld1 = psp_dispatch_probe_lookup(0x08843014u))) psp_dispatch_register(0x08843014u, ld1_hook); if ((g_orig_ld2 = psp_dispatch_probe_lookup(0x08845230u))) psp_dispatch_register(0x08845230u, ld2_hook); }
    if ((g_orig_batrec = psp_dispatch_probe_lookup(0x089132A4u))) psp_dispatch_register(0x089132A4u, batrec_hook);
    g_hud_on = !(std::getenv("PSPRECOMP_TAGTEAM_HUD") && std::getenv("PSPRECOMP_TAGTEAM_HUD")[0] == '0');
    if (const char* b = std::getenv("PSPRECOMP_TAGTEAM_SWAP_BUTTON")) g_swap_mask = static_cast<uint32_t>(std::strtoul(b, nullptr, 16));
    if (const char* c = std::getenv("PSPRECOMP_TAGTEAM_PARTNER_CHAR")) g_partner_char = std::atoi(c);
    if (const char* c = std::getenv("PSPRECOMP_TAGTEAM_ENEMY2_CHAR")) g_enemy2_char = std::atoi(c);
    if (g_partner_char || g_enemy2_char) dissidia_model_pool_request_extra(0x600000u);   // room for up to two more characters' models
    if (g_partner_char) g_preload[g_npreload++] = g_partner_char;
    if (g_enemy2_char) g_preload[g_npreload++] = g_enemy2_char;
    if (const char* m = std::getenv("PSPRECOMP_TAGTEAM_STOP")) g_stop = std::atoi(m);
    g_ram = rdram;
    g_orig_alloc = psp_dispatch_probe_lookup(ALLOC_CORE);
    if (g_orig_alloc) psp_dispatch_register(ALLOC_CORE, alloc_hook);
    g_orig_setup = psp_dispatch_probe_lookup(BATTLE_SETUP);
    if (!g_orig_setup) { std::fprintf(stderr, "[TAGTEAM] setup function not found\n"); return; }
    g_orig_flow = psp_dispatch_probe_lookup(FLOW_STEP);
    if (!g_orig_flow) { std::fprintf(stderr, "[TAGTEAM] flow step not found\n"); return; }
    psp_dispatch_register(BATTLE_SETUP, setup_hook);
    psp_dispatch_register(FLOW_STEP, flow_hook);
    if (std::getenv("PSPRECOMP_TAGTEAM_HITLOG")) { g_orig_hit = fn(0x08AEF284u); if (g_orig_hit) psp_dispatch_register(0x08AEF284u, hit_hook); g_orig_world = fn(0x089DEDB0u); if (g_orig_world) psp_dispatch_register(0x089DEDB0u, world_hook); }
    if (!std::getenv("PSPRECOMP_TAGTEAM_NOAFD")) { g_orig_afd424 = fn(0x08AFD424u); if (g_orig_afd424) psp_dispatch_register(0x08AFD424u, afd424_hook); }
    if (!std::getenv("PSPRECOMP_TAGTEAM_NOCTRL")) { g_orig_ctrl = fn(0x08886C88u); if (g_orig_ctrl) psp_dispatch_register(0x08886C88u, ctrl_hook); }
    if (std::getenv("PSPRECOMP_TAGTEAM_ENDLOG")) install_end_log(std::make_integer_sequence<int, 16>{});
    g_orig_rel1 = psp_dispatch_probe_lookup(0x0886CAA0u); g_orig_rel2 = psp_dispatch_probe_lookup(0x0886A140u);
    if (g_orig_rel1 && g_orig_rel2) { psp_dispatch_register(0x0886CAA0u, rel1_hook); psp_dispatch_register(0x0886A140u, rel2_hook); }
    psp_settings_add_debug_toggle({"Tag Team (2v2)", "Tag team battle (experimental)\nOn: the next battle adds a partner for you and a second fighter to the CPU's team (2 v 2), L cycles the lock between the two enemies.\nAffects the next battle that starts.",
                                   +[]() { return g_enabled; }, +[](bool on) { g_enabled = on; }});
    psp_settings_add_debug_toggle({"Tag Team 2v1", "Tag team 2 v 1 (experimental)\nWith Tag Team on: only your partner is added, so you and the partner fight one enemy. Off: 2 v 2.\nAffects the next battle that starts.",
                                   +[]() { return g_two_v_one; }, +[](bool on) { g_two_v_one = on; }});
    psp_settings_add_debug_toggle({"Tag Team 1v2", "Tag team 1 v 2 (experimental)\nWith Tag Team on: you alone fight the CPU's two fighters (no partner for you). Off: 2 v 2.\nAffects the next battle that starts.",
                                   +[]() { return g_one_v_two; }, +[](bool on) { g_one_v_two = on; }});
    std::fprintf(stderr, "[TAGTEAM] installed (%s)\n", g_enabled ? "on" : "off, Settings > Debug > Tag Team (2v2)");
}
