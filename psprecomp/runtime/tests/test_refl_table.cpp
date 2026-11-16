// Reflectivity table (issue #2): reflectivity.txt parsing, built-in list, file override and reload.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "psp_refl_table.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main() {
    {   // parsing
        std::unordered_map<uint64_t, PspReflMaterial> m;
        int bad = -1;
        const int n = psp_refl_parse(
            "# comment line\n"
            "30d3a0a1b2c3d4e5 1.0   # water\n"
            "0x00000000000000ff 0.25\r\n"
            "  eeff11e537eb4687\t0\n"
            "\n"
            "abc 2.5\n"            // clamped to 1
            "def -1\n"             // clamped to 0
            "aaa 0.4 0.3\n"        // roughness
            "bbb 0.4 0.5 1\n"      // metal
            "nothex 0.5\n"         // bad id
            "123 notanumber\n"     // bad value
            "ccc 0.9 0 0 1\n"      // water
            "123 0.5 0.2 1 1 9\n"  // five numbers
            "1234567890abcdef0 0.5\n"   // 17 digits
            "lonely\n",            // no value
            m, &bad);
        CHECK(n == 8);
        CHECK(bad == 5);
        CHECK(m.size() == 8);
        CHECK(m[0x30d3a0a1b2c3d4e5ull].refl == 1.0f && m[0x30d3a0a1b2c3d4e5ull].rough == 0.f && !m[0x30d3a0a1b2c3d4e5ull].metal);
        CHECK(m[0xffull].refl == 0.25f);
        CHECK(m.count(0xeeff11e537eb4687ull) && m[0xeeff11e537eb4687ull].refl == 0.0f);
        CHECK(m[0xabcull].refl == 1.0f);
        CHECK(m[0xdefull].refl == 0.0f);
        CHECK(m[0xaaaull].refl == 0.4f && m[0xaaaull].rough == 0.3f && !m[0xaaaull].metal);
        CHECK(m[0xbbbull].rough == 0.5f && m[0xbbbull].metal && !m[0xbbbull].water);
        CHECK(m[0xcccull].water && !m[0xcccull].metal);
    }
    {   // mask byte packing
        CHECK(psp_refl_encode(PspReflMaterial{0.f, 0.5f, true, true}) == 0);     // does not reflect: 0 whatever the rest
        CHECK(psp_refl_encode(PspReflMaterial{1.f, 0.f, false, false}) == 0xE0);
        CHECK(psp_refl_encode(PspReflMaterial{1.f, 1.f, true, true}) == 0xFF);
        CHECK(psp_refl_encode(PspReflMaterial{0.05f, 0.f, false, false}) == 0);   // below the first level
        const PspReflMaterial d = psp_refl_decode(psp_refl_encode(PspReflMaterial{0.43f, 0.3f, true, false}));
        CHECK(d.refl > 0.42f && d.refl < 0.44f && d.rough > 0.27f && d.rough < 0.3f && d.metal && !d.water);
        CHECK(psp_refl_decode(psp_refl_encode(PspReflMaterial{1.f, 0.f, false, true})).water);
    }

    char dir[] = "/tmp/refl_table_XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    const std::string path = std::string(dir) + "/reflectivity.txt";
    setenv("PSPRECOMP_REFLECTIVITY", path.c_str(), 1);

    // nothing yet: inactive, everything 0
    CHECK(!psp_refl_table_active());
    CHECK(psp_refl_lookup(0x1234).refl == 0.f);

    static const PspReflEntry builtin[] = {{0x1234, 0.5f, 0.2f, false, false}, {0x5678, 0.9f, 0.f, true, false}};
    const uint32_t g0 = psp_refl_generation();
    psp_refl_set_builtin(builtin, 2);
    CHECK(psp_refl_generation() != g0);
    CHECK(psp_refl_table_active());
    CHECK(psp_refl_lookup(0x1234).refl == 0.5f);
    CHECK(psp_refl_lookup(0x5678).refl == 0.9f && psp_refl_lookup(0x5678).metal);
    CHECK(psp_refl_lookup(0x1234).rough == 0.2f);
    CHECK(psp_refl_lookup(0x9999).refl == 0.f);

    // the file overrides a built-in entry (also to 0) and adds new ones
    {
        FILE* f = std::fopen(path.c_str(), "w");
        CHECK(f != nullptr);
        std::fputs("1234 0\n9999 0.7\n", f);
        std::fclose(f);
    }
    psp_refl_set_builtin(builtin, 2);   // forces a rebuild now (the lazy check runs every ~2 s)
    CHECK(psp_refl_lookup(0x1234).refl == 0.f);
    CHECK(psp_refl_lookup(0x5678).refl == 0.9f);
    CHECK(psp_refl_lookup(0x9999).refl == 0.7f);

    // an edit while running is picked up by the poll (mtime has 1 s resolution: wait it out)
    const uint32_t g1 = psp_refl_generation();
    sleep(2);
    {
        FILE* f = std::fopen(path.c_str(), "w");
        std::fputs("9999 0.1\n", f);
        std::fclose(f);
    }
    sleep(1);
    psp_refl_poll();
    CHECK(psp_refl_generation() != g1);
    CHECK(psp_refl_lookup(0x9999).refl == 0.1f);
    CHECK(psp_refl_lookup(0x1234).refl == 0.5f);   // back to the built-in value

    std::remove(path.c_str());
    rmdir(dir);
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("refl_table: all checks passed\n");
    return 0;
}
