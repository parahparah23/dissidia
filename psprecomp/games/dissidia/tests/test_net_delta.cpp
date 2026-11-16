// Host test of the online-pack delta decoder (runtime/net_delta.h): deltas from tools/netpack.py decode to the
// right bytes, and hostile input (truncated, corrupt, out-of-range copies, oversize) is refused without crashing.
// Build: tests/run_net_delta_test.sh
#include "../runtime/net_delta.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

static std::vector<uint8_t> rd(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::vector<uint8_t> pack(uint32_t tsize, uint32_t ssize, const std::vector<uint8_t>& ops) {
    std::vector<uint8_t> d{'N', 'P', 'D', '1'};
    for (uint32_t v : {tsize, ssize}) for (int i = 0; i < 4; i++) d.push_back((v >> (8 * i)) & 0xff);
    uLongf n = compressBound(ops.size());
    std::vector<uint8_t> z(n);
    compress(z.data(), &n, ops.data(), ops.size());
    d.insert(d.end(), z.begin(), z.begin() + n);
    return d;
}
int main(int argc, char** argv) {
    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; };
    std::vector<uint8_t> src(4096), out;
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 7);
    // a good one: copy 100 bytes from 10, add "hi"
    std::vector<uint8_t> ops{0, 10, 0, 0, 0, 100, 0, 0, 0, 1, 2, 0, 0, 0, 'h', 'i'};
    check(dissidia::apply_delta(pack(102, 4096, ops), src, 102, &out) && out.size() == 102 && out[0] == src[10] && out[101] == 'i', "copy + add");
    check(!dissidia::apply_delta(pack(102, 4096, ops), src, 101, &out), "target size other than the listed one");
    check(!dissidia::apply_delta(pack(102, 4095, ops), src, 102, &out), "source size mismatch");
    std::vector<uint8_t> far{0, 0xF0, 0x0F, 0, 0, 0x20, 0, 0, 0};
    check(!dissidia::apply_delta(pack(32, 4096, far), src, 32, &out), "copy past the source");
    std::vector<uint8_t> wrap{0, 0xFF, 0xFF, 0xFF, 0xFF, 2, 0, 0, 0};
    check(!dissidia::apply_delta(pack(2, 4096, wrap), src, 2, &out), "copy offset wrap-around");
    std::vector<uint8_t> big{1, 0xFF, 0xFF, 0xFF, 0x7F, 'x'};
    check(!dissidia::apply_delta(pack(1, 4096, big), src, 1, &out), "add longer than the stream");
    std::vector<uint8_t> grow{1, 3, 0, 0, 0, 'a', 'b', 'c'};
    check(!dissidia::apply_delta(pack(2, 4096, grow), src, 2, &out), "output past the listed size");
    check(!dissidia::apply_delta(pack(2, 4096, {7}), src, 2, &out), "unknown op");
    check(!dissidia::apply_delta(std::vector<uint8_t>{'N', 'P', 'D'}, src, 2, &out), "truncated header");
    // a zip bomb: tiny compressed, huge inflated
    std::vector<uint8_t> bomb(50u << 20, 1);
    check(!dissidia::apply_delta(pack(16, 4096, bomb), src, 16, &out), "inflates past the cap");
    // fuzz: random corruptions of the good delta never crash
    std::mt19937 rng(1);
    const std::vector<uint8_t> good = pack(102, 4096, ops);
    for (int k = 0; k < 20000; k++) {
        std::vector<uint8_t> d = good;
        for (int j = 0; j < 1 + (int)(rng() % 4); j++) d[rng() % d.size()] = (uint8_t)rng();
        if (rng() % 4 == 0) d.resize(rng() % d.size());
        dissidia::apply_delta(d, src, 102, &out);
    }
    check(true, "20000 corrupted deltas (no crash)");
    if (argc == 4) {   // a real delta: <delta> <retail file> <expected file>
        const auto d = rd(argv[1]), s = rd(argv[2]), e = rd(argv[3]);
        check(dissidia::apply_delta(d, s, (uint32_t)e.size(), &out) && out == e, "netpack.py delta of a real file");
    }
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
