// Discord Rich Presence (psp_discord.cpp): the JSON it builds, and a whole session against a fake Discord client on a Unix socket:
// handshake, first activity, rate limit (the newest state wins), ping / pong, turning off and on, the client going away, quit.
#include "psp_discord.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

using psp_discord::Activity;

static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static void json_tests() {
    CHECK(psp_discord::json_string("a\"b\\c\nd") == "a\\\"b\\\\c\\nd");
    CHECK(psp_discord::json_string(std::string("x\x01y")) == "x\\u0001y");
    CHECK(psp_discord::json_string("Ω stage") == "Ω stage");                    // UTF-8 kept
    CHECK(psp_discord::json_string(std::string("a\xFF" "b")) == "a\xEF\xBF\xBD" "b");   // invalid byte -> U+FFFD
    CHECK(psp_discord::json_string("abcΩ", 4) == "abc");                         // never cuts a character in two
    CHECK(psp_discord::json_string("abcΩ", 5) == "abcΩ");

    const std::string clear = psp_discord::activity_json(nullptr, 42, "7");
    CHECK(clear == "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":null},\"nonce\":\"7\"}");

    Activity a;
    a.details = "Quick Battle (1-on-1)";
    a.state = "Squall vs. Kefka";
    a.start = 1700000000;
    a.large_image = "https://example.org/icon.png";
    a.large_text = "x";                       // too short for Discord: left out
    a.party_size = 2; a.party_max = 2; a.party_id = "room";
    a.buttons = {{"Get it", "https://example.org"}, {"Bad", "http://insecure"}, {"Third", "https://c"}, {"Fourth", "https://d"}};
    const std::string j = psp_discord::activity_json(&a, 1, "n");
    CHECK(has(j, "\"details\":\"Quick Battle (1-on-1)\""));
    CHECK(has(j, "\"state\":\"Squall vs. Kefka\""));
    CHECK(has(j, "\"timestamps\":{\"start\":1700000000000}"));
    CHECK(has(j, "\"assets\":{\"large_image\":\"https://example.org/icon.png\"}"));
    CHECK(!has(j, "large_text"));
    CHECK(has(j, "\"party\":{\"id\":\"room\",\"size\":[2,2]}"));
    CHECK(has(j, "\"buttons\":[{\"label\":\"Get it\",\"url\":\"https://example.org\"},{\"label\":\"Third\",\"url\":\"https://c\"}]"));
    CHECK(!has(j, "insecure") && !has(j, "Fourth"));

    Activity b;
    b.details = std::string(300, 'x');
    b.state = " ";                            // blank: left out
    b.party_size = 3; b.party_max = 2;        // inconsistent: left out
    const std::string k = psp_discord::activity_json(&b, 1, "n");
    CHECK(has(k, "\"details\":\"" + std::string(128, 'x') + "\""));
    CHECK(!has(k, "\"state\"") && !has(k, "party") && !has(k, "assets") && !has(k, "timestamps") && !has(k, "buttons"));
}

// ---- a fake Discord client (Unix socket, or a named pipe on Windows) ----
static std::mutex g_mu;
static Activity g_act;
static bool g_have = true;
static bool provider(Activity* out) {
    std::lock_guard<std::mutex> l(g_mu);
    *out = g_act;
    return g_have;
}
static void set_act(const std::string& details, bool have = true) {
    std::lock_guard<std::mutex> l(g_mu);
    g_act.details = details;
    g_act.state = "state line";
    g_have = have;
}
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

