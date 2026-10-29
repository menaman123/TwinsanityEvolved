// test_disc_identity.cpp -- engine/app/host/disc_identity over GENERATED ISO 9660 images (byte patterns and a fake ELF
// header: no game data) and a table of known dumps the test writes for them: retail (size + CRC-32 + SHA-1), retail by
// size (not hashed), modified (another size; the same size and other bytes), the differing file named only against a
// list of RETAIL provenance, an unknown program (unknown but plausible), a demo serial refused, the core files missing,
// not a PlayStation 2 disc, not a disc image, the tables' readers, the CRC-32 / SHA-1 vectors -- and, when TE_TEST_ISO
// names the PAL disc used for development, over that disc with the repo's own tables (launcher/discs/redump.json + pal.json): a
// MODIFIED PAL image, 2,579,234,816 bytes vs the retail dump's 2,558,263,296, its whole-image CRC-32 and SHA-1 as
// published for this image (bc22864a / cf69ffd7...). A one-byte-modified copy of that disc is not made: the disc is a
// modified image already, and no file list of retail provenance exists to name a changed file against.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "app/host/disc_identity.h"
#include "app/host/disc_image_hash.h"
#include "engine/core/hash/sha256.h"
#include "te_test.h"
#include "te_test_iso.h"

namespace {
using ps2r::app::DiscIdentity;
using Verdict = ps2r::app::DiscIdentity::Verdict;
namespace fs = std::filesystem;

std::vector<uint8_t> Bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<uint8_t> FakeElf(uint32_t entry, size_t n) {
    std::vector<uint8_t> v = te_test::Pattern(n, 9);
    const uint8_t head[20] = {0x7F, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 8, 0};
    std::memcpy(v.data(), head, sizeof head);
    for (int i = 0; i < 4; ++i) v[24 + i] = uint8_t(entry >> (8 * i));
    return v;
}

std::string Hex(const std::vector<uint8_t>& v) { return ps2r::hash::Sha256::HexOf(v.data(), v.size()); }

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

// A file list over `files` (the ISO's own layout), `core` naming the core rows, `retail` its provenance.
std::string ListJson(const std::string& id, const std::string& exe, const std::vector<te_test::IsoFile>& files,
                     const std::set<std::string>& core, bool retail) {
    std::string j = "{\"schema\": 2, \"kind\": \"files\", \"id\": \"" + id + "\", \"title\": \"Test Title\", \"region\": \"PAL\", \"serial\": \"" +
                    ps2r::app::SerialOf(exe) + "\", \"executable\": \"" + exe + "\", \"retail\": " + (retail ? "true" : "false") +
                    ", \"provenance\": \"" + (retail ? "the retail dump" : "a test disc") + "\", \"elf\": {\"entry\": \"0x00100008\"}, \"files\": [";
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string path = (files[i].dir.empty() ? "" : files[i].dir + "/") + files[i].name;
        j += std::string(i ? "," : "") + "{\"path\": \"" + path + "\", \"bytes\": " + std::to_string(files[i].bytes.size()) +
             ", \"sha256\": \"" + Hex(files[i].bytes) + "\", \"core\": " + (core.count(path) ? "true" : "false") + "}";
    }
    return j + "]}";
}

// One row of a table of known dumps for the image at `path` (its size, CRC-32 and SHA-1), or with `bytes` given, a dump of
// another size.
std::string DumpRow(uint32_t id, const std::string& serial, const std::string& version, const std::string& path, uint64_t bytes = 0,
                    bool game = true) {
    const std::vector<uint8_t> image = ReadFile(path);
    const uint64_t size = bytes ? bytes : image.size();
    char crc[16];
    std::snprintf(crc, sizeof crc, "%08x", ps2r::app::Crc32::Of(image.data(), image.size()));
    return "{\"id\": " + std::to_string(id) + ", \"title\": \"Test Title\", \"serial\": \"" + serial + "\", \"region\": \"" +
           (serial.rfind("SLUS", 0) == 0 ? "NTSC-U" : "PAL") + "\", \"version\": \"" + version + "\", \"edition\": \"Original\", \"bytes\": " +
           std::to_string(size) + ", \"crc32\": \"" + crc + "\", \"sha1\": \"" + ps2r::app::Sha1::HexOf(image.data(), image.size()) +
           "\"" + (game ? "" : ", \"game\": false, \"why\": \"a demo disc\"") + "}";
}

