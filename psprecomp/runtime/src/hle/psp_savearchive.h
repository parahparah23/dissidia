// Save archives: unpack a compressed save someone shared (or a backup they made) so its saves can be imported, and write
// exported saves as one .zip. Built in: .zip (stored / deflate), .tar, .tar.gz / .tgz (zlib). Other formats (.7z, .rar,
// .tar.xz ...) are handed to a program on the machine when one exists (7z / 7za / 7zz, bsdtar or tar - Windows 10+ ships a
// tar that reads .zip / .7z / .rar -, unrar); without one the error says to unpack it first.
// Pure library (std::filesystem + zlib), host-testable (tests/test_savemgr.cpp).
#pragma once

#include <string>
#include <vector>

namespace psp_savearchive {

enum class Kind { None, Zip, Tar, TarGz, SevenZip, Rar, Xz, Bz2, Other };

// What `path` is, by its first bytes (falls back to the name's ending). None = not an archive (or unreadable).
Kind detect(const std::string& path);
const char* kind_name(Kind k);              // "zip", "7z", ...
bool looks_like_archive(const std::string& file_name);   // by the name: .zip .7z .rar .tar .tgz .tar.gz .tar.xz ...

// Unpacks `archive` into the (new or empty) folder `dest`. Entries that would land outside `dest` (absolute paths, "..")
// are refused, as are archives over the limits (save archives are small). "" on success, else an error text.
std::string extract(const std::string& archive, const std::string& dest);
constexpr unsigned long long kMaxTotal = 512ull << 20;   // bytes unpacked
constexpr int kMaxEntries = 20000;

// Writes a .zip of the folders `dirs` (each stored under its own folder name, files only, one level). "" on success.
// `prefix`: a folder path inside the zip the folders go under (e.g. "PSP/SAVEDATA"), "" = at the top.
std::string write_zip(const std::string& zip_path, const std::vector<std::string>& dirs, const std::string& prefix = "");

}  // namespace psp_savearchive
