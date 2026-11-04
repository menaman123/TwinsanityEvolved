// test_stream_card.cpp -- the stream service over the disc reader thread (TE_DISC_ASYNC=1: the PS Vita's path) and a card
// slower than the drive model (TE_DISC_SLOW_MS: 250 ms a read). In play a read whose drive time has come and whose bytes
// are still on the card stays in flight -- a poll answers "reading", the boundary leaves it -- and lands once the card has
// them, the bytes the image's; a Wait parks whole frames for the card; under the deterministic profile the landing waits for
// the card at the model's own frame; TE_STREAM_WAIT_CARD=1 is not tested here (the old landing: the deterministic one in
// play). A read of the archive (CRASH6\CRASH.BD, its index CRASH6\CRASH.BH) has the rest of its entry read ahead, and the
// entry's next reads are served from memory.
#include <chrono>
#include <cstring>
#include <thread>

#include "audio_service.h"
#include "disc_reader.h"
#include "stream_service.h"
#include "te_link_ahead.h"
#include "te_test.h"
#include "te_test_iso.h"

namespace {
using te::stream::Service;
using te::stream::StreamService;

std::string g_iso;
std::vector<uint8_t> g_data, g_bd;
constexpr uint32_t kDest = 0x00400000;
constexpr uint32_t kSm2 = 1u << 20, kRm2 = 2u << 20;   // the archive's two entries

void Fresh(bool deterministic) {
    te::io::Idle();   // (the reader's read-aheads of the disc about to go)
    te::Settings settings = te_test::DefaultSettings();
    settings.iso_path = g_iso;
    settings.stream_instant = false;
    settings.deterministic = deterministic;
    te_test::Reset(settings);
    Service().Reset();
    te::audio::Service().Configure(false);
}

bool Landed(uint32_t dest, const std::vector<uint8_t>& want, size_t at, size_t n) {
    const uint8_t* p = te::GuestPointer(dest, n);
    return p && std::memcmp(p, want.data() + at, n) == 0;
}

void TestPlayPutsOff() {
    Fresh(false);
    auto& s = Service();
    te::Clock& clock = te::Services::Get().clock();
    clock.Start();
    CHECK_EQ(s.Initialise(9, 0x400000), 0);
    const int32_t f = s.OpenFile("Crash6/Data.bd");
    const uint64_t waits = te::io::Count().waits_stream;
    // a 1000-byte read: the drive ends in frame 2; the card gives the bytes 250 ms after the issue
    const auto t0 = std::chrono::steady_clock::now();
    s.Read(3, f, 0, 1000, kDest);
    for (int k = 0; k < 4; ++k) clock.Boundary();   // the drive is done
    const double since = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (since < 200.0) {
        CHECK(s.IsReading(3));   // ... the card is not: the read stays in flight, nobody waited
        CHECK_EQ(s.Card().late_reads, 1u);
        CHECK(s.Card().late_boundaries + s.Card().late_polls >= 1u);
        CHECK(!Landed(kDest, g_data, 0, 1000));
        CHECK_EQ(te::io::Count().waits_stream, waits);
    } else {
        std::fprintf(stderr, "  (the boundaries took %.0f ms: the put-off checks skipped)\n", since);
    }
    // the card's bytes come; the next poll lands them
    bool reading = true;
    for (int k = 0; k < 200 && reading; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        reading = s.IsReading(3);
    }
    CHECK(!reading);
    CHECK(Landed(kDest, g_data, 0, 1000));
    CHECK_EQ(te::io::Count().waits_stream, waits);
    // a Wait in play: frames parked for the card, then the bytes
    s.Read(4, f, 1000, 3000, kDest + 0x1000);
    const uint64_t parks = s.Card().parks, frames = clock.BoundaryCount();
    CHECK_EQ(s.Wait(4), 0);
    CHECK(Landed(kDest + 0x1000, g_data, 1000, 3000));
    CHECK(s.Card().parks > parks);
    CHECK(clock.BoundaryCount() > frames);
    CHECK_EQ(te::io::Count().waits_stream, waits);
    s.CloseFile(f);
}

void TestDeterministicWaits() {
    Fresh(true);
    auto& s = Service();
    te::Clock& clock = te::Services::Get().clock();
    clock.Start();
    CHECK_EQ(s.Initialise(9, 0x400000), 0);
    const int32_t f = s.OpenFile("Crash6/Data.bd");
    const uint64_t waits = te::io::Count().waits_stream;
    s.Read(3, f, 0, 1000, kDest + 0x2000);
    const uint64_t due = uint64_t(s.PendingEndMs(3) / StreamService::kFrameMs);
    while (clock.BoundaryCount() < due) {
        CHECK(s.IsReading(3));
        clock.Boundary();
    }
    // the frame that holds the drive's end: the poll lands the read, waiting for the card
    CHECK(!s.IsReading(3));
    CHECK(Landed(kDest + 0x2000, g_data, 0, 1000));
    CHECK_EQ(te::io::Count().waits_stream, waits + 1);
    CHECK_EQ(s.Card().late_reads, 0u);
    s.CloseFile(f);
}

void TestEntryAhead() {
    Fresh(true);
    auto& s = Service();
    te::Clock& clock = te::Services::Get().clock();
    clock.Start();
    CHECK_EQ(s.Initialise(9, 0x400000), 0);
    te::archive::Entry sm2, rm2;
    CHECK(te::archive::Find("LEVELS\\test\\chunk.sm2", sm2));
    CHECK(te::archive::Find("levels\\test\\chunk.rm2", rm2));
    CHECK_EQ(sm2.size, kSm2);
    CHECK_EQ(rm2.image, sm2.image + kSm2);
    te::archive::Entry at;
    CHECK(te::archive::EntryAt(rm2.image + 5, at) && at.image == rm2.image);
    CHECK(!te::archive::EntryAt(rm2.image + kRm2, at));
    const int32_t bd = s.OpenFile("CRASH6\\CRASH.BD");
    // the game's walk through the RM2: 128 KB blocks with skips; the first has the rest of the entry read ahead
    const te::io::Census c0 = te::io::Count();
    for (uint32_t pos = kSm2 + 4096, k = 0; pos + 131072 <= kSm2 + kRm2; pos += 150000, ++k) {
        s.Read(1, bd, pos, 131072, kDest);
        CHECK_EQ(s.Wait(1), 0);
        CHECK(Landed(kDest, g_bd, pos, 131072));
    }
    const te::io::Census c = te::io::Count();
    CHECK_EQ(s.Card().entries_ahead, 1u);
    CHECK_EQ(c.ahead_entries - c0.ahead_entries, 1u);
    CHECK(c.ahead_hits + c.ahead_waited - c0.ahead_hits - c0.ahead_waited >= 12u);
    CHECK_EQ(c.ahead_held, 0u);   // read through: let go of
    (void)clock;
    s.CloseFile(bd);
}

}  // namespace

