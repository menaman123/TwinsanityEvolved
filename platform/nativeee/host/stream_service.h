// stream_service.h -- platform/nativeee's stream owner: Platform::Stream (stream.h), the disc's files read on
// channels, over the services' Disc (te_services.h: the engine's IsoFs on the one image). Host-typed so tests drive
// it directly; the extern "C" ps2e_stream_* bodies in stream.cpp are one-liners over Service().
//
// Model (the PS2 had STREAM.IRX on the IOP reading into IOP buffers and DMAing to the EE or the SPU2; here):
//   * a file is opened by its disc path and numbered from 0 as the PS2 module numbered its requests; FileSize()
//     is the last opened file's (0 when it was not found: the game then treats it as missing);
//   * a channel reads ONE thing at a time: Read() into guest memory, ReadSoundBank() into the audio owner's
//     sample registry (te::audio::Service().StoreSoundBank).
//   * DRIVE MODEL (the default): a read is ASYNCHRONOUS on the engine's deterministic drive timeline
//     (engine/services/read_timing.h -- the recomp's `cdvd.delivery_visibility=poll` arithmetic: a read issued in
//     frame k starts at max(drive free, k x 16.667 ms) and ends after the seek (0 / 42 / 112 ms by distance) and the
//     transfer (1.08 sectors a ms)). The bytes are read from the image at issue (immutable, like the engine's
//     Payload) and LAND in guest memory / the sample registry when the game can observe them, on the game thread:
//       - at a POLL (IsReading) whose frame contains the drive's end -- the console's EE saw a finished DVD read
//         mid-frame; a poll that finds the end past the frame's edge answers "reading" and CHARGES the clock the
//         EE time a STREAM status round trip cost (Clock::ChargeInFrame, 160 us), so the game's budget loops
//         (BackgroundWork's GameTimeHasTimeLeft) run out of frame as on the console instead of spinning;
//       - inside a WAIT (Wait): whole frames are PARKED (Clock::Boundary(): the field tick, the pads' sample, the
//         checkpoint -- the console's EE blocked on the IOP semaphore while the vblanks ran) until the end lies
//         inside the frame, then the read lands;
//       - at a boundary (the hook) when the end has passed and nobody polled: the bytes are in place next frame.
//     Everything is a function of the boundary count: deterministic, the same under play and deterministic.
//     THE CARD (the PS Vita: the bytes read by the disc reader thread, te::io): the drive model decides WHEN a read lands; the
//     card has to have given its bytes by then. Under the deterministic profile the landing waits for the reader (the model's
//     exact landings: the fixtures). In play it does not: a read whose drive time has come and whose bytes are still on the
//     card stays in flight -- a poll answers "reading" (charged as above), the boundary hook leaves it -- and lands at the first
//     poll or boundary after the reader has them, as a slow DVD's read did on the console: the game's loader polls and goes
//     on. A Wait parks whole frames for the card as it parks them for the drive. The census counts the reads put off
//     ("[te.stream] presents ...", LogWindow). TE_STREAM_WAIT_CARD=1: the landing waits for the card in play too (the A/B).
//     The decode of a streamed resource file is thereby paced by the drive (one reader block per delivery),
//     which is what keeps the game tick under a field (the streaming-freeze case: 267 models in one tick).
//   * INSTANT model (te::Settings::stream_instant, the pre-drive-model behaviour, kept for A/B): the bytes are in place
//     when the call returns, IsReading is false, Wait returns 0.
//   * A LOAD AT THE DEVICE'S SPEED (SetLoading, from te_loading.h's game state hook: the game behind its loading picture with
//     disc.speed = device): a read is not modelled -- its bytes land as soon as they are read: in the call where the read is
//     synchronous (a computer); on the PS Vita (the reader thread) at the first poll / boundary after the reader has them, a
//     poll waiting for the card while the frame's work is under three quarters of a field (past that it answers "reading"
//     and is charged as above, and the frame goes on to show the loading picture), in a wait at once; under the deterministic
//     profile every landing waits for the reader (the same landings as a synchronous read). The reads in flight when
//     it turns on land the same way. The drive model's busy time is not advanced by them; its head is left where their last
//     sector is, so the first read after the load seeks from there.
//   * AttachBuffer/DetachBuffer/FreeBufferMemory keep the IOP buffer pool's accounting (the game sizes its music
//     buffers against FreeBufferMemory) and mint the opaque per-channel buffer addresses Audio::
//     ChannelBufferAddress returns (the game hands one to the movie as lent memory; nobody dereferences it);
//   * GetState is Running after Initialise, Stopped (and Initialise -1) when there is no disc image; the disc-error
//     states are never entered (the image cannot be ejected).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "disc_reader.h"

class ReadTiming;

