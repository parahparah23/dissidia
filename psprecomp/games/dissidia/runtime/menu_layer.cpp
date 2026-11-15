// games/dissidia/runtime/menu_layer.cpp — see menu_layer.h.
#include "menu_layer.h"
#include "psp_ge_draw.h"
#include "hle/psp_hle_font.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

extern const unsigned char psp_ui_font_bodyb[];   // runtime/assets/embed_fonts.cmake (Crimson Text SemiBold)
extern const unsigned long psp_ui_font_bodyb_len;
extern const unsigned char psp_sans_regular[], psp_sans_bold[];   // Noto Sans: the game text's face where the host has none
extern const unsigned long psp_sans_regular_len, psp_sans_bold_len;

namespace dissidia {

namespace {

struct Faces {
    FT_Library lib = nullptr;
    FT_Face reg = nullptr, bold = nullptr, serif = nullptr;
    std::vector<uint8_t> reg_bytes, bold_bytes;
};
bool read_file(const std::string& p, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? size_t(n) : 0);
    const bool ok = n > 0 && std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}
// The game's text is drawn by sceFont from these host files (psp_hle_font): the same faces make an added label match.
Faces& faces() {
    static Faces f;
    static std::once_flag once;
    std::call_once(once, [] {
        if (FT_Init_FreeType(&f.lib)) { f.lib = nullptr; return; }
        FT_New_Memory_Face(f.lib, psp_ui_font_bodyb, FT_Long(psp_ui_font_bodyb_len), 0, &f.serif);
        // The same faces sceFont uses (psp_hle_font.cpp ft_init): the embedded Noto Sans on Android / Windows and wherever the host
        // has no font, the host file elsewhere; PSPRECOMP_FONT (a file) wins, PSPRECOMP_FONT=embedded forces Noto.
        std::string reg, bold;
        const char* fenv = std::getenv("PSPRECOMP_FONT");
        bool embedded = fenv && !std::strcmp(fenv, "embedded");
#if defined(__ANDROID__) || defined(_WIN32)
        if (!(fenv && *fenv)) embedded = true;
#endif
        if (!embedded && !psp_font_host_files(&reg, &bold)) embedded = true;
        if (embedded) reg.clear(), bold.clear();
        if (!reg.empty() && read_file(reg, &f.reg_bytes)) FT_New_Memory_Face(f.lib, f.reg_bytes.data(), FT_Long(f.reg_bytes.size()), 0, &f.reg);
        if (!bold.empty() && read_file(bold, &f.bold_bytes)) FT_New_Memory_Face(f.lib, f.bold_bytes.data(), FT_Long(f.bold_bytes.size()), 0, &f.bold);
        if (!f.reg) FT_New_Memory_Face(f.lib, psp_sans_regular, FT_Long(psp_sans_regular_len), 0, &f.reg);
        if (!f.bold) FT_New_Memory_Face(f.lib, psp_sans_bold, FT_Long(psp_sans_bold_len), 0, &f.bold);
        if (!f.bold) f.bold = f.reg;
    });
    return f;
}

}  // namespace

void MenuLayer::begin(int scale) {
    s_ = std::max(1, std::min(4, scale));
    w_ = 480 * s_; h_ = 272 * s_;
    px_.assign(size_t(w_) * size_t(h_) * 4, 0.f);
    copies_.clear();
}

void MenuLayer::blend(int x, int y, float r, float g, float b, float a) {
    if (a <= 0.f || x < 0 || y < 0 || x >= w_ || y >= h_) return;
    if (a > 1.f) a = 1.f;
    float* p = &px_[(size_t(y) * size_t(w_) + size_t(x)) * 4];
    const float da = p[3] * (1.f - a), oa = a + da;
    if (oa <= 0.f) return;
    p[0] = (r * a + p[0] * da) / oa; p[1] = (g * a + p[1] * da) / oa; p[2] = (b * a + p[2] * da) / oa; p[3] = oa;
}

void MenuLayer::line(float x0, float x1, float y, float thick, LayerColor c) {
    band(x0, y - thick / 2, x1, y + thick / 2, [&](float, float) { return c; });
}

void MenuLayer::glow(float cx, float cy, float rad, LayerColor c) {
    const int x0 = std::max(0, int((cx - rad) * s_)), x1 = std::min(w_, int((cx + rad) * s_) + 1);
    const int y0 = std::max(0, int((cy - rad) * s_)), y1 = std::min(h_, int((cy + rad) * s_) + 1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float dx = (x + 0.5f) / s_ - cx, dy = (y + 0.5f) / s_ - cy, d = std::sqrt(dx * dx + dy * dy) / rad;
            if (d < 1.f) blend(x, y, c.r, c.g, c.b, c.a * (1.f - d) * (1.f - d));
        }
}

