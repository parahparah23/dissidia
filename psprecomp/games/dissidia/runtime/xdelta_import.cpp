// games/dissidia/runtime/xdelta_import.cpp — installs an xdelta / VCDIFF ISO patch as a mod folder from inside the game
// (Settings > Mods > Install ISO patch), on every platform: the C++ twin of tools/xdelta_mod.py `import`
// (docs/xdelta-mods.md). The mod folder it writes is the same, so the build tools (build-code) and the runtime treat both
// alike; the mod's code runs in the interpreter until the game is rebuilt (xdelta_mods.cpp).
//
// The patch's base is rebuilt from the install folder (disc0/, EBOOT.enc.bin when there is one) and the table of the two
// retail discs' layouts (disc_layouts.inc, generated from tools/disc_layouts.json). Unlike the tool it keeps no copy of a
// big patched file: only the 64 KB pieces that differ from your own file are stored while the patch runs (a phone has no
// room for a second PACKAGE.BIN), and the retail EBOOT it compares with is the one compiled into the game.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include "psp_settings.h"
#include "psp_vcdiff.h"
#include "hle/psp_hle_io.h"

void psp_init_data_sections(uint8_t* rdram);   // generated (output/data_sections.cpp): the retail EBOOT image

namespace fs = std::filesystem;
using psp_vcdiff::Range;
using psp_vcdiff::Ranges;

namespace {

// ------------------------------------------------------------------------------------------------------- disc layouts
struct DiscVariant { const char* md5; const uint32_t* differs; int n; };   // n [lo, hi) pairs, file-relative
struct DiscFile { const char* path; uint64_t offset, size; const char* md5; bool zero; const DiscVariant* variants; int nvariants; };
struct DiscBlob { uint64_t offset; uint32_t size; const char* data; };
struct DiscLayout { const char* id; uint64_t size; const DiscFile* files; int nfiles; const DiscBlob* blobs; int nblobs; };
#include "disc_layouts.inc"
constexpr int kLayouts = int(sizeof(kDiscLayouts) / sizeof(kDiscLayouts[0]));

constexpr uint64_t SECTOR = 2048;
constexpr uint64_t kMemSink = 64ull << 20;     // patched files up to this size are kept in memory
constexpr uint64_t kChunk = 64ull << 10;       // bigger ones: the 64 KB pieces that differ from your own file

// ------------------------------------------------------------------------------------------------------------- MD5
struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0;
    uint8_t buf[64];
    size_t nbuf = 0;
    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
            0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
            0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
            0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
            0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
            0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t M[16];
        for (int i = 0; i < 16; i++) M[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 | uint32_t(p[i * 4 + 3]) << 24;
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; i++) {
            uint32_t F;
            int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + rol(F, S[i]);
        }
        a += A; b += B; c += C; d += D;
    }
    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            if (nbuf == 0 && n >= 64) { block(p); p += 64; n -= 64; continue; }
            const size_t k = std::min(n, 64 - nbuf);
            std::memcpy(buf + nbuf, p, k);
            nbuf += k; p += k; n -= k;
            if (nbuf == 64) { block(buf); nbuf = 0; }
        }
    }
    std::string hex() {
        const uint64_t bits = len * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t z = 0;
        while (nbuf != 56) update(&z, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; i++) lb[i] = uint8_t(bits >> (8 * i));
        update(lb, 8);
        char out[33];
        const uint32_t v[4] = {a, b, c, d};
        for (int i = 0; i < 16; i++) std::snprintf(out + 2 * i, 3, "%02x", (v[i / 4] >> (8 * (i % 4))) & 0xff);
        return out;
    }
};

// ---------------------------------------------------------------------------------------------------------- SHA-256
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t nbuf = 0;
    uint64_t len = 0;
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            if (nbuf == 0 && n >= 64) { block(p); p += 64; n -= 64; continue; }
            const size_t k = std::min(n, 64 - nbuf);
            std::memcpy(buf + nbuf, p, k);
            nbuf += k; p += k; n -= k;
            if (nbuf == 64) { block(buf); nbuf = 0; }
        }
    }
    std::string digest() {   // raw 32 bytes
        const uint64_t bits = len * 8;
        const uint8_t pad = 0x80, z = 0;
        update(&pad, 1);
        while (nbuf != 56) update(&z, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; i++) lb[i] = uint8_t(bits >> (56 - 8 * i));
        update(lb, 8);
        std::string out(32, '\0');
        for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) out[size_t(4 * i + k)] = char(h[i] >> (24 - 8 * k));
        return out;
    }
};
std::string to_hex(const std::string& raw) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : raw) { o += d[c >> 4]; o += d[c & 15]; }
    return o;
}
std::string sha256_hex(const uint8_t* p, size_t n) { Sha256 s; s.update(p, n); return to_hex(s.digest()); }

// ------------------------------------------------------------------------------------------------------------ helpers
std::string upper(std::string s) { for (char& c : s) c = char(std::toupper((unsigned char)c)); return s; }
std::string lower(std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; }
bool starts_with(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
uint64_t file_size(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0; }
bool is_file(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }


std::vector<uint8_t> read_file(const std::string& p) {
    std::vector<uint8_t> out;
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(size_t(n));
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return out;
}

bool write_file(const std::string& p, const uint8_t* d, size_t n) {
    std::error_code ec;
    fs::create_directories(fs::u8path(p).parent_path(), ec);
    std::FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return false;
    const bool ok = (n == 0 || std::fwrite(d, 1, n, f) == n);
    return std::fclose(f) == 0 && ok;
}

std::string md5_file(const std::string& p) {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return "";
    Md5 m;
    std::vector<uint8_t> buf(4 << 20);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) m.update(buf.data(), n);
    std::fclose(f);
    return m.hex();
}

// "Emendatus11.18.26" -> ("Emendatus", "11.18.26") (tools/xdelta_mod.py split_version)
std::pair<std::string, std::string> split_version(const std::string& name) {
    static const std::regex re(R"([\s_.-]*(?:v|ver\.?|version)?[\s_.-]*\d+(?:[._-]\d+)*[a-z]?$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(name, m, re) || m.position(0) == 0) return {name, ""};
    std::string base = name.substr(0, size_t(m.position(0))), ver = name.substr(size_t(m.position(0)));
    while (!base.empty() && std::strchr(" _.-", base.back())) base.pop_back();
    size_t i = 0;
    while (i < ver.size() && std::strchr(" _.-vV", ver[i])) i++;
    ver = ver.substr(i);
    while (!ver.empty() && std::strchr(" _.-vV", ver.back())) ver.pop_back();
    return {base, ver};
}

std::string slug(const std::string& name) {
    std::string s;
    bool dash = false;
    for (char c : name) {
        if (std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') { s += c; dash = false; }
        else if (!dash) { s += '-'; dash = true; }
    }
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == '-' || s[a] == '.')) a++;
    while (b > a && (s[b - 1] == '-' || s[b - 1] == '.')) b--;
    s = lower(s.substr(a, b - a));
    return s.empty() ? "xdelta-mod" : s;
}