namespace te::stream {

class StreamService {
public:
    enum class State : int32_t { Stopped = 0, Running = 1, DiscError = 2, Recovering = 4 };
    static constexpr int32_t kNoFile = -1;
    // The IOP memory the module had for stream buffers (the PS2's 2 MB less the modules), and where it put them.
    static constexpr uint32_t kBufferPoolBytes = 0x1A0000;
    static constexpr uint32_t kBufferPoolBase = 0x00060000;
    // The drive model's frame (ReadTiming::kTickMs) and the EE time one status poll of a read in flight cost on the
    // console (the engine's LoaderSpinLift stream_rpc_service_us), in T0 counts (576000 a second).
    static constexpr double kFrameMs = 16667.0 / 1000.0;
    static constexpr uint64_t kPollChargeTicks = 92;   // 160 us

    // ----- stream.h --------------------------------------------------------------------------------------------
    int32_t Initialise(int32_t channels, uint32_t workMemory);
    int32_t Update();
    State GetState() const { return state_; }
    void AttachBuffer(int32_t channel, uint32_t size, uint32_t soundAddress, uint32_t soundSize);
    void DetachBuffer(int32_t channel);
    uint32_t FreeBufferMemory() const;
    int32_t OpenFile(const char* path);
    uint32_t FileSize() const { return last_size_; }
    void CloseFile(int32_t file);
    void Read(int32_t channel, int32_t file, uint32_t offset, uint32_t size, uint32_t destination);
    void ReadSoundBank(int32_t channel, uint32_t bank, int32_t file, uint32_t offset, uint32_t size);
    bool IsReading(int32_t channel);
    int32_t Wait(int32_t channel);

    // ----- the audio owner's side --------------------------------------------------------------------------------
    // The open file's extent on the image (byte offset of its first byte, its size). False for an unknown number.
    bool FileExtent(int32_t file, uint64_t& image_offset, uint32_t& size) const;
    // The channel's opaque IOP buffer address (0 when it has none).
    uint32_t ChannelBufferAddress(int32_t channel) const;
    // A whole-range read of an open file's bytes (the audio owner's music windows). Short at the file's end.
    // Synchronous and off the drive timeline (the owned music asset plays from memory).
    bool ReadFileBytes(int32_t file, uint32_t offset, uint32_t size, std::vector<uint8_t>& out);
    // The same range NOT read (the audio owner reads it in pieces off the game thread, te::io): its first image byte and
    // the count ReadFileBytes would deliver (the file's and the image's ends clamp it). False for an unknown file.
    bool FileRange(int32_t file, uint32_t offset, uint32_t size, uint64_t& image_byte, size_t& bytes) const;
    // ... and the counters ReadFileBytes keeps, for such a range taken
    void CountFileRead(uint32_t size, size_t bytes) { if (bytes != size) ++count_.short_reads; }

    // ----- the drive model's sites ---------------------------------------------------------------------------------
    // The boundary hook's work: land every read whose drive end lies inside the frame that just ended.
    void OnBoundary(uint64_t boundary);
    bool Instant() const { return instant_; }
    // Tests: the model regardless of te::Settings (before Initialise / with no reads pending).
    void SetInstant(bool instant);
    // A read in flight on the channel: its drive end (ms on the boundary timeline), 0 when none.
    double PendingEndMs(int32_t channel) const;

    // ----- a load at the device's speed (te_loading.h) -------------------------------------------------------------
    // On while the game loads behind its loading picture with disc.speed = device (the header's model above).
    void SetLoading(bool loading);
    bool Loading() const { return loading_; }
    // A read the game asked for is not in place yet (any model)
    bool InFlight() const { return !pending_.empty(); }

