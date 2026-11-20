// games/dissidia/runtime/hooks_models.cpp — raise the battle model staging budget (PSPRECOMP_MODEL_BUDGET).
//
// The game loads every model file (p_*.gmo/.objx, the _a/_w animation files, ...) into a temporary
// "staging" block, builds the real model data from it and frees it. Findings (docs/roster.md, "Model size budget"):
//   * overlay_battle FUN_08B45310 registers the model slots (ids 0x54E, 0x134, 0x408, 0x548) with
//     FUN_0884417C(mgr, id, heap, size, 0), size = 0x264000 (lui 0x26 / addiu 0x4000);
//     FUN_08843014 allocates `size` bytes from that heap (top-down, align 0x40), reads the file there
//     (sceIoRead with the index size) and frees it again.
//   * the heap is the "FILE" heap: a 0x275800 block (staging + 0x11800) taken from the main BSS heap
//     (12.9 MB, nearly full) by FUN_0888204C and turned into a heap by FUN_0885C40C.
// A file larger than the 0x264000 staging block (2,506,752 B; the largest retail character GMO is
// 2,280,840 B) makes the allocation fail (null buffer) or, for a slightly smaller file, overrun the heap.
//
// Fix without touching the recompiled code, when the budget is raised: the 0x275800 allocation
// (FUN_0888204C) is served from the host heap with size 0x275800 + (budget - 0x264000) + slack,
// FUN_0885C40C gets the real size, FUN_0884417C swaps the 0x264000 slot size for the budget, and the
// block free (FUN_0888208C) keeps the host block for the next battle. Nothing changes while the budget
// is the retail value.
//
// The same budget raise also needs a bigger MODEL POOL (found 17 Oct, "both fighters invisible" regression):
//   * FUN_0888204C(0x780000) = 7,864,320 B is the heap that holds the built data of every fighter in the
//     battle (heap object 0x08C15F80, `used` at heap+8). It is sized for retail models; a replacement model
//     whose GMO is much bigger than retail (Noctis: 3.7 MB vs 2.1 MB) fills it as soon as the opponent has a
//     large model too (Vaan; Lightning fits), FUN_0885C6CC then returns null for the remaining allocations
//     (`[MODELS] heap ... FAILED`), the fighters' model objects are never built and nothing is drawn (no
//     skinned PRIMs, no GE_XFER into RAM); the Hellfire summon then shows Ifrit's model in place of both.
//   * It is NOT an overlap with the effect allocator (sceKernelAllocMemoryBlock "EFFECT_ALLOCATOR",
//     0x0A010000-0x0A3ED000): a canary-filled 4 MB gap between the effect block and the model heap stayed
//     untouched and the bug was unchanged; retail never writes behind the block either.
// With a raised budget the pool request is served from the host heap with kPoolExtra more bytes.
//
// Host heap layout (game.toml [runtime]): region 1 0x0A000000-0x0B800000 (24 MiB, stops where LIBSUPPREACC is
// loaded), region 2 0x0B810000-<host_heap2_end> (above that PRX, below the guest thread stacks). The pool and the
// model file heap are reserved TOGETHER, once, when the pool is requested at game start (the budget is known from
// the prescan of <mods>/obj/), after a fit check against the heap's real free space (including the effect allocator,
// which is requested later): a budget that cannot fit is
// lowered 1 MiB at a time (never below retail), so a too-big mod degrades to a warning instead of a failed
// allocation. All blocks come from the same bump allocator, so the effect allocator, the pool and the file heap
// cannot overlap; psp_kmem_log_layout() prints them ([MEM] lines) and flags any overlap.
#include "psp_game_module.h"
#include "hle/psp_hle.h"
#include "recomp.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#if !defined(__ANDROID__)
#include <execinfo.h>
#endif
#include <cstring>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

std::string dissidia_mods_dir();
std::vector<std::string> dissidia_mod_roots();   // hooks_io.cpp: installed mod folders that are on
uint32_t dissidia_model_budget_request(uint32_t bytes);

void dissidia_tagteam_presweep(uint8_t* rdram, recomp_context* ctx);   // tagteam.cpp
void dissidia_tagteam_after_free(uint8_t* rdram, recomp_context* ctx, uint32_t heap, uint32_t ptr);   // tagteam.cpp
uint32_t dissidia_tagteam_soundheap_pending();   // tagteam.cpp: bytes the sound heap will still take from the host heap (0 once taken / retail)
namespace {

constexpr uint32_t kRetailStaging = 0x264000u;
constexpr uint32_t kRetailFile = 0x275800u;  // the "FILE" heap holding the staging block: 0x264000 + 0x11800
constexpr uint32_t kMaxStaging = 0x1200000u;   // ceiling: 18 MiB, i.e. GMOs up to 16 MiB plus headroom. What really fits is decided by the fit check against the host heap (see the header comment); the heap is 24 MiB + region 2
constexpr uint32_t kRetailMaxGmo = 2280840u;   // largest retail character GMO (p_thr200_1p.gmo)
constexpr uint32_t kLaterReserve = 4050944u + 0x100000u;   // still to come from the host heap after the pool request: the EFFECT_ALLOCATOR block (4,050,944 B, seen in the boot log) + 1 MiB margin
constexpr uint32_t kPoolValidatedStaging = 0x440000u;   // the pool extra below is sized for models up to ~4.25 MB (Noctis 4.21 MB, 22 Oct); larger files add their growth 1:1
constexpr uint32_t kWmSlack = 0x20000u;       // headroom for the heap bookkeeping and the objects that share the heap
constexpr uint32_t kRetailPool = 0x780000u;   // battle model pool (FUN_0888204C request), see the header comment
constexpr uint32_t kFxRetail = 0x3DD000u;     // EFFECT_ALLOCATOR block (see "Effect pool" below)
constexpr uint32_t kFxMinExtension = 0x300000u, kFxMargin = 0x40000u;
constexpr uint32_t kObjdataTagSize = 0x41000u + 0x80000u;   // the tag team OBJDATA heap (region 2, allocated after the effect pool: hh_bridge leaves room for it)   // the effect pool extension the tag team pool leaves room for at least, and the margin for the small blocks that follow
void reserve_tex_overflow();
// extra bytes for the pool when the budget is raised (PSPRECOMP_MODEL_POOL_EXTRA). 0x80000 ran out on 19 Oct with the
// 3.65 MB Noctis in a battle (pool full ~110 KB short: alloc(74208)/(29736)/(16448) FAILED, Noctis invisible), so 1.5 MiB;
// raised to 2.5 MiB on 22 Oct for the 4.21 MB Noctis (EX Burst + glyph), so no env override is needed by default.
uint32_t g_pool_extra = 0x280000u;
bool g_pool_extra_forced = false;             // PSPRECOMP_MODEL_POOL_EXTRA set: no scaling
bool g_reserved = false;                      // pool + file heap reserved together (budget locked)
uint32_t g_staging = kRetailStaging;
uint32_t g_pool_block = 0, g_pool_size = 0;
uint32_t g_tt_pool_extra = 0;   // tag team: extra pool bytes (dissidia_model_pool_request_extra)
uint32_t g_od_block = 0, g_od_size = 0; bool g_od_done = false;   // tag team: the OBJDATA heap (0x41000, the characters' D_ data) served from the host heap with more room
FuncPtr g_orig_small_alloc = nullptr;
bool g_tt_pool = false;         // g_pool_block is the tag team pool (not tied to the budget reservation)
bool g_pool_in_use = false;
uint32_t g_wm_block = 0, g_wm_size = 0;      // host-heap block, allocated once and reused by every scene
bool g_wm_in_use = false;
FuncPtr g_orig_alloc = nullptr, g_orig_register = nullptr, g_orig_free = nullptr, g_orig_heap_init = nullptr;

// FUN_0888204C(size): block from the main BSS heap. The model file heap (always exactly 0x270800) comes
// from the host heap instead when the budget is raised.
const bool g_trace = std::getenv("PSPRECOMP_MODELS_TRACE") != nullptr;

// The model pool (FUN_0888204C(0x780000)) is allocated at game start, long before the package index is opened
// (hooks_io.cpp ensure_built raises the staging budget from the override sizes). Scan <mods>/obj/ for the
// biggest replacement file here so the pool can be sized for it.
uint32_t pool_extra_for(uint32_t staging) {
    if (g_pool_extra_forced) return g_pool_extra;
    uint32_t e = g_pool_extra;
    if (staging > kPoolValidatedStaging) e += ((staging - kPoolValidatedStaging) + 0xFFFu) & ~0xFFFu;   // all of the file growth (the 1/4 estimate under-sized it)
    // Both fighters can be the biggest modded character (a mirror match). One extra only covered one of them:
    // on 22 Oct Noctis vs Noctis ran the pool out by ~0.75 MB (his 512² body texture and the rest FAILED), so the
    // second Noctis drew untextured (a white, glowing silhouette). Size for two.
    return e * 2u;
}
uint32_t wm_total_for(uint32_t staging) { return kRetailFile + (staging - kRetailStaging) + kWmSlack; }
uint32_t rd256(uint32_t v) { return (v + 0xFFu) & ~0xFFu; }

// Reserve the pool and the model file heap together for the current budget, lowering the budget until both fit.
void reserve_blocks() {
    uint32_t s = g_staging;
    for (;; s -= 0x100000u) {
        const uint32_t sizes[3] = { rd256(kRetailPool + pool_extra_for(s)), rd256(wm_total_for(s)), kLaterReserve };
        if (psp_kmem_plan_fits(sizes, 3)) break;
        if (s <= kRetailStaging + 0x100000u) { s = kRetailStaging; break; }
    }
    if (s != g_staging)
        std::fprintf(stderr, "[MODELS] host heap too small for a %u B staging budget (%u B free): lowered to %u B\n",
                     g_staging, psp_kmem_free_total(), s);
    g_staging = s;
    if (g_staging == kRetailStaging) { std::fprintf(stderr, "[MODELS] staying at the retail budget\n"); return; }
    g_pool_size = kRetailPool + pool_extra_for(g_staging);
    g_pool_block = psp_kmem_bump_alloc_named(rd256(g_pool_size), "MODEL_POOL");
    g_wm_size = wm_total_for(g_staging);
    g_wm_block = psp_kmem_bump_alloc_named(rd256(g_wm_size), "MODEL_FILE_HEAP");
    if (g_pool_block == 0 || g_wm_block == 0) {   // cannot happen after a successful plan; stay safe anyway
        std::fprintf(stderr, "[MODELS] reservation failed after a successful fit check: staying at the retail budget\n");
        g_pool_block = g_wm_block = g_pool_size = g_wm_size = 0;
        g_staging = kRetailStaging;
        return;
    }
    g_reserved = true;
    std::fprintf(stderr, "[MODELS] model pool %u B at 0x%08X (retail %u B); model file heap %u B at 0x%08X (retail 0x%X), staging budget %u B\n",
                 g_pool_size, g_pool_block, kRetailPool, g_wm_size, g_wm_block, kRetailFile, g_staging);
    psp_kmem_log_layout("model budget");
}

void prescan_mods_for_budget() {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::string> dirs{dissidia_mods_dir()};
    for (const std::string& r : dissidia_mod_roots()) dirs.push_back(r);
    uint64_t biggest = 0;
    for (const std::string& dir : dirs) {   // <mods> and the installed mod folders that are on
        if (dir.empty() || !fs::is_directory(dir, ec)) { ec.clear(); continue; }
        // Only the obj/ folder holds model overrides, so only it is walked. (The whole mods tree also holds the HD texture pack, thousands of
        // files; walking it with fs::relative() canonicalised every path and took about 5 s of the start-up on Android's shared storage.)
        for (fs::directory_iterator top(dir, fs::directory_options::skip_permission_denied, ec), tend; !ec && top != tend; top.increment(ec)) {
            std::string name = top->path().filename().generic_string();
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name != "obj" || !top->is_directory(ec)) continue;
            std::error_code ec2;
            for (fs::recursive_directory_iterator it(top->path(), fs::directory_options::skip_permission_denied, ec2), end; !ec2 && it != end; it.increment(ec2)) {
                if (!it->is_regular_file(ec2)) continue;
                biggest = std::max<uint64_t>(biggest, it->file_size(ec2));
            }
        }
        ec.clear();
    }
    if (biggest > g_staging) {
        const uint32_t b = dissidia_model_budget_request(static_cast<uint32_t>(std::min<uint64_t>(biggest, 0xFFFFFFFFull)));
        std::fprintf(stderr, "[MODELS] mods prescan: largest obj/ override %llu B, staging budget %u B\n", (unsigned long long)biggest, b);
    }
}

