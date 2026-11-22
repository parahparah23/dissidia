#pragma once
#include <cstdint>
#include <string>
#include <vector>

/// SDL2+GL initialization: creates window, GL context, loads GLAD.
/// Must be called from main thread before any GL calls.
/// Returns 0 on success, non-zero on failure.
int psp_runtime_init_sdl();

/// Main event loop: processes SDL events, drains render queue, checks alive
/// threads. Blocks until g_should_exit is true or all game threads exit.
void psp_event_loop(uint8_t* rdram);

/// Install SIGTERM/SIGINT signal handlers that set g_should_exit.
void psp_install_signal_handlers();

/// Full shutdown: drain render queue, join threads, destroy SDL window/GL
/// context.
void psp_runtime_shutdown();

// Forward declare SDL_Window to avoid SDL.h in header
struct SDL_Window;

/// Get the SDL window pointer (for FBO blit and swap).
SDL_Window* psp_get_sdl_window();

// Window title (default "PSPrecomp Runtime"); may be called before or after the window exists.
void psp_set_window_title(const char* title);
// A game module gives its window an icon: w x h RGBA8 pixels (copied). Applied now if the window exists, else when it is created.
void psp_set_window_icon(const unsigned char* rgba, int w, int h);

/// Test Controls (Settings > Controls): one player's input as the game would read it. Main thread.
/// Player 0 = player 1: keyboard, the controllers "Player 1 Controller" allows, touch controls (and the debug socket's injections),
/// kept up to date even while the settings menu hides the input from the game. Player 1 = a placeholder for split screen: the second
/// connected controller. Buttons are PSP bits (SceCtrl: Select 1, Start 8, Up 0x10, Right 0x20, Down 0x40, Left 0x80, L 0x100,
/// R 0x200, Triangle 0x1000, Circle 0x2000, Cross 0x4000, Square 0x8000); lx / ly 0..255, 128 = centre.
struct PspInputSample {
    uint32_t buttons = 0;
    uint8_t lx = 128, ly = 128;
    bool present = false;       // false: no device for this player
    std::string name;           // what feeds this player
    // The physical controller behind the player (SDL game controller), raw, before the game's mapping. Keyboard-only / touch: pad = false.
    bool pad = false;
    int16_t rx = 0, ry = 0;     // right stick, -32768..32767
    int16_t lraw_x = 0, lraw_y = 0;   // left stick raw
    int16_t lt = 0, rt = 0;     // analogue triggers 0..32767
    uint32_t pad_buttons = 0;   // bit i = SDL_CONTROLLER_BUTTON i (SDL_GameControllerButton) is down
    std::string pad_l2_map, pad_r2_map;       // what each trigger does in the game: PSP button names ("L", "R + Cross"), "" = nothing
    std::string pad_rstick_map;               // what the right stick does ("" = nothing)
    std::vector<std::string> pad_map;         // [SDL_GameControllerButton] what that button does in the game ("" = nothing)
};
int psp_input_sample_players();
bool psp_input_sample(int player, PspInputSample* out);
