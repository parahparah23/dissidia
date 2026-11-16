// games/dissidia/runtime/hooks_io.cpp — loose-file mods (PSPRECOMP_MODS).
//
// Installs the PspIoPolicy overlay-read seam (runtime/include/hle/psp_hle_io.h)
// so files dropped in <mods>/<asset path> replace entries of
// DATA/PACKAGE.BIN without rebuilding the ISO. Logic lives in package_mods.cpp.
//
// Why the IO level and not the EBOOT's lookup (FUN_08883538 -> 0x08883D0C):
// the game reads all of PACKAGE_INFO.BIN into guest memory at boot and then
// seeks/reads PACKAGE.BIN by (sector * 2048, size) taken from that index
// (sceIoLseek + sceIoReadAsync with exact entry sizes). Patching the bytes the
// game reads for PACKAGE_INFO.BIN therefore patches every consumer of the
// size/offset (the allocator sizing, the reader, the directory), and the
// redirect needs no knowledge of the engine's data structures.

#include "package_mods.h"
#include "psp_quit.h"
#include "psp_platform.h"
#include "psp_settings.h"
#include "hle/psp_hle_io.h"
#include "psp_fx_shader.h"
#include "psp_tex_pack.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

// roster.cpp: in-flight data patches of the overlay blobs (extra roster slots).
extern uint8_t* g_dbg_rdram;   // runtime/src/psp_ge.cpp (guest memory base once the GE thread started)
extern void psp_dump_func_ring(const char* tag);
extern bool dissidia_roster_enabled();
extern int dissidia_roster_overlay_of(const char* lower_path);
extern void dissidia_roster_patch_overlay(int mod, int64_t off, uint32_t size, uint8_t* dest);
std::string dissidia_mods_dir();
std::vector<std::string> dissidia_mod_roots();
extern uint32_t dissidia_model_budget_request(uint32_t bytes);
extern uint32_t dissidia_model_budget();