void MenuLayer::image(const uint32_t* px, int iw, int ih, int sx, int sy, int sw, int sh, float x, float y, float w, float h, float alpha) {
    if (!px || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    const int x0 = std::max(0, int(x * s_)), x1 = std::min(w_, int((x + w) * s_ + 0.999f));
    const int y0 = std::max(0, int(y * s_)), y1 = std::min(h_, int((y + h) * s_ + 0.999f));
    auto texel = [&](int tx, int ty, float* out) {
        tx = std::min(std::max(tx, sx), sx + sw - 1); ty = std::min(std::max(ty, sy), sy + sh - 1);
        tx = std::min(std::max(tx, 0), iw - 1); ty = std::min(std::max(ty, 0), ih - 1);
        const uint32_t p = px[size_t(ty) * size_t(iw) + size_t(tx)];
        const float a = float(p >> 24) / 255.f;
        out[0] = float(p & 255) * a; out[1] = float((p >> 8) & 255) * a; out[2] = float((p >> 16) & 255) * a; out[3] = a;   // premultiplied for filtering
    };
    for (int py = y0; py < y1; ++py)
        for (int pxx = x0; pxx < x1; ++pxx) {
            const float u = sx + ((pxx + 0.5f) / s_ - x) / w * sw - 0.5f, v = sy + ((py + 0.5f) / s_ - y) / h * sh - 0.5f;
            const int iu = int(std::floor(u)), iv = int(std::floor(v));
            const float fu = u - iu, fv = v - iv;
            float a[4], b[4], c[4], d[4], o[4];
            texel(iu, iv, a); texel(iu + 1, iv, b); texel(iu, iv + 1, c); texel(iu + 1, iv + 1, d);
            for (int k = 0; k < 4; k++) o[k] = (a[k] * (1 - fu) + b[k] * fu) * (1 - fv) + (c[k] * (1 - fu) + d[k] * fu) * fv;
            if (o[3] <= 0.001f) continue;
            blend(pxx, py, o[0] / o[3], o[1] / o[3], o[2] / o[3], o[3] * alpha);
        }
}

float MenuLayer::text(const std::string& str, bool bold, float em, float x, float baseline, LayerColor c, bool measure_only) {
    Faces& f = faces();
    FT_Face face = bold ? f.bold : f.reg;
    if (!face) return 0.f;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    FT_Set_Pixel_Sizes(face, 0, FT_UInt(std::lround(em * s_)));
    float pen = x * s_;
    const int by = int(std::lround(baseline * s_));
    FT_UInt prev = 0;
    for (unsigned char ch : str) {
        const FT_UInt gi = FT_Get_Char_Index(face, ch);
        if (prev && gi && FT_HAS_KERNING(face)) { FT_Vector k; FT_Get_Kerning(face, prev, gi, FT_KERNING_DEFAULT, &k); pen += k.x / 64.f; }
        if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
            const FT_GlyphSlot g = face->glyph;
            if (!measure_only) {
                const int gx = int(std::lround(pen)) + g->bitmap_left, gy = by - g->bitmap_top;
                for (unsigned r = 0; r < g->bitmap.rows; r++)
                    for (unsigned q = 0; q < g->bitmap.width; q++) {
                        const uint8_t cov = g->bitmap.buffer[r * g->bitmap.pitch + q];
                        if (cov) blend(gx + int(q), gy + int(r), c.r, c.g, c.b, c.a * cov / 255.f);
                    }
            }
            pen += g->advance.x / 64.f;
        }
        prev = gi;
    }
    return pen / s_ - x;
}

float MenuLayer::serif_caps(const std::string& str, float em, float small, float track, float x, float baseline, LayerColor c, bool measure_only) {
    FT_Face face = faces().serif;
    if (!face) return 0.f;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    float pen = x * s_;
    const int by = int(std::lround(baseline * s_));
    for (unsigned char ch : str) {
        const bool lower = ch >= 'a' && ch <= 'z';
        FT_Set_Pixel_Sizes(face, 0, FT_UInt(std::lround(em * (lower ? small : 1.f) * s_)));
        const FT_UInt gi = FT_Get_Char_Index(face, lower ? ch - 32 : ch);
        if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
            const FT_GlyphSlot g = face->glyph;
            if (!measure_only) {
                const int gx = int(std::lround(pen)) + g->bitmap_left, gy = by - g->bitmap_top;
                for (unsigned r = 0; r < g->bitmap.rows; r++)
                    for (unsigned q = 0; q < g->bitmap.width; q++) {
                        const uint8_t cov = g->bitmap.buffer[r * g->bitmap.pitch + q];
                        if (cov) blend(gx + int(q), gy + int(r), c.r, c.g, c.b, c.a * cov / 255.f);
                    }
            }
            pen += g->advance.x / 64.f + track * s_;
        }
    }
    return pen / s_ - x - track;
}

void MenuLayer::publish() {
    std::vector<uint32_t> out(size_t(w_) * size_t(h_));
    for (size_t i = 0; i < out.size(); i++) {
        const float* p = &px_[i * 4];
        auto ch = [](float v) { return uint32_t(std::min(255.f, std::max(0.f, v + 0.5f))); };
        out[i] = ch(p[0]) | ch(p[1]) << 8 | ch(p[2]) << 16 | uint32_t(std::min(255.f, p[3] * 255.f + 0.5f)) << 24;
    }
    ge_draw_set_game_layer_copies(copies_.data(), int(copies_.size()));
    ge_draw_set_game_layer(out.data(), w_, h_);
}

void MenuLayer::clear() { ge_draw_clear_game_layer(); }

}  // namespace dissidia
