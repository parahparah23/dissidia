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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

// layout shared with the generated <work>/overlays/xmods/dispatch.cpp (tools/xdelta_code.py write_set)
struct DissidiaXmodFn { uint32_t addr; void (*fn)(uint8_t*, recomp_context*); };
struct DissidiaXmodSet { const char* folder; const char* stamp; int module; const DissidiaXmodFn* fns; int count; };
extern const DissidiaXmodSet* dissidia_xmod_sets(int* count);
extern FuncPtr ovl_battle_lookup(uint32_t vaddr);    // the generated overlay sets (build_overlay.py)
extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_extra_lookup(uint32_t vaddr);

std::string dissidia_mods_dir();                       // hooks_io.cpp
std::vector<std::string> dissidia_mod_roots();         // hooks_io.cpp: <mods>/<folder>/install of the folders that are on

namespace {

constexpr int kModules = 4;
const char* const kOvlFile[kModules] = {"EBOOT.BIN", "OVL_BATTLE_APP.ELF", "OVL_MENU_APP.ELF", "OVL_EXTRA_APP.ELF"};

std::map<uint32_t, FuncPtr> g_ovl[kModules];           // overlay id -> vaddr -> the mod's function
std::vector<std::string> g_active;                     // folders (full paths) whose code is installed
bool g_installed = false;

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
        if (out.empty()) out = p;
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