namespace {

enum { TAG_NONE = 0, TAG_INDEX = 1, TAG_PACKAGE = 2, TAG_OVL_BASE = 10 };   // TAG_OVL_BASE + roster module id


dissidia::PackageMods g_mods;
std::recursive_mutex g_mods_mu;   // g_mods can change while the game runs (net_packs.cpp adds a pack received online)
bool g_net_possible = false;      // packs may arrive online: keep PACKAGE.BIN tagged even with no mods (dissidia_mods_net_possible)
bool g_built = false;
bool g_active = false;
int g_trace_left = 0;
uint32_t g_index_capacity = dissidia::kIndexBufferBytes;   // raised by hooks_index.cpp once its hooks are installed

// ---- In-flight asset patches ------------------------------------------------
// A game module can edit one retail PACKAGE.BIN entry as the game reads it (dissidia_add_asset_patch): the entry is read
// once, the edit applied, and every PACKAGE.BIN read that overlaps the entry gets the edited bytes. An entry a loose-file
// mod replaces is left alone (the mod wins).
struct AssetPatch {
    std::string asset;
    std::function<bool(std::vector<uint8_t>&)> edit;
    int state = 0;            // 0 not prepared, 1 ready, -1 unavailable
    uint64_t off = 0;
    std::vector<uint8_t> bytes;
};
std::vector<AssetPatch>& asset_patches() { static std::vector<AssetPatch> v; return v; }
std::string g_data_dir;       // host folder of PACKAGE.BIN (set when the game first opens it)

void prepare_patch(AssetPatch& p) {
    if (p.state != 0) return;
    p.state = -1;
    if (g_data_dir.empty()) return;
    for (const auto& o : g_mods.overrides())
        if (o.asset == dissidia::normalize_asset_path(p.asset)) { std::fprintf(stderr, "[MODS] %s is replaced by a mod: built-in edit skipped\n", p.asset.c_str()); return; }
    uint32_t size = 0;
    if (!dissidia::asset_location(g_data_dir, p.asset, &p.off, &size)) return;
    if (!dissidia::read_asset(g_data_dir, "", p.asset, &p.bytes) || p.bytes.size() != size) return;
    if (!p.edit(p.bytes)) { std::fprintf(stderr, "[MODS] built-in edit of %s did not apply\n", p.asset.c_str()); return; }
    p.state = 1;
}

void apply_asset_patches(int64_t off, uint32_t n, uint8_t* dest) {
    for (AssetPatch& p : asset_patches()) {
        prepare_patch(p);
        if (p.state != 1) continue;
        const int64_t a = std::max<int64_t>(off, (int64_t)p.off), b = std::min<int64_t>(off + n, (int64_t)(p.off + p.bytes.size()));
        if (a < b) std::memcpy(dest + (a - off), p.bytes.data() + (a - (int64_t)p.off), size_t(b - a));
    }
}

// ---- Load trace -------------------------------------------------------------
// Always on and cheap: the last 32 PACKAGE.BIN reads (time, offset, size, the index entry's crc, and the mod
// asset when the read touched a loose file) are kept in a ring and printed when the game quits, so a load that
// never finishes (e.g. a new character hanging on the loading screen) can be traced: quit from the settings menu
// and the log ends with what the game read last. An override read that comes back short (the loose file is
// smaller than the entry the game asked for) is logged when it happens. PSPRECOMP_LOAD_TRACE=0 turns it off.
struct LoadRec {
    int64_t t_ms = 0;
    int64_t off = 0;
    uint32_t size = 0, got = 0, crc = 0;
    const dissidia::Override* ov = nullptr;
    bool served = false;   // served (partly) from a loose file
};
std::mutex g_lt_mu;
LoadRec g_lt[32];
uint64_t g_lt_total = 0;
int64_t g_lt_t0 = 0;
bool g_lt_on = true;
std::vector<std::pair<uint32_t, uint32_t>> g_by_sector;   // (sector, crc) of the patched index, by sector
int g_short_logs = 0;

int64_t lt_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t crc_at(int64_t off) {   // the index entry whose sectors contain `off` (caller holds g_lt_mu)
    if (g_by_sector.empty()) {
        dissidia::PackageIndex idx;
        std::string err;
        const auto& raw = g_mods.patched_index();
        if (raw.empty() || !idx.parse(raw.data(), raw.size(), &err)) return 0;
        g_by_sector.reserve(idx.entries.size());
        for (const auto& e : idx.entries) g_by_sector.emplace_back(e.sector, e.crc);
        std::sort(g_by_sector.begin(), g_by_sector.end());
    }
    const uint32_t sec = uint32_t(off / dissidia::kSectorSize);
    auto it = std::upper_bound(g_by_sector.begin(), g_by_sector.end(), std::make_pair(sec, 0xFFFFFFFFu));
    if (it == g_by_sector.begin()) return 0;
    return std::prev(it)->second;
}
const dissidia::Override* override_at(int64_t off, uint32_t size) {
    for (const auto& o : g_mods.overrides())
        if ((uint64_t)off < o.start + o.span && (uint64_t)off + size > o.start) return &o;
    return nullptr;
}
void load_trace_note(int64_t off, uint32_t size, uint32_t got, bool served) {
    if (!g_lt_on) return;
    std::lock_guard<std::mutex> lk(g_lt_mu);
    const int64_t t = lt_now_ms();
    if (!g_lt_t0) g_lt_t0 = t;
    LoadRec& r = g_lt[g_lt_total % 32];
    r = LoadRec{};
    r.t_ms = t; r.off = off; r.size = size; r.got = got; r.served = served;
    r.crc = crc_at(off);
    r.ov = served ? override_at(off, size) : nullptr;
    g_lt_total++;
    if (served && got < size && g_short_logs < 20) {
        g_short_logs++;
        std::fprintf(stderr, "[LOAD] short read from a mod file: asked %u B at 0x%llX, got %u B (%s)\n", size,
                     (unsigned long long)off, got, r.ov ? r.ov->file.c_str() : "?");
    }
}
void load_trace_dump() {
    if (!g_lt_on) return;
    std::lock_guard<std::mutex> lk(g_lt_mu);
    if (!g_lt_total) return;
    const int64_t now = lt_now_ms();
    const uint64_t n = std::min<uint64_t>(g_lt_total, 32);
    std::fprintf(stderr, "[LOAD] last %llu of %llu PACKAGE.BIN reads (newest last; the game quit %.1f s after the last one):\n",
                 (unsigned long long)n, (unsigned long long)g_lt_total,
                 (now - g_lt[(g_lt_total - 1) % 32].t_ms) / 1000.0);
    for (uint64_t i = g_lt_total - n; i < g_lt_total; i++) {
        const LoadRec& r = g_lt[i % 32];
        std::fprintf(stderr, "[LOAD]   t=%8.2fs  off=0x%09llX  size=%8u  crc=%08X  %s%s\n", (r.t_ms - g_lt_t0) / 1000.0,
                     (unsigned long long)r.off, r.size, r.crc,
                     r.served ? (r.ov ? ("MOD " + r.ov->asset).c_str() : "MOD") : "retail",
                     r.served && r.got < r.size ? "  SHORT" : "");
    }
}

bool ends_with_nocase(const std::string& s, const char* suf) {
    size_t n = std::char_traits<char>::length(suf);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != suf[i]) return false;
    return true;
}

