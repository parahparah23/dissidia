// games/dissidia/runtime/input.cpp — continuous analogue stick direction for Dissidia 012.
//
// The EBOOT's stick processor FUN_08872138 (called from the pad poll FUN_08871E24 each frame with
// a0 = pad.stick vector (0x08C15BB0 in our runs, pad object 0x08C15B90 + 0x20) and a1 = {lx, ly}
// as floats 0..255) works PER AXIS:
//     v = a1[i] - 127.5;  v -= pad.deadzone (float at pad+0x258, default 100.0 = 78% of travel);
//     clamp at 0 (and set the virtual D-pad bit for that axis);  v /= (127.5 - deadzone)
// then normalises the 2-vector: stick vector = (dir.x, dir.y, 0, min(|v|, 1)).
// With a 78% per-axis dead zone, any direction whose weaker axis is below 100/127.5 collapses onto the
// other axis: on a PSP nub (square gate) that leaves 8 usable directions; on a modern circular-gate stick
// (diagonals never exceed ~90/90 of 127.5) it leaves only 4 cardinal directions. That is the "characters
// snap to fixed angles" effect, and it is entirely in the game, not in our pass-through of the 8-bit
// stick.
//
// Fix: wrap FUN_08872138, let the original run (it still produces the virtual D-pad bits returned in
// v0/v1 that menus use, and any side effects), then overwrite the stick vector with a radial version:
//     m = |{lx,ly} - 127.5| / 127.5
//     m < PSPRECOMP_ANALOG_DEADZONE (default 0.20)  ->  (0,0,0,0)
//     else  (dx/|d|, dy/|d|, 0, min(1, (m - dz) / (1 - dz)))
// i.e. the same vector layout/semantics (unit direction + magnitude 0..1; y down-positive), but with the
// true angle. Consumers keep mapping magnitude to speed as the game always did.
//
// PSPRECOMP_ANALOG_FULL=0 restores the original 8-way behaviour. PSPRECOMP_ANALOG_LOG=<n> logs n stick
// updates (raw, game vector, replacement).
//
// Walking direction. The vector above is not what steers a walk: the walker's input, ovl_battle FUN_08B38A0C
// (a0 = out, a1 = walker, a2 = its pad, a3 = camera; called each frame from FUN_08B36D2C on the world map and for the
// player's fighter in battle), asks the pad for the four logical walk bits and builds a camera-space step from them
// (sp+0x40: x = +-1, z = +-1, or y with walker+0x5B1 bit 0x20), then turns it by the inverted camera matrix
// (FUN_0885EA88 .. FUN_089A3F40) or copies it (FUN_089A3BC8, walker+0x5B1 bit 0x40) and normalises it. So every walk goes
// one of eight ways whatever the stick says. Found by VCKing101 (https://github.com/VCKing101/Dissidia012-pc,
// docs/port/analog-movement.md, MIT); this is our own code from those notes.
// Fix: while FUN_08B38A0C runs for the LogicalPad of raw[0] (the hardware pad) with the stick pushed, the first of FUN_0885EA88 / FUN_089A3BC8
// called from its frame rewrites the step at sp+0x40 to the stick's own direction (the pad's +0x20 vector, the radial
// one written above). Everything after it (camera, normalisation, walker+0x5B0 flags from the 8-way bits) is the game's.
// The walk still starts when the game sees a walk bit. World map only by default: a battle replay records just the
// 8-way bits, so a stick-steered fighter would replay elsewhere. PSPRECOMP_ANALOG_WALK=0 turns it off,
// PSPRECOMP_ANALOG_WALK_BATTLE=1 also steers in battle (never while the pads are fed by the online exchange),
// PSPRECOMP_ANALOG_WALK_LOG=<n> logs n replaced steps.

#include "psp_game_module.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

float dissidia_camera_turn();   // camera.cpp

namespace {

constexpr uint32_t STICK_PROCESS = 0x08872138u;
FuncPtr g_orig = nullptr;
uint8_t* g_ram = nullptr;
float g_dz = 0.20f;
int g_log = 0;

float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }

