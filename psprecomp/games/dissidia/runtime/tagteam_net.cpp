// games/dissidia/runtime/tagteam_net.cpp - the N-player lock-step of a Communications Mode tag battle (docs/tag-team.md
// "Multiplayer: N-player lock-step").
//
// Retail (docs/multiplayer.md "Battle protocol", docs/tag-team.md "Multiplayer: players and slots"): only the two machines whose
// players are on the stage run the lock-step (EBOOT FUN_0890D664: read the pad, send one 16-bit input word per frame to the peer,
// deliver a frame when every player's word for it is here); the other members are spectators (FUN_0890DF58) that get the fighters'
// input history in batches every 30 frames, so they run up to ~30 frames behind. Both are reached through one dispatcher,
// FUN_0890C5BC(net object), which picks by the object's +64 (0 = player, 1 = spectator).
//
// In a tag battle this file replaces that dispatcher on EVERY machine of the group with one protocol:
//   - every member samples its own pad each frame (the game's own sceCtrl read and packing, FUN_0890CE58) and sends its input words
//     (the newest 8, like retail) straight to every other member on a side channel: no machine forwards anyone's input;
//   - "streams" are the fighters' input paths: stream 0 / 1 = the slot fighters (the game's pad slots 0 / 1), stream 2 + n = tag
//     extra n (a pad slot of ours, tagteam.cpp gives it to the extra's private controller object). Each stream has a driver schedule
//     (from frame F: player MAC, or nobody = the AI / no input), decided from battle state only (tagteam.cpp), so every machine has the
//     same schedule;
//   - frame F is delivered when the words for F of the players who drive a stream at F are here (the others are not waited for), then
//     every stream's word is unpacked (FUN_0890D090) into its pad slot. Same code path for every driven fighter, so a partner's driver
//     gets the same delay as a slot fighter's.
// Input delay (psp_netplay_input_delay) and the desync check (netplay.cpp) apply as for the retail lock-step.
//
// Test switches:
//   PSPRECOMP_TTLOCK=0              keep the game's own lock-step (tag battles then have no human partners)
//   PSPRECOMP_TTLOCK_LOG=1          log the lock-step (start, drivers, calls / waits every 5 s, [TTSYNC] every fighter every 30 frames)
//   PSPRECOMP_TTLOCK_PADTEST=1      single player: tag extras flagged human are driven by the local pad (mirror of the player)
//   PSPRECOMP_TTLOCK_FOLLOW=1       the camera follows the extra this machine drives (splits the battle today: camera-dependent movement)
//   PSPRECOMP_TTLOCK_LAT=1          log every delivered frame in which a stream's buttons change ([TTLAT], for the latency measurement)
//   PSPRECOMP_TTNET_LOWHP=<slot>:<frame>   test aid: that slot fighter's live HP -> 1 at that lock-step frame
#include "psp_game_module.h"
#include "psp_runtime.h"
#include "psp_netplay.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_adhoc.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

bool dissidia_tagteam_comms_battle();          // tagteam.cpp: a Communications Mode tag battle (every machine of the group agrees)
uint32_t dissidia_tagteam_battle_gen();
int dissidia_tagteam_fighters(uint32_t* out, int max);   // tagteam.cpp: every fighter of the battle (slots, extras)        // tagteam.cpp: +1 per battle setup
void dissidia_netplay_frame_delivered(uint32_t frame);   // netplay.cpp: desync check / dumps for a delivered frame
void dissidia_netplay_note_delay(int delay);

