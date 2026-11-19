// games/dissidia/runtime/xdelta_mods.cpp — the code side of installed xdelta mods (tools/xdelta_mod.py).
//
// An xdelta mod folder (<mods>/<name>/mod.json with "kind": "xdelta") is an ordinary installed mod folder (its install/
// files override PACKAGE.BIN entries, Settings > Mods turns it on and off) plus, when the ISO patch changed game code:
//   code/EBOOT.BIN, code/OVL_*_APP.ELF   the patched binaries
//   code/eboot.patch                     {u32 vaddr, u32 len, bytes}... the EBOOT bytes the patch changed
// and, when it appended data to PACKAGE.BIN past the last entry, package_tail.bin (served by hooks_io.cpp), and
// usrdir/<path> for other changed disc files.
//
// The changed functions were recompiled at build time (tools/xdelta_code.py) into the xmods function set, whose
// dispatch.cpp lists them per mod folder and module (0 = EBOOT, 1..3 = battle / menu / extra overlay). While a mod is
// on: its EBOOT functions are registered in the main dispatch table before any other hook (a hook that wraps one of
// them wraps the mod's version), its overlay functions come first in the overlay lookups (dissidia_ovl_*_lookup, used by
// the resolver and by every hook that wraps an overlay function), its EBOOT byte changes are written at boot and its
// overlay files are read instead of the disc's.
// A folder whose code is not in this build (imported after the last build, or changed since) is kept off entirely:
// the mod's data without its code would not work.

#include "hle/psp_hle.h"
#include "recomp.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

// layout shared with the generated <work>/overlays/xmods/dispatch.cpp (tools/xdelta_code.py write_set)
struct DissidiaXmodFn { uint32_t addr; void (*fn)(uint8_t*, recomp_context*); };
struct DissidiaXmodSet {
    const char* folder; const char* stamp; int module; const DissidiaXmodFn* fns; int count;
    // run-time code (module 4: code the mod loads from its package_tail.bin; modules 0..3 with blob_size: the game's
    // functions with the calls that code rewrites): active once that code is read to blob_addr, else 0
    uint64_t blob_off; uint32_t blob_size; uint32_t blob_addr;
};
extern const DissidiaXmodSet* dissidia_xmod_sets(int* count);
extern FuncPtr ovl_battle_lookup(uint32_t vaddr);    // the generated overlay sets (build_overlay.py)
extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_extra_lookup(uint32_t vaddr);

std::string dissidia_mods_dir();                       // hooks_io.cpp
std::vector<std::string> dissidia_mod_roots();         // hooks_io.cpp: <mods>/<folder>/install of the folders that are on

namespace {

constexpr int kModules = 4;   // 0 EBOOT, 1..3 overlays; 4 = code loaded at run time (sets only)
constexpr int kModBlob = 4;
const char* const kOvlFile[kModules] = {"EBOOT.BIN", "OVL_BATTLE_APP.ELF", "OVL_MENU_APP.ELF", "OVL_EXTRA_APP.ELF"};

std::map<uint32_t, FuncPtr> g_ovl[kModules];           // overlay id -> vaddr -> the mod's function
struct DynFn { FuncPtr fn; const std::atomic<bool>* on; };
std::map<uint32_t, DynFn> g_ovl_dyn[kModules];          // the same for functions active once their run-time code loaded
std::vector<const DissidiaXmodSet*> g_dyn_sets;         // sets of the mods that are on with blob_size != 0
std::atomic<bool> g_dyn_on[64];                         // per entry of g_dyn_sets
std::vector<std::string> g_active;                     // folders (full paths) whose code is installed
bool g_installed = false;
std::string g_tail_dir;                                // the mod folder whose package_tail.bin is served
uint8_t* g_rdram = nullptr;

std::string read_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// "key": "value" in a flat JSON text ("" when absent)
std::string json_str(const std::string& js, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = js.find(k);
    if (p == std::string::npos) return "";
    p = js.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = js.find('"', p);
    if (p == std::string::npos) return "";
    const size_t e = js.find('"', p + 1);
    return e == std::string::npos ? "" : js.substr(p + 1, e - p - 1);
}

bool is_xdelta(const std::string& js) { return json_str(js, "kind") == "xdelta"; }

// the mod.json lists the patched modules: "code": ["EBOOT.BIN", ...]; an empty list = data only
bool has_code(const std::string& js) {
    size_t p = js.find("\"code\"");
    if (p == std::string::npos) return false;
    p = js.find(':', p);
    if (p == std::string::npos) return false;
    while (++p < js.size() && (js[p] == ' ' || js[p] == '\t' || js[p] == '\n' || js[p] == '\r')) {}
    if (p >= js.size() || js[p] != '[') return false;
    while (++p < js.size() && (js[p] == ' ' || js[p] == '\t' || js[p] == '\n' || js[p] == '\r')) {}
    return p < js.size() && js[p] != ']';
}

std::string base_name(const std::string& dir) {
    const size_t s = dir.find_last_of('/');
    return s == std::string::npos ? dir : dir.substr(s + 1);
}

bool built_in(const std::string& name, const std::string& stamp) {
    int n = 0;
    const DissidiaXmodSet* sets = dissidia_xmod_sets(&n);
    for (int i = 0; i < n; i++)
        if (name == sets[i].folder && stamp == sets[i].stamp) return true;
    return false;
}

bool is_file(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }

}  // namespace