// a top-level "key": "value" / true / false of a small JSON object (mod.json)
std::string json_get(const std::string& js, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = js.find(k);
    if (p == std::string::npos) return "";
    p = js.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p++;
    while (p < js.size() && std::isspace((unsigned char)js[p])) p++;
    if (p < js.size() && js[p] == '"') {
        std::string out;
        for (size_t i = p + 1; i < js.size() && js[i] != '"'; i++) {
            if (js[i] == '\\' && i + 1 < js.size()) i++;
            out += js[i];
        }
        return out;
    }
    size_t e = p;
    while (e < js.size() && (std::isalnum((unsigned char)js[e]) || js[e] == '-' || js[e] == '.')) e++;
    return js.substr(p, e - p);
}

std::string json_esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}


// ---------------------------------------------------------------------------------------------------- the patch's base
// MD5s of the install folder's files, cached by path, size and time (<mods>/.xdelta_md5cache)
struct Md5Cache {
    std::string path;
    std::map<std::string, std::string> m;   // "path|size|mtime" -> md5
    bool changed = false;
    void load(const std::string& p) {
        path = p;
        std::ifstream f(p);
        std::string md5, key;
        while (f >> md5 && std::getline(f >> std::ws, key)) m[key] = md5;
    }
    std::string get(const std::string& file) {
        struct stat st;
        if (::stat(file.c_str(), &st) != 0) return "";
        const std::string key = file + "|" + std::to_string(st.st_size) + "|" + std::to_string(long(st.st_mtime));
        auto it = m.find(key);
        if (it != m.end()) return it->second;
        const std::string h = md5_file(file);
        m[key] = h;
        changed = true;
        return h;
    }
    void save() {
        if (!changed || path.empty()) return;
        std::ofstream f(path, std::ios::trunc);
        for (const auto& kv : m) f << kv.second << " " << kv.first << "\n";
    }
};

// One retail disc rebuilt from the install folder (tools/xdelta_mod.py WorkSource).
class Base {
public:
    const DiscLayout* layout = nullptr;
    std::vector<std::string> missing;

    Base(const DiscLayout& l, const std::string& disc0, const std::string& eboot_enc, Md5Cache& md5) : layout(&l) {
        for (int i = 0; i < l.nfiles; i++) {
            const DiscFile& f = l.files[i];
            files_.push_back({f.offset, f.offset + f.size});
            if (f.zero) { spans_.push_back({f.offset, f.size, ""}); continue; }
            const std::string up = upper(f.path);
            std::string cand = up == "PSP_GAME/SYSDIR/EBOOT.BIN" ? eboot_enc : disc0 + "/" + f.path;
            std::string host;
            if (!cand.empty() && is_file(cand) && file_size(cand) == f.size) {
                const std::string m = md5.get(cand);
                if (m == f.md5) host = cand;
                for (int v = 0; v < f.nvariants; v++)
                    if (m == f.variants[v].md5) {
                        host = cand;
                        for (int k = 0; k < f.variants[v].n; k++)
                            holes_.add(f.offset + f.variants[v].differs[2 * k], f.offset + f.variants[v].differs[2 * k + 1]);
                    }
            }
            if (!host.empty()) spans_.push_back({f.offset, f.size, host});
            else { missing.push_back(f.path); missing_.add(f.offset, f.offset + f.size); }
        }
        std::sort(spans_.begin(), spans_.end(), [](const Span& a, const Span& b) { return a.off < b.off; });
        for (int i = 0; i < l.nblobs; i++) blobs_.add(l.blobs[i].offset, l.blobs[i].offset + l.blobs[i].size);
    }
    ~Base() { for (auto& kv : fh_) std::fclose(kv.second); }

    void read(uint64_t pos, uint64_t n, uint8_t* out, std::vector<Range>& unknown) {
        std::memset(out, 0, size_t(n));
        const uint64_t end = pos + n;
        for (int i = 0; i < layout->nblobs; i++) {
            const DiscBlob& b = layout->blobs[i];
            const uint64_t lo = std::max(pos, b.offset), hi = std::min(end, b.offset + b.size);
            if (lo < hi) std::memcpy(out + (lo - pos), b.data + (lo - b.offset), size_t(hi - lo));
        }
        auto it = std::upper_bound(spans_.begin(), spans_.end(), pos, [](uint64_t v, const Span& s) { return v < s.off; });
        if (it != spans_.begin()) --it;
        for (; it != spans_.end() && it->off < end; ++it) {
            const uint64_t lo = std::max(pos, it->off), hi = std::min(end, it->off + it->size);
            if (lo >= hi || it->host.empty()) continue;
            std::FILE* f = open(it->host);
            if (!f) continue;
            fseeko(f, off_t(lo - it->off), SEEK_SET);
            const size_t got = std::fread(out + (lo - pos), 1, size_t(hi - lo), f);
            if (got < hi - lo) unknown.push_back({lo + got, hi});
        }
        // the bytes of files the install folder lacks are unknown (except what the blobs hold: the EBOOT's ~PSP header)
        for (const Range& m : missing_.overlaps(pos, end)) {
            uint64_t cur = m.lo;
            for (const Range& b : blobs_.overlaps(m.lo, m.hi)) {
                if (b.lo > cur) unknown.push_back({cur, b.lo});
                cur = std::max(cur, b.hi);
            }
            if (cur < m.hi) unknown.push_back({cur, m.hi});
        }
        for (const Range& h : holes_.overlaps(pos, end)) unknown.push_back(h);
    }

private:
    struct Span { uint64_t off, size; std::string host; };
    std::vector<Span> spans_;
    std::vector<Range> files_;
    Ranges missing_, holes_, blobs_;
    std::map<std::string, std::FILE*> fh_;
    std::FILE* open(const std::string& p) {
        auto it = fh_.find(p);
        if (it != fh_.end()) return it->second;
        std::FILE* f = std::fopen(p.c_str(), "rb");
        fh_[p] = f;
        return f;
    }
};

