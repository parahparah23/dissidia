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
#include "hle/psp_hle_imports.h"   // recomp_nid_stubs (generated syscall_table.cpp)
#include "recomp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>
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
FuncPtr dissidia_xmod_wrapped_overlay(int m, uint32_t vaddr);   // below: a wrapped (changed) overlay function
void scan_all_periodic();                                        // below: compare all game code with the compiled code
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
bool g_static_compiled_flag[4] = {};   // an active mod's compiled set covers module m (EBOOT, overlays)
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

extern const uint32_t* dissidia_xmod_functions(int module, int* count);   // generated (overlays/xmods/dispatch.cpp)

// hooks_io.cpp mod_roots(): may this folder be used? Not when it is an xdelta mod with code that this build lacks.
bool dissidia_xmod_folder_usable(const std::string& folder_dir, bool say) {
    const std::string js = read_text(folder_dir + "/mod.json");
    const bool tail = is_file(folder_dir + "/package_tail.bin");
    if (!is_xdelta(js) || (!has_code(js) && !tail)) return true;
    const std::string name = base_name(folder_dir);
    // ISO patches rewrite the whole disc: two that change code or append data cannot run together. The first one that
    // is on (folders in name order) is used; the others stay off with a note on the Mods page.
    static std::string chosen;
    if (!chosen.empty() && chosen != name) {
        std::ofstream st(folder_dir + "/status.txt", std::ios::trunc);
        st << "Not active: " << chosen << " is on too, and only one ISO patch mod that changes the game's code can run at a time. Turn one of them off.\n";
        if (say) std::fprintf(stderr, "[XMOD] %s: off, %s is on and changes the game's code too (only one can run)\n", name.c_str(), chosen.c_str());
        return false;
    }
    if (!has_code(js) || built_in(name, json_str(js, "code_stamp"))) {
        chosen = name;
        if (read_text(folder_dir + "/status.txt").compare(0, 10, "Not active") == 0)   // an old "not active" note is no longer true
            std::remove((folder_dir + "/status.txt").c_str());
        return true;
    }
    int fn = 0;
    dissidia_xmod_functions(0, &fn);
    if (fn) {   // this build follows changed code: the mod runs now, the functions it changes interpreted
        chosen = name;
        std::ofstream st(folder_dir + "/status.txt", std::ios::trunc);
        st << "On. The game code this mod changes runs in an interpreter until the game is rebuilt (Rebuild in the setup, or pipeline.sh), which makes it as fast as the rest.\n";
        if (say) std::fprintf(stderr, "[XMOD] %s: its code changes are not compiled in this build: interpreted until the next build\n", name.c_str());
        return true;
    }
    {
        std::ofstream st(folder_dir + "/status.txt", std::ios::trunc);
        st << "Not active yet: this mod changes the game's code, so the game has to be rebuilt once (Rebuild in the setup, or pipeline.sh).\n";
    }
    if (say)
        std::fprintf(stderr, "[XMOD] %s changes the game's code, which this build does not have yet: kept off until the "
                             "game is rebuilt (pipeline.sh, or Rebuild in the setup)\n", name.c_str());
    return false;
}

// register_hooks, before every other hook: put the code of the mods that are on into the dispatch tables.
bool g_capture = false;   // register_hooks is running: the hooks take their originals now

FuncPtr dissidia_xmod_probe_filter(uint32_t vaddr, FuncPtr fn);   // below