    // ----- inspection / tests ----------------------------------------------------------------------------------
    struct Counters {
        uint64_t opens = 0, opens_missing = 0, closes = 0, reads = 0, bank_reads = 0, delivered = 0,
                 delivered_bytes = 0, bank_delivered = 0, short_reads = 0, polls = 0, waits = 0, updates = 0,
                 attaches = 0, detaches = 0, attach_refused = 0, bad_channel = 0, bad_file = 0;
        // where the bytes landed (poll / boundary / wait), the frames waits parked, the polls that found the
        // drive busy (each charged), the reads landed early (a close / detach / a second read on the channel).
        uint64_t poll_delivered = 0, boundary_delivered = 0, wait_delivered = 0, wait_parks = 0, busy_polls = 0,
                 forced = 0;
    };
    Counters Count() const { return count_; }
    // The reads of loads at the device's speed (te_loading.h's census; not state): how many, their bytes, the time the calling
    // thread spent reading them (the synchronous reads: a computer's)
    struct DeviceCount { uint64_t reads = 0, bytes = 0; double read_ms = 0; };
    DeviceCount DeviceReads() const { return device_; }
    // The landings and the card (not state): the reads whose drive time came before the card gave their bytes (and their
    // bytes), the polls / boundaries that found such a read still reading, the frames a Wait parked for the card, the forced
    // landings (a close, a second read) that waited for it; the archive entries read ahead from their first read
    struct CardCount {
        uint64_t late_reads = 0, late_bytes = 0, late_polls = 0, late_boundaries = 0, parks = 0, forced_waits = 0, entries_ahead = 0;
    };
    CardCount Card() const { return card_; }
    int32_t Channels() const { return channels_; }
    void Reset();   // tests: everything as before Initialise
    // Save state (savestate.h): the open files, the channel buffers, the state and counters, the reads in flight.
    void Serialize(std::vector<uint8_t>& out) const;
    bool Restore(const std::vector<uint8_t>& payload, std::string* error);

private:
    struct File { uint64_t image_offset = 0; uint32_t size = 0; std::string path; bool found = false; };
    struct Buffer { uint32_t address = 0, size = 0, sound_address = 0, sound_size = 0; };
    // A read in flight (one per channel): what was asked, the bytes, the drive's end. The bytes are read at issue -- on the
    // calling thread, or (te::io::Async(): the Vita) by the disc reader thread into `read`, waited for at the landing; the
    // drive model's timeline is the same either way (it needs their count, known from the extents at issue).
    struct Pending {
        int32_t file = kNoFile;
        uint32_t offset = 0, size = 0, destination = 0;
        bool bank = false;
        uint32_t bank_id = 0;
        uint64_t issue_boundary = 0;
        double end_ms = 0;
        bool device = false;   // read at the device's speed (a load): in place once its bytes are, whatever end_ms says
        bool late = false;     // its drive time came before the card's bytes (counted once)
        std::vector<uint8_t> bytes;
        te::io::Handle<te::io::Job> read;
    };
    // The bytes ReadRange would return for the range (the file's and the image's ends clamp it), without reading them
    size_t ExpectedBytes(uint64_t image_offset, uint32_t file_size, uint32_t offset, uint32_t size) const;
    // A pending read's bytes (an async read waited for; a save state's)
    static const std::vector<uint8_t>& PendingBytes(const Pending& p);
    bool ValidChannel(int32_t channel) const;
    uint32_t AllocateBuffer(uint32_t size) const;
    // Reads [offset, offset+size) of the file (clamped to the file) into out; false for an unknown file.
    bool ReadRange(int32_t file, uint32_t offset, uint32_t size, std::vector<uint8_t>& out);
    void Issue(int32_t channel, int32_t file, uint32_t offset, uint32_t size, uint32_t destination, bool bank, uint32_t bank_id);
    void Land(int32_t channel, Pending& p);
    void LandPending(int32_t channel, const char* site);
    void ForceChannel(int32_t channel);
    // A pending read is in place now: a read at the device's speed once its bytes are (the reader thread done with them), any
    // other once its drive end lies inside the frame that ends at `frame_end_ms`
    static bool Due(const Pending& p, double frame_end_ms);
    // Whether a landing waits for the card (the deterministic profile; TE_STREAM_WAIT_CARD=1)
    static bool LandingWaits();
    void NoteLate(Pending& p);
    // A read of the archive at `at`: the rest of its entry read ahead whole (te::io::ReadAhead, Ahead::Entry)
    void AheadEntry(uint64_t at, size_t bytes);
    // A poll of a load's read the reader thread still has (play): wait for it, while this frame's work is under three quarters
    // of a field (te::Clock::MsSinceRelease) -- the reads at the card's own speed, the loading picture's frames kept
    static bool LoadPollWaits();
    static double LoadPollBudgetMs();   // what is left of it (ms; <= 0: none)
    uint64_t Boundary() const;
    uint64_t DriveFrame() const;   // the drive model's frame clock (the boundaries; the fields in Vita play mode)
    // the drive clock's frame in ms: kFrameMs at 60 Hz, 20 at the 50 Hz game rate
    static double FrameMs();
    double FrameEndMs() const;
    void EnsureTiming();

    State state_ = State::Stopped;
    int32_t channels_ = 0;
    int32_t next_file_ = 0;
    uint32_t last_size_ = 0;
    bool instant_ = false, instant_set_ = false, hooked_ = false;
    bool loading_ = false;
    DeviceCount device_;
    CardCount card_;
    std::map<int32_t, File> files_;
    std::map<int32_t, Buffer> buffers_;
    std::map<int32_t, Pending> pending_;
    std::unique_ptr<ReadTiming> timing_;
    Counters count_;
};

// The process-wide service the ps2e_* bodies use.
StreamService& Service();
// The window's line of the landings and the card (te_prof.cpp beside the reader's; at exit with from = to = 0): nothing when
// nothing was put off in a window
void LogWindow(uint64_t from, uint64_t to);

}  // namespace te::stream