// Where two retail discs differ (the end of the last such byte): a patch's fit can be judged before it.
uint64_t layouts_differ_end() {
    uint64_t end = 0;
    for (int a = 0; a < kLayouts; a++)
        for (int b = 0; b < kLayouts; b++) {
            if (a == b) continue;
            const DiscLayout& x = kDiscLayouts[a];
            const DiscLayout& y = kDiscLayouts[b];
            for (int i = 0; i < x.nblobs; i++) end = std::max(end, x.blobs[i].offset + x.blobs[i].size);
            for (int i = 0; i < x.nfiles; i++) {
                const DiscFile& f = x.files[i];
                const DiscFile* g = nullptr;
                for (int j = 0; j < y.nfiles; j++) if (std::strcmp(y.files[j].path, f.path) == 0) { g = &y.files[j]; break; }
                if (g && g->offset == f.offset && std::strcmp(g->md5, f.md5) == 0) continue;
                bool narrowed = false;
                if (g && g->offset == f.offset)
                    for (int v = 0; v < f.nvariants; v++)
                        if (std::strcmp(f.variants[v].md5, g->md5) == 0 && f.variants[v].n) {
                            end = std::max(end, f.offset + f.variants[v].differs[2 * f.variants[v].n - 1]);
                            narrowed = true;
                        }
                if (!narrowed) end = std::max(end, f.offset + f.size);
            }
        }
    return end;
}

// ---------------------------------------------------------------------------------------------------- ISO 9660 parse
using ReadFn = std::function<bool(uint64_t off, uint64_t n, std::vector<uint8_t>& out)>;   // false: not there (yet)

// {path: (offset, size)}; returns 0 = ok, 1 = needs more data, 2 = not an ISO
int iso_files(const ReadFn& read, std::map<std::string, std::pair<uint64_t, uint64_t>>& out) {
    std::vector<uint8_t> pvd;
    uint64_t sec = 16;
    for (;;) {
        if (!read(sec * SECTOR, SECTOR, pvd)) return 1;
        if (std::memcmp(pvd.data() + 1, "CD001", 5) != 0 || pvd[0] == 255) return 2;
        if (pvd[0] == 1) break;
        if (++sec > 32) return 2;
    }
    struct Dir { uint64_t ext, size; std::string prefix; int depth; };
    auto u32 = [](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
    std::vector<Dir> todo{{u32(pvd.data() + 156 + 2), u32(pvd.data() + 156 + 10), "", 0}};
    std::vector<uint8_t> data;
    while (!todo.empty()) {
        const Dir d = todo.back();
        todo.pop_back();
        if (d.depth > 16 || d.size > (16u << 20)) continue;
        if (!read(d.ext * SECTOR, d.size, data)) return 1;
        size_t off = 0;
        while (off < data.size()) {
            const uint8_t ln = data[off];
            if (!ln) { off = (off / SECTOR + 1) * SECTOR; continue; }
            if (off + 33 > data.size()) break;
            const uint8_t* r = data.data() + off;
            const uint8_t nl = r[32];
            std::string name(reinterpret_cast<const char*>(r + 33), std::min<size_t>(nl, data.size() - off - 33));
            const uint32_t ext = u32(r + 2), size = u32(r + 10);
            const bool is_dir = (r[25] & 2) != 0;
            off += ln;
            if (nl == 1 && (name[0] == 0 || name[0] == 1)) continue;
            const size_t semi = name.find(';');
            if (semi != std::string::npos) name = name.substr(0, semi);
            if (name.size() > 1 && name.back() == '.') name.pop_back();
            if (is_dir) todo.push_back({ext, size, d.prefix + name + "/", d.depth + 1});
            else out[d.prefix + name] = {uint64_t(ext) * SECTOR, size};
        }
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------------------ sinks
// The patched bytes of one file: in memory (small files), or the 64 KB pieces that differ from your own copy of it
// (big ones) with everything else read from your copy.
class Sink {
public:
    uint64_t size = 0;
    Ranges unknown;
    Sink(uint64_t sz, const std::string& mine, const std::string& tmp) : size(sz), mine_(mine), tmp_(tmp) {
        if (sz <= kMemSink) mem_.assign(size_t(sz), 0);
    }
    ~Sink() {
        if (tf_) std::fclose(tf_);
        if (mf_) std::fclose(mf_);
        if (!tmp_.empty()) std::remove(tmp_.c_str());
    }
    bool in_memory() const { return size <= kMemSink; }
    void put(uint64_t at, const uint8_t* d, uint64_t n) {
        if (in_memory()) { std::memcpy(mem_.data() + at, d, size_t(n)); return; }
        while (n) {   // piece by piece; a window covers part of a piece only at its ends
            const uint64_t ci = at / kChunk, co = at % kChunk, k = std::min(n, kChunk - co);
            std::vector<uint8_t>& pend = partial_[ci];
            if (pend.empty()) {   // the piece starts as your own bytes; it is stored only if the patch changes it
                pend.assign(size_t(chunk_len(ci)), 0);
                if (mine_read(ci * kChunk, pend.data(), pend.size()) != pend.size()) dirty_.insert(ci);
            }
            if (!dirty_.count(ci) && std::memcmp(pend.data() + co, d, size_t(k)) != 0) dirty_.insert(ci);
            std::memcpy(pend.data() + co, d, size_t(k));
            filled_[ci] += k;
            if (filled_[ci] >= chunk_len(ci)) flush(ci);
            at += k; d += k; n -= k;
        }
    }
    void finish() { while (!partial_.empty()) flush(partial_.begin()->first); }
    bool read(uint64_t at, uint8_t* out, uint64_t n) {
        if (at + n > size) return false;
        if (in_memory()) { std::memcpy(out, mem_.data() + at, size_t(n)); return true; }
        while (n) {
            const uint64_t ci = at / kChunk, co = at % kChunk, k = std::min(n, kChunk - co);
            auto it = stored_.find(ci);
            if (it != stored_.end()) { fseeko(tf_, off_t(it->second + co), SEEK_SET); if (std::fread(out, 1, size_t(k), tf_) != k) return false; }
            else if (mine_read(at, out, k) != k) return false;
            at += k; out += k; n -= k;
        }
        return true;
    }
    std::vector<uint8_t> read_all() { std::vector<uint8_t> v(static_cast<size_t>(size)); if (!read(0, v.data(), size)) v.clear(); return v; }
    uint64_t stored_bytes() const { return uint64_t(stored_.size()) * kChunk; }

private:
    std::vector<uint8_t> mem_;
    std::string mine_, tmp_;
    std::FILE* tf_ = nullptr;
    std::FILE* mf_ = nullptr;
    uint64_t tf_end_ = 0;
    std::map<uint64_t, uint64_t> stored_;              // piece -> offset in the temp file
    std::map<uint64_t, std::vector<uint8_t>> partial_;
    std::map<uint64_t, uint64_t> filled_;
    std::set<uint64_t> dirty_;
    uint64_t chunk_len(uint64_t ci) const { return std::min(kChunk, size - ci * kChunk); }
    uint64_t mine_read(uint64_t at, uint8_t* out, uint64_t n) {
        if (!mf_ && !mine_.empty()) mf_ = std::fopen(mine_.c_str(), "rb");
        if (!mf_) return 0;
        fseeko(mf_, off_t(at), SEEK_SET);
        return std::fread(out, 1, size_t(n), mf_);
    }
    void flush(uint64_t ci) {
        std::vector<uint8_t>& p = partial_[ci];
        const bool same = !dirty_.count(ci) && unknown.overlaps(ci * kChunk, ci * kChunk + p.size()).empty();
        if (!same) {
            if (!tf_) tf_ = std::fopen(tmp_.c_str(), "w+b");
            if (tf_) {
                fseeko(tf_, off_t(tf_end_), SEEK_SET);
                std::fwrite(p.data(), 1, p.size(), tf_);
                stored_[ci] = tf_end_;
                tf_end_ += p.size();
            }
        }
        partial_.erase(ci);
        filled_.erase(ci);
        dirty_.erase(ci);
    }
};

// ------------------------------------------------------------------------------------------------------- the import
struct Progress {
    std::mutex mu;
    std::string text;
    unsigned revision = 0;
    std::atomic<bool> busy{false};
    void set(const std::string& t) {
        std::lock_guard<std::mutex> lk(mu);
        if (t != text) { text = t; revision++; }
        std::fprintf(stderr, "[XMOD_IMPORT] %s\n", t.c_str());
    }
};
Progress g_prog;

const char* const kCodeKeys[] = {"EBOOT.BIN", "OVL_BATTLE_APP.ELF", "OVL_MENU_APP.ELF", "OVL_EXTRA_APP.ELF"};
bool is_code_key(const std::string& n) { for (const char* k : kCodeKeys) if (n == k) return true; return false; }

std::string code_stamp(const std::string& folder) {   // tools/xdelta_mod.py code_stamp
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(folder + "/code"), ec)) {
        const std::string n = e.path().filename().u8string();
        if (is_code_key(n)) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    Sha256 h;
    for (const std::string& n : names) {
        h.update(reinterpret_cast<const uint8_t*>(n.c_str()), n.size() + 1);   // the name and its NUL
        const std::vector<uint8_t> d = read_file(folder + "/code/" + n);
        Sha256 f;
        f.update(d.data(), d.size());
        const std::string dig = f.digest();
        h.update(reinterpret_cast<const uint8_t*>(dig.data()), dig.size());
    }
    return to_hex(h.digest()).substr(0, 16);
}

struct Installed { std::string name, path, json; };
std::vector<Installed> xdelta_folders(const std::string& mods) {
    std::vector<Installed> out;
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(fs::u8path(mods), ec)) names.push_back(e.path().filename().u8string());
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        if (n.empty() || n[0] == '.' || n[0] == '_') continue;
        const std::string p = mods + "/" + n;
        const std::vector<uint8_t> js = read_file(p + "/mod.json");
        const std::string s(js.begin(), js.end());
        if (json_get(s, "kind") == "xdelta") out.push_back({n, p, s});
    }
    return out;
}
std::string family_of(const Installed& m) {
    const std::string f = json_get(m.json, "family");
    if (!f.empty()) return f;
    const std::string nm = json_get(m.json, "name");
    return slug(split_version(nm.empty() ? m.name : nm).first);
}