void alloc_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_trace && static_cast<uint32_t>(ctx->r[4]) >= 0x100000u) std::fprintf(stderr, "[MODELS] alloc(%u) in_use=%d\n", (uint32_t)ctx->r[4], (int)g_wm_in_use);
    if (static_cast<uint32_t>(ctx->r[4]) == kRetailFile && g_staging != kRetailStaging && !g_wm_in_use) {
        uint32_t total = wm_total_for(g_staging);
        if (g_wm_block == 0) {   // budget raised after the pool was requested (no prescan hit): reserve the file heap now
            g_wm_block = psp_kmem_bump_alloc_named(rd256(total), "MODEL_FILE_HEAP");
            g_wm_size = total;
            if (g_wm_block == 0) {
                std::fprintf(stderr, "[MODELS] cannot reserve %u B for the model file heap: staying at the retail budget\n", total);
                g_staging = kRetailStaging;
            } else {
                std::fprintf(stderr, "[MODELS] model file heap %u B at 0x%08X (retail 0x%X), staging budget %u B\n",
                             total, g_wm_block, kRetailFile, g_staging);
            }
        }
        if (g_wm_block != 0) { g_wm_in_use = true; if (g_trace) std::fprintf(stderr, "[MODELS]   -> host block\n"); ctx->r[2] = static_cast<int32_t>(g_wm_block); return; }
    }
    if (static_cast<uint32_t>(ctx->r[4]) == kRetailPool && !g_pool_in_use && g_tt_pool_extra && g_pool_block == 0 && !g_reserved) {
        // Region 1 must keep, behind the pool: the sound heap (if still to come), the effect allocator with the extension it gets
        // (kFxMinExtension at least; the extension must be contiguous with the retail block, so both have to come from region 1) and
        // a margin for the small blocks allocated later. The extra is lowered 1 MiB at a time until that holds.
        const uint32_t later = dissidia_tagteam_soundheap_pending() + kFxRetail + kFxMinExtension + kFxMargin;
        uint32_t extra = g_tt_pool_extra;
        const uint32_t free1 = psp_kmem_free_region(1);
        while (extra > 0x100000u && rd256(kRetailPool + extra) + later > free1) extra -= 0x100000u;
        if (extra != g_tt_pool_extra) std::fprintf(stderr, "[MODELS] tag team model pool extra lowered from %u B to %u B (region 1 free %u B, %u B to keep)\n", g_tt_pool_extra, extra, free1, later);
        const uint32_t size = rd256(kRetailPool + extra), sizes[1] = {size};
        if (psp_kmem_plan_fits(sizes, 1)) {
            g_pool_size = kRetailPool + extra;
            g_pool_block = psp_kmem_bump_alloc_named(size, "MODEL_POOL_TAGTEAM");
            g_tt_pool = g_pool_block != 0;
            if (g_pool_block) std::fprintf(stderr, "[MODELS] tag team model pool %u B at 0x%08X (retail %u B)\n", g_pool_size, g_pool_block, kRetailPool);
            reserve_tex_overflow();   // region 2, now: lazily it found region 2 eaten (by the stray effect extension) and never came
        } else {
            std::fprintf(stderr, "[MODELS] no room for a %u B tag team model pool: retail size\n", size);
        }
    }
    if (static_cast<uint32_t>(ctx->r[4]) == kRetailPool && !g_pool_in_use) {
        if (g_pool_block == 0 && !g_reserved) {
            prescan_mods_for_budget();
            if (g_staging != kRetailStaging) reserve_blocks();
        }
        if (g_pool_block != 0) { g_pool_in_use = true; ctx->r[2] = static_cast<int32_t>(g_pool_block); return; }
    }
    const uint32_t req = static_cast<uint32_t>(ctx->r[4]);
    g_orig_alloc(rdram, ctx);
    if (g_trace && req >= 0x100000u) std::fprintf(stderr, "[MODELS]   -> 0x%08X\n", (uint32_t)ctx->r[2]);
}

// FUN_088820D8(size): block from the main heap. The OBJDATA heap (the file manager's, exactly 0x41000, allocated once at start) holds the
// D_ data (scripts / parameters) of every character in the battle; the extra characters of a tag team battle do not fit.
void small_alloc_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_tt_pool_extra && !g_od_done && static_cast<uint32_t>(ctx->r[4]) == 0x41000u) {
        g_od_done = true;
        const uint32_t size = kObjdataTagSize;
        g_od_block = psp_kmem_bump_alloc_named_r2(rd256(size), "OBJDATA_TAGTEAM");   // region 2 (the game heap code works there: sp5c run, 17 Nov), region 1 is for the pool and the effect pool
        if (g_od_block) { g_od_size = size; std::fprintf(stderr, "[MODELS] tag team OBJDATA heap %u B at 0x%08X (retail %u B)\n", size, g_od_block, 0x41000u); ctx->r[2] = static_cast<int32_t>(g_od_block); return; }
    }
    g_orig_small_alloc(rdram, ctx);
}

// FUN_0885C40C(block, size, name, align): heap init over a block; give the host block its real size.
// Tag team: the 16 KB manager heap whose object sits at 0x09971160 (a FUN_0885C40C(block, 0x4000, ...) heap made at boot from the main heap; one of the
// three persistent manager heaps load_character allocates from, see docs/tag-team.md) fills up at the fifth mid-battle load (`heap 0x09971160
// alloc(498 / 37 / 12 / 54 / 36) FAILED`, 18 Nov): the block is swapped for a host-heap one with room for ten more characters (the 16 KB from the main
// heap stay unused). Matched by the heap object's address, which is the same in every run (deterministic boot); a different address = no change.
constexpr uint32_t kSmallMgrHeap = 0x09971160u, kSmallMgrSize = 0x4000u, kSmallMgrExtra = 0x8000u;
uint32_t g_smallmgr_block = 0;
void hc_heap_made(const uint8_t* rdram, const recomp_context* ctx);   // below
void heap_init_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_tt_pool_extra && !g_smallmgr_block && static_cast<uint32_t>(ctx->r[5]) == kSmallMgrSize) {
        const uint32_t a = static_cast<uint32_t>(ctx->r[7]) ? static_cast<uint32_t>(ctx->r[7]) : 1u, obj = (static_cast<uint32_t>(ctx->r[4]) + a - 1u) & ~(a - 1u);
        if (obj == kSmallMgrHeap && (g_smallmgr_block = psp_kmem_bump_alloc_named_r2(rd256(kSmallMgrSize + kSmallMgrExtra), "SMALLMGR_TAGTEAM"))) {
            std::fprintf(stderr, "[MODELS] tag team: the 16 KB manager heap at 0x%08X is made over a %u B host block at 0x%08X\n", obj, kSmallMgrSize + kSmallMgrExtra, g_smallmgr_block);
            ctx->r[4] = static_cast<int32_t>(g_smallmgr_block); ctx->r[5] = static_cast<int32_t>(kSmallMgrSize + kSmallMgrExtra);
        }
    }
    hc_heap_made(rdram, ctx);   // PSPRECOMP_HEAPCHECK: the heap's name for the check's reports
    if (g_od_block != 0 && static_cast<uint32_t>(ctx->r[4]) == g_od_block && static_cast<uint32_t>(ctx->r[5]) == 0x41000u) ctx->r[5] = static_cast<int32_t>(g_od_size);
    if (g_wm_block != 0 && static_cast<uint32_t>(ctx->r[4]) == g_wm_block && static_cast<uint32_t>(ctx->r[5]) == kRetailFile)
        ctx->r[5] = static_cast<int32_t>(g_wm_size);
    else if (g_pool_block != 0 && static_cast<uint32_t>(ctx->r[4]) == g_pool_block && static_cast<uint32_t>(ctx->r[5]) == kRetailPool)
        ctx->r[5] = static_cast<int32_t>(g_pool_size);
    g_orig_heap_init(rdram, ctx);
}

