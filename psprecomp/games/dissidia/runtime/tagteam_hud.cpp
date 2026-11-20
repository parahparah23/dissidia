// games/dissidia/runtime/tagteam_hud.cpp - the tag team battle's own HUD, drawn in the game's style on the game layer (menu_layer.h,
// ge_draw_set_game_layer) at the output resolution:
//   - a compact panel for each fighter who is not on the game's big panel: the character's face strip (the menus' "face in name" picture,
//     menu/jp/common/chara_face_in_name/p_<code>_face_in_name.gim, as on the Customization screen), the name in the game's bold face, a thin
//     HP bar in the retail HUD's blues and the HP / BRV numbers;
//   - the reserve of each side: the face strips of the members still to come, the next one in front;
//   - the "NAME JOINS THE BATTLE" banner.
// The data is handed in by tagteam.cpp (tagteam_hud_draw, once per tick) and drawn by tagteam_layer_frame (once per frame, with the
// effects of tagteam_fx.cpp on the same layer).
#include "tagteam_hud.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <map>
#include "gim_decode.h"
#include "menu_layer.h"
#include "package_mods.h"
#include "psp_ge_draw.h"
#include "hle/psp_hle_io.h"
#include "../../../runtime/src/hle/psp_savedata_dialog.h"

std::string dissidia_mods_dir();   // hooks_io.cpp

