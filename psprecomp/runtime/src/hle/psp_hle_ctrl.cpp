#include "hle/psp_hle.h"
#include "psp_runtime.h"
#include "psp_scheduler.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>

// ---- Right stick (host pad, or debug-socket `A2` injection) ----
static void read_right_raw(int* rx, int* ry) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint32_t v = (now_ms < g_injected_ranalog_deadline_ms.load(std::memory_order_relaxed))
        ? g_injected_ranalog.load(std::memory_order_relaxed)
        : g_host_ranalog.load(std::memory_order_relaxed);
    *rx = static_cast<int>(v & 0xFF) - 128;
    *ry = static_cast<int>((v >> 8) & 0xFF) - 128;
}

// Radial dead zone (PSPRECOMP_RSTICK_DEADZONE, default 0.20 of full travel),
// rescaled so the output starts at 0 at the edge of the dead zone.
bool psp_ctrl_right_stick(float* x, float* y) {
    static const float dz = [] {
        const char* e = std::getenv("PSPRECOMP_RSTICK_DEADZONE");
        float d = e ? static_cast<float>(std::atof(e)) : 0.20f;
        return d < 0.f ? 0.f : d > 0.9f ? 0.9f : d;
    }();
    int rx, ry; read_right_raw(&rx, &ry);
    float fx = rx / 127.f, fy = -ry / 127.f;   // y: up positive
    float m = std::sqrt(fx * fx + fy * fy);
    if (m <= dz) { *x = *y = 0.f; return false; }
    float k = (m > 1.f ? 1.f : m);
    k = (k - dz) / (1.f - dz) / m;
    *x = fx * k; *y = fy * k;
    return true;
}

// Right stick -> D-pad bits: ON by default (a pad is present or injected);
// PSPRECOMP_RSTICK_DPAD=0 turns it off. Per-axis threshold 55% with a release
// at 40% (hysteresis), independent of the camera dead zone.
static uint32_t right_stick_dpad_bits() {
    static const bool on = [] {
        const char* e = std::getenv("PSPRECOMP_RSTICK_DPAD");
        return !(e && e[0] == '0');
    }();
    static uint32_t held = 0;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!on || now_ms < g_rstick_consumed_until_ms.load(std::memory_order_relaxed)) { held = 0; return 0; }
    int rx, ry; read_right_raw(&rx, &ry);
    auto upd = [&](int v, uint32_t lo, uint32_t hi) {
        int thr_on = 70, thr_off = 51;   // of 127
        uint32_t cur = held & (lo | hi);
        int th = cur ? thr_off : thr_on;
        held &= ~(lo | hi);
        if (v < -th) held |= lo; else if (v > th) held |= hi;
    };
    upd(rx, 0x0080, 0x0020);   // LEFT / RIGHT
    upd(ry, 0x0010, 0x0040);   // UP / DOWN (SDL y down == PSP y down)
    return held;
}

// ---- SceCtrlData layout (PSP SDK) ----
// offset 0: uint32_t timestamp
// offset 4: uint32_t buttons
// offset 8: uint8_t  analog_x  (128 = center)
// offset 9: uint8_t  analog_y  (128 = center)
// offset 10-15: reserved
// Total: 16 bytes per sample

static std::atomic<bool> g_script_on{false};
static std::atomic<uint32_t> g_script_buttons{0}, g_script_analog{0x8080}, g_last_buttons{0}, g_last_analog{0x8080};
void psp_ctrl_set_script(bool on, uint32_t buttons, uint32_t analog) {
    g_script_buttons.store(buttons, std::memory_order_relaxed);
    g_script_analog.store(analog, std::memory_order_relaxed);
    g_script_on.store(on, std::memory_order_relaxed);
}
static std::atomic<uint32_t (*)(uint32_t)> g_button_filter{nullptr};
void psp_ctrl_set_filter(uint32_t (*filter)(uint32_t buttons)) { g_button_filter.store(filter, std::memory_order_relaxed); }
void psp_ctrl_last_sample(uint32_t* buttons, uint32_t* analog) {
    *buttons = g_last_buttons.load(std::memory_order_relaxed);
    *analog = g_last_analog.load(std::memory_order_relaxed);
}