// run-time code recorded for an earlier version, still valid where the new version's appended data is the same
int carry_dynamic(const std::vector<Installed>& previous, const std::string& staging) {
    const std::string new_tail = staging + "/package_tail.bin";
    if (!is_file(new_tail)) return 0;
    std::set<std::string> keep;
    int kept = 0;
    for (const Installed& prev : previous) {
        std::ifstream dyn(prev.path + "/code/dynamic.txt");
        const std::string old_tail = prev.path + "/package_tail.bin";
        if (!dyn || !is_file(old_tail)) continue;
        std::vector<std::vector<std::string>> lines;
        std::string line;
        while (std::getline(dyn, line)) {
            std::istringstream ss(line);
            std::vector<std::string> f;
            std::string w;
            while (ss >> w) f.push_back(w);
            if (!f.empty()) lines.push_back(f);
        }
        std::set<std::pair<uint64_t, uint64_t>> same;
        for (const auto& f : lines) {
            if (f[0] != "blob" || f.size() != 5) continue;
            const uint64_t off = std::stoull(f[1]), size = std::stoull(f[2]);
            std::FILE* a = std::fopen(old_tail.c_str(), "rb");
            std::FILE* b = std::fopen(new_tail.c_str(), "rb");
            bool eq = false;
            if (a && b) {
                std::vector<uint8_t> x(static_cast<size_t>(size)), y(static_cast<size_t>(size));
                fseeko(a, off_t(off), SEEK_SET);
                fseeko(b, off_t(off), SEEK_SET);
                eq = std::fread(x.data(), 1, x.size(), a) == x.size() && std::fread(y.data(), 1, y.size(), b) == y.size() && x == y;
            }
            if (a) std::fclose(a);
            if (b) std::fclose(b);
            const std::string snap = prev.path + "/code/" + f[4];
            if (eq && is_file(snap)) {
                same.insert({off, size});
                const std::vector<uint8_t> d = read_file(snap);
                write_file(staging + "/code/" + f[4], d.data(), d.size());
                keep.insert(f[0] + " " + f[1] + " " + f[2] + " " + f[3] + " " + f[4]);
                kept++;
            }
        }
        for (const auto& f : lines)
            if (f[0] == "write" && f.size() == 6 && same.count({std::stoull(f[4]), std::stoull(f[5])}))
                keep.insert(f[0] + " " + f[1] + " " + f[2] + " " + f[3] + " " + f[4] + " " + f[5]);
    }
    if (!keep.empty()) {
        std::string s;
        for (const std::string& l : keep) s += l + "\n";
        write_file(staging + "/code/dynamic.txt", reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    return kept;
}

struct Elf { std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> loads; };   // vaddr -> (file offset, filesz)
bool elf_loads(const std::vector<uint8_t>& e, Elf& out) {
    if (e.size() < 52 || std::memcmp(e.data(), "\x7f" "ELF", 4) != 0) return false;
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, e.data() + o, 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, e.data() + o, 2); return v; };
    const uint32_t phoff = u32(28);
    const uint16_t phentsize = u16(42), phnum = u16(44);
    for (int i = 0; i < phnum; i++) {
        const size_t o = phoff + size_t(i) * phentsize;
        if (o + 32 > e.size()) return false;
        if (u32(o) == 1) out.loads.push_back({u32(o + 8), {u32(o + 4), u32(o + 16)}});
    }
    return !out.loads.empty();
}

