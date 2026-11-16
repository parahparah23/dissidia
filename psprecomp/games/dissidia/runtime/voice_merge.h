// games/dissidia/runtime/voice_merge.h — Japanese voices with a mod that changes a battle voice bank.
// A mod's bank (e.g. Bartz's, with Dissidia 1 lines in slots 012 left empty) is built on the English bank; the Japanese
// pack (voice_jp/) has the same path, and the mod's file would win, turning every line of the character English. This
// takes the Japanese bank and adds only the clips of slots it leaves empty and the mod's bank fills (SSCF: 0x20-byte
// header, u32 total size at +8, u32 slot count at +12, 0x20-byte slots from +0x20: u32 offset at +4, u32 size at +8;
// a slot with size 0 and no link flag (+3 bit 0x10) is empty). Host test: tests/test_voice_merge.cpp.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace dissidia {

inline bool merge_voice_bank(const std::vector<uint8_t>& jp, const std::vector<uint8_t>& mod, std::vector<uint8_t>* out, int* added) {
    auto u32 = [](const std::vector<uint8_t>& b, size_t o) { uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; };
    *added = 0;
    if (jp.size() < 0x20 || mod.size() < 0x20 || std::memcmp(jp.data(), "SSCF", 4) || std::memcmp(mod.data(), "SSCF", 4)) return false;
    const uint32_t n = u32(jp, 12);
    if (n != u32(mod, 12) || n > 4096 || 0x20 + 0x20ull * n > jp.size() || 0x20 + 0x20ull * n > mod.size()) return false;
    *out = jp;
    for (uint32_t i = 0; i < n; i++) {
        const size_t e = 0x20 + 0x20 * i;
        const bool jp_empty = u32(jp, e + 8) == 0 && !(jp[e + 3] & 0x10);
        const uint32_t mo = u32(mod, e + 4), ms = u32(mod, e + 8);
        if (!jp_empty || ms == 0 || (mod[e + 3] & 0x10) || (uint64_t)mo + ms > mod.size()) continue;
        while (out->size() % 16) out->push_back(0);
        const uint32_t at = (uint32_t)out->size();
        out->insert(out->end(), mod.begin() + mo, mod.begin() + mo + ms);
        std::memcpy(out->data() + e, mod.data() + e, 0x20);
        std::memcpy(out->data() + e + 4, &at, 4);
        ++*added;
    }
    const uint32_t total = (uint32_t)out->size();
    std::memcpy(out->data() + 8, &total, 4);
    return true;
}

}  // namespace dissidia
