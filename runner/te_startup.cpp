// runner/te_startup.cpp -- see te_startup.h.
#include "te_startup.h"

#include "engine/core/config/config.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#endif

namespace te::startup {
namespace fs = std::filesystem;

namespace {

fs::path ExecutableDir() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return fs::path(buf).parent_path();
#elif defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0) return {};
    std::error_code ec;
    const fs::path p = fs::canonical(path, ec);
    return (ec ? fs::path(path) : p).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p.parent_path();
#else
    return {};
#endif
}

bool Blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"'; }

std::string Trimmed(const std::string& line) {
    size_t begin = 0, end = line.size();
    while (begin < end && Blank(line[begin])) ++begin;
    while (end > begin && Blank(line[end - 1])) --end;
    return line.substr(begin, end - begin);
}

#if !defined(_WIN32) && !defined(__vita__)
// The first line a dialog command printed (its chosen path), or empty when it was cancelled or does not exist.
std::string FirstLineOf(const char* command) {
    FILE* p = popen(command, "r");
    if (!p) return {};
    char buf[4096];
    std::string out;
    if (std::fgets(buf, sizeof buf, p)) out = buf;
    const int rc = pclose(p);
    return rc == 0 ? Trimmed(out) : std::string();
}
#endif

fs::path HomeDataDir() {
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home && *home ? fs::path(home) / ".twinsanityevolved" : fs::path();
}

// A folder the game may write to: a probe file created and removed.
bool Writable(const fs::path& dir) {
    std::error_code ec;
    const fs::path probe = dir / ".twinsanityevolved-write-test";
    {
        std::ofstream f(probe, std::ios::binary);
        if (!f) return false;
    }
    fs::remove(probe, ec);
    return true;
}

}  // namespace

std::string GameFolderPath() {
    const fs::path exe = ExecutableDir();
    return exe.empty() ? std::string() : ps2r::app::GameFolder::FolderOf(exe.generic_string());
}

std::string DiscImageBesideExecutable() {
    // beside the executable, else in the game folder (the one holding the .app bundle on macOS: the launcher writes it there)
    fs::path dir = ExecutableDir();
    if (dir.empty()) return {};
    std::ifstream f(dir / kDiscImageFile, std::ios::binary);
    if (!f) {
        const std::string game = GameFolderPath();
        if (game.empty() || fs::path(game) == dir) return {};
        f.open(fs::path(game) / kDiscImageFile, std::ios::binary);
        if (!f) return {};
    }
    std::string line;
    std::getline(f, line);
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF)
        line.erase(0, 3);   // a UTF-8 byte-order mark from an editor
    return Trimmed(line);
}

