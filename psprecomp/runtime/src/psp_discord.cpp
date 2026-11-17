// psp_discord.cpp — Discord Rich Presence over the desktop client's local IPC (see psp_discord.h).
#include "psp_discord.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif !defined(__ANDROID__)
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

namespace psp_discord {

// ---- JSON ------------------------------------------------------------------------------------------------------------

std::string json_string(const std::string& s, size_t max_bytes) {
    std::string out;
    out.reserve(s.size() + 2);
    size_t used = 0;   // bytes of `s` content written (escapes count as their source character)
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        bool valid = true;
        if (c >= 0x80) {
            len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
            if (len == 0 || c == 0xC0 || c == 0xC1 || c > 0xF4 || i + len > s.size()) valid = false;
            for (size_t k = 1; valid && k < len; k++)
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) valid = false;
            if (!valid) len = 1;
        }
        const size_t cost = valid ? len : 3;   // U+FFFD is 3 bytes
        if (max_bytes && used + cost > max_bytes) break;
        used += cost;
        if (!valid) out += "\xEF\xBF\xBD";
        else if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out.append(s, i, len);
        i += len;
    }
    return out;
}

namespace {

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

// "key":"value", with Discord's length rules: details / state / image texts need 2..128 characters (shorter ones are left out:
// Discord rejects the whole activity otherwise), image keys / URLs up to 256 bytes.
void field(std::string& o, bool& first, const char* key, const std::string& raw, size_t max_bytes, size_t min_chars = 2) {
    const std::string v = trimmed(raw);
    if (v.size() < min_chars) return;
    if (!first) o += ',';
    first = false;
    o += '"'; o += key; o += "\":\""; o += json_string(v, max_bytes); o += '"';
}

}  // namespace

std::string activity_json(const Activity* a, int64_t pid, const std::string& nonce) {
    std::string o = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + ",\"activity\":";
    if (!a) {
        o += "null";
    } else {
        o += "{\"type\":0,\"instance\":false";
        bool first = false;
        field(o, first, "details", a->details, 128);
        field(o, first, "state", a->state, 128);
        if (a->start > 0) o += ",\"timestamps\":{\"start\":" + std::to_string(a->start * 1000) + "}";   // milliseconds
        std::string as = "{";
        bool afirst = true;
        field(as, afirst, "large_image", a->large_image, 256, 1);
        field(as, afirst, "large_text", a->large_text, 128);
        field(as, afirst, "small_image", a->small_image, 256, 1);
        field(as, afirst, "small_text", a->small_text, 128);
        if (!afirst) o += ",\"assets\":" + as + "}";
        if (a->party_size > 0 && a->party_max >= a->party_size) {
            const std::string id = trimmed(a->party_id).empty() ? std::string("party") : trimmed(a->party_id);
            o += ",\"party\":{\"id\":\"" + json_string(id, 128) + "\",\"size\":[" + std::to_string(a->party_size) + "," +
                 std::to_string(a->party_max) + "]}";
        }
        std::string bs;
        int nb = 0;
        for (const auto& b : a->buttons) {
            const std::string label = trimmed(b.first), url = trimmed(b.second);
            if (nb == 2 || label.empty() || url.size() > 512 || url.rfind("https://", 0) != 0) continue;
            bs += nb ? "," : "";
            bs += "{\"label\":\"" + json_string(label, 32) + "\",\"url\":\"" + json_string(url) + "\"}";
            nb++;
        }
        if (nb) o += ",\"buttons\":[" + bs + "]";
        o += "}";
    }
    o += "},\"nonce\":\"" + json_string(nonce) + "\"}";
    return o;
}

#if defined(__ANDROID__)

void start(const std::string&, Provider) {}
void stop() {}
void set_enabled(bool) {}
bool enabled() { return false; }
bool available() { return false; }
std::string status() { return "Not available"; }

#else

namespace {

enum Op : uint32_t { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

std::mutex g_mu;                       // g_status, g_user; the condition variable
std::condition_variable g_cv;
std::thread g_thread;
std::atomic<bool> g_quit{false}, g_on{true}, g_started{false}, g_done{false};
std::string g_app_id;
Provider g_provider = nullptr;
std::string g_status = "Off";
bool g_log = false;

void set_status(const std::string& s) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_status != s && g_log) std::fprintf(stderr, "[DISCORD] status: %s\n", s.c_str());
    g_status = s;
}