#if defined(_WIN32)
using Fd = HANDLE;
static const Fd kNone = INVALID_HANDLE_VALUE;
static std::string g_pipe;
static bool read_n(Fd fd, void* p, size_t n, int ms) {
    char* c = static_cast<char*>(p);
    const double until = now() + ms / 1000.0;
    while (n) {
        DWORD avail = 0;
        if (!PeekNamedPipe(fd, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (!avail) { if (now() > until) return false; Sleep(5); continue; }
        DWORD got = 0;
        if (!ReadFile(fd, c, DWORD(std::min<size_t>(n, avail)), &got, nullptr) || !got) return false;
        c += got; n -= got;
    }
    return true;
}
static void send_raw(Fd fd, const std::string& b) { DWORD w = 0; WriteFile(fd, b.data(), DWORD(b.size()), &w, nullptr); }
static void close_fd(Fd fd) { DisconnectNamedPipe(fd); CloseHandle(fd); }
// a fresh pipe instance, waiting for the runtime to open it
static Fd accept_client(int ms) {
    Fd h = CreateNamedPipeA(g_pipe.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return kNone;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = ConnectNamedPipe(h, &ov);
    DWORD err = GetLastError();
    bool connected = (!ok && err == ERROR_PIPE_CONNECTED);
    if (!connected && !ok && err == ERROR_IO_PENDING) connected = WaitForSingleObject(ov.hEvent, DWORD(ms)) == WAIT_OBJECT_0;
    CloseHandle(ov.hEvent);
    if (!connected) { CancelIo(h); CloseHandle(h); return kNone; }
    return h;
}
#else
using Fd = int;
static const Fd kNone = -1;
static int g_srv = -1;
static bool read_n(Fd fd, void* p, size_t n, int ms) {
    char* c = static_cast<char*>(p);
    while (n) {
        pollfd pf{fd, POLLIN, 0};
        if (::poll(&pf, 1, ms) <= 0) return false;
        const ssize_t r = ::recv(fd, c, n, 0);
        if (r <= 0) return false;
        c += r; n -= size_t(r);
    }
    return true;
}
static void send_raw(Fd fd, const std::string& b) { (void)!::send(fd, b.data(), b.size(), MSG_NOSIGNAL); }
static void close_fd(Fd fd) { ::close(fd); }
static Fd accept_client(int ms) {
    pollfd pf{g_srv, POLLIN, 0};
    if (::poll(&pf, 1, ms) <= 0) return kNone;
    return ::accept(g_srv, nullptr, nullptr);
}
#endif

static bool read_frame(Fd fd, uint32_t* op, std::string* j, int ms) {
    uint32_t h[2];
    if (!read_n(fd, h, 8, ms)) return false;
    *op = h[0];
    j->assign(h[1], '\0');
    return h[1] == 0 || read_n(fd, &(*j)[0], h[1], ms);
}
static void write_frame(Fd fd, uint32_t op, const std::string& j) {
    uint32_t h[2] = {op, uint32_t(j.size())};
    std::string b(reinterpret_cast<char*>(h), 8);
    b += j;
    send_raw(fd, b);
}
// accept + handshake; returns the client or kNone
static Fd handshake(int ms) {
    const Fd fd = accept_client(ms);
    if (fd == kNone) return kNone;
    uint32_t op = 99;
    std::string j;
    CHECK(read_frame(fd, &op, &j, 3000));
    CHECK(op == 0);
    CHECK(has(j, "\"client_id\":\"123456\"") && has(j, "\"v\":1"));
    write_frame(fd, 1, "{\"cmd\":\"DISPATCH\",\"data\":{\"v\":1,\"user\":{\"id\":\"1\",\"username\":\"tester\",\"global_name\":\"Tester\"}},\"evt\":\"READY\",\"nonce\":null}");
    return fd;
}
// the next SET_ACTIVITY frame (answered like Discord does)
static bool next_activity(Fd fd, std::string* j, int ms) {
    uint32_t op = 0;
    while (read_frame(fd, &op, j, ms)) {
        if (op == 1 && has(*j, "SET_ACTIVITY")) {
            write_frame(fd, 1, "{\"cmd\":\"SET_ACTIVITY\",\"data\":null,\"evt\":null,\"nonce\":\"x\"}");
            return true;
        }
    }
    return false;
}

static void session_test() {
    std::string cleanup_dir, path;
#if defined(_WIN32)
    g_pipe = "\\\\.\\pipe\\psp-discord-test-" + std::to_string(GetCurrentProcessId());
    path = g_pipe;
    _putenv_s("PSPRECOMP_DISCORD_IPC", path.c_str());
    _putenv_s("PSPRECOMP_DISCORD", "");
    _putenv_s("PSPRECOMP_DISCORD_APP_ID", "");
#else
    char dir[] = "/tmp/psp_discord_testXXXXXX";
    if (!::mkdtemp(dir)) { CHECK(false); return; }
    cleanup_dir = dir;
    path = cleanup_dir + "/discord-ipc-0";
    g_srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strcpy(sa.sun_path, path.c_str());
    CHECK(::bind(g_srv, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0);
    CHECK(::listen(g_srv, 4) == 0);
    ::setenv("PSPRECOMP_DISCORD_IPC", path.c_str(), 1);
    ::unsetenv("PSPRECOMP_DISCORD");
    ::unsetenv("PSPRECOMP_DISCORD_APP_ID");
#endif

    set_act("first");
    psp_discord::start("123456", provider);
    CHECK(psp_discord::available() && psp_discord::enabled());

    Fd fd = handshake(5000);
    CHECK(fd != kNone);
    if (fd == kNone) return;
    std::string j;
    CHECK(next_activity(fd, &j, 5000));
    CHECK(has(j, "\"details\":\"first\"") && has(j, "\"pid\":"));
    CHECK(psp_discord::status() == "Connected as Tester");

    // rate limit: two changes in a row -> one frame, at least ~4 s after the last, with the newest state
    const double t0 = now();
    set_act("second");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    set_act("third");
    CHECK(next_activity(fd, &j, 8000));
    CHECK(now() - t0 >= 2.5);
    CHECK(has(j, "\"details\":\"third\""));

    // ping -> pong
    write_frame(fd, 3, "{\"p\":1}");
    uint32_t op = 0;
    std::string pong;
    CHECK(read_frame(fd, &op, &pong, 3000));
    CHECK(op == 4 && pong == "{\"p\":1}");

    // off: the activity is cleared and the connection closed
    psp_discord::set_enabled(false);
    CHECK(next_activity(fd, &j, 3000));
    CHECK(has(j, "\"activity\":null"));
    CHECK(!read_frame(fd, &op, &pong, 2000));   // EOF
    close_fd(fd);
    CHECK(psp_discord::status() == "Off");

    // on again: a new connection, the activity is shown again
    psp_discord::set_enabled(true);
    fd = handshake(5000);
    CHECK(fd != kNone);
    CHECK(next_activity(fd, &j, 6000));
    CHECK(has(j, "\"details\":\"third\""));

    // the client goes away (Discord restarted): the runtime comes back on its own and shows the activity again
    close_fd(fd);
    fd = handshake(8000);
    CHECK(fd != kNone);
    CHECK(next_activity(fd, &j, 6000));
    CHECK(has(j, "\"details\":\"third\""));

    // nothing to show: cleared
    set_act("", false);
    CHECK(next_activity(fd, &j, 7000));
    CHECK(has(j, "\"activity\":null"));
    set_act("back");
    CHECK(next_activity(fd, &j, 7000));
    CHECK(has(j, "\"details\":\"back\""));

    // quit: cleared before the thread ends
    std::thread t([] { psp_discord::stop(); });
    CHECK(next_activity(fd, &j, 3000));
    CHECK(has(j, "\"activity\":null"));
    t.join();
    close_fd(fd);
#if !defined(_WIN32)
    ::close(g_srv);
    ::unlink(path.c_str());
    ::rmdir(cleanup_dir.c_str());
#endif
}

int main() {
    json_tests();
    session_test();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("discord: all tests passed\n");
    return 0;
}