void dissidia_xmods_install() {
    if (g_installed) return;
    g_installed = true;
    static const bool nowatch = std::getenv("PSPRECOMP_XMOD_NOWATCH") != nullptr;   // debugging: compiled code only
    g_capture = !nowatch;
    if (g_capture) psp_dispatch_set_probe_filter(dissidia_xmod_probe_filter);
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
            g_static_compiled_flag[set.module] = true;
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
    if (FuncPtr w = dissidia_xmod_wrapped_overlay(ovl_id, vaddr)) return w;
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
FuncPtr dissidia_xmod_capture(int m, uint32_t vaddr, FuncPtr f);   // below: a hook's "original" while it installs
FuncPtr dissidia_ovl_battle_lookup(uint32_t vaddr) { FuncPtr f = xmod_overlay_lookup(1, vaddr); return dissidia_xmod_capture(1, vaddr, f ? f : ovl_battle_lookup(vaddr)); }
FuncPtr dissidia_ovl_menu_lookup(uint32_t vaddr) { FuncPtr f = xmod_overlay_lookup(2, vaddr); return dissidia_xmod_capture(2, vaddr, f ? f : ovl_menu_lookup(vaddr)); }
FuncPtr dissidia_ovl_extra_lookup(uint32_t vaddr) { FuncPtr f = xmod_overlay_lookup(3, vaddr); return dissidia_xmod_capture(3, vaddr, f ? f : ovl_extra_lookup(vaddr)); }

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
thread_local uint32_t tl_lo = 0, tl_hi = 0;   // the range the lookup chose for it (0 = the tail read's)
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

bool ref_word(int module, uint32_t a, uint32_t* w);   // below: the word the compiled code of `module` was made from
bool fn_of(int m, uint32_t a, uint32_t* start, uint32_t* size);   // below: the compiled function covering a

// <mod>/code/dynamic.txt: "blob <tail offset> <size> <addr> <file>" and "write <module> <addr> <word>" lines.
// `overlay_only`: an overlay that was not resident at the first record is now: only its calls are added.
void learn(TailRead& r, bool overlay_only = false, bool rescan = false) {
    if (g_tail_dir.empty() || !g_rdram) return;
    if (!overlay_only && !rescan && r.learned) return;
    const bool first = !r.learned;
    r.learned = true;
    if (rescan) overlay_only = false;
    const std::string code = g_tail_dir + "/code";
    { std::error_code ec; std::filesystem::create_directories(code, ec); }
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
            // into this code, or anywhere else when it replaced an instruction of a compiled function that was no jump (the code
            // also points game code at the patch's own code in an overlay: Emendatus's Aerith moves). Memory the runtime itself
            // writes (import stubs) is neither.
            uint32_t rw = 0, fs = 0, fz = 0;
            const bool rewritten = ref_word(module, a, &rw) && rw != w && (rw >> 26) != 2 && (rw >> 26) != 3 &&
                                   fn_of(module, a, &fs, &fz);
            if ((t < r.addr || t >= r.addr + r.size) && !rewritten) continue;
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
        std::ofstream st(g_tail_dir + "/status.txt", std::ios::trunc);
        st << "This mod changed more of the game's code while it ran: rebuild the game once more to follow it (Rebuild in the setup, or pipeline.sh).\n";
        std::fprintf(stderr, "[XMOD] %s: the code at 0x%08X rewrote %d more game instruction(s) (interpreted now, recorded for the next build)\n",
                     base_name(g_tail_dir).c_str(), r.addr, (int)(lines.size() - before));
        return;
    }
    {   // shown on the Settings > Mods page until the next build compiled it (xdelta_code.py removes the note)
        std::ofstream st(g_tail_dir + "/status.txt", std::ios::trunc);
        st << "This mod runs code it loads while the game runs: it works now, faster and completely after the game is rebuilt once (Rebuild in the setup, or pipeline.sh).\n";
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
    {
        std::lock_guard<std::mutex> lk(g_dyn_mu);
        for (TailRead& r : g_reads)
            if (target >= r.addr && target < r.addr + r.size && r.learned) learn(r, false, true);
    }
    scan_all_periodic();
}

void rescan_overlay(uint32_t target) {
    if (!g_rdram) return;
    uint32_t ovl_id;
    std::memcpy(&ovl_id, g_rdram + (kOvlLo & 0x07FFFFFFu), 4);
    if (ovl_id < 1 || ovl_id > 3) return;
    bool looked = false;
    {
        std::lock_guard<std::mutex> lk(g_dyn_mu);
        for (TailRead& r : g_reads)
            if (target >= r.addr && target < r.addr + r.size && !(r.ovl_scanned & (1u << ovl_id))) {
                r.ovl_scanned |= 1u << ovl_id;
                if (r.learned) { learn(r, true); looked = true; }
            }
    }
    // the code patches an overlay as it is loaded, not always with an instruction cache flush after: follow what it
    // rewrote now (wrap the changed functions), not only at the next build
    if (looked) scan_all_periodic();
}

void interp_thunk(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t target = tl_target;
    uint32_t lo = target, hi = target + 4;
    if (tl_hi) { lo = tl_lo; hi = tl_hi; tl_lo = tl_hi = 0; }
    else {
        std::lock_guard<std::mutex> lk(g_dyn_mu);
        if (const TailRead* r = read_at(target)) { lo = r->addr; hi = r->addr + r->size; }
    }
    static int trace = std::getenv("PSPRECOMP_XMOD_TRACE") ? std::atoi(std::getenv("PSPRECOMP_XMOD_TRACE")) : 0;
    if (trace > 0) { trace--; std::fprintf(stderr, "[XMOD_TRACE] interp 0x%08X a0=%08X a1=%08X a2=%08X sp=%08X\n", target, ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[29]); }
    tl_depth++;
    static const bool abi = std::getenv("PSPRECOMP_XMOD_ABI") != nullptr;
    const int32_t s0 = ctx->r[16], sp = ctx->r[29], ra0 = ctx->r[31];
    dissidia_xmod_interpret(target, lo, hi, rdram, ctx);
    if (abi && (s0 != ctx->r[16] || sp != ctx->r[29]))
        std::fprintf(stderr, "[XMOD_ABI] thunk 0x%08X [%08X,%08X) ra %08X: s0 %08X -> %08X sp %08X -> %08X\n", target, lo, hi,
                     (uint32_t)ra0, (uint32_t)s0, (uint32_t)ctx->r[16], (uint32_t)sp, (uint32_t)ctx->r[29]);
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
    tl_lo = tl_hi = 0;
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
    static const bool abi = std::getenv("PSPRECOMP_XMOD_ABI") != nullptr;
    if (abi) std::fprintf(stderr, "[XMOD_ABI] site 0x%08X -> 0x%08X ra %08X s0 %08X sp %08X\n", site, target, (uint32_t)ctx->r[31], (uint32_t)ctx->r[16], (uint32_t)ctx->r[29]);
    if (op == 3) ctx->r[31] = (int32_t)(site + 8);   // a jal links before its delay slot runs, as on the PSP
    if (!dissidia_xmod_step(ds, site + 4, rdram, ctx)) {
        static std::set<uint32_t> said;
        static std::mutex mu;
        std::lock_guard<std::mutex> lk(mu);
        if (said.insert(site).second && said.size() < 32)
            std::fprintf(stderr, "[XMOD] delay slot 0x%08X at 0x%08X of a rewritten jump cannot be run\n", ds, site + 4);
    }
    if (op == 3) {
        RECOMP_LOOKUP(target)(rdram, ctx);
        return 1;
    }
    RECOMP_LOOKUP(target)(rdram, ctx);
    return 2;
}

// ---------------------------------------------------------------------------------------------------------------------
// Changed game code without a rebuild. The compiled game is what the build saw: retail code, plus the functions an
// xdelta mod changed if the build had the mod (its static sets). Whatever else differs in memory (a mod installed after
// the last build, an instruction its run-time code rewrote) is found by comparing memory with the bytes the compiled
// code was made from ("reference"), at boot, when an overlay is loaded and whenever the game flushes the instruction
// cache (sceKernelIcacheInvalidateRange: what code must do after it writes code, so the plugins do). Each function with
// a difference gets a wrapper: while its memory matches the reference the compiled function runs, else the interpreter
// runs it from memory, i.e. what the PSP would run. Calls into changed code that no function covers (code a patch put
// in a former data area or after an overlay) are interpreted too. The next build compiles all of it.

extern const uint32_t* dissidia_xmod_functions(int module, int* count);       // generated: {start, size} pairs
extern const uint32_t* dissidia_xmod_guarded_sites(int module, int* count);   // generated: sites the build guards
extern const uint32_t* dissidia_xmod_mid_entries(int module, int* count);     // generated: {parent, entry}, by parent
uint32_t psp_hle_stub_addr_for_nid(uint32_t nid);
std::string dissidia_package_dir();                                            // hooks_io.cpp: .../USRDIR/DATA

namespace {

constexpr uint32_t kOvlFileHdr = 0x54;
const char* const kOvlName[4] = {"", "OVL_BATTLE_APP.ELF", "OVL_MENU_APP.ELF", "OVL_EXTRA_APP.ELF"};

struct Ref { std::vector<uint8_t> bytes; uint32_t base = 0; bool ready = false; };
Ref g_ref[kModules];                       // what each module's compiled code was made from
bool g_static_compiled[kModules] = {};     // an active mod's compiled set covers this module (reference = patched)
std::string g_ref_file[kModules];          // overlays: the file the reference comes from
bool g_watch = false;                      // any xdelta mod is on: compare code at all

std::mutex g_wrap_mu;
std::map<uint32_t, FuncPtr> g_wrap_ovl[kModules];   // overlay id -> function start -> wrapper
std::set<uint32_t> g_wrapped[kModules];
std::atomic<bool> g_wrap_any{false};
std::set<uint32_t> g_guarded[kModules];

struct FnTab { const uint32_t* p = nullptr; int n = 0; };
FnTab fntab(int m) { FnTab t; t.p = dissidia_xmod_functions(m, &t.n); return t; }

bool fn_of(int m, uint32_t a, uint32_t* start, uint32_t* size) {   // the function covering a (binary search)
    const FnTab t = fntab(m);
    int lo = 0, hi = t.n - 1, best = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (t.p[2 * mid] <= a) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0 || a >= t.p[2 * best] + t.p[2 * best + 1]) return false;
    *start = t.p[2 * best];
    *size = t.p[2 * best + 1];
    return true;
}

bool ref_word(int m, uint32_t a, uint32_t* w) {
    const Ref& r = g_ref[m];
    if (!r.ready || a < r.base || a + 4 > r.base + r.bytes.size()) return false;
    std::memcpy(w, r.bytes.data() + (a - r.base), 4);
    return true;
}

bool same_as_ref(int m, uint32_t start, uint32_t size) {
    const Ref& r = g_ref[m];
    if (!r.ready || start < r.base || start + size > r.base + r.bytes.size()) return true;
    return std::memcmp(g_rdram + (start & 0x07FFFFFFu), r.bytes.data() + (start - r.base), size) == 0;
}

int resident_overlay() {
    uint32_t id;
    std::memcpy(&id, g_rdram + (kOvlLo & 0x07FFFFFFu), 4);
    return id >= 1 && id <= 3 ? (int)id : 0;
}

std::map<uint32_t, int> g_captured[4];   // functions hooks took as their original: start -> slot (wrap() leaves them to it)

// wrapper slots: a wrapped function's start is dispatched to slot_thunk<i>
constexpr int kSlots = 16384;
struct Slot { uint32_t start, size; int module; FuncPtr orig; uint32_t entry; };   // entry: where it is entered (a mid-entry or start)
Slot g_slot[kSlots];
std::atomic<int> g_nslots{0};

bool g_force[kModules] = {};   // PSPRECOMP_XMOD_INTERP_ALL: every function of these modules interpreted (interpreter test)

void load_ovl_ref(int m);
void run_slot(int i, uint8_t* rdram, recomp_context* ctx) {
    const Slot& s = g_slot[i];
    if (s.module > 0 && !g_ref[s.module].ready) load_ovl_ref(s.module);
    if (s.orig && !g_force[s.module] && same_as_ref(s.module, s.start, s.size)) { s.orig(rdram, ctx); return; }
    // a mid-entry slot does not set ctx->entry_point: the interpreter simply starts at the entry
    static const bool abi = std::getenv("PSPRECOMP_XMOD_ABI") != nullptr;
    if (!abi) { dissidia_xmod_interpret(s.entry, s.start, s.start + s.size, rdram, ctx); return; }
    int32_t keep[10];
    for (int k = 0; k < 8; k++) keep[k] = ctx->r[16 + k];
    keep[8] = ctx->r[29]; keep[9] = ctx->r[30];
    const uint32_t ra = (uint32_t)ctx->r[31];
    dissidia_xmod_interpret(s.entry, s.start, s.start + s.size, rdram, ctx);
    for (int k = 0; k < 10; k++) {
        const int r = k < 8 ? 16 + k : (k == 8 ? 29 : 30);
        if (ctx->r[r] != keep[k])
            std::fprintf(stderr, "[XMOD_ABI] 0x%08X (entry 0x%08X, ra %08X): r%d %08X -> %08X\n", s.start, s.entry, ra, r,
                         (uint32_t)keep[k], (uint32_t)ctx->r[r]);
    }
}
template <int I> void slot_thunk(uint8_t* r, recomp_context* c) { run_slot(I, r, c); }
template <int... I> constexpr std::array<FuncPtr, sizeof...(I)> make_thunks(std::integer_sequence<int, I...>) {
    return {{&slot_thunk<I>...}};
}
const std::array<FuncPtr, kSlots> g_thunks = make_thunks(std::make_integer_sequence<int, kSlots>{});

FuncPtr ovl_table_lookup(int m, uint32_t a);   // below

// one dispatch entry -> slot; the compiled function there is looked up without the lock (the overlay lookup takes it)
void wrap_entry(int m, uint32_t start, uint32_t size, uint32_t entry) {
    FuncPtr orig = m == 0 ? psp_dispatch_probe_lookup(entry) : ovl_table_lookup(m, entry);
    std::lock_guard<std::mutex> lk(g_wrap_mu);
    const int i = g_nslots.fetch_add(1);
    if (i >= kSlots) {
        static bool said = false;
        if (!said) { said = true; std::fprintf(stderr, "[XMOD] more than %d changed functions: the rest run as compiled\n", kSlots); }
        return;
    }
    g_slot[i] = {start, size, m, orig, entry};
    if (m == 0) psp_dispatch_register(entry, g_thunks[i]);
    else { g_wrap_ovl[m][entry] = g_thunks[i]; g_wrap_any.store(true, std::memory_order_release); }
}

void wrap(int m, uint32_t start, uint32_t size, const char* why) {
    if (const char* nw = std::getenv("PSPRECOMP_XMOD_NOWRAP")) {   // debugging: functions (hex starts) never wrapped
        char key[16];
        std::snprintf(key, sizeof key, "%08X", start);
        if (std::strstr(nw, key)) return;
    }
    bool captured;
    {
        std::lock_guard<std::mutex> lk(g_wrap_mu);
        if (g_wrapped[m].count(start)) return;
        g_wrapped[m].insert(start);
        captured = g_captured[m].count(start) != 0;   // a runtime hook holds it: its original is a checking wrapper already
    }
    if (m == 0) {   // an import stub: the runtime implements the firmware call there, never interpret it
        static const std::set<uint32_t> stubs = [] {
            std::set<uint32_t> out;
            for (int i = 0; i < recomp_nid_stub_count; i++) out.insert(recomp_nid_stubs[i].stub_addr);
            return out;
        }();
        if (stubs.count(start)) return;
    }
    if (!captured) wrap_entry(m, start, size, start);
    // its mid-entries (the compiled function entered in its middle) too: same range, own entry
    int mn = 0;
    const uint32_t* mt = dissidia_xmod_mid_entries(m, &mn);
    int lo = 0, hi = mn;
    while (lo < hi) { const int mid = (lo + hi) / 2; if (mt[2 * mid] < start) lo = mid + 1; else hi = mid; }
    for (int k = lo; k < mn && mt[2 * k] == start; k++) wrap_entry(m, start, size, mt[2 * k + 1]);
    static int said = 0;
    static const bool verbose = std::getenv("PSPRECOMP_XMOD_VERBOSE") != nullptr;
    if (said++ < 24 || verbose)
        std::fprintf(stderr, "[XMOD] %s function 0x%08X (%u B%s) differs from the compiled code: interpreted while it does (%s)\n",
                     m == 0 ? "EBOOT" : kOvlName[m], start, size, captured ? ", under a runtime hook" : "", why);
}

// compares [lo, hi) of module m's memory with its reference and wraps every function with a difference
void scan(int m, uint32_t lo, uint32_t hi, const char* why) {
    static const char* noscan = std::getenv("PSPRECOMP_XMOD_NOSCAN");   // debugging: modules not compared, e.g. "02"
    if (noscan && std::strchr(noscan, '0' + m)) return;
    const Ref& r = g_ref[m];
    if (!r.ready || !g_rdram) return;
    lo = std::max(lo, r.base) & ~3u;
    hi = std::min<uint32_t>(hi, r.base + (uint32_t)r.bytes.size());
    uint32_t last_fn = 0;
    for (uint32_t a = lo; a < hi; a += 4096) {
        const uint32_t n = std::min<uint32_t>(4096, hi - a);
        const uint8_t* mem = g_rdram + (a & 0x07FFFFFFu);
        const uint8_t* ref = r.bytes.data() + (a - r.base);
        if (std::memcmp(mem, ref, n) == 0) continue;
        for (uint32_t o = 0; o + 4 <= n; o += 4) {
            if (std::memcmp(mem + o, ref + o, 4) == 0) continue;
            uint32_t fs, fz;
            if (!fn_of(m, a + o, &fs, &fz) || fs == last_fn) continue;
            if (g_guarded[m].count(a + o)) continue;          // the build compiled a check for this word
            last_fn = fs;
            wrap(m, fs, fz, why);
        }
    }
}

bool read_file(const std::string& p, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

// the reference of overlay m: the file its compiled code was made from (the mod's patched one if a build had it)
void load_ovl_ref(int m) {
    Ref& r = g_ref[m];
    if (r.ready) return;
    r.ready = true;
    std::string path;
    if (g_static_compiled[m]) path = dissidia_xmod_overlay_file(m);
    if (path.empty()) path = dissidia_package_dir() + "/" + kOvlName[m];
    std::vector<uint8_t> f;
    if (!read_file(path, &f) || f.size() <= kOvlFileHdr) { r.bytes.clear(); r.ready = false; return; }
    r.base = kOvlLo;
    r.bytes.assign(f.begin() + kOvlFileHdr, f.end());
    if (r.bytes.size() > kOvlHi - kOvlLo) r.bytes.resize(kOvlHi - kOvlLo);
    g_ref_file[m] = path;
}

void icache_hook(uint8_t* rdram, recomp_context* ctx);
FuncPtr g_icache_orig = nullptr;
uint32_t g_icache_stub = 0;

// all game code, whatever range the game flushed (a PSP's caches let code rewritten outside that range take effect too;
// Emendatus' menu plugin rewrites the EBOOT and flushes only the overlay window)
void scan_all(const char* why) {
    if (!g_watch) return;
    scan(0, kEbootLo, kEbootHi, why);
    const int ov = resident_overlay();
    if (ov) { load_ovl_ref(ov); scan(ov, kOvlLo, kOvlHi, why); }
}

void icache_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_icache_orig) g_icache_orig(rdram, ctx);
    scan_all("the game flushed the instruction cache after writing code");
}

