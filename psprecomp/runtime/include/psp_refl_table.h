// psp_refl_table.h -- which surfaces reflect (issue #2): reflectivity per texture.
//
// The screen-space and ray-traced reflections used to treat every upward-facing pixel as a mirror (grass included) and
// nothing else (no wall ever reflected). The game's models carry no gloss / specular information, so the material is
// looked up by the texture it is drawn with: the texture-pack id (psp_tex_pack.h, 16 hex digits, the name of a
// replacement PNG and of texpack_extract.py's output) -> reflectivity 0..1. A texture that is not listed does not reflect.
//
// Two sources, the file winning over the built-in list:
//   * the game module's built-in list (psp_refl_set_builtin, e.g. games/dissidia/runtime/refl_materials.cpp);
//   * reflectivity.txt in the working directory (PSPRECOMP_REFLECTIVITY for another path), re-read while the game runs
//     when it changes. One entry per line: `<id> <reflectivity> [roughness] [metal] [water]`, `#` starts a comment.
//
// A material is reflectivity (strength, 0..1), roughness (0 = mirror, 1 = a broad, dim sheen: the reflection is blurred and its
// grazing-angle Fresnel boost is damped), metal (0/1: the reflection is tinted by the surface's own colour, as gold or copper) and
// water (0/1: the reflection is bent by the surface's own animated ripple pattern, so it wobbles and flows with the water instead of
// sitting on it as a flat mirror). Packed into one byte for the mask (psp_refl_encode): 3 bits reflectivity, 3 roughness, 1 metal,
// 1 water.
//
// When neither source has an entry the table is inactive and the reflections keep the old rule (upward surfaces).
// Pure C++ (host-tested: tests/test_refl_table.cpp); the GL side is in psp_ge_draw.cpp (mask layer) and psp_screenfx.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

struct PspReflMaterial {
    float refl = 0.f;    // 0..1
    float rough = 0.f;   // 0..1
    bool metal = false;
    bool water = false;
};
struct PspReflEntry {
    uint64_t id;
    float refl;          // 0..1
    float rough;         // 0..1
    bool metal;
    bool water;
};

/// Mask byte of a material: reflectivity in the top 3 bits (0 = does not reflect), roughness in the next 3, then metal, then water.
uint8_t psp_refl_encode(const PspReflMaterial& m);
PspReflMaterial psp_refl_decode(uint8_t b);

/// Parse reflectivity.txt text into `out` (added / replaced). Returns the number of entries read; bad lines are skipped
/// (counted in *bad when given).
int psp_refl_parse(const std::string& text, std::unordered_map<uint64_t, PspReflMaterial>& out, int* bad = nullptr);

/// The game module's list (static storage; may be called before or after the first lookup).
void psp_refl_set_builtin(const PspReflEntry* entries, size_t n);
/// True when any entry exists (built-in or file): the reflections then follow the table.
bool psp_refl_table_active();
/// Material of a texture (reflectivity 0 when not listed). Checks the file for changes at most every ~2 s.
PspReflMaterial psp_refl_lookup(uint64_t id);
/// Bumped whenever the table changes (file re-read, built-in list set): cached lookups must be redone.
uint32_t psp_refl_generation();
/// Re-check the file now (normally done lazily by psp_refl_lookup); called once per frame by the reflection pass.
void psp_refl_poll();
