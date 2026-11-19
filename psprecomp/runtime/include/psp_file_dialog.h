// psp_file_dialog.h - the operating system's own file / folder picker, run without blocking the caller (desktop: portable-file-dialogs,
// the library the installer uses: setup/third_party).
//
//   Windows   the COM file dialogs (a folder picker, an archive picker, "save as .zip")
//   Linux     zenity, kdialog, matedialog or qarma when one is installed and a desktop is running (not in Steam Deck game mode,
//             whose compositor does not show other windows: there the in-game browser is used); PSPRECOMP_FILE_DIALOG=none
//             always uses the in-game browser
//   macOS     osascript
//   Android   the system document picker (Storage Access Framework) through org.psprecomp.game.FilePicker (android/game_java),
//             when the APK has it. A picked folder or file is a content:// document there, not a path: an import is copied into
//             the scratch folder first (the result is that copy), an export is written to the scratch folder and copied out by
//             commit()
//
// Usage: start(); then poll() every frame until it says Done / Cancelled / Failed. One dialog at a time.
#pragma once

#include <string>

namespace psp_filedlg {

enum class Kind {
    ImportFolder,   // pick a folder to read saves from
    ImportArchive,  // pick an archive file (.zip .7z .rar .tar.gz ...)
    ImportPatch,    // pick an ISO patch (.xdelta / .vcdiff): Settings > Mods
    ExportFolder,   // pick (or create) a folder to write saves into
    ExportZip,      // choose a .zip file name to write
};

enum class State { Idle, Running, Done, Cancelled, Failed };

// A native picker exists on this machine (else the caller offers its own browser).
bool available();
const char* backend();   // "Windows", "zenity", "kdialog", "macOS", "Android", "" when none

// Where Android copies picked documents / stages exports (a folder the game can write; created as needed).
void set_scratch_dir(const std::string& dir);

// Opens the picker. `start_dir`: the folder it shows first ("" = the system's choice); `default_name`: ExportZip's file
// name. False when a dialog is already open or none is available.
bool start(Kind kind, const std::string& title, const std::string& start_dir = "", const std::string& default_name = "");

// The picker's state. Done: `path` is the folder / file to use (see the Android note above). Done, Cancelled and Failed are
// reported once, then Idle. `error` gets Failed's reason.
State poll(std::string* path, std::string* error = nullptr);

// Stops waiting for an open picker (its answer is ignored; a Linux / macOS picker window is closed).
void abandon();

// After an export into the path an ExportFolder / ExportZip dialog gave: Android copies it to the picked document (desktop:
// nothing to do). "" on success, else an error text.
std::string commit();

}  // namespace psp_filedlg