FuncPtr g_dcache_orig = nullptr, g_dcache_orig2 = nullptr, g_icache_orig2 = nullptr;
void icache_hook2(uint8_t* rdram, recomp_context* ctx) {
    if (g_icache_orig2) g_icache_orig2(rdram, ctx);
    scan_all("the game flushed the instruction cache after writing code");
}
void dcache_hook2(uint8_t* rdram, recomp_context* ctx) {
    if (g_dcache_orig2) g_dcache_orig2(rdram, ctx);
    scan_all("the game wrote back the data cache after writing code");
}
void dcache_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_dcache_orig) g_dcache_orig(rdram, ctx);
    scan_all("the game wrote back the data cache after writing code");
}

}  // namespace

FuncPtr ovl_table_lookup_impl(int m, uint32_t a) {
    switch (m) {
        case 1: return dissidia_ovl_battle_lookup(a);
        case 2: return dissidia_ovl_menu_lookup(a);
        case 3: return dissidia_ovl_extra_lookup(a);
        default: return nullptr;
    }
}
namespace { FuncPtr ovl_table_lookup(int m, uint32_t a) { return ovl_table_lookup_impl(m, a); } }

// xmod_overlay_lookup (above) asks this first: a wrapped overlay function of the resident overlay
FuncPtr dissidia_xmod_wrapped_overlay(int m, uint32_t vaddr) {
    if (!g_wrap_any.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard<std::mutex> lk(g_wrap_mu);
    auto it = g_wrap_ovl[m].find(vaddr);
    return it == g_wrap_ovl[m].end() ? nullptr : it->second;
}

// on_boot_context, before the EBOOT patch is written: the reference of the EBOOT, and the cache-flush hook
void wrap_all(int m) {   // the interpreter test: every function of module m
    const FnTab t = fntab(m);
    for (int i = 0; i < t.n; i++) wrap(m, t.p[2 * i], t.p[2 * i + 1], "PSPRECOMP_XMOD_INTERP_ALL");
}

void dissidia_xmods_watch_begin(uint8_t* rdram) {
    g_rdram = rdram;
    g_capture = false;
    psp_dispatch_set_probe_filter(nullptr);
    g_watch = !g_active.empty() && !std::getenv("PSPRECOMP_XMOD_NOWATCH");
    if (const char* e = std::getenv("PSPRECOMP_XMOD_INTERP_ALL"))
        for (int m = 0; m < kModules; m++) if (std::strchr(e, '0' + m)) { g_force[m] = true; g_watch = true; }
    if (!g_watch) return;
    for (int m = 0; m < kModules; m++) g_static_compiled[m] = g_static_compiled_flag[m];
    for (int m = 0; m < kModules; m++) {
        int gn = 0;
        const uint32_t* gs = dissidia_xmod_guarded_sites(m, &gn);
        for (int i = 0; i < gn; i++) g_guarded[m].insert(gs[i]);
    }
    int fn = 0;
    dissidia_xmod_functions(0, &fn);
    if (!fn) {
        std::fprintf(stderr, "[XMOD] this build has no function tables (built before this version): changed code is only "
                             "followed after a rebuild\n");
        g_watch = false;
        return;
    }
    Ref& r = g_ref[0];
    r.base = kEbootLo;
    r.bytes.assign(rdram + (kEbootLo & 0x07FFFFFFu), rdram + (kEbootHi & 0x07FFFFFFu));
    r.ready = true;
    struct { uint32_t nid; FuncPtr* orig; FuncPtr hook; } flush[] = {
        {0xC2DF770Eu, &g_icache_orig, icache_hook},   // sceKernelIcacheInvalidateRange
        {0x920F104Au, &g_icache_orig2, icache_hook2},  // sceKernelIcacheInvalidateAll
        {0x79D1C3FAu, &g_dcache_orig, dcache_hook},    // sceKernelDcacheWritebackAll
        {0xB435DEC5u, &g_dcache_orig2, dcache_hook2},  // sceKernelDcacheWritebackInvalidateAll
    };
    for (auto& f : flush) {
        const uint32_t stub = psp_hle_stub_addr_for_nid(f.nid);
        if (!stub) continue;
        *f.orig = psp_dispatch_probe_lookup(stub);
        psp_dispatch_register(stub, f.hook);
    }
}

// on_boot_context, after the EBOOT patch: what the compiled EBOOT does not cover yet
void dissidia_xmods_watch_boot() {
    if (!g_watch) return;
    if (g_static_compiled[0]) {   // the compiled set was made from the patched EBOOT: that is the reference
        Ref& r = g_ref[0];
        r.bytes.assign(g_rdram + (kEbootLo & 0x07FFFFFFu), g_rdram + (kEbootHi & 0x07FFFFFFu));
    }
    scan(0, kEbootLo, kEbootHi, "the mod's EBOOT changes are not in this build");
    if (g_force[0]) wrap_all(0);
}

// dissidia_overlay_resolver saw a newly loaded overlay
void dissidia_xmods_overlay_loaded(int ovl) {
    if (!g_watch || ovl < 1 || ovl > 3) return;
    load_ovl_ref(ovl);
    scan(ovl, kOvlLo, kOvlHi, g_static_compiled[ovl] ? "changed after loading" : "the mod's overlay changes are not in this build");
    scan(0, kEbootLo, kEbootHi, "run-time code changed it");   // a mod's code patches the EBOOT when an overlay loads, too
    if (g_force[ovl]) wrap_all(ovl);
}

// framerate.cpp, every frame: about once a second compare all game code again. Run-time code of a mod rewrites game
// instructions whenever it likes, not always with a cache flush (Emendatus points two EBOOT calls at Aerith's movement code
// in the battle overlay at the start of a battle); comparing 2 MB costs well under a millisecond.
void dissidia_xmods_frame() {
    if (!g_watch) return;
    static unsigned n = 0;
    if (++n % 60) return;
    scan_all_periodic();
}

// a call into game code that nothing compiled answers (a patch's new code in a former data area or after an overlay's
// end, a jump back into the middle of a function it hooked): interpreted
FuncPtr dissidia_xmod_changed_code(uint32_t vaddr) {
    if (!g_watch || !g_rdram) return nullptr;
    int m = -1;
    if (vaddr >= kEbootLo && vaddr < kEbootHi) m = 0;
    else if (vaddr >= kOvlLo && vaddr < kOvlHi) { m = resident_overlay(); if (m) load_ovl_ref(m); }
    if (m < 0 || (m == 0 && vaddr < kEbootLo)) return nullptr;
    // nothing compiled answers this address (a jump into the middle of a function, typically back from a mod's hook,
    // or code a patch added): run what memory holds there
    if (vaddr & 3) return nullptr;
    uint32_t fs, fz;
    tl_target = vaddr;
    if (fn_of(m, vaddr, &fs, &fz)) { tl_lo = fs; tl_hi = fs + fz; }
    else { tl_lo = m == 0 ? kEbootLo : kOvlLo; tl_hi = m == 0 ? kEbootHi : kOvlHi; }   // new code: jumps within it stay here
    static int said = 0;
    static const bool verbose = std::getenv("PSPRECOMP_XMOD_VERBOSE") != nullptr;
    if (said++ < 16 || verbose) std::fprintf(stderr, "[XMOD] call to 0x%08X, which nothing compiled covers: interpreted\n", vaddr);
    return interp_thunk;
}

// While the hooks install (register_hooks), an overlay function a hook takes as its "original" gets a checking wrapper
// when an xdelta mod is on: the hook then runs the mod's version of that function (interpreted) whenever the overlay in
// memory holds one, as it would with the mod's code compiled in.
FuncPtr dissidia_xmod_capture(int m, uint32_t vaddr, FuncPtr f) {
    const bool force = std::getenv("PSPRECOMP_XMOD_INTERP_ALL") != nullptr;
    if (!g_capture || (g_active.empty() && !force) || !f || m < 0 || m > 3) return f;
    uint32_t fs, fz;
    if (!fn_of(m, vaddr, &fs, &fz) || fs != vaddr) return f;
    std::lock_guard<std::mutex> lk(g_wrap_mu);
    auto it = g_captured[m].find(fs);
    if (it != g_captured[m].end()) return g_thunks[it->second];
    const int i = g_nslots.fetch_add(1);
    if (i >= kSlots) return f;
    g_slot[i] = {fs, fz, m, f, fs};
    g_captured[m][fs] = i;
    return g_thunks[i];
}

// psp_dispatch_probe_lookup while the hooks install: an EBOOT function a hook takes gets a checking wrapper
FuncPtr dissidia_xmod_probe_filter(uint32_t vaddr, FuncPtr fn) {
    if (vaddr < kEbootLo || vaddr >= kEbootHi) return fn;
    return dissidia_xmod_capture(0, vaddr, fn);
}

void scan_all_periodic() { scan_all("run-time code changed it"); }