// hooks_io.cpp mod_roots(): may this folder be used? Not when it is an xdelta mod with code that this build lacks.
bool dissidia_xmod_folder_usable(const std::string& folder_dir, bool say) {
    const std::string js = read_text(folder_dir + "/mod.json");
    if (!is_xdelta(js) || !has_code(js)) return true;
    const std::string name = base_name(folder_dir);
    if (built_in(name, json_str(js, "code_stamp"))) return true;
    if (say)
        std::fprintf(stderr, "[XMOD] %s changes the game's code, which this build does not have yet: kept off until the "
                             "game is rebuilt (pipeline.sh, or Rebuild in the setup)\n", name.c_str());
    return false;
}

// register_hooks, before every other hook: put the code of the mods that are on into the dispatch tables.
void dissidia_xmods_install() {
    if (g_installed) return;
    g_installed = true;
    int n = 0;
    const DissidiaXmodSet* sets = dissidia_xmod_sets(&n);
    for (const std::string& root : dissidia_mod_roots()) {
        const size_t s = root.find_last_of('/');
        if (s == std::string::npos) continue;
        const std::string dir = root.substr(0, s), name = base_name(dir);
        const std::string js = read_text(dir + "/mod.json");
        if (!is_xdelta(js)) continue;
        const std::string stamp = json_str(js, "code_stamp");
        int fns = 0, mods = 0;
        for (int i = 0; i < n; i++) {
            if (name != sets[i].folder || stamp != sets[i].stamp) continue;
            const DissidiaXmodSet& set = sets[i];
            if (set.blob_size) {                    // waits for its run-time code (dissidia_xmod_tail_read)
                if (g_dyn_sets.size() < 64) {
                    const size_t di = g_dyn_sets.size();
                    g_dyn_sets.push_back(&set);
                    if (set.module >= 1 && set.module < kModules)
                        for (int k = 0; k < set.count; k++) g_ovl_dyn[set.module][set.fns[k].addr] = {set.fns[k].fn, &g_dyn_on[di]};
                }
                continue;
            }
            if (set.module < 0 || set.module >= kModules) continue;
            for (int k = 0; k < set.count; k++) {
                if (set.module == 0) psp_dispatch_register(set.fns[k].addr, set.fns[k].fn);
                else g_ovl[set.module][set.fns[k].addr] = set.fns[k].fn;
            }
            fns += set.count;
            mods++;
        }
        g_active.push_back(dir);
        std::fprintf(stderr, "[XMOD] %s: on (%d function(s) in %d module(s))\n", name.c_str(), fns, mods);
    }
}

namespace {
FuncPtr xmod_overlay_lookup(int ovl_id, uint32_t vaddr) {
    if (!g_ovl_dyn[ovl_id].empty()) {
        auto d = g_ovl_dyn[ovl_id].find(vaddr);
        if (d != g_ovl_dyn[ovl_id].end() && d->second.on->load(std::memory_order_acquire)) return d->second.fn;
    }
    const auto& t = g_ovl[ovl_id];
    if (t.empty()) return nullptr;
    auto it = t.find(vaddr);
    return it == t.end() ? nullptr : it->second;
}
}  // namespace

