// Save archives: see psp_savearchive.h.
#include "psp_savearchive.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__ANDROID__)
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace psp_savearchive {
namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string S(const fs::path& p) { return p.u8string(); }

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(uint8_t(c))); return s; }
bool ends(const std::string& s, const char* e) { const size_t n = std::strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }

bool read_all(const fs::path& p, Bytes* out, uint64_t limit) {
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec || sz > limit) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->resize(size_t(sz));
    if (sz) f.read(reinterpret_cast<char*>(out->data()), std::streamsize(sz));
    return bool(f);
}

uint16_t g16(const Bytes& d, size_t o) { return o + 2 <= d.size() ? uint16_t(d[o] | d[o + 1] << 8) : 0; }
uint32_t g32(const Bytes& d, size_t o) { return o + 4 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24) : 0; }

// An archive entry's path made safe: "/" separators, no empty / "." parts; false for anything that would leave the folder.
bool safe_rel(std::string name, std::string* out) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name[0] == '/' || (name.size() > 1 && name[1] == ':')) return false;
    std::string r;
    size_t a = 0;
    while (a <= name.size()) {
        size_t b = name.find('/', a);
        if (b == std::string::npos) b = name.size();
        const std::string part = name.substr(a, b - a);
        a = b + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return false;
        for (char c : part) if (uint8_t(c) < 0x20) return false;
        r += (r.empty() ? "" : "/") + part;
    }
    if (r.empty()) return false;
    *out = r;
    return true;
}

struct Writer {
    fs::path dest;
    uint64_t total = 0;
    int entries = 0;
    std::string file(const std::string& name, const uint8_t* data, size_t n) {
        std::string rel;
        if (!safe_rel(name, &rel)) return "the archive has an unsafe file name (" + name + ")";
        if (++entries > kMaxEntries) return "the archive has too many files";
        total += n;
        if (total > kMaxTotal) return "the archive is too big for saves";
        const fs::path p = dest / P(rel);
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (n) f.write(reinterpret_cast<const char*>(data), std::streamsize(n));
        return f ? "" : "cannot write " + rel;
    }
    void dir(const std::string& name) {
        std::string rel;
        if (!safe_rel(name, &rel)) return;
        std::error_code ec;
        fs::create_directories(dest / P(rel), ec);
    }
};

// ---- zip ----
std::string inflate_raw(const uint8_t* src, size_t n, size_t expect, Bytes* out, int window_bits) {
    z_stream z{};
    if (inflateInit2(&z, window_bits) != Z_OK) return "zlib error";
    out->clear();
    out->resize(expect ? expect : std::max<size_t>(n * 4, 4096));
    z.next_in = const_cast<Bytef*>(src);
    z.avail_in = uInt(n);
    size_t have = 0;
    int r = Z_OK;
    for (;;) {
        if (have == out->size()) {
            if (out->size() >= kMaxTotal) { inflateEnd(&z); return "the archive is too big for saves"; }
            out->resize(std::min<size_t>(out->size() * 2, size_t(kMaxTotal)));
        }
        z.next_out = out->data() + have;
        z.avail_out = uInt(out->size() - have);
        r = inflate(&z, Z_NO_FLUSH);
        have = out->size() - z.avail_out;
        if (r == Z_STREAM_END) break;
        if (r != Z_OK && r != Z_BUF_ERROR) break;
        if (r == Z_BUF_ERROR && z.avail_in == 0) break;
    }
    inflateEnd(&z);
    out->resize(have);
    if (r != Z_STREAM_END) return "the archive is damaged (a file does not unpack)";
    return "";
}

