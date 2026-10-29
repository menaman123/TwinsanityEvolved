// test_region_map.cpp -- engine/app/host/disc_regions: the region table (PAL native, NTSC-U / NTSC-J needing a map, the
// readable refusal), the region map's reader, and the loader through a map over GENERATED executables (a tiny "PAL" ELF
// and a copy of it with every section moved and its pointers rebased: no game data) -- the image laid out through the map
// is byte for byte the native one; the archive index rename of a map's file names; the built-in maps read lazily. With
// TE_TEST_ISO (a PAL disc) and TE_TEST_REGION_MAP_DIR (a folder where tools/discs/elf_region_map.py wrote
// identity.json, shifted.elf + shifted_map.json and -- with a North American disc -- ntsc.elf + ntsc_map.json: the region
// map judge makes it), the same over the REAL executables: each image rebuilt through the tool's map differs from the
// native load in exactly the bytes the map lists, and the C++ map maker writes the same file byte for byte.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/host/disc_regions.h"
#include "app/host/region_map_make.h"
#include "engine/cdvd/iso_fs.h"
#include "te_test.h"

namespace {
using ps2r::app::RegionMap;

void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (8 * i));
}
void Put16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
    v[at] = uint8_t(x);
    v[at + 1] = uint8_t(x >> 8);
}
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// A one-PT_LOAD MIPS ELF whose file bytes start at `vaddr`: `body` (code then data), memsz = body + bss.
std::vector<uint8_t> MakeElf(uint32_t vaddr, uint32_t entry, const std::vector<uint8_t>& body, uint32_t bss) {
    std::vector<uint8_t> e(0x1000 + body.size(), 0);
    e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 1; e[6] = 1;
    Put16(e, 16, 2);
    Put16(e, 18, 8);
    Put32(e, 20, 1);
    Put32(e, 24, entry);
    Put32(e, 28, 52);   // phoff
    Put16(e, 40, 52);
    Put16(e, 42, 32);
    Put16(e, 44, 1);
    const size_t ph = 52;
    Put32(e, ph, 1);
    Put32(e, ph + 4, 0x1000);
    Put32(e, ph + 8, vaddr);
    Put32(e, ph + 12, vaddr);
    Put32(e, ph + 16, uint32_t(body.size()));
    Put32(e, ph + 20, uint32_t(body.size()) + bss);
    std::memcpy(e.data() + 0x1000, body.data(), body.size());
    return e;
}

std::string Hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

void Table() {
    const ps2r::app::RegionEntry* pal = ps2r::app::RegionEntryFor("SLES_525.68");
    CHECK(pal && pal->native && pal->region == ps2r::app::DiscIdentity::Region::PAL && pal->offers_50 && pal->default_hz == 60u);
    CHECK(pal && pal->image_start == 0x002D9D90u && pal->bss_start == 0x0030A480u && pal->image_end == 0x003DB200u && pal->entry == 0x00100008u);
    const ps2r::app::RegionEntry* us = ps2r::app::RegionEntryFor("SLUS-20909");
    CHECK(us && !us->native && us->region == ps2r::app::DiscIdentity::Region::NTSC_U && !us->offers_50 && us->native_hz == 60u);
    CHECK(us && us->executable == "SLUS_209.09" && us->map_file == "region_map_SLUS-20909.json" && us->elf_bytes == 2151940u);
    const ps2r::app::RegionEntry* jp = ps2r::app::RegionEntryFor("slpm 65801");
    CHECK(jp && !jp->native && jp->region == ps2r::app::DiscIdentity::Region::NTSC_J && jp->map_file == "region_map_SLPM-65801.json");
    CHECK(ps2r::app::RegionEntryFor("SLES-99999") == nullptr);
    ps2r::app::ClearRegionMaps();
    CHECK(ps2r::app::RegionNotPlayable("SLES-52568").empty());
    CHECK(ps2r::app::RegionNotPlayable("SLUS-20909") ==
          "This is the North American disc (SLUS-20909). The port runs the European game's code and needs this disc's address map, "
          "which has not been made yet: run twinsanityevolved-modtool region-map --pal <the European disc> --disc <this disc> once "
          "(or tools/discs/elf_region_map.py map with the two discs), and put the map it writes (region_map_SLUS-20909.json) beside the game.");
    CHECK(ps2r::app::RegionNotPlayable("SLPM-65801").rfind("This is the Japanese disc (SLPM-65801).", 0) == 0);
    CHECK(ps2r::app::RegionNotPlayable("SLES-99999").empty());
}

