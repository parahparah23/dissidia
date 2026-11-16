// games/dissidia/runtime/net_packs.cpp — online play with a player who uses a retail-moves mod (Bartz's Dissidia 1
// moves) when this player doesn't have it: the other player sends what this game needs, as verified packs.
//
// Trust: none in the peer. The packs that may be received are compiled in (net_packs.inc, from
// games/dissidia/net_packs.json via tools/netpack.py): every file with its size and SHA-256, the SHA-256 of the retail
// file its delta applies to, and the abilities. A peer only ever sends delta bytes. This game
//   * applies a delta (bounds-checked: every copy inside its own retail file, the output never past the listed size)
//     to its OWN retail file, and keeps the result only when size and SHA-256 are the listed ones;
//   * stores it as <mods>/.net_cache/<sha256>.bin, a name made here from the list, never from what the peer sent;
//   * makes the ability records itself, from the list (a copy of its own record the list names, the listed command);
//   * refuses to start a match with a player who uses a pack it does not know (another version, an edited mod, a pack
//     the player cannot share) or has not received and verified yet, and with a player who has not confirmed having
//     the packs this player uses (an older build never confirms: no match with it while a pack is in use).
// So a peer can make this game use exactly the listed files and abilities, or nothing.
//
// Wire (the ad-hoc side channel, psp_adhoc_side_*; little endian):
//   'M' 1 nuse {name[24] u16 version u8 state(1 = shareable, 2 = in use, not shareable)}*  nready {name[24] u16 version}*
//   'R' sha256[32] u32 offset u16 length          request a piece of a delta (at most kChunk bytes)
//   'D' sha256[32] u32 total u32 offset bytes     a piece of a delta (total = the delta's size)
// A sender serves only the deltas tools/netpack.py wrote for packs whose files it has installed byte for byte
// (<mods>/.net/<sha256 of the result>.dlt).
#include "psp_game_module.h"
#include "hle/psp_hle_adhoc.h"
#include "package_mods.h"
#include "net_delta.h"
#include "voice_merge.h"
#include "recomp.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace psp_cloud { std::string sha256_hex(const std::vector<uint8_t>& data); }
std::string dissidia_mods_dir();
std::string dissidia_package_dir();
std::string dissidia_voice_jp_dir();
void dissidia_mods_net_possible();
bool dissidia_mods_add_override(uint8_t* rdram, const std::string& asset, const std::string& file, uint32_t size);
int dissidia_retail_moves_state(const std::string& folder);
bool dissidia_ablt_ensure(uint8_t* rdram, uint16_t id, uint16_t copy, uint16_t cmd, uint8_t type, uint8_t phase, uint8_t owner);

