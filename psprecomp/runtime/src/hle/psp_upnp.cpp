// psp_upnp.cpp: UPnP IGD port forwarding for the ad-hoc UDP port (see include/hle/psp_upnp.h).
//
// The minimum of UPnP a home router needs: an SSDP M-SEARCH on 239.255.255.250:1900 for an InternetGatewayDevice, an HTTP GET
// of the device description it names (LOCATION), the controlURL of its WANIPConnection (or WANPPPConnection) service, then SOAP
// calls to it: AddPortMapping (UDP, external port = internal port = ours, this machine's LAN address, a 3600 s lease; routers
// that only take permanent mappings get 0 after error 725), GetExternalIPAddress, and DeletePortMapping when turned off.
// Plain HTTP on the local network, blocking sockets with timeouts, on a worker thread of its own.
#include "hle/psp_upnp.h"
#include "psp_platform.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void psp_net_notice(const char* key, int min_interval_s, const char* fmt, ...);   // psp_hle_adhoc.cpp

namespace {

std::mutex g_mu;
std::condition_variable g_cv;
bool g_enabled = false, g_started = false, g_quit = false;
uint16_t g_port = 0;
std::string g_status = "Off", g_public;
// the mapping in place (for renewal and removal)
std::string g_ctl_host, g_ctl_path, g_service;
int g_ctl_port = 0;
uint16_t g_mapped = 0;

void set_status(const std::string& s) { std::lock_guard<std::mutex> lk(g_mu); g_status = s; }

void set_timeout(int fd, int ms) {
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

// The LOCATION of the first InternetGatewayDevice that answers, "" if none within the time.
std::string ssdp_discover(int wait_ms) {
    const int fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (fd < 0) return "";
    set_timeout(fd, 300);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    static const char* const kTargets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                           "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                           "urn:schemas-upnp-org:service:WANIPConnection:1"};
    std::string location;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    int round = 0;
    while (location.empty() && std::chrono::steady_clock::now() < end) {
        if (round++ % 4 == 0)
            for (const char* st : kTargets) {
                char msg[256];
                std::snprintf(msg, sizeof msg, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", st);
                sendto(fd, msg, static_cast<int>(std::strlen(msg)), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
            }
        char buf[2048];
        const int n = static_cast<int>(recv(fd, buf, sizeof buf - 1, 0));
        if (n <= 0) continue;
        buf[n] = 0;
        const std::string r(buf), rl = lower(r);
        const size_t p = rl.find("\nlocation:");
        if (p == std::string::npos) continue;
        size_t a = p + 10;
        while (a < r.size() && r[a] == ' ') a++;
        const size_t b = r.find('\r', a);
        location = r.substr(a, b == std::string::npos ? std::string::npos : b - a);
    }
    psp_plat::close_socket(fd);
    return location;
}

bool split_url(const std::string& url, std::string& host, int& port, std::string& path) {
    if (url.compare(0, 7, "http://") != 0) return false;
    const size_t s = url.find('/', 7);
    const std::string hp = url.substr(7, s == std::string::npos ? std::string::npos : s - 7);
    path = s == std::string::npos ? "/" : url.substr(s);
    const size_t c = hp.find(':');
    host = hp.substr(0, c);
    port = c == std::string::npos ? 80 : std::atoi(hp.c_str() + c + 1);
    return !host.empty() && port > 0;
}

// One HTTP request (Connection: close); the whole response, "" on failure. local_ip: this machine's address toward the host.
std::string http(const std::string& host, int port, const std::string& request, std::string* local_ip = nullptr) {
    const int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return "";
    set_timeout(fd, 3000);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 || connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        psp_plat::close_socket(fd);
        return "";
    }
    if (local_ip) {
        sockaddr_in me{};
        socklen_t ml = sizeof me;
        getsockname(fd, reinterpret_cast<sockaddr*>(&me), &ml);
        char ip[32] = {0};
        inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip);
        *local_ip = ip;
    }
    size_t sent = 0;
    while (sent < request.size()) {
        const int n = static_cast<int>(send(fd, request.data() + sent, static_cast<int>(request.size() - sent), 0));
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    std::string out;
    char buf[4096];
    for (;;) {
        const int n = static_cast<int>(recv(fd, buf, sizeof buf, 0));
        if (n <= 0 || out.size() > (1u << 20)) break;
        out.append(buf, static_cast<size_t>(n));
    }
    psp_plat::close_socket(fd);
    return out;
}

std::string tag_value(const std::string& xml, const std::string& tag, size_t from = 0) {
    const std::string lx = lower(xml), open = "<" + lower(tag) + ">", close = "</" + lower(tag) + ">";
    const size_t a = lx.find(open, from);
    if (a == std::string::npos) return "";
    const size_t b = lx.find(close, a);
    if (b == std::string::npos) return "";
    return xml.substr(a + open.size(), b - a - open.size());
}

std::string soap(const std::string& action, const std::string& args, int* http_code) {
    const std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" +
                             g_service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    char head[512];
    std::snprintf(head, sizeof head, "POST %s HTTP/1.1\r\nHOST: %s:%d\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nCONTENT-LENGTH: %zu\r\n"
                  "SOAPACTION: \"%s#%s\"\r\nConnection: close\r\n\r\n",
                  g_ctl_path.c_str(), g_ctl_host.c_str(), g_ctl_port, body.size(), g_service.c_str(), action.c_str());
    const std::string r = http(g_ctl_host, g_ctl_port, head + body);
    *http_code = 0;
    if (r.size() > 12 && r.compare(0, 5, "HTTP/") == 0) *http_code = std::atoi(r.c_str() + 9);
    return r;
}

std::string mapping_args(uint16_t port, const std::string& local_ip, int lease) {
    char a[640];
    std::snprintf(a, sizeof a, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                  "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
                  "<NewPortMappingDescription>Dissidia ad-hoc</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>",
                  port, port, local_ip.c_str(), lease);
    return a;
}

void remove_mapping() {
    if (!g_mapped || g_ctl_host.empty()) return;
    char a[256];
    std::snprintf(a, sizeof a, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>", g_mapped);
    int code = 0;
    soap("DeletePortMapping", a, &code);
    std::fprintf(stderr, "[UPNP] removed the forward of UDP %u (HTTP %d)\n", g_mapped, code);
    g_mapped = 0;
    std::lock_guard<std::mutex> lk(g_mu);
    g_public.clear();
}

// Find the router and forward the port. Returns true when the port is open.
bool map_port(uint16_t port) {
    set_status("Looking for the router...");
    const std::string location = ssdp_discover(3000);
    if (location.empty()) {
        set_status("No UPnP router found");
        psp_net_notice("upnp", 60, "UPnP: no router answered (UPnP is off on the router, or not supported). Forward UDP port %u by hand.", port);
        return false;
    }
    std::string host, path;
    int hport = 0;
    if (!split_url(location, host, hport, path)) { set_status("Router gave a bad address"); return false; }
    std::string local_ip;
    char req[512];
    std::snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHOST: %s:%d\r\nConnection: close\r\n\r\n", path.c_str(), host.c_str(), hport);
    const std::string desc = http(host, hport, req, &local_ip);
    // the WAN connection service and its control URL
    const std::string ld = lower(desc);
    std::string ctl;
    for (size_t at = 0; (at = ld.find("<service>", at)) != std::string::npos; at++) {
        const size_t end = ld.find("</service>", at);
        const std::string block = desc.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const std::string type = tag_value(block, "serviceType");
        if (type.find("WANIPConnection") != std::string::npos || type.find("WANPPPConnection") != std::string::npos) {
            g_service = type;
            ctl = tag_value(block, "controlURL");
            break;
        }
    }
    if (ctl.empty() || local_ip.empty()) {
        set_status("Router has no port forwarding service");
        psp_net_notice("upnp", 60, "UPnP: the router does not offer port forwarding. Forward UDP port %u by hand.", port);
        return false;
    }
    if (ctl.compare(0, 7, "http://") == 0) {
        if (!split_url(ctl, g_ctl_host, g_ctl_port, g_ctl_path)) return false;
    } else {
        g_ctl_host = host;
        g_ctl_port = hport;
        g_ctl_path = ctl[0] == '/' ? ctl : "/" + ctl;
    }
    int code = 0;
    std::string r = soap("AddPortMapping", mapping_args(port, local_ip, 3600), &code);
    if (code != 200 && r.find("725") != std::string::npos) r = soap("AddPortMapping", mapping_args(port, local_ip, 0), &code);   // permanent only
    if (code != 200) {
        const std::string err = tag_value(r, "errorDescription");
        set_status("Router refused" + (err.empty() ? std::string() : ": " + err));
        psp_net_notice("upnp", 60, "UPnP: the router refused to forward UDP port %u%s%s.", port, err.empty() ? "" : " (", err.empty() ? "" : (err + ")").c_str());
        return false;
    }
    g_mapped = port;
    r = soap("GetExternalIPAddress", "", &code);
    const std::string ext = tag_value(r, "NewExternalIPAddress");
    char pub[64];
    std::snprintf(pub, sizeof pub, "%s:%u", ext.empty() ? "?" : ext.c_str(), port);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_public = ext.empty() ? "" : pub;
        g_status = std::string("Open: ") + pub;
    }
    std::fprintf(stderr, "[UPNP] UDP %u forwarded to %s by the router at %s (public address %s)\n", port, local_ip.c_str(), host.c_str(), pub);
    psp_net_notice("upnp", 0, "UPnP: UDP port %u is open. Your address for friends: %s", port, pub);
    return true;
}

void worker() {
    std::unique_lock<std::mutex> lk(g_mu);
    uint16_t tried = 0;   // the port last asked for (0: none)
    auto next = std::chrono::steady_clock::time_point::max();
    while (!g_quit) {
        const uint16_t want = g_enabled ? g_port : 0;
        if (want != tried || std::chrono::steady_clock::now() >= next) {
            lk.unlock();
            if (g_mapped && g_mapped != want) remove_mapping();
            const bool open = want && map_port(want);   // also renews the lease of a mapping in place
            if (!want) set_status("Off");
            lk.lock();
            tried = want;
            next = !want ? std::chrono::steady_clock::time_point::max()
                         : std::chrono::steady_clock::now() + std::chrono::minutes(open ? 30 : 5);
        }
        g_cv.wait_for(lk, std::chrono::seconds(1));
    }
    lk.unlock();
    remove_mapping();
}

void ensure_thread() {
    if (g_started) return;
    g_started = true;
    psp_plat::net_init();
    std::thread(worker).detach();
    std::atexit([] { psp_upnp_shutdown(); });   // take the forward down when the game quits (else the one-hour lease runs out)
}

}  // namespace

void psp_upnp_set_enabled(bool on) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_enabled = on;
    if (!on) g_status = "Off";
    else if (!g_port) g_status = "Waiting for the network (open Communications mode)";
    ensure_thread();
    g_cv.notify_all();
}
bool psp_upnp_enabled() { std::lock_guard<std::mutex> lk(g_mu); return g_enabled; }
void psp_upnp_port(uint16_t port) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_port = port;
    if (g_enabled) ensure_thread();
    g_cv.notify_all();
}
std::string psp_upnp_status() { std::lock_guard<std::mutex> lk(g_mu); return g_status; }
std::string psp_upnp_public_address() { std::lock_guard<std::mutex> lk(g_mu); return g_public; }
void psp_upnp_shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_started) return;
        g_enabled = false;
    }
    remove_mapping();
}