// The EBOOT's changed bytes (code/eboot.patch: {vaddr, length, bytes}...) against the retail image compiled into the game.
bool eboot_patch(const std::vector<uint8_t>& patched, std::string& out, int& runs, std::string& err) {
    Elf elf;
    if (!elf_loads(patched, elf)) { err = "the patched EBOOT.BIN is not a plain ELF"; return false; }
    constexpr uint32_t kLo = 0x08804000u, kSpan = 0x01400000u;   // the EBOOT's part of memory (well past its .bss)
    std::vector<uint8_t> img(kSpan, 0);
    std::vector<uint8_t> scratch;   // psp_init_data_sections writes at (vaddr & 0x07FFFFFF): give it a buffer that reaches
    scratch.assign((kLo & 0x07FFFFFFu) + kSpan, 0);
    psp_init_data_sections(scratch.data());
    std::memcpy(img.data(), scratch.data() + (kLo & 0x07FFFFFFu), kSpan);
    scratch.clear();
    scratch.shrink_to_fit();
    runs = 0;
    for (const auto& ld : elf.loads) {
        const uint32_t va = ld.first, off = ld.second.first, n = ld.second.second;
        if (va < kLo || va + n > kLo + kSpan || size_t(off) + n > patched.size()) { err = "the patched EBOOT has a different memory layout"; return false; }
        const uint8_t* a = img.data() + (va - kLo);
        const uint8_t* b = patched.data() + off;
        uint32_t i = 0;
        while (i < n) {
            if (a[i] == b[i]) { i++; continue; }
            uint32_t j = i;
            while (j < n && (a[j] != b[j] || (j + 8 < n && std::memcmp(a + j, b + j, 8) != 0))) j++;
            const uint32_t v = va + i, len = j - i;
            out.append(reinterpret_cast<const char*>(&v), 4);
            out.append(reinterpret_cast<const char*>(&len), 4);
            out.append(reinterpret_cast<const char*>(b + i), len);
            runs++;
            i = j;
        }
    }
    return true;
}

struct Request { std::string patch_path, shown_name, mods, disc0, eboot_enc; };

