// test_disc_overlay.cpp -- engine/cdvd/disc_overlay.h over a SYNTHETIC ISO 9660 volume the test writes (te_test_iso.h: byte
// patterns, no game data) and mod folders it writes beside it: a same-size (in place) replacement, a grown one (moved to the
// virtual region), added files (in a disc folder and in folders the disc lacks), a ranges recipe of base / file / zero, two
// layers stacking on one file (later wins; "base" is the layer below), a layer refused whole, Find / List answering the new
// LBAs and sizes, reads spanning an overridden file's end into the plain file after it, the zero-padded last partial
// sector, the image end, the hit count, the empty overlay as a pass-through -- then te::Disc composing the enabled folder
// mods (te_mods.h) so that title::Files and the stream façade read the composed bytes, and the census row.
#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "engine/cdvd/disc_overlay.h"
#include "engine/cdvd/iso_fs.h"
#include "ps2e_seam.h"
#include "stream_service.h"
#include "te_mods.h"
#include "te_test.h"
#include "te_test_iso.h"

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kSector = IsoFs::kSectorSize;

void WriteBytes(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
void WriteText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}
std::vector<uint8_t> Read(const IsoFs& v, uint64_t offset, size_t n, size_t* got = nullptr) {
    std::vector<uint8_t> out(n, 0xEE);
    const size_t g = v.ReadBytes(offset, n, out.data());
    if (got) *got = g;
    out.resize(g);
    return out;
}
std::vector<uint8_t> Slice(const std::vector<uint8_t>& v, size_t at, size_t n) { return std::vector<uint8_t>(v.begin() + std::ptrdiff_t(at), v.begin() + std::ptrdiff_t(at + n)); }
std::vector<uint8_t> Cat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
std::vector<uint8_t> Zeros(size_t n) { return std::vector<uint8_t>(n, 0); }
const IsoFs::Entry* Named(const std::vector<IsoFs::Entry>& list, const std::string& name) {
    for (const IsoFs::Entry& e : list) {
        std::string n = e.name.substr(0, e.name.find(';'));
        if (n == name) return &e;
    }
    return nullptr;
}

}  // namespace

