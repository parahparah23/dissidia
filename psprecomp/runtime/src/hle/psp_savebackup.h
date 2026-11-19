// Automatic save backups: whenever a save is about to be replaced (a cloud download, the cloud side of a conflict, an import,
// a restore) the old folder goes into a dated backup first, and once a day the whole save set is copied while it has changed.
// The user can browse them and put any save back (Settings > Saves > Backups), e.g. to roll back a cloud sync.
//
// Layout, in the backup folder (ms0/PSP/SAVEDATA_BACKUP, never inside SAVEDATA, which the game lists):
//   20261119-210405_cloud-download/          one backup: <local date-time>_<reason code>
//       backup.txt                           "reason=...", "time=<unix>"
//       ULES01505GameData00/ ...             the save folders as they were (untouched copies)
//   ULES01505GameData00.bak, ...bak1         older runtimes' single-save backups: listed too (reason "older backup")
//
// Several saves replaced by the same occasion within two minutes (one cloud sync that downloads three saves) share one backup.
// Automatic backups are pruned (the newest kKeepAuto, the newest kKeepDaily daily ones); those made with "Back up now" stay
// until deleted. Pure library (std::filesystem), thread-safe (the cloud worker and the menu both call it).
#pragma once

#include "psp_savemgr.h"

#include <ctime>
#include <string>
#include <vector>

namespace psp_savebackup {

// Reason codes (folder name suffix and backup.txt).
inline constexpr const char* kCloudDownload = "cloud-download";   // a newer save came from the cloud
inline constexpr const char* kCloudConflict = "cloud-conflict";   // "use the cloud save" in a conflict
inline constexpr const char* kImport = "import";                  // replaced by an import
inline constexpr const char* kRestore = "restore";                // replaced by restoring a backup
inline constexpr const char* kReplaced = "replaced";              // any other replacement
inline constexpr const char* kManual = "manual";                  // "Back up now"
inline constexpr const char* kDeleted = "deleted";                // deleted with the save manager, or on another device (cloud sync)
inline constexpr const char* kMoved = "moved";                    // moved to another slot
inline constexpr const char* kRenamed = "renamed";                // before its name was changed
inline constexpr const char* kDaily = "daily";                    // the first start of a day (when the saves changed)
inline constexpr const char* kLegacy = "legacy";                  // a <name>.bak folder of an older runtime

constexpr int kKeepAuto = 40;    // automatic backups kept (daily ones not counted)
constexpr int kKeepDaily = 14;

struct Backup {
    std::string id;          // the folder name in the backup folder
    std::string path;
    std::string reason;      // a code above
    time_t when = 0;
    std::vector<psp_savemgr::SaveInfo> saves;   // sorted by folder name
    uint64_t bytes = 0;
};

// "Before a cloud download", ... (for menus).
const char* reason_text(const std::string& code);
bool is_automatic(const std::string& code);

// Moves the save folder `save_dir` into a backup (reused when the same reason made one in the last two minutes and it does
// not hold that folder yet). "" on success.
std::string set_aside(const std::string& save_dir, const std::string& backup_root, const std::string& reason);

// Copies saves of `saves_root` (`names`: folder names, empty = every Dissidia 012 save there) into a new backup. `id` gets its
// folder name. Nothing to copy is not an error (id stays empty).
std::string snapshot(const std::string& saves_root, const std::string& backup_root, const std::string& reason,
                     const std::vector<std::string>& names = {}, std::string* id = nullptr);

// The once-a-day backup: a snapshot of every save, made when today has none yet and the saves differ from the newest backup
// that holds all of them. Returns true when one was made.
bool daily(const std::string& saves_root, const std::string& backup_root);

// Every backup, newest first.
std::vector<Backup> list(const std::string& backup_root);

// Deletes one backup (its folder). "" on success.
std::string remove(const std::string& backup_root, const std::string& id);

// Deletes the oldest automatic backups beyond the limits.
void prune(const std::string& backup_root, int keep_auto = kKeepAuto, int keep_daily = kKeepDaily);

// A fingerprint of a save folder's files (names, sizes, contents): equal fingerprints = the same save.
std::string fingerprint(const std::string& save_dir);

}  // namespace psp_savebackup