// FUN_0884417C(mgr, id, heap, size, 0): model slot registration; 0x264000 is the staging size.
// A patched game (an xdelta mod: Emendatus asks FUN_0888204C for a 0x3F5800 FILE heap, 0x3E4000 + 0x11800, for Aerith's
// 3.6 MB p_sev120.objx) can make the heap bigger than retail while the slot is still registered with 0x264000; the file is
// then read 0x264000 below the heap end and runs 1.1 MB past it. The slot follows the heap: heap size - 0x11800.
void register_hook(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<uint32_t>(ctx->r[7]) == kRetailStaging && g_staging != kRetailStaging)
        ctx->r[7] = static_cast<int32_t>(g_staging);
    const uint32_t heap = static_cast<uint32_t>(ctx->r[6]);
    if (heap >= 0x08800000u && heap < 0x0C000000u) {
        const uint32_t end = *reinterpret_cast<const uint32_t*>(rdram + ((heap + 4u) & 0x07FFFFFFu));
        const uint32_t slot = static_cast<uint32_t>(ctx->r[7]);
        if (g_trace) std::fprintf(stderr, "[MODELS] register slot id 0x%X heap 0x%08X end 0x%08X size %u\n", (uint32_t)ctx->r[5], heap, end, slot);
        if (slot >= kRetailStaging && end > heap && end - heap <= 0x02000000u && end - heap > slot + (kRetailFile - kRetailStaging)) {
            const uint32_t fit = end - heap - (kRetailFile - kRetailStaging);
            std::fprintf(stderr, "[MODELS] slot 0x%X: heap 0x%08X holds %u B, staging slot raised from %u B\n", (uint32_t)ctx->r[5], heap, fit, slot);
            ctx->r[7] = static_cast<int32_t>(fit);
        }
    }
    g_orig_register(rdram, ctx);
}

// FUN_0888208C(block): free to the main heap.
void free_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_trace && static_cast<uint32_t>(ctx->r[4]) >= 0x08C00000u) std::fprintf(stderr, "[MODELS] free(0x%08X)\n", (uint32_t)ctx->r[4]);
    if (g_od_block != 0 && static_cast<uint32_t>(ctx->r[4]) == g_od_block) { ctx->r[2] = 0; return; }   // (kept for the next battle)
    if (g_wm_block != 0 && static_cast<uint32_t>(ctx->r[4]) == g_wm_block) { g_wm_in_use = false; ctx->r[2] = 0; return; }
    if (g_pool_block != 0 && static_cast<uint32_t>(ctx->r[4]) == g_pool_block) { g_pool_in_use = false; ctx->r[2] = 0; return; }
    g_orig_free(rdram, ctx);
}

// FUN_0885C6CC(heap, size, align, ...): the game's generic heap allocator. Diagnostic: log every request that
// comes back null (the caller then reads / builds into guest address 0: an invisible model, not a crash), with
// the heap's end and used bytes (heap+4 / heap+8) so an exhausted pool is obvious.
FuncPtr g_orig_heap_alloc = nullptr;
// Tag team: the character texture heap lives in VRAM (heap object 0x04110000, 0xF0000 bytes = 960 KB, "GRAPHICS"-side memory). Retail sizes it for two
// fighters; the extra characters' textures do not fit and come out missing. Allocations that fail there are served from a second heap in
// the host heap (texture addresses in RAM are fine for the GE), and frees of those blocks are routed back to it.
// 2.5 MB (3 MB never fit: region 1 was full and region 2 had 2.6 MB left, so the bump allocation failed silently and every extra character's
// effect texture sheet (84-137 KB each) was lost - see the `heap 0x04110000 alloc(...) FAILED (heap end 0, used 0)` lines: the heap object is in
// VRAM, outside the diagnostic's RAM window, hence the zeros). Taken from region 2 when the pool is made, together with the staging heap and OBJDATA.
// 19 Nov: 2.5 MB -> 768 KB. Peak use seen: 383 KB (desktop vaan3, 10 members), 292 KB (host snd1); the 1.8 MB given back go to the effect pool and the
// sound heap (hh_bridge). PSPRECOMP_TEXHEAP_OVERFLOW=<bytes> (0x280000 = the old size).
constexpr uint32_t kVramHeap = 0x04110000u, kVramHeapName = 0x089C93F4u;
const uint32_t kOverflowSize = [] { const char* e = std::getenv("PSPRECOMP_TEXHEAP_OVERFLOW"); return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) & ~0xFFu : 0xC0000u; }();
uint32_t g_ov_block = 0, g_ov_heap = 0;
FuncPtr g_orig_heap_free = nullptr;
bool in_overflow(uint32_t p) { return g_ov_heap && p >= g_ov_heap && p < g_ov_heap + kOverflowSize; }
void reserve_tex_overflow() {
    if (g_ov_block || !g_tt_pool_extra) return;
    g_ov_block = psp_kmem_bump_alloc_named_r2(rd256(kOverflowSize), "TEXHEAP_TAGTEAM");
    if (!g_ov_block) std::fprintf(stderr, "[MODELS] no room for the %u B tag team texture overflow heap\n", kOverflowSize);
}