// The overlay function tables as the game module sees them (the resolver, and every hook that takes an overlay
// function's "original"): a mod's version first, so a runtime hook on a function the mod changed wraps the mod's code.
FuncPtr dissidia_ovl_battle_lookup(uint32_t vaddr) { FuncPtr f = xmod_overlay_lookup(1, vaddr); return f ? f : ovl_battle_lookup(vaddr); }
FuncPtr dissidia_ovl_menu_lookup(uint32_t vaddr) { FuncPtr f = xmod_overlay_lookup(2, vaddr); return f ? f : ovl_menu_lookup(vaddr); }
FuncPtr dissidia_ovl_extra_lookup(uint32_t vaddr) { FuncPtr f = xmod_overlay_lookup(3, vaddr); return f ? f : ovl_extra_lookup(vaddr); }

// on_boot_context: the EBOOT's data is in memory; write the bytes the mods changed.
void dissidia_xmods_boot_patch(uint8_t* rdram) {
    g_rdram = rdram;
    for (const std::string& dir : g_active) {
        const std::string p = read_text(dir + "/code/eboot.patch");
        size_t at = 0;
        int runs = 0;
        while (at + 8 <= p.size()) {
            uint32_t va, len;
            std::memcpy(&va, p.data() + at, 4);
            std::memcpy(&len, p.data() + at + 4, 4);
            at += 8;
            if (at + len > p.size()) break;
            std::memcpy(rdram + (va & 0x07FFFFFFu), p.data() + at, len);
            at += len;
            runs++;
        }
        if (runs) std::fprintf(stderr, "[XMOD] %s: %d EBOOT byte run(s) written\n", base_name(dir).c_str(), runs);
    }
}

// hooks_io.cpp: the file to read for overlay `ovl` (1..3) instead of the disc's, or "".
std::string dissidia_xmod_overlay_file(int ovl) {
    if (ovl < 1 || ovl >= kModules) return "";
    for (const std::string& dir : g_active) {
        const std::string p = dir + "/code/" + kOvlFile[ovl];
        if (is_file(p)) return p;
    }
    return "";
}

// hooks_io.cpp: data appended to PACKAGE.BIN by the first mod that is on and has some, or "".
std::string dissidia_xmod_package_tail() {
    std::string out;
    for (const std::string& root : dissidia_mod_roots()) {
        const size_t s = root.find_last_of('/');
        if (s == std::string::npos) continue;
        const std::string p = root.substr(0, s) + "/package_tail.bin";
        if (!is_file(p)) continue;
        if (out.empty()) { out = p; g_tail_dir = root.substr(0, s); }
        else std::fprintf(stderr, "[XMOD] %s also appends data to PACKAGE.BIN: only %s is used (turn one of them off)\n",
                          p.c_str(), out.c_str());
    }
    return out;
}

// hooks_io.cpp: a changed disc file (path relative to PSP_GAME/USRDIR, any case) from a mod that is on, or "".
std::string dissidia_xmod_usrdir_file(const std::string& rel_upper) {
    for (const std::string& root : dissidia_mod_roots()) {
        const size_t s = root.find_last_of('/');
        if (s == std::string::npos) continue;
        const std::string p = root.substr(0, s) + "/usrdir/" + rel_upper;
        if (is_file(p)) return p;
    }
    return "";
}

// ---------------------------------------------------------------------------------------------------------------------
// Code a mod loads while the game runs. hooks_io.cpp reports every read from package_tail.bin (where it went in guest
// memory). A call into such a range that this build has no compiled function for comes to the resolver
// (dissidia_overlay_resolver -> dissidia_xmod_dynamic_lookup) and runs in the interpreter (xmod_interp.cpp); when that
// first call returns, the code (as it is then: it may have rewritten itself) and the game's calls it redirected into
// itself are written to <mod>/code/dynamic.txt + dyn_<addr>.bin, and the next build compiles them (xdelta_code.py).
// A compiled set is switched on when its code is read to the same address again; each of its functions checks that
// memory still holds what it was compiled from and falls back to the interpreter where it does not.

bool dissidia_xmod_interpret(uint32_t pc, uint32_t lo, uint32_t hi, uint8_t* m, recomp_context* c);   // xmod_interp.cpp

