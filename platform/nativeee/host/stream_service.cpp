// stream_service.cpp -- see stream_service.h.
#include "stream_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio_service.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/core/config/config.h"
#include "engine/services/read_timing.h"
#include "savestate.h"
#include "te_host.h"
#include "te_link_ahead.h"
#include "te_prof.h"
#include "te_services.h"

namespace te::stream {
namespace {
// TE_STREAM_TRACE=1 in the environment: one line per open/read/bank read/landing on stderr (debugging the boot's file
// set and the drive timeline).
bool Trace() {
    static const bool on = std::getenv("TE_STREAM_TRACE") && std::getenv("TE_STREAM_TRACE")[0] == '1';
    return on;
}
// TE_STREAM_DRIVE_SCALE=x: the drive's time scaled (1 = the modelled DVD; 0.25 = a drive four times as fast; floor 0.01).
// The deliveries stay deterministic at any scale.
double DriveScale() {
    static const double scale = [] {
        const char* s = std::getenv("TE_STREAM_DRIVE_SCALE");
        const double v = s ? std::atof(s) : 1.0;
        return v > 0.01 ? v : (s ? 0.01 : 1.0);
    }();
    return scale;
}
// TE_STREAM_WAIT_CARD=1: in play a landing waits for the card as it did before (the game thread blocked until the reader
// thread has the bytes, at the drive model's time), and a load's poll for as long as the card takes -- for an A/B against
// the default, where the read stays in flight
bool WaitCardKnob() {
    static const bool on = std::getenv("TE_STREAM_WAIT_CARD") && std::getenv("TE_STREAM_WAIT_CARD")[0] == '1';
    return on;
}
// A read of the archive this close to its entry's end needs no read-ahead of the rest (the span read-ahead brings 256 KB)
constexpr size_t kEntryAheadMin = 256 * 1024;
// ... and an entry's rest is read ahead up to this much
constexpr uint64_t kEntryAheadMax = 8u << 20;
// A wait parks frames for the card at most this long, then waits on the reader (its own fallback reads a job still queued)
constexpr double kCardParkMaxMs = 3000.0;
// The services' disc (opened on first use: a seam body reached before System::Initialise constructs what it needs).
const IsoFs* Disc() {
    te::Disc& disc = te::Services::Get().disc();
    disc.Initialise();
    return disc.IsOpen() ? &disc.Iso() : nullptr;
}
inline uint64_t DoubleBits(double v) { uint64_t u; std::memcpy(&u, &v, sizeof u); return u; }
inline double BitsDouble(uint64_t u) { double v; std::memcpy(&v, &u, sizeof v); return v; }
}  // namespace

void StreamService::Reset() {
    files_.clear();
    buffers_.clear();
    pending_.clear();
    timing_.reset();
    state_ = State::Stopped;
    channels_ = 0;
    next_file_ = 0;
    last_size_ = 0;
    hooked_ = false;
    instant_set_ = false;
    loading_ = false;
    device_ = {};
    count_ = {};
    card_ = {};
}

void StreamService::SetInstant(bool instant) {
    instant_ = instant;
    instant_set_ = true;
}

void StreamService::SetLoading(bool loading) {
    if (loading == loading_) return;
    loading_ = loading;
    te::io::PauseLinks(loading);   // a load's reads have the card to themselves (the linked chunks after it)
    // what is in flight as a load begins lands as soon as its bytes are there (the reads issued in the load stay so after it)
    if (loading_)
        for (auto& [channel, p] : pending_) p.device = true;
}

void StreamService::EnsureTiming() {
    if (!instant_set_) {
        instant_ = te::GetSettings().stream_instant;
        instant_set_ = true;
    }
    if (!timing_) {
        ps2r::CdvdConfig cfg;
        cfg.latency_model = ps2r::CdvdLatencyModel::Modelled;
        cfg.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
        cfg.poll_latency_scale = DriveScale();
        cfg.latency_scale = DriveScale();
        timing_.reset(new ReadTiming(cfg));
    }
    static bool exit_line = false;
    if (!exit_line && te::io::Async()) {
        exit_line = true;
        std::atexit([] { LogWindow(0, 0); });   // the run's landings for the card at exit
    }
    if (!hooked_) {
        // The boundary hook: the reads whose drive end lies inside the frame that ended land before the next frame.
        // Through the registry (Services::ResetForTest rebuilds the clock; the hook is re-added by the next Initialise).
        te::Services::Get().clock().AddBoundaryHook("stream drive", [](uint64_t boundary) { Service().OnBoundary(boundary); });
        hooked_ = true;
    }
}

int32_t StreamService::Initialise(int32_t channels, uint32_t workMemory) {
    (void)workMemory;   // the PS2 module's fast-load server ran in it; nothing runs there now
    if (channels <= 0 || channels > 256) return -1;
    channels_ = channels;
    if (!Disc()) {
        // "Returns negative when it can't read": no image, no stream module. The state stays Stopped (the game's
        // StreamSystem then reports it is not Running); opens find nothing, reads deliver nothing.
        std::fprintf(stderr, "[twinsanityevolved] stream: no disc image open -- Initialise fails\n");
        state_ = State::Stopped;
        return -1;
    }
    EnsureTiming();
    if (te::GetSettings().verbose || Trace())
        std::fprintf(stderr, "[twinsanityevolved] stream: %s model%s\n", instant_ ? "instant" : "drive",
                     instant_ ? "" : (DriveScale() == 1.0 ? " (the modelled DVD)" : " (scaled)"));
    state_ = State::Running;
    return 0;
}

bool StreamService::ValidChannel(int32_t channel) const {
    return channel >= 0 && channel < channels_;
}

int32_t StreamService::Update() {
    ++count_.updates;
    return state_ == State::Running ? 0 : static_cast<int32_t>(state_);
}

uint32_t StreamService::AllocateBuffer(uint32_t size) const {
    // First fit over the pool, 64-byte granules (the IOP module's alignment).
    const uint32_t granule = 0x40;
    const uint32_t rounded = (size + granule - 1) & ~(granule - 1);
    std::vector<std::pair<uint32_t, uint32_t>> used;   // (address, size), sorted
    for (const auto& [channel, buffer] : buffers_) if (buffer.size) used.emplace_back(buffer.address, buffer.size);
    std::sort(used.begin(), used.end());
    uint32_t cursor = kBufferPoolBase;
    for (const auto& [address, used_size] : used) {
        if (address - cursor >= rounded) return cursor;
        cursor = std::max(cursor, address + used_size);
    }
    if (kBufferPoolBase + kBufferPoolBytes - cursor >= rounded) return cursor;
    return 0;
}

void StreamService::AttachBuffer(int32_t channel, uint32_t size, uint32_t soundAddress, uint32_t soundSize) {
    if (!ValidChannel(channel)) { ++count_.bad_channel; return; }
    DetachBuffer(channel);
    const uint32_t granule = 0x40;
    const uint32_t rounded = (size + granule - 1) & ~(granule - 1);
    const uint32_t address = AllocateBuffer(rounded);
    if (rounded && address == 0) {
        ++count_.attach_refused;
        std::fprintf(stderr, "[twinsanityevolved] stream: buffer pool full (channel %d wants %u bytes)\n", channel, size);
        return;
    }
    Buffer buffer;
    buffer.address = rounded ? address : 0;
    buffer.size = rounded;
    buffer.sound_address = soundAddress;
    buffer.sound_size = soundSize ? soundSize : size;
    buffers_[channel] = buffer;
    ++count_.attaches;
}

void StreamService::DetachBuffer(int32_t channel) {
    if (!ValidChannel(channel)) { ++count_.bad_channel; return; }
    ForceChannel(channel);   // a read in flight on the channel lands before its buffer goes
    if (buffers_.erase(channel)) ++count_.detaches;
}

uint32_t StreamService::FreeBufferMemory() const {
    uint32_t used = 0;
    for (const auto& [channel, buffer] : buffers_) used += buffer.size;
    return used >= kBufferPoolBytes ? 0 : kBufferPoolBytes - used;
}

uint32_t StreamService::ChannelBufferAddress(int32_t channel) const {
    const auto it = buffers_.find(channel);
    return it == buffers_.end() ? 0 : it->second.address;
}

int32_t StreamService::OpenFile(const char* path) {
    // The game's paths ("Crash6/Crash/Crash.bd"): IsoFs accepts either separator, any case, with or without ";1".
    File file;
    file.path = path ? path : "";
    IsoFs::Entry entry;
    const IsoFs* iso = Disc();
    if (iso && !file.path.empty() && iso->Find(file.path, entry) && !entry.directory) {
        file.found = true;
        file.image_offset = uint64_t{entry.lba} * IsoFs::kSectorSize;
        file.size = entry.size;
    } else {
        ++count_.opens_missing;
    }
    const int32_t number = next_file_++;
    files_[number] = file;
    last_size_ = file.found ? file.size : 0;
    ++count_.opens;
    if (Trace()) std::fprintf(stderr, "[stream] open #%d \"%s\" -> %s %u bytes\n", number, file.path.c_str(), file.found ? "found" : "MISSING", last_size_);
    return number;
}

void StreamService::CloseFile(int32_t file) {
    // A read in flight on the file lands first (the game waits before it closes; a close under a read is the
    // module's "forced" completion, never lost bytes).
    std::vector<int32_t> on_file;
    for (const auto& [channel, p] : pending_) if (p.file == file) on_file.push_back(channel);
    for (int32_t channel : on_file) ForceChannel(channel);
    if (files_.erase(file)) ++count_.closes;
    else ++count_.bad_file;
}

bool StreamService::FileExtent(int32_t file, uint64_t& image_offset, uint32_t& size) const {
    const auto it = files_.find(file);
    if (it == files_.end() || !it->second.found) return false;
    image_offset = it->second.image_offset;
    size = it->second.size;
    return true;
}

bool StreamService::ReadRange(int32_t file, uint32_t offset, uint32_t size, std::vector<uint8_t>& out) {
    uint64_t image_offset = 0;
    uint32_t file_size = 0;
    out.clear();
    if (!FileExtent(file, image_offset, file_size)) { ++count_.bad_file; return false; }
    const uint32_t clamped = offset <= file_size ? std::min<uint32_t>(size, file_size - offset) : 0;
    out.resize(clamped);
    const IsoFs* iso = Disc();
    std::size_t got = 0;
    if (clamped && iso) {
        TE_PROF_SCOPE("disc read (stream)");
        got = iso->ReadBytes(image_offset + offset, clamped, out.data());
    }
    out.resize(got);
    if (got != size) ++count_.short_reads;
    return true;
}

bool StreamService::ReadFileBytes(int32_t file, uint32_t offset, uint32_t size, std::vector<uint8_t>& out) {
    return ReadRange(file, offset, size, out);
}

bool StreamService::FileRange(int32_t file, uint32_t offset, uint32_t size, uint64_t& image_byte, size_t& bytes) const {
    uint64_t image_offset = 0;
    uint32_t file_size = 0;
    bytes = 0;
    if (!FileExtent(file, image_offset, file_size)) return false;
    bytes = ExpectedBytes(image_offset, file_size, offset, size);
    image_byte = image_offset + offset;
    return true;
}

size_t StreamService::ExpectedBytes(uint64_t image_offset, uint32_t file_size, uint32_t offset, uint32_t size) const {
    // ReadRange's clamp to the file, then IsoFs::ReadBytes's to the image
    const uint32_t clamped = offset <= file_size ? std::min<uint32_t>(size, file_size - offset) : 0;
    const IsoFs* iso = Disc();
    if (!clamped || !iso) return 0;
    const uint64_t at = image_offset + offset;
    if (at >= iso->ImageBytes()) return 0;
    return static_cast<size_t>(std::min<uint64_t>(clamped, iso->ImageBytes() - at));
}

const std::vector<uint8_t>& StreamService::PendingBytes(const Pending& p) {
    if (!p.read) return p.bytes;
    te::io::Wait(p.read.get(), "a save state");
    return p.read->bytes;
}

uint64_t StreamService::Boundary() const { return te::Services::Get().clock().BoundaryCount(); }
// The drive model's frame clock: the boundaries (one field each under the deterministic profile -- unchanged), except in
// play mode on the Vita, where a boundary may span several fields (a late frame; the tick lock's 30 / 20 Hz picture ticks:
// te_tick_lock.h) and the drive is timed by the fields the wall produced -- a load takes the same wall time at 30 as at 60.
uint64_t StreamService::DriveFrame() const {
#if defined(__vita__)
    if (!te::GetSettings().deterministic) return te::Services::Get().clock().Fields();
#endif
    return Boundary();
}
// A frame of the drive's clock is one field of the game rate: 16.667 ms at 60 (as ever), 20 ms at the PAL disc's own
// 50 Hz (game.hz = 50 / --hz 50) -- at 50 Hz every frame was credited the 60 Hz field's 16.667 ms of drive time, so a load
// took 1.2 times its drive time in wall time (and in frames, 1.2 times the frames the PAL console took)
double StreamService::FrameMs() { return te::GetSettings().field_hz == 50 ? 20.0 : kFrameMs; }
double StreamService::FrameEndMs() const { return static_cast<double>(DriveFrame() + 1) * FrameMs(); }

double StreamService::PendingEndMs(int32_t channel) const {
    const auto it = pending_.find(channel);
    return it == pending_.end() ? 0.0 : it->second.end_ms;
}

// The bytes land: guest memory (a Read) or the audio owner's sample registry (a bank read).
void StreamService::Land(int32_t channel, Pending& p) {
    (void)channel;
    if (p.read) {
        // The reader thread's bytes (it has had the frames since the issue; a wait is counted and logged by te::io)
        te::io::Wait(p.read.get(), p.bank ? "a sound bank's landing" : "a stream read's landing");
        if (p.read->got != p.read->size)
            std::fprintf(stderr, "[twinsanityevolved] stream: the card gave %u of the %u bytes of #%d +0x%X\n", unsigned(p.read->got), unsigned(p.read->size),
                         p.file, p.offset);
        p.bytes = std::move(p.read->bytes);
        p.read.reset();
    }
    ++count_.delivered;
    count_.delivered_bytes += p.bytes.size();
    if (p.bank) {
        ++count_.bank_delivered;
        te::audio::Service().StoreSoundBank(p.bank_id, std::move(p.bytes));
        return;
    }
    if (p.bytes.empty()) return;
    if (uint8_t* dst = te::GuestPointer(p.destination, p.bytes.size())) {
        std::memcpy(dst, p.bytes.data(), p.bytes.size());
    } else {
        std::fprintf(stderr, "[twinsanityevolved] stream: read of %zu bytes to 0x%08X is outside guest memory\n",
                     p.bytes.size(), p.destination);
    }
}

void StreamService::LandPending(int32_t channel, const char* site) {
    const auto it = pending_.find(channel);
    if (it == pending_.end()) return;
    Pending p = std::move(it->second);
    pending_.erase(it);
    if (Trace())
        std::fprintf(stderr, "[stream] ch%d landed at %s: #%d +0x%X %zu bytes (issued @%llu, drive end %.1f ms, frame %llu)\n", channel,
                     site, p.file, p.offset, p.read ? p.read->size : p.bytes.size(), (unsigned long long)p.issue_boundary, p.end_ms,
                     (unsigned long long)Boundary());
    Land(channel, p);
}

bool StreamService::LandingWaits() { return te::GetSettings().deterministic || WaitCardKnob(); }

bool StreamService::Due(const Pending& p, double frame_end_ms) {
    // a load's read: in place once its bytes are -- at once under the deterministic profile (the landing waits for the reader
    // thread: the same landings as a computer's synchronous read), else once the reader thread is done with it
    if (p.device) return !p.read || te::GetSettings().deterministic || te::io::Done(p.read.get());
    // the drive model's time; and in play, a read on the reader thread once the card has given its bytes too (the card slower
    // than the drive: the read stays in flight and lands at the first poll / boundary after that -- the game polls on, as it
    // did with a slow DVD; under the deterministic profile the landing waits for them: the model's exact landings)
    return p.end_ms <= frame_end_ms && (!p.read || LandingWaits() || te::io::Done(p.read.get()));
}

void StreamService::NoteLate(Pending& p) {
    // the drive model's time has come and the card has not given the bytes: the read put off (once per read)
    if (p.late) return;
    p.late = true;
    ++card_.late_reads;
    card_.late_bytes += p.read ? p.read->size : 0;
}

bool StreamService::LoadPollWaits() { return LoadPollBudgetMs() > 0; }

double StreamService::LoadPollBudgetMs() {
    // a frame of a load spends up to three quarters of its field on the load (the card's reads and the game's parse of what
    // they bring); the rest is the loading picture's
    const double field_ms = te::GetSettings().field_hz == 50 ? 20.0 : kFrameMs;
    return 0.75 * field_ms - te::Services::Get().clock().MsSinceRelease();
}

void StreamService::ForceChannel(int32_t channel) {
    const auto it = pending_.find(channel);
    if (it == pending_.end()) return;
    if (it->second.read && !te::io::Done(it->second.read.get())) ++card_.forced_waits;   // (the landing waits for the card)
    ++count_.forced;
    LandPending(channel, "force");
}

void StreamService::AheadEntry(uint64_t at, size_t bytes) {
    // the rest of the archive entry this read starts (the game reads its entries through: 128 KB blocks every few frames,
    // pieces several a frame): read ahead whole in the reader's big pieces, this read its first (disc_reader.h, Ahead::Entry)
    if (!te::io::AheadBudget()) return;
    archive::Entry entry;
    if (!archive::EntryAt(at, entry)) return;
    const uint64_t end = std::min<uint64_t>(entry.image + entry.size, at + kEntryAheadMax);
    if (at + bytes > end || end - (at + bytes) <= kEntryAheadMin) return;
    const IsoFs* iso = Disc();
    if (!iso || te::io::AheadHas(iso, at, at + bytes)) return;
    if (te::io::ReadAhead(iso, at, end, te::io::Ahead::Entry, "entry", bytes)) ++card_.entries_ahead;
}

void StreamService::Issue(int32_t channel, int32_t file, uint32_t offset, uint32_t size, uint32_t destination, bool bank, uint32_t bank_id) {
    EnsureTiming();
    ForceChannel(channel);   // one read at a time on a channel
    Pending p;
    p.file = file;
    p.offset = offset;
    p.size = size;
    p.destination = destination;
    p.bank = bank;
    p.bank_id = bank_id;
    p.issue_boundary = Boundary();
    uint64_t image_offset = 0;
    uint32_t file_size = 0;
    const bool known = FileExtent(file, image_offset, file_size);
    size_t bytes = 0;
    if (!instant_ && known && te::io::Async()) {
        // Off the game thread: the reader thread reads the range now, the landing waits for it. The drive model needs only
        // the count, which the extents give (ReadRange's clamps).
        bytes = ExpectedBytes(image_offset, file_size, offset, size);
        if (bytes) {
            AheadEntry(image_offset + offset, bytes);
            p.read.reset(new te::io::Job);
            p.read->offset = image_offset + offset;
            p.read->size = bytes;
            p.read->site = bank ? "sound bank" : "stream";
            te::io::Submit(p.read.get(), Disc());
        }
        if (bytes != size) ++count_.short_reads;
    } else if (loading_) {
        // a load at the device's speed: the device's own read, timed for the census
        const auto t0 = std::chrono::steady_clock::now();
        ReadRange(file, offset, size, p.bytes);
        device_.read_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bytes = p.bytes.size();
    } else {
        ReadRange(file, offset, size, p.bytes);
        bytes = p.bytes.size();
    }
    if (loading_ && known && bytes) {
        // A load at the device's speed: its reads counted, and the drive model's head left where this one ends (the first
        // read after the load seeks from there; the model's busy time is not advanced)
        ++device_.reads;
        device_.bytes += bytes;
        ReadTiming::Position at = timing_->Capture();
        at.head_lba = static_cast<uint32_t>((image_offset + offset + bytes - 1) / IsoFs::kSectorSize + 1);
        timing_->Restore(at);
        if (Trace())
            std::fprintf(stderr, "[stream] ch%d read at the device's speed: #%d +0x%X %zu bytes @%llu (%s)\n", channel, file, offset, bytes,
                         (unsigned long long)p.issue_boundary, p.read ? "on the reader thread" : "in the call");
    }
    if (instant_ || !known || bytes == 0 || (loading_ && !p.read)) {
        // The instant model, nothing to transfer (an unknown/closed file, a read past the end), or a load's read whose bytes
        // are here (read in the call): complete now.
        Land(channel, p);
        return;
    }
    if (loading_) {
        // A load's read on the reader thread (the PS Vita): in place once the card's read ends -- the first poll, boundary or
        // wait after that (a save state records it due at once: this frame's end)
        p.device = true;
        p.end_ms = FrameEndMs();
        pending_[channel] = std::move(p);
        return;
    }
    // The drive: issued at this frame's start, after whatever the drive is still doing; the seek from where its head
    // was, then the sectors the range covers.
    const uint64_t first = (image_offset + offset) / IsoFs::kSectorSize;
    const uint64_t last = (image_offset + offset + bytes - 1) / IsoFs::kSectorSize;
    const uint32_t lba = static_cast<uint32_t>(first);
    const uint32_t sectors = static_cast<uint32_t>(last - first + 1);
    const uint64_t tick = DriveFrame();   // (= p.issue_boundary under the deterministic profile)
    timing_->DueAtMs(tick, static_cast<double>(tick) * FrameMs(), lba, sectors);
    p.end_ms = timing_->Capture().last_end_ms;
    if (Trace())
        std::fprintf(stderr, "[stream] ch%d issued #%d +0x%X %u bytes lba %u x%u @%llu -> drive end %.1f ms (frame edge %.1f)\n", channel, file,
                     offset, size, lba, sectors, (unsigned long long)tick, p.end_ms, FrameEndMs());
    pending_[channel] = std::move(p);
}

void StreamService::Read(int32_t channel, int32_t file, uint32_t offset, uint32_t size, uint32_t destination) {
    if (!ValidChannel(channel)) { ++count_.bad_channel; return; }
    ++count_.reads;
    if (Trace()) std::fprintf(stderr, "[stream] ch%d read #%d +0x%X %u bytes -> 0x%08X\n", channel, file, offset, size, destination);
    Issue(channel, file, offset, size, destination, false, 0);
}

void StreamService::ReadSoundBank(int32_t channel, uint32_t bank, int32_t file, uint32_t offset, uint32_t size) {
    if (!ValidChannel(channel)) { ++count_.bad_channel; return; }
    ++count_.bank_reads;
    if (Trace()) std::fprintf(stderr, "[stream] ch%d bank 0x%X read #%d +0x%X %u bytes\n", channel, bank, file, offset, size);
    Issue(channel, file, offset, size, 0, true, bank);
}

bool StreamService::IsReading(int32_t channel) {
    ++count_.polls;
    if (!ValidChannel(channel)) { ++count_.bad_channel; return false; }
    const auto it = pending_.find(channel);
    if (it == pending_.end()) return false;
    const double edge = FrameEndMs();
    // A load's read the reader thread still has: the poll waits for the card for what is left of this frame's three quarters
    // of a field (te::Clock::MsSinceRelease) -- the reads at the card's own speed, the loading picture's frames kept
    // (TE_STREAM_WAIT_CARD=1: the poll waits for the card however long, as before)
    const bool load_read = it->second.device && it->second.read && !Due(it->second, edge) && LoadPollWaits() &&
                           (WaitCardKnob() || te::io::WaitFor(it->second.read.get(), LoadPollBudgetMs(), "a load's poll"));
    if (load_read || Due(it->second, edge)) {
        // The drive finishes inside this frame: the console's EE saw the read complete at this poll. A load's read: the
        // reader thread has its bytes
        ++count_.poll_delivered;
        LandPending(channel, "poll");
        return false;
    }
    // Still reading: the poll cost the EE a STREAM status round trip (the frame budget loops see it)
    if (!it->second.device && it->second.end_ms <= edge) {
        NoteLate(it->second);   // (the drive is done; the card is not)
        ++card_.late_polls;
    }
    ++count_.busy_polls;
    te::Services::Get().clock().ChargeInFrame(kPollChargeTicks);
    if (Trace() && (count_.busy_polls % 500) == 0)
        std::fprintf(stderr, "[stream] ch%d busy poll x%llu: frame %llu ticks %llu in-frame %llu (drive end %.1f, edge %.1f)\n", channel,
                     (unsigned long long)count_.busy_polls, (unsigned long long)Boundary(), (unsigned long long)te::Services::Get().clock().Ticks(),
                     (unsigned long long)te::Services::Get().clock().InFrameTicks(), it->second.end_ms, FrameEndMs());
    return true;
}

int32_t StreamService::Wait(int32_t channel) {
    ++count_.waits;
    if (!ValidChannel(channel)) { ++count_.bad_channel; return 0; }
    // The EE blocks until the drive is done: whole frames pass (the boundary: field tick, pads, checkpoint) while the
    // end lies past the frame's edge. The map may change under Boundary() (the hook lands other channels; a
    // checkpoint may resume in another process): re-find after each park.
    double card_since = -1;
    while (true) {
        const auto it = pending_.find(channel);
        if (it == pending_.end()) return 0;
        Pending& p = it->second;
        if (p.device) break;   // (a load's read: the landing waits for its bytes)
        if (p.end_ms > FrameEndMs()) {
            ++count_.wait_parks;
            te::Services::Get().clock().Boundary();
            continue;
        }
        if (!p.read || LandingWaits() || te::io::Done(p.read.get())) break;
        // In play the card slower than the drive: whole frames parked for it as for the drive (the boundary's field tick,
        // the pads, the sound), not the game thread blocked in the reader's wait; past kCardParkMaxMs the landing waits
        const double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (card_since < 0) card_since = now;
        if (now - card_since > kCardParkMaxMs) break;
        NoteLate(p);
        ++card_.parks;
        te::Services::Get().clock().Boundary();
    }
    ++count_.wait_delivered;
    LandPending(channel, "wait");
    return 0;
}

void StreamService::OnBoundary(uint64_t boundary) {
    // The frame that ended spans to boundary x frame: what the drive finished in it is in place for the next frame
    // (the drive's frame clock: DriveFrame -- the boundary itself except in Vita play mode, where it is the fields)
#if defined(__vita__)
    const double ended = static_cast<double>(te::GetSettings().deterministic ? boundary : DriveFrame()) * FrameMs();
#else
    const double ended = static_cast<double>(boundary) * FrameMs();
#endif
    std::vector<int32_t> due;
    for (auto& [channel, p] : pending_) {
        if (Due(p, ended)) {
            due.push_back(channel);
        } else if (!p.device && p.end_ms <= ended) {
            NoteLate(p);   // (the drive is done; the card is not: in place at a later boundary or poll)
            ++card_.late_boundaries;
        }
    }
    for (int32_t channel : due) {
        ++count_.boundary_delivered;
        LandPending(channel, "boundary");
    }
}

void StreamService::Serialize(std::vector<uint8_t>& out) const {
    savestate::Put(out, uint32_t(2));
    savestate::Put(out, static_cast<int32_t>(state_));
    savestate::Put(out, channels_);
    savestate::Put(out, next_file_);
    savestate::Put(out, last_size_);
    savestate::Put(out, uint32_t(files_.size()));
    for (const auto& [number, file] : files_) {
        savestate::Put(out, number);
        savestate::Put(out, file.image_offset);
        savestate::Put(out, file.size);
        savestate::Put(out, uint8_t(file.found ? 1 : 0));
        savestate::PutString(out, file.path);
    }
    savestate::Put(out, uint32_t(buffers_.size()));
    for (const auto& [channel, buffer] : buffers_) {
        savestate::Put(out, channel);
        savestate::Put(out, buffer.address); savestate::Put(out, buffer.size);
        savestate::Put(out, buffer.sound_address); savestate::Put(out, buffer.sound_size);
    }
    savestate::PutBytes(out, &count_, sizeof count_);   // the census counters: uint64_t fields only
    // v2: the reads in flight (their bytes: the image may be another at the load), the drive's position
    savestate::Put(out, uint32_t(pending_.size()));
    for (const auto& [channel, p] : pending_) {
        savestate::Put(out, channel);
        savestate::Put(out, p.file); savestate::Put(out, p.offset); savestate::Put(out, p.size); savestate::Put(out, p.destination);
        savestate::Put(out, uint8_t(p.bank ? 1 : 0)); savestate::Put(out, p.bank_id);
        savestate::Put(out, p.issue_boundary); savestate::Put(out, DoubleBits(p.end_ms));
        const std::vector<uint8_t>& bytes = PendingBytes(p);
        savestate::Put(out, uint32_t(bytes.size()));
        if (!bytes.empty()) savestate::PutBytes(out, bytes.data(), bytes.size());
    }
    ReadTiming::Position position;
    if (timing_) position = timing_->Capture();
    savestate::Put(out, DoubleBits(position.busy_ms)); savestate::Put(out, DoubleBits(position.last_end_ms)); savestate::Put(out, position.head_lba);
}

bool StreamService::Restore(const std::vector<uint8_t>& payload, std::string* error) {
    savestate::PayloadReader in(payload);
    auto fail = [&](const char* why) { if (error) *error = std::string("te::stream: ") + why; return false; };
    uint32_t version = 0, nfiles = 0, nbuffers = 0;
    int32_t state = 0, channels = 0, next_file = 0;
    uint32_t last_size = 0;
    if (!in.Get(&version) || (version != 1 && version != 2) || !in.Get(&state) || !in.Get(&channels) || !in.Get(&next_file) ||
        !in.Get(&last_size) || !in.Get(&nfiles) || nfiles > 4096)
        return fail("payload");
    std::map<int32_t, File> files;
    const IsoFs* iso = Disc();
    for (uint32_t k = 0; k < nfiles; ++k) {
        int32_t number = 0;
        File file;
        uint8_t found = 0;
        if (!in.Get(&number) || !in.Get(&file.image_offset) || !in.Get(&file.size) || !in.Get(&found) ||
            !savestate::GetString(in, file.path, 1024))
            return fail("payload truncated (files)");
        file.found = found != 0;
        if (file.found) {
            // The same file must be on the disc that is open now, at the same extent
            IsoFs::Entry entry;
            if (!iso || !iso->Find(file.path, entry) || entry.directory || uint64_t{entry.lba} * IsoFs::kSectorSize != file.image_offset ||
                entry.size != file.size)
                return fail("an open file is not on this disc at the saved extent");
        }
        files[number] = file;
    }
    if (!in.Get(&nbuffers) || nbuffers > 256) return fail("payload truncated (buffers)");
    std::map<int32_t, Buffer> buffers;
    for (uint32_t k = 0; k < nbuffers; ++k) {
        int32_t channel = 0;
        Buffer b;
        if (!in.Get(&channel) || !in.Get(&b.address) || !in.Get(&b.size) || !in.Get(&b.sound_address) || !in.Get(&b.sound_size))
            return fail("payload truncated (buffers)");
        buffers[channel] = b;
    }
    Counters counters{};
    if (version == 1) {
        // v1 counters were the pre-drive-model set: the first 17 uint64_t fields
        constexpr std::size_t kV1 = 17 * sizeof(uint64_t);
        if (in.Remaining() >= kV1 && !in.GetBytes(&counters, kV1)) return fail("payload truncated (counters)");
    } else {
        if (!in.GetBytes(&counters, sizeof counters)) return fail("payload truncated (counters)");
    }
    std::map<int32_t, Pending> pending;
    ReadTiming::Position position;
    if (version >= 2) {
        uint32_t npending = 0;
        if (!in.Get(&npending) || npending > 256) return fail("payload truncated (pending)");
        for (uint32_t k = 0; k < npending; ++k) {
            int32_t channel = 0;
            Pending p;
            uint8_t bank = 0;
            uint64_t end_bits = 0;
            uint32_t nbytes = 0;
            if (!in.Get(&channel) || !in.Get(&p.file) || !in.Get(&p.offset) || !in.Get(&p.size) || !in.Get(&p.destination) ||
                !in.Get(&bank) || !in.Get(&p.bank_id) || !in.Get(&p.issue_boundary) || !in.Get(&end_bits) || !in.Get(&nbytes) ||
                nbytes > (64u << 20))
                return fail("payload truncated (pending)");
            p.bank = bank != 0;
            p.end_ms = BitsDouble(end_bits);
            if (!std::isfinite(p.end_ms) || p.end_ms < 0) return fail("a pending read's drive end is not a time");
            p.bytes.resize(nbytes);
            if (nbytes && !in.GetBytes(p.bytes.data(), nbytes)) return fail("payload truncated (pending bytes)");
            pending[channel] = std::move(p);
        }
        uint64_t busy = 0, last_end = 0;
        if (!in.Get(&busy) || !in.Get(&last_end) || !in.Get(&position.head_lba)) return fail("payload truncated (drive)");
        position.busy_ms = BitsDouble(busy);
        position.last_end_ms = BitsDouble(last_end);
        if (!ReadTiming::Validate(position)) return fail("the drive position is not valid");
    }
    if (state == static_cast<int32_t>(State::Running) && !iso) return fail("the stream was running but no disc is open");
    state_ = static_cast<State>(state);
    channels_ = channels;
    next_file_ = next_file;
    last_size_ = last_size;
    files_ = std::move(files);
    buffers_ = std::move(buffers);
    count_ = counters;
    pending_ = std::move(pending);
    if (state_ == State::Running) {
        EnsureTiming();
        timing_->Restore(position);
        if (instant_) {
            // Loaded under the instant model: whatever was in flight is in place now
            std::vector<int32_t> channels_pending;
            for (const auto& [channel, p] : pending_) channels_pending.push_back(channel);
            for (int32_t channel : channels_pending) ForceChannel(channel);
        }
    }
    return true;
}

void LogWindow(uint64_t from, uint64_t to) {
    static StreamService::CardCount last;
    static uint64_t last_music = 0;
    const StreamService::CardCount now = Service().Card();
    const uint64_t music = te::audio::Service().MusicCardPutOffs();
    if (from || to) {
        if (now.late_reads == last.late_reads && now.parks == last.parks && now.forced_waits == last.forced_waits &&
            now.entries_ahead == last.entries_ahead && music == last_music)
            return;
    }
    std::fprintf(stderr, "[te.stream] presents %llu-%llu: %llu reads put off for the card (%.1f MB: the drive model's time came before "
                 "the card's bytes; the game polled on -- %llu polls and %llu boundaries found them still reading), %llu frames "
                 "parked for the card in a wait, %llu landings forced while the card had them; %llu archive entries read ahead "
                 "from their first read; the music not ready yet for the card %llu times (its window's first blocks)\n",
                 (unsigned long long)from, (unsigned long long)to, (unsigned long long)(now.late_reads - last.late_reads),
                 double(now.late_bytes - last.late_bytes) / 1048576.0, (unsigned long long)(now.late_polls - last.late_polls),
                 (unsigned long long)(now.late_boundaries - last.late_boundaries), (unsigned long long)(now.parks - last.parks),
                 (unsigned long long)(now.forced_waits - last.forced_waits), (unsigned long long)(now.entries_ahead - last.entries_ahead),
                 (unsigned long long)(music - last_music));
    last = now;
    last_music = music;
}

StreamService& Service() {
    static StreamService service;
    static const bool registered = [] {
        savestate::Register(savestate::kTagStream, "te::stream::StreamService", false,
            [](std::vector<uint8_t>& out) { Service().Serialize(out); },
            [](const savestate::Blob& blob, std::string* err) { return Service().Restore(blob.payload, err); });
        return true;
    }();
    (void)registered;
    return service;
}

}  // namespace te::stream