namespace {

struct NetFile { const char* path; uint32_t size; const char* sha; uint32_t base_size; const char* base_sha; };
struct NetAbil { uint16_t id, copy, cmd; uint8_t type, phase; };
struct NetPack { const char* name; int version; int charId; std::vector<NetFile> files; std::vector<NetAbil> abilities; };
const std::vector<NetPack>& packs() {
    static const std::vector<NetPack> v = {
#include "net_packs.inc"
    };
    return v;
}

using Mac = std::array<uint8_t, 6>;
constexpr size_t kName = 24, kChunk = 1024, kWindow = 24;
constexpr uint32_t kMaxDelta = 8u << 20;
constexpr int64_t kResendUs = 400000, kGiveUpUs = 90000000, kPeerGoneUs = 10000000, kManifestUs = 1000000;
const Mac kBcast{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string mac_str(const Mac& m) {
    char b[24];
    std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}
bool read_file(const std::string& p, std::vector<uint8_t>* out, uint64_t max) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0 || (uint64_t)st.st_size > max) return false;
    std::ifstream f(p, std::ios::binary);
    out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return f.good() || f.eof();
}
std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; i++) { std::snprintf(b, sizeof b, "%02x", p[i]); s += b; }
    return s;
}
bool unhex(const char* h, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        if (std::sscanf(h + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

// ---- state (g_mu; never held while calling into the ad-hoc layer, which calls the handler and filter with its own lock) ----
struct Peer {
    int64_t seen = 0;
    bool manifest = false;
    std::vector<std::pair<int, int>> uses;   // (pack index or -1 = unknown to this build, state)
    std::set<int> ready;                     // packs the peer says it has
};
struct Download {
    int pack = -1, file = -1;
    Mac from{};
    std::vector<uint8_t> data;
    uint32_t total = 0;                      // 0 = not known yet (first piece not received)
    std::vector<uint8_t> got;                // per piece
    std::map<uint32_t, int64_t> asked;       // piece -> when requested
    int64_t started = 0;
};
std::mutex g_mu;
std::map<Mac, Peer> g_peers;
std::deque<std::pair<Mac, std::vector<uint8_t>>> g_inbox;
std::vector<int> g_file_ok;                  // per (pack, file) flattened: -1 unknown, 0 no, 1 installed, 2 cached
std::vector<int> g_pack_state;               // per pack: 0 not ready, 1 ready, 2 failed (a received file did not verify)
std::vector<bool> g_want_active, g_active;   // a peer in the group uses it / applied in this session
std::vector<bool> g_share;                   // this player uses it and can send it
std::vector<int> g_use;                      // this player uses it: 1 shareable, 2 not shareable, 0 no
std::map<std::string, std::vector<uint8_t>> g_serve;   // sha -> delta bytes this player sends
std::vector<Download> g_dl;
std::atomic<bool> g_started{false};
std::atomic<bool> g_activate_pending{false};
Mac g_own{};

size_t file_slot(int p, int f) {
    size_t n = 0;
    for (int i = 0; i < p; i++) n += packs()[i].files.size();
    return n + (size_t)f;
}
std::string cache_dir() { return dissidia_mods_dir() + "/.net_cache"; }
std::string cache_path(const NetFile& f) { return cache_dir() + "/" + f.sha + ".bin"; }
std::string installed_path(const NetFile& f) { return dissidia_mods_dir() + "/" + f.path; }

// a file is available when this player's mods dir has it byte for byte, or a verified copy is in the cache (hashed
// again on first use: the cache is a plain folder)
int check_file(const NetFile& f) {
    std::vector<uint8_t> b;
    if (read_file(installed_path(f), &b, f.size) && b.size() == f.size && psp_cloud::sha256_hex(b) == f.sha) return 1;
    if (read_file(cache_path(f), &b, f.size) && b.size() == f.size && psp_cloud::sha256_hex(b) == f.sha) return 2;
    return 0;
}

void refresh_local() {   // worker, once: what this player has and uses
    const auto& P = packs();
    size_t nf = 0;
    for (const auto& p : P) nf += p.files.size();
    std::vector<int> ok(nf, 0), st(P.size(), 0), use(P.size(), 0);
    std::vector<bool> share(P.size(), false);
    std::map<std::string, std::vector<uint8_t>> serve;
    for (size_t i = 0; i < P.size(); i++) {
        bool all = true, installed = true;
        for (size_t f = 0; f < P[i].files.size(); f++) {
            const int c = check_file(P[i].files[f]);
            ok[file_slot((int)i, (int)f)] = c;
            all = all && c != 0;
            installed = installed && c == 1;
        }
        st[i] = all ? 1 : 0;
        if (dissidia_retail_moves_state(P[i].name) != 1) continue;     // this player does not use it
        bool deltas = installed;
        for (const auto& f : P[i].files) {
            std::vector<uint8_t> d;
            if (!deltas) break;
            if (!read_file(dissidia_mods_dir() + "/.net/" + f.sha + ".dlt", &d, kMaxDelta) || d.size() < 12) { deltas = false; break; }
            serve[f.sha] = std::move(d);
        }
        share[i] = deltas;
        use[i] = deltas ? 1 : 2;
        std::fprintf(stderr, "[NETPACK] %s v%d: in use here, %s\n", P[i].name, P[i].version,
                     deltas ? "can be sent to players who don't have it" : "NOT shareable (this build differs from the known one): "
                     "players without it cannot play this player online while it is on");
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_file_ok = ok; g_pack_state = st; g_use = use; g_share = share; g_serve = std::move(serve);
    g_want_active.assign(P.size(), false);
    g_active.assign(P.size(), false);
}

int pack_by_name(const uint8_t* name, uint16_t ver) {
    char n[kName + 1];
    std::memcpy(n, name, kName);
    n[kName] = 0;
    for (size_t i = 0; i < packs().size(); i++)
        if (std::strcmp(packs()[i].name, n) == 0 && packs()[i].version == ver) return (int)i;
    return -1;
}
void put_name(std::vector<uint8_t>& b, const char* name, int ver) {
    uint8_t n[kName] = {0};
    std::strncpy(reinterpret_cast<char*>(n), name, kName - 1);
    b.insert(b.end(), n, n + kName);
    b.push_back(ver & 0xff); b.push_back((ver >> 8) & 0xff);
}
template <class T> void put(std::vector<uint8_t>& b, T v) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + sizeof v); }

std::vector<uint8_t> manifest() {   // g_mu held
    std::vector<uint8_t> b{'M', 1};
    std::vector<int> uses;
    for (size_t i = 0; i < g_use.size(); i++) if (g_use[i]) uses.push_back((int)i);
    b.push_back((uint8_t)uses.size());
    for (int i : uses) { put_name(b, packs()[i].name, packs()[i].version); b.push_back((uint8_t)g_use[i]); }
    std::vector<int> ready;
    for (size_t i = 0; i < g_pack_state.size(); i++) if (g_pack_state[i] == 1 && (g_active[i] || g_use[i])) ready.push_back((int)i);
    b.push_back((uint8_t)ready.size());
    for (int i : ready) put_name(b, packs()[i].name, packs()[i].version);
    return b;
}

// ---- network thread: copy and queue only ----
void on_side(const uint8_t mac[6], const uint8_t* data, size_t len) {
    if (!len || len > PSP_ADHOC_SIDE_MAX) return;
    Mac m;
    std::memcpy(m.data(), mac, 6);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_inbox.size() > 4096) return;           // a flood: drop
    g_inbox.emplace_back(m, std::vector<uint8_t>(data, data + len));
}

// may a match with this player start? (network thread, called with the ad-hoc lock held: only reads state)
bool allow_match(const uint8_t mac[6]) {
    Mac m;
    std::memcpy(m.data(), mac, 6);
    std::lock_guard<std::mutex> lk(g_mu);
    bool i_use = false;
    for (int u : g_use) i_use = i_use || u != 0;
    auto it = g_peers.find(m);
    if (it == g_peers.end() || !it->second.manifest) return !i_use;   // unknown player: fine only while no pack is in use here
    const Peer& p = it->second;
    for (auto [idx, state] : p.uses) {
        if (idx < 0 || state != 1) return false;                     // unknown here, or the player's build is not the listed one
        if (!g_use[idx] && !(g_pack_state[idx] == 1 && g_active[idx])) return false;   // not received / applied yet
    }
    for (size_t i = 0; i < g_use.size(); i++)
        if (g_use[i] && !p.ready.count((int)i)) return false;        // the player has not confirmed having mine
    return true;
}

// ---- worker ----
void handle(const Mac& from, const std::vector<uint8_t>& b, std::vector<std::pair<Mac, std::vector<uint8_t>>>& out) {   // g_mu held
    const int64_t t = now_us();
    if (b[0] == 'M' && b.size() >= 4 && b[1] == 1) {
        Peer& p = g_peers[from];
        p.seen = t;
        p.manifest = true;
        p.uses.clear();
        p.ready.clear();
        size_t o = 2;
        const size_t nuse = b[o++];
        for (size_t k = 0; k < nuse; k++) {
            if (o + kName + 3 > b.size()) return;
            uint16_t ver = b[o + kName] | (b[o + kName + 1] << 8);
            p.uses.emplace_back(pack_by_name(&b[o], ver), b[o + kName + 2]);
            o += kName + 3;
        }
        if (o >= b.size()) return;
        const size_t nready = b[o++];
        for (size_t k = 0; k < nready; k++) {
            if (o + kName + 2 > b.size()) return;
            const int idx = pack_by_name(&b[o], (uint16_t)(b[o + kName] | (b[o + kName + 1] << 8)));
            if (idx >= 0) p.ready.insert(idx);
            o += kName + 2;
        }
        for (auto [idx, state] : p.uses) {
            if (idx < 0) continue;
            g_want_active[idx] = true;
            if (g_pack_state[idx] == 0 && state == 1) {   // fetch from this player what is missing
                for (size_t f = 0; f < packs()[idx].files.size(); f++) {
                    if (g_file_ok[file_slot(idx, (int)f)] != 0) continue;
                    bool have = false;
                    for (auto& d : g_dl) have = have || (d.pack == idx && d.file == (int)f);
                    if (have) continue;
                    Download d;
                    d.pack = idx; d.file = (int)f; d.from = from; d.started = t;
                    g_dl.push_back(d);
                    std::fprintf(stderr, "[NETPACK] %s: fetching %s from %s\n", packs()[idx].name, packs()[idx].files[f].path, mac_str(from).c_str());
                }
            } else if (g_pack_state[idx] == 0 && state != 1 && !g_use[idx]) {
                std::fprintf(stderr, "[NETPACK] %s uses %s but cannot send it: no match with that player\n", mac_str(from).c_str(), packs()[idx].name);
            }
        }
        if (g_pack_state.size()) for (auto [idx, state] : p.uses) if (idx < 0) {
            std::fprintf(stderr, "[NETPACK] %s uses a pack this build does not know: no match with that player\n", mac_str(from).c_str());
            break;
        }
        if (g_activate_pending.load() == false) for (size_t i = 0; i < g_want_active.size(); i++)
            if (g_want_active[i] && g_pack_state[i] == 1 && !g_active[i] && !g_use[i]) g_activate_pending = true;
        return;
    }
    if (b[0] == 'R' && b.size() == 1 + 32 + 4 + 2) {
        const std::string sha = hex(&b[1], 32);
        auto it = g_serve.find(sha);
        if (it == g_serve.end()) return;                                 // not something this player sends
        uint32_t off; uint16_t n;
        std::memcpy(&off, &b[33], 4);
        std::memcpy(&n, &b[37], 2);
        const std::vector<uint8_t>& d = it->second;
        if (off >= d.size()) return;
        n = (uint16_t)std::min<size_t>({(size_t)n, kChunk, d.size() - off});
        std::vector<uint8_t> r{'D'};
        r.insert(r.end(), &b[1], &b[33]);
        put<uint32_t>(r, (uint32_t)d.size());
        put<uint32_t>(r, off);
        r.insert(r.end(), d.begin() + off, d.begin() + off + n);
        out.emplace_back(from, std::move(r));
        return;
    }
    if (b[0] == 'D' && b.size() > 1 + 32 + 8) {
        uint8_t want[32];
        for (auto& dl : g_dl) {
            if (dl.from != from) continue;
            if (!unhex(packs()[dl.pack].files[dl.file].sha, want) || std::memcmp(want, &b[1], 32) != 0) continue;
            uint32_t total, off;
            std::memcpy(&total, &b[33], 4);
            std::memcpy(&off, &b[37], 4);
            const size_t n = b.size() - 41;
            if (total < 12 || total > kMaxDelta || (dl.total && total != dl.total) || off % kChunk || n > kChunk ||
                (uint64_t)off + n > total || (off + n != total && n != kChunk)) return;   // not the shape a sender makes
            if (!dl.total) {
                dl.total = total;
                dl.data.assign(total, 0);
                dl.got.assign((total + kChunk - 1) / kChunk, 0);
            }
            const uint32_t piece = off / kChunk;
            std::memcpy(dl.data.data() + off, &b[41], n);
            dl.got[piece] = 1;
            dl.asked.erase(piece);
            return;
        }
    }
}

// a finished delta: rebuild the file from this player's own retail file and keep it only if it is the listed one
int finish(Download& dl) {   // g_mu NOT held (reads files)
    const NetFile& f = packs()[dl.pack].files[dl.file];
    std::vector<uint8_t> base, out;
    const std::string dir = dissidia_package_dir();
    if (dir.empty() || !dissidia::read_asset(dir, "", f.path, &base)) {
        std::fprintf(stderr, "[NETPACK] %s: no retail %s to apply the delta to\n", packs()[dl.pack].name, f.path);
        return -1;
    }
    if (base.size() != f.base_size || psp_cloud::sha256_hex(base) != f.base_sha) {
        std::fprintf(stderr, "[NETPACK] %s: this disc's %s is not the known retail file: cannot use the pack\n", packs()[dl.pack].name, f.path);
        return -1;
    }
    if (!dissidia::apply_delta(dl.data, base, f.size, &out) || psp_cloud::sha256_hex(out) != f.sha) {
        std::fprintf(stderr, "[NETPACK] %s: %s from %s did NOT verify: thrown away, no match with that player\n",
                     packs()[dl.pack].name, f.path, mac_str(dl.from).c_str());
        return -1;
    }
    {
        std::error_code ec;                 // std::filesystem: Windows has no two-argument mkdir
        std::filesystem::create_directories(cache_dir(), ec);
    }
    const std::string tmp = cache_path(f) + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
        if (!o) return -1;
    }
    if (std::rename(tmp.c_str(), cache_path(f).c_str()) != 0) return -1;
    std::fprintf(stderr, "[NETPACK] %s: %s verified (%u B from a %u B delta), cached\n", packs()[dl.pack].name, f.path, f.size, dl.total);
    return 2;
}