// ---- Button chord window (Settings > Controls > Button Chord Window, PSPRECOMP_CHORD_MS) ----
// A chord (R + Cross dodge, R + Triangle dash, R + Square EX, R + Circle summon, L + Circle / L + Square assists) is pressed by a person
// within a few frames, and the face button often lands first. The PSP read the pad once per 60 Hz frame with the same spread, but a face
// button that arrives alone is a different move (Cross = jump / Chase start). A face button that goes down with no L / R held is held back
// for the window: if L or R comes down in it both are handed over in one read, else the button is handed over late by the window (a tap
// shorter than the window is still handed over, for 34 ms). Applied before the game's read, so what an online battle sends is what runs.
static std::atomic<int> g_chord_ms{-1};   // -1: not read from the environment yet
static std::atomic<bool (*)()> g_chord_gate{nullptr};
void psp_ctrl_set_chord_ms(int ms) { g_chord_ms.store(ms < 0 ? 0 : ms > 200 ? 200 : ms, std::memory_order_relaxed); }
int psp_ctrl_chord_ms() {
    int v = g_chord_ms.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = std::getenv("PSPRECOMP_CHORD_MS");
        v = (e && e[0]) ? std::atoi(e) : 70;
        psp_ctrl_set_chord_ms(v);
        v = g_chord_ms.load(std::memory_order_relaxed);
    }
    return v;
}
void psp_ctrl_set_chord_gate(bool (*gate)()) { g_chord_gate.store(gate, std::memory_order_relaxed); }

// [CHORD] log: PSPRECOMP_CHORD_LOG=1, or on with PSPRECOMP_TAGTEAM_LOG=1. Every change of the face / L / R bits of the raw pad or of what the game is
// handed, with the time in ms, rate limited (20 lines a second, 3000 in all) so a play session shows when each real press and release came.
static void chord_log(uint32_t raw, uint32_t out, bool held_back, bool window_on, int64_t now_us) {
    static const bool lg = std::getenv("PSPRECOMP_CHORD_LOG") || std::getenv("PSPRECOMP_TAGTEAM_LOG");
    if (!lg) return;
    static const uint32_t M = 0xF300u;   // face buttons, L, R
    static uint32_t lraw = 0, lout = 0; static int total = 0, in_sec = 0; static int64_t sec0 = 0;
    if ((raw & M) == (lraw & M) && (out & M) == (lout & M)) return;
    lraw = raw; lout = out;
    if (now_us - sec0 >= 1000000) { sec0 = now_us; in_sec = 0; }
    static const int per_sec = std::getenv("PSPRECOMP_CHORD_LOG") ? 400 : 20;   // an explicit CHORD_LOG keeps (nearly) every line, for mash tests
    if (in_sec >= per_sec || total >= (std::getenv("PSPRECOMP_CHORD_LOG") ? 100000 : 3000)) return;
    in_sec++; total++;
    std::fprintf(stderr, "[CHORD] %.1f ms raw %04X -> game %04X%s%s (window %d ms)\n", double(now_us) / 1000.0, raw & M, out & M,
                 held_back ? " held back" : "", window_on ? "" : " [window off: not in battle or setting Off]", psp_ctrl_chord_ms());
}

static uint32_t chord_apply(uint32_t raw, int64_t now_us) {
    static std::mutex mu;
    static uint32_t prev = 0, pend = 0;
    static int64_t pend_t[4] = {}, tap_until[4] = {};
    static uint32_t tap_mod[4] = {};
    static const uint32_t FACE[4] = {0x1000u, 0x2000u, 0x4000u, 0x8000u}, MOD = 0x0300u;   // triangle circle cross square, L R
    std::lock_guard<std::mutex> lk(mu);
    const int64_t win = int64_t(psp_ctrl_chord_ms()) * 1000;
    auto gate = g_chord_gate.load(std::memory_order_relaxed);
    if (win <= 0 || (gate && !gate())) { prev = raw; pend = 0; for (int i = 0; i < 4; i++) tap_until[i] = 0; chord_log(raw, raw, false, false, now_us); return raw; }
    uint32_t out_extra = 0;
    for (int i = 0; i < 4; i++) {
        const uint32_t bit = FACE[i];
        const bool held = raw & bit;
        if (pend & bit) {
            if (raw & MOD) { pend &= ~bit; if (!held) { tap_until[i] = now_us + 34000; tap_mod[i] = raw & MOD; } }
            else if (now_us - pend_t[i] >= win) { pend &= ~bit; if (!held) { tap_until[i] = now_us + 34000; tap_mod[i] = 0; } }
        } else if (held && !(prev & bit) && !(raw & MOD)) {
            pend |= bit; pend_t[i] = now_us; tap_until[i] = 0;
        }
        if (!(pend & bit) && !held && now_us < tap_until[i]) out_extra |= bit | tap_mod[i];
    }
    prev = raw;
    const uint32_t out = (raw & ~pend) | out_extra;
    chord_log(raw, out, pend != 0, true, now_us);
    return out;
}

