// Unit tests for the wizard's logic that needs no window: progress parsing, ETA, update lines, plan weights, features,
// the disc checks of the picked Dissidia 1 / undub images (SETUP_TEST_DISCS adds real images).
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "../install.h"
#include "../third_party/miniz.h"

using namespace setup;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// ---- tiny PSP disc images for the disc checks (ISO 9660 with PSP_GAME/PARAM.SFO, optionally a package index) ----
static void put32(std::string& s, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) s[o + i] = (char)(v >> (8 * i)); }
static std::string dirrec(const std::string& name, uint32_t lba, uint32_t size, bool dir) {
    std::string r(33 + name.size() + ((33 + name.size()) & 1), '\0');
    r[0] = (char)r.size(); put32(r, 2, lba); put32(r, 10, size); r[25] = dir ? 2 : 0; r[32] = (char)name.size();
    r.replace(33, name.size(), name);
    return r;
}
static std::string sfo(const std::string& id, const std::string& title) {
    const std::string keys = std::string("DISC_ID\0TITLE\0", 14);
    std::string data(16 + 128, '\0');
    data.replace(0, id.size(), id); data.replace(16, title.size(), title);
    std::string s(20 + 32, '\0');
    s.replace(0, 4, std::string("\0PSF", 4)); put32(s, 4, 0x101); put32(s, 8, 20 + 32); put32(s, 12, 20 + 32 + 16); put32(s, 16, 2);
    // entries: {key off, fmt 0x0204 (utf8)}, len, max, data off
    put32(s, 20, 0 | 0x0204u << 16); put32(s, 24, (uint32_t)id.size() + 1); put32(s, 28, 16); put32(s, 32, 0);
    put32(s, 36, 8 | 0x0204u << 16); put32(s, 40, (uint32_t)title.size() + 1); put32(s, 44, 128); put32(s, 48, 16);
    return s + keys + std::string(2, '\0') + data;
}
// sectors: 16 PVD, 18 root, 19 PSP_GAME, 20 PARAM.SFO, 21 USRDIR, 22 DATA, 23 PACKAGE_INFO.BIN, 24.. PACKAGE.BIN
static std::string make_iso(const std::string& id, const std::string& title, const std::string& pkg_info = "", const std::string& pkg = "") {
    const size_t S = 2048, sectors = 24 + (pkg.size() + S - 1) / S + 1;
    std::string img(sectors * S, '\0');
    std::string pvd(S, '\0');
    pvd[0] = 1; pvd.replace(1, 5, "CD001");
    pvd.replace(156, 34, dirrec(std::string(1, '\0'), 18, S, true).substr(0, 34));
    img.replace(16 * S, S, pvd);
    std::string root = dirrec("PSP_GAME", 19, S, true);
    img.replace(18 * S, root.size(), root);
    const std::string sf = sfo(id, title);
    std::string pg = dirrec("PARAM.SFO;1", 20, (uint32_t)sf.size(), false) + dirrec("USRDIR", 21, S, true);
    img.replace(19 * S, pg.size(), pg);
    img.replace(20 * S, sf.size(), sf);
    std::string ud = dirrec("DATA", 22, S, true);
    img.replace(21 * S, ud.size(), ud);
    if (!pkg_info.empty()) {
        std::string dd = dirrec("PACKAGE_INFO.BIN;1", 23, (uint32_t)pkg_info.size(), false) + dirrec("PACKAGE.BIN;1", 24, (uint32_t)pkg.size(), false);
        img.replace(22 * S, dd.size(), dd);
        img.replace(23 * S, pkg_info.size(), pkg_info);
        img.replace(24 * S, pkg.size(), pkg);
    }
    return img;
}
// the same image as a CSO (CISO v1): even blocks deflated, odd blocks stored
static std::string make_cso(const std::string& iso) {
    const uint32_t B = 2048, n = (uint32_t)((iso.size() + B - 1) / B);
    std::string out(24 + 4 * (n + 1), '\0');
    out.replace(0, 4, "CISO"); put32(out, 4, 24);
    for (int i = 0; i < 8; i++) out[8 + i] = (char)((uint64_t)iso.size() >> (8 * i));
    put32(out, 16, B); out[20] = 1; out[21] = 0;
    for (uint32_t b = 0; b < n; b++) {
        put32(out, 24 + 4 * b, (uint32_t)out.size() | (b & 1 ? 0x80000000u : 0));
        const std::string blk = iso.substr((size_t)b * B, B);
        if (b & 1) out += blk;
        else {
            std::string z(B * 2, '\0');
            const size_t zn = tdefl_compress_mem_to_mem(&z[0], z.size(), blk.data(), blk.size(), TDEFL_DEFAULT_MAX_PROBES);
            out += z.substr(0, zn);
        }
    }
    put32(out, 24 + 4 * n, (uint32_t)out.size());
    return out;
}
static std::string write_file(const std::filesystem::path& p, const std::string& data) {
    std::ofstream(p, std::ios::binary) << data;
    return p.string();
}
// a package index {crc, sector, size} of one entry, and its PACKAGE.BIN
static std::string pkg_index(uint32_t crc, uint32_t size) {
    std::string s(16 + 12, '\0');
    put32(s, 0, 0x09200218u); s.replace(4, 4, "pack"); put32(s, 8, 1);
    put32(s, 16, crc); put32(s, 20, 0); put32(s, 24, size | 0x10000000u);
    return s;
}