namespace {
constexpr uint32_t NET_DISPATCH = 0x0890C5BCu;   // EBOOT: per-frame lock-step update (a0 = net object; +64: 0 = FUN_0890D664, 1 = FUN_0890DF58)
constexpr uint32_t NET_PACK = 0x0890CE58u;       // EBOOT: (net object, SceCtrlData*) -> 16-bit input word
constexpr uint32_t NET_UNPACK = 0x0890D090u;     // EBOOT: (net object, SceCtrlData* out, word)
constexpr uint32_t CTRL_READ = 0x089C5AA8u;      // sceCtrlReadBufferPositive (the game's import stub)
constexpr uint32_t PAD_UPDATE = 0x08871E24u;     // EBOOT: a pad slot's per-frame update (a0 = slot; +80 == 1: take the SceCtrlData at +64 instead of the local pad)
constexpr uint32_t CTRL_UPDATE_ALL = 0x08872D88u;   // EBOOT: (pad manager) the two controller objects' per-frame update, FUN_088725A0 each (after the pad slots')
constexpr uint32_t CTRL_UPDATE = 0x088725A0u;       // EBOOT: a controller object's per-frame update: held / pressed / released from its pad slot (+0x40)
constexpr uint32_t PAD_SLOTS_PTR = 0x089D6008u;  // the pad manager: two 96-byte pad slots
constexpr uint32_t BATTLE_PTR = 0x089D6A10u;
constexpr uint32_t OFF_FLAGS = 16, OFF_MODE = 64, OFF_LOCAL_IDX = 68, OFF_FRAME = 72, OFF_PEERS = 76, OFF_PEER_COUNT = 852;
constexpr uint16_t CH_INPUT = 2;
constexpr int MAX_STREAMS = 12, RING = 512, WINDOW = 8, MAX_LEAD = 4;
using Mac = std::array<uint8_t, 6>;

uint8_t* g_ram = nullptr;
FuncPtr g_orig_dispatch = nullptr, g_orig_pad_update = nullptr, g_orig_ctrl_all = nullptr, g_ctrl_update = nullptr, g_pack = nullptr, g_unpack = nullptr, g_ctrl_read = nullptr;
bool g_enabled = true, g_log = false, g_padtest = false, g_lat = false;

bool ok(uint32_t a, uint32_t n = 4) { return a >= 0x08800000u && a + n < 0x0A000000u; }
uint32_t rd(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
void wr(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
std::string mac_str(const Mac& m) { char b[24]; std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]); return b; }

struct Ring { uint32_t tag[RING] = {}; uint16_t w[RING] = {}; uint32_t newest = 0;   // tag = frame + 1 (0 = empty)
    bool has(uint32_t f) const { return tag[f % RING] == f + 1; }
    void put(uint32_t f, uint16_t v) { tag[f % RING] = f + 1; w[f % RING] = v; if (f + 1 > newest) newest = f + 1; } };
struct Sched { uint32_t from; bool human; Mac mac; };
struct Stream { std::vector<Sched> s; uint32_t pad = 0, ctrl = 0, gen = 0; uint16_t last = 0; };

// ---- state of the running battle ----
bool g_running = false;
uint32_t g_gen = 0xFFFFFFFFu, g_obj = 0, g_F = 0, g_seq = 0, g_waits = 0, g_delivered = 0;
Mac g_me{};
Ring g_local;
std::map<Mac, Ring> g_remote;
Stream g_streams[MAX_STREAMS];
uint32_t g_wait_since = 0; bool g_wait_logged = false;
uint32_t g_lowhp_slot = 0, g_lowhp_frame = 0; bool g_lowhp_done = false;

bool driver_at(int s, uint32_t f, Mac* m) {   // the stream's driver at frame f (false = nobody: the AI or no input)
    const Sched* cur = nullptr;
    for (const Sched& e : g_streams[s].s) if (e.from <= f) cur = &e;
    if (!cur || !cur->human) return false;
    *m = cur->mac; return true;
}
const Ring* ring_of(const Mac& m) { if (m == g_me) return &g_local; auto it = g_remote.find(m); return it == g_remote.end() ? nullptr : &it->second; }

// guest calls on a scratch area below the caller's stack
uint32_t guest_call(recomp_context* ctx, FuncPtr f, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0) {
    const recomp_context saved = *ctx;
    ctx->r[4] = static_cast<int32_t>(a0); ctx->r[5] = static_cast<int32_t>(a1); ctx->r[6] = static_cast<int32_t>(a2);
    ctx->r[29] = static_cast<int32_t>(saved.r[29] - 256);
    f(g_ram, ctx);
    const uint32_t v = static_cast<uint32_t>(ctx->r[2]);
    *ctx = saved;
    return v;
}

void reset(uint32_t obj) {
    g_running = true; g_obj = obj; g_F = rd(obj + OFF_FRAME); g_seq = g_F; g_waits = 0; g_delivered = 0; g_wait_since = 0; g_wait_logged = false;
    g_local = Ring{}; g_remote.clear(); g_lowhp_done = false;
    for (int k = 0; k < 2; k++) { g_streams[k].s.clear(); g_streams[k].last = 0; }   // the extras' pads (2+) are registered by tagteam.cpp when they are made
    for (int k = 2; k < MAX_STREAMS; k++) if (g_streams[k].gen != dissidia_tagteam_battle_gen()) { g_streams[k].s.clear(); g_streams[k].pad = g_streams[k].ctrl = 0; g_streams[k].last = 0; }
    psp_adhoc_local_mac(g_me.data());
    uint8_t src[6], buf[128]; while (psp_adhoc_side_recv(CH_INPUT, src, buf, sizeof buf) >= 0) {}   // the last battle's leftovers
    // The slot fighters' first drivers come from the game's own session: on a fighter's machine (+64 = 0) it is player +68 and the peer
    // is the other one; on a spectator's (+64 = 1) peer i is player i. Player k plays slot k (docs: players and slots).
    const uint32_t mode = rd(obj + OFF_MODE), me = rd(obj + OFF_LOCAL_IDX), n = rd(obj + OFF_PEER_COUNT);
    const uint32_t slots = rd(PAD_SLOTS_PTR);
    for (int k = 0; k < 2; k++) g_streams[k].pad = ok(slots, 0x200) ? slots + 96u * static_cast<uint32_t>(k) : 0;
    if (mode == 0 && me < 2) g_streams[me].s.push_back({0, true, g_me});
    for (uint32_t i = 0; i < n && i < 8; i++) {
        Mac m; std::memcpy(m.data(), g_ram + ((obj + OFF_PEERS + 84u * i) & 0x07FFFFFFu), 6);
        const uint32_t p = mode == 0 ? (i < me ? i : i + 1) : i;
        if (p < 2) g_streams[p].s.push_back({0, true, m});
    }
    std::fprintf(stderr, "[TTLOCK] battle %u: own lock-step from frame %u (game object %08X: %s, local player %u, %u peer(s)); slot 0 %s, slot 1 %s; input delay %d\n",
                 dissidia_tagteam_battle_gen(), g_F, obj, mode == 0 ? "fighter" : "spectator", me, n,
                 g_streams[0].s.empty() ? "-" : mac_str(g_streams[0].s[0].mac).c_str(), g_streams[1].s.empty() ? "-" : mac_str(g_streams[1].s[0].mac).c_str(), psp_netplay_input_delay());
    for (int k = 0; k < 2; k++) if (g_streams[k].pad && rd(g_streams[k].pad + 80u) != 1u) {
        std::fprintf(stderr, "[TTLOCK] pad slot %d was not in lock-step mode (+80 = %u): set\n", k, rd(g_streams[k].pad + 80u));
        wr(g_streams[k].pad + 80u, 1u);
    }
}

void receive() {
    uint8_t src[6], buf[128]; int n;
    while ((n = psp_adhoc_side_recv(CH_INPUT, src, buf, sizeof buf)) >= 0) {
        if (n < 10 || buf[0] != 'T' || buf[1] != 'L' || buf[2] != 1) continue;
        const int cnt = buf[3]; uint32_t first; std::memcpy(&first, buf + 4, 4);
        if (cnt <= 0 || cnt > WINDOW || n < 8 + 2 * cnt) continue;
        if (first > g_F + 256u) continue;   // not this battle (a far-ahead frame number: the previous battle's packets)
        Mac m; std::memcpy(m.data(), src, 6);
        Ring& r = g_remote[m];
        for (int i = 0; i < cnt; i++) { uint16_t w; std::memcpy(&w, buf + 8 + 2 * i, 2); if (first + i + RING > g_F) r.put(first + i, w); }
    }
}

void send_window() {
    if (g_seq == 0) return;
    const uint32_t first = g_seq > WINDOW ? g_seq - WINDOW : 0; const int cnt = static_cast<int>(g_seq - first);
    uint8_t b[8 + 2 * WINDOW]; b[0] = 'T'; b[1] = 'L'; b[2] = 1; b[3] = static_cast<uint8_t>(cnt); std::memcpy(b + 4, &first, 4);
    for (int i = 0; i < cnt; i++) { const uint16_t w = g_local.w[(first + i) % RING]; std::memcpy(b + 8 + 2 * i, &w, 2); }
    uint8_t macs[16][6]; const int n = psp_adhoc_group_macs(macs, nullptr, 16);
    for (int i = 0; i < n && i < 16; i++) psp_adhoc_side_send(macs[i], CH_INPUT, b, 8 + 2 * cnt);
}

uint16_t sample_local(recomp_context* ctx) {
    const uint32_t buf = static_cast<uint32_t>(ctx->r[29]) - 64u;   // SceCtrlData scratch (below our callees' frames)
    for (uint32_t o = 0; o < 16; o += 4) wr(buf + o, 0);
    guest_call(ctx, g_ctrl_read, buf, 1);
    return static_cast<uint16_t>(guest_call(ctx, g_pack, g_obj, buf) & 0xFFFFu);
}

void write_pad(recomp_context* ctx, uint32_t slot, uint16_t w) {
    const uint32_t buf = static_cast<uint32_t>(ctx->r[29]) - 64u;
    guest_call(ctx, g_unpack, g_obj, buf, w);
    for (uint32_t o = 0; o < 16; o += 4) wr(slot + 64u + o, rd(buf + o));
}

void dispatch_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram;
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]);
    if (!g_enabled || !ok(obj, 0x400) || !(rd(obj + OFF_FLAGS) & 1u) || !dissidia_tagteam_comms_battle()) { g_running = false; g_orig_dispatch(rdram, ctx); return; }
    if (!g_running || g_gen != dissidia_tagteam_battle_gen() || g_obj != obj) { g_gen = dissidia_tagteam_battle_gen(); reset(obj); }
    const int delay = psp_netplay_input_delay();
    dissidia_netplay_note_delay(delay);
    if (g_log) {   // calls / deliveries / waits per 5 s of wall time
        static uint32_t calls = 0, d0 = 0, w0 = 0; static auto t0 = std::chrono::steady_clock::now(); calls++;
        const auto now = std::chrono::steady_clock::now();
        if (now - t0 > std::chrono::seconds(5)) { std::fprintf(stderr, "[TTLOCK] 5 s: %u calls, %u frames delivered, %u waits, frame %u, lead %d\n", calls, g_delivered - d0, g_waits - w0, g_F, static_cast<int>(g_seq - g_F)); calls = 0; d0 = g_delivered; w0 = g_waits; t0 = now; }
    }
    receive();
    // sample the local pad for frame g_seq (the input runs at most MAX_LEAD - 1 frames ahead beyond the input delay, as retail)
    if (g_seq < g_F + static_cast<uint32_t>(delay + MAX_LEAD)) {
        const uint16_t w = sample_local(ctx);
        if (g_lat && g_seq > 0 && w != g_local.w[(g_seq - 1) % RING]) std::fprintf(stderr, "[TTLAT] sampled %04X for frame %u while the battle is at frame %u (latency %u frames)\n", w, g_seq, g_F, g_seq - g_F);
        g_local.put(g_seq, w); g_seq++;
    }
    send_window();
    // deliver frame F when the own input has run `delay` frames ahead and every driver's word for F is here
    bool ready = g_seq > g_F + static_cast<uint32_t>(delay);
    Mac missing{};
    for (int s = 0; s < MAX_STREAMS && ready; s++) {
        Mac m; if (!driver_at(s, g_F, &m)) continue;
        const Ring* r = ring_of(m); if (!r || !r->has(g_F)) { ready = false; missing = m; }
    }
    if (!ready) {
        g_waits++;
        if (!g_wait_since) g_wait_since = g_delivered + 1;
        if (!g_wait_logged && g_waits > 0 && (g_waits % 300) == 0) { g_wait_logged = true; std::fprintf(stderr, "[TTLOCK] frame %u: still waiting for %s's input (%u waits so far)\n", g_F, mac_str(missing).c_str(), g_waits); }
        ctx->r[2] = 1; return;
    }
    g_wait_since = 0; g_wait_logged = false;
    for (int s = 0; s < MAX_STREAMS; s++) {
        Stream& st = g_streams[s]; if (!st.pad) continue;
        Mac m; uint16_t w = 0;
        if (driver_at(s, g_F, &m)) w = ring_of(m)->w[g_F % RING];
        if (g_lat && w != st.last) std::fprintf(stderr, "[TTLAT] frame %u stream %d word %04X (was %04X)\n", g_F, s, w, st.last);
        st.last = w;
        write_pad(ctx, st.pad, w);
    }
    dissidia_netplay_frame_delivered(g_F);
    if (g_log && g_F % 30 == 0) {   // every fighter (extras too) at the start of this frame: compare the machines' [TTSYNC] lines
        uint32_t f[8]; const int n = dissidia_tagteam_fighters(f, 8); uint32_t h = 2166136261u; std::string line; char b[96];
        for (int i = 0; i < n; i++) {
            const uint32_t st = rd(f[i] + 0x52Cu); const uint32_t hp = ok(st, 0x40) ? rd(st + 0xCu) : 0u;
            for (uint32_t v : {rd(f[i] + 0x80u), rd(f[i] + 0x84u), rd(f[i] + 0x88u), hp, rd(f[i] + 0x180u) & 0xFFFFu}) h = (h ^ v) * 16777619u;
            float x, z; std::memcpy(&x, g_ram + ((f[i] + 0x80u) & 0x07FFFFFFu), 4); std::memcpy(&z, g_ram + ((f[i] + 0x88u) & 0x07FFFFFFu), 4);
            std::snprintf(b, sizeof b, " [%.2f,%.2f hp %u sc %d]", x, z, hp, static_cast<int16_t>(rd(f[i] + 0x180u) & 0xFFFFu)); line += b;
        }
        std::fprintf(stderr, "[TTSYNC] frame %u n %d hash %08X%s\n", g_F, n, h, line.c_str());
    }
    if (g_lowhp_frame && !g_lowhp_done && g_F >= g_lowhp_frame) {
        g_lowhp_done = true;
        const uint32_t bs = rd(BATTLE_PTR), a = ok(bs, 0x100) ? rd(bs + 56 + 4 * g_lowhp_slot) : 0, st = ok(a, 0x12D0) ? rd(a + 0x52Cu) : 0;
        if (ok(st, 0x40)) { wr(st + 0xCu, 1u); std::fprintf(stderr, "[TTNET] LOWHP: slot %u fighter %08X live HP -> 1 at lock-step frame %u\n", g_lowhp_slot, a, g_F); }
    }
    g_F++; g_delivered++;
    wr(obj + OFF_FRAME, g_F);
    if (g_log && g_F % 300 == 0) {
        std::fprintf(stderr, "[TTLOCK] frame %u: lead %d, waits %u, peers:", g_F, static_cast<int>(g_seq - g_F), g_waits);
        for (auto& [m, r] : g_remote) std::fprintf(stderr, " %s@%u", mac_str(m).c_str(), r.newest);
        std::fprintf(stderr, "\n");
    }
    ctx->r[2] = 0;
}