using Clock = std::chrono::steady_clock;
double now_s() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// One connection to the client: a Unix socket or a Windows named pipe.
class Conn {
public:
    ~Conn() { close(); }
    bool is_open() const {
#if defined(_WIN32)
        return h_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }
    bool open(const std::string& path) {
        close();
#if defined(_WIN32)
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        return h_ != INVALID_HANDLE_VALUE;
#else
        sockaddr_un sa{};
        if (path.size() >= sizeof(sa.sun_path)) return false;
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
#  ifdef SO_NOSIGPIPE
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#  endif
        timeval tv{2, 0};   // a client that stops reading must not hang the thread for long
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        sa.sun_family = AF_UNIX;
        std::memcpy(sa.sun_path, path.c_str(), path.size() + 1);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) { close(); return false; }
        return true;
#endif
    }
    void close() {
#if defined(_WIN32)
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
#endif
    }
    bool write_all(const void* data, size_t n) {
        const char* p = static_cast<const char*>(data);
        while (n > 0 && is_open()) {
#if defined(_WIN32)
            DWORD w = 0;
            if (!WriteFile(h_, p, DWORD(n), &w, nullptr) || w == 0) { close(); return false; }
#else
#  ifdef MSG_NOSIGNAL
            const ssize_t w = ::send(fd_, p, n, MSG_NOSIGNAL);
#  else
            const ssize_t w = ::send(fd_, p, n, 0);
#  endif
            if (w <= 0) { close(); return false; }
#endif
            p += w; n -= size_t(w);
        }
        return n == 0;
    }
    // 1 = bytes are waiting, 0 = nothing within `timeout_ms`, -1 = closed / error (the connection is closed).
    int readable(int timeout_ms) {
        if (!is_open()) return -1;
#if defined(_WIN32)
        const double until = now_s() + timeout_ms / 1000.0;
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) { close(); return -1; }
            if (avail > 0) return 1;
            if (now_s() >= until) return 0;
            Sleep(10);
        }
#else
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, timeout_ms);
        if (r < 0) { close(); return -1; }
        if (r == 0) return 0;
        if (p.revents & POLLIN) return 1;
        close();   // POLLHUP / POLLERR without data
        return -1;
#endif
    }
    // Reads exactly n bytes, giving up after `timeout_ms` or when the presence thread is told to quit.
    bool read_all(void* data, size_t n, int timeout_ms) {
        char* p = static_cast<char*>(data);
        const double until = now_s() + timeout_ms / 1000.0;
        while (n > 0) {
            if (g_quit.load() || now_s() > until) return false;
            const int r = readable(100);
            if (r < 0) return false;
            if (r == 0) continue;
#if defined(_WIN32)
            DWORD avail = 0, got = 0;   // never ask for more than is there: ReadFile would block until it all came
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) { close(); return false; }
            if (!ReadFile(h_, p, std::min<DWORD>(DWORD(n), avail), &got, nullptr) || got == 0) { close(); return false; }
#else
            const ssize_t got = ::recv(fd_, p, n, 0);
            if (got <= 0) { close(); return false; }
#endif
            p += got; n -= size_t(got);
        }
        return true;
    }

private:
#if defined(_WIN32)
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

bool send_frame(Conn& c, uint32_t op, const std::string& json) {
    if (g_log) std::fprintf(stderr, "[DISCORD] -> op %u %s\n", op, json.c_str());
    const uint32_t len = uint32_t(json.size());
    unsigned char h[8] = {uint8_t(op), uint8_t(op >> 8), uint8_t(op >> 16), uint8_t(op >> 24),
                          uint8_t(len), uint8_t(len >> 8), uint8_t(len >> 16), uint8_t(len >> 24)};
    std::string buf(reinterpret_cast<char*>(h), 8);
    buf += json;
    return c.write_all(buf.data(), buf.size());
}

bool read_frame(Conn& c, uint32_t* op, std::string* json, int timeout_ms) {
    unsigned char h[8];
    if (!c.read_all(h, 8, timeout_ms)) return false;
    *op = uint32_t(h[0]) | uint32_t(h[1]) << 8 | uint32_t(h[2]) << 16 | uint32_t(h[3]) << 24;
    const uint32_t len = uint32_t(h[4]) | uint32_t(h[5]) << 8 | uint32_t(h[6]) << 16 | uint32_t(h[7]) << 24;
    if (len > (1u << 20)) { c.close(); return false; }   // not a Discord frame
    json->assign(len, '\0');
    if (len && !c.read_all(&(*json)[0], len, timeout_ms)) return false;
    if (g_log) std::fprintf(stderr, "[DISCORD] <- op %u %s\n", *op, json->c_str());
    return true;
}

