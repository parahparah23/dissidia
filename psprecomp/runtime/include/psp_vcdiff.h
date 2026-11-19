// VCDIFF (RFC 3284) / xdelta3 decoder: applies a patch window by window against a source read through a callback that
// may not know every byte. Unknown source bytes travel through the patch's COPYs, so the caller learns exactly which
// target bytes it could not rebuild. xdelta3's LZMA secondary compression (one .xz stream per section kind that runs on
// across windows, third_party/xz-embedded) and its per-window adler32 are supported; custom code tables and VCD_TARGET
// windows are not. The same algorithm as games/dissidia/tools/vcdiff.py `apply()` (the build tools' importer).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace psp_vcdiff {

struct Range { uint64_t lo, hi; };

// A sorted, merged set of [lo, hi) byte ranges.
class Ranges {
public:
    void add(uint64_t lo, uint64_t hi);
    std::vector<Range> overlaps(uint64_t lo, uint64_t hi) const;
    uint64_t total() const;
    bool empty() const { return r_.empty(); }
    const std::vector<Range>& all() const { return r_; }
private:
    std::vector<Range> r_;
};

struct Header {
    std::string app;          // the application header (xdelta3: "target name//source name/")
    size_t first_window = 0;  // offset of the first window
    int secondary = -1;       // secondary compressor id (2 = LZMA), -1 = none
};
bool read_header(const uint8_t* patch, size_t size, Header& h, std::string& err);

struct Window {
    uint32_t index = 0;
    uint64_t target_offset = 0;
    std::vector<uint8_t> data;
    std::vector<Range> unknown;   // window-relative target bytes that depend on unknown source bytes
    int checksum = -1;            // 1 = adler32 matches, 0 = it does not, -1 = the patch has none
};

// read(pos, n, out, unknown): fill out[0, n) with the source bytes at pos (zeros where unknown) and add the absolute
// source ranges that are unknown.
using ReadSource = std::function<void(uint64_t pos, uint64_t n, uint8_t* out, std::vector<Range>& unknown)>;
// on_window returns false to stop (apply then returns true with err empty).
bool apply(const uint8_t* patch, size_t size, const ReadSource& read, const std::function<bool(const Window&)>& on_window,
           std::string& err);

uint32_t adler32(const uint8_t* p, size_t n);

}  // namespace psp_vcdiff
