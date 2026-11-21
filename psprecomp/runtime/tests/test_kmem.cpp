// Unit tests for the host-heap (bump) allocator in hle/psp_hle_kernel_memory.cpp: the per-game
// region limits (RECOMP_HOST_HEAP_START / _END / _HEAP2_BASE / _HEAP2_END from recomp_game_config.h)
// and the first-fit region 1 -> region 2 policy.
//
// The translation unit under test is included directly (its heap state is file-static) with stubs
// for the two core functions it calls. Built twice by CMake with a different stand-in
// recomp_game_config.h on the include path: tests/kmem_cfg_default (no host-heap keys: the
// historical 0x0B000000 cap, no second region) and tests/kmem_cfg_two_region (Dissidia's values);
// KMEM_TEST_TWO_REGION selects the expectations.

#include "../src/hle/psp_hle_kernel_memory.cpp"

void psp_hle_register(const char*, HleFunc) {}
int psp_next_uid() { static int u = 1; return u++; }

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

int main() {
    constexpr uint32_t MiB = 0x100000u;
#ifdef KMEM_TEST_TWO_REGION
    constexpr uint32_t start = 0x09B80000u, end1 = 0x0B800000u, base2 = 0x0B810000u, end2 = 0x0BE00000u;
#else
    constexpr uint32_t start = ((RECOMP_HEAP_BASE + 0x00FFFFFFu) & ~0x00FFFFFFu), end1 = 0x0B000000u, base2 = 0, end2 = 0;
#endif
    CHECK(psp_kmem_host_top() == (end2 ? end2 : end1), "host top");
    CHECK(psp_kmem_free_total() == (end1 - start) + (end2 ? end2 - base2 : 0), "initial free");

    // first allocation is at the region start; the heap position is not realigned
    uint32_t a = psp_kmem_bump_alloc_named(0x10000, "a");
    CHECK(a == start, "first block at the heap start");
    CHECK(psp_kmem_bump_alloc(0) == 0, "zero-size alloc fails");

    // a block that fits what is left of region 1 stays in region 1
#ifdef KMEM_TEST_TWO_REGION
    constexpr uint32_t BIG = 16 * MiB;
#else
    constexpr uint32_t BIG = 8 * MiB;
#endif
    uint32_t big = psp_kmem_bump_alloc_named(BIG, "big");
    CHECK(big == start + 0x10000, "big block follows in region 1");

    // plan_fits mirrors the allocator without allocating
    const uint32_t free_before = psp_kmem_free_total();
    const uint32_t two[2] = { 8 * MiB, 8 * MiB };
    const bool fit2 = psp_kmem_plan_fits(two, 2);
    CHECK(psp_kmem_free_total() == free_before, "plan_fits does not allocate");
#ifdef KMEM_TEST_TWO_REGION
    // region 1 has 12.5 MiB - 64 KiB - 0 left after 'a' + 'big' (28.5 - 16 - 0.0625): 8 MiB fits there, the second 8 MiB
    // does not fit region 1 any more nor the 5.9 MiB region 2
    CHECK(!fit2, "two 8 MiB blocks do not fit");
    const uint32_t r2fit[2] = { 8 * MiB, 5 * MiB };
    CHECK(psp_kmem_plan_fits(r2fit, 2), "8 MiB in region 1 + 5 MiB in region 2 fit");

    // fill region 1, then the next block must come from region 2 (above the PRX at 0x0B800000)
    uint32_t fill = psp_kmem_bump_alloc_named(end1 - (start + 0x10000 + BIG), "fill");
    CHECK(fill == start + 0x10000 + BIG, "region 1 filled exactly");
    uint32_t r2 = psp_kmem_bump_alloc_named(4 * MiB, "r2");
    CHECK(r2 == base2, "overflow goes to region 2 base");
    CHECK(r2 >= 0x0B810000u && r2 + 4 * MiB <= end2, "region 2 block above the PRX, below the stack reserve");
    // a block larger than region 2's remainder fails even though region 1 + 2 have bytes in total
    CHECK(psp_kmem_bump_alloc_named(3 * MiB, "too big") == 0, "request larger than any free region fails");
    uint32_t small = psp_kmem_bump_alloc_named(0x1000, "small");
    CHECK(small == r2 + 4 * MiB, "small request fits the rest of region 2");
    CHECK(psp_kmem_free_total() == (end2 - base2) - 4 * MiB - 0x1000, "free total after region 2 use");
    // nothing is ever handed out inside [end1, base2) (the PRX) or above the host top
    CHECK(!(r2 < base2) && !(small + 0x1000 > end2), "no block touches the PRX gap");
#else
    CHECK(!fit2, "two 8 MiB blocks do not fit in a single 0x0B000000-capped region");
    uint32_t rem_start = 0, rem_end = 0;
    psp_kmem_reserve_remaining(&rem_start, &rem_end);
    CHECK(rem_start == big + BIG && rem_end == 0x0B000000u, "reserve_remaining returns the rest of region 1");
    CHECK(psp_kmem_free_total() == 0, "nothing left after reserve_remaining");
    CHECK(psp_kmem_bump_alloc(0x100) == 0, "OOM at 0x0B000000 without a second region");
#endif
    // high region (region 3): only psp_kmem_bump_alloc_hi hands it out, above VRAM and its mirrors, inside the mask
    {
        const uint32_t free_lo = psp_kmem_free_total();
        CHECK(psp_kmem_free_region(3) == PSP_HI_RAM_END - PSP_HI_RAM_BASE, "region 3 starts empty (56 MiB)");
        const uint32_t h1 = psp_kmem_bump_alloc_hi(0x1800100u, "hi pool");
        CHECK(h1 == PSP_HI_RAM_BASE, "first high block at 0x0C800000");
        CHECK(((h1 - PSP_RAM_BASE) & PSP_ADDR_MASK) >= 0x04800000u, "high block maps above VRAM and its mirrors");
        const uint32_t h2 = psp_kmem_bump_alloc_hi(0x100u, "hi small");
        CHECK(h2 == h1 + 0x1801000u, "high blocks are page-aligned and contiguous");
        CHECK(psp_kmem_free_total() == free_lo, "regions 1/2 untouched by high allocations");
        CHECK(psp_kmem_bump_alloc_hi(0x08000000u, "too big") == 0, "a request larger than region 3 fails");
        CHECK(psp_kmem_bump_alloc_hi(0, "zero") == 0, "zero-size high alloc fails");
        CHECK(psp_guest_ok(h1, 0x1800100u) && psp_guest_ok(0x0BFFFFFCu, 4) && !psp_guest_ok(0x0BFFFFFCu, 8),
              "psp_guest_ok: high block ok, RAM end exact");
        CHECK(!psp_guest_ok(0x0C000000u) && !psp_guest_ok(0x0C7FFFFFu) && psp_guest_ok(0x0FFFFFFFu) && !psp_guest_ok(0x10000000u),
              "psp_guest_ok: the VRAM alias gap and the mask end are rejected");
        CHECK(!psp_guest_user_ok(0x08400000u) && psp_guest_user_ok(0x08800000u), "psp_guest_user_ok: kernel area rejected");
    }
    psp_kmem_log_layout("test");

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("test_kmem: all passed\n");
    return 0;
}
