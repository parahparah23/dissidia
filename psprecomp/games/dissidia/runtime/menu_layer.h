// games/dissidia/runtime/menu_layer.h — a small straight-alpha painter for ge_draw_set_game_layer: elements the runtime adds to
// the game's own screens (menu_settings.cpp), drawn with the game's text font (the sceFont host file) and its menu textures.
#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dissidia {

struct LayerColor { float r, g, b, a; };   // 0..255 for r, g, b; 0..1 for a

class MenuLayer {
public:
    // A 480x272 design-unit layer at `scale` pixels per unit.
    void begin(int scale);
    int scale() const { return s_; }
    // Horizontal gradient band: colour(x) for x in design units, rows [y0, y1) with soft vertical edges.
    template <class F> void band(float x0, float y0, float x1, float y1, F color_at);
    void line(float x0, float x1, float y, float thick, LayerColor c);
    void glow(float cx, float cy, float rad, LayerColor c);   // soft round dot
    // RGBA8 image (straight alpha, w x h), source rect (sx, sy, sw, sh) drawn to (x, y, w, h) design units.
    void image(const uint32_t* px, int iw, int ih, int sx, int sy, int sw, int sh, float x, float y, float w, float h, float alpha);
    // Text in the game's font (bold or regular), em size in design units, left x and baseline y; returns the advance width.
    float text(const std::string& s, bool bold, float em, float x, float baseline, LayerColor c, bool measure_only = false);
    // Serif small capitals (Crimson Text SemiBold, shipped with the runtime): capitals at `em`, lower case as capitals at
    // `small` x em, `track` extra units between letters; the title screen's lettering.
    float serif_caps(const std::string& s, float em, float small, float track, float x, float baseline, LayerColor c, bool measure_only = false);
    void publish();   // hand the image to the runtime (ge_draw_set_game_layer)
    void clear();     // remove it

    void blend(int x, int y, float r, float g, float b, float a);   // pixel space, straight alpha "over"
private:
    int s_ = 2, w_ = 0, h_ = 0;
    std::vector<float> px_;   // r, g, b (0..255, not premultiplied), a (0..1)
};

template <class F> void MenuLayer::band(float x0, float y0, float x1, float y1, F color_at) {
    const int ix0 = std::max(0, int(x0 * s_)), ix1 = std::min(w_, int(x1 * s_ + 0.999f));
    const int iy0 = std::max(0, int(y0 * s_)), iy1 = std::min(h_, int(y1 * s_ + 0.999f));
    for (int y = iy0; y < iy1; ++y) {
        const float top = std::min(1.f, std::max(0.f, (y + 1) - y0 * s_)), bot = std::min(1.f, std::max(0.f, y1 * s_ - y));
        const float cov = std::min(top, bot);
        for (int x = ix0; x < ix1; ++x) {
            const LayerColor c = color_at((x + 0.5f) / float(s_), (y + 0.5f) / float(s_));
            blend(x, y, c.r, c.g, c.b, c.a * cov);
        }
    }
}

}  // namespace dissidia
