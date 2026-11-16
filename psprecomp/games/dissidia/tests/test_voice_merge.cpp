// Host test of voice_merge.h: the Japanese bank keeps its clips, gets the mod's clips only in its empty slots,
// and malformed banks are refused. Build: c++ -std=c++17 -fsanitize=address,undefined test_voice_merge.cpp
#include "../runtime/voice_merge.h"
#include <cstdio>
static std::vector<uint8_t> bank(const std::vector<std::vector<uint8_t>>& clips) {
    std::vector<uint8_t> b(0x20 + 0x20 * clips.size(), 0);
    std::memcpy(b.data(), "SSCF", 4);
    uint32_t n = (uint32_t)clips.size(); std::memcpy(b.data() + 12, &n, 4);
    for (size_t i = 0; i < clips.size(); i++) {
        uint32_t off = clips[i].empty() ? 0 : (uint32_t)b.size(), sz = (uint32_t)clips[i].size();
        if (sz) b.insert(b.end(), clips[i].begin(), clips[i].end());
        b[0x20 + 0x20 * i + 0x10] = (uint8_t)(0xA0 + i);   // a parameter byte, to see whose slot record is used
        std::memcpy(b.data() + 0x20 + 0x20 * i + 4, &off, 4); std::memcpy(b.data() + 0x20 + 0x20 * i + 8, &sz, 4);
    }
    uint32_t t = (uint32_t)b.size(); std::memcpy(b.data() + 8, &t, 4);
    return b;
}
int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* w) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", w); fails += !ok; };
    auto jp = bank({{'J', '0'}, {}, {'J', '2'}}), mod = bank({{'E', '0'}, {'D', '1', '!'}, {'E', '2'}});
    std::vector<uint8_t> out; int added = 0;
    check(dissidia::merge_voice_bank(jp, mod, &out, &added) && added == 1, "one empty slot filled");
    auto clip = [&](int i) { uint32_t o, s; std::memcpy(&o, out.data() + 0x20 + 0x20 * i + 4, 4); std::memcpy(&s, out.data() + 0x20 + 0x20 * i + 8, 4);
                             return std::vector<uint8_t>(out.begin() + o, out.begin() + o + s); };
    check(clip(0) == std::vector<uint8_t>{'J', '0'} && clip(2) == std::vector<uint8_t>{'J', '2'}, "Japanese clips kept");
    check(clip(1) == std::vector<uint8_t>{'D', '1', '!'}, "mod clip in the empty slot");
    uint32_t t; std::memcpy(&t, out.data() + 8, 4); check(t == out.size(), "total size updated");
    check(!dissidia::merge_voice_bank(jp, bank({{'E'}}), &out, &added), "slot count mismatch refused");
    auto bad = mod; uint32_t huge = 0x7FFFFFFF; std::memcpy(bad.data() + 0x20 + 0x20 + 8, &huge, 4);
    check(dissidia::merge_voice_bank(jp, bad, &out, &added) && added == 0, "out-of-range mod clip skipped");
    check(!dissidia::merge_voice_bank(std::vector<uint8_t>(8, 0), mod, &out, &added), "truncated bank refused");
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails;
}
