// test_stream.cpp -- stream.h's façade (te::stream::StreamService) over a generated disc: files by path, sizes,
// reads into guest memory (complete when the call returns under the INSTANT model), sound-bank reads into the audio
// registry, the buffer pool, the ps2e_stream_* bodies themselves (guest-address arguments), and the DRIVE model
// Reads land at a poll inside the frame the drive finishes in, at the boundary, or inside a wait that parks
// frames; a busy poll charges the clock; the reads in flight survive a save state.
#include <cstring>

#include "audio_service.h"
#include "ps2e_seam.h"
#include "stream_service.h"
#include "te_loading.h"
#include "te_test.h"
#include "te_test_adpcm.h"
#include "te_test_iso.h"

namespace {
using te::stream::Service;
using te::stream::StreamService;

std::string g_iso;

void Fresh(bool instant = true) {
    te::Settings settings = te_test::DefaultSettings();
    settings.iso_path = g_iso;
    settings.stream_instant = instant;
    te_test::Reset(settings);
    Service().Reset();
    te::audio::Service().Configure(false);
}

// The drive model (the default in the runner): the timeline is the boundary count's.
void TestDriveModel() {
    Fresh(false);
    auto& s = Service();
    te::Clock& clock = te::Services::Get().clock();
    clock.Start();
    CHECK_EQ(s.Initialise(9, 0x400000), 0);
    CHECK(!s.Instant());
    const int32_t f0 = s.OpenFile("Crash6/Data.bd");
    const auto expected = te_test::Pattern(5000, 0x11);
    const uint32_t dest = 0x00400000;
    // Frame 0. A 1000-byte read at the file's start: a seek from LBA 0 (the file sits a few sectors in: the SHORT
    // class, 42 ms) + one sector (0.93 ms): the drive ends in frame 2 (33.3 < 42.9 < 50). Nothing lands in the call.
    s.Read(3, f0, 0, 1000, dest);
    CHECK_EQ(s.Count().reads, 1u);
    CHECK_EQ(s.Count().delivered, 0u);
    CHECK_EQ(te::GuestPointer(dest, 1)[0], 0);
    const double end = s.PendingEndMs(3);
    CHECK(end > 42.0 && end < 44.0);
    const uint64_t due_frame = static_cast<uint64_t>(end / StreamService::kFrameMs);
    CHECK_EQ(due_frame, 2u);
    // A poll in frame 0 finds the drive busy: "reading", and the frame budget charged 160 us (92 T0 counts).
    const uint64_t t0 = clock.Ticks();
    CHECK(s.IsReading(3));
    CHECK_EQ(clock.Ticks() - t0, StreamService::kPollChargeTicks);
    CHECK_EQ(s.Count().busy_polls, 1u);
    CHECK(s.IsReading(3));
    CHECK_EQ(clock.InFrameTicks(), 2 * StreamService::kPollChargeTicks);
    // The charge ends with the frame; the field's T0 is what the next frame's Ticks() start at.
    clock.Boundary();
    CHECK_EQ(clock.InFrameTicks(), 0u);
    CHECK_EQ(clock.Ticks(), 9600u);
    // The frames before the due one: still busy at every poll; the boundary hook does not land it either.
    for (uint64_t frame = 1; frame < due_frame; ++frame) {
        CHECK(s.IsReading(3));
        clock.Boundary();
    }
    CHECK_EQ(clock.BoundaryCount(), due_frame);
    CHECK_EQ(s.Count().delivered, 0u);
    // The due frame contains the drive's end: the poll lands the bytes (the console's EE saw it complete mid-frame).
    CHECK(!s.IsReading(3));
    CHECK_EQ(s.Count().poll_delivered, 1u);
    CHECK_EQ(s.Count().delivered_bytes, 1000u);
    CHECK(std::memcmp(te::GuestPointer(dest, 1000), expected.data(), 1000) == 0);
    CHECK_EQ(s.Wait(3), 0);
    CHECK_EQ(s.Count().wait_parks, 0u);

    // A contiguous read right after (the head is a sector on): no seek, 2 sectors = 1.9 ms after the drive's end
    // (the drive is busy until `end`, later in this frame). Unpolled: the BOUNDARY lands it once its frame has ended.
    s.Read(4, f0, 1000, 3000, dest + 0x1000);
    const double end2 = s.PendingEndMs(4);
    CHECK(end2 > end && end2 < end + 3.0);
    while (clock.BoundaryCount() * StreamService::kFrameMs < end2) clock.Boundary();
    CHECK_EQ(s.Count().boundary_delivered, 1u);
    CHECK_EQ(s.PendingEndMs(4), 0.0);
    CHECK(std::memcmp(te::GuestPointer(dest + 0x1000, 3000), expected.data() + 1000, 3000) == 0);

    // A WAIT parks whole frames until the drive is done, then lands: a read of the ELF (a short seek back: 42 ms).
    const int32_t f2 = s.OpenFile("\\SLES_525.68;1");
    const uint64_t before = clock.BoundaryCount();
    s.Read(5, f2, 0, 16, dest + 0x2000);
    const double end3 = s.PendingEndMs(5);
    CHECK(end3 > static_cast<double>(before) * StreamService::kFrameMs + 42.0);
    CHECK_EQ(s.Wait(5), 0);
    CHECK(std::memcmp(te::GuestPointer(dest + 0x2000, 16), "SLES_525.68-TEST", 16) == 0);
    CHECK_EQ(s.Count().wait_delivered, 1u);
    const uint64_t parked = clock.BoundaryCount() - before;
    CHECK_EQ(s.Count().wait_parks, parked);
    CHECK(parked >= 2);   // 42 ms of seek = 2 or 3 frames
    CHECK((clock.BoundaryCount() + 1) * StreamService::kFrameMs >= end3);
    CHECK(!s.IsReading(5));

    // A bank read lands in the audio registry at its time, not before.
    auto& a = te::audio::Service();
    a.Reset();
    a.ReserveSounds(0x100);
    const int32_t bank_file = s.OpenFile("CRASH6/BANK.BD");
    s.ReadSoundBank(2, 0x8002, bank_file, 0, 160);
    CHECK(!a.Audio().HasSound(0x8002));
    CHECK_EQ(s.Wait(2), 0);
    CHECK(a.Audio().HasSound(0x8002));

    // A second read on a channel still reading lands the first (forced), a close lands the file's reads.
    s.Read(6, f0, 0, 100, dest + 0x3000);
    s.Read(6, f0, 100, 100, dest + 0x3100);
    CHECK_EQ(s.Count().forced, 1u);
    CHECK(std::memcmp(te::GuestPointer(dest + 0x3000, 100), expected.data(), 100) == 0);
    s.CloseFile(f0);
    CHECK_EQ(s.Count().forced, 2u);
    CHECK(std::memcmp(te::GuestPointer(dest + 0x3100, 100), expected.data() + 100, 100) == 0);
    CHECK(!s.IsReading(6));

    // A missing file's read completes at once (nothing to transfer).
    const int32_t missing = s.OpenFile("Crash6/Nope.bin");
    s.Read(7, missing, 0, 64, dest + 0x5000);
    CHECK(!s.IsReading(7));

    // The reads in flight survive a save state: the bytes, the destination, the drive's end.
    const int32_t f3 = s.OpenFile("CRASH6/DATA.BD");
    s.Read(8, f3, 2000, 500, dest + 0x6000);
    const double end4 = s.PendingEndMs(8);
    CHECK(end4 > 0);
    std::vector<uint8_t> payload;
    s.Serialize(payload);
    const auto count = s.Count();
    s.Reset();
    te::Settings settings = te_test::DefaultSettings();
    settings.iso_path = g_iso;
    te_test::Reset(settings);
    te::Services::Get().clock().Start();
    std::string error;
    CHECK(s.Restore(payload, &error));
    if (!error.empty()) std::fprintf(stderr, "restore: %s\n", error.c_str());
    CHECK_EQ(s.PendingEndMs(8), end4);
    CHECK_EQ(s.Count().reads, count.reads);
    CHECK(s.IsReading(8));   // frame 0 of the new clock: the end lies far ahead
    CHECK_EQ(s.Wait(8), 0);
    CHECK(std::memcmp(te::GuestPointer(dest + 0x6000, 500), expected.data() + 2000, 500) == 0);
    // Loaded under the instant model, whatever was in flight is in place.
    s.Reset();
    settings.stream_instant = true;
    te_test::Reset(settings);
    std::fill(te::GuestPointer(dest + 0x6000, 500), te::GuestPointer(dest + 0x6000, 500) + 500, 0);
    CHECK(s.Restore(payload, &error));
    CHECK(s.Instant());
    CHECK_EQ(s.PendingEndMs(8), 0.0);
    CHECK(std::memcmp(te::GuestPointer(dest + 0x6000, 500), expected.data() + 2000, 500) == 0);
}

void TestFilesAndReads() {
    Fresh();
    auto& s = Service();
    CHECK_EQ(static_cast<int>(s.GetState()), 0);
    CHECK_EQ(s.Initialise(9, 0x400000), 0);
    CHECK_EQ(static_cast<int>(s.GetState()), 1);
    CHECK_EQ(s.Channels(), 9);

    // Paths as the game gives them: forward slashes, mixed case, no ";1".
    const int32_t f0 = s.OpenFile("Crash6/Data.bd");
    CHECK_EQ(f0, 0);
    CHECK_EQ(s.FileSize(), 5000u);
    const int32_t missing = s.OpenFile("Crash6/Nope.bin");
    CHECK_EQ(missing, 1);
    CHECK_EQ(s.FileSize(), 0u);     // "no such file": the game treats a size of 0 as missing
    const int32_t f2 = s.OpenFile("\\SLES_525.68;1");
    CHECK_EQ(f2, 2);
    CHECK_EQ(s.FileSize(), 16u);

    // A read is in guest memory when the call returns; IsReading is false; Wait returns 0.
    const uint32_t dest = 0x00400000;
    const auto expected = te_test::Pattern(5000, 0x11);
    s.Read(3, f0, 100, 1000, dest);
    CHECK(std::memcmp(te::GuestPointer(dest, 1000), expected.data() + 100, 1000) == 0);
    CHECK_EQ(te::GuestPointer(dest + 1000, 1)[0], 0);   // nothing past the request
    CHECK(!s.IsReading(3));
    CHECK_EQ(s.Wait(3), 0);
    CHECK_EQ(s.Count().reads, 1u);
    CHECK_EQ(s.Count().delivered, 1u);
    CHECK_EQ(s.Count().delivered_bytes, 1000u);
    s.Read(4, f0, 4000, 1000, dest + 0x1000);
    CHECK(std::memcmp(te::GuestPointer(dest + 0x1000, 1000), expected.data() + 4000, 1000) == 0);
    s.Read(5, f2, 0, 16, dest + 0x2000);
    CHECK(std::memcmp(te::GuestPointer(dest + 0x2000, 16), "SLES_525.68-TEST", 16) == 0);
    CHECK_EQ(s.Update(), 0);
    // A read past the end is short (clamped), never beyond guest memory.
    s.Read(7, f0, 4990, 100, dest + 0x4000);
    CHECK(std::memcmp(te::GuestPointer(dest + 0x4000, 10), expected.data() + 4990, 10) == 0);
    CHECK_EQ(te::GuestPointer(dest + 0x4000 + 10, 1)[0], 0);
    CHECK(s.Count().short_reads >= 1);
    // Reads on the missing file deliver nothing, counted.
    s.Read(8, missing, 0, 64, dest + 0x5000);
    CHECK_EQ(te::GuestPointer(dest + 0x5000, 1)[0], 0);
    CHECK(s.Count().bad_file >= 1);
    // Bad channels are refused, counted, never crash.
    s.Read(9, f0, 0, 16, dest);
    s.Read(-1, f0, 0, 16, dest);
    CHECK(!s.IsReading(42));
    CHECK_EQ(s.Wait(42), 0);
    CHECK(s.Count().bad_channel >= 4);
    // A destination outside guest memory is refused (logged), nothing written.
    s.Read(2, f0, 0, 16, 0xFFFFFFF0u);

    s.CloseFile(f0);
    s.CloseFile(missing);
    s.CloseFile(f2);
    CHECK_EQ(s.Count().closes, 3u);
    uint64_t off = 0; uint32_t size = 0;
    CHECK(!s.FileExtent(f0, off, size));
    // ReadFileBytes (the audio owner's music windows): whole range, clamped.
    const int32_t again = s.OpenFile("CRASH6/DATA.BD");
    std::vector<uint8_t> bytes;
    CHECK(s.ReadFileBytes(again, 4000, 2000, bytes));
    CHECK_EQ(bytes.size(), 1000u);
    CHECK(std::memcmp(bytes.data(), expected.data() + 4000, 1000) == 0);
    CHECK(!s.ReadFileBytes(77, 0, 16, bytes));
    s.CloseFile(again);
}

void TestSoundBanks() {
    Fresh();
    auto& s = Service();
    auto& a = te::audio::Service();
    a.Reset();
    a.ReserveSounds(0x100);
    CHECK_EQ(s.Initialise(9, 0), 0);
    const int32_t bank_file = s.OpenFile("CRASH6/BANK.BD");
    CHECK_EQ(s.FileSize(), 160u);
    const auto before = a.Count();
    // The bank's bytes (10 ADPCM blocks) go to the audio owner's registry under the bank ID, not into memory.
    s.ReadSoundBank(2, 0x8000, bank_file, 0, 160);
    CHECK(!s.IsReading(2));
    CHECK_EQ(s.Count().bank_reads, 1u);
    CHECK_EQ(s.Count().bank_delivered, 1u);
    CHECK_EQ(a.Count().banks_stored, before.banks_stored + 1);
    CHECK_EQ(a.Count().bank_bytes, before.bank_bytes + 160);
    CHECK(a.Audio().HasSound(0x8000));
    // Not ready until the game says the bank is loaded.
    CHECK_EQ(a.PlaySound(0x8000, 5, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1), -1);
    a.SoundBankLoaded(0x8000);
    CHECK_EQ(a.PlaySound(0x8000, 5, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1), 0);
    // A partial last block is cut to whole blocks and still stored.
    s.ReadSoundBank(3, 0x8001, bank_file, 0, 150);
    CHECK_EQ(s.Wait(3), 0);
    CHECK_EQ(a.Count().banks_stored, before.banks_stored + 2);
    CHECK_EQ(a.Count().bank_bytes, before.bank_bytes + 160 + 144);
    // The sound-memory cursor the game stores advances by what it reserves (16-byte granules), never 0.
    const uint32_t first = a.ReserveSoundMemory(0x8000);
    const uint32_t second = a.ReserveSoundMemory(0x8000);
    CHECK(first != 0);
    CHECK_EQ(second, first + 0x8000);
    CHECK_EQ(a.ReserveSoundMemory(17) - second, 0x8000u);
    s.CloseFile(bank_file);
}

void TestBufferPool() {
    Fresh();
    auto& s = Service();
    CHECK_EQ(s.Initialise(9, 0), 0);
    const uint32_t free0 = s.FreeBufferMemory();
    CHECK_EQ(free0, StreamService::kBufferPoolBytes);
    // The title's music buffers (2 blocks x 2 tracks x 2 = 0x50000 each, 3 players) and bank streams all fit.
    s.AttachBuffer(0, 0x800, 0, 0);
    s.AttachBuffer(1, 0x50000, 0x5010, 0x4000);
    s.AttachBuffer(2, 0, 0x9010, 0x4000);     // the paired second stream: no buffer of its own
    s.AttachBuffer(3, 0x50000, 0xD010, 0x4000);
    s.AttachBuffer(4, 0x50000, 0x15010, 0x4000);
    CHECK_EQ(s.FreeBufferMemory(), free0 - 0x800 - 3 * 0x50000);
    CHECK(s.ChannelBufferAddress(1) != 0);
    CHECK(s.ChannelBufferAddress(1) != s.ChannelBufferAddress(3));
    CHECK_EQ(s.ChannelBufferAddress(2), 0u);
    CHECK_EQ(s.ChannelBufferAddress(7), 0u);
    // Audio::ChannelBufferAddress is the same handle.
    CHECK_EQ(te::audio::Service().ChannelBufferAddress(1), s.ChannelBufferAddress(1));
    // Detach frees; a re-attach reuses the gap.
    const uint32_t gap = s.ChannelBufferAddress(1);
    s.DetachBuffer(1);
    CHECK_EQ(s.FreeBufferMemory(), free0 - 0x800 - 2 * 0x50000);
    s.AttachBuffer(5, 0x1000, 0, 0);
    CHECK_EQ(s.ChannelBufferAddress(5), gap);
    // A buffer the pool cannot hold is refused (counted), the rest untouched.
    s.AttachBuffer(6, StreamService::kBufferPoolBytes, 0, 0);
    CHECK_EQ(s.Count().attach_refused, 1u);
    CHECK_EQ(s.ChannelBufferAddress(6), 0u);
    s.DetachBuffer(0); s.DetachBuffer(3); s.DetachBuffer(4); s.DetachBuffer(5); s.DetachBuffer(2);
    CHECK_EQ(s.FreeBufferMemory(), free0);
}

void TestSeamBodies() {
    // The extern "C" bodies: a guest-address path, a guest-address destination.
    Fresh();
    CHECK_EQ(ps2e_stream_initialise(9, 0), 0);
    CHECK_EQ(ps2e_stream_get_state(), 1);
    const uint32_t path = te_test::WriteString(0x00300000, "crash6/data.bd");
    const ps2e_s32 file = ps2e_stream_open_file(path);
    CHECK_EQ(ps2e_stream_file_size(), 5000);
    ps2e_stream_read(1, file, 0, 32, 0x00500000);
    CHECK_EQ(ps2e_stream_is_reading(1), 0);
    CHECK(std::memcmp(te::GuestPointer(0x00500000, 32), te_test::Pattern(32, 0x11).data(), 32) == 0);
    CHECK_EQ(ps2e_stream_wait(1), 0);
    ps2e_stream_attach_buffer(1, 0x800, 0, 0);
    CHECK_EQ(ps2e_stream_free_buffer_memory(), static_cast<ps2e_s32>(StreamService::kBufferPoolBytes - 0x800));
    ps2e_stream_detach_buffer(1);
    ps2e_stream_close_file(file);
    CHECK_EQ(ps2e_stream_update(), 0);
}

void TestWithoutDisc() {
    te::Settings settings = te_test::DefaultSettings();
    settings.iso_path = "";
    te_test::Reset(settings);
    auto& s = Service();
    s.Reset();
    CHECK_EQ(s.Initialise(9, 0), -1);  // "can't read": no module without a disc
    CHECK_EQ(static_cast<int>(s.GetState()), 0);
    const int32_t f = s.OpenFile("X.BIN");
    CHECK_EQ(s.FileSize(), 0u);
    s.Read(0, f, 0, 16, 0x00400000);
    CHECK(!s.IsReading(0));
    CHECK_EQ(s.Count().delivered_bytes, 0u);
    CHECK_EQ(te::GuestPointer(0x00400000, 1)[0], 0);
    s.CloseFile(f);
}

// THE LOADS (te_loading.h): the game's state word at a boundary puts the stream service at the device's speed while the game
// is in a loading state with disc.speed = device -- a read in place in the call, what was in flight landed, no frame waited,
// the drive model's busy time untouched and its head left where the load's last read ended; LoadReadsDone 1 there and only
// there. At the console's pace, or in another state, the drive model.
void SetGameState(unsigned state) {
    te_test::WriteValue<uint32_t>(0x00309888, 0x7F0000);   // G_GameController (PAL) -> a controller in the test memory
    te_test::WriteValue<uint64_t>(0x7F0008, uint64_t(state) << 44);
}

void TestLoads() {
    for (const bool device : {true, false}) {
        te::Settings settings = te_test::DefaultSettings();
        settings.iso_path = g_iso;
        settings.stream_instant = false;
        settings.disc_speed = device ? te::Settings::DiscSpeed::Device : te::Settings::DiscSpeed::Console;
        te_test::Reset(settings);
        Service().Reset();
        te::audio::Service().Configure(false);
        te::loading::Install();
        auto& s = Service();
        te::Clock& clock = te::Services::Get().clock();
        clock.Start();
        CHECK_EQ(s.Initialise(9, 0x400000), 0);
        const int32_t f0 = s.OpenFile("Crash6/Data.bd");
        const auto expected = te_test::Pattern(5000, 0x11);
        const uint32_t dest = 0x00400000;
        // The menu (MainMenu 8): the drive model -- a read in flight, due in frame 2
        SetGameState(8);
        clock.Boundary();
        CHECK(!s.Loading());
        CHECK(ps2e_system_load_reads_done() == 0);
        s.Read(3, f0, 0, 1000, dest);
        CHECK(s.PendingEndMs(3) > 42.0);
        CHECK(s.IsReading(3));
        // LoadingLevel (9) from the next boundary on
        SetGameState(9);
        clock.Boundary();
        CHECK_EQ(s.Loading(), device);
        if (!device) {
            // the console's pace: the read lands at its drive end, a load's read is modelled, the loader waits
            CHECK(s.IsReading(3));
            CHECK(ps2e_system_load_reads_done() == 0);
            CHECK_EQ(s.Wait(3), 0);
            s.Read(4, f0, 1000, 3000, dest + 0x1000);
            CHECK(s.PendingEndMs(4) > 0);
            CHECK(ps2e_system_load_reads_done() == 0);
            CHECK_EQ(s.Wait(4), 0);
            continue;
        }
        // the read in flight as the load began was landed at that boundary (its bytes were read at its issue)
        CHECK_EQ(s.PendingEndMs(3), 0.0);
        CHECK(std::memcmp(te::GuestPointer(dest, 1000), expected.data(), 1000) == 0);
        CHECK(!s.IsReading(3));
        CHECK(ps2e_system_load_reads_done() == 1);   // nothing in flight: the loader goes on
        // reads in the load: in place in the call, no poll charged, no frame waited, counted -- the 35 sectors of PAD.BIN
        // (32 ms of drive) and the ELF beyond them
        const int32_t pad = s.OpenFile("CRASH6/PAD.BIN");
        const int32_t elf = s.OpenFile("\\SLES_525.68;1");
        const auto pad_bytes = te_test::Pattern(70000, 0x22);
        const uint64_t polls = s.Count().busy_polls, parks = s.Count().wait_parks, frame = clock.BoundaryCount();
        s.Read(4, pad, 0, 70000, dest + 0x10000);
        CHECK(std::memcmp(te::GuestPointer(dest + 0x10000, 70000), pad_bytes.data(), 70000) == 0);
        CHECK(!s.IsReading(4));
        CHECK_EQ(s.Wait(4), 0);
        auto& a = te::audio::Service();
        a.Reset();
        a.ReserveSounds(0x100);
        s.ReadSoundBank(2, 0x8001, s.OpenFile("CRASH6/BANK.BD"), 0, 160);   // a bank too: in the registry at once
        CHECK(a.Audio().HasSound(0x8001));
        s.Read(6, elf, 0, 16, dest + 0x2000);
        CHECK(std::memcmp(te::GuestPointer(dest + 0x2000, 16), "SLES_525.68-TEST", 16) == 0);
        CHECK_EQ(s.Count().busy_polls, polls);
        CHECK_EQ(s.Count().wait_parks, parks);
        CHECK_EQ(clock.BoundaryCount(), frame);
        CHECK_EQ(s.DeviceReads().reads, 3u);
        CHECK_EQ(s.DeviceReads().bytes, 70016u + 160u);
        CHECK(ps2e_system_load_reads_done() == 1);
        // two more frames of the load (the menu's read, landed at the load's start, would have ended in the second)
        clock.Boundary();
        clock.Boundary();
        CHECK(s.Loading());
        // StartingPlay (11): the drive model again. A read of the ELF seeks nothing (the head where the load's last read
        // ended, not at the data the menu read) and waits for nothing the load read (the model's busy time was not advanced
        // by the 32 ms of PAD.BIN and the seeks between the files: modelled, they would end past 170 ms): its end is one
        // sector's transfer after this frame's start
        SetGameState(11);
        clock.Boundary();
        CHECK(!s.Loading());
        CHECK(ps2e_system_load_reads_done() == 0);
        const double start = double(clock.BoundaryCount()) * StreamService::kFrameMs;
        s.Read(5, elf, 0, 16, dest + 0x3000);
        CHECK(s.PendingEndMs(5) > start && s.PendingEndMs(5) < start + 3.0);
        CHECK_EQ(s.Wait(5), 0);
        CHECK(std::memcmp(te::GuestPointer(dest + 0x3000, 16), "SLES_525.68-TEST", 16) == 0);
        s.CloseFile(f0);
    }
}
}  // namespace

int main() {
    std::vector<uint8_t> elf(16);
    std::memcpy(elf.data(), "SLES_525.68-TEST", 16);
    g_iso = te_test::WriteTestIso({
        {"CRASH6", "DATA.BD", te_test::Pattern(5000, 0x11)},
        {"CRASH6", "BANK.BD", te_test::AdpcmBlocks(10, 0x3, false)},
        {"CRASH6", "PAD.BIN", te_test::Pattern(70000, 0x22)},   // 35 sectors between the data and the ELF: a short seek
        {"", "SLES_525.68", elf},
    });
    if (g_iso.empty()) { std::fprintf(stderr, "cannot write the test ISO\n"); return 2; }

    TestFilesAndReads();
    TestSoundBanks();
    TestBufferPool();
    TestSeamBodies();
    TestWithoutDisc();
    TestDriveModel();
    TestLoads();

    std::remove(g_iso.c_str());
    return te_test::Finish("test_stream");
}
