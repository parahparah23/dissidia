// Tag team test builds: a screenshot of the whole window (the final image with the HUD) at every Bravery Break and at an extra's BRV-0 state,
// so a device run shows what the player saw (docs/tag-team.md, "Break screenshots"). The game thread only schedules; a worker thread asks the render
// thread's window capture service (ge_draw_capture_window, the debug socket 'V' path: a TGA written on the render thread at its next present),
// then converts the TGA to PNG (zlib) and deletes it. Files: <game dir>/breakshots/<battle#>_<battle age s>_<role>_<char>_<kind>_<n>.png.
// Default on in tag battles; PSPRECOMP_TAGTEAM_BREAKSHOT=0 turns it off. At most 60 shots per run; none while the window is minimised / hidden.
#include <filesystem>
#include "psp_ge_draw.h"
#include "psp_event_loop.h"
#include "hle/psp_hle_io.h"
#include <SDL.h>
#include <zlib.h>
#include <sys/stat.h>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int kMaxShots = 60;
const float kOffsets[3] = {0.0f, 0.5f, 1.5f};   // seconds after the event: the event, the panel animation
struct Due { float at; std::string path, what; };
std::vector<Due> g_due;                          // game thread only
int g_scheduled = 0;                             // shots scheduled this run (the cap)
std::mutex g_mx; std::condition_variable g_cv; std::deque<Due> g_q; bool g_worker = false;

bool enabled() { static const char* e = std::getenv("PSPRECOMP_TAGTEAM_BREAKSHOT"); return !(e && e[0] == '0'); }

std::string shot_dir() {
    std::string ms0 = psp_io_ms0_host_path();
    while (ms0.size() > 1 && ms0.back() == '/') ms0.pop_back();
    const size_t slash = ms0.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : ms0.substr(0, slash)) + "/breakshots";
}

void put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> b; put32(b, static_cast<uint32_t>(data.size())); b.insert(b.end(), type, type + 4); b.insert(b.end(), data.begin(), data.end());
    const uint32_t crc = static_cast<uint32_t>(crc32(crc32(0L, Z_NULL, 0), b.data() + 4, static_cast<uInt>(b.size() - 4)));
    put32(b, crc); std::fwrite(b.data(), 1, b.size(), f);
}
// The capture service's TGA (18-byte header, 32 bpp BGRA, top-left origin) -> an RGB PNG (filter "Sub" per row).
bool tga_to_png(const std::string& tga, const std::string& png) {
    FILE* f = std::fopen(tga.c_str(), "rb"); if (!f) return false;
    uint8_t h[18]; if (std::fread(h, 1, 18, f) != 18 || h[2] != 2 || h[16] != 32) { std::fclose(f); return false; }
    const int w = h[12] | (h[13] << 8), ht = h[14] | (h[15] << 8);
    std::vector<uint8_t> px(size_t(w) * ht * 4); const bool rd_ok = std::fread(px.data(), 1, px.size(), f) == px.size(); std::fclose(f);
    if (!rd_ok || w <= 0 || ht <= 0) return false;
    const bool bottom_up = (h[17] & 0x20) == 0;
    std::vector<uint8_t> raw(size_t(ht) * (size_t(w) * 3 + 1));
    for (int y = 0; y < ht; y++) {
        const uint8_t* s = px.data() + size_t(bottom_up ? ht - 1 - y : y) * w * 4; uint8_t* d = raw.data() + size_t(y) * (size_t(w) * 3 + 1);
        d[0] = 1; uint8_t pr = 0, pg = 0, pb = 0;
        for (int x = 0; x < w; x++) { const uint8_t r = s[x * 4 + 2], g = s[x * 4 + 1], b = s[x * 4 + 0]; d[1 + x * 3] = uint8_t(r - pr); d[2 + x * 3] = uint8_t(g - pg); d[3 + x * 3] = uint8_t(b - pb); pr = r; pg = g; pb = b; }
    }
    uLongf zl = compressBound(static_cast<uLong>(raw.size())); std::vector<uint8_t> z(zl);
    if (compress2(z.data(), &zl, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return false;
    z.resize(zl);
    FILE* o = std::fopen(png.c_str(), "wb"); if (!o) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}; std::fwrite(sig, 1, 8, o);
    std::vector<uint8_t> ihdr; put32(ihdr, uint32_t(w)); put32(ihdr, uint32_t(ht)); ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(o, "IHDR", ihdr); chunk(o, "IDAT", z); chunk(o, "IEND", {});
    const bool good = std::ferror(o) == 0; std::fclose(o); return good;
}

void worker() {
    for (;;) {
        Due d;
        { std::unique_lock<std::mutex> lk(g_mx); g_cv.wait(lk, [] { return !g_q.empty(); }); d = g_q.front(); g_q.pop_front(); }
        SDL_Window* win = psp_get_sdl_window();
        if (!win || (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))) { std::fprintf(stderr, "[BREAKSHOT] %s: skipped (window minimised)\n", d.what.c_str()); continue; }
        const std::string tga = d.path + ".tga";
        if (!ge_draw_capture_window(tga.c_str(), 3000)) { std::fprintf(stderr, "[BREAKSHOT] %s: capture failed (busy or no frame)\n", d.what.c_str()); std::remove(tga.c_str()); continue; }
        const bool ok = tga_to_png(tga, d.path); std::remove(tga.c_str());
        struct stat st {}; const long sz = (ok && stat(d.path.c_str(), &st) == 0) ? static_cast<long>(st.st_size) : -1;
        if (ok) std::fprintf(stderr, "[BREAKSHOT] %s -> %s (%ld bytes)\n", d.what.c_str(), d.path.c_str(), sz);
        else std::fprintf(stderr, "[BREAKSHOT] %s: PNG write failed (%s)\n", d.what.c_str(), d.path.c_str());
    }
}
}  // namespace