int main() {
    setenv("TE_DISC_ASYNC", "1", 1);          // the reader thread before its first use
    setenv("TE_DISC_SLOW_MS", "0,250", 1);    // 250 ms a read
    setenv("TE_DISC_BENCH", "0", 1);
    g_data = te_test::Pattern(5000, 0x11);
    g_bd = te_test::Pattern(kSm2 + kRm2, 0x7E);
    // CRASH.BH: a header word, then per entry its name's length, the name, its offset in CRASH.BD and its size
    std::vector<uint8_t> bh(4, 0);
    auto put32 = [&bh](uint32_t v) { for (int b = 0; b < 4; ++b) bh.push_back(uint8_t(v >> (8 * b))); };
    auto entry = [&](const std::string& name, uint32_t offset, uint32_t size) {
        put32(uint32_t(name.size()));
        bh.insert(bh.end(), name.begin(), name.end());
        put32(offset);
        put32(size);
    };
    entry("Levels\\test\\Chunk.sm2", 0, kSm2);
    entry("Levels\\test\\Chunk.rm2", kSm2, kRm2);
    g_iso = te_test::WriteTestIso({
        {"CRASH6", "DATA.BD", g_data},
        {"CRASH6", "CRASH.BH", bh},
        {"CRASH6", "CRASH.BD", g_bd},
    });
    if (g_iso.empty()) { std::fprintf(stderr, "cannot write the test ISO\n"); return 2; }
    te::Settings settings = te_test::DefaultSettings();
    settings.iso_path = g_iso;
    te_test::Reset(settings);
    te::Services::Get().disc().Initialise();
    te::io::Start();
    CHECK(te::io::Async());

    TestDeterministicWaits();
    TestPlayPutsOff();
    TestEntryAhead();

    te::io::Idle();
    std::remove(g_iso.c_str());
    return te_test::Finish("test_stream_card");
}
