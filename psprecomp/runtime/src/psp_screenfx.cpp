// Screen-space scene effects (SSAO, contact shadows, SSR), see include/psp_screenfx.h.
//
// The scene's depth buffer holds the PSP's window depth: ndc z mapped through glDepthRange(near_z, far_z). A pixel's view
// space position is rebuilt with the inverse of the PSP projection matrix, so the passes do not need to know the game's
// near / far planes or field of view.
//
// Passes (all but the last two at half resolution, which is what makes this affordable on a handheld):
//   1. one pass computes AO + contact shadow (RG texture) and the reflection (premultiplied RGBA texture) from the depth,
//   2. a depth-aware blur of the AO / shadow texture in two directions,
//   3. composite: AO * (1 - shadow) multiplied into the scene (depth-aware 2x2 upsample), then the reflection blended
//      over the result (so a shadow darkens the floor but not the reflection in it).
// RGB only: the buffer's alpha is the GE stencil.
#include "psp_perf.h"
#include "psp_post.h"
#include "psp_screenfx.h"
#include "psp_rt.h"
#include "psp_refl_table.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sys/stat.h>
#include <vector>
PspRtHooks g_psp_rt;
// Per-stage lighting: the game module reports which stage is being fought (-1 = unknown / not in a battle).
static int (*g_stage_id_fn)() = nullptr;
void psp_set_stage_id_provider(int (*fn)()) { g_stage_id_fn = fn; }
int psp_stage_id() { return g_stage_id_fn ? g_stage_id_fn() : -1; }
// How much of the scene's light a stage has. Enclosed stages have no sun: ray-traced shadows darken the whole picture by `dark` there
// (1 = unchanged). PSPRECOMP_STAGE_DARK=<f> overrides it for every stage (tuning), PSPRECOMP_STAGE_LOG=1 logs the id.
// The game hides walls that stand between the camera and the fighters (about 200 maze walls on Pandaemonium, even 1-14 units from the
// camera), and that is not a reason for the ray-traced scene to lose them: every stage keeps all its settled draws for the whole battle,
// except the stages on this blacklist, which have destructible objects that must disappear when they break (the scene then drops draws
// that are inside the view but no longer drawn). Add a stage id here when its broken objects leave ghost shadows.
static const int k_drop_unseen_stages[] = {
    3,   // World of Darkness: crystal pillars / blocks break
};
bool psp_stage_keeps_geometry() {
    const int id = psp_stage_id();
    for (int s : k_drop_unseen_stages) if (s == id) return false;
    return true;
}
struct StageLook { int id; float dark; };
static const StageLook k_stage_looks[] = {
    {1, 0.3f},   // Pandaemonium: enclosed, no light source (issue #4)
};
static float stage_dark() {
    static const bool has_env = std::getenv("PSPRECOMP_STAGE_DARK") != nullptr;
    const int id = psp_stage_id();
    static int last = -2;
    if (id != last) { last = id; if (std::getenv("PSPRECOMP_STAGE_LOG")) std::fprintf(stderr, "[STAGE] id %d\n", id); }
    if (has_env) return std::getenv("PSPRECOMP_STAGE_DARK") ? float(std::atof(std::getenv("PSPRECOMP_STAGE_DARK"))) : 1.f;
    for (const StageLook& s : k_stage_looks) if (s.id == id) return s.dark;
    return 1.f;
}

