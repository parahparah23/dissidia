#pragma once
#include <cstdint>
#include <cstddef>

/// PSP address space constants (matching emitter's recomp.h mask 0x07FFFFFFU).
/// The runtime allocates 128MB via mmap(MAP_ANON) for sparse page allocation.

/// Address mask applied by emitter's MEM_* accessors
static constexpr uint32_t PSP_ADDR_MASK       = 0x07FFFFFFu;

/// Total mmap allocation size (128MB — covers full masked address range)
static constexpr size_t   PSP_MEM_SIZE        = 0x08000000u;

/// PSP main RAM: 64MB at physical 0x08000000 (masked to 0x00000000)
/// PSP Slim (2000/3000) 64MB RAM
static constexpr uint32_t PSP_RAM_BASE        = 0x08000000u;
static constexpr uint32_t PSP_RAM_SIZE        = 0x04000000u;  // 64MB

/// PSP VRAM (EDRAM): 2MB at physical 0x04000000
static constexpr uint32_t PSP_VRAM_BASE       = 0x04000000u;
static constexpr uint32_t PSP_VRAM_SIZE       = 0x00200000u;  // 2MB

/// PSP Scratchpad: 16KB at physical 0x00010000
static constexpr uint32_t PSP_SCRATCHPAD_BASE = 0x00010000u;
static constexpr uint32_t PSP_SCRATCHPAD_SIZE = 0x00004000u;  // 16KB

/// High RAM (host-only extension): guest 0x0C800000-0x0FFFFFFF = offsets 0x04800000-0x07FFFFFF of the mapping,
/// above VRAM (offset 0x04000000) and its mirrors (to 0x04800000). Not PSP hardware: only blocks a game asks
/// for with psp_kmem_bump_alloc_hi() live here.
static constexpr uint32_t PSP_HI_RAM_BASE     = 0x0C800000u;
static constexpr uint32_t PSP_HI_RAM_END      = 0x10000000u;  // exclusive: 56MB

/// True when guest [a, a+n) lies entirely in main RAM (0x08000000-0x0BFFFFFF) or in high RAM.
/// Use this instead of hard-coded `a < 0x0C000000` checks wherever a pointer can come from a high-RAM block.
inline bool psp_guest_ok(uint32_t a, uint32_t n = 1) {
    if (a >= PSP_RAM_BASE && a < PSP_RAM_BASE + PSP_RAM_SIZE) return n <= PSP_RAM_BASE + PSP_RAM_SIZE - a;
    if (a >= PSP_HI_RAM_BASE && a < PSP_HI_RAM_END) return n <= PSP_HI_RAM_END - a;
    return false;
}
/// Same, for game-data pointers that must lie above the kernel area (0x08800000).
inline bool psp_guest_user_ok(uint32_t a, uint32_t n = 1) { return a >= 0x08800000u && psp_guest_ok(a, n); }

/// Allocate 128MB sparse memory region via mmap(MAP_ANON).
/// Returns nullptr on failure.
uint8_t* psp_memory_init();

/// Release the mmap allocation.
void psp_memory_cleanup(uint8_t* rdram);