std::string extract_zip(const Bytes& d, Writer& w) {
    if (d.size() < 22) return "not a zip file";
    size_t eocd = std::string::npos;
    const size_t lo = d.size() > 22 + 65535 ? d.size() - 22 - 65535 : 0;
    for (size_t i = d.size() - 22 + 1; i-- > lo;)
        if (g32(d, i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) return "the zip file is damaged (no directory)";
    const uint32_t count = g16(d, eocd + 10), cd_size = g32(d, eocd + 12), cd_off = g32(d, eocd + 16);
    if (count == 0xFFFF || cd_off == 0xFFFFFFFFu) return "zip64 archives are not supported (unpack it first)";
    if (size_t(cd_off) + cd_size > d.size()) return "the zip file is damaged (directory out of range)";
    size_t p = cd_off;
    for (uint32_t i = 0; i < count; ++i) {
        if (g32(d, p) != 0x02014b50) return "the zip file is damaged (directory entry)";
        const uint16_t flags = g16(d, p + 8), method = g16(d, p + 10);
        const uint32_t crc = g32(d, p + 16), csize = g32(d, p + 20), usize = g32(d, p + 24);
        const uint16_t nlen = g16(d, p + 28), elen = g16(d, p + 30), clen = g16(d, p + 32);
        const uint32_t loff = g32(d, p + 42);
        if (p + 46 + nlen > d.size()) return "the zip file is damaged (name)";
        const std::string name(reinterpret_cast<const char*>(&d[p + 46]), nlen);
        p += 46 + size_t(nlen) + elen + clen;
        if (!name.empty() && (name.back() == '/' || name.back() == '\\')) { w.dir(name); continue; }
        if (name.rfind("__MACOSX/", 0) == 0) continue;   // macOS resource forks
        if (flags & 1) return "the zip file is password-protected";
        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || loff == 0xFFFFFFFFu) return "zip64 archives are not supported (unpack it first)";
        if (g32(d, loff) != 0x04034b50) return "the zip file is damaged (local header)";
        const size_t data = size_t(loff) + 30 + g16(d, loff + 26) + g16(d, loff + 28);
        if (data + csize > d.size()) return "the zip file is damaged (data out of range)";
        Bytes buf;
        const uint8_t* src = &d[data];
        size_t n = csize;
        if (method == 8) {
            if (usize > kMaxTotal) return "the archive is too big for saves";
            if (std::string e = inflate_raw(src, csize, usize, &buf, -15); !e.empty()) return e;
            src = buf.data();
            n = buf.size();
        } else if (method != 0) {
            return "the zip file uses a compression this game cannot unpack (method " + std::to_string(method) + "); unpack it first";
        }
        if (n != usize || uint32_t(crc32(0, src, uInt(n))) != crc) return "the zip file is damaged (" + name + " fails its check)";
        if (std::string e = w.file(name, src, n); !e.empty()) return e;
    }
    return "";
}

// ---- tar ----
uint64_t octal(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && p[i]; ++i) {
        if (p[i] == ' ') continue;
        if (p[i] < '0' || p[i] > '7') break;
        v = v * 8 + (p[i] - '0');
    }
    return v;
}
std::string cstr(const uint8_t* p, size_t n) { size_t i = 0; while (i < n && p[i]) ++i; return std::string(reinterpret_cast<const char*>(p), i); }

std::string extract_tar(const Bytes& d, Writer& w) {
    size_t p = 0;
    std::string long_name;
    while (p + 512 <= d.size()) {
        const uint8_t* h = &d[p];
        bool zero = true;
        for (int i = 0; i < 512 && zero; ++i) zero = h[i] == 0;
        if (zero) break;
        const uint64_t size = octal(h + 124, 12);
        const char type = char(h[156]);
        std::string name = cstr(h, 100);
        if (std::memcmp(h + 257, "ustar", 5) == 0) { const std::string pre = cstr(h + 345, 155); if (!pre.empty()) name = pre + "/" + name; }
        p += 512;
        if (size > d.size() - p) return "the tar file is damaged (truncated)";
        const uint8_t* body = &d[p];
        p += size_t((size + 511) / 512 * 512);
        if (type == 'L') { long_name = cstr(body, size_t(size)); continue; }               // GNU long name for the next entry
        if (type == 'x') {                                                                  // pax: "<len> path=<name>\n"
            const std::string px(reinterpret_cast<const char*>(body), size_t(size));
            const size_t k = px.find(" path=");
            if (k != std::string::npos) long_name = px.substr(k + 6, px.find('\n', k) - (k + 6));
            continue;
        }
        if (type == 'g') continue;
        if (!long_name.empty()) { name = long_name; long_name.clear(); }
        if (type == '5') { w.dir(name); continue; }
        if (type != '0' && type != '\0' && type != '7') continue;                           // links, devices: skipped
        if (std::string e = w.file(name, body, size_t(size)); !e.empty()) return e;
    }
    return "";
}