uint32_t fileheap_watch();
}
uint32_t g_alloc_in_heap = 0, g_alloc_in_size = 0;   // debug (tagteam.cpp's hang dump): the heap allocation in progress
// ---- the game's generic heap (FUN_0885C40C init / FUN_0885C6CC alloc / FUN_0885C538 free) ----
// Heap object: +0 base, +4 end (also the chain's terminator), +8 used bytes, +12 default alignment, +16 hint (the block the next bottom-up
// allocation goes after), +20 the sentinel block (first in the chain), +36 vtable 089C8E28. Blocks: +0 next, +4 prev, +8 size (header
// included), +12 alignment; the chain holds the ALLOCATED blocks in address order, the gaps between them are the free space.
//
// Retail bug in FUN_0885C6CC (the cause of the 19 Nov desktop freeze): a top-down request (t1 argument & 0xFF != 0, "take the top of the
// heap") whose top region is not free and whose hint gap is too small falls through to the first-fit scan (0885C844), which leaves t2 = the
// block it FOUND instead of the new block's address; the top-down store path (0885C8C4: sw v1,0(t2)) then writes the new header over that
// live block: prev = next = itself, size = the request. The caller gets the live block's data area (and copies its file over it), and the
// next walk of the chain past that block spins forever (FUN_0885C6CC's walk from the hint to the end: the game thread at 100 %).
// Retail only asks for top-down blocks at moments the top is free (the battle HUD setup's portrait read, FUN_08B21588); the tag team's
// portrait_for asks mid-battle (a new character on the big panel). Fix: such a request, when the game's own two top-down placements
// (the top, the hint gap) would both fail, is made bottom-up (the scan is correct there). PSPRECOMP_HEAP_TOPDOWN_RETAIL=1 = retail.
const char* g_tt_step = "";   // tagteam.cpp: the tag team operation in progress (the heap check's op ring)
uint64_t g_hc_frame = 0;      // frames seen by dissidia_heapcheck_frame
namespace {
inline uint32_t hw(const uint8_t* r, uint32_t a) { return *reinterpret_cast<const uint32_t*>(r + (a & 0x07FFFFFCu)); }
inline bool hc_ram(uint32_t a) { return a >= 0x08800000u && a < 0x0BF00000u; }
// would FUN_0885C6CC(heap, size, align, .., topdown) corrupt the chain? (an exact replay of its top / hint placement decisions)
bool topdown_would_break(const uint8_t* r, uint32_t h, uint32_t size, int32_t align_arg) {
    if (!hc_ram(h)) return false;
    const uint32_t al = align_arg > 0 ? static_cast<uint32_t>(align_arg) : hw(r, h + 12u), t3 = al - 1u;
    const uint32_t a1 = ((t3 + size) & ~t3) + ((t3 + 16u) & ~t3);
    const uint32_t end = hw(r, h + 4u), base = hw(r, h + 0u), cand = end - a1;
    uint32_t hint = hw(r, h + 16u);
    bool top = true;
    uint32_t p = h + 20u;
    for (int n = 0; p != end && n < 200000; n++) {
        const uint32_t be = p + hw(r, p + 8u);
        if (!(p < cand || end < p) || !(be < cand || end < be)) { top = false; break; }
        const uint32_t nx = hw(r, p); if (nx <= p) { top = false; break; }   // (a broken chain: let the checker report it)
        p = nx;
    }
    if (top && (cand < base || !(cand < end))) top = false;
    if (top) return false;
    if (hint) {
        const uint32_t na = (hint + hw(r, hint + 8u) + t3) & ~t3; const int32_t gap = static_cast<int32_t>(hw(r, hint) - na);
        if (gap > 0 && !(static_cast<uint32_t>(gap) < a1) && na < end) return false;
    }
    return true;   // the first-fit scan: broken for top-down requests
}
// ---- PSPRECOMP_HEAPCHECK=1: integrity check of every game heap, each frame and around every alloc / free ----
const bool kHeapCheck = std::getenv("PSPRECOMP_HEAPCHECK") != nullptr;
struct HcOp { uint64_t frame; uint32_t heap, ptr, size, ra, ret; const char* step; char kind; };
constexpr int kHcRing = 1024;
HcOp g_hc_ring[kHcRing]; uint32_t g_hc_n = 0;
struct HcHeap { uint32_t name = 0; int32_t drift = 0; bool drift_set = false, broken = false; uint64_t ops = 0, ops_at_ok = 0; };
std::map<uint32_t, HcHeap> g_hc;
void hc_name(const uint8_t* r, uint32_t a, char* b, size_t n) { size_t i = 0; if (hc_ram(a)) for (; i + 1 < n; i++) { const char c = static_cast<char>(r[(a + i) & 0x07FFFFFFu]); if (c < 32 || c > 126) break; b[i] = c; } b[i] = 0; }
// walks the chain: nullptr = consistent, else what is wrong (and where)
const char* hc_check(const uint8_t* r, uint32_t h, char* why, size_t wn, int* blocks, int32_t* drift) {
    const uint32_t end = hw(r, h + 4u), hint = hw(r, h + 16u), used = hw(r, h + 8u);
    if (!hc_ram(end) || end <= h) { std::snprintf(why, wn, "bad end %08X", end); return why; }
    uint32_t p = h + 20u, prev = 0; bool hint_seen = hint == 0; uint64_t sum = 0; int n = 0;
    for (; n < 200000; n++) {
        const uint32_t nx = hw(r, p), pv = hw(r, p + 4u), sz = hw(r, p + 8u);
        if (n > 0 && pv != prev) { std::snprintf(why, wn, "block #%d %08X+%X prev %08X (want %08X) next %08X", n, p, sz, pv, prev, nx); return why; }
        if (p == hint) hint_seen = true;
        if (n > 0) sum += sz;
        if (nx == end) break;
        if (nx <= p) { std::snprintf(why, wn, "block #%d %08X+%X next %08X goes back (loop) prev %08X", n, p, sz, nx, pv); return why; }
        if (nx > end) { std::snprintf(why, wn, "block #%d %08X+%X next %08X past the end %08X", n, p, sz, nx, end); return why; }
        if (n > 0 && nx < p + sz) { std::snprintf(why, wn, "block #%d %08X+%X overlaps the next %08X", n, p, sz, nx); return why; }
        prev = p; p = nx;
    }
    if (n >= 200000) { std::snprintf(why, wn, "more than 200000 blocks"); return why; }
    if (!hint_seen) { std::snprintf(why, wn, "hint %08X is not a block of the chain (%d blocks)", hint, n + 1); return why; }
    *blocks = n + 1; *drift = static_cast<int32_t>(used - static_cast<uint32_t>(sum));
    return nullptr;
}
void hc_dump_ring(uint32_t h) {
    int shown = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(kHcRing) && shown < 64; i++) {
        const uint32_t k = (g_hc_n - 1u - i); if (k >= g_hc_n) break;   // (fewer than kHcRing ops so far)
        const HcOp& o = g_hc_ring[k % kHcRing]; if (o.heap != h) continue;
        std::fprintf(stderr, "[HEAPCHECK]   -%u f%llu %s %08X size %X -> %08X ra %08X step '%s'\n", i, static_cast<unsigned long long>(o.frame), o.kind == 'A' ? "alloc" : o.kind == 'T' ? "alloc(top)" : o.kind == 'F' ? "fixed top-down alloc" : "free", o.ptr, o.size, o.ret, o.ra, o.step);
        shown++;
    }
}
// returns false (and reports, once per breakage) when the heap's chain is broken
bool hc_verify(const uint8_t* r, uint32_t h, const char* when) {
    if (!hc_ram(h) || hw(r, h + 36u) != 0x089C8E28u) return true;   // not (or no longer) a heap of this kind
    HcHeap& s = g_hc[h];
    char why[200]; int nb = 0; int32_t drift = 0;
    if (!hc_check(r, h, why, sizeof why, &nb, &drift)) {
        if (s.broken) std::fprintf(stderr, "[HEAPCHECK] f%llu heap %08X consistent again (%s)\n", static_cast<unsigned long long>(g_hc_frame), h, when);
        s.broken = false; s.ops_at_ok = s.ops;
        if (!s.drift_set) { s.drift = drift; s.drift_set = true; }
        else if (drift != s.drift) { std::fprintf(stderr, "[HEAPCHECK] f%llu heap %08X: used - sum(block sizes) %d -> %d (%s) step '%s'\n", static_cast<unsigned long long>(g_hc_frame), h, s.drift, drift, when, g_tt_step); s.drift = drift; }
        return true;
    }
    if (!s.broken) {
        char nm[32]; hc_name(r, s.name, nm, sizeof nm);
        std::fprintf(stderr, "[HEAPCHECK] f%llu FIRST BREAK of heap %08X '%s' (end %08X used %X hint %08X) %s: %s; step '%s'; %llu ops since the last good check; its last operations:\n",
                     static_cast<unsigned long long>(g_hc_frame), h, nm, hw(r, h + 4u), hw(r, h + 8u), hw(r, h + 16u), when, why, g_tt_step, static_cast<unsigned long long>(s.ops - s.ops_at_ok));
        hc_dump_ring(h);
    }
    s.broken = true;
    return false;
}
void hc_record(uint32_t h, char kind, uint32_t ptr, uint32_t size, uint32_t ra, uint32_t ret) {
    g_hc_ring[g_hc_n % kHcRing] = HcOp{g_hc_frame, h, ptr, size, ra, ret, g_tt_step, kind}; g_hc_n++;
    g_hc[h].ops++;
}
bool hc_in_chain(const uint8_t* r, uint32_t h, uint32_t blk) {
    const uint32_t end = hw(r, h + 4u); uint32_t p = h + 20u;
    for (int n = 0; p != end && n < 200000; n++) { if (p == blk) return true; const uint32_t nx = hw(r, p); if (nx <= p) return false; p = nx; }
    return false;
}
void hc_heap_made(const uint8_t* rdram, const recomp_context* ctx) {   // FUN_0885C40C(block, size, name, align): the heap object it makes
    if (!kHeapCheck) return;
    const uint32_t a = static_cast<uint32_t>(ctx->r[7]) ? static_cast<uint32_t>(ctx->r[7]) : 1u, obj = (static_cast<uint32_t>(ctx->r[4]) + a - 1u) & ~(a - 1u);
    HcHeap fresh; fresh.name = static_cast<uint32_t>(ctx->r[6]); g_hc[obj] = fresh;
    char nm[32]; hc_name(rdram, fresh.name, nm, sizeof nm);
    std::fprintf(stderr, "[HEAPCHECK] f%llu heap %08X '%s' made: %X bytes, ra %08X\n", static_cast<unsigned long long>(g_hc_frame), obj, nm, static_cast<uint32_t>(ctx->r[5]), static_cast<uint32_t>(ctx->r[31]));
}

// PSPRECOMP_HEAPCHECK_SELFTEST=1: the top-down bug on a private 8 KB heap with the game's own allocator (once, at the first allocation): a hole
// in the middle, a block at the top, the hint gap too small; a top-down request then goes through the scan. Run as retail and with the fix.
void hc_selftest(uint8_t* rdram, const recomp_context* ctx) {
    static bool done = false; if (done || !std::getenv("PSPRECOMP_HEAPCHECK_SELFTEST")) return; done = true;
    const uint32_t blk = psp_kmem_bump_alloc_named_r2(0x2000u, "HEAPCHECK_SELFTEST"); if (!blk) { std::fprintf(stderr, "[HEAPTEST] no memory\n"); return; }
    for (int mode = 0; mode < 2; mode++) {
        recomp_context c = *ctx;
        c.r[4] = static_cast<int32_t>(blk); c.r[5] = 0x2000; c.r[6] = 0; c.r[7] = 0x10; g_orig_heap_init(rdram, &c);
        const uint32_t h = blk;   // (aligned already)
        auto alloc = [&](uint32_t size, bool top) { recomp_context a = *ctx; a.r[4] = static_cast<int32_t>(h); a.r[5] = static_cast<int32_t>(size); a.r[6] = 0x10; a.r[7] = 0; a.r[8] = 1; a.r[9] = top ? 1 : 0;
            if (top && mode == 1 && topdown_would_break(rdram, h, size, 0x10)) a.r[9] = 0;
            g_orig_heap_alloc(rdram, &a); return static_cast<uint32_t>(a.r[2]); };
        auto release = [&](uint32_t p) { recomp_context f = *ctx; f.r[4] = static_cast<int32_t>(h); f.r[5] = static_cast<int32_t>(p); f.r[6] = 0x10; g_orig_heap_free(rdram, &f); };
        const uint32_t a1 = alloc(0x100, false), a2 = alloc(0x100, false), a3 = alloc(0x100, false);
        release(a2);                                     // a 0x110-byte hole between a1 and a3
        const uint32_t t = alloc(0x100, true);           // the top block
        const uint32_t end = hw(rdram, h + 4u), room = t - 0x10u - (a3 - 0x10u + 0x110u);   // free bytes between a3 and the top block
        const uint32_t b = alloc(room - 0x10u - 0x10u - 0x40u, false);   // fills all but 0x40 bytes (the hint gap is too small for the next request)
        const bool would = topdown_would_break(rdram, h, 0x100, 0x10);
        const uint32_t x = alloc(0x100, true);           // top-down again: top taken, hint gap too small -> the scan finds the hole
        char why[200]; int nb = 0; int32_t dr = 0; const char* bad = hc_check(rdram, h, why, sizeof why, &nb, &dr);
        std::fprintf(stderr, "[HEAPTEST] %s: heap %08X end %08X blocks a1 %08X a3 %08X top %08X fill %08X; guard predicts %s; top-down request -> %08X (the hole is at %08X, a1's data at %08X): chain %s%s\n",
                     mode ? "with the fix" : "retail", h, end, a1, a3, t, b, would ? "the scan (broken)" : "a safe placement", x, a2, a1, bad ? "BROKEN: " : "consistent", bad ? why : "");
    }
}
}  // namespace
void dissidia_heapcheck_frame(const uint8_t* rdram) {
    g_hc_frame++;
    if (!kHeapCheck) return;
    for (auto& e : g_hc) hc_verify(rdram, e.first, "frame check");
}
namespace {
void heap_alloc_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t heap = static_cast<uint32_t>(ctx->r[4]), size = static_cast<uint32_t>(ctx->r[5]);
    g_alloc_in_heap = heap; g_alloc_in_size = size;
    if (size >= 0x40000u) dissidia_tagteam_presweep(rdram, ctx);   // tagteam.cpp: the tag battle's leftovers leave the battle heap before the next loading screen's first big block
    hc_selftest(rdram, ctx);   // (PSPRECOMP_HEAPCHECK_SELFTEST=1, once)
    const uint32_t hc_ra = static_cast<uint32_t>(ctx->r[31]);
    char hc_kind = (ctx->r[9] & 0xFF) ? 'T' : 'A';
    if (hc_kind == 'T' && hc_ram(heap) && topdown_would_break(rdram, heap, size, ctx->r[6])) {   // the retail top-down bug (see topdown_would_break)
        static const bool retail = std::getenv("PSPRECOMP_HEAP_TOPDOWN_RETAIL") != nullptr;
        static int n = 0;
        if (n++ < 50) std::fprintf(stderr, "[HEAPFIX] top-down allocation of %X B in heap %08X (ra %08X, step '%s'): its top and hint gap are taken, the game's scan would write the new block over a live one%s\n", size, heap, hc_ra, g_tt_step, retail ? " (PSPRECOMP_HEAP_TOPDOWN_RETAIL: left as retail)" : ": made bottom-up");
        if (!retail) { ctx->r[9] = static_cast<int32_t>(static_cast<uint32_t>(ctx->r[9]) & ~0xFFu); hc_kind = 'F'; }
    }
    if (kHeapCheck) hc_verify(rdram, heap, "before an allocation");
    const recomp_context saved = *ctx;
    if (fileheap_watch() && heap == fileheap_watch() && size >= 0x70000u) {   // debug: the block chain before the loading step's 544 KB file read
        const uint32_t* h = reinterpret_cast<const uint32_t*>(rdram + (heap & 0x07FFFFFFu)); const uint32_t end = h[1];
        std::fprintf(stderr, "[HEAPDUMP] heap %08X end %08X hint %08X chain:", heap, end, h[4]);
        uint32_t p = heap + 20u; for (int n = 0; p != end && n < 3000; n++) { const uint32_t* b = reinterpret_cast<const uint32_t*>(rdram + (p & 0x07FFFFFFu)); std::fprintf(stderr, " %08X+%X", p, b[2]); if (b[0] <= p) { std::fprintf(stderr, " BAD-NEXT %08X", b[0]); break; } p = b[0]; }
        std::fprintf(stderr, "\n");
    }
    g_orig_heap_alloc(rdram, ctx);
    if (kHeapCheck && hc_ram(heap)) { hc_record(heap, hc_kind, 0, size, hc_ra, static_cast<uint32_t>(ctx->r[2])); hc_verify(rdram, heap, "after an allocation"); }
    if (fileheap_watch() && heap == fileheap_watch() && size >= 0x20000u) {
        std::fprintf(stderr, "[FILEHEAP] alloc heap %08X size 0x%X -> %08X stack:", heap, size, static_cast<uint32_t>(ctx->r[2]));
#if !defined(__ANDROID__)
        void* bt[32]; const int nb = backtrace(bt, 32); char** sy = backtrace_symbols(bt, nb); for (int i = 1; i < nb && sy; i++) { const char* f = std::strstr(sy[i], "(+"); if (f) std::fprintf(stderr, " %.11s", f + 1); } std::free(sy);
#endif
        std::fprintf(stderr, "\n");
    }
    if (ctx->r[2] == 0 && size != 0 && g_tt_pool_extra && heap == kVramHeap) {   // the texture heap is full: overflow into RAM
        if (!g_ov_heap) {
            reserve_tex_overflow();
            if (g_ov_block) {
                recomp_context c2 = saved;
                c2.r[4] = static_cast<int32_t>(g_ov_block); c2.r[5] = static_cast<int32_t>(kOverflowSize); c2.r[6] = static_cast<int32_t>(kVramHeapName); c2.r[7] = 0x40;
                g_orig_heap_init(rdram, &c2);
                g_ov_heap = g_ov_block;
                std::fprintf(stderr, "[MODELS] tag team texture overflow heap %u B at 0x%08X (first request: %u B)\n", kOverflowSize, g_ov_heap, size);
            }
        }
        if (g_ov_heap) {
            recomp_context c3 = saved;
            c3.r[4] = static_cast<int32_t>(g_ov_heap);
            g_orig_heap_alloc(rdram, &c3);
            if (c3.r[2] != 0) { *ctx = c3; return; }
        }
    }
    if (ctx->r[2] == 0 && size != 0) {
        static int n = 0;
        if (n++ < 100) {
            uint32_t used = 0, end = 0;
            if (heap >= 0x08000000u && heap < 0x0C000000u) {
                const uint32_t* h = reinterpret_cast<const uint32_t*>(rdram + (heap & 0x07FFFFFFu));
                end = h[1]; used = h[2];
            }
            std::fprintf(stderr, "[MODELS] heap 0x%08X alloc(%u) FAILED (heap end 0x%08X, used 0x%X)\n", heap, size, end, used);
        }
    }
}

