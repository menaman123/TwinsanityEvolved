// test_mods_folder.cpp -- te_mods.h's FOLDER MODS (<mods>/<id>/mod.json): the strict JSON reader (mods_json.h), the scan
// (schema 1 and 2, kinds disc / language / builtin, the folders refused and why), te::mods::List(), the switches
// (mods.<id>.enabled through Configure, --mod through SetEnabled), mods.order and the disc layers, the "-mods card while any
// mod is on" rule extended to folder mods, the boot line, the census row and ReadModFile. Everything is written to a
// temporary folder by the test: no game data.
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mods_json.h"
#include "te_mods.h"
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
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool ParseOk(const std::string& text, te::json::Value* out = nullptr) {
    te::json::Value v;
    std::string error;
    const bool ok = te::json::Parse(text, v, &error);
    if (out) *out = v;
    return ok;
}

const te::mods::Info* Row(const std::vector<te::mods::Info>& rows, const std::string& id) {
    for (const auto& r : rows) if (r.id == id) return &r;
    return nullptr;
}

int IndexOf(const std::string& id) {
    for (uint32_t i = 0; i < te::mods::Count(); ++i) if (te::mods::At(i)->id == id) return int(i);
    return -1;
}

}  // namespace

int main() {
    using namespace te_test;

    // --- the JSON reader: what a manifest may hold, and what it may not
    {
        te::json::Value v;
        CHECK(ParseOk("{\"a\": [1, -2.5e3, true, false, null, \"x\"], \"b\": {\"c\": \"\\u00e9\\ud83d\\ude00\\n\\\"\"}}", &v));
        CHECK(v.IsObject() && v.Get("a") && v.Get("a")->items.size() == 6);
        CHECK(v.Get("a")->items[1].number == -2500.0);
        CHECK(v.Get("b")->String("c") == "\xC3\xA9\xF0\x9F\x98\x80\n\"");   // é, U+1F600 (a surrogate pair), newline, quote
        CHECK(v.Unsigned("missing", 7) == 7);
        CHECK(ParseOk("\xEF\xBB\xBF{\"bom\": 1}", &v) && v.Unsigned("bom") == 1);   // a byte-order mark is skipped
        CHECK(ParseOk("  [ ]  ") && ParseOk("{}") && ParseOk("0") && ParseOk("\"\""));
        CHECK(!ParseOk("{\"a\": 1,}"));             // a trailing comma
        CHECK(!ParseOk("{\"a\": 1, \"a\": 2}"));    // a member named twice
        CHECK(!ParseOk("{\"a\": \"\\x\"}"));        // an unknown escape
        CHECK(!ParseOk("{\"a\": \"\\ud83d\"}"));    // a lone surrogate
        CHECK(!ParseOk("// no\n{}"));               // comments
        CHECK(!ParseOk("{} {}"));                   // text after the value
        CHECK(!ParseOk("{\"a\": 01}"));             // a leading zero
        CHECK(!ParseOk("{\"a\": NaN}"));
        CHECK(!ParseOk("[\"tab\there\"]"));         // a raw control character in a string
        CHECK(!ParseOk(std::string(100, '[') + std::string(100, ']')));   // deeper than 64
        std::string error;
        CHECK(!te::json::Parse("{\n  \"a\": tru\n}", v, &error) && error.find("line 2") == 0);
    }

    // --- a mods folder written here
    const fs::path root = fs::temp_directory_path() / ("te_mods_folder_" + std::to_string(::time(nullptr)) + "_" + std::to_string(std::rand()));
    const fs::path mods = root / "mods";
    Write(mods / "hubtest" / "mod.json",
          "{ \"schema\": 2, \"id\": \"hubtest\", \"kind\": \"disc\", \"name\": \"hub test\", \"author\": \"the tests\", \"version\": \"1.2\",\n"
          "  \"description\": \"a disc mod\", \"source\": { \"kind\": \"folder\", \"artefact\": \"hubtest\", \"sha256\": \"ab\" },\n"
          "  \"base\": { \"region\": \"PAL\", \"executable\": \"SLES_525.68\" },\n"
          "  \"files\": [ { \"path\": \"CRASH6/CRASH.BD\", \"form\": \"ranges\", \"bytes\": 123 } ],\n"
          "  \"executable\": { \"data_changes\": { \".data\": 4, \"vu\": 0 }, \"code_changes\": [ \"FUN_001ac8d0 Font::ConstructBase\" ] },\n"
          "  \"future_field\": { \"ignored\": true } }");
    Write(mods / "hubtest" / "disc" / "CRASH6" / "CRASH.BD.ranges", "base 0 16\n");
    Write(mods / "zeta" / "mod.json", "{ \"schema\": 2, \"id\": \"zeta\", \"kind\": \"disc\", \"name\": \"zeta\", \"version\": \"1\" }");
    Write(mods / "zeta" / "disc" / "SYSTEM.CNF", "BOOT2 = cdrom0:\\SLES_525.68;1\n");
    Write(mods / "arabic" / "mod.json",
          "{ \"schema\": 2, \"id\": \"arabic\", \"kind\": \"language\", \"name\": \"arabic\", \"version\": \"0.1\",\n"
          "  \"language\": { \"id\": \"arabic\", \"label\": \"arabic (text)\", \"text_bank\": \"data/arabic.txt.bank\", \"font\": \"data/arabic.font\", \"direction\": \"rtl\" } }");
    Write(mods / "arabic" / "data" / "arabic.txt.bank", "BANK");
    Write(mods / "Bad-Name" / "mod.json", "{ \"schema\": 2, \"id\": \"Bad-Name\", \"kind\": \"disc\" }");
    Write(mods / "broken" / "mod.json", "{ \"schema\": 2, \"id\": \"broken\", }");
    Write(mods / "wrongid" / "mod.json", "{ \"schema\": 2, \"id\": \"other\", \"kind\": \"disc\" }");
    Write(mods / "level_select" / "mod.json", "{ \"id\": \"level_select\", \"name\": \"level select\", \"version\": \"1.0.0\" }");
    Write(mods / "oldbuiltin" / "mod.json", "{ \"id\": \"oldbuiltin\", \"name\": \"x\", \"version\": \"1\" }");
    Write(mods / "futuristic" / "mod.json", "{ \"schema\": 3, \"id\": \"futuristic\", \"kind\": \"disc\" }");
    Write(mods / "nokind" / "mod.json", "{ \"schema\": 2, \"id\": \"nokind\" }");
    Write(mods / "nomanifest" / "disc" / "X.BIN", "x");
    Write(mods / ".hidden" / "mod.json", "{ \"schema\": 2, \"id\": \".hidden\", \"kind\": \"disc\" }");
    Write(mods / "notes.txt", "a file beside the folders");

    te::Settings s = DefaultSettings();
    s.iso_path = "";
    s.mc_dir = (root / "memcards").string();
    Reset(s);
    te::mods::InstallBuiltins();   // level_select, speedrun
    const uint32_t builtins = te::mods::Count();
    CHECK(builtins == 2);

    // --- the scan
    CHECK_EQ(te::mods::ScanFolder(mods.string()), 2u);   // hubtest, zeta (arabic is a language pack: kept apart)
    CHECK(te::mods::Folder() == mods.string());
    CHECK_EQ(te::mods::Count(), builtins + 2);
    std::vector<te::mods::Info> rows = te::mods::List();
    CHECK_EQ(rows.size(), size_t(builtins + 2));
    CHECK(rows.size() == 4 && rows[0].id == "level_select" && rows[1].id == "speedrun" && rows[2].id == "hubtest" && rows[3].id == "zeta");
    CHECK(Row(rows, "arabic") == nullptr);   // a language pack is not a mod: no registry row (the Mods page never shows it)
    for (const char* refused : {"Bad-Name", "broken", "wrongid", "other", "oldbuiltin", "futuristic", "nokind", "nomanifest", ".hidden"})
        CHECK(Row(rows, refused) == nullptr);
    if (const te::mods::Info* b = Row(rows, "level_select")) {
        CHECK(b->kind == te::mods::Kind::Builtin && !b->folder && b->dir.empty() && b->name == "level select" && b->schema == 1);
        CHECK(!b->has_switch);   // the built-in's own registration (opened as its page)
    }
    if (const te::mods::Info* h = Row(rows, "hubtest")) {
        CHECK(h->kind == te::mods::Kind::Disc && h->folder && h->schema == 2 && h->has_disc && h->has_switch && !h->enabled);
        CHECK(h->name == "hub test" && h->author == "the tests" && h->version == "1.2" && h->description == "a disc mod");
        CHECK(h->dir == (mods / "hubtest").string());
        CHECK(h->source.kind == "folder" && h->source.artefact == "hubtest" && h->source.sha256 == "ab");
        CHECK(h->base.region == "PAL" && h->base.executable == "SLES_525.68");
        CHECK(h->files.size() == 1 && h->files[0].path == "CRASH6/CRASH.BD" && h->files[0].form == "ranges" && h->files[0].bytes == 123);
        CHECK(h->data_changes.size() == 2 && h->data_changes[0].first == ".data" && h->data_changes[0].second == 4);
        CHECK(h->code_changes.size() == 1 && h->code_changes[0] == "FUN_001ac8d0 Font::ConstructBase");
        CHECK(h->layer == -1 && h->manifest.find("future_field") != std::string::npos);
    }
    const std::vector<te::mods::Info> packs = te::mods::LanguagePacks();
    CHECK(packs.size() == 1);
    if (packs.size() == 1) {
        const te::mods::Info* a = &packs[0];
        CHECK(a->id == "arabic" && a->kind == te::mods::Kind::Language && a->folder && !a->has_disc && a->dir == (mods / "arabic").string());
        CHECK(a->language.id == "arabic" && a->language.label == "arabic (text)" && a->language.text_bank == "data/arabic.txt.bank" &&
              a->language.font == "data/arabic.font" && a->language.direction == "rtl");
    }
    te::mods::Info one;
    CHECK(te::mods::FindInfo("zeta", &one) && one.kind == te::mods::Kind::Disc && one.name == "zeta");
    CHECK(te::mods::FindInfo("nosuch", &one) == nullptr);
    CHECK(std::string(te::mods::KindName(te::mods::Kind::Language)) == "language");

    // --- nothing on: no layers, the vanilla card, the boot line
    CHECK(te::mods::DiscLayers().empty());
    CHECK(!te::mods::AnyEnabled());
    CHECK(te::mods::DescribeFolders() == "mods: 2 folders (hubtest disc off, zeta disc off), order: none; language packs (not mods): arabic");
    CHECK(te::mods::CensusRow() == "mods=0:0:0");
    te::Services::Get().saves().Core();
    CHECK(!te::Services::Get().saves().UsingModsCard());

    // --- ps2r.ini's mods.hubtest.enabled = on (Configure): a layer, and the -mods card
    te::mods::Configure({"hubtest"});
    CHECK(te::mods::Enabled("hubtest") && !te::mods::Enabled("zeta") && te::mods::AnyEnabled());
    std::vector<te::mods::DiscLayer> layers = te::mods::DiscLayers();
    CHECK(layers.size() == 1 && layers[0].id == "hubtest" && layers[0].dir == (mods / "hubtest").string());
    CHECK(te::mods::DescribeFolders() == "mods: 2 folders (hubtest disc on, zeta disc off), order: hubtest; language packs (not mods): arabic");
    te::Services::Get().saves().Core();
    CHECK(te::Services::Get().saves().UsingModsCard());
    CHECK(te::Services::Get().saves().CardDirectory() == (root / "memcards").string() + "-mods");
    CHECK(te::mods::Describe().find("hubtest=on") != std::string::npos && te::mods::Describe().find("-mods card") != std::string::npos);

    // --- --mod zeta (on for the run) and the order: sorted by id when mods.order is absent, mods.order first otherwise
    const int zeta = IndexOf("zeta");
    CHECK(zeta >= 0);
    te::mods::SetEnabled(uint32_t(zeta), true, false);
    layers = te::mods::DiscLayers();
    CHECK(layers.size() == 2 && layers[0].id == "hubtest" && layers[1].id == "zeta");
    te::mods::SetOrder("zeta, hubtest");
    layers = te::mods::DiscLayers();
    CHECK(layers.size() == 2 && layers[0].id == "zeta" && layers[1].id == "hubtest");
    CHECK(te::mods::Order().size() == 2 && te::mods::Order()[0] == "zeta" && te::mods::Order()[1] == "hubtest");
    te::mods::SetOrder("nosuch,level_select,arabic,hubtest");   // unknown ids, built-ins and language packs are not overlay layers: ignored
    layers = te::mods::DiscLayers();
    CHECK(layers.size() == 2 && layers[0].id == "hubtest" && layers[1].id == "zeta");
    te::mods::SetOrder("");
    CHECK(te::mods::DescribeFolders() == "mods: 2 folders (hubtest disc on, zeta disc on), order: hubtest,zeta; language packs (not mods): arabic");
    // a language pack never counts for the -mods card: it has no switch -- an earlier version's mods.arabic.enabled = on
    // (Configure) changes nothing, and it is no layer
    te::mods::SetEnabled(uint32_t(zeta), false, false);
    te::mods::SetEnabled(uint32_t(IndexOf("hubtest")), false, false);
    CHECK(!te::mods::AnyEnabled());
    CHECK(IndexOf("arabic") < 0);
    te::mods::Configure({"arabic"});
    CHECK(!te::mods::AnyEnabled() && te::mods::DiscLayers().empty() && !te::mods::Enabled("arabic"));
    te::Services::Get().saves().Core();
    CHECK(!te::Services::Get().saves().UsingModsCard());
    // a language pack's stale lines dropped from a settings file: its switch, its place in mods.order (every other line kept)
    {
        const fs::path ini = root / "stale.ini";
        Write(ini, "# the player's\nmods.arabic.enabled = on\nmods.order = arabic,hubtest\nvideo.scale = 2\n");
        te::mods::DropPackKeys(ini.string());
        std::string text = Read(ini);
        CHECK(text == "# the player's\nmods.order = hubtest\nvideo.scale = 2\n");
        Write(ini, "mods.order = arabic\nmods.hubtest.enabled = on\n");
        te::mods::DropPackKeys(ini.string());
        text = Read(ini);
        CHECK(text == "mods.hubtest.enabled = on\n");
    }

    // --- the census row as the disc reports it
    te::mods::NoteOverlay({"hubtest"}, 2, [] { return uint64_t(5); });
    CHECK(te::mods::CensusRow() == "mods=1:2:5");
    rows = te::mods::List();
    CHECK(Row(rows, "hubtest") && Row(rows, "hubtest")->layer == 0 && Row(rows, "zeta")->layer == -1);

    // --- a folder mod's own files
    std::vector<uint8_t> bytes;
    std::string error;
    CHECK(te::mods::ReadModFile("arabic", "data/arabic.txt.bank", bytes, &error) && std::string(bytes.begin(), bytes.end()) == "BANK");   // a pack's own files
    CHECK(te::mods::ReadModFile("arabic", "data\\arabic.txt.bank", bytes, &error) && bytes.size() == 4);
    CHECK(!te::mods::ReadModFile("arabic", "../hubtest/mod.json", bytes, &error) && error.find("inside") != std::string::npos);
    CHECK(!te::mods::ReadModFile("arabic", "/etc/hosts", bytes, &error));
    CHECK(!te::mods::ReadModFile("arabic", "C:/x", bytes, &error));
    CHECK(!te::mods::ReadModFile("level_select", "mod.json", bytes, &error));   // a built-in has no folder
    CHECK(!te::mods::ReadModFile("arabic", "data/missing.bin", bytes, &error));

    // --- a second scan forgets the first folder's mods; an absent folder registers none
    CHECK_EQ(te::mods::ScanFolder((root / "nowhere").string()), 0u);
    CHECK_EQ(te::mods::Count(), builtins);
    CHECK(te::mods::LanguagePacks().empty());
    CHECK(te::mods::DescribeFolders() == "mods: 0 folders, order: none");
    CHECK(te::mods::CensusRow() == "mods=0:0:0");

    std::error_code ec;
    fs::remove_all(root, ec);
    return Finish("test_mods_folder");
}