// The game updates its pad slots (edges, stick) in FUN_08871E24; the extras' slots get the same update right after slot 0's, so their
// input is seen at the same point of the frame as a slot fighter's.
void pad_update_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram;
    const uint32_t slot = static_cast<uint32_t>(ctx->r[4]);
    g_orig_pad_update(rdram, ctx);
    const uint32_t slots = rd(PAD_SLOTS_PTR);
    if (slot != slots) return;
    for (int s = 2; s < MAX_STREAMS; s++) {
        const uint32_t p = g_streams[s].pad; if (!ok(p, 0x60)) continue;
        if (g_padtest && !g_running) {   // single-player test: the extra mirrors the local pad
            Mac m; if (driver_at(s, 0, &m)) { const uint32_t buf = static_cast<uint32_t>(ctx->r[29]) - 64u; guest_call(ctx, g_ctrl_read, buf, 1); for (uint32_t o = 0; o < 16; o += 4) wr(p + 64u + o, rd(buf + o)); }
        }
        guest_call(ctx, g_orig_pad_update, p);
    }
}
// The game's controller objects (pad manager +192) are updated from their pad slots once per frame (FUN_08872D88); the human-driven
// extras' private controllers get the same update right after them.
void ctrl_all_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram;
    g_orig_ctrl_all(rdram, ctx);
    for (int s = 2; s < MAX_STREAMS; s++) if (ok(g_streams[s].ctrl, 0x50) && ok(g_streams[s].pad, 0x60)) guest_call(ctx, g_ctrl_update, g_streams[s].ctrl);
}
}  // namespace