// FUN_0885C538(heap, ptr, align, ...): the game's generic heap free.
// debug (PSPRECOMP_FILEHEAP_LOG=<hex heap object>): allocations / frees on that heap, and frees of pointers inside its range on ANY heap
uint32_t fileheap_watch() { static const uint32_t h = std::getenv("PSPRECOMP_FILEHEAP_LOG") ? static_cast<uint32_t>(std::strtoul(std::getenv("PSPRECOMP_FILEHEAP_LOG"), nullptr, 16)) : 0u; return h; }
void heap_free_hook(uint8_t* rdram, recomp_context* ctx) {
    if (const uint32_t w = fileheap_watch()) { const uint32_t hp = static_cast<uint32_t>(ctx->r[4]), p = static_cast<uint32_t>(ctx->r[5]); if (hp == w && (p == 0x08F05700u || p == 0x08F05710u || p == 0x08F05740u)) { static int n = 0; if (n++ < 600) std::fprintf(stderr, "[FILEHEAP] free heap %08X ptr %08X ra %08X\n", hp, p, static_cast<uint32_t>(ctx->r[31])); } }
    if (static_cast<uint32_t>(ctx->r[4]) == kVramHeap && in_overflow(static_cast<uint32_t>(ctx->r[5]))) ctx->r[4] = static_cast<int32_t>(g_ov_heap);
    const uint32_t fheap = static_cast<uint32_t>(ctx->r[4]), fptr = static_cast<uint32_t>(ctx->r[5]);
    if (kHeapCheck && hc_ram(fheap) && hw(rdram, fheap + 36u) == 0x089C8E28u) {
        hc_verify(rdram, fheap, "before a free");
        const uint32_t al = static_cast<int32_t>(ctx->r[6]) > 0 ? static_cast<uint32_t>(ctx->r[6]) : hw(rdram, fheap + 12u), blk = fptr - ((al - 1u + 16u) & ~(al - 1u));
        hc_record(fheap, 'R', fptr, hc_ram(blk) ? hw(rdram, blk + 8u) : 0u, static_cast<uint32_t>(ctx->r[31]), blk);
        if (!hc_in_chain(rdram, fheap, blk)) { static int n = 0; if (n++ < 40) { std::fprintf(stderr, "[HEAPCHECK] f%llu free of %08X (block %08X) that is NOT in heap %08X's chain (double free / another heap's block), ra %08X step '%s'; the heap's last operations:\n", static_cast<unsigned long long>(g_hc_frame), fptr, blk, fheap, static_cast<uint32_t>(ctx->r[31]), g_tt_step); hc_dump_ring(fheap); } }
    }
    g_orig_heap_free(rdram, ctx);
    if (kHeapCheck) hc_verify(rdram, fheap, "after a free");
    dissidia_tagteam_after_free(rdram, ctx, fheap, fptr);
}