// The import itself. Returns the text shown in the menu (first line: the outcome).
std::string run_import(const Request& rq) {
    const std::vector<uint8_t> patch = read_file(rq.patch_path);
    if (patch.empty()) return "Not installed: the patch could not be read.";
    psp_vcdiff::Header hd;
    std::string err;
    if (!psp_vcdiff::read_header(patch.data(), patch.size(), hd, err)) return "Not installed: " + err + ".";
    // the name: the file's, else (Android hands over a copy) the patched image's name from the patch header
    std::string full = rq.shown_name;
    {
        const size_t dot = full.find_last_of('.');
        if (dot != std::string::npos && dot > 0) full = full.substr(0, dot);
    }
    if (full.empty() || full == "picked") {
        std::string target = hd.app.substr(0, hd.app.find("//"));
        const size_t sl = target.find_last_of("/\\");
        if (sl != std::string::npos) target = target.substr(sl + 1);
        const size_t dot = target.find_last_of('.');
        if (dot != std::string::npos && dot > 0) target = target.substr(0, dot);
        full = target.empty() ? "xdelta-patch" : target;
    }
    const auto nv = split_version(full);
    const std::string name = nv.first, version = nv.second, family = slug(name);
    const std::string folder = rq.mods + "/" + family;
    std::vector<Installed> previous;
    for (const Installed& m : xdelta_folders(rq.mods)) if (family_of(m) == family) previous.push_back(m);
    const std::string patch_sha = sha256_hex(patch.data(), patch.size());
    for (const Installed& m : previous)
        if (json_get(m.json, "patch_sha256") == patch_sha) return name + " is already installed (" + m.name + ").";

    // ---- the base disc
    g_prog.set("Checking your game files ...");
    std::error_code ec;
    fs::create_directories(fs::u8path(rq.mods), ec);
    Md5Cache md5;
    md5.load(rq.mods + "/.xdelta_md5cache");
    std::vector<std::unique_ptr<Base>> cands;
    for (int i = 0; i < kLayouts; i++) cands.push_back(std::make_unique<Base>(kDiscLayouts[i], rq.disc0, rq.eboot_enc, md5));
    md5.save();
    Base* src = cands[0].get();
    if (cands.size() > 1) {   // the disc the patch was made for: the one its first windows (where the discs differ) fit
        g_prog.set("Finding the disc the patch was made for ...");
        const uint64_t limit = layouts_differ_end();
        long best_fails = -1;
        uint64_t best_unk = 0;
        for (auto& c : cands) {
            long fails = 0;
            uint64_t unk = 0;
            std::string e2;
            psp_vcdiff::apply(patch.data(), patch.size(),
                [&](uint64_t pos, uint64_t n, uint8_t* out, std::vector<Range>& u) { c->read(pos, n, out, u); },
                [&](const psp_vcdiff::Window& w) {
                    if (w.checksum == 0) fails++;
                    for (const Range& x : w.unknown) unk += x.hi - x.lo;
                    return w.target_offset + w.data.size() < limit;
                }, e2);
            std::fprintf(stderr, "[XMOD_IMPORT] %s: %ld window(s) fail, %llu byte(s) unknown\n", c->layout->id, fails, (unsigned long long)unk);
            if (best_fails < 0 || fails < best_fails || (fails == best_fails && unk < best_unk)) { best_fails = fails; best_unk = unk; src = c.get(); }
        }
    }
    std::fprintf(stderr, "[XMOD_IMPORT] base: %s rebuilt from %s\n", src->layout->id, rq.disc0.c_str());

    // ---- apply
    const std::string tmpdir = rq.mods + "/.xdelta_tmp";
    fs::remove_all(fs::u8path(tmpdir), ec);
    fs::create_directories(fs::u8path(tmpdir), ec);
    const std::string usr = rq.disc0 + "/PSP_GAME/USRDIR";
    std::map<std::string, std::pair<uint64_t, uint64_t>> files;
    std::map<std::string, std::unique_ptr<Sink>> sinks;
    struct SpanP { uint64_t off, size; std::string path; };
    std::vector<SpanP> spans;
    struct Bad { uint32_t index; bool unknown; };
    std::vector<Bad> bad;
    std::vector<psp_vcdiff::Window> pending;
    std::vector<uint8_t> head;
    bool have_dir = false, not_iso = false;
    const uint64_t total = src->layout->size;
    auto route = [&](const psp_vcdiff::Window& w) {
        const uint64_t lo = w.target_offset, hi = lo + w.data.size();
        // every file, in order (a patched disc can put files inside another's range: a grown PACKAGE.BIN over moved movies)
        for (auto it = spans.begin(); it != spans.end() && it->off < hi; ++it) {
            const uint64_t a = std::max(lo, it->off), b = std::min(hi, it->off + it->size);
            if (a >= b) continue;
            auto s = sinks.find(it->path);
            if (s == sinks.end()) continue;
            for (const Range& u : w.unknown) {
                const uint64_t ua = std::max(u.lo + lo, a), ub = std::min(u.hi + lo, b);
                if (ua < ub) s->second->unknown.add(ua - it->off, ub - it->off);
            }
            s->second->put(a - it->off, w.data.data() + (a - lo), b - a);
        }
        if (w.checksum == 0) bad.push_back({w.index, !w.unknown.empty()});
    };
    const bool applied = psp_vcdiff::apply(patch.data(), patch.size(),
        [&](uint64_t pos, uint64_t n, uint8_t* out, std::vector<Range>& u) { src->read(pos, n, out, u); },
        [&](const psp_vcdiff::Window& w) {
            if (!have_dir) {
                pending.push_back(w);
                head.insert(head.end(), w.data.begin(), w.data.end());
                const int r = iso_files([&](uint64_t o, uint64_t n, std::vector<uint8_t>& out) {
                    if (o + n > head.size()) return false;
                    out.assign(head.begin() + std::ptrdiff_t(o), head.begin() + std::ptrdiff_t(o + n));
                    return true;
                }, files);
                if (r == 2) { not_iso = true; return false; }
                if (r == 1) { files.clear(); return head.size() <= (256u << 20); }
                have_dir = true;
                for (const auto& kv : files) {
                    spans.push_back({kv.second.first, kv.second.second, kv.first});
                    const std::string u = upper(kv.first);
                    if (!starts_with(u, "PSP_GAME/USRDIR/") && u != "PSP_GAME/SYSDIR/EBOOT.BIN") continue;
                    const std::string mine = u == "PSP_GAME/SYSDIR/EBOOT.BIN" ? rq.eboot_enc : rq.disc0 + "/" + kv.first;
                    sinks[kv.first] = std::make_unique<Sink>(kv.second.second, mine, tmpdir + "/t" + std::to_string(sinks.size()) + ".bin");
                }
                std::sort(spans.begin(), spans.end(), [](const SpanP& a, const SpanP& b) { return a.off < b.off; });
                for (const auto& pw : pending) route(pw);
                pending.clear();
                head.clear();
                head.shrink_to_fit();
                return true;
            }
            route(w);
            if (total) g_prog.set("Applying the patch ... " + std::to_string(int((w.target_offset + w.data.size()) * 100 / total)) + "%");
            return true;
        }, err);
    for (auto& kv : sinks) kv.second->finish();
    auto cleanup = [&]() { sinks.clear(); fs::remove_all(fs::u8path(tmpdir), ec); };
    if (!applied) { cleanup(); return "Not installed: " + err + "."; }
    if (not_iso || !have_dir) { cleanup(); return "Not installed: the patched disc has no readable directory. Is this a Dissidia 012 ISO patch?"; }
    std::vector<uint32_t> unexplained;
    for (const Bad& b : bad) if (b.index != 0 && !b.unknown) unexplained.push_back(b.index);
    if (!unexplained.empty()) {
        cleanup();
        return "Not installed: " + std::to_string(unexplained.size()) + " block(s) of the patch do not match your disc. This patch is not for a "
               "retail Dissidia 012 disc (an update made for an earlier version of the mod? Those need the full patch from the mod's author).";
    }

    // ---- the mod folder
    g_prog.set("Writing the mod ...");
    const std::string staging = folder + ".new";
    fs::remove_all(fs::u8path(staging), ec);
    fs::create_directories(fs::u8path(staging + "/install"), ec);
    std::vector<std::string> problems, summary;
    bool refuse = false;
    std::map<std::string, std::string> up;
    for (const auto& kv : files) up[upper(kv.first)] = kv.first;
    const std::string pinfo = up["PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN"], pbin = up["PSP_GAME/USRDIR/DATA/PACKAGE.BIN"];
    if (!pinfo.empty() && !pbin.empty() && sinks.count(pinfo) && sinks.count(pbin)) {
        Sink& si = *sinks[pinfo];
        Sink& tp = *sinks[pbin];
        auto parse_index = [](const std::vector<uint8_t>& b, std::map<uint32_t, std::array<uint64_t, 3>>& out) {
            if (b.size() < 16 || std::memcmp(b.data() + 4, "pack", 4) != 0) return false;
            uint32_t count;
            std::memcpy(&count, b.data() + 8, 4);
            for (uint32_t i = 0; i < count && 16 + 12 * size_t(i) + 12 <= b.size(); i++) {
                uint32_t crc, sector, sf;
                std::memcpy(&crc, b.data() + 16 + 12 * i, 4);
                std::memcpy(&sector, b.data() + 20 + 12 * i, 4);
                std::memcpy(&sf, b.data() + 24 + 12 * i, 4);
                out[crc] = {uint64_t(sector) * SECTOR, sf & 0x0FFFFFFFu, sf >> 28};
            }
            return true;
        };
        std::map<uint32_t, std::array<uint64_t, 3>> tidx, bidx;
        if (!si.unknown.empty()) { problems.push_back("PACKAGE_INFO.BIN could not be rebuilt"); refuse = true; }
        else if (!parse_index(si.read_all(), tidx) || !parse_index(read_file(usr + "/DATA/PACKAGE_INFO.BIN"), bidx)) {
            problems.push_back("PACKAGE_INFO.BIN could not be read"); refuse = true;
        } else {
            std::FILE* bpkg = std::fopen((usr + "/DATA/PACKAGE.BIN").c_str(), "rb");
            int changed = 0, added = 0;
            uint64_t nbytes = 0;
            std::vector<std::pair<uint64_t, uint32_t>> order;
            for (const auto& kv : tidx) order.push_back({kv.second[0], kv.first});
            std::sort(order.begin(), order.end());
            std::vector<uint8_t> data, mine;
            for (const auto& o : order) {
                const uint32_t crc = o.second;
                const uint64_t off = tidx[crc][0], size = tidx[crc][1];
                data.resize(size_t(size));
                char nm[32];
                std::snprintf(nm, sizeof nm, "%08x", crc);
                if (!tp.read(off, data.data(), size)) { problems.push_back(std::string("PACKAGE.BIN entry ") + nm + " lies past the end of the patched file"); continue; }
                auto old = bidx.find(crc);
                bool have_mine = false;
                if (old != bidx.end() && bpkg) {
                    mine.resize(size_t(old->second[1]));
                    fseeko(bpkg, off_t(old->second[0]), SEEK_SET);
                    have_mine = std::fread(mine.data(), 1, mine.size(), bpkg) == mine.size();
                }
                std::vector<Range> holes = tp.unknown.overlaps(off, off + size);
                if (!holes.empty() && have_mine && mine.size() == size) {   // bytes the base lacked: your own entry stands in
                    for (const Range& h : holes) std::memcpy(data.data() + (h.lo - off), mine.data() + (h.lo - off), size_t(h.hi - h.lo));
                    holes.clear();
                }
                if (have_mine && mine == data) continue;
                if (!holes.empty()) { problems.push_back(std::string("PACKAGE.BIN entry ") + nm + " could not be rebuilt"); refuse = true; continue; }
                write_file(staging + "/install/by_crc/" + nm + ".bin", data.data(), data.size());
                nbytes += size;
                if (old != bidx.end()) changed++; else added++;
            }
            if (bpkg) std::fclose(bpkg);
            const uint64_t base_size = file_size(usr + "/DATA/PACKAGE.BIN"), tsize = tp.size;
            if (tsize > base_size) {
                if (base_size % SECTOR) problems.push_back("PACKAGE.BIN is not a whole number of sectors; the appended data cannot be served");
                else if (!tp.unknown.overlaps(base_size, tsize).empty()) { problems.push_back("PACKAGE.BIN: the data appended by the patch could not be rebuilt"); refuse = true; }
                else {
                    std::FILE* f = std::fopen((staging + "/package_tail.bin").c_str(), "wb");
                    std::vector<uint8_t> buf(8 << 20);
                    for (uint64_t at = base_size; f && at < tsize;) {
                        const uint64_t n = std::min<uint64_t>(buf.size(), tsize - at);
                        if (!tp.read(at, buf.data(), n) || std::fwrite(buf.data(), 1, size_t(n), f) != n) {
                            std::fprintf(stderr, "[XMOD_IMPORT] tail write failed at %llu of %llu (stored %llu B)\n", (unsigned long long)at,
                                         (unsigned long long)tsize, (unsigned long long)tp.stored_bytes());
                            problems.push_back("package_tail.bin could not be written");
                            refuse = true;
                            break;
                        }
                        at += n;
                    }
                    if (f) std::fclose(f);
                    char b[128];
                    std::snprintf(b, sizeof b, "PACKAGE.BIN: %.1f MB appended after the last entry", double(tsize - base_size) / 1048576.0);
                    summary.push_back(b);
                }
            }
            char b[128];
            std::snprintf(b, sizeof b, "PACKAGE.BIN: %d entr%s changed, %d added (%.1f MB)", changed, changed == 1 ? "y" : "ies", added, double(nbytes) / 1048576.0);
            summary.push_back(b);
        }
    }
    // ---- other USRDIR files and the code
    std::map<std::string, std::vector<uint8_t>> code;
    for (const auto& kv : files) {
        const std::string& p = kv.first;
        if (!sinks.count(p) || p == pinfo || p == pbin) continue;
        Sink& s = *sinks[p];
        const std::string u = upper(p);
        const uint64_t size = kv.second.second;
        std::string local, key, rel;
        if (u == "PSP_GAME/SYSDIR/EBOOT.BIN") {
            std::vector<uint8_t> hd4(4);
            s.read(0, hd4.data(), 4);
            if (std::memcmp(hd4.data(), "~PSP", 4) == 0 && (rq.eboot_enc.empty() || !is_file(rq.eboot_enc))) continue;   // still the disc's (encrypted) EBOOT
            if (!rq.eboot_enc.empty() && s.unknown.empty() && file_size(rq.eboot_enc) == size && s.read_all() == read_file(rq.eboot_enc)) continue;
            key = "EBOOT.BIN";
        } else {
            rel = p.substr(std::strlen("PSP_GAME/USRDIR/"));
            local = usr + "/" + rel;
            const std::string bn = upper(fs::u8path(p).filename().u8string());
            if (bn != "EBOOT.BIN" && is_code_key(bn)) key = bn;
        }
        // unchanged?
        if (!local.empty() && file_size(local) == size && s.unknown.empty()) {
            bool same = true;
            std::FILE* f = std::fopen(local.c_str(), "rb");
            std::vector<uint8_t> a(4 << 20), b(4 << 20);
            for (uint64_t at = 0; f && same && at < size;) {
                const uint64_t n = std::min<uint64_t>(a.size(), size - at);
                same = std::fread(a.data(), 1, size_t(n), f) == n && s.read(at, b.data(), n) && std::memcmp(a.data(), b.data(), size_t(n)) == 0;
                at += n;
            }
            if (f) std::fclose(f);
            if (f && same) continue;
        }
        if (!s.unknown.empty()) {
            problems.push_back(p + ": " + std::to_string(s.unknown.total()) + " byte(s) could not be rebuilt (the install folder lacks part of the base disc)");
            refuse = true;
            continue;
        }
        if (!key.empty()) {
            std::vector<uint8_t> d = s.read_all();
            if (key == "EBOOT.BIN" && (d.size() < 4 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0)) {
                problems.push_back("the patched EBOOT.BIN is encrypted; only patches that put a plain (decrypted) EBOOT on the disc are supported");
                refuse = true;
                continue;
            }
            code[key] = std::move(d);
            continue;
        }
        if (u.size() > 4 && u.compare(u.size() - 4, 4, ".PRX") == 0) { problems.push_back(p + ": patched PRX modules are not supported (left as it is)"); continue; }
        if (u.size() > 4 && u.compare(u.size() - 4, 4, ".PMF") == 0 && is_file(local) && size < file_size(local)) {
            summary.push_back(rel + ": made smaller by the patch to make room on the disc; the original is kept");
            continue;
        }
        const std::string out = staging + "/usrdir/" + upper(rel);
        fs::create_directories(fs::u8path(out).parent_path(), ec);
        std::FILE* f = std::fopen(out.c_str(), "wb");
        std::vector<uint8_t> buf(8 << 20);
        for (uint64_t at = 0; f && at < size;) {
            const uint64_t n = std::min<uint64_t>(buf.size(), size - at);
            if (!s.read(at, buf.data(), n)) break;
            std::fwrite(buf.data(), 1, size_t(n), f);
            at += n;
        }
        if (f) std::fclose(f);
        summary.push_back(rel + ": replaced");
    }
    sinks.clear();
    fs::remove_all(fs::u8path(tmpdir), ec);

    std::vector<std::string> code_keys;
    for (auto& kv : code) {
        code_keys.push_back(kv.first);
        write_file(staging + "/code/" + kv.first, kv.second.data(), kv.second.size());
        if (kv.first == "EBOOT.BIN") {
            std::string recs, e2;
            int runs = 0;
            if (!eboot_patch(kv.second, recs, runs, e2)) { problems.push_back(e2); refuse = true; }
            else {
                write_file(staging + "/code/eboot.patch", reinterpret_cast<const uint8_t*>(recs.data()), recs.size());
                summary.push_back("EBOOT: " + std::to_string(runs) + " changed byte run(s)");
            }
        } else {
            summary.push_back(kv.first + ": patched (code)");
        }
    }
    if (refuse) {
        fs::remove_all(fs::u8path(staging), ec);
        std::string t = "Not installed: the patch could not be applied completely.";
        for (const std::string& p : problems) t += "\n" + p;
        return t;
    }
    bool enabled = true;
    if (!previous.empty()) {   // keep your on/off choice across an update
        enabled = false;
        for (const Installed& m : previous) if (json_get(m.json, "enabled") != "false") enabled = true;
        const int carried = carry_dynamic(previous, staging);
        if (carried) summary.push_back(std::to_string(carried) + " block(s) of run-time code unchanged: their record is kept");
    }
    // another mod that changes the game's code: only one runs at a time
    std::vector<std::string> others;
    for (const Installed& m : xdelta_folders(rq.mods))
        if (family_of(m) != family && json_get(m.json, "enabled") != "false" &&
            (m.json.find("\"code\": [\"") != std::string::npos || is_file(m.path + "/package_tail.bin")))
            others.push_back(json_get(m.json, "name").empty() ? m.name : json_get(m.json, "name"));
    const std::string shown = rq.shown_name.empty() || rq.shown_name == "picked.patch" ? full : rq.shown_name;
    std::string js = "{\n";
    js += "  \"name\": \"" + json_esc(name + (version.empty() ? "" : " " + version)) + "\",\n";
    js += "  \"family\": \"" + json_esc(family) + "\",\n";
    js += "  \"version\": \"" + json_esc(version) + "\",\n";
    js += "  \"kind\": \"xdelta\",\n";
    js += std::string("  \"enabled\": ") + (enabled ? "true" : "false") + ",\n";
    js += "  \"patch\": \"" + json_esc(shown) + "\",\n";
    js += "  \"patch_sha256\": \"" + patch_sha + "\",\n";
    js += "  \"base\": \"" + json_esc(src->layout->id) + "\",\n";
    js += "  \"code\": [";
    for (size_t i = 0; i < code_keys.size(); i++) js += (i ? ", \"" : "\"") + code_keys[i] + "\"";
    js += "],\n";
    js += "  \"code_stamp\": \"" + code_stamp(staging) + "\",\n";
    js += "  \"installed_by\": \"game\",\n";
    js += "  \"description\": \"" + json_esc("Installed from " + shown + ". Settings > Mods turns it on and off (next start).") + "\"\n}\n";
    write_file(staging + "/mod.json", reinterpret_cast<const uint8_t*>(js.data()), js.size());
    for (const Installed& m : previous) fs::remove_all(fs::u8path(m.path), ec);
    fs::remove_all(fs::u8path(folder), ec);
    fs::rename(fs::u8path(staging), fs::u8path(folder), ec);
    if (ec) return "Not installed: the mod folder could not be moved into place (" + ec.message() + ").";

    std::string t = "Installed " + name + (version.empty() ? "" : " " + version) + (previous.empty() ? "" : " (update)") + "\n" +
                    (previous.empty() ? "" : "It replaces the earlier version. ") + "It is " + (enabled ? "on" : "off (as the earlier version was)") +
                    " from the next start" + (code.empty() ? "." : "; the code it changes runs in an interpreter until the game is rebuilt.");
    if (!others.empty()) {
        t += "\nNote: ";
        for (size_t i = 0; i < others.size(); i++) t += (i ? ", " : "") + others[i];
        t += " also changes the game's code; only one such mod runs at a time. Turn the other off.";
    }
    for (const std::string& s : summary) t += "\n" + s;
    for (const std::string& p : problems) t += "\nNote: " + p;
    return t;
}