// ---- interface for tagteam.cpp ----
bool dissidia_ttnet_running() { return g_running; }
// Our lock-step will run the tag battles of a group (the switch and the game functions; the same on every machine of the group).
bool dissidia_ttnet_enabled() { return g_enabled && g_orig_dispatch && g_pack && g_unpack && g_ctrl_read && g_orig_ctrl_all && g_ctrl_update; }
uint32_t dissidia_ttnet_frame() { return g_running ? g_F : 0; }
// A stream's driver from lock-step frame `from` on (nullptr = nobody: the AI). Decided from battle state, the same on every machine.
void dissidia_ttnet_set_driver(int stream, const uint8_t* mac, uint32_t from) {
    if (stream < 0 || stream >= MAX_STREAMS) return;
    Sched e{from, mac != nullptr, {}}; if (mac) std::memcpy(e.mac.data(), mac, 6);
    std::vector<Sched>& v = g_streams[stream].s;
    while (!v.empty() && v.back().from >= from) v.pop_back();
    v.push_back(e);
    std::fprintf(stderr, "[TTLOCK] stream %d driven by %s from frame %u%s\n", stream, mac ? mac_str(e.mac).c_str() : "the AI", from, mac && e.mac == g_me ? " (this machine)" : "");
}
// The pad slot (96 bytes, a copy of the game's) a tag extra's controller reads; stream = 2 + the extra's index.
void dissidia_ttnet_set_stream_pad(int stream, uint32_t slot, uint32_t ctrl) {
    if (stream < 2 || stream >= MAX_STREAMS) return;
    g_streams[stream].pad = slot; g_streams[stream].ctrl = ctrl; g_streams[stream].last = 0; g_streams[stream].gen = dissidia_tagteam_battle_gen();
    g_streams[stream].s.clear();
}
// Does this machine drive the stream at frame f (camera / HUD follow the fighter it drives)?
bool dissidia_ttnet_local_drives(int stream, uint32_t f) { Mac m; return stream >= 0 && stream < MAX_STREAMS && driver_at(stream, f, &m) && m == g_me; }
bool dissidia_ttnet_padtest() { return g_padtest; }
void dissidia_ttnet_slot_rebuilt(int slot, int charId) { std::fprintf(stderr, "[TTLOCK] slot %d rebuilt as char %d at lock-step frame %u\n", slot, charId, g_F); }
uint32_t dissidia_ttnet_object() { return g_obj; }