void worker() {
    refresh_local();
    int64_t next_manifest = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const int64_t t = now_us();
        std::vector<std::pair<Mac, std::vector<uint8_t>>> out;
        std::vector<Download> done;
        uint8_t peers[32][6];
        size_t npeers = 0;
        psp_adhoc_group_peers(peers, 32, &npeers, g_own.data());
        {
            std::lock_guard<std::mutex> lk(g_mu);
            while (!g_inbox.empty()) {
                auto [m, b] = std::move(g_inbox.front());
                g_inbox.pop_front();
                if (!b.empty()) handle(m, b, out);
            }
            for (auto it = g_peers.begin(); it != g_peers.end();)
                if (t - it->second.seen > kPeerGoneUs) it = g_peers.erase(it); else ++it;
            if (npeers && t >= next_manifest) { next_manifest = t + kManifestUs; out.emplace_back(kBcast, manifest()); }
            for (auto it = g_dl.begin(); it != g_dl.end();) {
                Download& d = *it;
                const bool complete = d.total && std::all_of(d.got.begin(), d.got.end(), [](uint8_t g) { return g != 0; });
                if (complete) { done.push_back(std::move(d)); it = g_dl.erase(it); continue; }
                if (t - d.started > kGiveUpUs || !g_peers.count(d.from)) {
                    std::fprintf(stderr, "[NETPACK] %s: gave up on %s (the player left or stopped sending)\n", packs()[d.pack].name, packs()[d.pack].files[d.file].path);
                    it = g_dl.erase(it);
                    continue;
                }
                uint8_t sha[32];
                if (!unhex(packs()[d.pack].files[d.file].sha, sha)) { it = g_dl.erase(it); continue; }
                auto ask = [&](uint32_t piece) {
                    std::vector<uint8_t> r{'R'};
                    r.insert(r.end(), sha, sha + 32);
                    put<uint32_t>(r, piece * (uint32_t)kChunk);
                    put<uint16_t>(r, (uint16_t)kChunk);
                    out.emplace_back(d.from, std::move(r));
                    d.asked[piece] = t;
                };
                if (!d.total) { if (!d.asked.count(0) || t - d.asked[0] > kResendUs) ask(0); }
                else {
                    for (auto& [piece, when] : d.asked) if (t - when > kResendUs) when = 0;   // resend lost ones
                    for (auto& [piece, when] : std::map<uint32_t, int64_t>(d.asked)) if (!when) ask(piece);
                    for (uint32_t piece = 0; piece < d.got.size() && d.asked.size() < kWindow; piece++)
                        if (!d.got[piece] && !d.asked.count(piece)) ask(piece);
                }
                ++it;
            }
        }
        for (auto& [m, b] : out) psp_adhoc_side_send(m.data(), b.data(), b.size());
        for (Download& d : done) {
            const int r = finish(d);
            std::lock_guard<std::mutex> lk(g_mu);
            g_file_ok[file_slot(d.pack, d.file)] = r > 0 ? r : 0;
            if (r < 0) { g_pack_state[d.pack] = 2; continue; }
            bool all = true;
            for (size_t f = 0; f < packs()[d.pack].files.size(); f++) all = all && g_file_ok[file_slot(d.pack, (int)f)] != 0;
            if (all && g_pack_state[d.pack] == 0) {
                g_pack_state[d.pack] = 1;
                g_activate_pending = true;
                std::fprintf(stderr, "[NETPACK] %s: every file received and verified\n", packs()[d.pack].name);
            }
        }
    }
}