// The game's own 8-way processing (the virtual D-pad bits it returns, which battle movement and the dash / step
// commands read) needs a PSP nub's square gate: a modern stick's circular gate never reaches 78% on both axes at
// a diagonal, so only the four cardinal bits ever fire. In battle the original is run on a square-gate version of
// the stick (the circle stretched to the square: same angle, same size by the max norm) with the game's per-axis
// dead zone lowered to PSPRECOMP_ANALOG_DPAD_ZONE (default 0.35 of travel), which makes eight even sectors.
// PSPRECOMP_ANALOG_8WAY=0 turns this off.
constexpr uint32_t OVL_BASE = 0x089DD4C0u;      // resident overlay id (1 = battle)
constexpr uint32_t PAD_DEADZONE = 0x258u;       // float in the pad object (stick vector is at pad + 0x20)
bool g_8way = true, g_move_rel = true;   // g_move_rel: PSPRECOMP_CAM_MOVE_REL=0 leaves the stick in the game's own frame
float g_dpad_zone = 0.35f;

void dissidia_stick_process(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t out = static_cast<uint32_t>(ctx->r[4]), in = static_cast<uint32_t>(ctx->r[5]);
    float lx = rdf(in), ly = rdf(in + 4);
    uint32_t id = 0;
    std::memcpy(&id, g_ram + (OVL_BASE & 0x07FFFFFFu), 4);
    if (g_move_rel && id == 1) {
        // The free camera (camera.cpp) turns the view away from the game's own camera, but the game still reads the stick
        // in its own frame (behind the fighter, towards the opponent): without this "up" would not go into the screen.
        // The stick is turned by the same angle the view was turned to the right (right and forward are the stick's x and -y).
        const float psi = dissidia_camera_turn();
        if (psi != 0.f) {
            const float sx = (lx - 127.5f) / 127.5f, sf = -(ly - 127.5f) / 127.5f;   // right, forward
            const float c = std::cos(psi), sn = std::sin(psi);
            const float mx = sx * c + sf * sn, mf = -sx * sn + sf * c;
            lx = 127.5f + mx * 127.5f;
            ly = 127.5f - mf * 127.5f;
            wrf(in, lx); wrf(in + 4, ly);
        }
    }
    if (g_8way && id == 1) {
        const float dx = lx - 127.5f, dy = ly - 127.5f;
        const float ax = std::fabs(dx), ay = std::fabs(dy), linf = ax > ay ? ax : ay;
        float sx = dx, sy = dy;
        if (linf > 1e-3f) {
            const float k = std::sqrt(dx * dx + dy * dy) / linf;   // 1 on an axis, up to sqrt 2 on a diagonal
            sx = dx * k; sy = dy * k;
            if (sx > 127.5f) sx = 127.5f; else if (sx < -127.5f) sx = -127.5f;
            if (sy > 127.5f) sy = 127.5f; else if (sy < -127.5f) sy = -127.5f;
        }
        const uint32_t dz_addr = out - 0x20u + PAD_DEADZONE;
        const float dz_was = rdf(dz_addr);
        wrf(in, 127.5f + sx); wrf(in + 4, 127.5f + sy);
        wrf(dz_addr, g_dpad_zone * 127.5f);
        g_orig(rdram, ctx);
        wrf(dz_addr, dz_was);
        wrf(in, lx); wrf(in + 4, ly);
    } else {
        g_orig(rdram, ctx);   // v0/v1 (virtual D-pad bits) are left exactly as the game computed them
    }
    const float dx = lx - 127.5f, dy = ly - 127.5f;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float m = len / 127.5f;
    float ox = 0.f, oy = 0.f, w = 0.f;
    if (m >= g_dz && len > 1e-3f) {
        ox = dx / len; oy = dy / len;
        w = (m - g_dz) / (1.f - g_dz);
        if (w > 1.f) w = 1.f;
    }
    if (g_log > 0 && (std::fabs(dx) > 3.f || std::fabs(dy) > 3.f)) {
        g_log--;
        std::fprintf(stderr, "[STICK] raw=(%.0f,%.0f) angle=%.1f game=(%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f)\n", lx, ly,
            std::atan2(dx, -dy) * 57.29578f, rdf(out), rdf(out + 4), rdf(out + 12), ox, oy, w);
    }
    wrf(out, ox); wrf(out + 4, oy); wrf(out + 8, 0.f); wrf(out + 12, w);
}