// A known retail program row: the serial, the executable, the sha256 of its bytes.
std::string ProgramRow(const std::string& serial, const std::string& exe, const std::vector<uint8_t>& bytes) {
    return "{\"serial\": \"" + serial + "\", \"executable\": \"" + exe + "\", \"bytes\": " + std::to_string(bytes.size()) +
           ", \"sha256\": \"" + Hex(bytes) + "\", \"source\": \"the test\"}";
}

std::string DumpTable(const std::vector<std::string>& rows, const std::vector<std::string>& programs = {}) {
    std::string j = "{\"schema\": 1, \"kind\": \"redump\", \"dumps\": [";
    for (size_t i = 0; i < rows.size(); ++i) j += (i ? "," : "") + rows[i];
    j += "], \"programs\": [";
    for (size_t i = 0; i < programs.size(); ++i) j += (i ? "," : "") + programs[i];
    return j + "]}";
}

void Register(const std::string& table) {
    std::string error;
    const bool ok = ps2r::app::RegisterDiscTable(table, &error);
    CHECK(ok);
    if (!ok) std::fprintf(stderr, "  %s\n", error.c_str());
}

// The same volume as a raw 2352-byte-sector image (.bin): sync, MSF header, Mode 1, the 2048 user bytes, EDC/ECC zeros.
std::string ToRaw(const std::string& iso) {
    FILE* in = std::fopen(iso.c_str(), "rb");
    if (!in) return {};
    const std::string raw = iso + ".bin";
    FILE* out = std::fopen(raw.c_str(), "wb");
    std::vector<uint8_t> sector(2048);
    uint32_t lba = 0;
    while (out && std::fread(sector.data(), 1, sector.size(), in) == sector.size()) {
        uint8_t head[16] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0, 0, 0, 0x01};
        const uint32_t f = lba + 150;
        auto bcd = [](uint32_t v) { return uint8_t(((v / 10) << 4) | (v % 10)); };
        head[12] = bcd(f / (75 * 60));
        head[13] = bcd((f / 75) % 60);
        head[14] = bcd(f % 75);
        const std::vector<uint8_t> tail(288, 0);
        std::fwrite(head, 1, sizeof head, out);
        std::fwrite(sector.data(), 1, sector.size(), out);
        std::fwrite(tail.data(), 1, tail.size(), out);
        ++lba;
    }
    std::fclose(in);
    if (out) std::fclose(out);
    return out ? raw : std::string();
}

bool Contains(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v) if (x == s) return true;
    return false;
}