// The string value of the first "key":"..." after `from` (no full JSON parser needed for READY / ERROR / CLOSE).
std::string json_field(const std::string& j, const char* key, size_t from = 0) {
    const std::string pat = std::string("\"") + key + "\":";
    size_t p = j.find(pat, from);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < j.size() && j[p] == ' ') p++;
    if (p >= j.size() || j[p] != '"') return "";
    std::string v;
    for (++p; p < j.size() && j[p] != '"'; ++p) {
        if (j[p] == '\\' && p + 1 < j.size()) {
            ++p;
            if (j[p] == 'u') { v += '?'; p += 4; continue; }
            v += j[p] == 'n' ? '\n' : j[p];
        } else v += j[p];
    }
    return v;
}

std::vector<std::string> candidates() {
    std::vector<std::string> out;
    if (const char* p = std::getenv("PSPRECOMP_DISCORD_IPC"); p && *p) { out.push_back(p); return out; }
#if defined(_WIN32)
    for (int i = 0; i < 10; i++) out.push_back("\\\\.\\pipe\\discord-ipc-" + std::to_string(i));
#else
    std::vector<std::string> bases;
    for (const char* e : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
        if (const char* v = std::getenv(e); v && *v) bases.push_back(v);
    bases.push_back("/tmp");
    // The client itself, then the sandboxed packages (Flatpak, Snap) and Vesktop, which put their socket in a sub-folder.
    static const char* const subs[] = {"", "app/com.discordapp.Discord/", "app/com.discordapp.DiscordCanary/", "app/com.discordapp.DiscordPTB/",
                                       "snap.discord/", "snap.discord-canary/", ".flatpak/dev.vencord.Vesktop/xdg-run/",
                                       "app/dev.vencord.Vesktop/"};
    std::vector<std::string> seen;
    for (std::string b : bases) {
        while (b.size() > 1 && b.back() == '/') b.pop_back();
        if (std::find(seen.begin(), seen.end(), b) != seen.end()) continue;
        seen.push_back(b);
        for (const char* s : subs)
            for (int i = 0; i < 10; i++) out.push_back(b + "/" + s + "discord-ipc-" + std::to_string(i));
    }
#endif
    return out;
}

int64_t self_pid() {
#if defined(_WIN32)
    return int64_t(GetCurrentProcessId());
#else
    return int64_t(::getpid());
#endif
}

// Open the first socket that answers the handshake with READY. Returns false with a status line for the settings menu.
bool connect_client(Conn& c, std::string* why, bool* refused) {
    *refused = false;
    *why = "Discord is not running";
    for (const std::string& path : candidates()) {
        if (g_quit.load() || !g_on.load()) return false;
        if (!c.open(path)) continue;
        if (!send_frame(c, OP_HANDSHAKE, "{\"v\":1,\"client_id\":\"" + json_string(g_app_id, 64) + "\"}")) continue;
        uint32_t op = 0;
        std::string j;
        if (!read_frame(c, &op, &j, 5000)) { c.close(); continue; }
        if (op == OP_FRAME && j.find("\"READY\"") != std::string::npos) {
            const size_t u = j.find("\"user\"");
            std::string name = u == std::string::npos ? "" : json_field(j, "global_name", u);
            if (name.empty() && u != std::string::npos) name = json_field(j, "username", u);
            std::fprintf(stderr, "[DISCORD] connected (%s)%s%s\n", path.c_str(), name.empty() ? "" : " as ", name.c_str());
            *why = name.empty() ? "Connected" : "Connected as " + name;
            return true;
        }
        // CLOSE (op 2): e.g. {"code":4000,"message":"Invalid Client ID"}; the next socket would say the same
        const std::string msg = json_field(j, "message");
        std::fprintf(stderr, "[DISCORD] %s refused the connection: %s\n", path.c_str(), j.c_str());
        *why = "Discord refused: " + (msg.empty() ? std::string("unknown reason") : msg);
        *refused = true;
        c.close();
        return false;
    }
    return false;
}

void worker() {
    Conn c;
    double next_try = 0, backoff = 5, last_send = -1e9;
    std::string last_body;       // the activity last sent (payload without nonce); "" = nothing sent on this connection
    bool shown = false;          // an activity is up on this connection (so turning off / quitting clears it)
    uint64_t nonce = 0;
    const int64_t pid = self_pid();
    auto drop = [&](const std::string& why, double retry_in) {
        c.close();
        last_body.clear();
        shown = false;
        next_try = now_s() + retry_in;
        set_status(why);
    };
    auto send_activity = [&](const Activity* a, const std::string& body) {
        if (!send_frame(c, OP_FRAME, activity_json(a, pid, std::to_string(++nonce)))) { drop("Discord is not running", 2); return; }
        last_body = body;
        shown = a != nullptr;
        last_send = now_s();
    };
    while (!g_quit.load()) {
        {
            std::unique_lock<std::mutex> l(g_mu);
            g_cv.wait_for(l, std::chrono::seconds(1));
        }
        if (g_quit.load()) break;
        if (!g_on.load()) {
            set_status("Off");
            if (c.is_open()) {
                if (shown) send_activity(nullptr, "");
                c.close();
                std::fprintf(stderr, "[DISCORD] turned off\n");
            }
            last_body.clear(); shown = false; next_try = 0; backoff = 5;
            continue;
        }
        if (!c.is_open()) {
            if (now_s() < next_try) continue;
            std::string why;
            bool refused = false;
            if (!connect_client(c, &why, &refused)) {
                if (g_quit.load()) break;
                if (!g_on.load()) continue;
                next_try = now_s() + (refused ? 300.0 : backoff);
                backoff = std::min(backoff * 2, 60.0);
                set_status(why);
                continue;
            }
            backoff = 5;
            last_body.clear();
            shown = false;
            set_status(why);
        }
        // what the client sent meanwhile: pings, errors, a close
        uint32_t op = 0;
        std::string j;
        while (c.is_open() && c.readable(0) == 1 && read_frame(c, &op, &j, 1000)) {
            if (op == OP_PING) send_frame(c, OP_PONG, j);
            else if (op == OP_CLOSE) { std::fprintf(stderr, "[DISCORD] the client closed the connection: %s\n", j.c_str()); c.close(); }
            else if (op == OP_FRAME && j.find("\"ERROR\"") != std::string::npos) std::fprintf(stderr, "[DISCORD] error: %s\n", j.c_str());
        }
        if (!c.is_open()) { std::fprintf(stderr, "[DISCORD] disconnected\n"); drop("Discord is not running", 2); continue; }
        Activity a;
        const bool has = g_provider && g_provider(&a);
        const std::string body = activity_json(has ? &a : nullptr, pid, "");
        if (body == last_body || (!has && !shown)) continue;
        if (now_s() - last_send < 4.0) continue;   // Discord's limit is 5 per 20 s; the newest state goes out on a later round
        send_activity(has ? &a : nullptr, body);
    }
    if (c.is_open() && shown) send_activity(nullptr, "");   // quit: take the activity down at once (the client would after a few s)
    c.close();
    g_done.store(true);
}

}  // namespace