void Synthetic() {
    // "PAL": code at 0x100000 (0x100 bytes), data at 0x100100 (0x80 words: code pointers, data pointers, numbers), bss 0x40
    const uint32_t base = 0x00100000, data = base + 0x100, n = 0x80, bss = data + n * 4;
    std::vector<uint8_t> body(0x100 + n * 4, 0);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t w = 1000 + i;                                // a number
        if (i % 4 == 0) w = base + 0x10 * (i % 16);           // a code pointer
        if (i % 4 == 1) w = data + 4 * ((i * 5) % n);         // a data pointer
        if (i % 4 == 2 && i % 8 == 2) w = bss + 8;            // a pointer into the bss
        Put32(body, 0x100 + 4 * i, w);
    }
    const std::vector<uint8_t> pal = MakeElf(base, base + 8, body, 0x40);
    // the same program moved by +0x1000: every pointer word rebased
    const uint32_t shift = 0x1000;
    std::vector<uint8_t> moved_body = body;
    std::vector<uint32_t> pointer_words;
    std::vector<std::pair<uint32_t, uint32_t>> code;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t w = Get32(&body[0x100 + 4 * i]);
        if (w >= base && w < bss + 0x40) {
            Put32(moved_body, 0x100 + 4 * i, w + shift);
            pointer_words.push_back(data + 4 * i);
            if (w < data) code.emplace_back(w + shift, w);
        }
    }
    std::sort(code.begin(), code.end());
    code.erase(std::unique(code.begin(), code.end()), code.end());
    const std::vector<uint8_t> moved = MakeElf(base + shift, base + 8 + shift, moved_body, 0x40);
    std::string json = "{\"schema\": 1, \"kind\": \"region-map\", \"serial\": \"SLUS-20909\", \"executable\": \"SLUS_209.09\", \"elf\": {\"sha256\": \"" +
                       ps2r::app::Sha256OfBytes(moved) + "\", \"bytes\": " + std::to_string(moved.size()) + ", \"entry\": \"" + Hex(base + 8 + shift) +
                       "\"}, \"image\": {\"pal_start\": \"" + Hex(data) + "\", \"pal_bss_start\": \"" + Hex(bss) + "\", \"pal_end\": \"" + Hex(bss + 0x40) +
                       "\"}, \"segments\": [{\"pal\": \"" + Hex(data) + "\", \"other\": \"" + Hex(data + shift) + "\", \"bytes\": " + std::to_string(n * 4) +
                       "}], \"bss\": [{\"pal\": \"" + Hex(bss) + "\", \"other\": \"" + Hex(bss + shift) + "\", \"bytes\": 64}], \"pointers\": [";
    for (size_t i = 0; i < pointer_words.size(); ++i) json += (i ? ", \"" : "\"") + Hex(pointer_words[i]) + "\"";
    json += "], \"code\": [";
    for (size_t i = 0; i < code.size(); ++i) json += (i ? ", [\"" : "[\"") + Hex(code[i].first) + "\", \"" + Hex(code[i].second) + "\"]";
    json += "]}";
    RegionMap map;
    std::string error;
    CHECK(ps2r::app::ParseRegionMap(json, &map, &error));
    if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
    CHECK(map.segments.size() == 1 && map.bss.size() == 1 && map.pointers.size() == pointer_words.size() && map.code.size() == code.size());
    std::vector<uint8_t> want(0x200000, 0xAA), got(0x200000, 0x55);
    CHECK(ps2r::app::LoadImageNative(pal, data, bss, bss + 0x40, want.data(), want.size(), &error));
    ps2r::app::MapLoadReport r;
    CHECK(ps2r::app::LoadImageThroughMap(moved, map, got.data(), got.size(), &r, &error));
    CHECK(std::memcmp(want.data() + data, got.data() + data, bss + 0x40 - data) == 0);
    CHECK_EQ(r.pointers, uint32_t(pointer_words.size()));
    CHECK_EQ(r.unmapped, 0u);
    CHECK(r.by_code > 0 && r.by_range > 0 && r.segments == 1u);
    // the map refuses another executable (the PAL one itself), and a map that does not read
    CHECK(!ps2r::app::LoadImageThroughMap(pal, map, got.data(), got.size(), &r, &error));
    CHECK(error.rfind("the region map was made for another executable", 0) == 0);
    CHECK(!ps2r::app::ParseRegionMap("{\"kind\": \"files\"}", &map, &error));
    CHECK(!ps2r::app::ParseRegionMap("{\"kind\": \"region-map\", \"elf\": {\"sha256\": \"x\"}, \"image\": {}}", &map, &error));
    // registered: the NTSC-U disc plays, and the map is found by its executable's hash
    ps2r::app::ClearRegionMaps();
    CHECK(ps2r::app::ParseRegionMap(json, &map, &error));
    ps2r::app::AddRegionMap(map);
    CHECK(ps2r::app::HasRegionMap("SLUS_209.09") && ps2r::app::RegionNotPlayable("SLUS-20909").empty());
    CHECK(ps2r::app::FindRegionMap("SLUS-20909", ps2r::app::Sha256OfBytes(moved)) != nullptr);
    CHECK(ps2r::app::FindRegionMap("SLUS-20909", ps2r::app::Sha256OfBytes(pal)) == nullptr);
    ps2r::app::ClearRegionMaps();
}

