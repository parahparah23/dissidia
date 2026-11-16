// Reflectivity per texture, see include/psp_refl_table.h.
#include "psp_refl_table.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>

uint8_t psp_refl_encode(const PspReflMaterial& m) {
    auto q = [](float v, int levels) { v = v < 0.f ? 0.f : v > 1.f ? 1.f : v; return int(v * float(levels) + 0.5f); };
    int r = q(m.refl, 7);
    if (r == 0) return 0;
    return uint8_t(r << 5 | q(m.rough, 7) << 2 | (m.metal ? 2 : 0) | (m.water ? 1 : 0));
}
PspReflMaterial psp_refl_decode(uint8_t b) {
    PspReflMaterial m;
    m.refl = float(b >> 5) / 7.f;
    m.rough = float((b >> 2) & 7) / 7.f;
    m.metal = (b & 2) != 0;
    m.water = (b & 1) != 0;
    return m;
}

int psp_refl_parse(const std::string& text, std::unordered_map<uint64_t, PspReflMaterial>& out, int* bad) {
    int n = 0, nbad = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        size_t b = line.find_first_of(" \t\r", a);
        if (b == std::string::npos) { ++nbad; continue; }
        std::string id_s = line.substr(a, b - a);
        if (id_s.size() > 2 && id_s[0] == '0' && (id_s[1] == 'x' || id_s[1] == 'X')) id_s = id_s.substr(2);
        if (id_s.empty() || id_s.size() > 16) { ++nbad; continue; }
        bool hex = true;
        for (char c : id_s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) hex = false;
        if (!hex) { ++nbad; continue; }
        const uint64_t id = std::strtoull(id_s.c_str(), nullptr, 16);
        // 1 to 4 numbers: reflectivity, roughness, metal, water
        double v[4] = {0.0, 0.0, 0.0, 0.0};
        int nv = 0;
        const char* p = line.c_str() + b;
        bool ok = true;
        for (;;) {
            while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
            if (!*p) break;
            if (nv == 4) { ok = false; break; }
            char* end = nullptr;
            v[nv] = std::strtod(p, &end);
            if (end == p) { ok = false; break; }
            ++nv;
            p = end;
        }
        if (!ok || nv == 0) { ++nbad; continue; }
        auto cl = [](double x) { return float(x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x); };
        PspReflMaterial m;
        m.refl = cl(v[0]);
        m.rough = cl(v[1]);
        m.metal = v[2] >= 0.5;
        m.water = v[3] >= 0.5;
        out[id] = m;
        ++n;
    }
    if (bad) *bad = nbad;
    return n;
}

namespace {
std::mutex g_mu;
const PspReflEntry* g_builtin = nullptr;
size_t g_builtin_n = 0;
std::unordered_map<uint64_t, PspReflMaterial> g_table;   // built-in + file
std::atomic<uint32_t> g_gen{1};
std::atomic<bool> g_active{false};
bool g_loaded = false;
long long g_file_mtime = -1;
std::chrono::steady_clock::time_point g_next_check{};

const char* file_path() {
    const char* e = std::getenv("PSPRECOMP_REFLECTIVITY");
    return e && e[0] ? e : "reflectivity.txt";
}

// Rebuild g_table from the built-in list and the file (g_mu held).
void rebuild_locked(bool log) {
    g_table.clear();
    for (size_t i = 0; i < g_builtin_n; ++i) g_table[g_builtin[i].id] = PspReflMaterial{g_builtin[i].refl, g_builtin[i].rough, g_builtin[i].metal, g_builtin[i].water};
    const size_t nb = g_table.size();
    int nf = 0, bad = 0;
    struct stat sb{};
    g_file_mtime = -1;
    if (::stat(file_path(), &sb) == 0) {
        g_file_mtime = (long long)sb.st_mtime;
        if (FILE* f = std::fopen(file_path(), "rb")) {
            std::string s;
            char buf[4096];
            size_t k;
            while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, k);
            std::fclose(f);
            nf = psp_refl_parse(s, g_table, &bad);
        }
    }
    g_active.store(!g_table.empty());
    g_gen.fetch_add(1);
    g_loaded = true;
    if (log || nf || bad)
        std::fprintf(stderr, "[REFL] reflectivity table: %zu built-in, %d from %s%s (%zu textures)\n", nb, nf, file_path(),
                     bad ? " (some lines not understood)" : "", g_table.size());
}

void check_file_locked() {
    const auto now = std::chrono::steady_clock::now();
    if (g_loaded && now < g_next_check) return;
    g_next_check = now + std::chrono::seconds(2);
    if (!g_loaded) { rebuild_locked(true); return; }
    struct stat sb{};
    const long long mt = ::stat(file_path(), &sb) == 0 ? (long long)sb.st_mtime : -1;
    if (mt != g_file_mtime) rebuild_locked(true);
}
}  // namespace

void psp_refl_set_builtin(const PspReflEntry* entries, size_t n) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_builtin = entries;
    g_builtin_n = n;
    rebuild_locked(false);
}

bool psp_refl_table_active() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_loaded) rebuild_locked(true);
    }
    return g_active.load();
}

PspReflMaterial psp_refl_lookup(uint64_t id) {
    std::lock_guard<std::mutex> lk(g_mu);
    check_file_locked();
    auto it = g_table.find(id);
    return it == g_table.end() ? PspReflMaterial{} : it->second;
}

uint32_t psp_refl_generation() { return g_gen.load(); }

void psp_refl_poll() {
    std::lock_guard<std::mutex> lk(g_mu);
    check_file_locked();
}