namespace {

struct TailRead { uint64_t off; uint32_t size; uint32_t addr; bool learned; unsigned ovl_scanned; bool compiled; };
std::mutex g_dyn_mu;
std::vector<TailRead> g_reads;          // recent reads from package_tail.bin (newest last)
thread_local uint32_t tl_target = 0;
thread_local int tl_depth = 0;

constexpr uint32_t kEbootLo = 0x08804040u, kEbootHi = 0x08804040u + 0x1D9380u;   // the EBOOT's code segment
constexpr uint32_t kOvlLo = 0x089DD4C0u, kOvlHi = 0x08B63170u;

uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

const TailRead* read_at(uint32_t vaddr) {   // newest read that covers vaddr (caller holds g_dyn_mu)
    for (auto it = g_reads.rbegin(); it != g_reads.rend(); ++it)
        if (vaddr >= it->addr && vaddr < it->addr + it->size) return &*it;
    return nullptr;
}

// <mod>/code/dynamic.txt: "blob <tail offset> <size> <addr> <file>" and "write <module> <addr> <word>" lines.
// `overlay_only`: an overlay that was not resident at the first record is now: only its calls are added.
void learn(TailRead& r, bool overlay_only = false, bool rescan = false) {
    if (g_tail_dir.empty() || !g_rdram) return;
    if (!overlay_only && !rescan && r.learned) return;
    const bool first = !r.learned;
    r.learned = true;
    if (rescan) overlay_only = false;
    const std::string code = g_tail_dir + "/code";
    ::mkdir(code.c_str(), 0755);
    char name[64];
    std::snprintf(name, sizeof name, "dyn_%08X_%llX.bin", r.addr, (unsigned long long)r.off);
    if (first && !overlay_only) {
        std::ofstream f(code + "/" + name, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(g_rdram + (r.addr & 0x07FFFFFFu)), r.size);
    }
    // the game's calls (j / jal) that now lead into this code: the code rewrote them
    std::set<std::string> lines;
    {
        std::istringstream in(read_text(code + "/dynamic.txt"));
        std::string line;
        while (std::getline(in, line)) if (!line.empty()) lines.insert(line);
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "blob %llu %u %u %s", (unsigned long long)r.off, r.size, r.addr, name);
    // an older record of the same code (another address) is replaced
    for (auto it = lines.begin(); it != lines.end();) {
        unsigned long long o; unsigned sz;
        if (std::sscanf(it->c_str(), "blob %llu %u", &o, &sz) == 2 && o == r.off && sz == r.size) it = lines.erase(it); else ++it;
    }
    lines.insert(buf);
    int writes = 0;
    uint32_t ovl_id;
    std::memcpy(&ovl_id, g_rdram + (kOvlLo & 0x07FFFFFFu), 4);
    if (ovl_id >= 1 && ovl_id <= 3) r.ovl_scanned |= 1u << ovl_id;
    auto scan = [&](uint32_t lo, uint32_t hi, int module) {
        for (uint32_t a = lo; a < hi; a += 4) {
            uint32_t w;
            std::memcpy(&w, g_rdram + (a & 0x07FFFFFFu), 4);
            if ((w >> 26) != 2 && (w >> 26) != 3) continue;
            const uint32_t t = ((a + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
            if (t < r.addr || t >= r.addr + r.size) continue;
            std::snprintf(buf, sizeof buf, "write %d %u %u %llu %u", module, a, w, (unsigned long long)r.off, r.size);
            lines.insert(buf);
            writes++;
        }
    };
    const size_t before = lines.size();
    if (!overlay_only) scan(kEbootLo, kEbootHi, 0);
    if (ovl_id >= 1 && ovl_id <= 3) scan(kOvlLo + 0x10, kOvlHi, (int)ovl_id);
    if ((overlay_only || rescan) && lines.size() == before) return;   // nothing new
    std::ofstream f(code + "/dynamic.txt", std::ios::trunc);
    for (const std::string& l : lines) f << l << "\n";
    if (overlay_only || rescan) {
        std::fprintf(stderr, "[XMOD] %s: the code at 0x%08X rewrote %d more game instruction(s) (recorded; followed after the next build)\n",
                     base_name(g_tail_dir).c_str(), r.addr, (int)(lines.size() - before));
        return;
    }
    std::fprintf(stderr, "[XMOD] %s loads code at run time (%u B at 0x%08X, %d game call(s) redirected into it): run in the "
                         "interpreter now, compiled after the next build (recorded in %s/dynamic.txt)\n",
                 base_name(g_tail_dir).c_str(), r.size, r.addr, writes, code.c_str());
}

// a call into run-time code with an overlay resident that was not resident when the code was recorded: record the calls
// it redirects in that overlay too (the code may patch each overlay as it is loaded)
// while run-time code runs, look again every 2 s for game instructions it rewrote since (hooks set up on demand)
void maybe_rescan(uint32_t target) {
    static std::atomic<long long> next{0};
    const long long now = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long n = next.load();
    if (now < n || !next.compare_exchange_strong(n, now + 2000)) return;
    std::lock_guard<std::mutex> lk(g_dyn_mu);
    for (TailRead& r : g_reads)
        if (target >= r.addr && target < r.addr + r.size && r.learned) learn(r, false, true);
}

void rescan_overlay(uint32_t target) {
    if (!g_rdram) return;
    uint32_t ovl_id;
    std::memcpy(&ovl_id, g_rdram + (kOvlLo & 0x07FFFFFFu), 4);
    if (ovl_id < 1 || ovl_id > 3) return;
    std::lock_guard<std::mutex> lk(g_dyn_mu);
    for (TailRead& r : g_reads)
        if (target >= r.addr && target < r.addr + r.size && !(r.ovl_scanned & (1u << ovl_id))) {
            r.ovl_scanned |= 1u << ovl_id;
            if (r.learned) learn(r, true);
        }
}

void interp_thunk(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t target = tl_target;
    uint32_t lo = target, hi = target + 4;
    {
        std::lock_guard<std::mutex> lk(g_dyn_mu);
        if (const TailRead* r = read_at(target)) { lo = r->addr; hi = r->addr + r->size; }
    }
    static int trace = std::getenv("PSPRECOMP_XMOD_TRACE") ? std::atoi(std::getenv("PSPRECOMP_XMOD_TRACE")) : 0;
    if (trace > 0) { trace--; std::fprintf(stderr, "[XMOD_TRACE] interp 0x%08X a0=%08X a1=%08X a2=%08X sp=%08X\n", target, ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[29]); }
    tl_depth++;
    dissidia_xmod_interpret(target, lo, hi, rdram, ctx);
    tl_depth--;
    if (tl_depth == 0) {   // the game's call into the loaded code returned: record it once
        {
            std::lock_guard<std::mutex> lk(g_dyn_mu);
            for (TailRead& r : g_reads)
                if (target >= r.addr && target < r.addr + r.size) learn(r);
        }
        rescan_overlay(target);
        maybe_rescan(target);
    }
}

}  // namespace

// hooks_io.cpp: the game read [tail_off, tail_off+size) of package_tail.bin to guest address `addr`.
void dissidia_xmod_tail_read(uint64_t tail_off, uint32_t size, uint32_t addr) {
    if (!size || !addr) return;
    std::lock_guard<std::mutex> lk(g_dyn_mu);
    for (auto it = g_reads.begin(); it != g_reads.end();) {   // what this read overwrites is gone
        if (it->addr < addr + size && addr < it->addr + it->size) it = g_reads.erase(it); else ++it;
    }
    g_reads.push_back({tail_off, size, addr, false, 0u, false});
    if (g_reads.size() > 256) g_reads.erase(g_reads.begin());
    static const char* skip = std::getenv("PSPRECOMP_XMOD_DYN_SKIP");   // debugging: modules whose run-time sets stay off, e.g. "0,2"
    for (size_t i = 0; i < g_dyn_sets.size(); i++) {
        const DissidiaXmodSet& set = *g_dyn_sets[i];
        if (set.blob_off != tail_off || set.blob_size != size) continue;
        if (skip) {   // "4" = module 4 everywhere; "4@0B330000" = only the code read to that address
            char key[24];
            std::snprintf(key, sizeof key, "%d@%08X", set.module, set.blob_addr);
            const char* at = std::strstr(skip, key);
            bool any_at = false;
            for (const char* p = skip; (p = std::strchr(p, '@')); p++) if (p > skip && p[-1] == '0' + set.module) any_at = true;
            if (at || (!any_at && std::strchr(skip, '0' + set.module))) continue;
        }
        if (set.blob_addr != addr) {
            std::fprintf(stderr, "[XMOD] %s: its run-time code was read to 0x%08X, built for 0x%08X: interpreted (rebuild to compile it again)\n",
                         set.folder, addr, set.blob_addr);
            g_dyn_on[i].store(false);
            continue;
        }
        if (set.module >= 1 && set.module <= 3) g_reads.back().ovl_scanned |= 1u << set.module;
        if (set.module == 0 || set.module == kModBlob)
            for (int k = 0; k < set.count; k++) psp_dispatch_register(set.fns[k].addr, set.fns[k].fn);
        g_dyn_on[i].store(true, std::memory_order_release);
        g_reads.back().learned = true;        // compiled: recorded already
        g_reads.back().compiled = true;
        static int said = 0;
        if (said++ < 8)
            std::fprintf(stderr, "[XMOD] %s: run-time code at 0x%08X: %d compiled function(s) (module %d) on\n", set.folder, addr, set.count, set.module);
    }
}

// dissidia_overlay_resolver: an address inside code read from package_tail.bin that nothing compiled answers.
FuncPtr dissidia_xmod_dynamic_lookup(uint32_t vaddr) {
    if (g_tail_dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lk(g_dyn_mu);
    if (g_reads.empty() || !read_at(vaddr)) return nullptr;
    tl_target = vaddr;
    return interp_thunk;
}

// A compiled function of run-time code (generated wrappers in overlays/xmods/dispatch.cpp): runs it when memory still
// holds what it was compiled from, else interprets the code as it is now.
void dissidia_xmod_checked(uint32_t addr, uint32_t len, uint32_t crc, uint32_t blob_lo, uint32_t blob_hi,
                           void (*fn)(uint8_t*, recomp_context*), uint8_t* rdram, recomp_context* ctx) {
    {
        static thread_local uint32_t last_ovl = 0;
        uint32_t ovl_id;
        std::memcpy(&ovl_id, rdram + (kOvlLo & 0x07FFFFFFu), 4);
        if (ovl_id != last_ovl) { last_ovl = ovl_id; rescan_overlay(addr); }
        if (tl_depth == 0) maybe_rescan(addr);
    }
    static int trace = std::getenv("PSPRECOMP_XMOD_TRACE") ? std::atoi(std::getenv("PSPRECOMP_XMOD_TRACE")) : 0;
    if (trace > 0) { trace--; std::fprintf(stderr, "[XMOD_TRACE] compiled 0x%08X a0=%08X a1=%08X a2=%08X sp=%08X\n", addr, ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[29]); }
    if (crc32(rdram + (addr & 0x07FFFFFFu), len) == crc) { fn(rdram, ctx); return; }
    static std::mutex mu;
    static std::set<uint32_t> said;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (said.insert(addr).second && said.size() < 32)
            std::fprintf(stderr, "[XMOD] run-time code at 0x%08X changed since it was compiled: interpreted\n", addr);
    }
    dissidia_xmod_interpret(addr, blob_lo, blob_hi, rdram, ctx);
}

uint8_t* dissidia_xmod_rdram() { return g_rdram; }

bool dissidia_xmod_step(uint32_t op, uint32_t pc, uint8_t* m, recomp_context* c);   // xmod_interp.cpp

// A game instruction that run-time code of a mod rewrites (recorded in dynamic.txt): the compiled function checks the
// word in memory before it runs the instruction (xdelta_code.py puts the check there) and, when it is no longer the
// original, calls this. A j / jal is followed as the PSP would: its delay slot, then the jump. 1 = a call was made,
// continue after the delay slot; 2 = a jump was taken, the function is done; 0 = run the compiled instruction.
int dissidia_xmod_site(uint8_t* rdram, recomp_context* ctx, uint32_t site) {
    uint32_t w;
    std::memcpy(&w, rdram + (site & 0x07FFFFFFu), 4);
    const uint32_t op = w >> 26;
    if (op != 2 && op != 3) {
        static std::set<uint32_t> said;
        static std::mutex mu;
        std::lock_guard<std::mutex> lk(mu);
        if (said.insert(site).second && said.size() < 32)
            std::fprintf(stderr, "[XMOD] game code at 0x%08X was rewritten to 0x%08X (not a jump): the compiled instruction runs\n", site, w);
        return 0;
    }
    const uint32_t target = ((site + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
    uint32_t ds;
    std::memcpy(&ds, rdram + ((site + 4) & 0x07FFFFFFu), 4);
    if (!dissidia_xmod_step(ds, site + 4, rdram, ctx)) {
        static std::set<uint32_t> said;
        static std::mutex mu;
        std::lock_guard<std::mutex> lk(mu);
        if (said.insert(site).second && said.size() < 32)
            std::fprintf(stderr, "[XMOD] delay slot 0x%08X at 0x%08X of a rewritten jump cannot be run\n", ds, site + 4);
    }
    if (op == 3) {
        ctx->r[31] = (int32_t)(site + 8);
        RECOMP_LOOKUP(target)(rdram, ctx);
        return 1;
    }
    RECOMP_LOOKUP(target)(rdram, ctx);
    return 2;
}