// ---- Effect pool (EFFECT_ALLOCATOR) ----
// FUN_088A5054 at boot: sceKernelAllocMemoryBlock("EFFECT_ALLOCATOR", 0, 0x3DD000), then FUN_08867310(heap 0x09B4D420,
// base, 0x3DD000, 0x40) makes it the handle heap that every character's effect package (objx effe: FEP + FEPA) and the
// effect instances are allocated from (FUN_088673CC(heap, size) -> handle id, 0 = out of memory). Retail sizes it for two
// retail fighters; a big modded package (Noctis with Alterna's baked sheets, 2.55 MB vs 1.75 MB) runs it out mid-battle:
// new effects / projectiles fail to allocate and textures come out as garbage. The block is the last bump allocation
// before the heap init, so the extra bytes are bump-allocated right behind it and the heap is initialised with the sum.
// PSPRECOMP_EFFECT_POOL_EXTRA (bytes, 0 = retail).
// Tag team: every character loaded mid-battle puts its effect package here too (1.06-1.17 MB each + a ~0.3 MB companion, 18 Nov logs) and
// nothing is ever freed, so retail + 3 MB holds the two starting fighters' packs plus THREE more; the fourth newcomer's pack fails (`effect
// pool alloc(1.1 MB) FAILED`) and that character fights with invisible moves and no effect sounds (the user's RP6 report, 17 Nov). The extension
// therefore takes all that region 1 has left behind the retail block (minus a pending sound heap and kFxMargin); the tag team pool is sized to leave
// kFxMinExtension at least. The extension MUST be contiguous with the retail block: on 17 Nov (8 MB pool extra) it landed in region 2 instead,
// the pool stayed at retail size (`NOT extended`) and the 3 MB in region 2 were wasted - which also starved the texture overflow heap.
constexpr uint32_t kFxHeap = 0x09B4D420u;
constexpr uint32_t kSndHeap = 0x09B4CF80u;   // the sound manager's handle heap (same allocator)
int hh_idx(uint32_t heap) { return heap == kFxHeap ? 0 : heap == kSndHeap ? 1 : -1; }
const char* hh_name(uint32_t heap) { return heap == kFxHeap ? "fx" : "snd"; }
uint32_t g_fx_extra = 0x300000u; bool g_fx_extra_forced = false;
FuncPtr g_orig_fx_heap_init = nullptr, g_orig_fx_alloc = nullptr;
// ---- Handle heap bridge into region 2 (19 Nov) ----
// The effect pool and the sound heap must be contiguous blocks of region 1, and region 1 is full (model pool 15.5 MB + sound heap 4.8 MB + effect pool
// 8.2 MB). Their allocator only follows the block list (heap+12, prev / next) and merges a freed block with free LIST neighbours, so the list may
// jump: behind the heap's own range a locked, used "gap" block (a handle id nobody has, so it is never freed, and locked, so compaction never moves
// it) spans everything up to a block of region 2, which follows as a free block. Allocations, frees, merges and compaction then work on both parts;
// no block can cross the gap. The region 2 room comes from the smaller texture overflow heap. PSPRECOMP_FX_BRIDGE / PSPRECOMP_SND_BRIDGE=<bytes>
// (0 = off) force the sizes; default: the sound heap 512 KB, the effect pool what region 2 has left minus OBJDATA and a 64 KB margin.
uint32_t g_gap_id[2] = {0, 0}, g_gap_blk[2] = {0, 0}, g_bridge_size[2] = {0, 0};
uint32_t bridge_env(const char* n, uint32_t def) { const char* e = std::getenv(n); return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) & ~0xFFu : def; }
uint32_t bridge_want(uint32_t heap) {
    if (!g_tt_pool_extra) return 0;
    const uint32_t keep = rd256(kObjdataTagSize) + 0x10000u, free2 = psp_kmem_free_region(2);
    const uint32_t avail = free2 > keep ? (free2 - keep) & ~0xFFFu : 0u;
    uint32_t want = heap == kSndHeap ? bridge_env("PSPRECOMP_SND_BRIDGE", 0x80000u) : bridge_env("PSPRECOMP_FX_BRIDGE", avail);
    if (want > avail) want = avail;
    return want >= 0x40000u ? want : 0u;   // not worth a gap below 256 KB
}
// after the heap's init over [base, base + size - H): the gap block at base + size - H, then the region 2 block
void hh_bridge(uint8_t* rdram, uint32_t heap, uint32_t gap_hdr, uint32_t ext, uint32_t ext_size) {
    const int hi = hh_idx(heap); const uint32_t H = 64u;
    uint32_t last = 0, n = 0; for (uint32_t b = MEM_W(rdram, heap + 12u); b && n < 100000; ++n, b = MEM_W(rdram, b + 16u)) last = b;
    if (!last || ext <= gap_hdr + H) return;
    const uint32_t id = MEM_W(rdram, heap + 16u); MEM_W_WRITE(rdram, heap + 16u, id + 1u);
    MEM_W_WRITE(rdram, gap_hdr, id); MEM_W_WRITE(rdram, gap_hdr + 4u, ext - gap_hdr - H); MEM_W_WRITE(rdram, gap_hdr + 8u, 1u);
    MEM_W_WRITE(rdram, gap_hdr + 12u, last); MEM_W_WRITE(rdram, gap_hdr + 16u, ext);
    MEM_W_WRITE(rdram, last + 16u, gap_hdr);
    MEM_W_WRITE(rdram, ext, 0u); MEM_W_WRITE(rdram, ext + 4u, ext_size - H); MEM_W_WRITE(rdram, ext + 8u, 0u); MEM_W_WRITE(rdram, ext + 12u, gap_hdr); MEM_W_WRITE(rdram, ext + 16u, 0u);
    g_gap_id[hi] = id; g_gap_blk[hi] = gap_hdr; g_bridge_size[hi] = ext_size;
    std::fprintf(stderr, "[HHEAP] %s heap bridged into region 2: gap block #%u at 0x%08X (locked, %u B), free block 0x%08X %u B\n", hh_name(heap), id, gap_hdr, ext - gap_hdr - H, ext, ext_size - H);
}
void fx_heap_init_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t heap = static_cast<uint32_t>(ctx->r[4]), base = static_cast<uint32_t>(ctx->r[5]);
    const uint32_t size = static_cast<uint32_t>(ctx->r[6]);
    if (heap == kSndHeap && static_cast<uint32_t>(ctx->r[7]) == 0x40u && !g_gap_blk[1]) {
        if (const uint32_t want = bridge_want(heap)) if (const uint32_t ext = psp_kmem_bump_alloc_named_r2(want, "SOUND_HEAP (region 2 part)")) {
            ctx->r[6] = static_cast<int32_t>(size - 64u);
            g_orig_fx_heap_init(rdram, ctx);
            hh_bridge(rdram, heap, base + size - 64u, ext, want);
            return;
        }
    }
    if (heap == kFxHeap && size == kFxRetail && (g_fx_extra || g_tt_pool_extra)) {
        uint32_t extra = (g_fx_extra + 0xFFu) & ~0xFFu;
        const uint32_t free1 = psp_kmem_free_region(1), keep = dissidia_tagteam_soundheap_pending() + kFxMargin;
        if (g_tt_pool_extra && !g_fx_extra_forced) extra = free1 > keep ? (free1 - keep) & ~0xFFu : 0u;   // tag team: fill region 1
        if (extra > free1) { std::fprintf(stderr, "[FXPOOL] effect pool extension %u B does not fit region 1 (%u B left): %u B\n", extra, free1, free1 & ~0xFFu); extra = free1 & ~0xFFu; }
        const uint32_t ext = extra ? psp_kmem_bump_alloc_named(extra, "EFFECT_ALLOCATOR (extension)") : 0u;
        if (ext && ext == base + size) {
            ctx->r[6] = static_cast<int32_t>(size + extra);
            std::fprintf(stderr, "[FXPOOL] effect pool 0x%08X: %u B (retail %u B + %u B)\n", base, size + extra, size, extra);
            if (const uint32_t want = bridge_want(heap)) if (const uint32_t ext2 = psp_kmem_bump_alloc_named_r2(want, "EFFECT_ALLOCATOR (region 2 part)")) {
                const uint32_t total = size + extra;
                ctx->r[6] = static_cast<int32_t>(total - 64u);
                g_orig_fx_heap_init(rdram, ctx);
                hh_bridge(rdram, heap, base + total - 64u, ext2, want);
                return;
            }
        } else {
            std::fprintf(stderr, "[FXPOOL] effect pool NOT extended: extension at 0x%08X, pool ends 0x%08X\n", ext, base + size);
        }
    }
    g_orig_fx_heap_init(rdram, ctx);
}
// walk the block list (heap+12: first block {id(0 = free), size, ?, prev, next}) for the free total
void fx_pool_free(const uint8_t* rdram, uint32_t* total, uint32_t* free_b, uint32_t* largest, uint32_t heap = kFxHeap) {
    uint32_t n = 0; *total = *free_b = *largest = 0;
    const uint32_t* h = reinterpret_cast<const uint32_t*>(rdram + (heap & 0x07FFFFFFu));
    const int hi = hh_idx(heap);
    *total = h[1] + (hi >= 0 ? g_bridge_size[hi] : 0u);
    for (uint32_t b = h[3]; b && n < 100000; ++n) {
        const uint32_t* bk = reinterpret_cast<const uint32_t*>(rdram + (b & 0x07FFFFFFu));
        if (bk[0] == 0) { *free_b += bk[1]; *largest = std::max(*largest, bk[1]); }
        b = bk[4];
    }
}
uint32_t g_fx_fail_total = 0;
// ---- Handle heap instrumentation (PSPRECOMP_HHEAP_LOG=1): every allocation / free / compaction of the effect pool and the sound heap ----
// The two heaps share the allocator: FUN_08867310 init, FUN_088673CC alloc (first fit from the lowest address, handle id from heap+16), FUN_088674AC
// free (merges with free neighbours), FUN_088676B4 lock (+8 = 1, returns the data), FUN_08867718 unlock, FUN_08867590 compaction (an UNLOCKED block
// whose previous block is free slides down into it; locked blocks stay). Block header (rounded to the heap's alignment, 64 B): {id (0 = free), data
// size, locked, prev, next}; heap: {base, size, align, first block, next id}.
}  // namespace
int dissidia_tagteam_loading_char();   // tagteam.cpp: the character load_character is loading now (0 = none)
namespace {
bool hh_log() { static const bool v = std::getenv("PSPRECOMP_HHEAP_LOG") != nullptr; return v; }
struct HhOwner { uint32_t size, ra; int ch; };
std::vector<std::pair<uint32_t, HhOwner>> g_hh_own[2];   // per heap: id -> who allocated it
HhOwner* hh_owner(int hi, uint32_t id) { for (auto& e : g_hh_own[hi]) if (e.first == id) return &e.second; return nullptr; }
uint32_t hh_block(const uint8_t* rdram, uint32_t heap, uint32_t id) {
    const uint32_t* h = reinterpret_cast<const uint32_t*>(rdram + (heap & 0x07FFFFFFu)); uint32_t n = 0;
    for (uint32_t b = h[3]; b && n < 100000; ++n) { const uint32_t* bk = reinterpret_cast<const uint32_t*>(rdram + (b & 0x07FFFFFFu)); if (bk[0] == id) return b; b = bk[4]; }
    return 0;
}
void hh_dump(const uint8_t* rdram, uint32_t heap, const char* tag) {
    const int hi = hh_idx(heap); if (hi < 0) return;
    const uint32_t* h = reinterpret_cast<const uint32_t*>(rdram + (heap & 0x07FFFFFFu)); uint32_t n = 0;
    std::string line; char b[96];
    for (uint32_t blk = h[3]; blk && n < 100000; ++n) {
        const uint32_t* bk = reinterpret_cast<const uint32_t*>(rdram + (blk & 0x07FFFFFFu));
        if (bk[0] == 0) std::snprintf(b, sizeof b, " [%08X FREE %u]", blk, bk[1]);
        else { const HhOwner* o = hh_owner(hi, bk[0]); std::snprintf(b, sizeof b, " [%08X #%u %u%s c%d ra%08X]", blk, bk[0], bk[1], bk[2] ? " L" : "", o ? o->ch : -1, o ? o->ra : 0u); }
        line += b; blk = bk[4];
    }
    std::fprintf(stderr, "[HHEAP] %s blocks %s:%s\n", hh_name(heap), tag ? tag : "", line.c_str());
}
FuncPtr g_orig_hh_free = nullptr, g_orig_hh_compact = nullptr;
void hh_free_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t heap = static_cast<uint32_t>(ctx->r[4]), id = static_cast<uint32_t>(ctx->r[5]);
    const int hi = hh_idx(heap);
    if (hi >= 0 && hh_log()) {
        const uint32_t blk = hh_block(rdram, heap, id); const HhOwner* o = hh_owner(hi, id);
        const uint32_t caller = MEM_W(rdram, ctx->r[29] + 0);
        std::fprintf(stderr, "[HHEAP] %s free #%u blk %08X size %u (alloc c%d ra %08X) ra %08X/%08X loading c%d\n", hh_name(heap), id, blk,
                     blk ? MEM_W(rdram, blk + 4) : 0u, o ? o->ch : -1, o ? o->ra : 0u, static_cast<uint32_t>(ctx->r[31]), caller, dissidia_tagteam_loading_char());
    }
    g_orig_hh_free(rdram, ctx);
    if (hi >= 0) { auto& v = g_hh_own[hi]; for (size_t i = 0; i < v.size(); i++) if (v[i].first == id) { v.erase(v.begin() + static_cast<long>(i)); break; } }
}
// ---- The sound system's bank registry (19 Nov) ----
// FUN_088A0DBC(system, name, data, size, flag) -> FUN_088A0EEC(system, request): 32 entries of 48 B at the system (*0x089D6C80); an entry whose
// name matches gets refcount (+40) + 1 and its index back (its OLD data stays), else a free entry (+0 == -1) is filled; -1 = table full.
// FUN_088A0DFC(system, index): refcount - 1, freed at 0. PSPRECOMP_SNDREG_LOG=1 (or PSPRECOMP_HHEAP_LOG): every call with the table's occupancy.
bool sndreg_log() { static const bool v = std::getenv("PSPRECOMP_SNDREG_LOG") || std::getenv("PSPRECOMP_HHEAP_LOG"); return v; }
FuncPtr g_orig_sndreg = nullptr, g_orig_sndunreg = nullptr;
std::string guest_str(const uint8_t* rdram, uint32_t a, size_t n = 40) { std::string r; if (a < 0x08000000u || a >= 0x0C000000u) return r; for (size_t i = 0; i < n; i++) { const char c = static_cast<char>(rdram[(a + i) & 0x07FFFFFFu]); if (!c) break; r += (c >= 32 && c < 127) ? c : '?'; } return r; }
int sndreg_used(uint8_t* rdram, uint32_t sys) { int u = 0; for (uint32_t i = 0; i < 32; i++) if (MEM_W(rdram, sys + 48u * i) != -1) u++; return u; }
void sndreg_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t sys = static_cast<uint32_t>(ctx->r[4]), req = static_cast<uint32_t>(ctx->r[5]);
    const std::string name = guest_str(rdram, static_cast<uint32_t>(MEM_W(rdram, req + 4u)));
    const uint32_t data = static_cast<uint32_t>(MEM_W(rdram, req + 8u)), size = static_cast<uint32_t>(MEM_W(rdram, req + 12u));
    g_orig_sndreg(rdram, ctx);
    if (sndreg_log()) { const int idx = ctx->r[2];
        std::fprintf(stderr, "[SNDREG] register '%s' data %08X size %u -> #%d (refs %d, entry data %08X) loading c%d, %d of 32 used\n", name.c_str(), data, size, idx,
                     idx >= 0 ? MEM_W(rdram, sys + 48u * static_cast<uint32_t>(idx) + 40u) : 0, idx >= 0 ? static_cast<uint32_t>(MEM_W(rdram, sys + 48u * static_cast<uint32_t>(idx) + 4u)) : 0u,
                     dissidia_tagteam_loading_char(), sndreg_used(rdram, sys)); }
}
void sndunreg_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t sys = static_cast<uint32_t>(ctx->r[4]), idx = static_cast<uint32_t>(ctx->r[5]);
    const int refs = idx < 32u ? MEM_W(rdram, sys + 48u * idx + 40u) : -99;
    g_orig_sndunreg(rdram, ctx);
    if (sndreg_log()) std::fprintf(stderr, "[SNDREG] unregister #%d (refs %d -> %d), %d of 32 used\n", static_cast<int>(idx), refs, idx < 32u ? MEM_W(rdram, sys + 48u * idx + 40u) : -99, sndreg_used(rdram, sys));
}
// FUN_0883EBE4(holder): binds an effect pack (holder +32 = pool handle) to the effect manager, instance -> holder +40. Logged with the registry.
FuncPtr g_orig_fxbind = nullptr;
void fxbind_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t hold = static_cast<uint32_t>(ctx->r[4]); const uint32_t h = static_cast<uint32_t>(MEM_W(rdram, hold + 32u)), old = static_cast<uint32_t>(MEM_W(rdram, hold + 40u));
    g_orig_fxbind(rdram, ctx);
    if (sndreg_log()) { const int hi = 0; const HhOwner* o = h ? hh_owner(hi, h) : nullptr; static int n = 0;
        if (n++ < 400) std::fprintf(stderr, "[FXBIND] holder %08X pack handle %u (alloc c%d) instance %08X -> %08X%s\n", hold, h, o ? o->ch : -1, old, static_cast<uint32_t>(MEM_W(rdram, hold + 40u)), old ? " (BOUND AGAIN)" : ""); }
}
void hh_compact_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t heap = static_cast<uint32_t>(ctx->r[4]);
    uint32_t t0, f0, l0; if (hh_log()) fx_pool_free(rdram, &t0, &f0, &l0, heap);
    const uint32_t ra = static_cast<uint32_t>(ctx->r[31]), caller = MEM_W(rdram, ctx->r[29] + 0);
    g_orig_hh_compact(rdram, ctx);
    if (hh_log()) { uint32_t t1, f1, l1; fx_pool_free(rdram, &t1, &f1, &l1, heap);
        static int n = 0; if (n++ < 200) std::fprintf(stderr, "[HHEAP] %s compaction (ra %08X/%08X): largest %u -> %u of %u free\n", heap == kFxHeap ? "fx" : heap == kSndHeap ? "snd" : "?", ra, caller, l0, l1, f1); }
}
// Tag team: characters come and go, and the pool fragments: a departed character's pack (1.06-1.17 MB) left a hole a slightly bigger newcomer's pack did
// not fit (19 Nov desktop: Vaan's 1145968 B pack FAILED with 2.1 MB free, largest hole 1095360 B -> invisible moves). Every character pack is therefore
// allocated in a slot of one size (the largest retail pack seen, 1172176 B, rounded up): any freed slot takes any newcomer. Packs outside the window
// (modded ones, the ~0.3 MB companions, effect instances) keep their size; a padded request that does not fit is retried at the exact size.
// PSPRECOMP_FX_PACK_SLOT=<bytes> (0 = off).
uint32_t fx_pack_slot() { static const uint32_t v = [] { const char* e = std::getenv("PSPRECOMP_FX_PACK_SLOT"); return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0x120000u; }(); return v; }
// Top-down allocation (the mirror of FUN_088673CC): the free block with the HIGHEST address that holds `size`, carved from its end. Same header, same
// handle ids (heap+16), so the game's free / lock / compaction work on it unchanged. Returns the handle id, 0 = no room.
uint32_t hh_alloc_top(uint8_t* rdram, uint32_t heap, uint32_t size) {
    const uint32_t al = MEM_W(rdram, heap + 8u); if (!al || !size) return 0;
    auto rnd = [&](uint32_t v) { return (v + al - 1u) / al * al; };
    const uint32_t H = rnd(20u), N = rnd(size);
    uint32_t pick = 0, n = 0;
    for (uint32_t b = MEM_W(rdram, heap + 12u); b && n < 100000; ++n, b = MEM_W(rdram, b + 16u))
        if (MEM_W(rdram, b) == 0 && MEM_W(rdram, b + 4u) >= N) pick = b;
    if (!pick) return 0;
    const uint32_t id = MEM_W(rdram, heap + 16u); MEM_W_WRITE(rdram, heap + 16u, id + 1u);
    const uint32_t S = MEM_W(rdram, pick + 4u);
    if (S <= H + N) { MEM_W_WRITE(rdram, pick, id); return id; }   // too small to split: the whole block (as retail does)
    const uint32_t rem = S - H - N, u = pick + H + rem, nx = MEM_W(rdram, pick + 16u);
    MEM_W_WRITE(rdram, u, id); MEM_W_WRITE(rdram, u + 4u, N); MEM_W_WRITE(rdram, u + 8u, 0u); MEM_W_WRITE(rdram, u + 12u, pick); MEM_W_WRITE(rdram, u + 16u, nx);
    if (nx) MEM_W_WRITE(rdram, nx + 12u, u);
    MEM_W_WRITE(rdram, pick + 4u, rem); MEM_W_WRITE(rdram, pick + 16u, u);
    return id;
}
// Tag team placement (19 Nov, desktop vaan3): the slots alone did not keep the pool usable. After Tifa's slot was freed, Bartz's 77 KB second pack
// (first fit = the lowest hole) went INTO that slot: 1102784 B left, and Vaan's 1145968 B pack FAILED with 1.76 MB free. Character packs now take
// the lowest hole (first fit, slot-sized) and everything smaller than a pack (W_ / assist packs, ~0.3 MB companions, effect instances) the highest
// hole that fits, so the two kinds grow toward each other and a freed slot stays a whole slot. PSPRECOMP_FX_SMALL_TOP=0: first fit for all (old).
bool fx_small_top() { static const bool v = [] { const char* e = std::getenv("PSPRECOMP_FX_SMALL_TOP"); return !e || std::atoi(e) != 0; }(); return v; }
void fx_alloc_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t heap = static_cast<uint32_t>(ctx->r[4]), size = static_cast<uint32_t>(ctx->r[5]);
    const uint32_t slot = fx_pack_slot();
    bool done = false;
    if (heap == kFxHeap && g_tt_pool_extra && slot && size >= 0xE0000u && size < slot) {
        const recomp_context sv = *ctx;
        ctx->r[5] = static_cast<int32_t>(slot);
        g_orig_fx_alloc(rdram, ctx);
        ctx->r[5] = sv.r[5];
        if (ctx->r[2] != 0) { done = true; static int n = 0; if (n++ < 40) std::fprintf(stderr, "[FXPOOL] effect pack alloc(%u) in a %u B slot -> id %d\n", size, slot, ctx->r[2]); }
        else { const int32_t keep_ra = ctx->r[31]; *ctx = sv; ctx->r[31] = keep_ra; }   // no slot free: the exact size
    }
    // The sound heap the same way: a character's two big blocks (~360 KB + 250-300 KB) from the bottom, the 1.6 KB block between them and the 52-60 KB
    // assist voice banks (voice_banks_load) from the top, so a departed character leaves one ~650 KB hole. PSPRECOMP_SND_SMALL_TOP=0: off.
    static const bool snd_top = [] { const char* e = std::getenv("PSPRECOMP_SND_SMALL_TOP"); return !e || std::atoi(e) != 0; }();
    if (!done && heap == kSndHeap && g_tt_pool_extra && snd_top && size && size < 0x20000u) {
        if (const uint32_t id = hh_alloc_top(rdram, heap, size)) { ctx->r[2] = static_cast<int32_t>(id); done = true; }
    }
    if (!done && heap == kFxHeap && g_tt_pool_extra && fx_small_top() && size && size < 0xE0000u) {
        if (const uint32_t id = hh_alloc_top(rdram, heap, size)) { ctx->r[2] = static_cast<int32_t>(id); done = true; }
    }
    if (!done) g_orig_fx_alloc(rdram, ctx);
    if (const int hi = hh_idx(heap); hi >= 0 && ctx->r[2] != 0) {
        const uint32_t id = static_cast<uint32_t>(ctx->r[2]), caller = MEM_W(rdram, ctx->r[29] + 0);
        g_hh_own[hi].push_back({id, HhOwner{size, caller, dissidia_tagteam_loading_char()}});
        if (hh_log()) { uint32_t t, f, l; fx_pool_free(rdram, &t, &f, &l, heap);
            std::fprintf(stderr, "[HHEAP] %s alloc(%u) -> #%u blk %08X ra %08X/%08X loading c%d (free %u largest %u)\n", hh_name(heap), size, id, hh_block(rdram, heap, id),
                         static_cast<uint32_t>(ctx->r[31]), caller, dissidia_tagteam_loading_char(), f, l); }
    }
    if (heap == kSndHeap && size && ctx->r[2] == 0) {
        static int nsf = 0; uint32_t t, f, l; fx_pool_free(rdram, &t, &f, &l, heap);
        if (nsf++ < 50) std::fprintf(stderr, "[SNDHEAP] sound heap alloc(%u) FAILED: heap %u B, free %u B, largest free %u B (caller %08X, loading c%d)\n", size, t, f, l, MEM_W(rdram, ctx->r[29] + 0), dissidia_tagteam_loading_char());
        if (hh_log()) hh_dump(rdram, heap, "at the failure");
    }
    if (heap != kFxHeap || size == 0) return;
    static int nfail = 0, nbig = 0;
    if (ctx->r[2] == 0) {
        g_fx_fail_total++;
        if (nfail++ < 50) {
            uint32_t total, free_b, largest; fx_pool_free(rdram, &total, &free_b, &largest);
            std::fprintf(stderr, "[FXPOOL] effect pool alloc(%u) FAILED: pool %u B, free %u B, largest free %u B\n",
                         size, total, free_b, largest);
        }
        if (hh_log()) hh_dump(rdram, heap, "at the failure");
    } else if (size >= 0x40000u && nbig++ < 40) {
        std::fprintf(stderr, "[FXPOOL] effect pool alloc(%u) -> id %d\n", size, ctx->r[2]);
    }
}

