// Settings > Saves: the save manager, drawn by the settings screen (SettingsView) like every other category, so it works with
// the pad, the keyboard, the mouse and touch. Also opened straight from the game's save list (Triangle).
//
//   Needs your choice   saves changed on two devices (cloud sync): one screen per save shows both sides (when, which device, the
//                       save's own description) and offers keep this device's / use the cloud's / later, or "keep the newer of each"
//   Import              found automatically (PPSSPP, a PSP memory stick), a folder or an archive (.zip .7z .rar .tar.gz) picked with the
//                       system's file dialog, the in-game file browser, or a typed path. Saves of every region are found anywhere below
//                       the picked place and get this game's folder names (psp_savemgr::copy_save signs PARAM.SFO again)
//   Export              to a folder or a .zip picked with the system's dialog, PPSSPP / a memory stick, the in-game browser or a typed
//                       path; folder names keep their region or take this game's
//   This game's saves   duplicate a save into another slot (the game shows game data slots 00-19), move it to another slot, rename it
//                       (the name the save list shows), delete it (into the backups; with cloud sync, on the other devices too)
//   Backups             every replaced save is kept (psp_savebackup), plus a daily copy and "Back up now"; any save of any backup can be
//                       put back (e.g. to undo a cloud download), which itself is backed up first and is uploaded at the next sync
//   Cloud sync          status, Sync now, a jump to Settings > Cloud
//
// Pure logic (no SDL): the settings menu feeds it confirm / change / back and shows its rows, dialogs and text questions; the file
// dialog, the cloud service and the source detection come in through Env so tests can fake them (tests/test_savemgr.cpp).
#pragma once

#include "psp_cloud_service.h"
#include "psp_file_dialog.h"
#include "psp_savebackup.h"
#include "psp_savedata_dialog.h"
#include "psp_savemgr.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psp_saves {

struct Env {
    std::string saves_root;      // ms0/PSP/SAVEDATA
    std::string backup_root;     // ms0/PSP/SAVEDATA_BACKUP
    std::string scratch_dir;     // a folder for unpacked archives and staged exports (emptied as needed)
    std::string disc_id;         // the running disc's ID: imported saves get it (unless the game already uses another region's folder)
    std::string game_dir;        // the game's folder (a place in the browser)
    // The folder name already holding `suffix` under any shared region's ID ("" = none). Null: any-region folder in saves_root.
    std::function<std::string(const std::string& suffix)> existing;
    // Other installs: PPSSPP, memory sticks (null = psp_savemgr::detect_sources).
    std::function<std::vector<psp_savemgr::Source>(bool with_saves)> detect;
    // The system's file dialog (null = psp_filedlg).
    std::function<bool()> dlg_available;
    std::function<bool(psp_filedlg::Kind, const std::string& title, const std::string& start, const std::string& name)> dlg_start;
    std::function<psp_filedlg::State(std::string* path, std::string* err)> dlg_poll;
    std::function<void()> dlg_abandon;
    std::function<std::string()> dlg_commit;
    // Cloud sync (null cloud_status = not running in this session).
    std::function<psp_cloud_svc::Status()> cloud_status;
    std::function<bool()> cloud_configured;
    std::function<void()> cloud_sync;
    std::function<void(const std::string& suffix, psp_cloud::Choice)> cloud_resolve;
    std::function<void(const std::string& suffix)> cloud_delete;   // a slot deleted / moved away here: delete it in the cloud too
    std::string device_name;     // this device, as the conflict screen names it
    // Places for the in-game browser (null = home, Downloads, Documents, the game folder, mounts / drives / Android storage).
    std::function<std::vector<psp_savemgr::Source>()> places;
};

struct Dialog {                  // a question / message over the page; the last button is what Back chooses
    std::string title, text;
    std::vector<std::string> buttons;
};
struct TextAsk {                 // a text field to type (the settings menu's keyboard)
    std::string title, text;
};

class Page {
public:
    Page();
    ~Page();
    void set_env(const Env& e);
    const Env& env() const;
    // The category was opened: back to the top level, lists looked up again.
    void reset();

    // ---- what the settings screen shows ----
    const std::vector<psp_sddialog::SetRow>& rows();   // the current level (rebuilt when stale)
    std::vector<std::string> crumbs() const;           // the levels below "Saves"
    bool in_sub() const;
    int sel() const;
    void set_sel(int i);

    // ---- input ----
    void confirm(int row);
    void change(int row, int d);
    bool back();                                       // up one level (true), or false at the top
    bool dialog(Dialog* out) const;                    // a dialog is showing
    void dialog_choose(int button);
    bool text(TextAsk* out) const;                     // a text question is open
    void text_done(bool ok, const std::string& value);

    // Every frame: polls the file dialog and the cloud status. True when the rows changed (redraw).
    bool tick(int64_t now_ms);
    bool native_dialog_open() const;                   // the system's file dialog is up (the settings menu steps the window aside)
    bool take_cloud_settings_request();                // the user asked for Settings > Cloud (once)
    unsigned saves_changed() const;                    // bumped whenever this game's saves were changed here

    // Tests: waits (up to timeout_ms) for the background search of PPSSPP / memory sticks that reset() starts.
    bool wait_idle(int timeout_ms = 5000);
    // Tests: jump to a conflict's page as the "Needs your choice" row does.
    void open_conflict(const std::string& suffix);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Labels shared with the save list: "Game data", "Friend card 03", ... and "US" / "EU" / "JP".
std::string slot_label(const std::string& suffix);
std::string when_text(time_t t);        // "19 Nov 2026 21:04"

}  // namespace psp_saves