namespace {

using dissidia::LayerColor;
dissidia::MenuLayer g_layer;
bool g_layer_shown = false;
std::vector<TagHudFighter> g_fighters;
std::vector<TagHudReserve> g_reserves;
std::string g_banner;
struct Notice { std::string text; int kind; double until, start; };   // wall-clock seconds
std::vector<Notice> g_notices;
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- the characters' face strips (128 x 32 GIM; the picture is the top 26 rows) ----
struct Face { bool tried = false; dissidia::GimImage img; };
std::map<std::string, Face> g_faces;
const dissidia::GimImage* face(const std::string& code) {
    if (code.empty()) return nullptr;
    Face& f = g_faces[code];
    if (!f.tried) {
        f.tried = true;
        std::vector<uint8_t> b;
        const std::string path = "menu/jp/common/chara_face_in_name/p_" + code + "_face_in_name.gim";
        if (dissidia::read_asset(psp_path_to_host("disc0:/PSP_GAME/USRDIR/DATA"), dissidia_mods_dir(), path, &b) && !b.empty()) {
            std::string err;
            if (!dissidia::gim_decode(b.data(), b.size(), 0, &f.img, &err)) std::fprintf(stderr, "[TAGHUD] %s: %s\n", path.c_str(), err.c_str());
        } else std::fprintf(stderr, "[TAGHUD] %s: not read\n", path.c_str());
    }
    return f.img.w > 0 ? &f.img : nullptr;
}
// the face strip with its right end fading out (the strip's own art fades to the menu's plate; over the battle it ends softly)
void draw_face(const std::string& code, float x, float y, float w, float h, float alpha, bool mirror) {
    const dissidia::GimImage* g = face(code);
    if (!g) { g_layer.band(x, y, x + w, y + h, [&](float, float) { return LayerColor{40, 44, 70, 0.7f * alpha}; }); return; }
    const int sh = std::min(g->h, 26);
    (void)mirror;   // (the strip is not mirrored: negative sizes draw nothing; on the right side it simply sits at the panel's right end)
    g_layer.image(g->rgba.data(), g->w, g->h, 0, 0, g->w, sh, x, y, w, h, alpha);
}

// ---- pieces in the retail HUD's colours ----
const LayerColor kPlate{14, 16, 40, 0.62f}, kEdgeL{120, 170, 255, 0.95f}, kEdgeR{255, 120, 110, 0.95f};
void plate(float x0, float y0, float x1, float y1, float a, bool right) {   // a dark plate fading towards the screen centre
    g_layer.band(x0, y0, x1, y1, [&](float x, float) {
        const float t = right ? (x1 - x) / (x1 - x0) : (x - x0) / (x1 - x0);
        return LayerColor{kPlate.r, kPlate.g, kPlate.b, kPlate.a * a * (1.f - 0.65f * t)};
    });
}
void hp_bar(float x, float y, float w, float frac, bool right, float a) {   // the retail HP bar: cyan to deep blue, a light top line
    frac = std::clamp(frac, 0.f, 1.f);
    g_layer.band(x, y, x + w, y + 3.2f, [&](float, float) { return LayerColor{20, 24, 44, 0.85f * a}; });
    const float fx0 = right ? x + w * (1.f - frac) : x, fx1 = right ? x + w : x + w * frac;
    const bool low = frac < 0.25f;
    g_layer.band(fx0, y, fx1, y + 3.2f, [&](float xx, float yy) {
        const float t = (xx - x) / std::max(1.f, w), top = yy < y + 1.f ? 1.f : 0.f;
        if (low) return LayerColor{255.f, 150.f + 40.f * top, 60.f + 60.f * top, a};
        return LayerColor{40.f + 60.f * t + 80.f * top, 170.f + 50.f * t + 40.f * top, 255.f, a};
    });
}
void shadow_text(const std::string& s, bool bold, float em, float x, float base, LayerColor c) {
    for (const auto& o : {std::pair<float, float>{0.5f, 0.5f}, {0.5f, 0.f}, {0.f, 0.5f}, {-0.4f, 0.f}, {0.f, -0.4f}})
        g_layer.text(s, bold, em, x + o.first, base + o.second, LayerColor{10, 10, 30, 0.8f * c.a});
    g_layer.text(s, bold, em, x, base, c);
}
float text_w(const std::string& s, bool bold, float em) { return g_layer.text(s, bold, em, 0, 0, LayerColor{0, 0, 0, 0}, true); }

// the EX gauge as the retail HUD's vertical tube (purple, filling from the bottom), at the panel's outer edge. Full: gold with a glow (as retail);
// EX Mode (the gauge drains): pulsing magenta.
void ex_tube(float x, float y0, float y1, float frac, bool full, bool exmode, float a, double t) {
    frac = std::clamp(frac, 0.f, 1.f);
    const float w = 3.4f, h = y1 - y0, fy = y1 - h * frac;
    g_layer.band(x, y0, x + w, y1, [&](float xx, float) { const float e = std::fabs(xx - (x + w / 2.f)) / (w / 2.f); return LayerColor{26, 18, 52, (0.85f - 0.25f * e) * a}; });
    const float pulse = exmode ? 0.5f + 0.5f * static_cast<float>(std::sin(t * 9.0)) : 0.f;
    g_layer.band(x + 0.4f, fy, x + w - 0.4f, y1 - 0.3f, [&](float xx, float yy) {
        const float e = std::fabs(xx - (x + w / 2.f)) / (w / 2.f), k = (yy - fy) / std::max(1.f, y1 - fy), hi = 1.f - e;
        if (exmode) return LayerColor{230.f + 25.f * hi, 90.f + 120.f * pulse * hi, 230.f + 25.f * hi, a};
        if (full) return LayerColor{255.f, 170.f + 70.f * hi, 40.f + 120.f * hi, a};   // full: gold, as the retail tube
        return LayerColor{120.f + 60.f * hi - 30.f * k, 60.f + 60.f * hi - 20.f * k, 210.f + 45.f * hi, a};
    });
    if (full || exmode) {   // glow around the full tube
        const float ga = exmode ? 0.25f + 0.35f * pulse : 0.35f;
        g_layer.band(x - 1.2f, fy - 1.f, x + w + 1.2f, y1 + 0.5f, [&](float xx, float) { const float d = std::min(std::fabs(xx - x), std::fabs(xx - (x + w))); return LayerColor{255.f, exmode ? 120.f : 200.f, exmode ? 255.f : 80.f, (xx >= x && xx <= x + w) ? 0.f : ga * a * (1.f - std::min(1.f, d / 1.2f))}; });
    }
}
// the assist gauge as the retail HUD's two chevrons under the HP bar (each a level of 5000), dark red to orange; a full level brightens
void assist_chevrons(float x, float y, float w, float frac, bool right, bool has, float a) {
    frac = std::clamp(frac, 0.f, 1.f);
    const float h = 2.8f, gap = 1.6f, sw = (w - gap) / 2.f, sl = 1.6f;   // slant
    for (int s = 0; s < 2; s++) {
        const float sx = x + s * (sw + gap);
        const int lev = right ? 1 - s : s;   // levels fill from the panel's outer edge
        const float lf = std::clamp(frac * 2.f - lev, 0.f, 1.f);
        auto inside = [&](float xx, float yy) { const float d = (yy - y) / h * sl; const float l = right ? sx + sl - d : sx + d, r = l + sw - sl; return xx >= l && xx <= r; };
        g_layer.band(sx, y, sx + sw, y + h, [&](float xx, float yy) { return inside(xx, yy) ? LayerColor{18, 18, 36, 0.85f * a} : LayerColor{0, 0, 0, 0}; });
        if (lf <= 0.f) continue;
        const float fx0 = right ? sx + sw * (1.f - lf) : sx, fx1 = right ? sx + sw : sx + sw * lf, lit = lf >= 1.f ? 1.f : 0.f;
        g_layer.band(fx0, y, fx1, y + h, [&](float xx, float yy) {
            if (!inside(xx, yy)) return LayerColor{0, 0, 0, 0};
            const float t = (xx - sx) / sw, top = yy < y + 0.9f ? 1.f : 0.f, k = right ? 1.f - t : t;
            const float aa = (has ? 1.f : 0.45f) * a;
            return LayerColor{190.f + 50.f * k + 15.f * top + 10.f * lit, 40.f + 70.f * k + 60.f * top + 70.f * lit, 30.f + 20.f * top + 40.f * lit, aa};
        });
    }
}

// one compact panel: 112 x 24 design units (31 with the gauges), plus the EX tube at its outer edge
void panel(const TagHudFighter& f, float x, float y, double tnow) {
    const bool right = f.side != 0, ko = f.hp <= 0, gauges = f.ex >= 0.f || f.assist >= 0.f;
    const float w = 112.f, a = ko ? 0.55f : 1.f, H = gauges ? 31.f : 24.f;
    plate(x, y, x + w, y + H, 1.f, right);
    g_layer.band(right ? x + w - 1.2f : x, y, right ? x + w : x + 1.2f, y + H, [&](float, float) { const LayerColor c = right ? kEdgeR : kEdgeL; return LayerColor{c.r, c.g, c.b, c.a * a}; });
    if (f.ex >= 0.f) ex_tube(right ? x + w + 1.f : x - 4.4f, y + 0.5f, y + H - 0.5f, ko ? 0.f : f.ex, !ko && f.ex >= 0.999f, !ko && f.ex_mode, a, tnow);
    const float fw = 52.f, fh = 13.f, fx = right ? x + w - 2.f - fw : x + 2.f;
    draw_face(f.code, fx, y + 1.5f, fw, fh, a, right);
    std::string nm = f.name; for (char& ch : nm) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const float tx = right ? fx - 3.f - text_w(nm, true, 7.f) : fx + fw + 3.f;
    shadow_text(nm, true, 7.f, tx, y + 8.f, ko ? LayerColor{160, 160, 170, 1.f} : LayerColor{255, 255, 255, 1.f});
    // BRV under the name, HP number at the bar's end
    if (ko) { shadow_text("KO", true, 7.f, right ? fx - 3.f - text_w("KO", true, 7.f) : fx + fw + 3.f, y + 15.6f, LayerColor{255, 110, 100, 1.f}); }
    else if (f.brv >= 0) {
        const std::string b = std::to_string(f.brv);
        const float bw = g_layer.serif_caps(b, 7.6f, 1.f, 0.2f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        const float bx = right ? fx - 3.f - bw : fx + fw + 3.f;
        g_layer.serif_caps(b, 7.6f, 1.f, 0.2f, bx + 0.5f, y + 16.5f, LayerColor{10, 10, 30, 0.8f});
        g_layer.serif_caps(b, 7.6f, 1.f, 0.2f, bx, y + 16.f, f.brv == 0 ? LayerColor{255, 120, 110, 1.f} : LayerColor{255, 236, 190, 1.f});
    }
    // HP bar over the whole panel width, filling from the outer edge (as the retail slot panels). With the gauges the HP number sits on the row under it beside the
    // half-length assist chevrons (native layout: our side: assist at the left, HP number at the right of the box; the enemy's panel mirrored); without them it ends the bar row.
    const float bx0 = x + 2.f, bw0 = w - 4.f;
    hp_bar(bx0, y + 18.6f, bw0, ko ? 0.f : float(f.hp) / float(std::max(1, f.hp_max)), right, a);
    const std::string hp = ko ? "0" : std::to_string(std::max(0, f.hp));
    const float hw = text_w(hp, true, 6.2f);
    if (!gauges) shadow_text(hp, true, 6.2f, right ? bx0 : x + w - 2.f - hw, y + 23.f, LayerColor{230, 238, 255, a});
    else {
        const float aw = bw0 * 0.5f - 6.f;   // half the former chevron length (the old gauge was the panel's width less 26)
        const float ax = right ? x + w - 2.f - aw : bx0;
        if (f.assist >= 0.f) assist_chevrons(ax, y + 24.6f, aw, ko ? 0.f : f.assist, right, f.has_assist, a);
        shadow_text(hp, true, 6.2f, right ? bx0 : x + w - 2.f - hw, y + 29.3f, LayerColor{230, 238, 255, a});
    }
    if (!ko && (f.ex_mode || f.ex >= 0.999f)) {   // "EX" at the panel's far end of the BRV row: gold = the gauge is full (EX Mode ready), pulsing magenta = in EX Mode
        const float pulse = f.ex_mode ? 0.85f + 0.15f * static_cast<float>(std::sin(tnow * 9.0)) : 1.f;
        const std::string t = f.ex_mode ? "EX MODE" : "EX";
        const float em = f.ex_mode ? 5.0f : 6.0f, tw = text_w(t, true, em);
        shadow_text(t, true, em, right ? x + 3.f : x + w - 3.f - tw, y + 15.6f, f.ex_mode ? LayerColor{255, 150, 245, pulse} : LayerColor{255, 214, 110, 1.f});
    }
    if (!ko && f.break_flash >= 0.f && f.break_flash < 1.f) {   // this fighter's own break (never on the big panel of the player: user, 18 + 20 Nov): the panel flashes red and
        // shakes, "BREAK" pops over its BRV number, shards fly off it (the retail break animation, scaled down to the small panel)
        const float t = f.break_flash, al = t < 0.1f ? t / 0.1f : (t > 0.75f ? (1.f - t) / 0.25f : 1.f), em = 9.6f + 3.f * (1.f - std::min(1.f, t * 5.f));
        const float fl = std::max(0.f, 1.f - t * 3.5f);
        g_layer.band(x, y, x + w, y + H, [&](float, float) { return LayerColor{255, 70, 50, 0.55f * fl * a}; });
        const std::string bt = "BREAK";
        const float bwid = text_w(bt, true, em), shk = t < 0.4f ? 0.9f * std::sin(t * 140.f) * (1.f - t / 0.4f) : 0.f;
        const float bx = (right ? fx - 3.f - bwid : fx + fw + 3.f) + shk, by = y + 16.2f;
        g_layer.band(bx - 2.f, by - em * 0.78f, bx + bwid + 2.f, by + 2.f, [&](float, float) { return LayerColor{20, 8, 18, 0.7f * al}; });
        g_layer.text(bt, true, em, bx + 0.6f, by + 0.6f, LayerColor{10, 10, 30, 0.85f * al});
        g_layer.text(bt, true, em, bx, by, LayerColor{255, 96, 76, al});
        for (int i = 0; i < 7; i++) {   // shards
            const float ang = i * 0.9f + 0.4f, d = 3.f + t * 26.f * (0.6f + 0.1f * (i % 4)), sa = std::max(0.f, 1.f - t * 1.3f);
            const float cx = bx + bwid * 0.5f + std::cos(ang) * d * 1.6f, cy = by - 3.f + std::sin(ang) * d * 0.7f;
            g_layer.band(cx - 1.2f, cy - 0.7f, cx + 1.2f, cy + 0.7f, [&](float, float) { return LayerColor{255, 190.f - 12.f * i, 150.f, 0.9f * sa * a}; });
        }
    }
}

// a side's reserve: face strips of the members still to come (next one first), small, under the x1.0 counters
void reserve(const TagHudReserve& r) {
    const int n = static_cast<int>(std::min(r.names.size(), static_cast<size_t>(5)));
    if (n <= 0) return;
    const bool right = r.side != 0;
    const float fw = 40.f, fh = 10.f, gap = 2.f, y = 22.f;
    const float label_w = text_w("RESERVE", true, 5.6f);
    const float total = label_w + 3.f + n * (fw + gap);
    float x = right ? 480.f - 4.f - total : 4.f;
    plate(x - 2.f, y - 1.5f, x + total + 1.f, y + fh + 1.5f, 0.9f, right);
    shadow_text("RESERVE", true, 5.6f, x, y + 7.2f, LayerColor{200, 210, 240, 0.95f});
    x += label_w + 3.f;
    for (int i = 0; i < n; i++) {
        const std::string code = i < static_cast<int>(r.codes.size()) ? r.codes[i] : std::string();
        draw_face(code, x, y, fw, fh, i == 0 ? 1.f : 0.6f, right);
        if (i == 0) g_layer.band(x, y + fh, x + fw, y + fh + 0.8f, [&](float, float) { const LayerColor c = right ? kEdgeR : kEdgeL; return c; });
        x += fw + gap;
    }
    if (r.countdown >= 0.f) {   // the next member's call-in wait, under the strip
        char b[32]; std::snprintf(b, sizeof b, "NEXT IN %d", static_cast<int>(std::ceil(r.countdown)));
        const std::string t = b; const float tw = text_w(t, true, 6.4f), tx = right ? 480.f - 4.f - tw : 4.f + 2.f, ty = y + fh + 9.f;
        plate(right ? tx - 4.f : tx - 3.f, ty - 7.f, right ? tx + tw + 3.f : tx + tw + 5.f, ty + 2.2f, 0.9f, right);
        shadow_text(t, true, 6.4f, tx, ty, r.countdown < 5.f ? LayerColor{255, 214, 120, 1.f} : LayerColor{235, 240, 255, 1.f});
    }
}

void banner(const std::string& t) {
    const float em = 10.f, tw = text_w(t, true, em), cx = 240.f, y = 50.f;
    g_layer.band(cx - tw / 2.f - 40.f, y - 9.f, cx + tw / 2.f + 40.f, y + 5.f, [&](float x, float yy) {
        const float d = std::fabs(x - cx) / (tw / 2.f + 40.f), fade = 1.f - std::pow(d, 3.f);
        float r = 63, g = 53, b = 174; if (yy < y - 8.f) { r += 40; g += 45; b += 30; }
        return LayerColor{r, g, b, 0.85f * fade};
    });
    shadow_text(t, true, em, cx - tw / 2.f, y + 2.5f, LayerColor{255, 255, 255, 1.f});
}

// a notice. A fall ("NAME IS DOWN"): a dark plate with a red edge under the join banner. The job powers: the game's own result-screen bar ("Party
// received strong powers of the Warrior." across the top: a navy bar, white text, the |marked| words in cyan), fading in and out.
void notice(const Notice& n, float y, double t) {
    const float fin = static_cast<float>(std::clamp((t - n.start) / 0.25, 0.0, 1.0)), fout = static_cast<float>(std::clamp((n.until - t) / 0.5, 0.0, 1.0)), a = std::min(fin, fout);
    if (n.kind == 0) {
        const float em = 8.4f, tw = text_w(n.text, true, em), cx = 240.f, hw = tw / 2.f + 26.f;
        g_layer.band(cx - hw, y - 7.f, cx + hw, y + 4.2f, [&](float x, float) { const float d = std::fabs(x - cx) / hw; return LayerColor{14, 16, 40, 0.72f * a * (1.f - std::pow(d, 3.f))}; });
        g_layer.band(cx - hw, y - 7.f, cx + hw, y - 6.2f, [&](float x, float) { const float d = std::fabs(x - cx) / hw; return LayerColor{255, 110, 100, 0.9f * a * (1.f - std::pow(d, 2.f))}; });
        shadow_text(n.text, true, em, cx - tw / 2.f, y + 2.f, LayerColor{255, 255, 255, a});
        return;
    }
    const float em = 9.f, h = 19.f;
    std::vector<std::pair<std::string, bool>> seg; { bool hi = false; std::string cur; for (char c : n.text) { if (c == '|') { if (!cur.empty()) seg.push_back({cur, hi}); cur.clear(); hi = !hi; } else cur += c; } if (!cur.empty()) seg.push_back({cur, hi}); }
    float tw = 0.f; for (const auto& sg : seg) tw += text_w(sg.first, true, em);
    g_layer.band(0.f, y, 480.f, y + h, [&](float, float yy) { const float k = (yy - y) / h; return LayerColor{18.f + 8.f * (1.f - k), 28.f + 14.f * (1.f - k), 96.f - 30.f * k, 0.92f * a}; });
    g_layer.band(0.f, y + h - 1.f, 480.f, y + h, [&](float, float) { return LayerColor{110, 150, 255, 0.85f * a}; });
    g_layer.band(0.f, y, 480.f, y + 0.8f, [&](float, float) { return LayerColor{150, 180, 255, 0.55f * a}; });
    float x = 240.f - tw / 2.f;
    for (const auto& sg : seg) { shadow_text(sg.first, true, em, x, y + 13.f, sg.second ? LayerColor{90, 215, 255, a} : LayerColor{255, 255, 255, a}); x += text_w(sg.first, true, em); }
}

}  // namespace

void tagteam_hud_notice(const std::string& text, int kind, float secs) {
    const double t = now_s();
    for (const Notice& n : g_notices) if (n.text == text && n.until > t) return;   // the same message is not stacked twice
    g_notices.push_back({text, kind, t + secs, t});
    if (g_notices.size() > 6) g_notices.erase(g_notices.begin());
}
void tagteam_hud_clear() { g_notices.clear(); g_banner.clear(); }

void tagteam_hud_set_banner(const std::string& t) { g_banner = t; }

void tagteam_hud_draw(const std::vector<TagHudFighter>& fighters, const std::vector<TagHudReserve>& reserves) {
    g_fighters = fighters; g_reserves = reserves;
}
void tagteam_hud_hide() { g_fighters.clear(); g_reserves.clear(); g_banner.clear(); g_notices.clear(); }

bool tagteam_fx_draw(dissidia::MenuLayer& layer);   // tagteam_fx.cpp

// once per frame: the HUD and the effects on the game layer
void tagteam_layer_frame(bool in_battle) {
    if (!in_battle) { tagteam_hud_hide(); }
    const double tnow = now_s();
    g_notices.erase(std::remove_if(g_notices.begin(), g_notices.end(), [&](const Notice& n) { return n.until <= tnow; }), g_notices.end());
    const bool any = !g_fighters.empty() || !g_reserves.empty() || !g_banner.empty() || !g_notices.empty();
    g_layer.begin(std::max(2, psp_sddialog::output_scale()));
    if (any) {
        int row[2] = {0, 0};
        for (const TagHudFighter& f : g_fighters) {
            const int s = f.side != 0;
            const bool g = f.ex >= 0.f || f.assist >= 0.f;   // with the gauges a panel is 31 high (rows 34 apart) and its EX tube sits outside it
            panel(f, s == 0 ? (g ? 9.f : 4.f) : 480.f - (g ? 9.f : 4.f) - 112.f, 142.f - (g ? 34.f : 27.f) * row[s]++, tnow);
        }
        for (const TagHudReserve& r : g_reserves) reserve(r);
        if (!g_banner.empty()) banner(g_banner);
        { float yd = 70.f, yj = 0.f;   // falls under the join banner, job powers along the top edge
          for (const Notice& n : g_notices) { if (n.kind == 0) { notice(n, yd, tnow); yd += 13.f; } else { notice(n, yj, tnow); yj += 19.f; } } }
    }
    const bool fx = tagteam_fx_draw(g_layer);
    if (!any && !fx) { if (g_layer_shown) { g_layer.clear(); g_layer_shown = false; } return; }
    g_layer.publish();
    g_layer_shown = true;
}