void Hashes() {
    using ps2r::app::Crc32;
    using ps2r::app::Sha1;
    CHECK_EQ(Crc32::Of("", 0), 0u);
    CHECK_EQ(Crc32::Of("123456789", 9), 0xCBF43926u);
    CHECK(Crc32::Hex(Crc32::Of("abc", 3)) == "352441c2");
    CHECK(Sha1::HexOf("", 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(Sha1::HexOf("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    const std::string million(1000000, 'a');
    CHECK(Sha1::HexOf(million.data(), million.size()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    CHECK_EQ(Crc32::Of(million.data(), million.size()), 0xdc25bfbcu);
    // streamed in uneven pieces: the same digests (python3 zlib / hashlib: 1ff605c6, ee530133...)
    std::vector<uint8_t> big;
    for (int r = 0; r < 4099; ++r) for (int b = 0; b < 256; ++b) big.push_back(uint8_t(b));
    Crc32 c;
    Sha1 s;
    size_t at = 0, step = 1;
    while (at < big.size()) {
        const size_t n = std::min(step, big.size() - at);
        c.Update(big.data() + at, n);
        s.Update(big.data() + at, n);
        at += n;
        step = step * 3 + 7;
    }
    uint8_t d[Sha1::kDigestBytes];
    s.Finish(d);
    CHECK_EQ(c.Value(), 0x1ff605c6u);
    CHECK(Sha1::Hex(d) == "ee530133b4a64868051d615d148a51143053b615");
    CHECK(ps2r::app::Thousands(2579234816ull) == "2,579,234,816" && ps2r::app::Thousands(999) == "999" && ps2r::app::Thousands(1000) == "1,000");
}

void Synthetic() {
    using namespace te_test;
    const std::vector<IsoFile> retail = {
        {"", "SYSTEM.CNF", Bytes("BOOT2 = cdrom0:\\SLES_525.68;1 \r\nVER = 1.01\r\nVMODE = PAL\r\n")},
        {"", "SLES_525.68", FakeElf(0x00100008, 6000)},
        {"CRASH6", "CRASH.BH", Pattern(3000, 1)},
        {"CRASH6", "CRASH.BD", Pattern(70000, 2)},
        {"CRASH6", "ENGLISH.MH", Pattern(100, 3)},
    };
    const std::set<std::string> core = {"SYSTEM.CNF", "SLES_525.68", "CRASH6/CRASH.BH", "CRASH6/CRASH.BD"};
    std::string error;
    ps2r::app::ClearRetailManifests();
    ps2r::app::ClearKnownDumps();
    // a list of THIS disc's files (not retail): it supplies the core files, never a difference
    ps2r::app::RetailManifest m;
    CHECK(ps2r::app::ParseRetailManifest(ListJson("test-pal", "SLES_525.68", retail, core, false), &m, &error));
    CHECK(m.files.size() == 5 && m.executable == "SLES_525.68" && m.elf_entry == 0x00100008u && m.region == DiscIdentity::Region::PAL);
    CHECK(!m.retail && m.provenance == "a test disc");
    ps2r::app::AddRetailManifest(m);

    const std::string iso = WriteTestIso(retail, "TESTVOL");
    const uint64_t iso_bytes = ReadFile(iso).size();
    CHECK(ps2r::app::DiscTableKind(DumpTable({DumpRow(4514, "SLES-52568", "1.01", iso)})) == "redump");
    CHECK(ps2r::app::DiscTableKind(ListJson("x", "SLES_525.68", retail, core, false)) == "files");
    CHECK(ps2r::app::DiscTableKind("{\"kind\": \"region-map\"}") == "region-map" && ps2r::app::DiscTableKind("[1]").empty());
    Register(DumpTable({DumpRow(4514, "SLES-52568", "1.01", iso), DumpRow(35281, "SLED-52574", "1.01", iso, 123456, false)},
                       {ProgramRow("SLES-52568", "SLES_525.68", retail[1].bytes)}));
    CHECK_EQ(ps2r::app::KnownDumps().size(), 2u);
    CHECK_EQ(ps2r::app::KnownPrograms().size(), 1u);

    // retail: the size alone is "retail by size"; the hashes make it retail
    {
        DiscIdentity id;
        CHECK(ps2r::app::IdentifyDisc(iso, &id, &error));
        CHECK(id.verdict == Verdict::RetailBySize && !id.retail && !id.image_hashed && id.same_size && id.known_program);
        CHECK(id.known_executable && id.program_sha256 == Hex(retail[1].bytes));
        CHECK(id.file_set_ok && id.warning.empty() && id.region == DiscIdentity::Region::PAL);
        CHECK(id.executable == "SLES_525.68" && id.serial == "SLES-52568" && id.volume_id == "TESTVOL");
        CHECK(id.boot == "cdrom0:\\SLES_525.68;1" && id.version == "1.01" && id.video_mode == "PAL");
        CHECK_EQ(id.elf_entry, 0x00100008u);
        CHECK_EQ(id.files, 5u);
        CHECK_EQ(id.files_hashed, 0u);   // no retail-provenance list: no file is hashed
        CHECK(id.manifest == "test-pal" && id.list_provenance == "a test disc" && !id.list_retail);
        CHECK(ps2r::app::DescribeDisc(id) == "PAL image SLES-52568 v1.01 by size (the retail dump's " + ps2r::app::Thousands(iso_bytes) +
                                             " bytes; contents not verified: --verify-disc hashes the image; volume TESTVOL)");
        uint64_t last = 0;
        CHECK(ps2r::app::IdentifyDiscFully(iso, &id, &error, [&](uint64_t done, uint64_t total, const std::string&) {
            last = done;
            return done <= total;
        }));
        CHECK(id.verdict == Verdict::Retail && id.retail && id.image_hashed && id.dump_id == 4514u && id.dump_version == "1.01");
        CHECK_EQ(last, iso_bytes);
        CHECK(ps2r::app::DescribeDisc(id) == "retail PAL image SLES-52568 v1.01 (Redump disc 4514; volume TESTVOL)");
        CHECK(ps2r::app::ClassifyDisc(id) == "retail SLES-52568 v1.01 (Redump)");
        CHECK(!ps2r::app::IdentifyDiscFully(iso, &id, &error, [](uint64_t, uint64_t, const std::string&) { return false; }));
        CHECK(error == "The check was stopped." && id.verdict == Verdict::Refused);
        // the same disc as a raw .bin (2352-byte sectors): the user data is the dump -- retail, raw
        const std::string raw = ToRaw(iso);
        CHECK(!raw.empty());
        DiscIdentity r;
        CHECK(ps2r::app::IdentifyDiscFully(raw, &r, &error));
        CHECK(r.raw && r.retail && r.verdict == Verdict::Retail && r.region == DiscIdentity::Region::PAL && r.serial == "SLES-52568" && r.files == 5u);
        std::remove(raw.c_str());
    }
    // one byte of one file changed: the same size -- retail by size until hashed, then modified; no file is named against a
    // list of this disc's files
    std::string modified_iso;
    {
        std::vector<IsoFile> files = retail;
        files[2].bytes[1234] ^= 0xFF;
        modified_iso = WriteTestIso(files, "TESTVOL");
        DiscIdentity id;
        CHECK(ps2r::app::IdentifyDisc(modified_iso, &id, &error));
        CHECK(id.verdict == Verdict::RetailBySize);
        CHECK(ps2r::app::IdentifyDiscFully(modified_iso, &id, &error));
        CHECK(id.verdict == Verdict::Modified && !id.retail && id.same_size && id.file_set_ok && id.differing_files.empty() && !id.list_retail);
        CHECK(ps2r::app::DescribeDisc(id) == "modified PAL image SLES-52568 (not the retail dump: the size of the v1.01 dump, a different SHA-1; volume TESTVOL)");
        CHECK(ps2r::app::ClassifyDisc(id) == "modified PAL image (not a known retail dump; the size of SLES-52568's dump, other contents)");
    }
    // a file added: another size -- modified without hashing (the size is decisive)
    {
        std::vector<IsoFile> files = retail;
        files.push_back({"CRASH6", "EXTRA.TXT", Bytes("added by a mod")});
        const std::string grown = WriteTestIso(files, "TESTVOL");
        const uint64_t grown_bytes = ReadFile(grown).size();
        CHECK(grown_bytes > iso_bytes);
        DiscIdentity id;
        CHECK(ps2r::app::IdentifyDisc(grown, &id, &error));
        CHECK(id.verdict == Verdict::Modified && !id.same_size && !id.image_hashed && id.dump_bytes == iso_bytes && id.image_bytes == grown_bytes);
        CHECK(ps2r::app::DescribeDisc(id) == "modified PAL image SLES-52568 (not the retail dump: " + ps2r::app::Thousands(grown_bytes) + " bytes vs " +
                                             ps2r::app::Thousands(iso_bytes) + "; volume TESTVOL)");
        CHECK(ps2r::app::ClassifyDisc(id) == "modified PAL image (not a known retail dump; " + ps2r::app::Thousands(grown_bytes - iso_bytes) +
                                             " bytes larger than SLES-52568's dump)");
        // with a file list of RETAIL provenance: the changed and added files are named
        ps2r::app::RetailManifest r;
        CHECK(ps2r::app::ParseRetailManifest(ListJson("test-pal-retail", "SLES_525.68", retail, core, true), &r, &error) && r.retail);
        ps2r::app::AddRetailManifest(r);
        CHECK(ps2r::app::IdentifyDisc(grown, &id, &error));
        CHECK(id.list_retail && id.manifest == "test-pal-retail" && Contains(id.added_files, "CRASH6/EXTRA.TXT") && id.differing_files.empty());
        CHECK(ps2r::app::IdentifyDisc(modified_iso, &id, &error));
        CHECK(id.verdict == Verdict::RetailBySize);   // the same size, not hashed: nothing to name yet
        CHECK(ps2r::app::IdentifyDiscFully(modified_iso, &id, &error));
        CHECK(id.verdict == Verdict::Modified && id.list_retail && id.differing_files.size() == 1 && Contains(id.differing_files, "CRASH6/CRASH.BH"));
        CHECK(ps2r::app::DescribeDisc(id) == "modified PAL image SLES-52568 (not the retail dump: the size of the v1.01 dump, a different SHA-1; "
                                             "1 file differs from the retail file list: CRASH6/CRASH.BH; volume TESTVOL)");
        ps2r::app::ClearRetailManifests();
        ps2r::app::AddRetailManifest(m);
        std::remove(grown.c_str());
    }
    std::remove(modified_iso.c_str());
    // a non-core file missing: still accepted (modified by size)
    {
        std::vector<IsoFile> files(retail.begin(), retail.end() - 1);
        const std::string small = WriteTestIso(files, "TESTVOL");
        DiscIdentity id;
        CHECK(ps2r::app::IdentifyDisc(small, &id, &error));
        CHECK(id.verdict == Verdict::Modified && id.file_set_ok && !id.retail);
        std::remove(small.c_str());
    }
    // the core files missing: refused, and the message names them
    {
        const std::vector<IsoFile> files = {retail[0], retail[1], retail[4]};
        const std::string other = WriteTestIso(files, "OTHER");
        DiscIdentity id;
        CHECK(!ps2r::app::IdentifyDisc(other, &id, &error));
        CHECK(!id.file_set_ok && !id.retail && id.verdict == Verdict::Refused);
        CHECK(error == "It is a PlayStation 2 disc (SLES-52568), but not Test Title: CRASH6/CRASH.BD and CRASH6/CRASH.BH are missing.");
        CHECK(ps2r::app::DescribeDisc(id) == "refused PAL image SLES-52568 (missing: CRASH6/CRASH.BD, CRASH6/CRASH.BH; volume OTHER)");
        CHECK(ps2r::app::ClassifyDisc(id) == "refused");
        std::remove(other.c_str());
    }
    // a demo disc's serial: refused by the table, whatever files it carries
    {
        std::vector<IsoFile> files = retail;
        files[0].bytes = Bytes("BOOT2 = cdrom0:\\SLED_525.74;1\r\nVER = 1.01\r\nVMODE = PAL\r\n");
        files[1].name = "SLED_525.74";
        const std::string demo = WriteTestIso(files, "DEMO");
        DiscIdentity id;
        CHECK(!ps2r::app::IdentifyDisc(demo, &id, &error));
        CHECK(error == "It is a PlayStation 2 disc (SLED-52574), but a demo disc, not Test Title itself.");
        std::remove(demo.c_str());
    }
    // another region's program with the core files, no dump of it known: unknown but plausible -- accepted with a warning
    {
        std::vector<IsoFile> files = retail;
        files[0].bytes = Bytes("BOOT2 = cdrom0:\\SLUS_209.09;1\r\nVER = 1.00\r\n");
        files[1].name = "SLUS_209.09";
        const std::string us = WriteTestIso(files, "USVOL");
        DiscIdentity id;
        CHECK(ps2r::app::IdentifyDisc(us, &id, &error));
        CHECK(id.file_set_ok && !id.retail && id.manifest.empty() && id.verdict == Verdict::Unknown && !id.known_program);
        CHECK(id.region == DiscIdentity::Region::NTSC_U && id.serial == "SLUS-20909");
        CHECK(id.warning.find("SLUS-20909, NTSC-U") != std::string::npos);
        CHECK(ps2r::app::DescribeDisc(id) == "unknown but plausible NTSC-U image SLUS-20909 (no retail dump of this program is known; volume USVOL)");
        CHECK(ps2r::app::ClassifyDisc(id) == "unknown but plausible");
        // two dumps of that program known (neither this image): modified, against the closest
        const uint64_t us_bytes = ReadFile(us).size();
        Register(DumpTable({DumpRow(42680, "SLUS-20909", "1.00", us, us_bytes + 4096), DumpRow(13689, "SLUS 20909", "2.00", us, us_bytes + 2048 * 100)}));
        // the release is known, its program is not (no programs row for SLUS-20909): unknown but plausible, said so
        CHECK(ps2r::app::IdentifyDisc(us, &id, &error));
        CHECK(id.verdict == Verdict::Unknown && id.known_program && !id.known_executable);
        CHECK(ps2r::app::DescribeDisc(id) == "unknown but plausible NTSC-U image SLUS-20909 (its program is not a known retail executable; volume USVOL)");
        CHECK(id.warning == "This disc of Test Title (SLUS-20909, NTSC-U) does not carry the original release's program (SLUS_209.09): it may not play as expected.");
        // with its program known: a modified disc of that release, against the closest of its two dumps
        Register(DumpTable({DumpRow(42680, "SLUS-20909", "1.00", us, us_bytes + 4096)}, {ProgramRow("SLUS-20909", "SLUS_209.09", files[1].bytes)}));
        CHECK(ps2r::app::IdentifyDisc(us, &id, &error));
        CHECK(id.verdict == Verdict::Modified && id.program_dumps == 2u && id.dump_id == 42680u && id.dump_version == "1.00");
        CHECK(ps2r::app::DescribeDisc(id) == "modified NTSC-U image SLUS-20909 (not a retail dump: " + ps2r::app::Thousands(us_bytes) + " bytes vs " +
                                             ps2r::app::Thousands(us_bytes + 4096) + " (v1.00, the closest of 2); volume USVOL)");
        CHECK(ps2r::app::ClassifyDisc(id) == "modified NTSC-U image (not a known retail dump; 4,096 bytes smaller than SLUS-20909 v1.00's dump)");
        std::remove(us.c_str());
    }
    // the title's file names and a program named SLES_525.68 that is not the retail executable: unknown but plausible
    {
        std::vector<IsoFile> files = retail;
        files[1].bytes = FakeElf(0x00100008, 5000);
        const std::string other = WriteTestIso(files, "TESTVOL");
        DiscIdentity id;
        CHECK(ps2r::app::IdentifyDisc(other, &id, &error));
        CHECK(id.verdict == Verdict::Unknown && id.file_set_ok && id.known_program && !id.known_executable && !id.retail);
        CHECK(ps2r::app::DescribeDisc(id) == "unknown but plausible PAL image SLES-52568 (its program is not a known retail executable; volume TESTVOL)");
        CHECK(ps2r::app::ClassifyDisc(id) == "unknown but plausible");
        CHECK(!id.warning.empty());
        std::remove(other.c_str());
    }
    // a disc image that is not a PlayStation 2 game
    {
        const std::string data = WriteTestIso({{"", "README.TXT", Bytes("hello")}}, "DATA");
        DiscIdentity id;
        CHECK(!ps2r::app::IdentifyDisc(data, &id, &error));
        CHECK(error == "It is a disc image, but not of a PlayStation 2 game (it has no SYSTEM.CNF).");
        std::remove(data.c_str());
    }
    // not a disc image at all, and a file that is not there
    {
        const std::string junk = (fs::temp_directory_path() / "te_test_identity_junk.iso").string();
        FILE* f = std::fopen(junk.c_str(), "wb");
        CHECK(f != nullptr);
        if (f) {
            const std::vector<uint8_t> x(40 * 1024, 'x');
            std::fwrite(x.data(), 1, x.size(), f);
            std::fclose(f);
        }
        DiscIdentity id;
        CHECK(!ps2r::app::IdentifyDisc(junk, &id, &error));
        CHECK(error.rfind("It is not a disc image", 0) == 0);
        std::remove(junk.c_str());
        CHECK(!ps2r::app::IdentifyDisc(junk + ".missing", &id, &error));
        CHECK(error.rfind("It cannot be opened", 0) == 0);
    }
    std::remove(iso.c_str());
    // the readers refuse what is not a table of their kind
    CHECK(!ps2r::app::ParseRetailManifest("{\"id\": \"x\"}", &m, &error));
    CHECK(!ps2r::app::ParseRetailManifest("{\"id\": \"x\", \"executable\": \"A\", \"files\": [{\"path\": \"A\"}]}", &m, &error));
    CHECK(!ps2r::app::ParseRetailManifest("[1, 2", &m, &error));
    CHECK(!ps2r::app::ParseRetailManifest("{\"kind\": \"redump\", \"id\": \"x\", \"executable\": \"A\", \"files\": [1]}", &m, &error));
    std::vector<ps2r::app::KnownDump> dumps;
    CHECK(!ps2r::app::ParseKnownDumps("{\"kind\": \"redump\", \"dumps\": []}", &dumps, &error));
    CHECK(!ps2r::app::ParseKnownDumps("{\"kind\": \"redump\", \"dumps\": [{\"id\": 1, \"serial\": \"SLES-52568\", \"bytes\": 5, \"crc32\": \"zz\", \"sha1\": \"\"}]}", &dumps, &error));
    CHECK(!ps2r::app::RegisterDiscTable("{\"kind\": \"region-map\"}", &error));
    CHECK(ps2r::app::RegionOfSerial("SLPM_658.01") == DiscIdentity::Region::NTSC_J);
    CHECK(ps2r::app::SerialOf("SLPM_658.01") == "SLPM-65801" && ps2r::app::SerialOf("README.TXT").empty());
    CHECK(std::string(ps2r::app::VerdictName(Verdict::RetailBySize)) == "retail-size");
}

void UsersDisc(const std::string& iso) {
    const char* src = std::getenv("TE_SOURCE_DIR");
    const std::string root = std::string(src ? src : ".") + "/launcher/discs/";
    ps2r::app::ClearRetailManifests();
    ps2r::app::ClearKnownDumps();
    std::string error;
    for (const char* name : {"redump.json", "pal.json"}) {
        const bool ok = ps2r::app::LoadDiscTable(root + name, &error);
        CHECK(ok);
        if (!ok) std::fprintf(stderr, "  %s: %s\n", name, error.c_str());
    }
    CHECK_EQ(ps2r::app::KnownDumps().size(), 6u);
    CHECK_EQ(ps2r::app::KnownPrograms().size(), 2u);
    CHECK(ps2r::app::RetailManifests().size() == 1 && !ps2r::app::RetailManifests()[0].retail);
    DiscIdentity id;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(ps2r::app::IdentifyDisc(iso, &id, &error));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "  the TE_TEST_ISO disc (no hashing): %s -- %s -- %u files in %.0f ms\n", ps2r::app::DescribeDisc(id).c_str(),
                 ps2r::app::ClassifyDisc(id).c_str(), id.files, ms);
    CHECK(id.region == DiscIdentity::Region::PAL && !id.retail && id.file_set_ok && id.verdict == Verdict::Modified);
    CHECK(id.serial == "SLES-52568" && id.executable == "SLES_525.68" && id.volume_id == "CRASH");
    CHECK(id.elf_entry == 0x00100008u && id.elf_bytes == 2152836u && id.files == 45u && id.version == "1.01");
    // the program is the retail PAL executable (the one the decompilation was made from): the release is certain
    CHECK(id.known_executable && id.program_sha256 == "7a609bb3ac32c4ef42129088902c4cf8aa05cf9cb62845e121764e32d2cedc1d");
    CHECK(id.image_bytes == 2579234816ull && id.dump_bytes == 2558263296ull && id.dump_id == 4514u && !id.same_size && id.program_dumps == 1u);
    CHECK(!id.list_retail && id.differing_files.empty() && id.list_provenance == "a modified PAL image used for development (Redump mismatch)");
    CHECK(ps2r::app::DescribeDisc(id) == "modified PAL image SLES-52568 (not the retail dump: 2,579,234,816 bytes vs 2,558,263,296; volume CRASH)");
    CHECK(ps2r::app::ClassifyDisc(id) == "modified PAL image (not a known retail dump; 20,971,520 bytes larger than SLES-52568's dump)");
    // the whole image: its CRC-32 and SHA-1 (the values redump-style tools give for this file: bc22864a, cf69ffd7...)
    const auto t1 = std::chrono::steady_clock::now();
    uint64_t reported = 0;
    CHECK(ps2r::app::IdentifyDiscFully(iso, &id, &error, [&](uint64_t done, uint64_t, const std::string&) { reported = done; return true; }));
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    std::fprintf(stderr, "  the TE_TEST_ISO disc hashed: crc32 %08x sha1 %s -- %.1f s (%.0f MB/s)\n", id.image_crc32, id.image_sha1.c_str(), s,
                 double(id.image_bytes) / 1048576.0 / (s > 0 ? s : 1));
    CHECK(id.image_hashed && id.image_crc32 == 0xbc22864au && id.image_sha1 == "cf69ffd7ab5225ac5e8ffb6a494495743ba2b40a");
    CHECK(id.verdict == Verdict::Modified && !id.retail && reported >= id.image_bytes);
}
}  // namespace

int main() {
    Hashes();
    Synthetic();
    const std::string iso = te_test::FixtureIso();
    if (iso.empty()) std::fprintf(stderr, "test_disc_identity: TE_TEST_ISO not set -- the real-disc checks are SKIPPED\n");
    else UsersDisc(iso);
    return te_test::Finish("test_disc_identity");
}