int main() {
    using namespace te_test;

    // --- the disc: CRASH6/{A,B,C,BIG} + root SLES_525.68, SYSTEM.CNF (sectors from 20 on, one after another)
    const std::vector<uint8_t> a = Pattern(5000, 1), b = Pattern(3000, 2), c = Pattern(2048, 3), big = Pattern(10000, 4);
    const std::vector<uint8_t> elf = Pattern(3000, 5), cnf = Pattern(57, 6);
    const std::string iso = WriteTestIso({{"CRASH6", "A.BIN", a}, {"CRASH6", "B.BIN", b}, {"CRASH6", "C.BIN", c},
                                          {"CRASH6", "BIG.BD", big}, {"", "SLES_525.68", elf}, {"", "SYSTEM.CNF", cnf}});
    CHECK(!iso.empty());
    IsoFs plain;
    CHECK(plain.Open(iso));
    IsoFs::Entry ea, eb, ec, ebig, ecnf;
    CHECK(plain.Find("CRASH6/A.BIN", ea) && plain.Find("CRASH6/B.BIN", eb) && plain.Find("CRASH6/C.BIN", ec) &&
          plain.Find("CRASH6/BIG.BD", ebig) && plain.Find("SYSTEM.CNF", ecnf));
    CHECK_EQ(ea.lba, 20u);
    CHECK_EQ(eb.lba, 23u);
    CHECK_EQ(ec.lba, 25u);
    CHECK_EQ(ebig.lba, 26u);
    const uint32_t volume = plain.VolumeSectors();
    CHECK_EQ(volume, 34u);
    CHECK_EQ(plain.ImageBytes(), uint64_t(volume) * kSector);

    // --- the mods
    const fs::path root = fs::temp_directory_path() / ("te_overlay_" + std::to_string(::time(nullptr)) + "_" + std::to_string(std::rand()));
    const fs::path one = root / "mods" / "one", two = root / "mods" / "two", bad = root / "mods" / "bad";
    const std::vector<uint8_t> a1 = Pattern(4500, 101), c1 = Pattern(5000, 103), new1 = Pattern(100, 104), x1 = Pattern(10, 105);
    const std::vector<uint8_t> patch1 = Pattern(100, 106), a2 = Pattern(6000, 201), p2 = Pattern(8, 202);
    WriteBytes(one / "disc" / "CRASH6" / "A.BIN", a1);                 // replaced, smaller: in place
    WriteBytes(one / "disc" / "crash6" / "c.bin", c1);                 // replaced, grown: moved (names any case)
    WriteBytes(one / "disc" / "CRASH6" / "NEW.BIN", new1);             // added in a disc folder
    WriteBytes(one / "disc" / "Extra" / "Deep" / "X.TXT", x1);         // added in folders the disc lacks
    WriteBytes(one / "data" / "patch.bin", patch1);
    WriteText(one / "disc" / "CRASH6" / "BIG.BD.ranges",
              "# the archive with 100 bytes changed and 50 inserted\nbase 0 4096\nfile data/patch.bin 0 100   # new bytes\nzero 0x32\nbase 4196 5804\n");
    WriteText(one / "disc" / "CRASH6" / ".DS_Store", "host clutter: ignored");
    WriteBytes(two / "disc" / "CRASH6" / "A.BIN", a2);                 // the later layer wins (still in A's 3 sectors)
    WriteBytes(two / "data" / "p2.bin", p2);
    WriteText(two / "disc" / "CRASH6" / "BIG.BD.ranges", "base 0 4100\nfile data/p2.bin 0 8\nbase 4108 5892\n");   // over layer one's (valid over the disc's too)
    WriteText(bad / "disc" / "CRASH6" / "B.BIN.ranges", "base 0 999999\n");   // past the file below: the whole layer is refused
    WriteBytes(bad / "disc" / "SYSTEM.CNF", Pattern(30, 301));

    // the expected composed files
    const std::vector<uint8_t> big1 = Cat(Cat(Cat(Slice(big, 0, 4096), patch1), Zeros(50)), Slice(big, 4196, 5804));
    CHECK_EQ(big1.size(), size_t(10050));
    const std::vector<uint8_t> big2 = Cat(Cat(Slice(big1, 0, 4100), p2), Slice(big1, 4108, 5892));
    CHECK_EQ(big2.size(), size_t(10000));
    // the other order: two over the disc, then one over that
    const std::vector<uint8_t> big_b = Cat(Cat(Slice(big, 0, 4100), p2), Slice(big, 4108, 5892));
    const std::vector<uint8_t> big_ba = Cat(Cat(Cat(Slice(big_b, 0, 4096), patch1), Zeros(50)), Slice(big_b, 4196, 5804));

    DiscOverlay v({{"one", one.string()}, {"two", two.string()}, {"bad", bad.string()}});
    std::string error;
    CHECK(v.Open(iso, &error));
    const DiscOverlay::Report& r = v.GetReport();
    CHECK(r.applied.size() == 2 && r.applied[0] == "one" && r.applied[1] == "two");
    CHECK(r.refused.size() == 1 && r.refused[0].find("bad: ") == 0 && r.refused[0].find("line 1") != std::string::npos &&
          r.refused[0].find("past the end") != std::string::npos);
    CHECK_EQ(r.overrides, 5u);   // A, BIG, C, NEW, EXTRA/DEEP/X
    CHECK_EQ(v.Overrides(), 5u);
    CHECK_EQ(r.relocated, 3u);   // C (grown), NEW, X (added)
    for (const std::string& line : r.files) std::fprintf(stderr, "  composed: %s\n", line.c_str());
    for (const std::string& line : r.refused) std::fprintf(stderr, "  refused: %s\n", line.c_str());

    // --- in place: A.BIN (layer two's 6000 bytes in A's three sectors), zero after its end, B after A
    IsoFs::Entry e;
    CHECK(v.Find("CRASH6/A.BIN", e) && e.lba == 20 && e.size == 6000 && !e.directory && e.name == "A.BIN;1");
    CHECK(v.Find("cdrom0:\\crash6\\a.bin;1", e) && e.lba == 20 && e.size == 6000);
    CHECK(Read(v, 20 * kSector, 6000) == a2);
    CHECK(Read(v, 20 * kSector + 6000, 144) == Zeros(144));                       // the last partial sector, padded
    CHECK(Read(v, 20 * kSector + 5900, 144 + 100 + 100) == Cat(Cat(Slice(a2, 5900, 100), Zeros(144)), Slice(b, 0, 100)));   // across into B
    CHECK(Read(v, eb.lba * kSector, 3000) == b);                                  // B untouched (the bad layer was refused)
    std::vector<uint8_t> sectors(4 * kSector);
    CHECK_EQ(v.ReadSectors(20, 4, sectors.data()), 4u);                           // A's three sectors + B's first
    CHECK(sectors == Cat(Cat(a2, Zeros(144)), Slice(b, 0, 2048)));

    // --- the ranges recipe, two layers: BIG.BD in place (10000 <= its 5 sectors)
    CHECK(v.Find("CRASH6/BIG.BD", e) && e.lba == 26 && e.size == 10000);
    CHECK(Read(v, 26 * kSector, 10000) == big2);
    CHECK(Read(v, 26 * kSector + 4090, 30) == Slice(big2, 4090, 30));           // a read across three segments of the recipe
    CHECK(Read(v, 26 * kSector + 10000, 240) == Zeros(240));                      // to the end of its 5 sectors
    CHECK(Read(v, 26 * kSector + 10200, 40 + 100) == Cat(Zeros(40), Slice(elf, 0, 100)));   // into SLES_525.68 after it

    // --- grown: C.BIN moved past the volume; its old sector still reads the disc's bytes
    CHECK(v.Find("CRASH6/C.BIN", e) && e.lba >= volume && e.size == 5000);
    const uint32_t c_lba = e.lba;
    CHECK(Read(v, uint64_t(c_lba) * kSector, 5000) == c1);
    CHECK(Read(v, uint64_t(ec.lba) * kSector, 2048) == c);
    // --- added: NEW.BIN, EXTRA/DEEP/X.TXT (synthesized folders)
    CHECK(v.Find("CRASH6/NEW.BIN", e) && e.lba >= volume && e.size == 100);
    CHECK(Read(v, uint64_t(e.lba) * kSector, 100) == new1);
    CHECK(Read(v, uint64_t(e.lba) * kSector + 100, 1948) == Zeros(1948));        // its one sector, padded
    IsoFs::Entry ex, ed, ex2;
    CHECK(v.Find("EXTRA", ex) && ex.directory && v.Find("Extra/Deep", ed) && ed.directory);
    CHECK(v.Find("\\EXTRA\\DEEP\\X.TXT;1", ex2) && !ex2.directory && ex2.size == 10);
    std::vector<uint8_t> last(kSector);
    CHECK_EQ(v.ReadSectors(ex2.lba, 1, last.data()), 1u);
    CHECK(last == Cat(x1, Zeros(2038)));
    CHECK(!v.Find("EXTRA/NOPE.BIN", e) && !v.Find("NOPE/X.TXT", e) && !v.Find("CRASH6/NOPE.BIN", e));
    CHECK(!v.Find("CRASH6/.DS_STORE", e));
    // --- the image end grows over the virtual region; past it nothing reads
    CHECK(v.ImageBytes() > plain.ImageBytes());
    CHECK(v.ImageBytes() % kSector == 0);
    size_t got = 0;
    CHECK(Read(v, v.ImageBytes() - kSector, kSector, &got).size() == kSector && got == kSector);
    CHECK(Read(v, v.ImageBytes(), 16, &got).empty() && got == 0);
    CHECK(Read(v, v.ImageBytes() - 8, 16, &got).size() == 8);                    // short at the end
    // --- a read across the disc's last sector into the virtual region (SYSTEM.CNF, then the synthesized folders' zeros)
    std::vector<uint8_t> across(3 * kSector);
    CHECK_EQ(v.ReadSectors(volume - 1, 3, across.data()), 3u);
    CHECK(Slice(across, 0, 57) == cnf && Slice(across, 57, 2048 - 57) == Zeros(2048 - 57));

    // --- List: the new sizes and LBAs, the added entries
    std::vector<IsoFs::Entry> list;
    IsoFs::Entry crash6;
    CHECK(v.Find("CRASH6", crash6) && crash6.directory && v.List(crash6, list));
    CHECK(Named(list, "A.BIN") && Named(list, "A.BIN")->size == 6000 && Named(list, "A.BIN")->lba == 20);
    CHECK(Named(list, "B.BIN") && Named(list, "B.BIN")->size == 3000 && Named(list, "B.BIN")->lba == eb.lba);
    CHECK(Named(list, "C.BIN") && Named(list, "C.BIN")->size == 5000 && Named(list, "C.BIN")->lba == c_lba);
    CHECK(Named(list, "BIG.BD") && Named(list, "BIG.BD")->size == 10000);
    CHECK(Named(list, "NEW.BIN") && Named(list, "NEW.BIN")->size == 100);
    CHECK_EQ(list.size(), size_t(5));
    CHECK(v.List(v.Root(), list) && Named(list, "EXTRA") && Named(list, "EXTRA")->directory && Named(list, "CRASH6") && Named(list, "SYSTEM.CNF"));
    CHECK(Named(list, "SYSTEM.CNF") && Named(list, "SYSTEM.CNF")->size == 57);   // the refused layer's file is not there
    CHECK(v.List(ex, list) && list.size() == 1 && Named(list, "DEEP") && Named(list, "DEEP")->directory);
    CHECK(v.List(ed, list) && list.size() == 1 && Named(list, "X.TXT") && Named(list, "X.TXT")->size == 10);

    // --- hits: a read of the plain disc counts none, a composed read one
    const uint64_t h0 = v.Hits();
    Read(v, eb.lba * kSector + 10, 100);
    CHECK_EQ(v.Hits(), h0);
    Read(v, 26 * kSector, 10);
    CHECK_EQ(v.Hits(), h0 + 1);
    // --- the reads are const and safe from several threads (the Vita's reader thread reads beside the game's)
    {
        std::atomic<int> wrong{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&, t] {
                for (int k = 0; k < 200; ++k) {
                    const uint64_t at = 26 * kSector + uint64_t((k * 37 + t * 101) % 9000);
                    std::vector<uint8_t> out(64);
                    if (v.ReadBytes(at, 64, out.data()) != 64 || out != Slice(big2, size_t(at - 26 * kSector), 64)) ++wrong;
                }
            });
        for (auto& th : threads) th.join();
        CHECK_EQ(wrong.load(), 0);
        CHECK_EQ(v.Hits(), h0 + 1 + 800);
    }

    // --- an overlay with no layers (or only empty ones) is the disc as it is
    {
        DiscOverlay empty({{"nothing", (root / "mods" / "absent").string()}});
        CHECK(empty.Open(iso) && empty.Empty() && empty.Overrides() == 0);
        CHECK_EQ(empty.ImageBytes(), plain.ImageBytes());
        CHECK(Read(empty, 0, size_t(plain.ImageBytes())) == Read(plain, 0, size_t(plain.ImageBytes())));
        CHECK(empty.Find("CRASH6/A.BIN", e) && e.lba == ea.lba && e.size == ea.size);
        std::vector<IsoFs::Entry> l1, l2;
        CHECK(empty.List(crash6, l1) && plain.List(crash6, l2) && l1.size() == l2.size());
        CHECK_EQ(empty.Hits(), 0u);
    }
    // --- COMPUTED files (the region loader's: a file made in memory from the one below it -- the image's, or a layer's)
    {
        auto upper_first = [](const std::vector<uint8_t>& below, std::vector<uint8_t>* out, std::string*) {
            *out = below;
            if (!out->empty()) (*out)[0] = uint8_t((*out)[0] ^ 0xFF);
            out->push_back(0x42);   // one byte longer
            return true;
        };
        DiscOverlay::Layer region("region:test", "");
        region.computed.push_back({"CRASH6/B.BIN", upper_first});                       // over the image's B (3000 -> 3001: in place)
        region.computed.push_back({"crash6\\a.bin", upper_first});                     // over layer two's A (6000 -> 6001)
        region.computed.push_back({"CRASH6/MADE.BIN", [](const std::vector<uint8_t>& below, std::vector<uint8_t>* out, std::string*) {
            CHECK(below.empty());                                                     // nothing below: an added file
            *out = {1, 2, 3};
            return true;
        }});
        DiscOverlay cv({{"one", one.string()}, {"two", two.string()}, region});
        CHECK(cv.Open(iso, &error));
        const DiscOverlay::Report& cr = cv.GetReport();
        CHECK(cr.applied.size() == 3 && cr.applied[2] == "region:test" && cr.refused.empty());
        std::vector<uint8_t> b_want = b;
        b_want[0] = uint8_t(b_want[0] ^ 0xFF);
        b_want.push_back(0x42);
        CHECK(cv.Find("CRASH6/B.BIN", e) && e.lba == eb.lba && e.size == 3001 && Read(cv, uint64_t(e.lba) * kSector, 3001) == b_want);
        std::vector<uint8_t> a_want = a2;
        a_want[0] = uint8_t(a_want[0] ^ 0xFF);
        a_want.push_back(0x42);
        CHECK(cv.Find("CRASH6/A.BIN", e) && e.lba == 20 && e.size == 6001 && Read(cv, 20 * kSector, 6001) == a_want);
        CHECK(cv.Find("CRASH6/MADE.BIN", e) && e.lba >= volume && e.size == 3 && Read(cv, uint64_t(e.lba) * kSector, 3) == std::vector<uint8_t>({1, 2, 3}));
        CHECK(cv.Find("CRASH6/BIG.BD", e) && Read(cv, uint64_t(e.lba) * kSector, 10000) == big2);   // the others as before
        // a computed file that fails: the whole layer is refused, the volume is the layers below
        DiscOverlay::Layer failing("region:fail", "");
        failing.computed.push_back({"CRASH6/B.BIN", [](const std::vector<uint8_t>&, std::vector<uint8_t>*, std::string* why) {
            *why = "the index does not read";
            return false;
        }});
        DiscOverlay fv({{"one", one.string()}, failing});
        CHECK(fv.Open(iso, &error));
        CHECK(fv.GetReport().refused.size() == 1 && fv.GetReport().refused[0].find("the index does not read") != std::string::npos);
        CHECK(fv.Find("CRASH6/B.BIN", e) && e.size == 3000 && Read(fv, uint64_t(e.lba) * kSector, 3000) == b);
        CHECK(fv.Find("CRASH6/A.BIN", e) && e.size == 4500);
    }
    // --- the path form a recipe is keyed by
    CHECK(DiscOverlay::NormalizePath("cdrom0:\\Crash6\\crash.bd;1") == "CRASH6/CRASH.BD");
    CHECK(DiscOverlay::NormalizePath("//SLES_525.68") == "SLES_525.68");

    // --- a RAW image (.bin, 2352 bytes a sector: sync, header, the 2048 user bytes, EDC/ECC): the same composed volume
    {
        const std::string raw_path = iso + ".bin";
        std::vector<uint8_t> image(size_t(plain.ImageBytes()));
        CHECK_EQ(plain.ReadBytes(0, image.size(), image.data()), image.size());
        {
            std::ofstream raw(raw_path, std::ios::binary);
            static const uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
            for (size_t s = 0; s < image.size() / kSector; ++s) {
                uint8_t head[16] = {};
                std::memcpy(head, kSync, 12);
                head[15] = 1;   // Mode 1: the user bytes at +16
                raw.write(reinterpret_cast<const char*>(head), 16);
                raw.write(reinterpret_cast<const char*>(&image[s * kSector]), std::streamsize(kSector));
                const std::vector<uint8_t> trailer(2352 - 16 - kSector, 0);
                raw.write(reinterpret_cast<const char*>(trailer.data()), std::streamsize(trailer.size()));
            }
        }
        DiscOverlay rv({{"one", one.string()}, {"two", two.string()}});
        CHECK(rv.Open(raw_path) && rv.RawSectors());
        CHECK_EQ(rv.ImageBytes(), v.ImageBytes());
        CHECK(rv.Find("CRASH6/A.BIN", e) && e.lba == 20 && e.size == 6000 && Read(rv, 20 * kSector, 6000) == a2);
        CHECK(rv.Find("CRASH6/BIG.BD", e) && Read(rv, uint64_t(e.lba) * kSector, 10000) == big2);
        CHECK(Read(rv, 0, size_t(rv.ImageBytes())) == Read(v, 0, size_t(v.ImageBytes())));   // byte for byte the plain image's volume
        std::error_code rm;
        fs::remove(raw_path, rm);
    }

    // --- te::Disc over the folder mods (te_mods.h): no mod on -> a plain IsoFs; "one" on -> the overlay every reader sees
    {
        te::Settings s = DefaultSettings();
        s.iso_path = iso;
        Reset(s);
        CHECK_EQ(te::mods::ScanFolder((root / "mods").string()), 0u);   // no mod.json yet: nothing registered
        WriteText(one / "mod.json", "{\"schema\": 2, \"id\": \"one\", \"kind\": \"disc\", \"name\": \"one\", \"version\": \"1\"}");
        WriteText(two / "mod.json", "{\"schema\": 2, \"id\": \"two\", \"kind\": \"disc\", \"name\": \"two\", \"version\": \"1\"}");
        CHECK_EQ(te::mods::ScanFolder((root / "mods").string()), 2u);
        te::Services::Get().disc().Initialise();
        CHECK(dynamic_cast<const DiscOverlay*>(&te::Services::Get().disc().Iso()) == nullptr);   // nothing on: the plain volume
        CHECK(te::mods::CensusRow() == "mods=0:0:0");
        // the region loader's files (te_mods.h SetRegionFiles): ONE last layer over the mods, not counted as a mod
        te::mods::SetRegionFiles("region:SLUS-20909", {{"CRASH6/C.BIN", [](const std::vector<uint8_t>& below, std::vector<uint8_t>* out, std::string*) {
            *out = below;
            out->resize(below.size() / 2);
            return true;
        }}});
        Reset(s);
        ps2e_disc_initialise();
        const IsoFs& rvol = te::Services::Get().disc().Iso();
        const DiscOverlay* rov = dynamic_cast<const DiscOverlay*>(&rvol);
        CHECK(rov && rov->GetReport().applied.size() == 1 && rov->GetReport().applied[0] == "region:SLUS-20909");
        CHECK(rvol.Find("CRASH6/C.BIN", e) && e.size == 1024 && Read(rvol, uint64_t(e.lba) * kSector, 1024) == Slice(c, 0, 1024));
        CHECK(te::mods::CensusRow() == "mods=0:0:0");   // the region layer is not a mod
        te::mods::SetRegionFiles("", {});
        Reset(s);
        ps2e_disc_initialise();
        CHECK(dynamic_cast<const DiscOverlay*>(&te::Services::Get().disc().Iso()) == nullptr);   // none: the plain volume again

        te::mods::Configure({"one"});
        Reset(s);   // a fresh disc is composed from the switches in force when it is made
        CHECK_EQ(ps2e_disc_initialise(), 1);
        const DiscOverlay* composed = dynamic_cast<const DiscOverlay*>(&te::Services::Get().disc().Iso());
        CHECK(composed != nullptr);
        CHECK(composed && composed->GetReport().applied.size() == 1 && composed->GetReport().applied[0] == "one");
        // title::Files (Platform::Files) reads the replaced A.BIN
        constexpr uint32_t kPath = 0x1000, kBuf = 0x100000;
        ps2e_files_reset();
        WriteString(kPath, "Crash6\\A.bin");
        const int32_t fd = ps2e_files_open(kPath, 1);
        CHECK(fd >= 0);
        CHECK_EQ(ps2e_files_read(fd, kBuf, 10000), 4500);
        CHECK(std::memcmp(te::GuestPointer(kBuf, 4500), a1.data(), 4500) == 0);
        ps2e_files_close(fd);
        WriteString(kPath, "Extra\\Deep\\X.txt");   // an added file opens like a disc file
        const int32_t fx = ps2e_files_open(kPath, 1);
        CHECK(fx >= 0 && ps2e_files_read(fx, kBuf, 64) == 10 && std::memcmp(te::GuestPointer(kBuf, 10), x1.data(), 10) == 0);
        // the stream façade reads the composed archive (layer one alone here)
        te::stream::StreamService& stream = te::stream::Service();
        stream.Reset();
        const int32_t sf = stream.OpenFile("Crash6/Big.bd");
        std::vector<uint8_t> bytes;
        CHECK(stream.ReadFileBytes(sf, 4090, 120, bytes) && bytes == Slice(big1, 4090, 120));
        const std::string row = te::mods::CensusRow();
        CHECK(row.rfind("mods=1:5:", 0) == 0 && row != "mods=1:5:0");   // A, BIG, C, NEW, X; reads served
        std::fprintf(stderr, "  census: %s\n", row.c_str());
        // mods.order: two before one -> one wins A.BIN
        te::mods::Configure({"one", "two"});
        te::mods::SetOrder("two,one");
        Reset(s);
        ps2e_disc_initialise();
        const IsoFs& vol = te::Services::Get().disc().Iso();
        const DiscOverlay* ordered = dynamic_cast<const DiscOverlay*>(&vol);
        CHECK(ordered && ordered->GetReport().applied.size() == 2 && ordered->GetReport().applied[0] == "two" && ordered->GetReport().refused.empty());
        CHECK(vol.Find("CRASH6/A.BIN", e) && e.size == 4500 && Read(vol, uint64_t(e.lba) * kSector, 4500) == a1);   // one, last, wins
        CHECK(vol.Find("CRASH6/BIG.BD", e) && e.size == 10050 && Read(vol, uint64_t(e.lba) * kSector, 10050) == big_ba);   // stacked the other way
        te::mods::SetOrder("");
        Reset(s);
        ps2e_disc_initialise();
        const IsoFs& vol2 = te::Services::Get().disc().Iso();
        CHECK(vol2.Find("CRASH6/A.BIN", e) && e.size == 6000 && Read(vol2, uint64_t(e.lba) * kSector, 6000) == a2);   // by id: two last
        CHECK(vol2.Find("CRASH6/BIG.BD", e) && e.size == 10000 && Read(vol2, uint64_t(e.lba) * kSector, 10000) == big2);
        te::mods::ScanFolder("");
        te::Services::ResetForTest();
    }

    std::error_code ec_rm;
    fs::remove_all(root, ec_rm);
    fs::remove(iso, ec_rm);
    return Finish("test_disc_overlay");
}
