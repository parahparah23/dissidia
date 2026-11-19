// VCDIFF / xdelta3 decoder: see include/psp_vcdiff.h.
#include "psp_vcdiff.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include "../third_party/xz-embedded/xz.h"

namespace psp_vcdiff {

// ------------------------------------------------------------------------------------------------------------- ranges
void Ranges::add(uint64_t lo, uint64_t hi) {
    if (lo >= hi) return;
    if (r_.empty() || lo > r_.back().hi) { r_.push_back({lo, hi}); return; }     // the common case: in order
    if (lo >= r_.back().lo) { r_.back().hi = std::max(r_.back().hi, hi); return; }
    std::vector<Range> out;                                                        // out of order: insert and merge
    bool placed = false;
    for (const Range& x : r_) {
        if (x.hi < lo || x.lo > hi) {
            if (x.lo > hi && !placed) { out.push_back({lo, hi}); placed = true; }
            out.push_back(x);
        } else {
            lo = std::min(lo, x.lo);
            hi = std::max(hi, x.hi);
        }
    }
    if (!placed) out.push_back({lo, hi});
    r_.swap(out);
}

std::vector<Range> Ranges::overlaps(uint64_t lo, uint64_t hi) const {
    std::vector<Range> out;
    auto it = std::upper_bound(r_.begin(), r_.end(), lo, [](uint64_t v, const Range& x) { return v < x.hi; });
    for (; it != r_.end() && it->lo < hi; ++it) out.push_back({std::max(it->lo, lo), std::min(it->hi, hi)});
    return out;
}

uint64_t Ranges::total() const {
    uint64_t t = 0;
    for (const Range& x : r_) t += x.hi - x.lo;
    return t;
}

uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = std::min<size_t>(n, 5552);
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

namespace {

enum { NOOP = 0, RUN = 1, ADD = 2, COPY = 3 };
constexpr int NEAR_SIZE = 4, SAME_SIZE = 3;

struct Code { uint8_t t1, s1, m1, t2, s2, m2; };

// RFC 3284 section 5.6: the default code table
std::array<Code, 256> make_table() {
    std::array<Code, 256> t{};
    int i = 0;
    t[i++] = {RUN, 0, 0, NOOP, 0, 0};
    for (int s = 0; s < 18; s++) t[i++] = {ADD, uint8_t(s), 0, NOOP, 0, 0};
    for (int mode = 0; mode < 9; mode++) {
        t[i++] = {COPY, 0, uint8_t(mode), NOOP, 0, 0};
        for (int s = 4; s < 19; s++) t[i++] = {COPY, uint8_t(s), uint8_t(mode), NOOP, 0, 0};
    }
    for (int mode = 0; mode < 6; mode++)
        for (int a = 1; a < 5; a++)
            for (int c = 4; c < 7; c++) t[i++] = {ADD, uint8_t(a), 0, COPY, uint8_t(c), uint8_t(mode)};
    for (int mode = 6; mode < 9; mode++)
        for (int a = 1; a < 5; a++) t[i++] = {ADD, uint8_t(a), 0, COPY, 4, uint8_t(mode)};
    for (int mode = 0; mode < 9; mode++) t[i++] = {COPY, 4, uint8_t(mode), ADD, 1, 0};
    return t;
}
const std::array<Code, 256> kTable = make_table();

struct Reader {
    const uint8_t* p = nullptr;
    size_t n = 0, at = 0;
    bool bad = false;
    Reader() = default;
    Reader(const uint8_t* d, size_t sz) : p(d), n(sz) {}
    bool eof() const { return at >= n; }
    uint8_t byte() {
        if (at >= n) { bad = true; return 0; }
        return p[at++];
    }
    uint64_t varint() {
        uint64_t v = 0;
        for (int k = 0; k < 10; k++) {
            uint8_t c = byte();
            v = (v << 7) | (c & 0x7F);
            if (!(c & 0x80)) return v;
        }
        bad = true;
        return 0;
    }
    const uint8_t* take(size_t k) {
        if (k > n - std::min(at, n)) { bad = true; at = n; return nullptr; }
        const uint8_t* r = p + at;
        at += k;
        return r;
    }
};

struct XzDel { void operator()(xz_dec* d) const { if (d) xz_dec_end(d); } };

}  // namespace

bool read_header(const uint8_t* patch, size_t size, Header& h, std::string& err) {
    Reader r(patch, size);
    const uint8_t* m = r.take(3);
    if (!m || m[0] != 0xD6 || m[1] != 0xC3 || m[2] != 0xC4) { err = "not a VCDIFF / xdelta3 file"; return false; }
    r.byte();                                   // version
    const uint8_t hind = r.byte();
    h.secondary = (hind & 1) ? r.byte() : -1;
    if (hind & 2) { err = "custom code tables are not supported"; return false; }
    if (hind & 4) {
        const uint64_t n = r.varint();
        const uint8_t* a = r.take(size_t(n));
        if (a) h.app.assign(reinterpret_cast<const char*>(a), size_t(n));
    }
    if (r.bad) { err = "truncated patch header"; return false; }
    h.first_window = r.at;
    return true;
}

bool apply(const uint8_t* patch, size_t size, const ReadSource& read, const std::function<bool(const Window&)>& on_window,
           std::string& err) {
    Header h;
    if (!read_header(patch, size, h, err)) return false;
    if (h.secondary != -1 && h.secondary != 2) {
        err = "secondary compressor " + std::to_string(h.secondary) + " is not supported (only LZMA, id 2)";
        return false;
    }
    std::unique_ptr<xz_dec, XzDel> dec[3];
    if (h.secondary == 2) {
        xz_crc32_init();
#ifdef XZ_USE_CRC64
        xz_crc64_init();
#endif
        for (auto& d : dec) {
            d.reset(xz_dec_init(XZ_DYNALLOC, 1u << 27));
            if (!d) { err = "out of memory (LZMA decoder)"; return false; }
        }
    }
    Reader r(patch, size);
    r.at = h.first_window;
    uint64_t tpos = 0;
    Window w;
    std::vector<uint8_t> src, sect_buf[3];
    std::vector<Range> s_unk;
    for (uint32_t wi = 0; !r.eof(); wi++) {
        const uint8_t wind = r.byte();
        if (wind & 2) { err = "VCD_TARGET windows are not supported"; return false; }
        uint64_t src_len = 0, src_pos = 0;
        if (wind & 1) { src_len = r.varint(); src_pos = r.varint(); }
        const uint64_t body_len = r.varint();
        const uint8_t* bp = r.take(size_t(body_len));
        if (r.bad || !bp) { err = "truncated patch (window " + std::to_string(wi) + ")"; return false; }
        Reader body(bp, size_t(body_len));
        const uint64_t tlen = body.varint();
        const uint8_t dind = body.byte();
        uint64_t sizes[3];
        for (auto& s : sizes) s = body.varint();
        uint32_t cksum = 0;
        const bool has_ck = (wind & 4) != 0;
        if (has_ck) {
            const uint8_t* c = body.take(4);
            if (c) cksum = (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3];
        }
        if (tlen > (1ull << 31) || src_len > (1ull << 31)) { err = "window too large"; return false; }
        Reader sect[3];
        for (int k = 0; k < 3; k++) {
            const uint8_t* b = body.take(size_t(sizes[k]));
            if (body.bad) { err = "truncated window " + std::to_string(wi); return false; }
            if (dind & (1 << k)) {
                if (!dec[k]) { err = "compressed section without a secondary compressor"; return false; }
                Reader q(b, size_t(sizes[k]));
                const uint64_t n = q.varint();
                if (q.bad || n > (1ull << 31)) { err = "bad compressed section"; return false; }
                std::vector<uint8_t>& out = sect_buf[k];
                out.assign(size_t(n) + 1, 0);
                xz_buf xb{};
                xb.in = b + q.at;
                xb.in_pos = 0;
                xb.in_size = size_t(sizes[k]) - q.at;
                xb.out = out.data();
                xb.out_pos = 0;
                xb.out_size = out.size();
                for (;;) {
                    const size_t ip = xb.in_pos, op = xb.out_pos;
                    const xz_ret ret = xz_dec_run(dec[k].get(), &xb);
                    if (ret == XZ_STREAM_END) break;
                    if (ret != XZ_OK) {
                        err = "window " + std::to_string(wi) + ": LZMA section " + std::to_string(k) + " is damaged (" + std::to_string(int(ret)) + ")";
                        return false;
                    }
                    if (xb.in_pos == xb.in_size && xb.out_pos == op && xb.in_pos == ip) break;   // all input used, nothing more
                    if (xb.in_pos == ip && xb.out_pos == op) break;
                }
                if (xb.out_pos != n) {
                    err = "window " + std::to_string(wi) + ": LZMA section " + std::to_string(k) + " gave " + std::to_string(xb.out_pos) +
                          " bytes, header says " + std::to_string(n);
                    return false;
                }
                sect[k] = Reader(out.data(), size_t(n));
            } else {
                sect[k] = Reader(b, size_t(sizes[k]));
            }
        }
        Reader& add = sect[0];
        Reader& inst = sect[1];
        Reader& addr = sect[2];
        src.assign(size_t(src_len), 0);
        s_unk.clear();
        if (src_len) read(src_pos, src_len, src.data(), s_unk);
        Ranges su;
        for (const Range& x : s_unk) {
            const uint64_t lo = std::max(x.lo, src_pos), hi = std::min(x.hi, src_pos + src_len);
            if (lo < hi) su.add(lo - src_pos, hi - src_pos);
        }
        std::vector<uint8_t>& T = w.data;
        T.clear();
        T.reserve(size_t(tlen));
        Ranges tu;
        uint64_t near[NEAR_SIZE] = {}, same[SAME_SIZE * 256] = {};
        int slot = 0;
        while (!inst.eof()) {
            const Code& c = kTable[inst.byte()];
            for (int half = 0; half < 2; half++) {
                const uint8_t typ = half ? c.t2 : c.t1, mode = half ? c.m2 : c.m1;
                uint64_t sz = half ? c.s2 : c.s1;
                if (!typ) continue;
                if (sz == 0) sz = inst.varint();
                if (T.size() + sz > tlen) { err = "window " + std::to_string(wi) + " writes past its length"; return false; }
                if (typ == ADD) {
                    const uint8_t* d = add.take(size_t(sz));
                    if (!d) { err = "window " + std::to_string(wi) + ": ADD past the data section"; return false; }
                    T.insert(T.end(), d, d + sz);
                    continue;
                }
                if (typ == RUN) {
                    const uint8_t v = add.byte();
                    T.insert(T.end(), size_t(sz), v);
                    continue;
                }
                const uint64_t here = src_len + T.size();
                uint64_t a;
                if (mode == 0) a = addr.varint();
                else if (mode == 1) a = here - addr.varint();
                else if (mode < 2 + NEAR_SIZE) a = near[mode - 2] + addr.varint();
                else a = same[(mode - 2 - NEAR_SIZE) * 256 + addr.byte()];   // SAME: one byte, not a varint
                near[slot] = a;
                slot = (slot + 1) % NEAR_SIZE;
                same[a % (SAME_SIZE * 256)] = a;
                const uint64_t at = T.size();
                if (a < src_len) {
                    if (a + sz > src_len) { err = "COPY crosses the end of the source segment"; return false; }
                    T.insert(T.end(), src.begin() + std::ptrdiff_t(a), src.begin() + std::ptrdiff_t(a + sz));
                    for (const Range& x : su.overlaps(a, a + sz)) tu.add(at + x.lo - a, at + x.hi - a);
                } else {
                    a -= src_len;
                    if (a >= at) { err = "COPY from beyond the written window"; return false; }
                    const bool unk = !tu.empty() && !tu.overlaps(a, a + sz).empty();
                    for (uint64_t k = 0; k < sz; k++) T.push_back(T[size_t(a + k)]);   // overlapping copies repeat, byte by byte
                    if (unk) tu.add(at, at + sz);   // conservative for overlapping copies: the whole run
                }
            }
            if (inst.bad || add.bad || addr.bad) { err = "window " + std::to_string(wi) + " is truncated"; return false; }
        }
        if (T.size() != tlen) {
            err = "window " + std::to_string(wi) + " decoded to " + std::to_string(T.size()) + " bytes, header says " + std::to_string(tlen);
            return false;
        }
        w.index = wi;
        w.target_offset = tpos;
        w.unknown = tu.all();
        w.checksum = has_ck ? (adler32(T.data(), T.size()) == cksum ? 1 : 0) : -1;
        if (!on_window(w)) { err.clear(); return true; }
        tpos += tlen;
    }
    return true;
}

}  // namespace psp_vcdiff
