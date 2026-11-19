#include "extras.h"

#include <cstdio>
#include <filesystem>
#include <set>

#include "install.h"

namespace setup {
namespace fs = std::filesystem;

namespace {
std::string join(const std::string& a, const std::string& b) { return (fs::path(a) / b).string(); }

std::string esc(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

// ---- the list. Order = order on the pages and of the install steps.
std::vector<Extra> make_extras() {
    std::vector<Extra> v;

    {   // HD textures: a downloaded pack, unpacked into mods/textures (the runtime's texture replacement)
        Extra e;
        e.key = "hd_textures";
        e.feature = "hd_textures";
        e.title = "HD textures";
        e.help = "Higher resolution textures for the whole game. Turn it on to pick which pack to use.";
        e.android_help = " The download goes over your connection (WiFi is best) and needs about 3 GB of free space while it unpacks.";
        e.icon = "texture";
        e.def = "1";
        e.reapply = Extra::Redo;
        e.present = [](const ExtraCtx& c) { std::error_code ec; return fs::is_regular_file(fs::path(c.workdir) / "mods/textures/textures.ini", ec); };
        e.remove = [](const ExtraCtx& c) {
            std::error_code ec;
            fs::remove_all(fs::path(c.workdir) / "mods/textures", ec);
            if (c.android) fs::remove_all(fs::path(c.game_files()) / "mods/textures", ec);
        };
        e.steps = [](const ExtraCtx& c, const Options& o, std::vector<Step>& p) {
            const bool ai = o.get("texture_pack") == "ai";
            if (!c.android) {   // fetch_textures.py downloads and unpacks in one go
                p.push_back(c.tool(ai ? "Downloading the AI upscaled textures" : "Downloading the HD textures",
                                   ai ? "Downloads the AI upscaled HD textures (about 1.3 GB)." : "Downloads LiinkPK's HD textures (about 0.8 GB).",
                                   "fetch_textures.py", {c.workdir, "--pack", ai ? "ai" : "liink"}, 0.06));
                return;
            }
            const std::string mods = join(c.workdir, "mods");
            std::error_code ec;
            fs::create_directories(mods, ec);
            std::vector<std::pair<std::string, std::string>> files;
            std::vector<std::string> args = {c.workdir, "--pack", ai ? "ai" : "liink", "--consume-zips"};
            if (ai) {
                files = {{"https://codeload.github.com/Niosa/D012FFRT/zip/main", join(mods, ".d012ffrt.zip")},
                         {"https://codeload.github.com/schindlershadow/dissidia-hd-textures/zip/main", join(mods, ".hdtextures.zip")}};
                args.insert(args.end(), {"--zip", join(mods, ".d012ffrt.zip"), "--ids-zip", join(mods, ".hdtextures.zip")});
            } else {
                files = {{"https://codeload.github.com/LiinkPK/Dissidia-012-HD-Textures/zip/master", join(mods, ".liink.zip")}};
                args.insert(args.end(), {"--zip", join(mods, ".liink.zip")});
            }
            p.push_back(c.download(ai ? "Downloading the AI upscaled textures" : "Downloading the HD textures",
                                   ai ? "Downloads the AI upscaled textures (about 1.3 GB). WiFi is best." : "Downloads LiinkPK's HD textures (about 0.8 GB). WiFi is best.",
                                   files, 0.25));
            p.push_back(c.tool("Unpacking the HD textures", "Unpacks the textures into the mods folder.", "fetch_textures.py", args, 0.06));
        };
        v.push_back(e);
    }
    {   // which pack (a choice under HD textures)
        Extra e;
        e.key = "texture_pack";
        e.feature = "hd_textures";
        e.title = "Texture pack";
        e.icon = "sliders";
        e.parent = "hd_textures";
        e.def = "liink";
        e.choices = {{"liink", "LiinkPK's HD Project",
                      "Hand-retouched HD textures, redrawn faces and text (about 0.8 GB). Still a work in progress, so some textures stay original. "
                      "Credits: Dissidia 012 HD Project by LiinkPK (github.com/LiinkPK/Dissidia-012-HD-Textures); artwork also by ajb3art."},
                     {"ai", "AI upscaled",
                      "D012FFRT plus our own AI upscales of the other textures (about 1.3 GB). "
                      "Credits: D012FFRT by Niosa (github.com/Niosa/D012FFRT); AI upscales by schindlershadow made with Real-ESRGAN (Xintao Wang et al.)."}};
        v.push_back(e);
    }
    {   // Japanese voices from an undub / Japanese disc
        Extra e;
        e.key = "jp_audio";
        e.feature = "jp_audio";
        e.title = "Japanese voices (undub)";
        e.help = "Adds the Japanese voices and movies from a second disc image (an undub ISO) that you pick next.";
        e.icon = "voice";
        e.disc = {"undub_iso", "The undub file", "Japanese voices", "A second disc image",
                  "The Japanese voices and movies come from a Japanese or undub version of Dissidia 012, as a disc image: a .iso or .cso. It is only read once; your own disc stays the base of the game.",
                  "Valid: the undub / Japanese disc",
                  "A Dissidia 012 image that has the Japanese voices, for example the Japanese release or an undub patch of the game. The same game as your own disc, just with different voice and movie files. A disc with the English voices, or another game, is refused with a message.",
                  ".iso or .cso", "Japanese / undub"};
        e.disc.check = undub_disc_problem;   // a Dissidia 012 without the English voices
        e.steps = [](const ExtraCtx& c, const Options& o, std::vector<Step>& p) {
            p.push_back(c.tool("Extracting the Japanese voices", "Adds the Japanese voices and movies from the undub disc.", "jp_audio.py",
                               {"--iso", o.get("undub_iso"), "--work", c.workdir}, 0.05));
        };
        v.push_back(e);
    }
    {   // Bartz's eight moves of Dissidia (2008), rebuilt from the player's two discs
        Extra e;
        e.key = "d1_moves";
        e.feature = "d1_moves";
        e.title = "Bartz: Dissidia 1 moves";
        e.help = "Gives Bartz back the eight moves he had in the first Dissidia (Soul Eater, Flood, Wind Shear, Paladin Force...), next to his 012 kit. "
                 "Needs your Dissidia 1 (2008) disc image, which you pick next. In the game, Settings > Mods turns them off and on. Online, players without them "
                 "receive what they need from you.";
        e.icon = "sword";
        e.disc = {"d1_iso", "Dissidia 1 disc", "Bartz's Dissidia 1 moves", "A Dissidia 1 disc image",
                  "Bartz's eight moves from the first Dissidia (Soul Eater, Flood, Paladin Force...) are rebuilt from your own copy of Dissidia Final Fantasy (2008), as a .iso disc image. It is only read once. The app ships only small patches that need both discs to turn into the moves.",
                  "Valid: the US release",
                  "Dissidia Final Fantasy (USA, ULUS-10437) as an uncompressed .iso, together with a US Dissidia 012. Unpack .zip / .7z first. Other releases are refused with a message; the rest of the install is not affected.",
                  ".iso", "US"};
        e.disc.check = d1_disc_problem;   // Dissidia Final Fantasy (USA, ULUS-10437) only
        e.steps = [](const ExtraCtx& c, const Options& o, std::vector<Step>& p) {
            p.push_back(c.tool("Adding Bartz's Dissidia 1 moves",
                               "Rebuilds the eight moves Bartz had in the first Dissidia from your Dissidia 1 disc and adds them as new abilities. Settings > Mods in the game turns them off and on.",
                               "d1_moves.py", {"install", "--d1", o.get("d1_iso"), "--work", c.workdir}, 0.01));
        };
        v.push_back(e);
    }
    return v;
}

bool offered(const Extra& e, const Layout& l, bool android) {
    return (android ? e.android : e.desktop) && (e.feature.empty() || feature_available(l, e.feature));
}

bool is_on(const Extra& e, const Options& o) {
    std::string v = o.get(e.key);
    return e.choices.empty() ? v == "1" : !v.empty() && v != "0";
}

// a sub-choice (texture pack) changed
bool children_changed(const Extra& e, const Options& now, const Options& prev) {
    for (const Extra& c : extras())
        if (c.parent == e.key && now.get(c.key) != prev.get(c.key)) return true;
    return false;
}
}  // namespace

const std::vector<Extra>& extras() {
    static const std::vector<Extra> list = make_extras();
    return list;
}

const Extra* find_extra(const std::string& key) {
    for (const Extra& e : extras())
        if (e.key == key) return &e;
    return nullptr;
}

std::string Options::get(const std::string& key) const {
    auto it = extras.find(key);
    if (it != extras.end()) return it->second;
    const Extra* e = find_extra(key);
    return e ? e->def : "";
}

void Options::clear_discs() {
    for (const Extra& e : setup::extras())
        if (!e.disc.key.empty()) extras.erase(e.disc.key);
}

Step ExtraCtx::tool(const std::string& label, const std::string& explain, const std::string& script, const std::vector<std::string>& args, double weight) const {
    Step s;
    s.label = label;
    s.explain = explain;
    s.argv = {l.python, join(l.game_dir, "tools/" + script)};
    s.argv.insert(s.argv.end(), args.begin(), args.end());
#ifdef __ANDROID__
    if (android) s.env = android_env(l, workdir);
    else
#endif
        s.env = {{"PYTHONUNBUFFERED", "1"}};
    s.weight = weight;
    return s;
}

Step ExtraCtx::download(const std::string& label, const std::string& explain, const std::vector<std::pair<std::string, std::string>>& files, double weight) const {
    if (android) {
        Step s;
        s.label = label;
        s.explain = explain;
        s.downloads = files;
        s.weight = weight;
        return s;
    }
    std::vector<std::string> args;
    for (auto& f : files) { args.push_back(f.first); args.push_back(f.second); }
    return tool(label, explain, "download.py", args, weight);
}

std::string ExtraCtx::game_files() const {
#ifdef __ANDROID__
    if (android) return android_game_dir();
#endif
    return workdir;
}

void plan_extra_steps(std::vector<Step>& p, const ExtraCtx& ctx, const Options& now, const Options* prev) {
    for (const Extra& e : extras()) {
        if (!e.parent.empty() || !e.steps || !offered(e, ctx.l, ctx.android)) continue;   // sub-choices belong to their parent's steps
        const bool on = is_on(e, now), was = prev && is_on(e, *prev);
        if (e.reapply == Extra::Redo && prev) {
            const bool changed = !was || children_changed(e, now, *prev);
            const bool there = e.present ? e.present(ctx) : false;
            if (e.remove && (was || there) && (!on || changed)) e.remove(ctx);   // off, or another variant: the old files must not stay
            if (!on || (!changed && there)) continue;
        } else if (!on) {
            continue;
        } else if (prev && was && e.disc.key.empty()) {
            continue;   // Once, no disc: already added
        }
        if (!e.disc.key.empty() && now.get(e.disc.key).empty()) continue;   // already added earlier (no new disc), or not picked
        e.steps(ctx, now, p);
    }
}

std::vector<std::string> extras_needing_disc(const Layout& l, bool android, const Options& now, const Options* prev) {
    std::vector<std::string> keys;
    for (const Extra& e : extras()) {
        if (e.disc.key.empty() || !offered(e, l, android) || !is_on(e, now)) continue;
        if (prev && is_on(e, *prev)) continue;   // already added: no disc needed (its files stay)
        if (now.get(e.disc.key).empty()) keys.push_back(e.key);
    }
    return keys;
}

std::string extra_disc_problem(const std::string& key, const std::string& path) {
    const Extra* e = find_extra(key);
    return e && e->disc.check ? e->disc.check(path) : "";
}

std::string extras_json(const Layout& l, bool android) {
    std::string o = "[";
    bool first = true;
    for (const Extra& e : extras()) {
        if (!(android ? e.android : e.desktop)) continue;
        if (!first) o += ",";
        first = false;
        o += "{\"key\":" + esc(e.key) + ",\"title\":" + esc(e.title) + ",\"help\":" + esc(e.help + (android ? e.android_help : "")) + ",\"icon\":" + esc(e.icon) +
             ",\"def\":" + esc(e.def) + ",\"parent\":" + esc(e.parent) + ",\"available\":" + (offered(e, l, android) ? "true" : "false") + ",\"choices\":[";
        for (size_t i = 0; i < e.choices.size(); i++)
            o += (i ? "," : "") + std::string("{\"value\":") + esc(e.choices[i].value) + ",\"label\":" + esc(e.choices[i].label) + ",\"help\":" + esc(e.choices[i].help) + "}";
        o += "]";
        if (!e.disc.key.empty())
            o += ",\"disc\":{\"key\":" + esc(e.disc.key) + ",\"title\":" + esc(e.disc.page_title) + ",\"eyebrow\":" + esc(e.disc.eyebrow) + ",\"what\":" + esc(e.disc.what) +
                 ",\"what_help\":" + esc(e.disc.what_help) + ",\"valid\":" + esc(e.disc.valid) + ",\"valid_help\":" + esc(e.disc.valid_help) + ",\"exts\":" + esc(e.disc.exts) + ",\"valid_short\":" + esc(e.disc.valid_short) + "}";
        o += "}";
    }
    return o + "]";
}

}  // namespace setup
