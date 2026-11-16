// psp_discord.h — Discord Rich Presence ("Playing ... / Quick Battle / Squall vs. Kefka") over Discord's local IPC.
//
// No Discord SDK: the desktop client listens on a local socket (Linux / macOS: discord-ipc-0..9 in $XDG_RUNTIME_DIR, $TMPDIR or
// /tmp, including the Flatpak, Snap and Vesktop sub-folders; Windows: the named pipe \\.\pipe\discord-ipc-0..9) and speaks
// length-prefixed JSON frames: {u32 opcode, u32 length, JSON}. Opcodes 0 handshake, 1 frame, 2 close, 3 ping, 4 pong.
//
// A background thread owns the connection. Once a second it asks the game module's provider what the player is doing and, when
// that changed, sends SET_ACTIVITY (at most one every 4 s: Discord drops more than 5 updates in 20 s; the newest one always goes
// out). Discord not running, closed or restarted is normal: the thread retries with a growing pause (5 s .. 60 s) and never
// blocks the game. Nothing is sent before the provider has something to show, and the activity is cleared on quit and when the
// player turns it off. Android has no Discord IPC: there everything here is a no-op.
//
// Settings: Network > Discord Status (saved as discord=0|1, exported as PSPRECOMP_DISCORD; default on).
// Environment: PSPRECOMP_DISCORD=0 off, PSPRECOMP_DISCORD_APP_ID=<id> another Discord application, PSPRECOMP_DISCORD_LOG=1 log
// every frame sent and received, PSPRECOMP_DISCORD_IPC=<path> one socket / pipe path instead of the search (tests).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace psp_discord {

struct Activity {
    std::string details;        // first line under the game name ("Quick Battle (1-on-1)")
    std::string state;          // second line ("Squall vs. Kefka")
    int64_t start = 0;          // unix seconds: "xx:xx elapsed" counts from here (0 = no timer)
    std::string large_image;    // asset key of the Discord application, or an https:// image URL
    std::string large_text;     // tooltip of the large image
    std::string small_image, small_text;
    std::string party_id;       // with party_size / party_max > 0: "(2 of 4)" after the state line
    int party_size = 0, party_max = 0;
    std::vector<std::pair<std::string, std::string>> buttons;   // up to 2 {label, https URL}, shown to other people
};

/// The game module's view of the player: fill `out` and return true, or return false for "nothing to show" (the activity is
/// cleared). Called on the presence thread about once a second, so it must only read state that is safe to read from another
/// thread (guest RAM words, atomics).
using Provider = bool (*)(Activity* out);

/// Start the presence thread for this Discord application id. Does nothing on Android, without an id, or when the player turned
/// it off (it can be turned on later with set_enabled).
void start(const std::string& app_id, Provider provider);
/// Clear the activity and close the connection (waits up to ~1 s). Safe to call when start() did nothing.
void stop();

/// Live on / off switch (the settings menu). Off clears the activity at once and closes the connection.
void set_enabled(bool on);
bool enabled();
/// False where there is no Discord IPC (Android) or start() was never called: the settings menu hides its rows.
bool available();
/// One line for the settings menu: "Off", "Discord is not running", "Connected (name)", ...
std::string status();

// ---- exposed for tests ----
/// SET_ACTIVITY payload for `a` (nullptr = clear) with the strings checked against Discord's limits.
std::string activity_json(const Activity* a, int64_t pid, const std::string& nonce);
/// Escape a UTF-8 string for a JSON string literal; invalid UTF-8 bytes become U+FFFD, the result is cut at `max_bytes` on a
/// character boundary (0 = no limit).
std::string json_string(const std::string& s, size_t max_bytes = 0);

}  // namespace psp_discord