// An event (break / an extra's BRV 0): three shots at age, +0.5 s, +1.5 s. age_s = battle age in seconds.
void tagteam_breakshot_event(float age_s, unsigned battle_no, const char* role, int chr, const char* kind, const char* brv) {
    if (!enabled() || g_scheduled >= kMaxShots) return;
    const std::string dir = shot_dir();
    { static std::string made; if (made != dir) { std::error_code mk_ec; std::filesystem::create_directories(dir, mk_ec); made = dir; } }
    for (int n = 0; n < 3 && g_scheduled < kMaxShots; n++, g_scheduled++) {
        char name[160]; std::snprintf(name, sizeof name, "/%u_%07.2f_%s_%d_%s_%d.png", battle_no, age_s, role, chr, kind, n);
        char what[160]; std::snprintf(what, sizeof what, "t=%.2f %s %d %s #%d", age_s + kOffsets[n], role, chr, kind, n);
        g_due.push_back({age_s + kOffsets[n], dir + name, what});
    }
    std::fprintf(stderr, "[BREAKSHOT] t=%.2f %s char %d %s: 3 shots scheduled (%d of %d this run) {%s}\n", age_s, role, chr, kind, g_scheduled, kMaxShots, brv ? brv : "");
}
// Every game frame: hand the due shots to the worker (never blocks the game thread). age_s < 0: no battle (pending shots dropped).
void tagteam_breakshot_frame(float age_s) {
    if (g_due.empty()) return;
    if (age_s < 0.f) { g_due.clear(); return; }
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_worker) { g_worker = true; std::thread(worker).detach(); }
    for (size_t i = 0; i < g_due.size();) if (g_due[i].at - age_s > 5.f) g_due.erase(g_due.begin() + static_cast<long>(i));   // a new battle started (the age went back)
        else if (age_s >= g_due[i].at) { g_q.push_back(g_due[i]); g_due.erase(g_due.begin() + static_cast<long>(i)); } else i++;
    g_cv.notify_one();
}
