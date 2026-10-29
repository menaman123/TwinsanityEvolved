// GameFolder implementation; the layout and the rules are in the header.
#include "app/host/game_folder.h"

#include <cstdio>

namespace ps2r::app {

namespace {

bool IsSeparator(char c) { return c == '/' || c == '\\'; }

std::string TrimSeparators(std::string s) {
    while (s.size() > 1 && IsSeparator(s.back())) s.pop_back();
    return s;
}

bool EndsWithNoCase(const std::string& s, const std::string& tail) {
    if (s.size() < tail.size()) return false;
    for (size_t i = 0; i < tail.size(); ++i) {
        char a = s[s.size() - tail.size() + i], b = tail[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b && !(IsSeparator(a) && IsSeparator(b))) return false;
    }
    return true;
}

// one more clause of Resolved::how
void Note(std::string& how, const char* what) {
    if (!how.empty()) how += ", ";
    how += what;
}

std::string Join(const std::string& folder, const char* name) {
    if (folder.empty()) return name;
    return IsSeparator(folder.back()) ? folder + name : folder + "/" + name;
}

}  // namespace

std::string GameFolder::FolderOf(const std::string& exe_dir) {
    const std::string dir = TrimSeparators(exe_dir);
    if (dir.empty()) return dir;
    // <folder>/<name>.app/Contents/MacOS -> <folder>
    static const std::string kBundleTail = "/Contents/MacOS";
    if (EndsWithNoCase(dir, kBundleTail)) {
        const std::string bundle = dir.substr(0, dir.size() - kBundleTail.size());
        if (EndsWithNoCase(bundle, ".app")) {
            const size_t slash = bundle.find_last_of("/\\");
            if (slash != std::string::npos) return slash == 0 ? bundle.substr(0, 1) : bundle.substr(0, slash);
        }
    }
    return dir;
}

bool GameFolder::IsAbsolute(const std::string& path) {
    if (path.empty()) return false;
    if (IsSeparator(path[0])) return true;
    // a drive or a device ("C:", "ux0:", "cdrom0:") before the first separator
    for (size_t i = 0; i < path.size(); ++i) {
        if (IsSeparator(path[i])) return false;
        if (path[i] == ':') return i > 0;
    }
    return false;
}

std::string GameFolder::Resolve(const std::string& folder, const std::string& path) {
    if (path.empty() || IsAbsolute(path) || folder.empty()) return path;
    std::string rel = path;
    while (rel.size() > 2 && rel[0] == '.' && IsSeparator(rel[1])) rel.erase(0, 2);   // "./mods" -> "mods"
    if (rel == ".") return folder;
    return IsSeparator(folder.back()) ? folder + rel : folder + "/" + rel;
}

std::string GameFolder::FolderOfFile(const std::string& file) {
    const size_t slash = file.find_last_of("/\\");
    if (slash == std::string::npos) {
        // "C:ps2r.ini" / "ux0:ps2r.ini": the device's current folder
        const size_t colon = file.find(':');
        return colon == std::string::npos ? std::string(".") : file.substr(0, colon + 1);
    }
    return slash == 0 ? file.substr(0, 1) : file.substr(0, slash);
}

bool GameFolder::HasSettings(const std::string& folder) {
    if (folder.empty()) return false;
    std::FILE* f = std::fopen(Join(folder, kSettingsFile).c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

GameFolder::Resolved GameFolder::Locate(const Inputs& in) {
    Resolved r;
    const std::string game = FolderOf(in.exe_dir);
    // the settings file: --config, else ps2r.ini beside the game (portable), else beside the memory cards (earlier installs)
    if (in.config_arg == "none") {
        Note(r.how, "settings: none (--config none)");
    } else if (!in.config_arg.empty()) {
        r.settings = in.config_arg;
        r.folder = FolderOfFile(in.config_arg);
        Note(r.how, "settings: --config");
    } else if (HasSettings(game)) {
        r.portable = true;
        r.folder = game;
        r.settings = Join(game, kSettingsFile);
        Note(r.how, "settings: beside the game (portable)");
    }
    // the memory cards: --mc-dir, else <folder>/memcards in portable mode, else the data folder's (earlier installs)
    if (!in.mc_dir_arg.empty()) {
        r.memcards = in.mc_dir_arg;
        Note(r.how, "memory cards: --mc-dir");
    } else if (r.portable) {
        r.memcards = Join(r.folder, kMemcards);
        Note(r.how, "memory cards: the game folder's");
    } else if (!in.data_dir.empty()) {
        r.memcards = Join(in.data_dir, kMemcards);
        Note(r.how, "memory cards: the data folder's");
    }
    if (r.settings.empty() && in.config_arg != "none" && r.memcards.find_last_of("/\\") != std::string::npos) {
        // the earlier rule: the settings file beside the memory cards' folder (a cards path without a folder part: none)
        r.folder = FolderOfFile(r.memcards);
        r.settings = Join(r.folder, kSettingsFile);
        Note(r.how, "settings: beside the memory cards");
    }
    // the mods: --mods-dir, else <the settings file's folder>/mods (no settings file: none)
    if (!in.mods_dir_arg.empty()) {
        r.mods = in.mods_dir_arg;
        Note(r.how, "mods: --mods-dir");
    } else if (!r.folder.empty()) {
        r.mods = Join(r.folder, kMods);
        Note(r.how, "mods: beside the settings");
    }
    return r;
}

void GameFolder::ApplySettings(Resolved& r, const Inputs& in, const std::string& paths_memcards, const std::string& paths_mods) {
    if (in.mc_dir_arg.empty() && !paths_memcards.empty()) {
        r.memcards = Resolve(r.folder, paths_memcards);
        Note(r.how, "memory cards: paths.memcards");
    }
    if (in.mods_dir_arg.empty() && !paths_mods.empty()) {
        r.mods = Resolve(r.folder, paths_mods);
        Note(r.how, "mods: paths.mods");
    }
}

std::string GameFolder::Describe(const Resolved& r) {
    std::string s = "game folder: " + (r.folder.empty() ? std::string("(none)") : r.folder) + (r.portable ? " (portable)" : "");
    s += " -- settings " + (r.settings.empty() ? std::string("none") : r.settings);
    s += ", memory cards " + (r.memcards.empty() ? std::string("none") : r.memcards);
    s += ", mods " + (r.mods.empty() ? std::string("none") : r.mods);
    s += " (" + r.how + ")";
    return s;
}

}  // namespace ps2r::app