std::string mods_dir() {
    if (const char* e = std::getenv("PSPRECOMP_MODS")) return e;
    std::string ms0 = psp_io_ms0_host_path();
    while (ms0.size() > 1 && ms0.back() == '/') ms0.pop_back();
    size_t slash = ms0.find_last_of('/');
    std::string parent = slash == std::string::npos ? "." : ms0.substr(0, slash);
    return parent + "/mods";
}

// Installed mod folders: <mods>/<folder>/mod.json + <folder>/install/ holding files at their game paths (obj/..., textures/...,
// by_crc/...). While the mod.json does not say "enabled": false, install/ is one more mods root: its files override the game's
// like loose files in <mods> do, its textures/ joins the texture pack. The settings menu's Mods page lists these folders and
// turns them on and off (applies at the next start). Loose files in <mods> itself win over a folder's.
bool mod_json_off(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const std::string js((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t k = js.find("\"enabled\"");
    if (k == std::string::npos) return false;
    size_t v = js.find(':', k);
    if (v == std::string::npos) return false;
    ++v;
    while (v < js.size() && std::isspace((unsigned char)js[v])) ++v;
    return js.compare(v, 5, "false") == 0;
}
std::vector<std::string> mod_roots() {
    static bool logged = false;            // asked from several places at start: say which folders are off once
    const bool say = !logged;
    logged = true;
    std::vector<std::string> out;
    const std::string dir = mods_dir();
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    std::vector<std::string> names;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.empty() || n[0] == '.' || n[0] == '_') continue;
        names.push_back(n);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    struct stat st;
    for (const std::string& n : names) {
        const std::string root = dir + "/" + n;
        if (::stat((root + "/mod.json").c_str(), &st) != 0 || ::stat((root + "/install").c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (mod_json_off(root + "/mod.json")) { if (say) std::fprintf(stderr, "[MODS] mod folder %s: off\n", n.c_str()); continue; }
        out.push_back(root + "/install");
    }
    return out;
}

// Voice language (settings "Voice", PSPRECOMP_VOICE=en|jp). `jp` serves the Japanese voice pack that
// tools/jp_audio.py extracted from an undub / Japanese disc: archive entries from <workdir>/voice_jp (loose
// files, scanned after the mods) and movies from <workdir>/movie_jp.
bool voice_jp_on() {
    const char* e = std::getenv("PSPRECOMP_VOICE");
    return e && (e[0] == 'j' || e[0] == 'J');
}
std::string work_parent() {
    std::string ms0 = psp_io_ms0_host_path();
    while (ms0.size() > 1 && ms0.back() == '/') ms0.pop_back();
    size_t slash = ms0.find_last_of('/');
    return slash == std::string::npos ? "." : ms0.substr(0, slash);
}
bool is_dir(const std::string& d) { struct stat st; return ::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

// DATA/MOVIE/X.PMF of the disc -> movie_jp/X.PMF when the voice is Japanese and that file exists
std::string remap_disc_path(const std::string& host) {
    if (!voice_jp_on() || !ends_with_nocase(host, ".pmf")) return "";
    const size_t slash = host.find_last_of('/');
    if (slash == std::string::npos) return "";
    std::string dirpart = host.substr(0, slash);
    for (char& c : dirpart) c = (char)std::tolower((unsigned char)c);
    if (dirpart.size() < 11 || dirpart.compare(dirpart.size() - 11, 11, "/data/movie") != 0) return "";
    std::string other = work_parent() + "/movie_jp/" + host.substr(slash + 1);
    struct stat st;
    return ::stat(other.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? other : "";
}

void ensure_built(const std::string& host_info_path) {
    if (g_built) return;
    g_built = true;
    std::string dir = mods_dir();
    std::vector<std::string> extra = mod_roots();   // installed mod folders that are on, then the voice pack
    for (const std::string& r : extra) std::fprintf(stderr, "[MODS] mod folder on: %s\n", r.c_str());
    if (voice_jp_on()) {
        const std::string vd = work_parent() + "/voice_jp";
        if (is_dir(vd)) { extra.push_back(vd); std::fprintf(stderr, "[VOICE] Japanese voice pack: %s\n", vd.c_str()); }
        else std::fprintf(stderr, "[VOICE] Japanese voice selected but %s does not exist (run tools/jp_audio.py): English voices\n", vd.c_str());
    }
    if (!is_dir(dir) && extra.empty() && !g_net_possible) {
        std::fprintf(stderr, "[MODS] no mods directory (%s): loose-file overrides off\n", dir.c_str());
        return;
    }
    std::string pkg_path = host_info_path.substr(0, host_info_path.size() - std::string("_INFO.BIN").size());
    // host paths: ".../PACKAGE_INFO.BIN" -> ".../PACKAGE.BIN" (same case as the INFO file)
    pkg_path += host_info_path.substr(host_info_path.size() - 4);
    struct stat pst;
    if (::stat(pkg_path.c_str(), &pst) != 0) {
        std::fprintf(stderr, "[MODS] cannot stat %s: overrides off\n", pkg_path.c_str());
        return;
    }
    std::ifstream f(host_info_path, std::ios::binary);
    std::vector<uint8_t> orig((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string log, err;
    if (!g_mods.build(orig, (uint64_t)pst.st_size, dir, &log, &err, g_index_capacity, extra)) {
        std::fprintf(stderr, "[MODS] cannot parse %s: %s (overrides off)\n", host_info_path.c_str(), err.c_str());
        return;
    }
    std::fputs(log.c_str(), stderr);
    std::fprintf(stderr, "[MODS] %s: %zu override(s) active\n", dir.c_str(), g_mods.overrides().size());
    g_active = !g_mods.empty();
    {   // model files are staged in a fixed block (hooks_models.cpp): raise it for the largest override
        uint32_t biggest = 0; std::string which;
        for (const auto& o : g_mods.overrides())
            if (o.asset.compare(0, 4, "obj/") == 0 && o.size > biggest) { biggest = o.size; which = o.asset; }
        const uint32_t retail = dissidia_model_budget();
        if (biggest > retail) {
            uint32_t b = dissidia_model_budget_request(biggest);
            std::fprintf(stderr, "[MODS] %s (%u B) exceeds the retail model budget (%u B): staging budget raised to %u B\n",
                         which.c_str(), biggest, retail, b);
        }
        for (const auto& o : g_mods.overrides())
            if (o.asset.compare(0, 4, "obj/") == 0 && o.size > dissidia_model_budget())
                std::fprintf(stderr, "[MODS] warning: %s (%u B) exceeds the game's model budget (%u B)\n", o.asset.c_str(), o.size, dissidia_model_budget());
    }
    if (const char* t = std::getenv("PSPRECOMP_MODS_TRACE")) g_trace_left = std::atoi(t);
}

int classify_open(const char* psp_path) {
    std::string p = psp_path ? psp_path : "";
    {   // always: the overlay code reads the relocated roster tables, which are filled from the overlay stream
        std::string lo = p;
        for (char& c : lo) c = (char)std::tolower((unsigned char)c);
        int m = dissidia_roster_overlay_of(lo.c_str());
        if (m >= 0) return TAG_OVL_BASE + m;
    }
    bool info = ends_with_nocase(p, "package_info.bin");
    bool pkg = !info && ends_with_nocase(p, "package.bin");
    if (!info && !pkg) return TAG_NONE;
    if (std::getenv("PSPRECOMP_MODS_TRACE")) std::fprintf(stderr, "[MODS] open %s\n", psp_path);
    if (!g_built) {
        std::string host = psp_path_to_host(info ? psp_path : (p.substr(0, p.size() - 4) + "_INFO.BIN").c_str());
        if (host.empty()) return TAG_NONE;
        ensure_built(host);
    }
    if (pkg && g_data_dir.empty()) {
        std::string host = psp_path_to_host(psp_path);
        const size_t slash = host.find_last_of('/');
        if (slash != std::string::npos) g_data_dir = host.substr(0, slash);
    }
    if (!g_active) return (pkg && (!asset_patches().empty() || g_net_possible)) ? TAG_PACKAGE : TAG_NONE;
    return info ? TAG_INDEX : TAG_PACKAGE;
}

bool overlay_read(int tag, int host_fd, int64_t off, uint32_t size, uint8_t* dest, uint32_t* nread) {
    if (tag >= TAG_OVL_BASE) {   // overlay blob: normal read, then patch roster slots in the buffer
        int64_t got = 0;
        while (got < (int64_t)size) {
            ssize_t r = psp_plat::pread_(host_fd, dest + got, size - got, off + got);
            if (r <= 0) break;
            got += r;
        }
        *nread = (uint32_t)got;
        dissidia_roster_patch_overlay(tag - TAG_OVL_BASE, off, (uint32_t)got, dest);
        return true;
    }
    auto base = [host_fd](uint8_t* d, uint32_t n, int64_t o) -> int64_t {
        int64_t got = 0;
        while (got < (int64_t)n) {
            ssize_t r = psp_plat::pread_(host_fd, d + got, n - got, o + got);
            if (r <= 0) break;
            got += r;
        }
        return got;
    };
    std::lock_guard<std::recursive_mutex> mods_lock(g_mods_mu);
    if (tag == TAG_PACKAGE && !asset_patches().empty() && !g_active) {   // no loose-file mods: plain read plus the built-in edits
        const int64_t got = base(dest, size, off);
        *nread = (uint32_t)std::max<int64_t>(got, 0);
        apply_asset_patches(off, *nread, dest);
        return true;
    }
    if (!g_active) return false;
    if (tag == TAG_INDEX) return g_mods.read_index(off, size, dest, nread);
    if (tag != TAG_PACKAGE) return false;
    static const bool pkg_trace = std::getenv("PSPRECOMP_PKG_TRACE") != nullptr;   // log every PACKAGE.BIN read
    if (pkg_trace) std::fprintf(stderr, "[PKG] off=0x%llX sector=%lld size=%u dest=0x%08X\n", (unsigned long long)off, (long long)(off / 2048), size,
                                  g_dbg_rdram ? (uint32_t)(dest - g_dbg_rdram) + 0x08000000u : 0u);
    if (pkg_trace && size >= 0x100000) psp_dump_func_ring("PKG-RING");
    bool hit = g_mods.read_package(off, size, dest, nread, base);
    if (!asset_patches().empty()) {   // the built-in edits apply to retail entries, so the read is done here when no mod touched it
        if (!hit) { const int64_t got = base(dest, size, off); *nread = (uint32_t)std::max<int64_t>(got, 0); hit = true; }
        apply_asset_patches(off, *nread, dest);
    }
    load_trace_note(off, size, hit ? *nread : size, hit);
    if (hit && g_trace_left > 0) {
        g_trace_left--;
        std::fprintf(stderr, "[MODS] read off=0x%llX size=%u -> %u B served via overrides dest=0x%08X\n",
                     (unsigned long long)off, size, *nread, g_dbg_rdram ? (uint32_t)(dest - g_dbg_rdram) + 0x08000000u : 0u);
    }
    return hit;
}

}  // namespace

std::string dissidia_mods_dir() { return mods_dir(); }
std::vector<std::string> dissidia_mod_roots() { return mod_roots(); }
// net_packs.cpp, at registration (before the game opens PACKAGE.BIN): packs may arrive online, so PACKAGE.BIN reads go
// through the override layer even when no mod is installed (it serves nothing until a pack is added).
void dissidia_mods_net_possible() { g_net_possible = true; }
// net_packs.cpp, game thread: serve `file` for `asset` from now on (a verified pack received online). The entry moves to
// fresh virtual sectors past every other override, in the patched index and in the game's in-memory copy.
extern bool dissidia_index_set_entry(uint8_t* rdram, uint32_t crc, uint32_t sector, uint32_t size_flags);
bool dissidia_mods_add_override(uint8_t* rdram, const std::string& asset, const std::string& file, uint32_t size) {
    std::lock_guard<std::recursive_mutex> lk(g_mods_mu);
    if (!g_built) return false;
    dissidia::IndexEntry e;
    if (!g_mods.add_override(asset, file, size, &e)) return false;
    if (!dissidia_index_set_entry(rdram, e.crc, e.sector, e.size_flags)) return false;
    g_active = true;
    { std::lock_guard<std::mutex> l2(g_lt_mu); g_by_sector.clear(); }
    std::fprintf(stderr, "[MODS] online pack: %s now served from %s (%u B, sector %u)\n", asset.c_str(), file.c_str(), size, e.sector);
    return true;
}
std::string dissidia_package_dir() { return g_data_dir; }
void dissidia_add_asset_patch(const std::string& asset, std::function<bool(std::vector<uint8_t>&)> edit) {
    AssetPatch p;
    p.asset = asset;
    p.edit = std::move(edit);
    asset_patches().push_back(std::move(p));
}

// hooks_index.cpp: called by the index loader hook before the game reads PACKAGE_INFO.BIN. Builds the patched
// index (once) and returns its size in bytes, or 0 when no loose-file mods are active.
uint32_t dissidia_package_index_bytes(const std::string& psp_info_path) {
    if (!g_built) {
        std::string host = psp_path_to_host(psp_info_path.c_str());
        if (host.empty()) return 0;
        ensure_built(host);
    }
    return g_active ? (uint32_t)g_mods.patched_index().size() : 0;
}
void dissidia_package_index_extend(bool on) { g_index_capacity = on ? dissidia::kIndexExtendedBytes : dissidia::kIndexBufferBytes; }

void dissidia_install_mod_io() {
    if (const char* e = std::getenv("PSPRECOMP_LOAD_TRACE")) g_lt_on = !(e[0] == '0');
    if (g_lt_on) { std::atexit(load_trace_dump); psp_on_quit(load_trace_dump); }
    PspIoPolicy policy{};
    policy.classify_open = classify_open;
    policy.overlay_read = overlay_read;
    policy.remap_disc_path = remap_disc_path;
    psp_io_set_policy(policy);
    // The settings menu offers a Voice row once a Japanese voice pack has been extracted (tools/jp_audio.py).
    psp_settings_set_voice_hook({+[]() { return is_dir(work_parent() + "/voice_jp"); }});
    psp_settings_set_texpack_hook({+[]() {
        if (is_dir(mods_dir() + "/textures")) return true;
        for (const std::string& r : mod_roots()) if (is_dir(r + "/textures")) return true;
        return false;
    }});
    // Texture packs (runtime/include/psp_tex_pack.h) live under <mods>/textures, and the textures/ of each installed mod folder that is on.
    psp_texpack_set_dir_provider([]() -> std::string { return mods_dir() + "/textures"; });
    psp_texpack_set_extra_dirs_provider([]() -> std::vector<std::string> {
        std::vector<std::string> v;
        for (const std::string& r : mod_roots()) if (is_dir(r + "/textures")) v.push_back(r + "/textures");
        return v;
    });
    // FX shader overrides (runtime/include/psp_fx_shader.h, FX_SHADERS.md) live under <mods>/fx_shaders.
    psp_fx_set_dir_provider([]() -> std::string { return mods_dir() + "/fx_shaders"; });
}
