// test_mods_page.cpp -- te_mods_page.h: the title's Options > MODS page as the platform answers it. With a mods folder of three
// disc mods (one on at start-up) and a language pack, the page's rows are the built-ins ALONE (level select, speedrun mode:
// the mods that apply while the game runs), a row past the page is no mod (no label, no switch, no page, nothing written),
// the folder mods stay in the registry with the switches the start-up read (the -mods card rule, the overlay's layers), a
// built-in's switch applies at once and is persisted, and nothing the page does writes a folder mod's key. Everything
// is written to a temporary folder by the test: no game data.
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ps2e_seam.h"
#include "te_mods.h"
#include "te_mods_page.h"
#include "te_test.h"

namespace fs = std::filesystem;

namespace {

void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << text;
}

std::string Read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

uint32_t IndexOf(const std::string& id) {
    for (uint32_t i = 0; i < te::mods::Count(); ++i)
        if (te::mods::At(i)->id == id) return i;
    return 0xFFFFFFFFu;
}

constexpr uint32_t kScratch = 0x01F00000;   // guest memory the seam's answers are copied into

std::string SeamLabel(int32_t row) {
    ps2e_system_mod_label(ps2e_s32(row), kScratch, 48);
    return std::string(reinterpret_cast<const char*>(te::GuestPointer(kScratch, 48)));
}

}  // namespace