// The pad as a read now would give it (buttons, analog x | y << 8), without writing a sample: the same sources and order
// as write_ctrl_data (host pad / keyboard, debug-socket injections, right-stick D-pad, test script, button filter).
static void current_pad(uint32_t* out_buttons, uint32_t* out_analog) {
    auto now = std::chrono::steady_clock::now();
    uint32_t buttons =
        g_host_buttons.load(std::memory_order_relaxed);
    auto now_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    if (now_ms < g_injected_buttons_deadline_ms.load(
            std::memory_order_relaxed)) {
        buttons |= g_injected_buttons.load(
            std::memory_order_relaxed);
    }
    buttons |= right_stick_dpad_bits();
    const bool scripted = g_script_on.load(std::memory_order_relaxed);
    if (scripted) buttons = g_script_buttons.load(std::memory_order_relaxed);
    buttons = chord_apply(buttons, std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    if (auto f = g_button_filter.load(std::memory_order_relaxed)) buttons = f(buttons);

    // Analog stick: host pad/keyboard stick (128 = centre)
    uint32_t analog = g_host_analog.load(std::memory_order_relaxed);
    if (now_ms < g_injected_analog_deadline_ms.load(
            std::memory_order_relaxed)) {
        analog = g_injected_analog.load(std::memory_order_relaxed);
    }
    if (scripted) analog = g_script_analog.load(std::memory_order_relaxed);
    *out_buttons = buttons;
    *out_analog = analog;
}

void psp_ctrl_sample_now(uint32_t* buttons, uint32_t* analog) { current_pad(buttons, analog); }

static void write_ctrl_data(
    uint8_t* rdram, uint32_t ptr
) {
    // Timestamp: microseconds since boot
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<
        std::chrono::microseconds>(
            now.time_since_epoch()).count();
    psp_mem_write<uint32_t>(rdram, ptr,
        static_cast<uint32_t>(us & 0xFFFFFFFF));

    uint32_t buttons = 0, analog = 0x8080;
    current_pad(&buttons, &analog);
    psp_mem_write<uint32_t>(rdram, ptr + 4, buttons);
    g_last_buttons.store(buttons, std::memory_order_relaxed);
    g_last_analog.store(analog, std::memory_order_relaxed);
    psp_mem_write<uint8_t>(rdram, ptr + 8,
        static_cast<uint8_t>(analog & 0xFF));
    psp_mem_write<uint8_t>(rdram, ptr + 9,
        static_cast<uint8_t>((analog >> 8) & 0xFF));

    // Reserved bytes: zero
    for (uint32_t i = 10; i < 16; i++) {
        psp_mem_write<uint8_t>(rdram, ptr + i, 0);
    }
}

// ---- HLE Functions ----

static void hle_sceCtrlPeekBufferPositive(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t data_ptr = static_cast<uint32_t>(ctx->r[4]);
    int32_t count = ctx->r[5];

    if (std::getenv("PSPRECOMP_CTRL_LOG")) {   // distinct callers of the pad poll (input-hunt aid)
        static uint32_t seen[64]; static int ns = 0;
        uint32_t ra = static_cast<uint32_t>(ctx->r[31]);
        bool known = false;
        for (int i = 0; i < ns; i++) known |= seen[i] == ra;
        if (!known && ns < 64) {
            seen[ns++] = ra;
            std::fprintf(stderr, "[CTRL] sceCtrlPeek/Read buf=0x%08X count=%d ra=0x%08X\n", data_ptr, count, ra);
        }
    }
    if (data_ptr != 0 && count > 0) {
        // Write count samples (16 bytes each)
        for (int32_t i = 0; i < count; i++) {
            write_ctrl_data(rdram, data_ptr + i * 16);
        }
    }

    ctx->r[2] = count;
}

static void hle_sceCtrlReadBufferPositive(
    uint8_t* rdram, recomp_context* ctx
) {
    sched_yield_point();
    hle_sceCtrlPeekBufferPositive(rdram, ctx);
}

static void hle_sceCtrlSetSamplingCycle(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceCtrlSetSamplingMode(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceCtrlSetIdleCancelThreshold(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Registration ----

void psp_hle_register_ctrl() {
    psp_hle_register("sceCtrlPeekBufferPositive",
                      hle_sceCtrlPeekBufferPositive);
    psp_hle_register("sceCtrlReadBufferPositive",
                      hle_sceCtrlReadBufferPositive);
    psp_hle_register("sceCtrlSetSamplingCycle",
                      hle_sceCtrlSetSamplingCycle);
    psp_hle_register("sceCtrlSetSamplingMode",
                      hle_sceCtrlSetSamplingMode);
    psp_hle_register("sceCtrlSetIdleCancelThreshold",
                      hle_sceCtrlSetIdleCancelThreshold);
}