std::string ChooseDiscImage() {
#if defined(_WIN32)
    static wchar_t file[32768];
    file[0] = L'\0';
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"PS2 disc image (*.iso;*.bin)\0*.iso;*.bin\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(sizeof file / sizeof file[0]);
    ofn.lpstrTitle = L"Crash Twinsanity: choose your disc image (the European SLES-52568 or the North American SLUS-20909)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return {};
    // the C library opens the image through the ANSI code page, as it does a path given with --iso
    const int n = WideCharToMultiByte(CP_ACP, 0, file, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string path(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, file, -1, path.data(), n, nullptr, nullptr);
    return path;
#elif defined(__vita__)
    return {};   // the device has no file dialog: the disc is the fixed ux0:data path (runner/te_vita.cpp)
#elif defined(__APPLE__)
    return FirstLineOf("/usr/bin/osascript -e 'POSIX path of (choose file with prompt "
                       "\"Crash Twinsanity: choose your disc image (the European SLES-52568 or the North American SLUS-20909)\")' 2>/dev/null");
#else
    std::string path = FirstLineOf("zenity --file-selection --title='Crash Twinsanity: choose your disc image (SLES-52568 or SLUS-20909)' "
                                   "--file-filter='PS2 disc image | *.iso *.bin' 2>/dev/null");
    if (path.empty())
        path = FirstLineOf("kdialog --getopenfilename . 'PS2 disc image (*.iso *.bin)' --title 'Crash Twinsanity: choose your disc image' 2>/dev/null");
    return path;
#endif
}

bool RememberDiscImage(const std::string& path) {
    // the game folder: beside the executable, or beside the .app bundle on macOS (never inside a signed bundle)
    const std::string game = GameFolderPath();
    if (game.empty() || path.empty()) return false;
    std::ofstream f(fs::path(game) / kDiscImageFile, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << path << "\n";
    return static_cast<bool>(f);
}

std::string DataDirectory() {
    std::error_code ec;
    const std::string game_folder = GameFolderPath();
    const fs::path exe = game_folder.empty() ? fs::path() : fs::path(game_folder);
    const fs::path portable = exe.empty() ? fs::path() : exe / "userdata";
    const fs::path home = HomeDataDir();
    if (!portable.empty() && fs::is_directory(portable, ec)) return portable.generic_string();
    if (!home.empty() && fs::is_directory(home, ec)) return home.generic_string();   // an earlier install's saves stay where they are
    if (!portable.empty() && Writable(exe)) return portable.generic_string();
    return home.generic_string();
}

ps2r::app::GameFolder::Resolved LocateFolders(const std::string& config_arg, const std::string& mc_dir_arg, const std::string& mods_dir_arg,
                                              ps2r::app::GameFolder::Inputs* inputs) {
    ps2r::app::GameFolder::Inputs in;
    const fs::path exe = ExecutableDir();
    in.exe_dir = exe.empty() ? std::string() : exe.generic_string();
    in.data_dir = DataDirectory();
    in.config_arg = config_arg;
    in.mc_dir_arg = mc_dir_arg;
    in.mods_dir_arg = mods_dir_arg;
    if (inputs) *inputs = in;
    return ps2r::app::GameFolder::Locate(in);
}

namespace {
// The key a line of ps2r.ini sets (the engine's grammar: `#`/`;` comments, `[section]` headers, `key = value`)
std::string IniLineKey(const std::string& line, std::string& section) {
    std::string l = line.substr(0, line.find_first_of("#;"));
    l = Trimmed(l);
    if (l.empty()) return {};
    if (l.front() == '[' && l.back() == ']') { section = Trimmed(l.substr(1, l.size() - 2)); return {}; }
    const size_t eq = l.find('=');
    if (eq == std::string::npos) return {};
    std::string key = Trimmed(l.substr(0, eq));
    if (!section.empty() && key.find('.') == std::string::npos) key = section + "." + key;
    return key;
}
}  // namespace

std::string TakePortKeys(const std::string& text, std::string* memcards, std::string* mods) {
    static const char* const kPortKeys[] = {"paths.memcards", "paths.mods", "mods.order", "display.tick_lock"};
    std::string out, section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const std::string key = IniLineKey(line, section);
        bool port = false;
        for (const char* k : kPortKeys) port |= key == k;
        if (port && !ps2r::Config::IsKey(key)) {
            std::string l = line.substr(0, line.find_first_of("#;"));
            const std::string value = Trimmed(l.substr(l.find('=') + 1));
            if (key == "paths.memcards" && memcards) *memcards = value;
            if (key == "paths.mods" && mods) *mods = value;
            out += "\n";   // the line numbers of the engine's messages stay the file's
            continue;
        }
        out += line;
        out += "\n";
    }
    return out;
}


void AttachParentConsole() {
#if defined(_WIN32)
    if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_UNKNOWN) return;   // redirected: keep it
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;                               // started from a shortcut
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONOUT$", "w", stdout);
#endif
}

void ShowError(const char* title, const std::string& text) {
    std::fprintf(stderr, "[twinsanityevolved] %s: %s\n", title, text.c_str());
#if defined(_WIN32)
    if (GetConsoleWindow() == nullptr && GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_UNKNOWN)
        MessageBoxA(nullptr, text.c_str(), title, MB_OK | MB_ICONERROR);
#endif
}

}  // namespace te::startup