// ----------------------------------------------------------------------------------------------- Settings > Mods hook
std::string mods_dir() {
    if (const char* m = std::getenv("PSPRECOMP_MODS")) if (m[0]) return m;
    const std::string ms0 = psp_io_ms0_host_path();
    const size_t sl = ms0.find_last_of('/');
    return (sl == std::string::npos ? std::string(".") : ms0.substr(0, sl)) + "/mods";
}

bool start_import(const std::string& path) {
    bool expected = false;
    if (!g_prog.busy.compare_exchange_strong(expected, true)) return false;
    Request rq;
    rq.patch_path = path;
    rq.shown_name = fs::u8path(path).filename().u8string();
    rq.mods = mods_dir();
    const char* d0 = std::getenv("PSPRECOMP_DISC0");
    std::error_code ec;
    rq.disc0 = fs::absolute(fs::u8path(d0 && d0[0] ? d0 : "./disc0"), ec).lexically_normal().u8string();
    while (rq.disc0.size() > 1 && rq.disc0.back() == '/') rq.disc0.pop_back();
    const std::string enc = fs::u8path(rq.disc0).parent_path().u8string() + "/EBOOT.enc.bin";
    rq.eboot_enc = is_file(enc) ? enc : "";
    g_prog.set("Reading the patch ...");
    std::thread([rq]() {
        std::string r;
        try { r = run_import(rq); }
        catch (const std::exception& e) { r = std::string("Not installed: ") + e.what(); }
        g_prog.set(r);
        g_prog.busy.store(false);
    }).detach();
    return true;
}

