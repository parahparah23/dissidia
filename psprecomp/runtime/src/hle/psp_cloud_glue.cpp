#include "psp_cloud_glue.h"

#include "psp_cloud_service.h"
#include "psp_savebackup.h"
#include "psp_savemgr.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace svc = psp_cloud_svc;
namespace fs = std::filesystem;

namespace {
std::atomic<bool> g_on{false};
std::mutex g_env_mu;
psp_saves::Env g_env;   // folders, filled at boot
bool disabled() {
    const char* e = std::getenv("PSPRECOMP_CLOUD");
    return e && e[0] == '0';
}
}  // namespace

void psp_cloud_glue_boot(const std::string& disc0_dir, const std::string& ms0_dir) {
    {   // the day's backup, before a start-up sync may download anything (psp_savebackup.h)
        const fs::path ms0 = fs::u8path(ms0_dir);
        std::error_code ec;
        std::lock_guard<std::mutex> lk(g_env_mu);
        g_env.saves_root = (ms0 / "PSP" / "SAVEDATA").u8string();
        g_env.backup_root = (ms0 / "PSP" / "SAVEDATA_BACKUP").u8string();
        g_env.game_dir = fs::weakly_canonical(ms0, ec).parent_path().u8string();
        g_env.scratch_dir = (fs::u8path(g_env.game_dir) / "savemgr_tmp").u8string();
        g_env.disc_id = psp_savemgr::installed_disc_id(fs::u8path(disc0_dir).parent_path().u8string());
        if (!psp_savemgr::valid_disc_id(g_env.disc_id)) g_env.disc_id.clear();
        if (psp_savebackup::daily((ms0 / "PSP" / "SAVEDATA").u8string(), (ms0 / "PSP" / "SAVEDATA_BACKUP").u8string()))
            std::fprintf(stderr, "[SAVEMGR] daily backup made\n");
    }
    if (disabled()) { std::fprintf(stderr, "[CLOUD] disabled (PSPRECOMP_CLOUD=0)\n"); return; }
    svc::Paths p;
    const fs::path ms0 = fs::u8path(ms0_dir);
    std::error_code ec;
    p.workdir = fs::weakly_canonical(ms0, ec).parent_path().u8string();
    p.saves_root = (ms0 / "PSP" / "SAVEDATA").u8string();
    p.backup_root = (ms0 / "PSP" / "SAVEDATA_BACKUP").u8string();
    p.disc_id = psp_savemgr::installed_disc_id(fs::u8path(disc0_dir).parent_path().u8string());
    if (!psp_savemgr::valid_disc_id(p.disc_id)) p.disc_id.clear();
    svc::init(p);
    g_on = true;
    if (svc::configured()) {
        const svc::Config c = svc::config();
        std::fprintf(stderr, "[CLOUD] %s sync %s (disc %s)\n", svc::provider_name(c.provider), c.auto_sync ? "on" : "manual only",
                     p.disc_id.empty() ? "?" : p.disc_id.c_str());
        svc::startup_sync();
    }
}

void psp_cloud_glue_before_savedata() {
    if (!g_on) return;
    if (!svc::wait_ready(20000)) std::fprintf(stderr, "[CLOUD] start-up sync still running after 20 s; continuing\n");
}

void psp_cloud_glue_after_save() {
    if (g_on) svc::after_save();
}

psp_saves::Env psp_cloud_glue_saves_env() {
    psp_saves::Env e;
    {
        std::lock_guard<std::mutex> lk(g_env_mu);
        e = g_env;
    }
    e.device_name = svc::device_name();
    if (!g_on) return e;
    e.cloud_status = [] { return svc::status(); };
    e.cloud_configured = [] { return svc::configured(); };
    e.cloud_sync = [] { svc::sync_now(); };
    e.cloud_resolve = [](const std::string& s, psp_cloud::Choice c) { svc::resolve(s, c); };
    return e;
}

void psp_cloud_glue_shutdown() {
    if (!g_on) return;
    svc::shutdown(4000);       // lets a running upload finish
    g_on = false;
}
