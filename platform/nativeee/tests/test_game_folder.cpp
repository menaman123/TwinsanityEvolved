// test_game_folder.cpp -- engine/app/host/game_folder.h: THE GAME FOLDER the runner (runner/te_startup.cpp LocateFolders)
// and the launcher resolve -- ps2r.ini beside the executable (macOS: beside the .app bundle) wins when present (portable),
// paths.memcards / paths.mods relative to the settings file's folder, the earlier userdata/ / <home> lookups as the
// fallback, --config / --mc-dir / --mods-dir overriding for a run, the Vita's fixed ux0:data/twinsanity layout. Over
// temporary folders the test makes.
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/host/game_folder.h"
#include "te_test.h"

namespace fs = std::filesystem;
using ps2r::app::GameFolder;

namespace {
void Touch(const fs::path& p, const std::string& text = "game.language = english\n") {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}
std::string G(const fs::path& p) { return p.generic_string(); }
}  // namespace

int main() {
    using namespace te_test;

    // --- the pure rules
    CHECK(GameFolder::FolderOf("/a/b") == "/a/b");
    CHECK(GameFolder::FolderOf("/a/b/") == "/a/b");
    CHECK(GameFolder::FolderOf("/x/Twinsanity Evolved.app/Contents/MacOS") == "/x");
    CHECK(GameFolder::FolderOf("/x/TE.APP/Contents/MacOS/") == "/x");
    CHECK(GameFolder::FolderOf("/x/Contents/MacOS") == "/x/Contents/MacOS");   // no bundle: the folder itself
    CHECK(GameFolder::FolderOf("C:\\Games\\TE") == "C:\\Games\\TE");
    CHECK(GameFolder::FolderOf("").empty());
    CHECK(GameFolder::IsAbsolute("/x") && GameFolder::IsAbsolute("\\x") && GameFolder::IsAbsolute("C:\\x") && GameFolder::IsAbsolute("C:/x"));
    CHECK(GameFolder::IsAbsolute("ux0:data/twinsanity/mods"));
    CHECK(!GameFolder::IsAbsolute("mods") && !GameFolder::IsAbsolute("./mods") && !GameFolder::IsAbsolute("a/b:c") && !GameFolder::IsAbsolute(""));
    CHECK(GameFolder::Resolve("/g", "cards") == "/g/cards");
    CHECK(GameFolder::Resolve("/g/", "cards") == "/g/cards");
    CHECK(GameFolder::Resolve("/g", "./mods") == "/g/mods");
    CHECK(GameFolder::Resolve("/g", ".") == "/g");
    CHECK(GameFolder::Resolve("/g", "/abs/mods") == "/abs/mods");
    CHECK(GameFolder::Resolve("/g", "D:\\cards") == "D:\\cards");
    CHECK(GameFolder::Resolve("ux0:data/twinsanity", "mods") == "ux0:data/twinsanity/mods");
    CHECK(GameFolder::Resolve("", "x") == "x" && GameFolder::Resolve("/g", "").empty());
    CHECK(GameFolder::FolderOfFile("/g/ps2r.ini") == "/g");
    CHECK(GameFolder::FolderOfFile("C:\\g\\ps2r.ini") == "C:\\g");
    CHECK(GameFolder::FolderOfFile("ps2r.ini") == ".");
    CHECK(GameFolder::FolderOfFile("/ps2r.ini") == "/");
    CHECK(GameFolder::FolderOfFile("ux0:data/twinsanity/ps2r.ini") == "ux0:data/twinsanity");

    const fs::path root = fs::temp_directory_path() / ("te_game_folder_" + std::to_string(::time(nullptr)) + "_" + std::to_string(std::rand()));
    const fs::path data = root / "home" / ".twinsanityevolved";

    // --- portable: ps2r.ini beside the executable
    {
        const fs::path game = root / "game";
        Touch(game / "ps2r.ini");
        CHECK(GameFolder::HasSettings(G(game)) && !GameFolder::HasSettings(G(root / "nowhere")) && !GameFolder::HasSettings(""));
        GameFolder::Inputs in;
        in.exe_dir = G(game);
        in.data_dir = G(data);
        GameFolder::Resolved r = GameFolder::Locate(in);
        CHECK(r.portable && r.folder == G(game) && r.settings == G(game / "ps2r.ini"));
        CHECK(r.memcards == G(game / "memcards") && r.mods == G(game / "mods"));
        CHECK(GameFolder::Describe(r).find("(portable)") != std::string::npos);
        // paths.memcards / paths.mods: relative to the settings file's folder, absolute as given
        GameFolder::ApplySettings(r, in, "cards", "/elsewhere/mods");
        CHECK(r.memcards == G(game / "cards") && r.mods == "/elsewhere/mods");
        // --mc-dir / --mods-dir win over the file for the run
        in.mc_dir_arg = "/run/cards";
        in.mods_dir_arg = "/run/mods";
        r = GameFolder::Locate(in);
        GameFolder::ApplySettings(r, in, "cards", "mymods");
        CHECK(r.portable && r.settings == G(game / "ps2r.ini") && r.memcards == "/run/cards" && r.mods == "/run/mods");
        // --config FILE overrides the settings file (its folder places the mods)
        in = GameFolder::Inputs{};
        in.exe_dir = G(game);
        in.data_dir = G(data);
        in.config_arg = G(root / "other" / "my.ini");
        r = GameFolder::Locate(in);
        CHECK(!r.portable && r.settings == G(root / "other" / "my.ini") && r.folder == G(root / "other") && r.mods == G(root / "other" / "mods"));
        CHECK(r.memcards == G(data / "memcards"));
        // --config none: no settings file, no mods folder unless --mods-dir
        in.config_arg = "none";
        r = GameFolder::Locate(in);
        CHECK(r.settings.empty() && r.folder.empty() && r.mods.empty() && r.memcards == G(data / "memcards"));
        in.mods_dir_arg = "/judge/mods";
        r = GameFolder::Locate(in);
        CHECK(r.settings.empty() && r.mods == "/judge/mods");
    }

    // --- macOS: ps2r.ini beside the .app bundle (not inside it)
    {
        const fs::path mac = root / "mac";
        const fs::path exe = mac / "Twinsanity Evolved.app" / "Contents" / "MacOS";
        fs::create_directories(exe);
        GameFolder::Inputs in;
        in.exe_dir = G(exe);
        in.data_dir = G(data);
        GameFolder::Resolved r = GameFolder::Locate(in);
        CHECK(!r.portable && r.settings == G(data / "ps2r.ini"));   // nothing beside the bundle yet
        Touch(exe / "ps2r.ini");                                     // inside the bundle: not the game folder
        r = GameFolder::Locate(in);
        CHECK(!r.portable);
        Touch(mac / "ps2r.ini");
        r = GameFolder::Locate(in);
        CHECK(r.portable && r.folder == G(mac) && r.settings == G(mac / "ps2r.ini") && r.memcards == G(mac / "memcards") && r.mods == G(mac / "mods"));
    }

    // --- an earlier install: no ps2r.ini beside the game -> the data folder (the settings beside the memory cards)
    {
        GameFolder::Inputs in;
        in.exe_dir = G(root / "bare");
        in.data_dir = G(data);
        GameFolder::Resolved r = GameFolder::Locate(in);
        CHECK(!r.portable && r.memcards == G(data / "memcards") && r.settings == G(data / "ps2r.ini") && r.folder == G(data));
        CHECK(r.mods == G(data / "mods"));
        GameFolder::ApplySettings(r, in, "", "");   // a file without paths.*: nothing moves
        CHECK(r.memcards == G(data / "memcards") && r.mods == G(data / "mods"));
        // --mc-dir alone: the settings file beside those cards (the earlier rule)
        in.mc_dir_arg = G(root / "leg" / "mc");
        r = GameFolder::Locate(in);
        CHECK(r.memcards == G(root / "leg" / "mc") && r.settings == G(root / "leg" / "ps2r.ini") && r.mods == G(root / "leg" / "mods"));
        // a cards path without a folder part: no settings file (as before)
        in.mc_dir_arg = "cards";
        r = GameFolder::Locate(in);
        CHECK(r.settings.empty() && r.mods.empty());
        // nothing known at all (no data folder, no executable): nothing
        r = GameFolder::Locate(GameFolder::Inputs{});
        CHECK(r.settings.empty() && r.memcards.empty() && r.mods.empty() && !r.portable);
    }

    // --- the Vita: the built-in arguments name the card and the settings under ux0:data/twinsanity
    {
        GameFolder::Inputs in;
        in.config_arg = "ux0:data/twinsanity/ps2r.ini";
        in.mc_dir_arg = "ux0:data/twinsanity/memcards";
        GameFolder::Resolved r = GameFolder::Locate(in);
        CHECK(r.settings == "ux0:data/twinsanity/ps2r.ini" && r.folder == "ux0:data/twinsanity");
        CHECK(r.memcards == "ux0:data/twinsanity/memcards" && r.mods == "ux0:data/twinsanity/mods");
        GameFolder::ApplySettings(r, in, "elsewhere", "addons");   // --mc-dir is given: the cards stay; the mods follow the file
        CHECK(r.memcards == "ux0:data/twinsanity/memcards" && r.mods == "ux0:data/twinsanity/addons");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return Finish("test_game_folder");
}