// ---- Stage lights: hand-placed point lights per stage, lit with ray-traced shadows (added on top of the darkened scene) ----
// File stage_lights.txt (cwd, or PSPRECOMP_STAGE_LIGHTS), re-read while the game runs when it changes:
//   stage <id>
//   light <x> <y> <z> <r> <g> <b> <radius> <intensity>
// In game: F6 drops a light at the camera + forward * distance (PageUp / PageDown change the distance), F7 removes the stage's last light.
struct StageLight { float p[3], c[3], radius, intensity; float clear = 0.f; };   // clear: the shadow ray stops this far short of the light (a pickup must not shadow itself)
static const int kMaxLights = 24;
static std::map<int, std::vector<StageLight>> g_light_file;   // from the file (overrides the built-in table of a stage)
static std::atomic<int> g_light_req{0};                       // 1 drop, 2 remove the selected light (set by the event loop)
static std::atomic<int> g_light_dist10{80};                   // placement distance x10
static std::atomic<bool> g_light_edit{std::getenv("PSPRECOMP_LIGHT_EDIT") != nullptr};                 // edit mode: markers shown, the movement keys are live
static std::atomic<unsigned> g_light_keys{0};                 // held keys, bit = PSP_LIGHT_LEFT..INT_UP, FAST
static std::atomic<int> g_light_nav{0};                       // pending select prev (-1) / next (+1)
static int g_light_sel = 0;
static int g_light_first = 0, g_light_end = 0;   // this frame: the stage's own lights are [first, end) of the lights sent to the shader
static bool g_light_dirty = false;
static double g_light_last_change = 0;
static long g_light_mtime = -1;
static double g_light_check = 0;
static const char* light_path() { const char* e = std::getenv("PSPRECOMP_STAGE_LIGHTS"); return e && e[0] ? e : "stage_lights.txt"; }
static void lights_save_file();
static long file_mtime(const char* p);
static void lights_load_file() {
    g_light_file.clear();
    FILE* f = std::fopen(light_path(), "r");
    if (!f) return;
    char line[256]; int stage = -1; bool has_help = false;
    while (std::fgets(line, sizeof line, f)) {
        if (std::strstr(line, "# Controls")) has_help = true;
        int id; StageLight l;
        if (std::sscanf(line, "stage %d", &id) == 1) { stage = id; g_light_file[stage]; }
        else if (stage >= 0 && std::sscanf(line, "light %f %f %f %f %f %f %f %f", &l.p[0], &l.p[1], &l.p[2], &l.c[0], &l.c[1], &l.c[2], &l.radius, &l.intensity) == 8)
            g_light_file[stage].push_back(l);
    }
    std::fclose(f);
    if (!has_help) { lights_save_file(); g_light_mtime = file_mtime(light_path()); }   // add the controls note
}
static void lights_save_file() {
    FILE* f = std::fopen(light_path(), "w");
    if (!f) return;
    std::fputs(
        "# Stage lights (read while the game runs; edits here apply within a second, in-game edits are written here).\n"
        "# stage <id>                                         the stage the lights below belong to (Pandaemonium = 1)\n"
        "# light <x> <y> <z> <r> <g> <b> <radius> <intensity> world position, colour 0..1, reach (the light fades to 0 there), brightness\n"
        "# Lights are omnidirectional points with ray-traced shadows; they need Ray-Traced Shadows on.\n"
        "#\n"
        "# Controls (in the game window):\n"
        "#   F6  drop a light at the camera + forward * distance, and turn edit mode on     PageUp / PageDown  placement distance\n"
        "#   F8  edit mode on / off (a glowing marker at every light; the selected one is white with a yellow ring)\n"
        "#   F7  remove the selected light\n"
        "#   In edit mode (hold Shift for 4x speed):\n"
        "#     J / L  left / right     I / K  forward / back     U / O  down / up      (camera relative, kept level)\n"
        "#     , / .  radius - / +     - / =  intensity - / +     [ / ]  select previous / next light     P  select the light closest to the player\n"
        "#     T  copy the colour of the surface the selected light is touching (the nearest scene triangle's texture colour, brightened to full)\n"
        "#   The same edit mode can be switched on in Settings > Debug > Light Editor.\n"
        "#\n", f);
    for (const auto& kv : g_light_file) {
        std::fprintf(f, "stage %d\n", kv.first);
        for (const StageLight& l : kv.second) std::fprintf(f, "light %.2f %.2f %.2f %.2f %.2f %.2f %.1f %.2f\n", l.p[0], l.p[1], l.p[2], l.c[0], l.c[1], l.c[2], l.radius, l.intensity);
    }
    std::fclose(f);
}
static long file_mtime(const char* p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_mtime : -1; }
// Lights that ship with the game (from stage_lights.txt files authored in game with the edit mode); a stage_lights.txt entry for the stage
// replaces them (the first edit of a stage copies them into the file).
static const StageLight k_builtin_stage1[] = {   // Pandaemonium
    {{3.79f, 5.97f, 2.03f}, {1.00f, 0.40f, 0.90f}, 80.0f, 1.50f},
    {{-14.73f, 11.33f, 1.16f}, {1.00f, 0.40f, 0.90f}, 17.1f, 1.50f},
    {{-18.59f, 14.88f, -18.47f}, {0.40f, 0.37f, 0.50f}, 48.2f, 1.50f},
    {{-35.07f, 15.00f, -46.99f}, {1.00f, 0.40f, 0.90f}, 34.8f, 1.50f},
    {{-3.92f, 11.06f, -57.27f}, {1.00f, 0.40f, 0.90f}, 37.9f, 1.50f},
    {{3.55f, 2.28f, -28.05f}, {0.40f, 0.37f, 0.50f}, 24.6f, 1.50f},
    {{11.13f, 11.19f, -39.38f}, {1.00f, 0.40f, 0.90f}, 31.4f, 1.50f},
    {{-35.60f, 15.58f, -24.77f}, {1.00f, 0.40f, 0.90f}, 80.0f, 1.50f},
};
static const StageLight k_builtin_stage6[] = {
    {{-14.06f, 32.84f, 27.00f}, {1.00f, 0.90f, 0.80f}, 20.0f, 3.50f},
    {{-27.17f, 32.80f, -6.11f}, {1.00f, 0.90f, 0.80f}, 20.0f, 3.50f},
    {{-5.98f, 32.84f, -27.06f}, {1.00f, 0.90f, 0.80f}, 20.0f, 3.50f},
    {{-4.01f, 33.02f, -4.01f}, {1.00f, 0.90f, 0.80f}, 20.0f, 3.50f},
    {{22.00f, 33.29f, -18.92f}, {1.00f, 0.90f, 0.80f}, 20.0f, 3.50f},
    {{26.99f, 33.06f, 6.05f}, {1.00f, 0.90f, 0.80f}, 20.0f, 3.50f},
    {{3.91f, 33.02f, 11.04f}, {1.00f, 0.90f, 0.80f}, 20.0f, 3.50f},
    {{-10.23f, 20.00f, 15.83f}, {1.00f, 0.90f, 0.80f}, 40.0f, 1.50f},
    {{-7.83f, 12.26f, 6.04f}, {1.00f, 0.90f, 0.80f}, 40.0f, 1.50f},
    {{-18.10f, 33.48f, -23.64f}, {1.00f, 0.90f, 0.80f}, 40.0f, 1.50f},
    {{22.13f, 33.51f, 15.79f}, {1.00f, 0.90f, 0.80f}, 40.0f, 1.50f},
};
static const StageLight k_builtin_stage7[] = {
    {{0.07f, -34.00f, 0.16f}, {0.43f, 1.00f, 0.81f}, 800.0f, 1.50f},
    {{-0.25f, -34.00f, -10.90f}, {0.43f, 1.00f, 0.81f}, 800.0f, 1.50f},
    {{-4.32f, -34.00f, 13.91f}, {0.43f, 1.00f, 0.81f}, 800.0f, 1.50f},
    {{-14.08f, -34.00f, -1.08f}, {0.43f, 1.00f, 0.81f}, 800.0f, 1.50f},
    {{15.12f, -34.00f, 2.17f}, {0.43f, 1.00f, 0.81f}, 800.0f, 1.50f},
};
static const StageLight k_builtin_stage8[] = {
    {{-1.92f, 27.64f, -0.09f}, {0.40f, 0.37f, 0.50f}, 40.0f, 1.50f},
    {{-6.20f, -27.65f, 5.32f}, {0.40f, 0.37f, 0.50f}, 20.0f, 1.50f},
    {{10.16f, -27.67f, -6.74f}, {0.40f, 0.37f, 0.50f}, 20.0f, 1.50f},
    {{0.36f, -8.52f, -25.84f}, {1.00f, 0.90f, 0.80f}, 100.0f, 1.50f},
    {{-12.43f, -14.58f, -24.60f}, {1.00f, 0.90f, 0.80f}, 100.0f, 1.50f},
    {{8.70f, -13.46f, -23.94f}, {1.00f, 0.90f, 0.80f}, 100.0f, 1.50f},
    {{-1.32f, 19.93f, -26.13f}, {1.00f, 0.90f, 0.80f}, 100.0f, 1.50f},
    {{12.72f, 13.70f, -27.61f}, {1.00f, 0.90f, 0.80f}, 100.0f, 1.50f},
    {{-7.75f, 13.15f, -28.84f}, {1.00f, 0.90f, 0.80f}, 100.0f, 1.50f},
};
static const StageLight k_builtin_stage9[] = {
    {{0.38f, 13.53f, -40.27f}, {1.00f, 0.90f, 0.80f}, 200.0f, 1.50f},
};
static const StageLight k_builtin_stage10[] = {
    {{-16.32f, 35.87f, 99.65f}, {1.00f, 0.90f, 0.80f}, 200.0f, 1.50f},
    {{5.31f, -20.38f, 18.80f}, {1.00f, 0.90f, 0.80f}, 400.0f, 1.50f},
};
static const StageLight k_builtin_stage15[] = {
    {{0.08f, 3.98f, -0.18f}, {1.00f, 0.90f, 0.80f}, 200.0f, 1.50f},
};
static const StageLight k_builtin_stage18[] = {
    {{19.98f, 7.62f, -19.23f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
    {{20.03f, 7.60f, -15.87f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
    {{19.95f, 7.83f, -12.37f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
    {{20.02f, 7.89f, -8.88f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
    {{19.98f, 7.68f, -5.32f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
    {{19.99f, 7.74f, -1.58f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
    {{19.97f, 7.87f, 1.61f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
    {{20.00f, 7.80f, 5.06f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
    {{20.00f, 7.72f, 8.67f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
    {{19.93f, 7.79f, 12.15f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
    {{20.01f, 7.75f, 15.66f}, {1.00f, 0.90f, 0.80f}, 14.0f, 1.50f},
};
static const std::vector<StageLight>* builtin_lights(int id) {
    static const std::map<int, std::vector<StageLight>> t = {
        {1, std::vector<StageLight>(std::begin(k_builtin_stage1), std::end(k_builtin_stage1))},
        {6, std::vector<StageLight>(std::begin(k_builtin_stage6), std::end(k_builtin_stage6))},
        {7, std::vector<StageLight>(std::begin(k_builtin_stage7), std::end(k_builtin_stage7))},
        {8, std::vector<StageLight>(std::begin(k_builtin_stage8), std::end(k_builtin_stage8))},
        {9, std::vector<StageLight>(std::begin(k_builtin_stage9), std::end(k_builtin_stage9))},
        {10, std::vector<StageLight>(std::begin(k_builtin_stage10), std::end(k_builtin_stage10))},
        {15, std::vector<StageLight>(std::begin(k_builtin_stage15), std::end(k_builtin_stage15))},
        {18, std::vector<StageLight>(std::begin(k_builtin_stage18), std::end(k_builtin_stage18))},
    };
    const auto it = t.find(id);
    return it == t.end() ? nullptr : &it->second;
}
// The stage's lights now (file entries, re-read about once a second when the file changed).
static const std::vector<StageLight>& stage_lights(int id) {
    static const std::vector<StageLight> none;
    const double now = double(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    if (now - g_light_check > 1.0) {
        g_light_check = now;
        const long m = file_mtime(light_path());
        if (m != g_light_mtime && !g_light_dirty) { g_light_mtime = m; lights_load_file(); }
    }
    auto it = g_light_file.find(id);
    if (it != g_light_file.end() && !it->second.empty()) return it->second;
    const std::vector<StageLight>* b = builtin_lights(id);
    return b ? *b : none;
}
static bool (*g_player_pos_fn)(float*) = nullptr;
static int (*g_pickup_fn)(float (*)[3], int) = nullptr;
void psp_set_pickup_lights_provider(int (*fn)(float (*)[3], int)) { g_pickup_fn = fn; }
void psp_set_player_pos_provider(bool (*fn)(float*)) { g_player_pos_fn = fn; }
bool psp_player_pos(float* p) { return g_player_pos_fn && g_player_pos_fn(p); }
bool psp_stage_light_edit_get() { return g_light_edit.load(); }
void psp_stage_light_edit_set(bool on) { g_light_edit.store(on); std::fprintf(stderr, "[LIGHT] edit mode %s\n", on ? "ON" : "off"); }
static std::atomic<bool> g_light_nearest{false}, g_light_pickcol{false};
// "Copy colour": the light service posts the selected light's position, the scene capture (psp_ge_draw.cpp, which owns the triangles) answers.
static bool g_pick_wait = false, g_pick_have = false; static float g_pick_p[3], g_pick_rgb[3], g_pick_dist;
bool psp_stage_light_pick_request(float* p) { if (!g_pick_wait) return false; g_pick_wait = false; std::memcpy(p, g_pick_p, sizeof g_pick_p); return true; }
void psp_stage_light_pick_result(const float* rgb, float dist) { std::memcpy(g_pick_rgb, rgb, sizeof g_pick_rgb); g_pick_dist = dist; g_pick_have = true; }
bool psp_stage_light_input(int what, bool down) {
    if (what == PSP_LIGHT_TOGGLE_EDIT) { if (down) { const bool v = !g_light_edit.load(); g_light_edit.store(v); std::fprintf(stderr, "[LIGHT] edit mode %s\n", v ? "ON" : "off"); } return true; }
    if (what == PSP_LIGHT_DROP) { if (down) { g_light_req.store(1); g_light_edit.store(true); } return true; }
    if (what == PSP_LIGHT_REMOVE) { if (down) g_light_req.store(2); return true; }
    if (what == PSP_LIGHT_DIST_UP || what == PSP_LIGHT_DIST_DOWN) {
        if (down) { const int v = g_light_dist10.load() + (what == PSP_LIGHT_DIST_UP ? 10 : -10); g_light_dist10.store(std::min(400, std::max(20, v))); std::fprintf(stderr, "[LIGHT] placement distance %.1f\n", g_light_dist10.load() / 10.0); }
        return true;
    }
    if (what == PSP_LIGHT_FAST) { if (down) g_light_keys.fetch_or(1u << 31); else g_light_keys.fetch_and(~(1u << 31)); return false; }
    if (!g_light_edit.load()) return false;                    // the other keys only act in edit mode
    if (what == PSP_LIGHT_NEAREST) { if (down) g_light_nearest.store(true); return true; }
    if (what == PSP_LIGHT_COPY_COLOR) { if (down) g_light_pickcol.store(true); return true; }
    if (what == PSP_LIGHT_PREV || what == PSP_LIGHT_NEXT) { if (down) g_light_nav.store(what == PSP_LIGHT_NEXT ? 1 : -1); return true; }
    if (what >= PSP_LIGHT_LEFT && what <= PSP_LIGHT_INT_UP) { if (down) g_light_keys.fetch_or(1u << what); else g_light_keys.fetch_and(~(1u << what)); return true; }
    return false;
}
// Drop / remove / move / edit requests, from the camera (view = 4x3 column-major world -> view). Writes the file shortly after the last change.
static void lights_service(int id, const float* v) {
    if (id < 0) return;
    const double now = double(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    static double last = 0;
    const float dt = last > 0 ? float(std::min(0.1, now - last)) : 0.f;
    last = now;
    std::vector<StageLight>& L = g_light_file[id];
    static std::map<int, bool> seeded;
    if (!seeded[id]) { seeded[id] = true; if (L.empty()) if (const std::vector<StageLight>* b = builtin_lights(id)) L = *b; }   // edits start from the shipped lights
    const int req = g_light_req.exchange(0);
    const float tx = v[9], ty = v[10], tz = v[11];
    const float cam[3] = {-(v[0] * tx + v[1] * ty + v[2] * tz), -(v[3] * tx + v[4] * ty + v[5] * tz), -(v[6] * tx + v[7] * ty + v[8] * tz)};
    const float fwd[3] = {-v[2], -v[5], -v[8]};
    if (req == 1 && (int)L.size() < kMaxLights) {
        const float d = g_light_dist10.load() / 10.f;
        StageLight l = {{cam[0] + fwd[0] * d, cam[1] + fwd[1] * d, cam[2] + fwd[2] * d}, {1.0f, 0.9f, 0.8f}, 14.f, 1.5f};
        L.push_back(l); g_light_sel = (int)L.size() - 1; g_light_dirty = true; g_light_last_change = now;
        std::fprintf(stderr, "[LIGHT] stage %d: light %d at %.2f %.2f %.2f\n", id, g_light_sel + 1, l.p[0], l.p[1], l.p[2]);
    } else if (req == 2 && !L.empty()) {
        g_light_sel = std::min(g_light_sel, (int)L.size() - 1);
        L.erase(L.begin() + g_light_sel); g_light_sel = std::max(0, g_light_sel - 1); g_light_dirty = true; g_light_last_change = now;
        std::fprintf(stderr, "[LIGHT] stage %d: removed, %d left\n", id, (int)L.size());
    }
    if (g_light_nearest.exchange(false) && !L.empty()) {   // the light closest to the player (horizontal distance)
        float P[3];
        if (g_player_pos_fn && g_player_pos_fn(P)) {
            int best = 0; float bd = 1e30f;
            for (int i = 0; i < (int)L.size(); ++i) { const float dx = L[i].p[0] - P[0], dz = L[i].p[2] - P[2], d = dx * dx + dz * dz; if (d < bd) { bd = d; best = i; } }
            g_light_sel = best;
            std::fprintf(stderr, "[LIGHT] selected light %d of %d, the closest to the player (%.1f away)\n", best + 1, (int)L.size(), std::sqrt(bd));
        } else std::fprintf(stderr, "[LIGHT] no player position available\n");
    }
    if (g_light_pickcol.exchange(false) && !L.empty()) {
        g_light_sel = std::min(g_light_sel, (int)L.size() - 1);
        std::memcpy(g_pick_p, L[g_light_sel].p, sizeof g_pick_p); g_pick_wait = true; g_pick_have = false;
    }
    if (g_pick_have) {
        g_pick_have = false;
        if (g_light_sel < (int)L.size()) {
            StageLight& l = L[g_light_sel];
            float mx = std::max(g_pick_rgb[0], std::max(g_pick_rgb[1], g_pick_rgb[2]));
            if (mx < 0.02f) { std::fprintf(stderr, "[LIGHT] light %d: the surface %.1f away is black, colour kept\n", g_light_sel + 1, g_pick_dist); }
            else {
                for (int c = 0; c < 3; ++c) l.c[c] = g_pick_rgb[c] / mx;   // hue only: brightness is the intensity's job
                g_light_dirty = true; g_light_last_change = now;
                std::fprintf(stderr, "[LIGHT] light %d: colour %.2f %.2f %.2f copied from the surface %.1f away\n", g_light_sel + 1, l.c[0], l.c[1], l.c[2], g_pick_dist);
            }
        }
    }
    if (const int nav = g_light_nav.exchange(0); nav && !L.empty()) { g_light_sel = (g_light_sel + nav + (int)L.size()) % (int)L.size(); std::fprintf(stderr, "[LIGHT] selected light %d of %d\n", g_light_sel + 1, (int)L.size()); }
    if (g_light_edit.load() && !L.empty() && dt > 0.f) {
        g_light_sel = std::min(g_light_sel, (int)L.size() - 1);
        StageLight& l = L[g_light_sel];
        const unsigned k = g_light_keys.load();
        const float sp = ((k >> 31) & 1u ? 12.f : 3.f) * dt;
        float fh[3] = {fwd[0], 0.f, fwd[2]}; const float fl = std::sqrt(fh[0] * fh[0] + fh[2] * fh[2]); if (fl > 1e-4f) { fh[0] /= fl; fh[2] /= fl; }
        const float rt[3] = {v[0], 0.f, v[6]};   // the view x axis in the world (row 0 of the rotation), kept level
        float mv[3] = {0, 0, 0};
        auto held = [&](int b) { return (k >> b) & 1u; };
        const float ax = float(held(PSP_LIGHT_RIGHT)) - float(held(PSP_LIGHT_LEFT)), af = float(held(PSP_LIGHT_FWD)) - float(held(PSP_LIGHT_BACK)), au = float(held(PSP_LIGHT_UP)) - float(held(PSP_LIGHT_DOWN));
        for (int c = 0; c < 3; ++c) mv[c] = (rt[c] * ax + fh[c] * af) * sp;
        mv[1] += au * sp;
        const float dr = (float(held(PSP_LIGHT_RAD_UP)) - float(held(PSP_LIGHT_RAD_DOWN))) * 6.f * dt, di = (float(held(PSP_LIGHT_INT_UP)) - float(held(PSP_LIGHT_INT_DOWN))) * 1.5f * dt;
        if (mv[0] != 0.f || mv[1] != 0.f || mv[2] != 0.f || dr != 0.f || di != 0.f) {
            for (int c = 0; c < 3; ++c) l.p[c] += mv[c];
            l.radius = std::min(1000.f, std::max(1.f, l.radius + dr)); l.intensity = std::min(20.f, std::max(0.f, l.intensity + di));
            g_light_dirty = true; g_light_last_change = now;
        }
    }
    if (g_light_dirty && now - g_light_last_change > 0.4) {
        g_light_dirty = false; lights_save_file(); g_light_mtime = file_mtime(light_path());
        if (g_light_sel < (int)L.size()) { const StageLight& l = L[g_light_sel]; std::fprintf(stderr, "[LIGHT] light %d: %.2f %.2f %.2f radius %.1f intensity %.2f\n", g_light_sel + 1, l.p[0], l.p[1], l.p[2], l.radius, l.intensity); }
    }
}
static std::atomic<int> g_rt_on{-1};
bool psp_rt_enabled() {
    int v = g_rt_on.load();
    if (v < 0) { const char* e = std::getenv("PSPRECOMP_VK_RT"); v = e && e[0] ? std::min(3, std::max(0, std::atoi(e))) : 0; g_rt_on.store(v); }
    return v != 0;
}
static std::atomic<int> g_rt_soft_lvl{-1};   // -1 = not chosen: PSPRECOMP_RT_SOFT (a float) or the default level (Firm)
static const float k_rt_soft_ang[5] = {0.012f, 0.025f, 0.04f, 0.075f, 0.12f};
int psp_rt_softness() { int v = g_rt_soft_lvl.load(); return v < 0 ? 1 : v; }   // default Firm
void psp_rt_set_softness(int level) { g_rt_soft_lvl.store(std::min(4, std::max(0, level))); }
float psp_rt_soft_angle() {
    const int v = g_rt_soft_lvl.load();
    if (v >= 0) return k_rt_soft_ang[v];
    const char* e = std::getenv("PSPRECOMP_RT_SOFT");
    return e && e[0] ? (float)std::atof(e) : k_rt_soft_ang[1];
}
int psp_rt_quality() { return psp_rt_enabled() ? g_rt_on.load() : 0; }
void psp_rt_set_quality(int q) { g_rt_on.store(std::min(3, std::max(0, q))); }
void psp_rt_set_enabled(bool on) { psp_rt_set_quality(on ? 2 : 0); }
static std::atomic<int> g_rt_refl_on{-1};
bool psp_rt_refl_enabled() {
    int v = g_rt_refl_on.load();
    if (v < 0) { const char* e = std::getenv("PSPRECOMP_VK_RT_REFL"); v = e && e[0] ? std::min(3, std::max(0, std::atoi(e))) : 0; g_rt_refl_on.store(v); }
    return v != 0;
}
int psp_rt_refl_quality() { return psp_rt_refl_enabled() ? g_rt_refl_on.load() : 0; }
void psp_rt_refl_set_quality(int q) { g_rt_refl_on.store(std::min(3, std::max(0, q))); }
void psp_rt_refl_set_enabled(bool on) { psp_rt_refl_set_quality(on ? 2 : 0); }
static std::atomic<int> g_rt_refl_fx{-1};
bool psp_rt_refl_fx_enabled() {
    int v = g_rt_refl_fx.load();
    if (v < 0) { const char* e = std::getenv("PSPRECOMP_RT_REFL_FX"); v = e && e[0] == '0' ? 0 : 1; g_rt_refl_fx.store(v); }
    return v != 0;
}
void psp_rt_refl_fx_set_enabled(bool on) { g_rt_refl_fx.store(on ? 1 : 0); }
const char* psp_rt_unavailable_reason() {
    if (psp_rt_available()) return "";
    if (g_psp_rt.unavailable_reason) { const char* r = g_psp_rt.unavailable_reason(); if (r && r[0]) return r; }
#ifdef __ANDROID__
    return "Needs the Vulkan renderer and a driver with ray query: on an Adreno 740 or newer, set Graphics API to Vulkan and pick a Turnip driver under GPU Driver, then restart.";
#else
    return "Needs the Vulkan renderer: set Graphics API to Vulkan and restart.";
#endif
}

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::atomic<int> g_ao{-1}, g_cs{-1}, g_sr{-1};

int env_level(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return 0;
    int n = std::atoi(v);
    return n < 0 ? 0 : n > 3 ? 3 : n;
}
float env_float(const char* name, float dflt) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? float(std::atof(v)) : dflt;
}

const char* const k_vs = R"GLSL(
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// Shared by the passes.
const char* const k_common = R"GLSL(
in vec2 v_uv;
uniform sampler2D u_depth;
uniform vec2 u_size;        // full-resolution size of the depth texture
uniform vec2 u_step;        // one texel of the pass being drawn, in uv (the neighbours for normals / blur)
uniform mat4 u_proj;
uniform mat4 u_inv;
uniform vec2 u_dr;          // glDepthRange near, far
uniform float u_clearz;     // the cleared (background) depth
uniform float u_sky;        // depth_t above which a pixel is the sky dome / background
uniform vec2 u_fade;        // view distance where the effects start to fade out / are gone (the 16-bit depth is too coarse far away)

uniform int u_depth_snap;    // 0 = the old unsnapped read (PSPRECOMP_FX_DEPTH_SNAP=0, for comparison)
// The depth at uv, from one definite full-resolution texel. The passes run at half resolution, where a pixel's centre lies exactly on
// the border between two depth texels: nearest sampling then picks either one depending on the GPU's rounding (Adreno / Turnip did
// not do it consistently), so the normals rebuilt from neighbouring depths jumped in bands along pixel columns and depth contours,
// and AO and the ray-traced shadow's facing term drew evenly spaced lines on walls (RP6, render scale 2). The quarter-texel pull-back
// makes every read land inside a texel.
// The position is rebuilt at the same texel centre the depth came from (vpos): a depth from one place and a screen position from another
// put a point off its surface by up to half a texel's depth change, which is large on a floor seen at a low angle (lines across floors).
uniform int u_snap_split;   // debug PSPRECOMP_FX_SNAP_SPLIT=1
vec2 snap_uv(vec2 uv) { return u_depth_snap == 0 ? uv : (floor(uv * u_size - 0.25) + 0.5) / u_size; }
float wd_at(vec2 uv) { return texture(u_depth, snap_uv(uv)).r; }
float depth_t(float wd) { return (wd - u_dr.x) / (u_dr.y - u_dr.x); }   // 0 at the near plane, 1 at the far plane
bool is_far(float wd) { return abs(wd - u_clearz) < 3.0 / 65535.0 || depth_t(wd) > u_sky; }
vec3 vpos(vec2 uv, float wd) {
    float z = depth_t(wd) * 2.0 - 1.0;
    vec2 ps = (u_snap_split != 0 && v_uv.x < 0.5) ? uv : snap_uv(uv);   // debug split: the left half rebuilds at the unsnapped point
    vec4 p = u_inv * vec4(ps * 2.0 - 1.0, z, 1.0);
    return p.xyz / p.w;
}
vec3 vpos(vec2 uv) { return vpos(uv, wd_at(uv)); }
// View-space z straight from the projection (no matrix product): used where only the depth along the view axis matters.
float vz(float wd) {
    float ndc = depth_t(wd) * 2.0 - 1.0;
    return (u_proj[3][2] - ndc * u_proj[3][3]) / (ndc * u_proj[2][3] - u_proj[2][2]);
}
float dist_fade(float lp) { return 1.0 - smoothstep(u_fade.x, u_fade.y, lp); }
// Surface normal from the depth neighbours (the side with the smaller jump on each axis), facing the camera.
vec3 normal_at(vec2 uv, vec3 P) {
    vec2 px = u_step;
    vec3 a = vpos(uv + vec2(px.x, 0.0)), b = vpos(uv - vec2(px.x, 0.0));
    vec3 c = vpos(uv + vec2(0.0, px.y)), d = vpos(uv - vec2(0.0, px.y));
    vec3 dx = abs(length(a) - length(P)) < abs(length(b) - length(P)) ? a - P : P - b;
    vec3 dy = abs(length(c) - length(P)) < abs(length(d) - length(P)) ? c - P : P - d;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, P) > 0.0) n = -n;      // the camera is at the origin
    return n;
}
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
)GLSL";

// Pass 1 (half resolution): AO + contact shadow -> o_fx, reflection (premultiplied) -> o_refl.
const char* const k_fs_main = R"GLSL(
layout(location = 0) out vec4 o_fx;
layout(location = 1) out vec4 o_refl;
uniform sampler2D u_color;  // the scene, half resolution (reflection source)
uniform float u_radius;     // AO radius as a fraction of the distance to the camera
uniform int u_n_ao;         // samples per effect; 0 = effect off
uniform int u_n_cs;
uniform int u_n_ssr;
uniform vec3 u_light;       // view space, towards the light
uniform vec3 u_up;          // world up in view space
uniform float u_cs_len;     // contact shadow ray length, view units
uniform float u_cs_thick;   // how far behind a visible surface a ray point still counts as occluded, view units
uniform float u_ssr_len;    // reflection ray length as a fraction of the distance to the camera
uniform float u_ssr_amt;
uniform int u_have_fx;      // 1: the scene has an effects layer (blended draws, ge_draw: fxl_*)
uniform sampler2D u_fxc;    // effects colour (premultiplied by the game's own blend) + coverage in alpha; additive draws leave alpha alone
uniform sampler2D u_fxd;    // effects depth: the nearest effect fragment per pixel
uniform int u_fx_dbg;       // PSPRECOMP_SSR_FX_DEBUG=1: a reflection that hit an effect is magenta
uniform float u_fx_gain;    // base reflectivity of an effect (opaque reflections use Fresnel alone)
uniform float u_fx_thick;   // how far behind an effect's front surface a ray still counts as inside it, view units
uniform float u_lscale;     // 1 / stage darkening: the reflection layer is darkened with the stage, lights and markers must not be
uniform int u_mask_on;      // 1: which surfaces reflect, and how much, is the alpha of u_color (reflectivity mask, k_fs_mask); 0: every upward surface
uniform int u_mask_dbg;     // PSPRECOMP_REFL_MASK_DEBUG=1: tint the reflective surfaces green by their reflectivity (red: roughness, blue: metal)
uniform float u_wave;       // water: reflection-normal tilt per unit of ripple-pattern gradient (PSPRECOMP_REFL_WAVE)
uniform float u_wave_max;   // the largest tilt (PSPRECOMP_REFL_WAVE_MAX)
// The surface's material from the mask byte (psp_refl_table.h psp_refl_encode): x reflectivity, y roughness, z metal, w water.
vec4 mask_material(float a) {
    float b = floor(a * 255.0 + 0.5);
    return vec4(floor(b / 32.0) / 7.0, mod(floor(b / 4.0), 8.0) / 7.0, mod(floor(b / 2.0), 2.0), mod(b, 2.0));
}
// Water: the game animates its water by scrolling a ripple pattern, the geometry stays flat, so a reflection from the depth normal sat
// on it as a still mirror. The ripples' brightness gradient (the scene copy, before the reflection is added) tilts the normal along the
// surface, so the reflection bends and flows with the pattern. Limited, so an edge next to the water (a fighter's feet) cannot fold it.
vec3 wave_normal(vec2 uv, vec3 P, vec3 n) {
    vec2 s = u_step * 4.0;      // a wide span: the swell of the ripples, not their half-resolution texels (those gave a harsh speckle)
    const vec3 lw = vec3(0.299, 0.587, 0.114);
    float lx = dot(texture(u_color, uv + vec2(s.x, 0.0)).rgb - texture(u_color, uv - vec2(s.x, 0.0)).rgb, lw);
    float ly = dot(texture(u_color, uv + vec2(0.0, s.y)).rgb - texture(u_color, uv - vec2(0.0, s.y)).rgb, lw);
    vec3 dx = vpos(uv + vec2(s.x, 0.0)) - P, dy = vpos(uv + vec2(0.0, s.y)) - P;
    vec3 tx = dx - n * dot(dx, n), ty = dy - n * dot(dy, n);
    tx /= max(length(tx), 1e-5);
    ty /= max(length(ty), 1e-5);
    vec3 t = (tx * lx + ty * ly) * u_wave;
    float tl = length(t);
    if (tl > u_wave_max) t *= u_wave_max / tl;
    return normalize(n - t);
}
// Schlick's Fresnel with the roughness damping of the grazing-angle boost: a rough surface does not turn into a mirror at a low
// angle the way water or glass does. Metals reflect strongly head-on as well (base 0.5 instead of 0.04).
float fresnel_r(float cosv, float rough, float metal) {
    float f0 = mix(0.04, 0.5, metal);
    return f0 + (max(1.0 - rough, f0) - f0) * pow(1.0 - clamp(cosv, 0.0, 1.0), 5.0);
}
//@RT_DECL
// The effect at uv floats in front of the scene there: its depth is clearly nearer than the scene's. A blended sheet lying on a
// surface (the water layer of the floor, decals, shadows) is at the scene's own depth and is not an effect to reflect.
bool fx_floats(vec2 uv, float wd_scene, float efd) {
    return length(vpos(uv, wd_scene)) - length(vpos(uv, efd)) > 0.2 + 0.01 * length(vpos(uv, wd_scene));
}

void main() {
    o_fx = vec4(1.0, 0.0, 0.0, 1.0);
    o_refl = vec4(0.0);
    float wd = wd_at(v_uv);
    if (u_fx_dbg == 3 && u_have_fx != 0) {   // debug: where the effects layer holds anything (sky included), no floor test
        vec4 fc = texture(u_fxc, v_uv);
        if (fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) { o_refl = vec4(fc.rgb * 0.5 + vec3(0.5, 0.0, 0.5), 1.0); return; }
    }
    vec3 mk = vec3(0.0);
//@RT_MARK
    if (is_far(wd)) { o_refl.rgb = mk * u_lscale; return; }
    vec3 P = vpos(v_uv, wd);
    float lp = length(P);
    float fd = dist_fade(lp);
    if (fd <= 0.0) { o_refl.rgb = mk * u_lscale; return; }
    vec3 n = normal_at(v_uv, P);
    float noise = ign(gl_FragCoord.xy);
    float ao = 1.0;
    if (u_n_ao > 0) {
        float rad = u_radius * lp;
        vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 tx = normalize(cross(up, n)), ty = cross(n, tx);
        float occ = 0.0;
        for (int i = 0; i < u_n_ao; ++i) {
            float t = (float(i) + 0.5) / float(u_n_ao);
            float a = noise * 6.2831853 + float(i) * 2.39996323;
            float c = mix(0.12, 1.0, fract(float(i) * 0.618034 + noise));
            float s = sqrt(1.0 - c * c);
            vec3 dir = tx * (cos(a) * s) + ty * (sin(a) * s) + n * c;
            vec3 S = P + dir * (rad * mix(0.15, 1.0, t * t));
            vec4 cl = u_proj * vec4(S, 1.0);
            if (cl.w <= 1e-5) continue;
            vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
            if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) continue;
            float wd2 = wd_at(uv2);
            if (is_far(wd2)) continue;
            float dq = length(vpos(uv2, wd2)), ds = length(S);
            if (ds - dq > 0.02 * rad) occ += smoothstep(0.0, 1.0, rad / max(abs(lp - dq), 1e-4));
        }
        ao = mix(1.0, 1.0 - occ / float(u_n_ao), fd);
    }
    float sh = 0.0;
    if (u_n_cs > 0) {
        float facing = smoothstep(0.15, 0.5, dot(n, u_light));   // surfaces turned away from the light are dark already
        if (facing > 0.0) {
            float jit = ign(gl_FragCoord.xy + 17.0);
            vec3 O = P + n * (0.01 + 0.004 * lp);
//@RT_CODE
            for (int j = 0; j < u_n_cs; ++j) {
                float t = (float(j) + jit) / float(u_n_cs);
                vec3 S = O + u_light * (t * t * u_cs_len);      // denser near the surface
                vec4 cl = u_proj * vec4(S, 1.0);
                if (cl.w <= 1e-5) break;
                vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
                if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
                float wd2 = wd_at(uv2);
                if (is_far(wd2)) continue;
                float diff = length(S) - length(vpos(uv2, wd2));
                if (diff > 0.01 + 0.004 * lp && diff < u_cs_thick) { sh = facing * fd; break; }
            }
        }
    }
    o_fx = vec4(ao, sh, 0.0, 1.0);
    if (u_fx_dbg == 2 && u_have_fx != 0) {   // debug: the effects layer itself, wherever the layer holds something
        vec4 fc = texture(u_fxc, v_uv);
        if ((fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) && fx_floats(v_uv, wd, texture(u_fxd, snap_uv(v_uv)).r)) { o_refl = vec4(fc.rgb + vec3(0.4, 0.0, 0.4), 1.0); return; }
    }

    // How reflective this surface is (issue #2): the material's reflectivity from the mask, in any orientation (walls too). Without a
    // reflectivity table (another game) the old rule: every upward-facing surface is a mirror floor.
    vec4 mat = u_mask_on != 0 ? mask_material(texture(u_color, v_uv).a) : vec4(smoothstep(0.7, 0.95, dot(n, u_up)), 0.0, 0.0, 0.0);
    float surf_k = mat.x, m_rough = mat.y, m_metal = mat.z;
    vec3 n_geo = n;                                   // the reflection alone sees the water's ripples; the stage lights keep the flat normal
    if (mat.w > 0.5 && u_wave > 0.0 && surf_k > 0.004 && u_n_ssr > 0) n = wave_normal(v_uv, P, n);
    if (u_n_ssr > 0 && surf_k > 0.004) {
//@RT_REFL
        {
        float upness = dot(n, u_up);
        vec3 V = normalize(P);
        vec3 R = reflect(V, n);
        float maxd = u_ssr_len * lp;
        float stepl = maxd / float(u_n_ssr);
        float jit = ign(gl_FragCoord.xy);
        vec3 O = P + n * (0.004 * lp);
        vec2 hit = vec2(-1.0);
        float hit_t = 0.0, prev_t = 0.0;
        vec2 last_in = vec2(-1.0);
        bool fx_hit = false;
        float fx_t = 0.0;
        vec4 fx_c = vec4(0.0);
        vec2 fx_uv = vec2(0.5);
        for (int i = 0; i < u_n_ssr; ++i) {
            float t = (float(i) + jit) * stepl;
            vec3 S = O + R * t;
            vec4 cl = u_proj * vec4(S, 1.0);
            if (cl.w <= 1e-5) break;
            vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
            if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
            last_in = uv2;
            float wd2 = wd_at(uv2);
            if (u_have_fx != 0 && !fx_hit) {
                // effects layer: orbs, beams, auras, glass. They write no scene depth, so the ray is tested against the nearest effect
                // fragment's own depth; an effect later covered by opaque geometry (its depth behind the scene's) is not there.
                vec4 fc = texture(u_fxc, uv2);
                if (fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) {
                    float efd = texture(u_fxd, snap_uv(uv2)).r;
                    if ((is_far(wd2) || fx_floats(uv2, wd2, efd))) {
                        float de = length(vpos(uv2, efd)), ds = length(S);
                        if (ds > de - 0.02 * lp && ds < de + stepl * 2.5 + u_fx_thick) { fx_hit = true; fx_t = t; fx_c = fc; fx_uv = uv2; }
                    }
                }
            }
            if (!is_far(wd2)) {
                float diff = length(S) - length(vpos(uv2, wd2));
                if (diff > 0.0 && diff < stepl * 2.5 + 0.01 * lp) {
                    float lo = prev_t, hi = t;
                    for (int k = 0; k < 4; ++k) {           // refine between the previous and this step
                        float mid = 0.5 * (lo + hi);
                        vec3 Sm = O + R * mid;
                        vec4 cm = u_proj * vec4(Sm, 1.0);
                        vec2 um = cm.xy / cm.w * 0.5 + 0.5;
                        float wm = wd_at(um);
                        if (!is_far(wm) && length(Sm) - length(vpos(um, wm)) > 0.0) hi = mid; else lo = mid;
                    }
                    vec3 Sh = O + R * hi;
                    vec4 ch = u_proj * vec4(Sh, 1.0);
                    hit = ch.xy / ch.w * 0.5 + 0.5;
                    hit_t = hi;
                    break;
                }
            }
            prev_t = t;
        }
        if (hit.x < 0.0 && last_in.x >= 0.0 && is_far(wd_at(last_in))) {   // the ray left the scene into the sky
            hit = last_in; hit_t = maxd;
            // The sky (and the beams / clouds painted on it) lies in the ray's direction, at infinity: take the picture where that
            // direction projects, not where the short march happened to end (that showed the sky straight above the floor point).
            vec4 cs = u_proj * vec4(R, 0.0);
            if (cs.w > 1e-5) {
                vec2 us = cs.xy / cs.w * 0.5 + 0.5;
                if (us.x > 0.0 && us.x < 1.0 && us.y > 0.0 && us.y < 1.0 && is_far(wd_at(us))) hit = us;
            }
        }
        if (hit.x >= 0.0 || fx_hit) {
            vec3 col = vec3(0.0);
            vec2 euv = fx_uv;
            float ht = hit_t;
            if (hit.x >= 0.0) { col = texture(u_color, hit).rgb; euv = hit; }
            vec2 e = min(euv, 1.0 - euv);
            float edge = smoothstep(0.0, 0.12, min(e.x, e.y));
            float fres = fresnel_r(dot(n, -V), m_rough, m_metal);
            float refl_k = surf_k * fd * u_ssr_amt;
            float k = hit.x >= 0.0 ? clamp(fres * 1.6 + 0.08, 0.0, 1.0) * edge * (1.0 - ht / maxd * 0.6) * refl_k : 0.0;
            o_refl = vec4(col * k, k);
            if (fx_hit) {
                // Effects are glowing / see-through things that read as bright reflections even at a steep angle on a wet floor, so they
                // get a stronger base than the Fresnel-only opaque reflection (an orb or beam at 14% was invisible).
                float efe = min(min(fx_uv.x, 1.0 - fx_uv.x), min(fx_uv.y, 1.0 - fx_uv.y));
                float kf = clamp(fres * 1.6 + u_fx_gain, 0.0, 1.0) * smoothstep(0.0, 0.12, efe) * (1.0 - fx_t / maxd * 0.5) * refl_k;
                float cov = clamp(max(fx_c.a, max(fx_c.r, max(fx_c.g, fx_c.b))), 0.0, 1.0);
                float af = cov * kf;
                o_refl = vec4(fx_c.rgb * kf + o_refl.rgb * (1.0 - fx_c.a * kf), af + o_refl.a * (1.0 - af));
            }
            if (u_fx_dbg == 1 && fx_hit) o_refl = vec4(0.8, 0.0, 0.8, 0.8);
        }
        }
    }
    n = n_geo;
    if (m_metal > 0.5 && o_refl.a > 0.0) {   // metal: the reflection takes the surface's colour (gold reflects gold)
        vec3 alb = texture(u_color, v_uv).rgb;
        float l = max(dot(alb, vec3(0.299, 0.587, 0.114)), 0.05);
        o_refl.rgb *= clamp(alb / l, 0.0, 2.0) * 0.85;
    }
    if (u_mask_dbg != 0 && u_n_ssr > 0) o_refl = vec4(0.7 * m_rough * surf_k, 0.7 * surf_k, 0.7 * m_metal * surf_k, 0.7 * surf_k);
//@RT_LIGHTS
}
)GLSL";

// Reflectivity mask (issue #2; psp_ge_draw.cpp g_rml_tris): the reflective draws' triangles, captured in clip space with their
// material byte, drawn in one pass into a full-resolution mask (RGBA8, alpha = the byte) with its own depth.
const char* const k_vs_rmask = R"GLSL(
in vec4 a_clip;
in float a_mat;
out float v_mat;
void main() {
    v_mat = a_mat;
    gl_Position = a_clip;
}
)GLSL";
const char* const k_fs_rmask = R"GLSL(
in float v_mat;
uniform float u_byte;       // 1/255 (a uniform also keeps the Vulkan layer's uniform block from being empty)
layout(location = 0) out vec4 o_color;
void main() {
    o_color = vec4(0.0, 0.0, 0.0, v_mat * u_byte);
}
)GLSL";
// Mask resolve, half resolution, into the alpha of the colour copy (which pass 1 reads as u_color anyway, so it costs no sampler
// there). A mask texel counts only where its depth is still the scene's: nothing that does not reflect was drawn over it later, and
// a see-through texel of a cut-out card (the scene shows what lies behind it) does not count either.
const char* const k_fs_mask = R"GLSL(
layout(location = 0) out vec4 o_color;
uniform sampler2D u_mask;   // alpha = reflectivity (0 where no reflective draw)
uniform sampler2D u_mdepth; // the mask's own depth
uniform int u_have;         // 0: no reflective draw this frame
void main() {
    float r = 0.0;
    float wd = wd_at(v_uv);
    if (u_have != 0 && !is_far(wd)) {
        float m = texture(u_mask, snap_uv(v_uv)).a;
        if (m > 0.0) {
            float a = length(vpos(v_uv, wd)), b = length(vpos(v_uv, texture(u_mdepth, snap_uv(v_uv)).r));
            if (abs(a - b) < 0.002 * a + 0.01) r = m;
        }
    }
    o_color = vec4(0.0, 0.0, 0.0, r);
}
)GLSL";

// Glossy reflections (issue #2): a separable blur of the reflection layer whose radius is the surface's roughness (from the mask byte in
// the colour copy's alpha), so polished stone gets a soft sheen and water stays a mirror. Depth-aware and limited to reflective
// neighbours, so nothing bleeds across a silhouette or onto a surface that does not reflect.
const char* const k_fs_rblur = R"GLSL(
out vec4 o_color;
uniform sampler2D u_src;    // the reflection layer (premultiplied)
uniform sampler2D u_color;  // alpha = the mask byte
uniform vec2 u_dir;         // one texel along the blur axis
uniform float u_rad;        // radius in texels at roughness 1
float rough_at(vec2 uv) { float b = floor(texture(u_color, uv).a * 255.0 + 0.5); return b < 32.0 ? -1.0 : mod(floor(b / 4.0), 8.0) / 7.0; }
void main() {
    vec4 c0 = texture(u_src, v_uv);
    float rough = rough_at(v_uv);
    float wd = wd_at(v_uv);
    if (rough <= 0.0 || is_far(wd)) { o_color = c0; return; }
    float z0 = vz(wd);
    float r = rough * u_rad;
    vec4 sum = c0;
    float ws = 1.0;
    for (int i = 1; i <= 6; ++i) {
        float t = float(i) / 6.0;
        for (int sd = -1; sd <= 1; sd += 2) {
            vec2 uv = v_uv + u_dir * (float(sd) * t * r);
            float w2 = wd_at(uv);
            if (is_far(w2) || rough_at(uv) < 0.0) continue;
            float dl = abs(vz(w2) - z0) / abs(z0);
            float w = exp(-t * t * 2.5) * exp(-dl * 40.0);
            sum += texture(u_src, uv) * w;
            ws += w;
        }
    }
    o_color = sum / ws;
}
)GLSL";

// Pass 2: depth-aware blur of o_fx (5 taps, half resolution).
const char* const k_fs_blur = R"GLSL(
out vec4 o_color;
uniform sampler2D u_fx;
uniform vec2 u_dir;         // one texel step
void main() {
    float wd = wd_at(v_uv);
    if (is_far(wd)) { o_color = vec4(1.0, 0.0, 0.0, 1.0); return; }
    float z0 = vz(wd);
    vec2 sum = vec2(0.0);
    float wsum = 0.0;
    for (int i = -2; i <= 2; ++i) {
        vec2 uv = v_uv + u_dir * float(i);
        float w2 = wd_at(uv);
        if (is_far(w2)) continue;
        float dl = abs(vz(w2) - z0) / abs(z0);
        float w = exp(-float(i * i) / 3.0) * exp(-dl * 60.0);
        sum += texture(u_fx, uv).rg * w;
        wsum += w;
    }
    o_color = vec4(wsum > 0.0 ? sum / wsum : vec2(1.0, 0.0), 0.0, 1.0);
}
)GLSL";

// Pass 3a (full resolution, blend dst * src): AO and contact shadow, upsampled from half resolution by weighting the four
// nearest texels by how close their depth is to this pixel's, so nothing bleeds across a silhouette.
const char* const k_fs_mul = R"GLSL(
out vec4 o_color;
uniform sampler2D u_fx;
uniform vec2 u_half;        // half-resolution size
uniform float u_ao_amt;
uniform float u_cs_amt;
uniform float u_dark;       // stage darkening (enclosed stages), 1 = none
void main() {
    float wd = wd_at(v_uv);
    if (is_far(wd)) { o_color = vec4(1.0); return; }
    float z0 = vz(wd);
    vec2 hp = v_uv * u_half - 0.5;
    vec2 base = floor(hp), f = hp - base;
    vec2 sum = vec2(0.0);
    float wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        vec2 o = vec2(float(k & 1), float(k >> 1));
        vec2 uvc = (base + o + 0.5) / u_half;
        float bw = (o.x > 0.5 ? f.x : 1.0 - f.x) * (o.y > 0.5 ? f.y : 1.0 - f.y);
        float w2 = wd_at(uvc);
        float dl = is_far(w2) ? 1.0 : abs(vz(w2) - z0) / abs(z0);
        float w = (bw + 0.001) * exp(-dl * 60.0) + 1e-5;
        sum += texture(u_fx, uvc).rg * w;
        wsum += w;
    }
    vec2 fx = sum / wsum;
    o_color = vec4(vec3(mix(1.0, fx.r, u_ao_amt) * (1.0 - fx.g * u_cs_amt) * u_dark), 1.0);
}
)GLSL";

// Pass 3b (full resolution, blend one / one-minus-src-alpha): the premultiplied reflection.
const char* const k_fs_refl = R"GLSL(
out vec4 o_color;
uniform sampler2D u_refl;
uniform float u_dbg;        // debug (PSPRECOMP_REFL_ONLY=1): show the reflection layer alone
uniform float u_dark;       // stage darkening: the reflection is light too
void main() {
    if (is_far(wd_at(v_uv))) { o_color = vec4(0.0); return; }
    o_color = texture(u_refl, v_uv);
    o_color.rgb *= u_dark;
    if (u_dbg > 0.0) o_color = vec4(o_color.rgb * 2.0, 1.0);
}
)GLSL";

// Sky map update (ray-traced reflections): an equirectangular map of the stage's sky by WORLD direction, filled from the sky
// pixels on screen and kept across frames, so a reflection ray that misses (or hits the dome) shows the sky in its direction
// even where that part of the sky is off screen. Before it the picture's own sky was used only while on screen and a flat
// colour elsewhere, which cut the reflected sky off along a hard line. Premultiplied (blend one / one-minus-src-alpha):
// alpha says how much of a texel has been seen.
const char* const k_fs_env = R"GLSL(
layout(location = 0) out vec4 o_env;
uniform sampler2D u_color;
uniform mat4 u_view;        // world -> view
uniform float u_w;          // weight of this frame (the rest is the map so far)
void main() {
    float az = (v_uv.x - 0.5) * 6.2831853;
    float el = (v_uv.y - 0.5) * 3.14159265;
    vec3 d = vec3(cos(el) * cos(az), sin(el), cos(el) * sin(az));
    vec3 dv = (u_view * vec4(d, 0.0)).xyz;
    if (dv.z > -0.02) discard;
    vec4 c = u_proj * vec4(dv * 1000.0, 1.0);
    if (c.w <= 1e-5) discard;
    vec2 uv = c.xy / c.w * 0.5 + 0.5;
    if (uv.x < 0.01 || uv.x > 0.99 || uv.y < 0.01 || uv.y > 0.99) discard;
    vec2 o = 3.0 / u_size;      // the colour copy is half resolution: keep clear of geometry so its edge does not bleed in
    if (!is_far(wd_at(uv)) || !is_far(wd_at(uv + vec2(o.x, 0.0))) || !is_far(wd_at(uv - vec2(o.x, 0.0)))
        || !is_far(wd_at(uv + vec2(0.0, o.y))) || !is_far(wd_at(uv - vec2(0.0, o.y)))) discard;
    o_env = vec4(texture(u_color, uv).rgb * u_w, u_w);
}
)GLSL";

struct Prog {
    GLuint id = 0;
    bool failed = false;
};
Prog g_main_p, g_blur_p, g_mul_p, g_refl_p, g_main_rt_p, g_main_rt_sky_p, g_env_p, g_mask_p, g_rblur_p, g_rmask_p;

// Ray-queried contact shadow (PSPRECOMP_VK_RT, Vulkan only; docs/VULKAN_PORT.md): the pass-1 shader with the screen-space march
// replaced by a ray query from the surface toward the light against the frame's world-space triangles (psp_rt.h). The GL
// shaders never see these lines: the markers are comments there.
const char* const k_rt_decl = R"GLSL(
uniform accelerationStructureEXT u_tlas;
uniform int u_use_rt;
uniform mat4 u_view_inv;    // view space -> PSP world space (where the triangles are)
uniform vec3 u_light_w;     // world space, towards the light
uniform float u_rt_len;     // ray length, world units
uniform int u_rt_rays;      // shadow rays per pixel (quality level: 1, 3, 6), jittered across the light disc for a soft edge
uniform float u_rt_soft;    // half angle of the light disc, radians-ish
uniform int u_use_rt_refl;
uniform int u_rt_dbg;       // debug (PSPRECOMP_RT_REFL_DEBUG=1): colour by what the ray found (red tri colour, green visible on screen, blue miss, yellow sky)
uniform int u_rt_refl_rays; // reflection rays per pixel (quality: 1, 1, 3), jittered by u_rt_rough for a glossy blur
uniform float u_rt_rough;
uniform float u_rough_jit;  // extra jitter at material roughness 1
uniform int u_rt_fx;        // the scene has an effects layer
uniform float u_fx_minh;    // effect hits lower than this above the reflecting surface are skipped (world units, PSPRECOMP_RT_FX_MINH)
uniform float u_rt_hidden;   // brightness / strength of a hit on a surface that is not visible on screen
uniform float u_rt_sky_amt;  // strength of a reflection of the sky relative to one of geometry
uniform float u_rt_sky_dist; // view units past which a hit counts as the sky dome
uniform int u_refl_split;   // debug (PSPRECOMP_RT_REFL_SPLIT=1): the left half of the picture keeps the screen-space reflection
uniform float u_rt_refl_len; // reflection ray length, world units
uniform vec3 u_rt_sky;      // what a reflection ray that hits nothing shows when the sky is off screen
uniform int u_n_lights;     // stage point lights (world space xyz + radius; colour x intensity)
uniform vec4 u_lp[24];
uniform vec4 u_lc[24];
uniform vec4 u_lvp[24];     // the same lights in view space (markers)
uniform int u_edit;         // light edit mode: draw a marker at each light
uniform int u_sel;          // the selected light
uniform int u_fx0;          // lights from u_fx0 on are effect lights (linear falloff)
uniform int u_fx_split;     // debug: effect lights only on the left half of the screen
uniform float u_fx_alb;     // the least albedo an effect light sees
uniform int u_mark0;        // markers for lights [u_mark0, u_mark1) (the stage's own, not EX cores / effects)
uniform int u_mark1;
//@SKY_DECL
uniform int u_env_on;
// The sky seen in world direction d. A direction never on screen yet (overhead, behind the camera) falls back to the same
// bearing nearer the horizon, which the camera sees most. rgb is the colour, a how sure (0: not seen at all).
vec4 sky_map(vec3 d) {
    if (u_env_on == 0) return vec4(0.0);
    float az = atan(d.z, d.x);
    float el = asin(clamp(d.y, -1.0, 1.0));
    vec3 acc = vec3(0.0);
    float aa = 0.0;
    for (int i = 0; i < 4; ++i) {
        float e = el * (1.0 - float(i) / 3.0 * 0.85);
        vec4 s = texture(u_env, vec2(az / 6.2831853 + 0.5, e / 3.14159265 + 0.5));
        if (s.a > 0.02) {
            float c = min(1.0, s.a * 1.5) * (1.0 - aa);
            acc += s.rgb / s.a * c;
            aa += c;
        }
        if (aa > 0.95) break;
    }
    return aa > 0.0 ? vec4(acc / aa, aa) : vec4(0.0);
}
)GLSL";
// Ray-traced reflection: the closest hit along the reflected view ray. Shading the hit: the geometry has positions only, so a
// hit shows the triangle's colour captured at draw time (texture average x vertex colour / lit material, rt_tri_col[]); where
// the hit point is visible on screen the real, shaded scene colour is used instead (screen-space assisted). A miss shows the
// sky: the scene colour in that direction when it is on screen, else u_rt_sky.
const char* const k_rt_refl = R"GLSL(
        if (u_use_rt_refl != 0 && !(u_refl_split != 0 && v_uv.x < 0.5)) {
            float upness = dot(n, u_up);
            vec3 V = normalize(P);
            vec3 R0 = reflect(V, n);
            float el0 = max(dot(R0, n), 0.0);
            vec4 racc = vec4(0.0);
            for (int ri = 0; ri < u_rt_refl_rays; ++ri) {
            vec3 R = R0;
            if (u_rt_refl_rays > 1 || m_rough > 0.0) {
                // Glossy jitter. The part along the normal is limited to half the ray's own elevation: at a grazing angle a fixed
                // angular jitter bent rays up by many times their elevation and smeared the horizon over the whole floor (High looked blurry).
                float ja = ign(gl_FragCoord.xy + 31.0 * float(ri + 1)) * 6.2831853, jb = ign(gl_FragCoord.yx + 57.0 * float(ri + 1)) * 2.0 - 1.0, jr = sqrt(1.0 - jb * jb);
                vec3 J = vec3(cos(ja) * jr, jb, sin(ja) * jr) * (u_rt_rough + m_rough * u_rough_jit);   // the material's roughness widens the lobe (the blur pass smooths the noise)
                float jn = dot(J, n);
                J += n * (clamp(jn, -0.5 * el0, 0.5 * el0) - jn);
                R = normalize(R0 + J);
            }
            vec3 O = P + n * (0.01 + 0.004 * lp);
            vec3 Ow = (u_view_inv * vec4(O, 1.0)).xyz;
            vec3 Rw = normalize((u_view_inv * vec4(R, 0.0)).xyz);
            vec3 col = u_rt_sky;
            float hit_t = u_rt_refl_len;
            bool hit = false;
            bool hidden = false;                 // the hit shows a flat triangle colour (not visible on screen)
            vec3 dbg = vec3(0.0, 0.0, 1.0);      // miss
            vec3 cut = vec3(0.0);                // cut-out cards passed through off screen, weighted by their coverage
            float trans = 1.0;
            float tmin = 0.0;
            // Up to 4 hits: a hit on a see-through texel of a cut-out card (tree, spire, fence: the triangle is a whole quad) is not
            // a reflector. On screen that shows as a hit point the picture does not draw (the picture is farther there, or sky):
            // trace on past it. Off screen the triangle's mean texture alpha (coverage) blends it with what lies behind.
            for (int pass = 0; pass < 4; ++pass) {
                rayQueryEXT rq;
                rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsOpaqueEXT, 0x1, Ow, tmin, Rw, u_rt_refl_len);
                while (rayQueryProceedEXT(rq)) {}
                hit = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
                if (!hit) { hit_t = u_rt_refl_len; break; }
                hit_t = rayQueryGetIntersectionTEXT(rq, true);
                uint pc = rt_tri_col[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true) + rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)];
                col = vec3(float(pc & 255u), float((pc >> 8) & 255u), float((pc >> 16) & 255u)) / 255.0;
                float cov = float(pc >> 24) / 255.0;
                dbg = vec3(1.0, 0.0, 0.0);       // hit, shaded by the triangle's colour
                hidden = true;
                vec3 H = O + R * hit_t;
                float lh = length(H);
                if (lh >= u_rt_sky_dist) break;  // the sky dome: shown from the sky map below
                vec4 ch = u_proj * vec4(H, 1.0);
                if (ch.w > 1e-5) {
                    vec2 uvh = ch.xy / ch.w * 0.5 + 0.5;
                    if (uvh.x > 0.0 && uvh.x < 1.0 && uvh.y > 0.0 && uvh.y < 1.0) {
                        float wdh = wd_at(uvh);
                        float ls = is_far(wdh) ? 1e9 : length(vpos(uvh, wdh));
                        if (abs(ls - lh) < 0.03 * lh + 0.05) { col = texture(u_color, uvh).rgb; dbg = vec3(0.0, 1.0, 0.0); hidden = false; break; }   // visible: the real shaded colour
                        if (ls > lh * 1.04 + 0.3) { tmin = hit_t + 0.05; hit = false; continue; }   // the picture shows what is behind it: a see-through texel
                    }
                }
                if (cov > 0.97 || pass == 3) break;
                cut += trans * cov * col * u_rt_hidden;
                trans *= 1.0 - cov;
                tmin = hit_t + 0.05;
                hit = false;
            }
            vec3 H = O + R * (hit ? hit_t : 500.0);
            bool sky = !hit || length(H) >= u_rt_sky_dist;    // the sky dome (or nothing): the floor already carries a baked sky, so it counts less
            if (sky) {
                vec4 sm = sky_map(Rw);                         // the stage's sky as seen so far, by world direction
                vec3 scr = u_rt_sky;
                vec4 ch = u_proj * vec4(H, 1.0);
                if (ch.w > 1e-5) {                              // old fallback: the picture's sky in that direction, if it is the sky there
                    vec2 uvs = clamp(ch.xy / ch.w * 0.5 + 0.5, vec2(0.02), vec2(0.98));
                    if (is_far(wd_at(uvs))) scr = texture(u_color, uvs).rgb;
                }
                col = mix(scr, sm.rgb, sm.a);
                hidden = false;
                dbg = vec3(1.0, 1.0, 0.0);
                hit_t = 0.25 * u_rt_refl_len;
            }
            if (hidden) col *= u_rt_hidden;      // a surface the camera cannot see (an underside, a back): its flat colour is unlit, so show it dim
            col = cut + trans * col;
            // The effects layer (orbs, auras, glass: blended draws that write no depth): the closest one in front of the opaque hit,
            // shown with its real on-screen colour when it is visible there, else its triangle colour, blended over the opaque reflection.
            vec3 fxc = vec3(0.0);   // summed effect colour x coverage
            float fxa = 0.0, fx_t = 0.0;
            int nfx = 0;
            if (u_rt_fx != 0 && !(u_rt_fx == 2 && v_uv.x < 0.5)) {   // 2 = debug split: no effects layer in the left half
                // The closest effect hit that is clearly above the reflecting surface: flat sheets lying on or just above the floor (Watera's
                // splash ring, Thundaga's ground ripples, decals) are hit at once by every ray leaving the floor and washed the whole area out.
                // Shaded with the effect triangle's own colour (texture average x vertex colour, coverage in alpha): most effects are drawn after
                // the 3D run, so the scene copy this pass reads does not hold them, and sampling it showed the floor behind the effect instead.
                // Effects are additive glows drawn in layers (a rail beam: a white core inside a green ribbon), so every effect triangle along
                // the ray adds its colour x coverage (up to 4), in ONE traversal: the effect instance is traced as non-opaque, each triangle the
                // traversal meets is a candidate that is summed and never committed. (Separate closest-hit queries per layer cost the RP6 8-10
                // fps during spells.)
                rayQueryEXT rf;
                rayQueryInitializeEXT(rf, u_tlas, gl_RayFlagsNoOpaqueEXT, 0x2, Ow, 0.0, Rw, hit ? hit_t : u_rt_refl_len);
                while (rayQueryProceedEXT(rf)) {
                    if (rayQueryGetIntersectionTypeEXT(rf, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
                    float t = rayQueryGetIntersectionTEXT(rf, false);
                    if (dot(R * t, n) < u_fx_minh) continue;   // height of the hit above the surface point
                    uint fc = rt_fx_col[rayQueryGetIntersectionPrimitiveIndexEXT(rf, false)];
                    float a = float(fc >> 24) / 255.0;
                    fxc += vec3(float(fc & 255u), float((fc >> 8) & 255u), float((fc >> 16) & 255u)) / 255.0 * a;
                    fx_t = fxa == 0.0 ? t : min(fx_t, t);   // the nearest one sets the distance fade
                    fxa = 1.0;
                    if (++nfx >= 4) rayQueryTerminateEXT(rf);
                }
            }
            float fres = fresnel_r(dot(n, -V), m_rough, m_metal);
            float k = clamp(fres * 1.6 + 0.08, 0.0, 1.0) * (hidden ? u_rt_hidden : 1.0) * (1.0 - hit_t / u_rt_refl_len * 0.5) * surf_k * fd * u_ssr_amt * (sky ? u_rt_sky_amt : 1.0);
            o_refl = vec4(col * k, k);
            if (fxa > 0.0) {
                // Effects are glows (beams, bolts, the water orb's highlights): their reflection adds light over the floor and its reflection
                // instead of covering them, with a stronger base than the Fresnel-only opaque reflection (same as the screen-space effects
                // layer, PSPRECOMP_SSR_FX_GAIN): at the floor's steep angles a glow at plain Fresnel strength was invisible.
                float kf = clamp(fres * 1.6 + u_fx_gain, 0.0, 1.0) * (1.0 - fx_t / u_rt_refl_len * 0.5) * surf_k * fd * u_ssr_amt;
                o_refl.rgb += min(fxc, vec3(1.5)) * kf;   // fxc is already colour x coverage, summed
                if (u_rt_dbg != 0) dbg = vec3(1.0, 0.0, 1.0);
            }
            if (u_rt_dbg == 1 || (u_rt_dbg == 2 && v_uv.x > 0.5)) o_refl = vec4(dbg * 0.6, 0.6);
            racc += o_refl;
            }
            o_refl = racc / float(u_rt_refl_rays);
        } else
)GLSL";
const char* const k_rt_mark = R"GLSL(
    if (u_edit != 0 && u_n_lights > 0) {
        vec4 q = u_inv * vec4(v_uv * 2.0 - 1.0, 1.0, 1.0);
        vec3 dir = normalize(q.xyz / q.w);
        float surf = is_far(wd) ? 1e9 : length(vpos(v_uv, wd));
        for (int li = u_mark0; li < min(u_mark1, u_n_lights); ++li) {
            vec3 Lv = u_lvp[li].xyz;
            float t = dot(Lv, dir);
            if (t <= 0.0) continue;
            float d = length(Lv - dir * t);
            float core = smoothstep(0.3, 0.12, d), halo = 0.35 * smoothstep(1.4, 0.0, d), ring = (li == u_sel) ? smoothstep(0.06, 0.0, abs(d - 0.5)) : 0.0;
            vec3 col = (li == u_sel) ? vec3(1.0) : normalize(u_lc[li].rgb + 1e-4) ;
            float vis = t > surf + 0.3 ? 0.25 : 1.0;                 // behind a wall: dimmed, so it can still be found
            mk += (col * (core + halo) + vec3(1.0, 0.9, 0.2) * ring) * vis;
        }
    }
)GLSL";
const char* const k_rt_lights = R"GLSL(
    if (u_n_lights > 0 && u_use_rt != 0) {
        vec3 Pw = (u_view_inv * vec4(P, 1.0)).xyz;
        vec3 nw = normalize((u_view_inv * vec4(n, 0.0)).xyz);
        vec3 acc = vec3(0.0), acc_fx = vec3(0.0);
        for (int li = 0; li < u_n_lights; ++li) {
            vec3 d = u_lp[li].xyz - Pw;
            float dist = length(d), rad = u_lp[li].w;
            if (dist >= rad || dist < 1e-3) continue;
            vec3 L = d / dist;
            float ndl = max(dot(nw, L), 0.0);
            if (li >= u_fx0 && u_fx_split != 0 && v_uv.x > 0.5) continue;
            float x = 1.0 - dist / rad, att = li >= u_fx0 ? x : x * x;   // an attack's flash carries further than a lamp's pool
            if (ndl * att < 0.002) continue;
            rayQueryEXT rl;
            rayQueryInitializeEXT(rl, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0x1, Pw + nw * 0.06, 0.0, L, max(dist - 0.1 - u_lc[li].w, 0.01));
            while (rayQueryProceedEXT(rl)) {}
            if (rayQueryGetIntersectionTypeEXT(rl, true) != gl_RayQueryCommittedIntersectionNoneEXT) continue;
            if (li >= u_fx0) acc_fx += u_lc[li].rgb * (ndl * att); else acc += u_lc[li].rgb * (ndl * att);
        }
        vec3 alb = texture(u_color, v_uv).rgb;
        // an attack's flash lights dark surfaces too (the scene colour is the albedo here, and a dark floor took almost nothing): effect lights
        // see at least u_fx_alb
        o_refl.rgb += (alb * acc + max(alb, vec3(u_fx_alb)) * acc_fx) * fd * u_lscale;
    }
    o_refl.rgb += mk * u_lscale;
)GLSL";
const char* const k_rt_code = R"GLSL(
            if (u_use_rt != 0) {
                vec3 Ow = (u_view_inv * vec4(O, 1.0)).xyz;
                vec3 lw = u_light_w;
                vec3 lu = normalize(abs(lw.y) < 0.99 ? cross(lw, vec3(0.0, 1.0, 0.0)) : cross(lw, vec3(1.0, 0.0, 0.0))), lv = cross(lw, lu);
                float blocked = 0.0;
                for (int r = 0; r < u_rt_rays; ++r) {
                    vec3 ld = lw;
                    if (u_rt_rays > 1) {
                        float a = (float(r) + jit) / float(u_rt_rays) * 6.2831853 + jit * 6.2831853, rad = sqrt(fract(jit * 7.31 + float(r) * 0.618034)) * u_rt_soft;
                        ld = normalize(lw + lu * (cos(a) * rad) + lv * (sin(a) * rad));
                    }
                    rayQueryEXT rq;
                    rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0x1, Ow, 0.0, ld, u_rt_len);
                    while (rayQueryProceedEXT(rq)) {}
                    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) blocked += 1.0;
                }
                if (blocked > 0.0) sh = facing * fd * blocked / float(u_rt_rays);
            } else
)GLSL";
static int env_int_dbg() { const char* e = std::getenv("PSPRECOMP_RT_REFL_DEBUG"); return e ? (std::atoi(e) == 2 ? 2 : 1) : 0; }
// sky: the variant for ray-traced reflections, which reads the sky map (u_env). The Vulkan layer has 4 sampler bindings per
// program, so that variant gives the screen-space effects layer's two samplers to the sky map (the effects layer is never on
// together with ray-traced reflections: have_fx). The other variant has no sky map (u_env_on is 0 there).
std::string rt_main_source(bool sky) {
    std::string s = k_fs_main;
    auto rep = [&](const char* marker, const char* with) { size_t i = s.find(marker); if (i != std::string::npos) s.replace(i, std::strlen(marker), with); };
    if (sky) {
        rep("uniform sampler2D u_fxc;", "uniform sampler2D u_env;");
        rep("uniform sampler2D u_fxd;", "#define u_fxc u_env\n#define u_fxd u_env");
    }
    rep("//@RT_DECL", k_rt_decl);
    rep("//@SKY_DECL", sky ? "" : "#define u_env u_color");
    rep("//@RT_CODE", k_rt_code);
    rep("//@RT_REFL", k_rt_refl);
    rep("//@RT_LIGHTS", k_rt_lights);
    rep("//@RT_MARK", k_rt_mark);
    return s;
}
GLuint g_vao = 0, g_smp_nn = 0, g_smp_lin = 0;