void dissidia_ttnet_install(uint8_t* rdram) {
    g_ram = rdram;
    if (const char* e = std::getenv("PSPRECOMP_TTLOCK")) g_enabled = std::atoi(e) != 0;
    g_log = std::getenv("PSPRECOMP_TTLOCK_LOG") != nullptr;
    g_padtest = std::getenv("PSPRECOMP_TTLOCK_PADTEST") != nullptr;
    g_lat = std::getenv("PSPRECOMP_TTLOCK_LAT") != nullptr;
    if (const char* e = std::getenv("PSPRECOMP_TTNET_LOWHP")) std::sscanf(e, "%u:%u", &g_lowhp_slot, &g_lowhp_frame);
    g_orig_dispatch = psp_dispatch_probe_lookup(NET_DISPATCH);
    g_orig_pad_update = psp_dispatch_probe_lookup(PAD_UPDATE);
    g_orig_ctrl_all = psp_dispatch_probe_lookup(CTRL_UPDATE_ALL); g_ctrl_update = psp_dispatch_probe_lookup(CTRL_UPDATE);
    g_pack = psp_dispatch_probe_lookup(NET_PACK); g_unpack = psp_dispatch_probe_lookup(NET_UNPACK); g_ctrl_read = psp_dispatch_probe_lookup(CTRL_READ);
    if (!g_orig_dispatch || !g_orig_pad_update || !g_pack || !g_unpack || !g_ctrl_read || !g_orig_ctrl_all || !g_ctrl_update) { std::fprintf(stderr, "[TTLOCK] lock-step functions not found: the game's own lock-step stays\n"); return; }
    psp_dispatch_register(NET_DISPATCH, dispatch_hook);
    psp_dispatch_register(PAD_UPDATE, pad_update_hook);
    psp_dispatch_register(CTRL_UPDATE_ALL, ctrl_all_hook);
}