// ---- walking direction (see the top of the file) ----
constexpr uint32_t WALK_INPUT = 0x08B38A0Cu;    // ovl_battle
constexpr uint32_t CAM_MATRIX = 0x0885EA88u;    // first call after the step is built (camera path)
constexpr uint32_t VEC_COPY = 0x089A3BC8u;      // first call after the step is built (no-camera path)
constexpr uint32_t WALK_FRAME = 336u;           // FUN_08B38A0C's stack frame; the step is at sp+0x40
constexpr uint32_t LOGICAL_PAD_VT = 0x089C96E8u, PAD_MGR = 0x08C15B90u, BATTLE_OBJ = 0x089D6A10u;
FuncPtr g_orig_walk = nullptr, g_orig_cam_matrix = nullptr, g_orig_vec_copy = nullptr;
bool g_walk_battle = false;
int g_walk_log = 0;
uint32_t g_walk_sp = 0, g_walk_walker = 0;   // set only while an eligible FUN_08B38A0C runs
float g_walk_dx = 0.f, g_walk_dy = 0.f;

uint32_t rdw(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }

bool walk_eligible(uint32_t pad) {
    if (!pad || rdw(pad + 0x34u) != LOGICAL_PAD_VT) return false;           // a CPU brain's or the null pad: the game's way
    if ((rdw(pad + 0x44u) & 1u) || !rdw(pad + 0x4Cu)) return false;         // disabled, or no action map
    if (rdw(PAD_MGR + 0x264u) & 3u) return false;                           // replay playback / muted
    const uint32_t src = rdw(pad + 0x40u);
    if (src != PAD_MGR) return false;                                       // only the pad read from the hardware (raw[0]): not a CPU's
    if (rdw(src + 0x50u) == 1u) return false;                               // raw[0] fed by the online exchange or a replay
    const uint32_t bo = rdw(BATTLE_OBJ);
    const bool battle = bo && (rdw(bo + 0x38u) || rdw(bo + 0x3Cu));         // the world map's battle object has no fighters
    if (battle && !g_walk_battle) return false;
    return rdf(pad + 0x2Cu) > 0.f;                                          // stick past the radial dead zone
}

void walk_input_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t pad = static_cast<uint32_t>(ctx->r[6]);
    if (!walk_eligible(pad)) { g_orig_walk(rdram, ctx); return; }
    const uint32_t prev_sp = g_walk_sp, prev_walker = g_walk_walker;
    const float pdx = g_walk_dx, pdy = g_walk_dy;
    g_walk_sp = static_cast<uint32_t>(ctx->r[29]) - WALK_FRAME;
    g_walk_walker = static_cast<uint32_t>(ctx->r[5]);
    g_walk_dx = rdf(pad + 0x20u); g_walk_dy = rdf(pad + 0x24u);
    g_orig_walk(rdram, ctx);
    g_walk_sp = prev_sp; g_walk_walker = prev_walker; g_walk_dx = pdx; g_walk_dy = pdy;
}

// Called at the first of FUN_0885EA88 / FUN_089A3BC8 from the walk's own frame: the step is complete at sp+0x40.
void walk_replace_step(const recomp_context* ctx) {
    if (!g_walk_sp || static_cast<uint32_t>(ctx->r[29]) != g_walk_sp) return;
    const uint32_t v = g_walk_sp + 0x40u;
    const float x = rdf(v), y = rdf(v + 4), z = rdf(v + 8);
    if (x == 0.f && y == 0.f && z == 0.f) return;                            // not walking this frame
    const bool vertical = (g_ram[(g_walk_walker + 0x5B1u) & 0x07FFFFFFu] & 0x20u) != 0;
    const float nx = g_walk_dx, nf = g_walk_dy;                              // stick: x right, y down = towards the camera
    if (g_walk_log > 0) {
        g_walk_log--;
        std::fprintf(stderr, "[WALK] walker=%08X step (%.0f,%.0f,%.0f) -> (%.3f,%.3f,%.3f) stick angle %.1f\n", g_walk_walker, x, y, z,
                     nx, vertical ? -nf : 0.f, vertical ? 0.f : nf, std::atan2(nx, -nf) * 57.29578f);
    }
    wrf(v, nx);
    if (vertical) { wrf(v + 4, -nf); wrf(v + 8, 0.f); } else { wrf(v + 4, 0.f); wrf(v + 8, nf); }
    g_walk_sp = 0;   // once per walk call
}