// ---- game thread (psp_adhoc_pump): apply what a player in the group uses and this player now has ----
void on_pump(uint8_t* rdram) {
    if (!g_started.exchange(true)) std::thread(worker).detach();
    if (!g_activate_pending.exchange(false)) return;
    std::vector<int> todo;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (size_t i = 0; i < g_want_active.size(); i++)
            if (g_want_active[i] && g_pack_state[i] == 1 && !g_active[i] && !g_use[i]) todo.push_back((int)i);
    }
    for (int i : todo) {
        const NetPack& p = packs()[i];
        bool ok = true;
        for (size_t f = 0; f < p.files.size() && ok; f++) {
            int src;
            { std::lock_guard<std::mutex> lk(g_mu); src = g_file_ok[file_slot(i, (int)f)]; }
            if (src == 2) {
                std::string file = cache_path(p.files[f]);
                uint32_t size = p.files[f].size;
                const std::string jp = dissidia_voice_jp_dir();
                std::vector<uint8_t> jb, mb, out;
                int added = 0;
                if (!jp.empty() && std::strncmp(p.files[f].path, "voice/", 6) == 0 &&   // Japanese voices: their bank + the pack's added clips
                    read_file(jp + "/" + p.files[f].path, &jb, 64u << 20) && read_file(file, &mb, 64u << 20) &&
                    dissidia::merge_voice_bank(jb, mb, &out, &added)) {
                    const std::string mf = file + ".jp";
                    std::ofstream o(mf, std::ios::binary | std::ios::trunc);
                    o.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
                    if (o) { file = mf; size = (uint32_t)out.size(); }
                }
                ok = dissidia_mods_add_override(rdram, p.files[f].path, file, size);
            }
            // src 1: this player's own install of the pack, served since boot
        }
        for (const NetAbil& a : p.abilities)
            ok = ok && dissidia_ablt_ensure(rdram, a.id, a.copy, a.cmd, a.type, a.phase, (uint8_t)p.charId);
        std::lock_guard<std::mutex> lk(g_mu);
        if (ok) g_active[i] = true; else g_pack_state[i] = 2;
        std::fprintf(stderr, "[NETPACK] %s v%d %s for this session\n", p.name, p.version, ok ? "applied" : "could NOT be applied (no match with players who use it)");
    }
}

}  // namespace

// Called from dissidia_register_hooks.
void dissidia_net_packs_install() {
    if (packs().empty()) return;
    dissidia_mods_net_possible();
    psp_adhoc_side_set_handler(on_side);
    psp_adhoc_set_match_filter(allow_match);
    psp_adhoc_set_pump_hook(on_pump);
    std::fprintf(stderr, "[NETPACK] %zu known pack(s); online players can send them to this game (verified)\n", packs().size());
}
