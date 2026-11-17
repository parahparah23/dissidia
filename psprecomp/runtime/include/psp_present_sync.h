// Vertical sync, variable refresh rate and display refresh hints (issue #67).
//
// The game paces itself (the simulated vblank in psp_hle_display.cpp); this decides how a finished frame reaches the
// screen. Off: present at once (lowest latency, may tear outside a compositor). On: wait for the display (Vulkan FIFO,
// GL swap interval -1 / 1); on a FreeSync / G-Sync / HDMI VRR display that is what lets the refresh follow the game.
//
// PSPRECOMP_VSYNC=0|1 (unset = off on desktops; Android sets 1), saved as `vsync` by the settings menu.
// PSPRECOMP_PRESENT_STATS=1 logs present-to-present timing every 5 s ([PRESENT_STATS]).
#pragma once
#include <SDL.h>

/// 1 when vertical sync is wanted (the setting / PSPRECOMP_VSYNC), 0 when not.
int psp_vsync_wanted();
/// Change it while running (settings menu): applied by the presenting thread at the next swap.
void psp_vsync_set(int on);
/// Apply the current choice to a GL context (call on the GL thread; the Vulkan backend reads psp_vsync_wanted()).
void psp_vsync_apply_gl();

/// The frame rate the game is set to (battles; menus run at 30). Android asks the display for a matching refresh rate.
void psp_present_set_target_fps(int fps);

/// Called by psp_gl_swap on the presenting thread, right before / after the swap.
void psp_present_before_swap(SDL_Window* w);
void psp_present_after_swap();