void cam_matrix_hook(uint8_t* rdram, recomp_context* ctx) { walk_replace_step(ctx); g_orig_cam_matrix(rdram, ctx); }
void vec_copy_hook(uint8_t* rdram, recomp_context* ctx) { walk_replace_step(ctx); g_orig_vec_copy(rdram, ctx); }

}  // namespace

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);

void dissidia_input_install(uint8_t* rdram) {
    const char* e = std::getenv("PSPRECOMP_ANALOG_FULL");
    if (e && e[0] == '0') { std::fprintf(stderr, "[INPUT] analogue stick: original 8-way processing (PSPRECOMP_ANALOG_FULL=0)\n"); return; }
    g_ram = rdram;
    if ((e = std::getenv("PSPRECOMP_ANALOG_8WAY")) && e[0] == '0') g_8way = false;
    if ((e = std::getenv("PSPRECOMP_CAM_MOVE_REL")) && e[0] == '0') g_move_rel = false;
    if ((e = std::getenv("PSPRECOMP_ANALOG_DPAD_ZONE"))) g_dpad_zone = static_cast<float>(std::atof(e));
    if (g_dpad_zone < 0.05f) g_dpad_zone = 0.05f; else if (g_dpad_zone > 0.78f) g_dpad_zone = 0.78f;
    if ((e = std::getenv("PSPRECOMP_ANALOG_DEADZONE"))) g_dz = static_cast<float>(std::atof(e));
    if (g_dz < 0.f) g_dz = 0.f; else if (g_dz > 0.9f) g_dz = 0.9f;
    if ((e = std::getenv("PSPRECOMP_ANALOG_LOG"))) g_log = std::atoi(e);
    g_orig = psp_dispatch_probe_lookup(STICK_PROCESS);
    if (!g_orig) { std::fprintf(stderr, "[INPUT] stick hook NOT installed\n"); return; }
    psp_dispatch_register(STICK_PROCESS, dissidia_stick_process);
    std::fprintf(stderr, "[INPUT] continuous analogue stick direction (dead zone %.2f)\n", g_dz);

    if ((e = std::getenv("PSPRECOMP_ANALOG_WALK")) && e[0] == '0') { std::fprintf(stderr, "[INPUT] walking: original 8 ways\n"); return; }
    if ((e = std::getenv("PSPRECOMP_ANALOG_WALK_BATTLE"))) g_walk_battle = e[0] == '1';
    if ((e = std::getenv("PSPRECOMP_ANALOG_WALK_LOG"))) g_walk_log = std::atoi(e);
    g_orig_walk = ovl_battle_lookup(WALK_INPUT);
    g_orig_cam_matrix = psp_dispatch_probe_lookup(CAM_MATRIX);
    g_orig_vec_copy = psp_dispatch_probe_lookup(VEC_COPY);
    if (!g_orig_walk || !g_orig_cam_matrix || !g_orig_vec_copy) { std::fprintf(stderr, "[INPUT] walk hook NOT installed\n"); return; }
    psp_dispatch_register(WALK_INPUT, walk_input_hook);
    psp_dispatch_register(CAM_MATRIX, cam_matrix_hook);
    psp_dispatch_register(VEC_COPY, vec_copy_hook);
    std::fprintf(stderr, "[INPUT] analogue walking direction (world map%s)\n", g_walk_battle ? " and battle" : "");
}