// The matching blocks of the C++ map maker against python3 difflib.SequenceMatcher(None, a, b, autojunk=True) on the same
// sequences (an LCG; the expected blocks printed by difflib itself): a hand case, a short alphabet where every element is
// popular (no match at all: autojunk), and an insertion in a 500-element sequence.
std::vector<uint64_t> Lcg(size_t n, uint32_t seed, uint32_t mod) {
    std::vector<uint64_t> out;
    uint32_t x = seed;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        out.push_back((x >> 16) % mod);
    }
    return out;
}

void Matching() {
    using ps2r::app::MatchBlock;
    auto same = [](const std::vector<MatchBlock>& got, const std::vector<MatchBlock>& want) {
        if (got.size() != want.size()) return false;
        for (size_t i = 0; i < got.size(); ++i)
            if (got[i].a != want[i].a || got[i].b != want[i].b || got[i].n != want[i].n) return false;
        return true;
    };
    CHECK(same(ps2r::app::MatchingBlocks({1, 2, 3, 4, 5}, {1, 2, 9, 3, 4, 5}), {{0, 0, 2}, {2, 3, 3}, {5, 6, 0}}));
    CHECK(same(ps2r::app::MatchingBlocks(Lcg(300, 1, 5), Lcg(300, 2, 5)), {{300, 300, 0}}));
    const std::vector<uint64_t> a = Lcg(500, 3, 40);
    std::vector<uint64_t> b(a.begin(), a.begin() + 200);
    b.insert(b.end(), {999, 998, 997});
    b.insert(b.end(), a.begin() + 200, a.begin() + 450);
    b.insert(b.end(), a.begin() + 460, a.end());
    CHECK(same(ps2r::app::MatchingBlocks(a, b), {{0, 0, 200}, {200, 203, 250}, {500, 493, 0}}));
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    out->clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

std::string ReadText(const std::string& path) {
    std::vector<uint8_t> b;
    if (!ReadFile(path, &b)) return {};
    return std::string(b.begin(), b.end());
}

// The archive index rename the loader does for a map's file names (CRASH6/CRASH.BH's form: s32 header word, then
// {s32 length, name, u32 offset, u32 size}): the entry named the other way takes the PAL name, offsets and sizes kept,
// nothing renamed onto a name the index has already
void ArchiveRename() {
    auto entry = [](std::vector<uint8_t>& v, const std::string& name, uint32_t off, uint32_t size) {
        const size_t at = v.size();
        v.resize(at + 4 + name.size() + 8);
        Put32(v, at, uint32_t(name.size()));
        std::memcpy(&v[at + 4], name.data(), name.size());
        Put32(v, at + 4 + name.size(), off);
        Put32(v, at + 8 + name.size(), size);
    };
    std::vector<uint8_t> bh(4, 0);
    Put32(bh, 0, 0x501);
    entry(bh, "Startup\\Fonts\\Crash.psf", 100, 2000);
    entry(bh, "Startup\\Default.rm2", 3000, 50);
    std::vector<RegionMap::Name> names{{"StartUp\\Fonts\\Crash_Euro", "StartUp\\Fonts\\Crash", 0x2F49B0u}};
    std::vector<uint8_t> out;
    std::vector<std::string> renamed;
    std::string why;
    CHECK(ps2r::app::RenameArchiveEntries(bh, names, &out, &renamed, &why));
    CHECK(renamed.size() == 1u && renamed[0] == "Startup\\Fonts\\Crash.psf -> StartUp\\Fonts\\Crash_Euro.psf");
    CHECK_EQ(out.size(), bh.size() + 5);
    CHECK(Get32(&out[0]) == 0x501u && Get32(&out[4]) == uint32_t(std::strlen("StartUp\\Fonts\\Crash_Euro.psf")));
    const size_t n = std::strlen("StartUp\\Fonts\\Crash_Euro.psf");
    CHECK(std::memcmp(&out[8], "StartUp\\Fonts\\Crash_Euro.psf", n) == 0 && Get32(&out[8 + n]) == 100u && Get32(&out[12 + n]) == 2000u);
    // the PAL name already there: nothing renamed
    std::vector<uint8_t> both = bh;
    entry(both, "StartUp\\Fonts\\Crash_Euro.psf", 5000, 10);
    renamed.clear();
    CHECK(ps2r::app::RenameArchiveEntries(both, names, &out, &renamed, &why));
    CHECK(renamed.empty() && out == both);
    // an index that does not read
    std::vector<uint8_t> bad(bh.begin(), bh.end() - 3);
    CHECK(!ps2r::app::RenameArchiveEntries(bad, names, &out, &renamed, &why) && !why.empty());
}

// A built-in map is kept as text and read only when a disc of its serial needs it
void LazyMaps() {
    ps2r::app::ClearRegionMaps();
    static const char* text = "{\"schema\": 2, \"kind\": \"region-map\", \"serial\": \"SLUS-20909\", \"executable\": \"SLUS_209.09\", "
                              "\"elf\": {\"sha256\": \"0000000000000000000000000000000000000000000000000000000000000000\", \"entry\": 1048584}, "
                              "\"image\": {\"pal_start\": 2989456, \"pal_bss_start\": 3187840, \"pal_end\": 4043264}, \"segments\": [], \"bss\": [], "
                              "\"pointers\": [], \"names\": [{\"pal\": \"A\\\\B_Euro\", \"other\": \"A\\\\B\", \"at\": \"0x002F49B0\"}]}";
    ps2r::app::AddRegionMapText(text);
    CHECK(ps2r::app::RegionMaps().empty());                       // not read yet
    CHECK(ps2r::app::HasRegionMap("SLUS-20909") && ps2r::app::RegionNotPlayable("SLUS-20909").empty());
    CHECK(!ps2r::app::HasRegionMap("SLPM-65801"));
    ps2r::app::ParsePendingRegionMaps("SLES-52568");              // another serial: still not read
    CHECK(ps2r::app::RegionMaps().empty());
    CHECK(ps2r::app::FindRegionMap("SLUS-20909", std::string(64, '0')) != nullptr);   // read on demand
    CHECK(ps2r::app::RegionMaps().size() == 1u && ps2r::app::RegionMaps()[0].names.size() == 1u &&
          ps2r::app::RegionMaps()[0].names[0].pal == "A\\B_Euro" && ps2r::app::RegionMaps()[0].schema == 2u);
    CHECK(ps2r::app::DiscTableKind(text) == "region-map");
    ps2r::app::ClearRegionMaps();
}

void Real(const std::string& iso, const std::string& dir) {
    IsoFs fs;
    IsoFs::Entry e;
    CHECK(fs.Open(iso) && fs.Find("SLES_525.68", e));
    std::vector<uint8_t> pal(e.size);
    CHECK(fs.ReadBytes(uint64_t(e.lba) * IsoFs::kSectorSize, e.size, pal.data()) == e.size);
    const ps2r::app::RegionEntry* row = ps2r::app::RegionEntryFor("SLES-52568");
    CHECK(row && ps2r::app::Sha256OfBytes(pal) == row->elf_sha256);
    std::vector<uint8_t> want(32u << 20, 0), got(32u << 20, 0xCD);
    std::string error;
    CHECK(ps2r::app::LoadImageNative(pal, row->image_start, row->bss_start, row->image_end, want.data(), want.size(), &error));
    const uint32_t lo = row->image_start, hi = row->image_end;
    const std::string symbols = ReadText(std::string(TE_SOURCE_DIR) + "/game/symbol_addrs.txt");
    CHECK(!symbols.empty());
    // every map the Python tool wrote into the folder: the loader lays the image out through it with exactly the differing
    // bytes the map lists, and the C++ map maker (the mod tool's region-map) writes the SAME FILE from the same executables
    struct Case { const char* what; const char* map; const char* elf; const char* serial; const char* exe; };
    const Case cases[] = {{"PAL against PAL", "identity.json", "", "SLES-52568", "SLES_525.68"},
                          {"the moved executable", "shifted_map.json", "shifted.elf", "SLES-52568", "SHIFTED.ELF"},
                          {"the North American executable", "ntsc_map.json", "ntsc.elf", "SLUS-20909", "SLUS_209.09"}};
    for (const Case& c : cases) {
        const std::string text = ReadText(dir + "/" + c.map);
        if (text.empty()) {
            std::fprintf(stderr, "  %s: no %s in the folder (skipped)\n", c.what, c.map);
            continue;
        }
        std::vector<uint8_t> other = pal;
        if (*c.elf) CHECK(ReadFile(dir + "/" + c.elf, &other));
        RegionMap map;
        CHECK(ps2r::app::ParseRegionMap(text, &map, &error));
        ps2r::app::MapLoadReport r;
        std::fill(got.begin(), got.end(), 0xCD);
        CHECK(ps2r::app::LoadImageThroughMap(other, map, got.data(), got.size(), &r, &error));
        size_t differ = 0;
        for (uint32_t a = lo; a < hi; ++a) differ += want[a] != got[a];
        const size_t listed = size_t(std::strtoull(text.substr(text.find("\"differing_bytes\": ") + 19).c_str(), nullptr, 10));
        std::fprintf(stderr, "  %s through the tool's map: %u segments, %llu bytes, %u pointers (%u code, %u data, %u by range, %u unmapped), %u fills "
                             "-- %zu of %u image bytes differ (the map lists %zu)\n",
                     c.what, r.segments, (unsigned long long)r.bytes_copied, r.pointers, r.by_code, r.by_data, r.by_range, r.unmapped, r.fills, differ,
                     hi - lo, listed);
        CHECK_EQ(differ, listed);
        CHECK_EQ(r.unmapped, 0u);
        std::string json;
        ps2r::app::RegionMapMakeReport made;
        ps2r::app::RegionMapMakeOptions o;
        o.serial = c.serial;
        o.executable = c.exe;
        o.symbols_text = symbols;
        CHECK(ps2r::app::MakeRegionMap(pal, other, o, &json, &made, &error));
        const bool same = json == text;
        size_t first = 0;
        while (first < json.size() && first < text.size() && json[first] == text[first]) ++first;
        std::fprintf(stderr, "  the C++ map maker, %s: %zu bytes -- %s the Python tool's file%s\n", c.what, json.size(),
                     same ? "byte for byte" : "NOT", same ? "" : (" (first difference at byte " + std::to_string(first) + ")").c_str());
        CHECK(same);
    }
}
}  // namespace

int main(int argc, char** argv) {
    // the mod tool's sub-command, driven here until the mod tool calls it: test_region_map region-map --pal ... --disc ...
    if (argc > 1 && std::strcmp(argv[1], "region-map") == 0) return ps2r::app::RegionMapCommand(argc - 1, argv + 1);
    Table();
    Matching();
    Synthetic();
    ArchiveRename();
    LazyMaps();
    const std::string iso = te_test::FixtureIso();
    const char* dir = std::getenv("TE_TEST_REGION_MAP_DIR");
    if (iso.empty() || !dir || !*dir) std::fprintf(stderr, "test_region_map: TE_TEST_ISO / TE_TEST_REGION_MAP_DIR not set -- the real executable's checks are SKIPPED\n");
    else Real(iso, dir);
    return te_test::Finish("test_region_map");
}