// Render targets, sized to the scene buffer (depth) and half of it (everything else).
int g_w = 0, g_h = 0, g_hw = 0, g_hh = 0;
GLuint g_depth_tex = 0, g_depth_fbo = 0;
GLuint g_color_tex = 0, g_color_fbo = 0;                     // half-res copy of the scene
GLuint g_fx_tex[2] = {0, 0}, g_refl_tex = 0;
GLuint g_main_fbo = 0, g_fx_fbo[2] = {0, 0};                 // main: fx[0] + refl; fx_fbo[i]: fx[i] alone
GLuint g_refl_fbo = 0;                                        // refl alone (the glossy blur's second pass)
GLuint g_rm_fbo = 0, g_rm_tex = 0, g_rm_depth = 0;            // reflectivity mask, full resolution (k_vs_rmask)
GLuint g_rm_vao = 0, g_rm_vbo = 0;
GLuint g_env_tex = 0, g_env_fbo = 0;                          // the sky map (k_fs_env), not tied to the screen size
bool g_env_fp16 = false;
int g_env_stage = -2;
constexpr int kEnvW = 512, kEnvH = 256;

GLuint compile(GLenum type, const char* const* body, int n) {
    const char* src[6];
    int k = 0;
    src[k++] = PSP_GLSL_VERSION;
    for (int i = 0; i < n; ++i) src[k++] = body[i];
    GLuint s = glCreateShader(type);
    glShaderSource(s, k, src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[SCREENFX] shader failed:\n%s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool build(Prog& p, const char* fs_body) {
    if (p.id) return true;
    if (p.failed) return false;
    const char* vsrc[1] = {k_vs};
    const char* fsrc[2] = {k_common, fs_body};
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc, 1), fs = vs ? compile(GL_FRAGMENT_SHADER, fsrc, 2) : 0;
    if (!vs || !fs) { p.failed = true; if (vs) glDeleteShader(vs); return false; }
    GLuint id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glLinkProgram(id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(id, sizeof log, nullptr, log);
        std::fprintf(stderr, "[SCREENFX] link failed:\n%s\n", log);
        glDeleteProgram(id);
        p.failed = true;
        return false;
    }
    p.id = id;
    return true;
}

// A program from its own vertex and fragment shader (no k_common): the reflectivity mask.
bool build_vf(Prog& p, const char* vs_body, const char* fs_body) {
    if (p.id) return true;
    if (p.failed) return false;
    GLuint vs = compile(GL_VERTEX_SHADER, &vs_body, 1), fs = vs ? compile(GL_FRAGMENT_SHADER, &fs_body, 1) : 0;
    if (!vs || !fs) { p.failed = true; if (vs) glDeleteShader(vs); return false; }
    GLuint id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glBindAttribLocation(id, 0, "a_clip");   // the Vulkan layer numbers inputs in declaration order: the same
    glBindAttribLocation(id, 1, "a_mat");
    glLinkProgram(id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(id, sizeof log, nullptr, log);
        std::fprintf(stderr, "[SCREENFX] link failed:\n%s\n", log);
        glDeleteProgram(id);
        p.failed = true;
        return false;
    }
    p.id = id;
    return true;
}

GLuint make_tex(GLenum ifmt, GLenum fmt, GLenum type, int w, int h, GLint filter) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void free_targets() {
    GLuint f[7] = {g_depth_fbo, g_color_fbo, g_main_fbo, g_fx_fbo[0], g_fx_fbo[1], g_refl_fbo, g_rm_fbo};
    GLuint t[7] = {g_depth_tex, g_color_tex, g_fx_tex[0], g_fx_tex[1], g_refl_tex, g_rm_tex, g_rm_depth};
    for (int i = 0; i < 7; ++i) {
        if (f[i]) glDeleteFramebuffers(1, &f[i]);
        if (t[i]) glDeleteTextures(1, &t[i]);
    }
    g_depth_fbo = g_color_fbo = g_main_fbo = g_fx_fbo[0] = g_fx_fbo[1] = g_refl_fbo = g_rm_fbo = 0;
    g_rm_tex = g_rm_depth = 0;
    g_depth_tex = g_color_tex = g_fx_tex[0] = g_fx_tex[1] = g_refl_tex = 0;
}

bool g_fp16 = false;

bool complete() { return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE; }

bool ensure_targets(int w, int h) {
    const bool fp16 = psp_hdr_scene_live();   // HDR Scene: the colour copies / effect buffers are fp16 so values above 1.0 survive
    if (g_w == w && g_h == h && g_depth_fbo && g_fp16 == fp16) return true;
    free_targets();
    g_fp16 = fp16;
    g_w = w; g_h = h; g_hw = (w + 1) / 2; g_hh = (h + 1) / 2;
    g_depth_tex = make_tex(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, w, h, GL_NEAREST);
    glGenFramebuffers(1, &g_depth_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_depth_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_depth_tex, 0);
    bool ok = complete();
    g_color_tex = make_tex(g_fp16 ? GL_RGBA16F : GL_RGBA8, GL_RGBA, g_fp16 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, g_hw, g_hh, GL_LINEAR);
    glGenFramebuffers(1, &g_color_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_color_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color_tex, 0);
    ok = ok && complete();
    for (int i = 0; i < 2; ++i) {
        g_fx_tex[i] = make_tex(g_fp16 ? GL_RGBA16F : GL_RGBA8, GL_RGBA, g_fp16 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, g_hw, g_hh, GL_LINEAR);
        glGenFramebuffers(1, &g_fx_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fx_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fx_tex[i], 0);
        ok = ok && complete();
    }
    g_refl_tex = make_tex(g_fp16 ? GL_RGBA16F : GL_RGBA8, GL_RGBA, g_fp16 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, g_hw, g_hh, GL_LINEAR);
    glGenFramebuffers(1, &g_main_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_main_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fx_tex[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_refl_tex, 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    ok = ok && complete();
    glGenFramebuffers(1, &g_refl_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_refl_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_refl_tex, 0);
    ok = ok && complete();
    g_rm_tex = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h, GL_NEAREST);
    g_rm_depth = make_tex(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, w, h, GL_NEAREST);
    glGenFramebuffers(1, &g_rm_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_rm_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rm_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_rm_depth, 0);
    ok = ok && complete();
    if (!ok) std::fprintf(stderr, "[SCREENFX] framebuffer incomplete (%dx%d)\n", w, h);
    return ok;
}

// column-major 4x4 inverse (general)
bool invert4(const float* m, float* out) {
    double a[16];
    for (int i = 0; i < 16; ++i) a[i] = m[i];
    double inv[16];
    inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
    double det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (std::fabs(det) < 1e-30) return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i) out[i] = float(inv[i] * det);
    return true;
}

void normalize3(float* v) {
    float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// view matrix (PSP 4x3, columns of 3) applied to a world direction
void view_dir(const float* vm, const float* d, float* out) {
    for (int r = 0; r < 3; ++r) out[r] = vm[r] * d[0] + vm[3 + r] * d[1] + vm[6 + r] * d[2];
    normalize3(out);
}

struct Common {
    GLint depth, size, step, proj, inv, dr, clearz, sky, fade;
};
Common common_locs(GLuint p) {
    Common c;
    c.depth = glGetUniformLocation(p, "u_depth");
    c.size = glGetUniformLocation(p, "u_size");
    c.step = glGetUniformLocation(p, "u_step");
    c.proj = glGetUniformLocation(p, "u_proj");
    c.inv = glGetUniformLocation(p, "u_inv");
    c.dr = glGetUniformLocation(p, "u_dr");
    c.clearz = glGetUniformLocation(p, "u_clearz");
    c.sky = glGetUniformLocation(p, "u_sky");
    c.fade = glGetUniformLocation(p, "u_fade");
    return c;
}

#ifndef PSP_GLES
// per-pass GPU timestamps (PSPRECOMP_FX_DEBUG): marks 0 start, 1 copies, 2 main pass, 3 blur, 4 AO/shadow composite, 5 reflection composite
GLuint g_ts[6] = {0, 0, 0, 0, 0, 0};
bool g_ts_busy = false;
double g_ts_sum[5] = {0, 0, 0, 0, 0};
int g_ts_n = 0;
void ts_begin() {
    static const bool on = std::getenv("PSPRECOMP_FX_DEBUG") != nullptr;
    if (!on) return;
    if (!g_ts[0]) glGenQueries(6, g_ts);
    if (g_ts_busy) {
        GLuint ok = 0;
        glGetQueryObjectuiv(g_ts[5], GL_QUERY_RESULT_AVAILABLE, &ok);
        if (ok) {
            GLuint64 t[6];
            for (int i = 0; i < 6; ++i) glGetQueryObjectui64v(g_ts[i], GL_QUERY_RESULT, &t[i]);
            for (int i = 0; i < 5; ++i) g_ts_sum[i] += double(t[i + 1] - t[i]) * 1e-6;
            if (++g_ts_n == 120) {
                std::fprintf(stderr, "[SCREENFX] passes ms: copies %.2f main %.2f blur %.2f mul %.2f refl %.2f\n", g_ts_sum[0] / 120, g_ts_sum[1] / 120,
                             g_ts_sum[2] / 120, g_ts_sum[3] / 120, g_ts_sum[4] / 120);
                for (double& v : g_ts_sum) v = 0;
                g_ts_n = 0;
            }
            g_ts_busy = false;
        }
    }
    if (!g_ts_busy) { glQueryCounter(g_ts[0], GL_TIMESTAMP); g_ts_busy = true; }
}
void ts_mark(int i) {
    static const bool on = std::getenv("PSPRECOMP_FX_DEBUG") != nullptr;
    if (on && g_ts_busy && g_ts[0] && i > 0) glQueryCounter(g_ts[i], GL_TIMESTAMP);
}
#else
inline void ts_begin() {}   // GLES has no timestamp queries here
inline void ts_mark(int) {}
#endif

}  // namespace

int psp_fx_ssao() {
    int v = g_ao.load();
    if (v < 0) { v = env_level("PSPRECOMP_SSAO"); g_ao.store(v); }
    return v;
}
void psp_fx_set_ssao(int level) { g_ao.store(level < 0 ? 0 : level > 3 ? 3 : level); }
int psp_fx_contact() {
    int v = g_cs.load();
    if (v < 0) { v = env_level("PSPRECOMP_CONTACT_SHADOWS"); g_cs.store(v); }
    return v;
}
void psp_fx_set_contact(int level) { g_cs.store(level < 0 ? 0 : level > 3 ? 3 : level); }
int psp_fx_ssr() {
    int v = g_sr.load();
    if (v < 0) { v = env_level("PSPRECOMP_SSR"); g_sr.store(v); }
    return v;
}
void psp_fx_set_ssr(int level) { g_sr.store(level < 0 ? 0 : level > 3 ? 3 : level); }

bool psp_screenfx_active() { return psp_fx_ssao() || psp_fx_contact() || psp_fx_ssr() || psp_rt_active() || psp_rt_refl_active(); }

void psp_screenfx_run(GLuint fbo, GLuint color_tex, int w, int h, const ScreenFxScene& sc) {
    psp_perf::Scope perf_scope_(psp_perf::SCREENFX);
    (void)color_tex;
#ifndef PSP_GLES
    ts_begin();
#endif
    if (!build(g_main_p, k_fs_main) || !build(g_blur_p, k_fs_blur) || !build(g_mul_p, k_fs_mul) || !build(g_refl_p, k_fs_refl)) return;
    if (!ensure_targets(w, h)) return;
    if (!g_vao) {
        glGenVertexArrays(1, &g_vao);
        glGenSamplers(1, &g_smp_nn);
        glGenSamplers(1, &g_smp_lin);
        for (int i = 0; i < 2; ++i) {
            GLuint s = i ? g_smp_lin : g_smp_nn;
            GLint f = i ? GL_LINEAR : GL_NEAREST;
            glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, f);
            glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, f);
            glSamplerParameteri(s, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(s, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    static const bool dbg_print = std::getenv("PSPRECOMP_FX_DEBUG") != nullptr;
    float inv[16];
    if (!invert4(sc.proj, inv)) return;
    const bool rt_refl_on = psp_rt_refl_active();
    const int ao = psp_fx_ssao(), sr = rt_refl_on ? 3 : psp_fx_ssr();   // ray-traced reflections replace the screen-space ones (full strength)
    const int cs = psp_rt_active() ? 3 : psp_fx_contact();   // ray-traced shadows replace the contact shadows (full contact-shadow strength; a ray query has no sample count)
    static const float k_ao_amt[4] = {0.f, 0.55f, 0.8f, 1.0f};
    static const float k_cs_amt[4] = {0.f, 0.3f, 0.45f, 0.6f};
    static const float k_sr_amt[4] = {0.f, 0.45f, 0.7f, 1.0f};
    // samples per effect and level: the cost of the pass is mostly these loops
    static const int k_n_ao[4] = {0, 5, 8, 12};
    static const int k_n_cs[4] = {0, 6, 10, 14};
    static const int k_n_ssr[4] = {0, 10, 16, 22};
    const float radius = env_float("PSPRECOMP_SSAO_RADIUS", 0.09f);
    const float cs_len = env_float("PSPRECOMP_CONTACT_LEN", 2.5f);       // view units (a fighter is about 2.4 tall)
    const float cs_thick = env_float("PSPRECOMP_CONTACT_THICK", 0.5f);
    // The sky dome is real geometry far away (about 250 units here, the farthest floor and ruins about 80): everything
    // past PSPRECOMP_FX_SKY_DIST view units counts as background. The depth_t that distance has under this projection:
    const float fade0 = env_float("PSPRECOMP_FX_FADE_START", 30.f), fade1 = env_float("PSPRECOMP_FX_FADE_END", 70.f);
    float sky_t = 0.97f;
    {
        const float D = env_float("PSPRECOMP_FX_SKY_DIST", 150.f), zv = -D;
        const float den = sc.proj[11] * zv;
        if (std::fabs(den) > 1e-6f) {
            const float ndc = (sc.proj[10] * zv + sc.proj[14]) / den;
            sky_t = ndc * 0.5f + 0.5f;
        }
    }
    const float dark = psp_rt_active() ? stage_dark() : 1.f;   // stage looks ride on the ray-traced lighting
    int n_lights = 0;
    float lp4[kMaxLights * 4], lc4[kMaxLights * 4], lv4[kMaxLights * 4];
    if (sc.rt_ready && cs > 0 && psp_rt_enabled()) {   // stage lights need the ray-traced scene for their shadows
        const int sid = psp_stage_id();
        lights_service(sid, sc.view);
        std::vector<StageLight> all;
        if (g_pickup_fn) {   // pickups that glow (the game module's EX cores): a warm light each, first so the stage's own lights cannot crowd them out
            static const float pc[5] = {env_float("PSPRECOMP_PICKUP_R", 0.35f), env_float("PSPRECOMP_PICKUP_G", 0.75f), env_float("PSPRECOMP_PICKUP_B", 1.0f),
                                        env_float("PSPRECOMP_PICKUP_RADIUS", 22.f), env_float("PSPRECOMP_PICKUP_INTENSITY", 1.5f)};
            float P[4][3]; const int n = g_pickup_fn(P, 4);
            for (int i = 0; i < n; ++i) all.push_back({{P[i][0], P[i][1], P[i][2]}, {pc[0], pc[1], pc[2]}, pc[3], pc[4], env_float("PSPRECOMP_PICKUP_CLEAR", 2.5f)});
        }
        g_light_first = (int)all.size();   // the editor's markers / selection start at the stage's own lights
        for (const StageLight& l : stage_lights(sid)) all.push_back(l);
        g_light_end = (int)all.size();
        for (int i = 0; i < sc.n_fx_lights; ++i) {   // glowing effects (attacks) fill the slots left: their colour, reach / brightness from their size
            const float* f = sc.fx_lights[i];
            all.push_back({{f[0], f[1], f[2]}, {f[3], f[4], f[5]}, f[6], f[7], f[8]});
        }
        for (const StageLight& l : all) {
            if (n_lights >= kMaxLights) break;
            for (int k = 0; k < 3; ++k) { lp4[n_lights * 4 + k] = l.p[k]; lc4[n_lights * 4 + k] = l.c[k] * l.intensity; }
            lp4[n_lights * 4 + 3] = l.radius; lc4[n_lights * 4 + 3] = l.clear;
            for (int k = 0; k < 3; ++k) lv4[n_lights * 4 + k] = sc.view[k] * l.p[0] + sc.view[3 + k] * l.p[1] + sc.view[6 + k] * l.p[2] + sc.view[9 + k];
            lv4[n_lights * 4 + 3] = 1.f;
            ++n_lights;
        }
    }
    const float ssr_len = env_float("PSPRECOMP_SSR_LEN", 0.6f);
    // Light: mostly overhead (world space, so the shadows stay put while the camera moves), a little to one side.
    float wl[3] = {env_float("PSPRECOMP_LIGHT_X", 0.4f), env_float("PSPRECOMP_LIGHT_Y", 1.0f), env_float("PSPRECOMP_LIGHT_Z", 0.3f)};
    float lv[3], up[3], wup[3] = {0.f, 1.f, 0.f};
    view_dir(sc.view, wl, lv);
    float wl_n[3] = {wl[0], wl[1], wl[2]};
    { const float l = std::sqrt(wl[0] * wl[0] + wl[1] * wl[1] + wl[2] * wl[2]); if (l > 1e-6f) for (float& c : wl_n) c /= l; }
    bool use_rt = false;
    const bool use_rt_sh = sc.rt_ready && cs > 0 && psp_rt_enabled(), use_rt_refl = sc.rt_ready && rt_refl_on;
    if (use_rt_sh || use_rt_refl) {
        Prog& rp = use_rt_refl ? g_main_rt_sky_p : g_main_rt_p;
        if (!rp.id && !rp.failed) { const std::string src = rt_main_source(use_rt_refl); build(rp, src.c_str()); }
        use_rt = rp.id != 0;
    }
    view_dir(sc.view, wup, up);
    if (dbg_print) {
        static int n = 0;
        if (n++ % 120 == 0) {
            std::fprintf(stderr, "[SCREENFX] %dx%d near/far %.4f %.4f clear %.4f\n  P:", w, h, sc.near_z, sc.far_z, sc.clear_z);
            for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %g", sc.proj[i]);
            std::fprintf(stderr, "\n  V:");
            for (int i = 0; i < 12; ++i) std::fprintf(stderr, " %g", sc.view[i]);
            std::fprintf(stderr, "\n  light(view) %g %g %g up(view) %g %g %g\n", lv[0], lv[1], lv[2], up[0], up[1], up[2]);
        }
    }

    // copies of the scene's depth (full resolution) and colour (half resolution, only the reflections read it)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_depth_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (sr) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_color_fbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, g_hw, g_hh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    ts_mark(1);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(g_vao);

    auto bind_tex = [&](int unit, GLuint tex, GLuint smp) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(unit, smp);
    };
    auto set_common = [&](GLuint prog, int pw, int ph) {
        const Common c = common_locs(prog);
        glUseProgram(prog);
        bind_tex(0, g_depth_tex, g_smp_nn);
        glUniform1i(c.depth, 0);
        glUniform2f(c.size, float(w), float(h));
        glUniform2f(c.step, 1.f / float(pw), 1.f / float(ph));
        glUniformMatrix4fv(c.proj, 1, GL_FALSE, sc.proj);
        glUniformMatrix4fv(c.inv, 1, GL_FALSE, inv);
        glUniform2f(c.dr, sc.near_z, sc.far_z);
        glUniform1f(c.clearz, sc.clear_z);
        glUniform1f(c.sky, sky_t);
        glUniform2f(c.fade, fade0, fade1);
        static const int depth_snap = std::getenv("PSPRECOMP_FX_DEPTH_SNAP") ? std::atoi(std::getenv("PSPRECOMP_FX_DEPTH_SNAP")) : 1;
        glUniform1i(glGetUniformLocation(prog, "u_depth_snap"), depth_snap);
        static const int snap_split = std::getenv("PSPRECOMP_FX_SNAP_SPLIT") ? 1 : 0;
        glUniform1i(glGetUniformLocation(prog, "u_snap_split"), snap_split);
    };
    auto loc = [](GLuint p, const char* n) { return glGetUniformLocation(p, n); };

    float vm[16] = {sc.view[0], sc.view[1], sc.view[2], 0.f, sc.view[3], sc.view[4], sc.view[5], 0.f,
                    sc.view[6], sc.view[7], sc.view[8], 0.f, sc.view[9], sc.view[10], sc.view[11], 1.f};
    // 0a: the reflectivity mask into the alpha of the half-resolution colour copy (k_fs_mask)
    const bool mask_on = sr && sc.refl_table && build(g_mask_p, k_fs_mask);
    const bool have_mask = mask_on && sc.refl_tris && sc.refl_nverts >= 3 && build_vf(g_rmask_p, k_vs_rmask, k_fs_rmask);
    if (have_mask) {   // the reflective triangles into the mask, one draw (depth range and test as the scene's draws had them)
        if (!g_rm_vao) { glGenVertexArrays(1, &g_rm_vao); glGenBuffers(1, &g_rm_vbo); }
        glBindFramebuffer(GL_FRAMEBUFFER, g_rm_fbo);
        glViewport(0, 0, w, h);
        glDepthMask(GL_TRUE);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClearDepth(sc.clear_z);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(sc.near_z > sc.far_z ? GL_GEQUAL : GL_LEQUAL);
        glDepthRange(sc.near_z, sc.far_z);
        glUseProgram(g_rmask_p.id);
        glUniform1f(glGetUniformLocation(g_rmask_p.id, "u_byte"), 1.f / 255.f);
        glBindVertexArray(g_rm_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_rm_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sc.refl_nverts * 5 * sizeof(float), sc.refl_tris, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void*)(4 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, sc.refl_nverts);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_DEPTH_TEST);
        glDepthRange(0.0, 1.0);
        glBindVertexArray(g_vao);
    }
    if (mask_on) {
        psp_refl_poll();   // reflectivity.txt edited while the game runs
        glBindFramebuffer(GL_FRAMEBUFFER, g_color_fbo);
        glViewport(0, 0, g_hw, g_hh);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        set_common(g_mask_p.id, g_hw, g_hh);
        const bool have = have_mask;
        if (have) {
            bind_tex(1, g_rm_tex, g_smp_nn);
            bind_tex(2, g_rm_depth, g_smp_nn);
            glUniform1i(loc(g_mask_p.id, "u_mask"), 1);
            glUniform1i(loc(g_mask_p.id, "u_mdepth"), 2);
        }
        glUniform1i(loc(g_mask_p.id, "u_have"), have ? 1 : 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    // 0: the sky map (ray-traced reflections only), from this frame's sky pixels
    bool have_env = false;
    if (use_rt && use_rt_refl && build(g_env_p, k_fs_env)) {
        const int sid = psp_stage_id();
        if (!g_env_fbo || g_env_fp16 != g_fp16) {
            if (g_env_fbo) glDeleteFramebuffers(1, &g_env_fbo);
            if (g_env_tex) glDeleteTextures(1, &g_env_tex);
            g_env_fp16 = g_fp16;
            g_env_tex = make_tex(g_fp16 ? GL_RGBA16F : GL_RGBA8, GL_RGBA, g_fp16 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, kEnvW, kEnvH, GL_LINEAR);
            glGenFramebuffers(1, &g_env_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, g_env_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_env_tex, 0);
            if (!complete()) { std::fprintf(stderr, "[SCREENFX] sky map framebuffer incomplete\n"); glDeleteFramebuffers(1, &g_env_fbo); glDeleteTextures(1, &g_env_tex); g_env_fbo = g_env_tex = 0; }
            g_env_stage = -2;
        }
        if (g_env_fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_env_fbo);
            glViewport(0, 0, kEnvW, kEnvH);
            if (sid != g_env_stage) {   // a new stage (or none): forget the old sky
                glClearColor(0.f, 0.f, 0.f, 0.f);
                glClear(GL_COLOR_BUFFER_BIT);
                g_env_stage = sid;
            }
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            set_common(g_env_p.id, kEnvW, kEnvH);
            bind_tex(1, g_color_tex, g_smp_lin);
            glUniform1i(loc(g_env_p.id, "u_color"), 1);
            glUniformMatrix4fv(loc(g_env_p.id, "u_view"), 1, GL_FALSE, vm);
            glUniform1f(loc(g_env_p.id, "u_w"), env_float("PSPRECOMP_RT_SKY_MAP_W", 0.25f));
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDisable(GL_BLEND);
            have_env = !std::getenv("PSPRECOMP_RT_SKY_MAP_OFF");
        }
    }

    // 1: AO + contact shadow + reflection (half resolution)
    Prog& mp = !use_rt ? g_main_p : use_rt_refl ? g_main_rt_sky_p : g_main_rt_p;
    glBindFramebuffer(GL_FRAMEBUFFER, g_main_fbo);
    glViewport(0, 0, g_hw, g_hh);
    set_common(mp.id, g_hw, g_hh);
    bind_tex(1, g_color_tex, g_smp_lin);
    glUniform1i(loc(mp.id, "u_color"), 1);
    glUniform1f(loc(mp.id, "u_radius"), radius);
    glUniform1i(loc(mp.id, "u_n_ao"), k_n_ao[ao]);
    glUniform1i(loc(mp.id, "u_n_cs"), k_n_cs[cs]);
    glUniform1i(loc(mp.id, "u_n_ssr"), k_n_ssr[sr]);
    glUniform3f(loc(mp.id, "u_light"), lv[0], lv[1], lv[2]);
    glUniform3f(loc(mp.id, "u_up"), up[0], up[1], up[2]);
    glUniform1f(loc(mp.id, "u_cs_len"), cs_len);
    glUniform1f(loc(mp.id, "u_cs_thick"), cs_thick);
    glUniform1f(loc(mp.id, "u_ssr_len"), ssr_len);
    glUniform1f(loc(mp.id, "u_ssr_amt"), k_sr_amt[sr]);
    glUniform1i(loc(mp.id, "u_mask_on"), mask_on ? 1 : 0);
    glUniform1f(loc(mp.id, "u_wave"), env_float("PSPRECOMP_REFL_WAVE", 0.8f));
    glUniform1f(loc(mp.id, "u_wave_max"), env_float("PSPRECOMP_REFL_WAVE_MAX", 0.08f));
    glUniform1i(loc(mp.id, "u_mask_dbg"), std::getenv("PSPRECOMP_REFL_MASK_DEBUG") ? 1 : 0);
    static const bool fxl_off = std::getenv("PSPRECOMP_SSR_FX_OFF") != nullptr;
    const bool have_fx = sr && sc.fx_color && sc.fx_depth && !use_rt_refl && !fxl_off;
    glUniform1i(loc(mp.id, "u_have_fx"), have_fx ? 1 : 0);
    glUniform1i(loc(mp.id, "u_fx_dbg"), std::getenv("PSPRECOMP_SSR_FX_DEBUG") ? std::atoi(std::getenv("PSPRECOMP_SSR_FX_DEBUG")) : 0);
    if (have_fx) {
        bind_tex(2, sc.fx_color, g_smp_lin);
        bind_tex(3, sc.fx_depth, g_smp_nn);
        glUniform1i(loc(mp.id, "u_fxc"), 2);
        glUniform1i(loc(mp.id, "u_fxd"), 3);
        glUniform1f(loc(mp.id, "u_fx_gain"), env_float("PSPRECOMP_SSR_FX_GAIN", 0.4f));
        glUniform1f(loc(mp.id, "u_fx_thick"), env_float("PSPRECOMP_SSR_FX_THICK", 0.6f));
    }
    if (use_rt) {
        glUniform1i(loc(mp.id, "u_use_rt"), use_rt_sh ? 1 : 0);
        glUniform1i(loc(mp.id, "u_use_rt_refl"), use_rt_refl ? 1 : 0);
        glUniform1i(loc(mp.id, "u_rt_dbg"), env_int_dbg());
        { const int q = psp_rt_refl_quality(); glUniform1i(loc(mp.id, "u_rt_refl_rays"), q >= 3 ? 3 : 1); }
        glUniform1f(loc(mp.id, "u_rt_rough"), env_float("PSPRECOMP_RT_ROUGH", 0.005f));
        glUniform1f(loc(mp.id, "u_rough_jit"), env_float("PSPRECOMP_REFL_ROUGH_JITTER", 0.12f));
        glUniform1i(loc(mp.id, "u_rt_fx"), !sc.rt_fx || psp_rt_refl_quality() < 2 || !psp_rt_refl_fx_enabled() || std::getenv("PSPRECOMP_RT_FX_OFF") ? 0 : std::getenv("PSPRECOMP_RT_FX_SPLIT") ? 2 : 1);
        glUniform1f(loc(mp.id, "u_rt_hidden"), env_float("PSPRECOMP_RT_HIDDEN", 0.45f));
        glUniform1f(loc(mp.id, "u_fx_minh"), env_float("PSPRECOMP_RT_FX_MINH", 0.6f));
        glUniform1f(loc(mp.id, "u_fx_gain"), env_float("PSPRECOMP_SSR_FX_GAIN", 0.4f));
        glUniform1f(loc(mp.id, "u_rt_sky_amt"), env_float("PSPRECOMP_RT_SKY_AMT", 0.4f));
        glUniform1f(loc(mp.id, "u_rt_sky_dist"), env_float("PSPRECOMP_FX_SKY_DIST", 150.f));
        glUniform1i(loc(mp.id, "u_refl_split"), std::getenv("PSPRECOMP_RT_REFL_SPLIT") ? 1 : 0);
        glUniform1f(loc(mp.id, "u_rt_refl_len"), env_float("PSPRECOMP_RT_REFL_LEN", 300.f));
        glUniform3f(loc(mp.id, "u_rt_sky"), env_float("PSPRECOMP_RT_SKY_R", 0.45f), env_float("PSPRECOMP_RT_SKY_G", 0.55f), env_float("PSPRECOMP_RT_SKY_B", 0.75f));
        float vinv[16];
        // the sky map, or a blank one (alpha 0 = never seen: the old fallback) when it is off
        if (use_rt_refl) { bind_tex(4, have_env ? g_env_tex : g_fx_tex[1], g_smp_lin); glUniform1i(loc(mp.id, "u_env"), 4); }
        glUniform1i(loc(mp.id, "u_env_on"), have_env ? 1 : 0);
        if (!invert4(vm, vinv)) std::memset(vinv, 0, sizeof vinv);
        glUniformMatrix4fv(loc(mp.id, "u_view_inv"), 1, GL_FALSE, vinv);
        glUniform3f(loc(mp.id, "u_light_w"), wl_n[0], wl_n[1], wl_n[2]);
        glUniform1f(loc(mp.id, "u_rt_len"), env_float("PSPRECOMP_RT_LEN", 40.f));
        { const int q = psp_rt_quality(); glUniform1i(loc(mp.id, "u_rt_rays"), q >= 3 ? 6 : q == 2 ? 3 : 1); }
        glUniform1f(loc(mp.id, "u_rt_soft"), psp_rt_soft_angle());
        glUniform1i(loc(mp.id, "u_n_lights"), n_lights);
        if (n_lights) { glUniform4fv(loc(mp.id, "u_lp"), n_lights, lp4); glUniform4fv(loc(mp.id, "u_lc"), n_lights, lc4); glUniform4fv(loc(mp.id, "u_lvp"), n_lights, lv4); }
        glUniform1f(loc(mp.id, "u_lscale"), 1.f / std::max(0.05f, dark));
        static const bool mark_all = std::getenv("PSPRECOMP_FX_LIGHT_MARK") != nullptr;   // debug: markers on every light (EX cores, effects too)
        glUniform1i(loc(mp.id, "u_edit"), g_light_edit.load() || mark_all ? 1 : 0);
        { static const bool split = std::getenv("PSPRECOMP_FX_LIGHT_SPLIT") != nullptr; glUniform1i(loc(mp.id, "u_fx0"), g_light_end); glUniform1i(loc(mp.id, "u_fx_split"), split ? 1 : 0); glUniform1f(loc(mp.id, "u_fx_alb"), env_float("PSPRECOMP_FX_LIGHT_MIN_ALBEDO", 0.35f)); }
        glUniform1i(loc(mp.id, "u_sel"), g_light_sel + g_light_first); glUniform1i(loc(mp.id, "u_mark0"), mark_all ? 0 : g_light_first); glUniform1i(loc(mp.id, "u_mark1"), mark_all ? 64 : g_light_end);   // (the Vulkan GL layer has no glUniform2i)
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ts_mark(2);

    // 2: blur AO / shadow there and back
    if (ao || cs) {
        for (int pass = 0; pass < 2; ++pass) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_fx_fbo[1 - pass]);
            set_common(g_blur_p.id, g_hw, g_hh);
            bind_tex(1, g_fx_tex[pass], g_smp_lin);
            glUniform1i(loc(g_blur_p.id, "u_fx"), 1);
            glUniform2f(loc(g_blur_p.id, "u_dir"), pass ? 0.f : 1.f / g_hw, pass ? 1.f / g_hh : 0.f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    // 2b: glossy reflections: blur the reflection layer by the surface's roughness, there (into fx[1], free after the AO blur) and back
    static const bool rblur_off = std::getenv("PSPRECOMP_REFL_BLUR_OFF") != nullptr;
    if (mask_on && !rblur_off && build(g_rblur_p, k_fs_rblur)) {
        const float rad = env_float("PSPRECOMP_REFL_BLUR", 10.f) * float(g_hh) / 408.f;   // texels at roughness 1, scaled with the resolution
        for (int pass = 0; pass < 2; ++pass) {
            glBindFramebuffer(GL_FRAMEBUFFER, pass ? g_refl_fbo : g_fx_fbo[1]);
            set_common(g_rblur_p.id, g_hw, g_hh);
            bind_tex(1, pass ? g_fx_tex[1] : g_refl_tex, g_smp_lin);
            bind_tex(2, g_color_tex, g_smp_nn);
            glUniform1i(loc(g_rblur_p.id, "u_src"), 1);
            glUniform1i(loc(g_rblur_p.id, "u_color"), 2);
            glUniform2f(loc(g_rblur_p.id, "u_dir"), pass ? 0.f : 1.f / g_hw, pass ? 1.f / g_hh : 0.f);
            glUniform1f(loc(g_rblur_p.id, "u_rad"), rad);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    ts_mark(3);
    // 3: composite into the scene (RGB only: alpha is the GE's stencil)
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    if (ao || cs || dark < 0.999f) {
        glEnable(GL_BLEND);
        // debug PSPRECOMP_FX_SHOW=<s>: the AO x shadow factor alone, as the picture, from <s> seconds after the first 3D frame
        static const char* fx_show_e = std::getenv("PSPRECOMP_FX_SHOW");
        static const auto fx_t0 = std::chrono::steady_clock::now();
        const bool fx_show = fx_show_e && std::chrono::duration<double>(std::chrono::steady_clock::now() - fx_t0).count() >= std::atof(fx_show_e);
        if (fx_show) glBlendFunc(GL_ONE, GL_ZERO); else
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);              // dst * (AO * (1 - shadow))
        set_common(g_mul_p.id, w, h);
        bind_tex(1, g_fx_tex[0], g_smp_nn);
        glUniform1i(loc(g_mul_p.id, "u_fx"), 1);
        glUniform2f(loc(g_mul_p.id, "u_half"), float(g_hw), float(g_hh));
        glUniform1f(loc(g_mul_p.id, "u_ao_amt"), k_ao_amt[ao]);
        glUniform1f(loc(g_mul_p.id, "u_dark"), dark);
        glUniform1f(loc(g_mul_p.id, "u_cs_amt"), (std::getenv("PSPRECOMP_CONTACT_AMT") ? env_float("PSPRECOMP_CONTACT_AMT", 0.6f) : k_cs_amt[cs]));
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    ts_mark(4);
    if (sr || n_lights) {
        glEnable(GL_BLEND);
        static const bool refl_only = std::getenv("PSPRECOMP_REFL_ONLY") != nullptr;
        glBlendFunc(refl_only ? GL_ONE : GL_ONE, refl_only ? GL_ZERO : GL_ONE_MINUS_SRC_ALPHA);     // the reflection over the (shadowed) floor
        set_common(g_refl_p.id, w, h);
        bind_tex(1, g_refl_tex, g_smp_lin);
        glUniform1i(loc(g_refl_p.id, "u_refl"), 1);
        glUniform1f(loc(g_refl_p.id, "u_dbg"), refl_only ? 1.f : 0.f);
        glUniform1f(loc(g_refl_p.id, "u_dark"), dark);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    ts_mark(5);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(0);
    for (int u = 4; u >= 0; --u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindSampler(u, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUseProgram(0);
}