void import_status(unsigned* revision, bool* busy, std::string* text) {
    std::lock_guard<std::mutex> lk(g_prog.mu);
    if (revision) *revision = g_prog.revision;
    if (busy) *busy = g_prog.busy.load();
    if (text) *text = g_prog.text;
}

}  // namespace

// hooks_main.cpp: offer Settings > Mods > Install ISO patch
void dissidia_xdelta_register_installer() {
    static const PspModInstaller inst = {
        "Install ISO Patch (.xdelta)",
        "Install ISO patch\nChoose an .xdelta / VCDIFF patch made for the US or EU disc (a community mod such as Emendatus). The game "
        "applies it to your own game files and installs what it changes as a mod below, where it can be turned off and on. An update of "
        "an installed mod replaces it. Takes effect the next time the game starts; code the patch changes runs in an interpreter until "
        "the game is rebuilt (Rebuild in the setup).",
        "Choose an ISO patch (.xdelta)",
        ".xdelta",
        start_import,
        import_status,
    };
    psp_settings_set_mod_installer(inst);
    // PSPRECOMP_XMOD_IMPORT=<patch> (tests, scripts): install it at start the way the menu does, print the result and quit
    if (const char* p = std::getenv("PSPRECOMP_XMOD_IMPORT")) {
        if (p[0] && start_import(p)) {
            while (g_prog.busy.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::string t;
            import_status(nullptr, nullptr, &t);
            std::fprintf(stderr, "[XMOD_IMPORT] result:\n%s\n", t.c_str());
            std::fflush(stderr);
            std::_Exit(t.rfind("Installed", 0) == 0 || t.find("already installed") != std::string::npos ? 0 : 1);
        }
    }
}
