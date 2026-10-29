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
//     The decode of a streamed resource file is thereby paced by the drive (one reader block per delivery),
//     which is what keeps the game tick under a field (the streaming-freeze case: 267 models in one tick).
//   * INSTANT model (te::Settings::stream_instant, the pre-drive-model behaviour, kept for A/B): the bytes are in place
//     when the call returns, IsReading is false, Wait returns 0.
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
    std::map<int32_t, File> files_;
    std::map<int32_t, Buffer> buffers_;
    std::map<int32_t, Pending> pending_;
    std::unique_ptr<ReadTiming> timing_;
    Counters count_;
};

// The process-wide service the ps2e_* bodies use.
StreamService& Service();

}  // namespace te::stream