// The effect allocator is the last big host-heap block requested at boot: print the final layout after it.
void partition_alloc_observer(const char* name, uint32_t, uint32_t) {
    if (name && std::strcmp(name, "EFFECT_ALLOCATOR") == 0) psp_kmem_log_layout("final, after the effect allocator");
}

}  // namespace

// hooks_io.cpp (largest model override) and PSPRECOMP_MODEL_BUDGET. Returns the budget in effect.
uint32_t dissidia_model_budget_request(uint32_t bytes) {
    if (bytes > g_staging) {
        uint32_t b = (bytes + 0xFFFu) & ~0xFFFu;
        b = b > kMaxStaging ? kMaxStaging : b;
        if (g_reserved) {   // the pool and file heap are already reserved for g_staging: it cannot grow any more
            if (b > g_staging) std::fprintf(stderr, "[MODELS] budget request %u B exceeds the reserved budget %u B (ignored)\n", b, g_staging);
        } else {
            g_staging = b;
        }
    }
    return g_staging;
}
uint32_t dissidia_model_budget() { return g_staging; }
// tagteam.cpp: extra characters in the battle (tag team) need more room in the model pool than the retail two fighters and their assists.
// The pool request at the loading screen is then served from the host heap with `bytes` more (kept like the budget pool, reused by every battle).
void dissidia_model_pool_request_extra(uint32_t bytes) { if (bytes > g_tt_pool_extra) g_tt_pool_extra = bytes; }
// tagteam.cpp: one line on the tag battle's memory after a mid-battle load (model pool, effect pool, texture overflow heap)
uint32_t dissidia_fx_fail_count() { return g_fx_fail_total; }
uint32_t dissidia_model_pool_free(const uint8_t* rdram) { if (!g_pool_block) return 0; const uint32_t* h = reinterpret_cast<const uint32_t*>(rdram + (g_pool_block & 0x07FFFFFFu)); return g_pool_size > h[2] ? g_pool_size - h[2] : 0u; }   // tagteam.cpp: room left in the model pool (extras' assists)
void dissidia_fx_pool_free(const uint8_t* rdram, uint32_t* total, uint32_t* free_b, uint32_t* largest) { fx_pool_free(rdram, total, free_b, largest); }
void dissidia_snd_heap_free(const uint8_t* rdram, uint32_t* total, uint32_t* free_b, uint32_t* largest) { fx_pool_free(rdram, total, free_b, largest, kSndHeap); }
bool dissidia_hheap_locked(const uint8_t* rdram, uint32_t heap, uint32_t id) { const uint32_t b = hh_block(rdram, heap, id); return !b || MEM_W(const_cast<uint8_t*>(rdram), b + 8u) != 0; }
void dissidia_models_mem_summary(const uint8_t* rdram, const char* tag) {
    auto heap_used = [&](uint32_t heap, uint32_t* end, uint32_t* used) { *end = *used = 0; if (!heap) return; const uint32_t* h = reinterpret_cast<const uint32_t*>(rdram + (heap & 0x07FFFFFFu)); *end = h[1]; *used = h[2]; };
    uint32_t pe, pu, te, tu, total, free_b, largest; heap_used(g_pool_block, &pe, &pu); heap_used(g_ov_heap, &te, &tu); fx_pool_free(rdram, &total, &free_b, &largest);
    uint32_t st, sf, sl; fx_pool_free(rdram, &st, &sf, &sl, 0x09B4CF80u);   // the sound manager's handle heap (same allocator)
    std::fprintf(stderr, "[MEMSUM] %s: model pool used %u of %u KB, effect pool free %u KB (largest %u KB) of %u KB, texture overflow used %u of %u KB, sound heap free %u KB (largest %u KB) of %u KB\n",
                 tag ? tag : "", pu >> 10, g_pool_size >> 10, free_b >> 10, largest >> 10, total >> 10, tu >> 10, g_ov_heap ? kOverflowSize >> 10 : 0u, sf >> 10, sl >> 10, st >> 10);
    if (hh_log()) { hh_dump(rdram, kFxHeap, tag); hh_dump(rdram, kSndHeap, tag); }
}
constexpr uint32_t dissidia_model_budget_retail() { return kRetailStaging; }