void start(const std::string& app_id, Provider provider) {
    if (g_started.exchange(true)) return;
    g_app_id = app_id;
    g_provider = provider;
    if (const char* id = std::getenv("PSPRECOMP_DISCORD_APP_ID"); id && *id) g_app_id = id;
    g_log = std::getenv("PSPRECOMP_DISCORD_LOG") != nullptr;
    const char* e = std::getenv("PSPRECOMP_DISCORD");
    g_on.store(!(e && e[0] == '0'));
    if (g_app_id.empty()) {   // nothing to talk to Discord as
        g_on.store(false);
        g_started.store(false);
        set_status("Not available");
        return;
    }
    set_status(g_on.load() ? "Looking for Discord" : "Off");
    g_thread = std::thread(worker);
}

void stop() {
    if (!g_thread.joinable()) return;
    g_quit.store(true);
    g_cv.notify_all();
    const double until = now_s() + 1.5;
    while (!g_done.load() && now_s() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_done.load()) g_thread.join();
    else g_thread.detach();   // stuck in a write to a client that stopped reading: the process is ending anyway
}

void set_enabled(bool on) {
    g_on.store(on);
    set_status(on ? "Looking for Discord" : "Off");
    g_cv.notify_all();
}
bool enabled() { return g_on.load(); }
bool available() { return g_started.load(); }
std::string status() {
    std::lock_guard<std::mutex> l(g_mu);
    return g_status;
}

#endif  // !__ANDROID__

}  // namespace psp_discord
