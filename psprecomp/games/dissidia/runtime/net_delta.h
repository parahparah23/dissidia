// games/dissidia/runtime/net_delta.h — the delta decoder of online packs (net_packs.cpp; written by tools/netpack.py).
// It reads bytes another player sent, before anything is verified: every bound is checked, the output never grows past
// `want` bytes and every copy stays inside `src`. Host test: tests/test_net_delta.cpp.
// Format: "NPD1", u32 target size, u32 source size, zlib stream of ops: 0x00 COPY {u32 src off, u32 len}, 0x01 ADD {u32 len, bytes}.
#pragma once
#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace dissidia {

inline bool apply_delta(const std::vector<uint8_t>& delta, const std::vector<uint8_t>& src, uint32_t want, std::vector<uint8_t>* out) {
    if (delta.size() < 12 || std::memcmp(delta.data(), "NPD1", 4) != 0) return false;
    uint32_t tsize, ssize;
    std::memcpy(&tsize, delta.data() + 4, 4);
    std::memcpy(&ssize, delta.data() + 8, 4);
    if (tsize != want || ssize != src.size()) return false;
    // inflate the op stream: at most twice the target plus slack (ADD bytes plus op headers); more = not ours
    const uLong cap = (uLong)want * 2 + 65536;
    std::vector<uint8_t> ops(cap);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(delta.data() + 12);
    z.avail_in = (uInt)(delta.size() - 12);
    z.next_out = ops.data();
    z.avail_out = (uInt)cap;
    const int r = inflate(&z, Z_FINISH);
    const size_t nops = cap - z.avail_out;
    inflateEnd(&z);
    if (r != Z_STREAM_END) return false;
    out->clear();
    out->reserve(want);
    size_t p = 0;
    while (p < nops) {
        const uint8_t op = ops[p];
        if (op == 0 && p + 9 <= nops) {
            uint32_t o, n;
            std::memcpy(&o, &ops[p + 1], 4);
            std::memcpy(&n, &ops[p + 5], 4);
            if ((uint64_t)o + n > src.size() || (uint64_t)out->size() + n > want) return false;
            out->insert(out->end(), src.begin() + o, src.begin() + o + n);
            p += 9;
        } else if (op == 1 && p + 5 <= nops) {
            uint32_t n;
            std::memcpy(&n, &ops[p + 1], 4);
            if ((uint64_t)p + 5 + n > nops || (uint64_t)out->size() + n > want) return false;
            out->insert(out->end(), ops.begin() + p + 5, ops.begin() + p + 5 + n);
            p += 5 + n;
        } else {
            return false;
        }
    }
    return out->size() == want;
}

}  // namespace dissidia