void dissidia_models_install() {
    psp_kmem_set_partition_alloc_observer(partition_alloc_observer);
    if (const char* e = std::getenv("PSPRECOMP_EFFECT_POOL_EXTRA")) { g_fx_extra = static_cast<uint32_t>(std::strtoul(e, nullptr, 0)); g_fx_extra_forced = true; }
    g_orig_fx_heap_init = psp_dispatch_probe_lookup(0x08867310u);
    g_orig_fx_alloc = psp_dispatch_probe_lookup(0x088673CCu);
    if (g_orig_fx_heap_init && g_orig_fx_alloc) {
        psp_dispatch_register(0x08867310u, fx_heap_init_hook);
        psp_dispatch_register(0x088673CCu, fx_alloc_hook);
        if ((g_orig_hh_free = psp_dispatch_probe_lookup(0x088674ACu))) psp_dispatch_register(0x088674ACu, hh_free_hook);
        if ((g_orig_hh_compact = psp_dispatch_probe_lookup(0x08867590u))) psp_dispatch_register(0x08867590u, hh_compact_hook);
        if ((g_orig_sndreg = psp_dispatch_probe_lookup(0x088A0EECu))) psp_dispatch_register(0x088A0EECu, sndreg_hook);
        if ((g_orig_fxbind = psp_dispatch_probe_lookup(0x0883EBE4u))) psp_dispatch_register(0x0883EBE4u, fxbind_hook);
        if ((g_orig_sndunreg = psp_dispatch_probe_lookup(0x088A0DFCu))) psp_dispatch_register(0x088A0DFCu, sndunreg_hook);
    } else {
        std::fprintf(stderr, "[FXPOOL] effect pool hooks NOT installed (a target function is missing)\n");
    }
    if (const char* e = std::getenv("PSPRECOMP_MODEL_POOL_EXTRA")) { g_pool_extra = static_cast<uint32_t>(std::strtoul(e, nullptr, 0)); g_pool_extra_forced = true; }
    if (const char* e = std::getenv("PSPRECOMP_MODEL_BUDGET")) dissidia_model_budget_request(static_cast<uint32_t>(std::strtoul(e, nullptr, 0)));
    g_orig_alloc = psp_dispatch_probe_lookup(0x0888204Cu);
    g_orig_register = psp_dispatch_probe_lookup(0x0884417Cu);
    g_orig_free = psp_dispatch_probe_lookup(0x0888208Cu);
    g_orig_heap_init = psp_dispatch_probe_lookup(0x0885C40Cu);
    if (!g_orig_alloc || !g_orig_register || !g_orig_free || !g_orig_heap_init) {
        std::fprintf(stderr, "[MODELS] model budget hooks NOT installed (a target function is missing)\n");
        g_staging = kRetailStaging;
        return;
    }
    g_orig_heap_alloc = psp_dispatch_probe_lookup(0x0885C6CCu);
    if (g_orig_heap_alloc) psp_dispatch_register(0x0885C6CCu, heap_alloc_hook);
    if ((g_orig_heap_free = psp_dispatch_probe_lookup(0x0885C538u))) psp_dispatch_register(0x0885C538u, heap_free_hook);
    if ((g_orig_small_alloc = psp_dispatch_probe_lookup(0x088820D8u))) psp_dispatch_register(0x088820D8u, small_alloc_hook);
    psp_dispatch_register(0x0888204Cu, alloc_hook);
    psp_dispatch_register(0x0885C40Cu, heap_init_hook);
    psp_dispatch_register(0x0884417Cu, register_hook);
    psp_dispatch_register(0x0888208Cu, free_hook);
}
