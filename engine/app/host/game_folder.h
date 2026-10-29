#pragma once
// GameFolder -- TwinsanityEvolved's GAME FOLDER: the one folder a downloaded / launcher-installed game keeps everything in,
// so moving the folder moves everything:
//
//   <folder>/twinsanityevolved(.exe)   or   <folder>/TwinsanityEvolved.app   (the Vita: ux0:data/twinsanity/)
//   <folder>/ps2r.ini                  the ONE settings file (the launcher's Settings page and the in-game Options edit it)
//   <folder>/memcards/                 the vanilla card (memcards-mods/ beside it while any mod is on)
//   <folder>/mods/                     one sub-folder per mod (<id>/mod.json, disc/, data/)
//
// The game finds ps2r.ini BESIDE ITS EXECUTABLE (macOS: in the folder that holds the .app bundle) when one is there --
// PORTABLE mode. Without one, the earlier lookups stand (the caller's data folder: userdata/ beside the game, or an earlier
// install's <home>/.twinsanityevolved; the settings file beside the memory cards). ps2r.ini may carry paths.memcards and
// paths.mods, relative to the folder that holds it unless absolute. For one run --config FILE | none, --mc-dir and
// --mods-dir override. With no settings file (--config none) the mods folder is only what --mods-dir names.
//
// Shared host code: plain C++17, no directory iteration (the Vita), stdio for the one existence check; the runner
// (runner/te_startup.cpp) and the launcher resolve the same folder with it. Engine-neutral: nothing here reads the ini.
#include <string>

namespace ps2r::app {

class GameFolder {
public:
    static constexpr const char* kSettingsFile = "ps2r.ini";
    static constexpr const char* kMemcards = "memcards";
    static constexpr const char* kMods = "mods";

    // The folder of a game whose executable lives in `exe_dir`: exe_dir itself, or -- an executable inside a macOS
    // bundle (<name>.app/Contents/MacOS) -- the folder that holds the bundle. "" stays "".
    static std::string FolderOf(const std::string& exe_dir);
    // Absolute on any target: "/x", "\x", "C:\x", "C:/x", "ux0:data/x" (a device or drive before the first separator)
    static bool IsAbsolute(const std::string& path);
    // `path` relative to `folder` unless absolute ("" stays ""; no folder: the path as given)
    static std::string Resolve(const std::string& folder, const std::string& path);
    // The folder part of a file path ("." when it has none)
    static std::string FolderOfFile(const std::string& file);
    // Whether <folder>/ps2r.ini exists (a file that opens for reading)
    static bool HasSettings(const std::string& folder);

    struct Inputs {
        std::string exe_dir;        // the running executable's folder ("" when the platform cannot say)
        std::string data_dir;       // the earlier lookups' data folder (userdata/ or <home>/.twinsanityevolved; "" = none)
        std::string config_arg;     // --config: "" not given, "none" no settings file, else the file
        std::string mc_dir_arg;     // --mc-dir ("" not given)
        std::string mods_dir_arg;   // --mods-dir ("" not given)
    };
    struct Resolved {
        bool portable = false;      // ps2r.ini beside the executable is the settings file
        std::string folder;         // the folder that holds the settings file ("" = no settings file)
        std::string settings;       // the settings file ("" = none)
        std::string memcards;       // the memory cards' folder ("" = none known)
        std::string mods;           // the mods folder ("" = none)
        std::string how;            // one line for the log: where each came from
    };
    // Where everything is before ps2r.ini is read
    static Resolved Locate(const Inputs& in);
    // ps2r.ini's paths.memcards / paths.mods (as written in the file; "" = absent) applied, unless --mc-dir / --mods-dir
    static void ApplySettings(Resolved& r, const Inputs& in, const std::string& paths_memcards, const std::string& paths_mods);
    // "game folder: <folder> (portable) -- settings ..., memory cards ..., mods ..."
    static std::string Describe(const Resolved& r);
};

}  // namespace ps2r::app
