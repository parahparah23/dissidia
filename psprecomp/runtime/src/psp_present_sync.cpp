// Vertical sync, VRR and display refresh hints (issue #67). See include/psp_present_sync.h.
#include "psp_present_sync.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(PSPRECOMP_HAVE_VKGL)
#include "vk/vkgl.h"
#endif
#ifdef __ANDROID__
#include <android/native_window.h>
#include <dlfcn.h>
#endif

namespace {

int env_vsync() {
    const char* e = std::getenv("PSPRECOMP_VSYNC");
    return (e && *e && std::strcmp(e, "0") != 0) ? 1 : 0;   // unset or "0": off
}

std::atomic<int> g_vsync{-1};        // -1: not read from the environment yet
std::atomic<int> g_vsync_applied{-1};// what the presenting thread last applied (GL swap interval / Vulkan swapchain)
std::atomic<int> g_target_fps{0};

// ---- present timing statistics (PSPRECOMP_PRESENT_STATS=1)
using Clock = std::chrono::steady_clock;
bool g_stats = false, g_stats_init = false;
Clock::time_point g_last{}, g_window_t0{};
std::vector<float> g_iv;   // present-to-present intervals of the current window, ms

void stats_report(Clock::time_point now) {
    if (g_iv.size() < 10) return;
    std::vector<float> s = g_iv;
    std::sort(s.begin(), s.end());
    double sum = 0, sq = 0;
    for (float v : s) { sum += v; sq += double(v) * v; }
    const double mean = sum / s.size(), sd = std::sqrt(std::max(0.0, sq / s.size() - mean * mean));
    const float med = s[s.size() / 2], p99 = s[std::min(s.size() - 1, size_t(s.size() * 0.99))];
    int uneven = 0;   // a frame shown for more than 1.5x the typical frame time: a visible hitch
    for (float v : s) if (v > med * 1.5f) uneven++;
    const int tf = g_target_fps.load();
    std::fprintf(stderr, "[PRESENT_STATS] %zu presents in %.1f s: mean %.2f ms, sd %.2f, median %.2f, p99 %.2f, max %.2f, "
                 "uneven %d; vsync %s, target %d fps\n",
                 s.size(), std::chrono::duration<double>(now - g_window_t0).count(), mean, sd, med, p99, s.back(), uneven,
                 g_vsync_applied.load() == 1 ? "on" : "off", tf);
    g_iv.clear();
    g_window_t0 = now;
}

#ifdef __ANDROID__
// ANativeWindow_setFrameRate is API 30; the APK's minSdk is lower, so look it up at run time.
using SetFrameRateFn = int32_t (*)(ANativeWindow*, float, int8_t);
SetFrameRateFn set_frame_rate_fn() {
    // The NDK puts it in libnativewindow (libandroid only re-exports it on some builds); RTLD_DEFAULT does not search either
    // from the app's linker namespace (both found on the Android 15 emulator).
    static SetFrameRateFn fn = [] {
        for (const char* lib : {"libnativewindow.so", "libandroid.so"}) {
            void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
            if (void* f = h ? dlsym(h, "ANativeWindow_setFrameRate") : nullptr) return reinterpret_cast<SetFrameRateFn>(f);
            const char* e = dlerror();
            std::fprintf(stderr, "[PRESENT] %s: %s\n", lib, e ? e : "no ANativeWindow_setFrameRate");
        }
        return static_cast<SetFrameRateFn>(nullptr);
    }();
    return fn;
}
void android_frame_rate(SDL_Window* w) {
    static ANativeWindow* s_win = nullptr;
    static int s_fps = -1;
    const int fps = g_target_fps.load();
    if (fps <= 0 || !w) return;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_ANDROID) return;
    ANativeWindow* nw = info.info.android.window;
    if (!nw || (nw == s_win && fps == s_fps)) return;   // the surface is recreated after a resume: vote again then
    s_win = nw; s_fps = fps;
    if (SetFrameRateFn fn = set_frame_rate_fn()) {
        const int32_t r = fn(nw, float(fps), 0 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT */);
        std::fprintf(stderr, "[PRESENT] asked the display for %d Hz (ANativeWindow_setFrameRate -> %d)\n", fps, r);
    } else {
        std::fprintf(stderr, "[PRESENT] ANativeWindow_setFrameRate not found (it needs Android 11): display refresh left to the system\n");
    }
}
#endif

}  // namespace

int psp_vsync_wanted() {
    int v = g_vsync.load();
    if (v < 0) { v = env_vsync(); g_vsync.store(v); }
    return v;
}

void psp_vsync_set(int on) {
    on = on ? 1 : 0;
    if (g_vsync.exchange(on) != on) std::fprintf(stderr, "[PRESENT] vertical sync %s (applied at the next frame)\n", on ? "on" : "off");
}

void psp_vsync_apply_gl() {
    const int on = psp_vsync_wanted();
    int got = 0;
    if (on) {
        // Adaptive (late swaps tear instead of waiting a whole refresh) where the driver has it, else plain vsync.
        if (SDL_GL_SetSwapInterval(-1) == 0) got = -1;
        else if (SDL_GL_SetSwapInterval(1) == 0) got = 1;
    } else {
        SDL_GL_SetSwapInterval(0);
    }
    g_vsync_applied.store(on);
    std::fprintf(stderr, "[PRESENT] GL swap interval %d (vertical sync %s)\n", got, on ? "on" : "off");
}

void psp_present_set_target_fps(int fps) { g_target_fps.store(fps); }

void psp_present_before_swap(SDL_Window* w) {
    const int want = psp_vsync_wanted();
    if (g_vsync_applied.load() != want) {
#if defined(PSPRECOMP_HAVE_VKGL)
        if (vkgl::active()) {
            vkgl::set_swap_interval(want != 0);   // new swapchain (present mode) at this swap
            g_vsync_applied.store(want);
        } else
#endif
        psp_vsync_apply_gl();
    }
#ifdef __ANDROID__
    android_frame_rate(w);
#else
    (void)w;
#endif
}

void psp_present_after_swap() {
    if (!g_stats_init) {
        const char* e = std::getenv("PSPRECOMP_PRESENT_STATS");
        g_stats = e && *e && std::strcmp(e, "0") != 0;
        g_stats_init = true;
    }
    if (!g_stats) return;
    const auto now = Clock::now();
    if (g_last.time_since_epoch().count()) {
        const float ms = std::chrono::duration<float, std::milli>(now - g_last).count();
        if (ms < 1000.f) g_iv.push_back(ms);   // a loading pause is not a hitch
    } else {
        g_window_t0 = now;
    }
    g_last = now;
    if (now - g_window_t0 > std::chrono::seconds(5)) stats_report(now);
}