int main() {
    using namespace te_test;
    const fs::path root = fs::temp_directory_path() / ("te_mods_page_" + std::to_string(::time(nullptr)) + "_" + std::to_string(std::rand()));
    const fs::path mods = root / "mods";
    const fs::path ini = root / "ps2r.ini";
    Write(ini, "# the test's settings\nmods.beta.enabled = on\n");
    Write(mods / "alpha" / "mod.json",
          "{ \"schema\": 2, \"id\": \"alpha\", \"kind\": \"disc\", \"name\": \"Ant Agony Remix\", \"version\": \"1.2\",\n"
          "  \"executable\": { \"data_changes\": { \".rodata\": 4 }, \"code_changes\": [ \"FUN_001ac8d0 Font::ConstructBase\" ] } }");
    Write(mods / "alpha" / "disc" / "SLES_525.68", "an executable");
    Write(mods / "beta" / "mod.json", "{ \"schema\": 2, \"id\": \"beta\", \"kind\": \"disc\", \"name\": \"beta\", \"version\": \"1\",\n"
                                      "  \"base\": { \"region\": \"NTSC-U\", \"executable\": \"SLUS_209.09\" } }");
    Write(mods / "beta" / "disc" / "SYSTEM.CNF", "BOOT2 = cdrom0:\\SLES_525.68;1\n");
    Write(mods / "gamma" / "mod.json", "{ \"schema\": 2, \"id\": \"gamma\", \"kind\": \"disc\", \"name\": \"Gamma [v2.0]\", \"version\": \"1\" }");
    Write(mods / "lingo" / "mod.json",
          "{ \"schema\": 2, \"id\": \"lingo\", \"kind\": \"language\", \"name\": \"lingo (text)\", \"version\": \"1\",\n"
          "  \"language\": { \"id\": \"lingo\", \"label\": \"lingo (text)\", \"text_bank\": \"data/lingo.bank\" } }");

    te::Settings s = DefaultSettings();
    s.iso_path = "";
    s.mc_dir = (root / "memcards").string();
    s.settings_path = ini.string();
    Reset(s);
    te::mods::InstallBuiltins();   // level_select, speedrun
    CHECK_EQ(te::mods::ScanFolder(mods.string()), 3u);   // lingo is a language pack: not a mod, kept apart
    te::mods::SetOrder("gamma,beta,alpha");
    te::mods::Configure({"beta"});   // ps2r.ini had mods.beta.enabled = on at start-up (the launcher's switch)
    te::mods::SetDiscRegion("PAL");

    // --- the registry keeps every mod: the built-ins, then the folder's disc mods; only the built-ins apply live
    CHECK_EQ(te::mods::Count(), 5u);
    const uint32_t level_select = IndexOf("level_select"), speedrun = IndexOf("speedrun");
    CHECK(te::mods::AppliesLive(level_select) && te::mods::AppliesLive(speedrun));
    for (const char* id : {"alpha", "beta", "gamma"}) CHECK(IndexOf(id) < te::mods::Count() && !te::mods::AppliesLive(IndexOf(id)));
    CHECK(!te::mods::AppliesLive(te::mods::Count()) && !te::mods::AppliesLive(te::mods::page::kNoMod));
    CHECK(te::mods::Enabled("beta") && te::mods::AnyEnabled());   // on from the start: the -mods card rule counts it (unchanged)

    // --- the page: the built-ins alone, in registration order; a row past them is no mod
    CHECK_EQ(te::mods::page::Count(), 2u);
    CHECK_EQ(ps2e_system_mod_count(), 2);
    CHECK_EQ(te::mods::page::Entry(0), level_select);
    CHECK_EQ(te::mods::page::Entry(1), speedrun);
    for (uint32_t row : {2u, 3u, 4u, 17u}) CHECK_EQ(te::mods::page::Entry(row), te::mods::page::kNoMod);
    CHECK(te::mods::page::Label(0) == "level select" && te::mods::page::Label(1) == "speedrun mode");
    CHECK(SeamLabel(0) == "level select" && SeamLabel(1) == "speedrun mode");
    for (int32_t row : {2, 3, 4, -1}) CHECK(SeamLabel(row).empty());   // the registry's folder mods are past the page
    CHECK_EQ(ps2e_system_mod_has_switch(0), 0);   // level select: its row opens its page
    CHECK_EQ(ps2e_system_mod_has_switch(1), 1);
    CHECK(ps2e_system_mod_page_rows(0) > 0);
    CHECK_EQ(ps2e_system_mod_page_rows(2), 0);
    CHECK_EQ(ps2e_system_mod_page_row_chosen(2, 0), ps2e_s32(te::mods::ActionNone));
    ps2e_system_mod_page_label(3, kScratch, 48);
    CHECK(std::string(reinterpret_cast<const char*>(te::GuestPointer(kScratch, 48))).empty());

    // --- a row past the page switches nothing: beta (the registry's index 3) stays on, nothing is written
    CHECK_EQ(ps2e_system_mod_enabled(3), 0);
    ps2e_system_set_mod_enabled(3, 0);
    ps2e_system_set_mod_enabled(2, 1);
    CHECK(te::mods::Enabled("beta") && !te::mods::Enabled("alpha") && te::mods::AnyEnabled());

    // --- a built-in's switch applies at once and is persisted, as before
    CHECK(!te::mods::Enabled("speedrun") && !te::mods::page::Shown(1));
    ps2e_system_set_mod_enabled(1, 1);
    CHECK(te::mods::Enabled("speedrun") && te::mods::page::Shown(1));
    CHECK_EQ(ps2e_system_mod_enabled(1), 1);
    CHECK(Read(ini).find("mods.speedrun.enabled = on") != std::string::npos);
    te::mods::page::Choose(1, false);
    CHECK(!te::mods::Enabled("speedrun") && Read(ini).find("mods.speedrun.enabled = off") != std::string::npos);
    te::mods::page::Choose(9, true);   // past the page: nothing

    // --- nothing the page did wrote a folder mod's key: the launcher's switch (beta = on) is as it was, the others absent
    const std::string after = Read(ini);
    CHECK(after.find("mods.beta.enabled = on") != std::string::npos);
    CHECK(after.find("mods.alpha") == std::string::npos && after.find("mods.gamma") == std::string::npos);
    CHECK(after.find("mods.beta.enabled = off") == std::string::npos);

    // --- the folder mods are still the overlay's business: in the order, beta (on, a disc tree) the one layer
    const std::vector<te::mods::DiscLayer> layers = te::mods::DiscLayers();
    CHECK(layers.size() == 1 && layers[0].id == "beta");
    CHECK(te::mods::Order() == std::vector<std::string>({"gamma", "beta", "alpha"}));

    // --- no folder: the page is the built-ins, exactly as before
    CHECK_EQ(te::mods::ScanFolder((root / "nowhere").string()), 0u);
    CHECK_EQ(te::mods::Count(), 2u);
    CHECK_EQ(te::mods::page::Count(), 2u);
    CHECK(te::mods::page::Entry(0) == 0 && te::mods::page::Entry(1) == 1 && te::mods::page::Entry(2) == te::mods::page::kNoMod);

    std::error_code ec;
    fs::remove_all(root, ec);
    return Finish("test_mods_page");
}