// ---- other formats: a program on this machine ----
#if defined(__ANDROID__)
bool run_tool(const std::vector<std::string>&) { return false; }
#elif defined(_WIN32)
std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
bool run_tool(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (const std::string& a : argv) cmd += (cmd.empty() ? L"\"" : L" \"") + wide(a) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}
#else
bool run_tool(const std::vector<std::string>& argv) {
    std::vector<char*> av;
    for (const std::string& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int r = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r != 0) return false;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}
#endif

std::string extract_with_tool(const std::string& archive, const std::string& dest, Kind k) {
    std::error_code ec;
    fs::create_directories(P(dest), ec);
    std::vector<std::vector<std::string>> tries;
#ifdef _WIN32
    for (const char* sz : {"7z", "C:\\Program Files\\7-Zip\\7z.exe"}) tries.push_back({sz, "x", "-y", "-o" + dest, archive});
    tries.push_back({"tar", "-xf", archive, "-C", dest});   // Windows' bsdtar
#else
    for (const char* sz : {"7z", "7za", "7zz"}) tries.push_back({sz, "x", "-y", "-o" + dest, archive});
    tries.push_back({"bsdtar", "-xf", archive, "-C", dest});
    if (k == Kind::Rar) tries.push_back({"unrar", "x", "-o+", archive, dest + "/"});
    else tries.push_back({"tar", "-xf", archive, "-C", dest});
#endif
    for (const auto& t : tries) {
        if (!run_tool(t)) continue;
        // A tool may write anything: check that nothing landed outside (a symlink) and the size is sane.
        uint64_t total = 0;
        int n = 0;
        for (auto it = fs::recursive_directory_iterator(P(dest), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (it->is_symlink(e2)) { fs::remove(it->path(), e2); continue; }
            if (it->is_regular_file(e2)) total += it->file_size(e2);
            if (++n > kMaxEntries || total > kMaxTotal) return "the archive is too big for saves";
        }
        std::fprintf(stderr, "[SAVEMGR] unpacked %s with %s\n", archive.c_str(), t[0].c_str());
        return "";
    }
#ifdef __ANDROID__
    return std::string("this game cannot unpack .") + kind_name(k) + " files on Android: use a .zip, or unpack it with a file manager first";
#else
    return std::string("cannot unpack .") + kind_name(k) + " files: install 7-Zip, or unpack it first and choose the folder";
#endif
}

void put16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }

}  // namespace

const char* kind_name(Kind k) {
    switch (k) {
    case Kind::Zip: return "zip";
    case Kind::Tar: return "tar";
    case Kind::TarGz: return "tar.gz";
    case Kind::SevenZip: return "7z";
    case Kind::Rar: return "rar";
    case Kind::Xz: return "xz";
    case Kind::Bz2: return "bz2";
    case Kind::Other: return "archive";
    default: return "";
    }
}

bool looks_like_archive(const std::string& file_name) {
    const std::string n = lower(file_name);
    for (const char* e : {".zip", ".7z", ".rar", ".tar", ".tgz", ".tar.gz", ".gz", ".tar.xz", ".txz", ".xz", ".tar.bz2", ".tbz2", ".bz2"})
        if (ends(n, e)) return true;
    return false;
}

Kind detect(const std::string& path) {
    uint8_t h[512] = {};
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return Kind::None;
    f.read(reinterpret_cast<char*>(h), sizeof h);
    const std::streamsize got = f.gcount();
    if (got >= 4 && h[0] == 'P' && h[1] == 'K' && (h[2] == 3 || h[2] == 5)) return Kind::Zip;
    if (got >= 6 && std::memcmp(h, "7z\xBC\xAF\x27\x1C", 6) == 0) return Kind::SevenZip;
    if (got >= 4 && std::memcmp(h, "Rar!", 4) == 0) return Kind::Rar;
    if (got >= 2 && h[0] == 0x1F && h[1] == 0x8B) return Kind::TarGz;
    if (got >= 6 && std::memcmp(h, "\xFD" "7zXZ", 5) == 0) return Kind::Xz;
    if (got >= 3 && std::memcmp(h, "BZh", 3) == 0) return Kind::Bz2;
    if (got >= 262 && std::memcmp(h + 257, "ustar", 5) == 0) return Kind::Tar;
    if (looks_like_archive(S(P(path).filename()))) return Kind::Other;
    return Kind::None;
}