static void disc_checks(const std::filesystem::path& tmp) {
    namespace fs = std::filesystem;
    std::string id, title;
    const std::string d1 = write_file(tmp / "d1.iso", make_iso("ULUS10437", "DISSIDIA FINAL FANTASY"));
    CHECK(read_disc_id(d1, &id, &title) && id == "ULUS10437" && title == "DISSIDIA FINAL FANTASY");
    CHECK(d1_disc_problem(d1).empty());
    const std::string d1cso = write_file(tmp / "d1.cso", make_cso(make_iso("ULUS10437", "DISSIDIA FINAL FANTASY")));
    CHECK(read_disc_id(d1cso, &id, &title) && id == "ULUS10437");
    CHECK(d1_disc_problem(d1cso).empty());
    const std::string d012 = write_file(tmp / "012.iso", make_iso("ULUS10566", "DISSIDIA duodecim FINAL FANTASY"));
    CHECK(d1_disc_problem(d012).find("Dissidia 012") != std::string::npos);                  // the reported mistake
    const std::string d1eu = write_file(tmp / "d1eu.iso", make_iso("ULES01270", "DISSIDIA FINAL FANTASY"));
    CHECK(d1_disc_problem(d1eu).find("another release") != std::string::npos);
    const std::string other = write_file(tmp / "other.iso", make_iso("ULUS10391", "Crisis Core"));
    CHECK(d1_disc_problem(other).find("not Dissidia") != std::string::npos);
    const std::string junk = write_file(tmp / "junk.iso", std::string(40000, 'x'));
    CHECK(!read_disc_id(junk, &id, &title));
    CHECK(!d1_disc_problem(junk).empty());
    CHECK(!d1_disc_problem((tmp / "missing.iso").string()).empty());

    // undub: Dissidia 012 whose voices are not the English ones
    CHECK(undub_disc_problem(d1).find("first Dissidia") != std::string::npos);
    CHECK(undub_disc_problem(other).find("not Dissidia 012") != std::string::npos);
    CHECK(!undub_disc_problem(junk).empty());
    const std::string jpbank(284832, 'j');
    const std::string undub = write_file(tmp / "undub.iso", make_iso("ULUS10566", "DISSIDIA duodecim FINAL FANTASY", pkg_index(0x4300D781u, (uint32_t)jpbank.size()), jpbank));
    CHECK(undub_disc_problem(undub).empty());
    const std::string undubcso = write_file(tmp / "undub.cso", make_cso(make_iso("ULUS10566", "DISSIDIA duodecim FINAL FANTASY", pkg_index(0x4300D781u, (uint32_t)jpbank.size()), jpbank)));
    CHECK(undub_disc_problem(undubcso).empty());
    CHECK(undub_disc_problem(d012).find("cannot be read") != std::string::npos);   // no PACKAGE.BIN: a broken 012 image

    // real disc images, when given: SETUP_TEST_DISCS="<D1 USA iso>|<012 iso with English voices>|<undub iso>"
    if (const char* e = std::getenv("SETUP_TEST_DISCS")) {
        std::string v = e;
        const size_t a = v.find('|'), b = v.find('|', a + 1);
        const std::string rd1 = v.substr(0, a), r012 = v.substr(a + 1, b - a - 1), rundub = v.substr(b + 1);
        CHECK(d1_disc_problem(rd1).empty());
        CHECK(d1_disc_problem(r012).find("Dissidia 012") != std::string::npos);
        CHECK(d1_disc_problem(rundub).find("Dissidia 012") != std::string::npos);
        CHECK(undub_disc_problem(rundub).empty());
        CHECK(undub_disc_problem(r012).find("English voices") != std::string::npos);
        CHECK(undub_disc_problem(rd1).find("first Dissidia") != std::string::npos);
        std::printf("real discs:\n  D1 as undub: %s\n  012 as D1: %s\n  012 as undub: %s\n", undub_disc_problem(rd1).c_str(),
                    d1_disc_problem(r012).c_str(), undub_disc_problem(r012).c_str());
    }
}

int main() {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "psp_setup_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "game" / "tools");
    { std::ofstream f(tmp / "game" / "features.ini"); f << "hd_textures=1\njp_audio=0\n"; }
    Layout l = find_layout(tmp.string());
    l.python = "python3";
    CHECK(feature_available(l, "hd_textures"));
    CHECK(!feature_available(l, "jp_audio"));
    CHECK(!feature_available(l, "android"));

    Options o;
    CHECK(o.hd_textures);   // on by default
    CHECK(o.texture_pack == "liink");   // LiinkPK's pack is the first choice
    o.hd_textures = false;
    auto plan = plan_install(l, "x.iso", "/w", o);
    CHECK(plan.size() == 2);
    double sum = 0;
    for (auto& s : plan) sum += s.weight;
    CHECK(std::fabs(sum - 1.0) < 1e-9);
    CHECK(std::fabs(plan[1].base - plan[0].weight) < 1e-9);
    {   // extras on an installed game: no build step, only what is wanted
        Options e;
        auto ep = plan_extras(l, "/w", e, false);
        CHECK(ep.size() == 1 && !ep[0].is_build);           // the texture download only
        CHECK(plan_extras(l, "/w", e, true).empty());        // textures already in place: nothing to do
        e.hd_textures = false;
        CHECK(plan_extras(l, "/w", e, false).empty());
    }
    {   // the Steam shortcut is one optional step (a failure does not fail the install)
        auto sp = plan_steam(l, "/w");
        CHECK(sp.size() == 1 && sp[0].optional && !sp[0].is_build);
        CHECK(sp[0].argv.size() > 3 && sp[0].argv[1].find("steam_shortcut.py") != std::string::npos);
    }
    o.hd_textures = true;   // available in this release: one more step
    o.android = true;       // not available: no step
    plan = plan_install(l, "x.iso", "/w", o);
    CHECK(plan.size() == 3);
    {
        auto& a = plan.back().argv;
        CHECK(a.size() >= 5 && a[a.size() - 2] == "--pack" && a.back() == "liink");
        o.texture_pack = "ai";
        auto p2 = plan_install(l, "x.iso", "/w", o);
        CHECK(p2.back().argv.back() == "ai");
        o.texture_pack = "liink";
    }
    sum = 0;
    for (auto& s : plan) sum += s.weight;
    CHECK(std::fabs(sum - 1.0) < 1e-9);

    // psp_build progress lines + ninja counter
    Progress p;
    p.step = 1;
    feed_line(p, plan, "[psp-build] STEP 2/5 Translating the game to C++ (recompile)", 1000);
    CHECK(p.stage.find("Translating") == 0);
    CHECK(p.fraction > plan[1].base && p.fraction < plan[1].base + plan[1].weight);
    double f0 = p.fraction;
    feed_line(p, plan, "[psp-build] STEP 5/5 Compiling the game", 2000);
    feed_line(p, plan, "[psp-build] PROGRESS 10", 2000);
    CHECK(p.fraction > f0);
    feed_line(p, plan, "[10/474] Building CXX object a/b/batch_0001.cpp.o", 3000);
    CHECK(p.compile_i == 10 && p.compile_n == 474);
    CHECK(p.detail.find("10 of 474") != std::string::npos);
    CHECK(p.detail.find("batch_0001.cpp") != std::string::npos);
    feed_line(p, plan, "[237/474] Building CXX object a/b/batch_0002.cpp.o", 3000 + 120000);
    // 227 files in 120 s -> about 0.53 s each, 237 left: about 125 s -> "About 2 minutes left"
    std::string eta = eta_text(p, 3000 + 120000);
    CHECK(eta.find("2 minutes") != std::string::npos);
    feed_line(p, plan, "[psp-build] ERROR the compiler failed", 4000);
    CHECK(p.error == "the compiler failed");
    CHECK(elapsed_text(p, 1000 + 65000).find("1:05") == 0);

    // update lines
    UpdateInfo u = parse_update_line("UPDATE version=1.2.3 current=1.2.0 size=112233 rebuild=1 relink=0");
    CHECK(u.available && u.version == "1.2.3" && u.current == "1.2.0" && u.size == 112233 && u.rebuild && !u.relink);
    CHECK(!parse_update_line("NOUPDATE current=1.2.0").available);
    CHECK(parse_update_line("ERROR no network").error == "no network");
    auto up = plan_update(l, "/w", Options(), u);
    CHECK(up.size() == 2);
    u.rebuild = false;
    CHECK(plan_update(l, "/w", Options(), u).size() == 1);
    Progress q;
    q.step = 0;
    auto up1 = plan_update(l, "/w", Options(), u);
    feed_line(q, up1, "[update] PROGRESS 50", 10);
    CHECK(q.detail == "Downloading: 50%" && q.fraction > 0.2 && q.fraction < 0.6);

    // settings opt-out
    { std::ofstream f(tmp / "settings.ini"); f << "fps=60\ncheck_updates=0\n"; }
    CHECK(!update_check_enabled(tmp.string()));
    { std::ofstream f(tmp / "settings.ini"); f << "fps=60\n"; }
    CHECK(update_check_enabled(tmp.string()));

    {   // Bartz's Dissidia 1 moves: a step only with the feature, the option and the disc image
        Options e; e.hd_textures = false; e.d1_moves = true; e.d1_iso = "/d1.iso";
        CHECK(plan_extras(l, "/w", e, true).empty());                     // feature off in this release
        { std::ofstream f(tmp / "game" / "features.ini"); f << "hd_textures=1\njp_audio=0\nd1_moves=1\n"; }
        auto dp = plan_extras(l, "/w", e, true);
        CHECK(dp.size() == 1 && dp[0].argv.size() == 7 && dp[0].argv[2] == "install" && dp[0].argv[4] == "/d1.iso" && dp[0].argv[6] == "/w");
        e.d1_iso.clear();
        CHECK(plan_extras(l, "/w", e, true).empty());                     // no disc picked: nothing
        { std::ofstream f(tmp / "game" / "features.ini"); f << "hd_textures=1\njp_audio=0\n"; }
    }

    // state round trip
    State s;
    s.workdir = "/a b/c"; s.iso = "/i.iso"; s.version = "1"; s.options.android = true; s.options.undub_iso = "/u.iso"; s.options.check_updates = false; s.options.d1_moves = true;
    save_state((tmp / "state.ini").string(), s);
    State r = load_state((tmp / "state.ini").string());
    CHECK(r.workdir == s.workdir && r.options.android && r.options.undub_iso == "/u.iso" && !r.options.check_updates && r.options.d1_moves);

#ifndef _WIN32
    {   // the menu entry
        fs::path data = tmp / "xdg", wd = tmp / "my game dir";
        fs::create_directories(wd / "disc0/PSP_GAME");
        { std::ofstream(wd / "disc0/PSP_GAME/ICON0.PNG") << "png"; }
        setenv("XDG_DATA_HOME", data.string().c_str(), 1);
        std::string where, err;
        CHECK(create_shortcut(wd.string(), "", &where, &err));
        CHECK(where == (data / "applications/dissidia-012.desktop").string());
        std::ifstream df(where);
        std::string txt((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
        CHECK(txt.find("Exec=\"") == 0 || txt.find("\nExec=\"") != std::string::npos);
        CHECK(txt.find("--launch") != std::string::npos);
        CHECK(txt.find("Icon=" + (wd / "icon.png").string()) != std::string::npos);
        CHECK(txt.find("Categories=Game;") != std::string::npos);
    }
#endif

    disc_checks(tmp);

    fs::remove_all(tmp);
    std::printf(failures ? "%d FAILED\n" : "all setup tests passed\n", failures);
    return failures ? 1 : 0;
}