std::string extract(const std::string& archive, const std::string& dest) {
    const Kind k = detect(archive);
    if (k == Kind::None) return "this file is not an archive this game knows (.zip, .7z, .rar, .tar.gz)";
    std::error_code ec;
    fs::create_directories(P(dest), ec);
    if (ec) return "cannot create " + dest + ": " + ec.message();
    Writer w;
    w.dest = P(dest);
    if (k == Kind::Zip || k == Kind::Tar || k == Kind::TarGz) {
        Bytes d;
        if (!read_all(P(archive), &d, kMaxTotal)) return "cannot read the archive (missing, unreadable or too big)";
        if (k == Kind::Zip) return extract_zip(d, w);
        if (k == Kind::Tar) return extract_tar(d, w);
        Bytes t;
        if (std::string e = inflate_raw(d.data(), d.size(), 0, &t, 16 + 15); !e.empty()) return e;
        const std::string r = extract_tar(t, w);
        if (r.empty() && w.entries == 0) return "the .gz file does not hold a tar archive";
        return r;
    }
    return extract_with_tool(archive, dest, k);
}

std::string write_zip(const std::string& zip_path, const std::vector<std::string>& dirs, const std::string& prefix_in) {
    std::string prefix = prefix_in;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    Bytes out, cd;
    uint16_t count = 0;
    // DOS date/time of now (the entries' time stamp)
    const time_t now = std::time(nullptr);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    const uint16_t dtime = uint16_t(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
    const uint16_t ddate = uint16_t((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
    for (const std::string& dir : dirs) {
        const std::string folder = S(P(dir).filename());
        std::vector<fs::path> files;
        std::error_code ec;
        for (auto it = fs::directory_iterator(P(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2)) files.push_back(it->path());
        }
        if (ec) return "cannot read " + dir + ": " + ec.message();
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            Bytes raw;
            if (!read_all(f, &raw, kMaxTotal)) return "cannot read " + S(f);
            const std::string name = (prefix.empty() ? "" : prefix + "/") + folder + "/" + S(f.filename());
            const uint32_t crc = uint32_t(crc32(0, raw.data(), uInt(raw.size())));
            // deflate; keep it stored when that is no smaller (encrypted save data does not shrink)
            Bytes comp(compressBound(uLong(raw.size())) + 64);
            z_stream z{};
            uint16_t method = 0;
            const uint8_t* body = raw.data();
            size_t blen = raw.size();
            if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
                z.next_in = raw.data(); z.avail_in = uInt(raw.size());
                z.next_out = comp.data(); z.avail_out = uInt(comp.size());
                if (deflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out < raw.size()) { method = 8; body = comp.data(); blen = z.total_out; }
                deflateEnd(&z);
            }
            if (out.size() > 0xFFFFFFF0u - blen) return "the zip would be too big";
            const uint32_t off = uint32_t(out.size());
            put32(out, 0x04034b50); put16(out, 20); put16(out, 0x0800); put16(out, method); put16(out, dtime); put16(out, ddate);
            put32(out, crc); put32(out, uint32_t(blen)); put32(out, uint32_t(raw.size())); put16(out, uint16_t(name.size())); put16(out, 0);
            out.insert(out.end(), name.begin(), name.end());
            out.insert(out.end(), body, body + blen);
            put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0x0800); put16(cd, method); put16(cd, dtime); put16(cd, ddate);
            put32(cd, crc); put32(cd, uint32_t(blen)); put32(cd, uint32_t(raw.size())); put16(cd, uint16_t(name.size()));
            put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, off);
            cd.insert(cd.end(), name.begin(), name.end());
            if (++count == 0xFFFF) return "too many files for a zip";
        }
    }
    const uint32_t cd_off = uint32_t(out.size());
    out.insert(out.end(), cd.begin(), cd.end());
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, count); put16(out, count);
    put32(out, uint32_t(cd.size())); put32(out, cd_off); put16(out, 0);
    const fs::path dst = P(zip_path), tmp = P(zip_path + ".tmp");
    std::error_code ec;
    if (dst.has_parent_path()) fs::create_directories(dst.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        if (!f) { fs::remove(tmp, ec); return "cannot write " + zip_path; }
    }
    fs::rename(tmp, dst, ec);
    if (ec) { fs::remove(dst, ec); ec.clear(); fs::rename(tmp, dst, ec); }
    return ec ? "cannot write " + zip_path + ": " + ec.message() : "";
}

}  // namespace psp_savearchive
