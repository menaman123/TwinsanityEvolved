// te_services.h -- the port's services over NativeEE, constructed in ServiceStartup order by Platform::System
// (host/system.cpp) and reached by the seam bodies of each header (host/<header>.cpp). One instance of each per
// process (the game is one EE thread; everything here runs on it unless a method says otherwise).
//
// Construction order (the engine's native composition root, app/host/ee_bootstrap_*.cpp, and the PS2
// arm's Platform::System::Initialise/StartServices agree on it):
//   System::Initialise     1 Clock (EmuDomain + DerivedClocks + TickSource/TickSync/BoundaryProtocol: the pacer)
//                          2 Disc       (the ISO opened with the engine's IsoFs: CDVDSTM's place)
//                          3 Files      (title::Files over IsoFs: the synchronous descriptors)
//   System::StartServices  4 Pads       (title::Input + the host sampler: PADMAN/SIO2MAN's place)
//                          5 Saves      (title::SaveStorage over the memory-card directory: MCMAN/MCSERV's place)
//                          6 IoHeap     (the IOP heap the stream buffers are sized against: opaque handles)
//                          7 Sound      (ps2e_sound_initialise_remote: LIBSD/SDRDRV's place)
//   Stream (STREAM.IRX's place) is constructed by the game's own Platform::Stream::Initialise call.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "te_host.h"

class IsoFs;
namespace title { class Files; class Input; class SaveStorage; }
namespace ps2r::savestate { struct Blob; }

namespace te {

// ---------------------------------------------------------------------------------------------------------------------
// The clock (time.h; the frame boundary graphics.h's WaitVSync/Present stand on)
// ---------------------------------------------------------------------------------------------------------------------
class Clock {
public:
    explicit Clock(const Settings& settings);
    ~Clock();
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    // Platform::Time::Initialise: the T0 count the game's ticks are counted from (idempotent).
    void Start();
    bool Started() const { return started_; }
    // Platform::Time::Ticks: T0 counts (576000/s) since Start -- DerivedClocks::T0Counts, a pure function of the tick.
    uint64_t Ticks() const;
    // The raw derived T0 count (no Start offset).
    uint64_t T0Counts() const;
    uint64_t DomainTick() const;
    // The fields fired so far (every tick body: one per boundary under the deterministic profile; in play mode the fields
    // the wall produced -- a late frame's and the tick lock's 2 / 3 per picture tick included): the stream drive's clock.
    uint64_t Fields() const;
    // Boundaries passed (the logical boundary tick the engine's services take: Input::SampleBoundary, SaveStorage).
    uint64_t BoundaryCount() const { return boundaries_; }
    // The presenter's clock (Play only): the wall instant (steady-clock ns) of the pacer edge that delivered the latest
    // field, and that field's ordinal -- the scene the game hands over after a boundary is the picture of that edge. A
    // render thread interpolating two pictures runs its fraction on it (never on its own arrival times). False when the
    // clock is deterministic or no field has been delivered yet.
    bool LatestFieldEdge(int64_t* edge_ns, uint64_t* field) const;
    // The game thread's time since the last boundary released it (ms; the frame's work so far): the presenter's census.
    double MsSinceRelease() const;
    // T0 counts per field (9600 at 60 Hz, 11520 at 50): one field of the game's clock (DerivedClocks::CountsPerTick(2)).
    uint64_t TicksPerField() const;
    // The in-frame charge (the stream drive model): T0 counts added to Ticks() for the rest of THIS frame --
    // the EE time a service's poll cost on the console (a STREAM status round trip), so the game's per-frame budget
    // loops (GameTimeHasTimeLeft) run out of frame as they did there instead of spinning on a read the drive has not
    // finished. Logical (a count, never wall time): the same under deterministic and play. Reset at every Boundary();
    // capped at the game's longest frame budget (the PAL game's 11520 = 576000 / 50; the game keeps a stamp that goes
    // back across such a boundary, GameTimeSetStamp). Zero at every boundary = not state.
    static constexpr uint64_t kChargeCapTicks = 576000u / 50u;
    void ChargeInFrame(uint64_t ticks);
    uint64_t InFrameTicks() const { return in_frame_; }

    // THE frame boundary: the one game thread arrives here from Platform::Graphics::WaitVSync and ::Present (each call
    // is one field). Pace park (Play: the pacer's 60 Hz edge), the field tick(s), the EmuDomain settle, then the
    // boundary hooks in registration order (input sample, movie present callback, scheduler drain, ...).
    void Boundary();
    using Hook = std::function<void(uint64_t boundary)>;
    // Hooks run on the game thread after the settle. Returns an id for Remove.
    int AddBoundaryHook(const std::string& name, Hook hook);
    void RemoveBoundaryHook(int id);

    // The RTC (System::LocalTime): the fixed 2005-06-15 12:00 (deterministic) or the host's date at construction,
    // advanced by the tick -- never read from the host clock after construction.
    struct Civil { int year, month, day, hour, minute, second; };
    Civil LocalTime() const;

    // Diagnostics (the clock census): pacer parks / late boundaries / timeouts, fields delivered; the TICKS -- the
    // game thread's wall time from one boundary's release to the next arrival (the frame's work: logic, draws, present;
    // never the pacer's park) -- counted over the field (1000/hz ms) and at their maximum. `TE_TICK_LOG_MS=x` logs
    // every tick over x ms as `[twinsanityevolved] tick: boundary N took Y ms` (the hub render-rate judge reads them by
    // boundary, so the load windows are boundaries, not wall spans).
    struct Census {
        uint64_t boundaries, fields, parks, late, timeouts, slept_us; bool play; uint32_t field_hz;
        uint64_t ticks_measured, ticks_over_field; double tick_max_ms, tick_sum_ms;
        // The clock contract's catch-up (rule 3): boundaries that delivered a backlog field without parking, and the
        // backlog fields the catch-up cap discarded (the game ran slower than the wall for those fields).
        uint64_t catchup_boundaries, dropped_fields;
        // The game thread's own CPU time over the measured ticks (ms; ps2r::host::ThreadCpuTimeNs between a boundary's
        // release and the next arrival): the EE's cost as the perf gate's ee_cpu_ms, independent of what the other threads
        // and the host's load do to the wall time.
        double tick_cpu_sum_ms;
        // THE GAME TIME VS WALL: the T0 counts the game's clock advanced since Platform::Time::Initialise
        // (576000 a second of game time), the wall seconds from then to the latest boundary (play; 0 deterministic), and
        // the counts a field gives (576000 / field_hz: 9600 at 60, 11520 at 50).
        uint64_t game_counts;
        double game_wall_s;
        uint64_t counts_per_field;
    };
    Census GetCensus() const;

    // Save state (savestate.h): the clock's section payload / its restore (the registry calls these).
    void Serialize(std::vector<uint8_t>& out) const;
    bool Restore(const ps2r::savestate::Blob& blob, std::string* err);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool started_ = false;
    uint64_t start_counts_ = 0;
    uint64_t boundaries_ = 0;
    uint64_t in_frame_ = 0;
    struct Entry { int id; std::string name; Hook hook; };
    std::vector<Entry> hooks_;
    int next_hook_ = 1;
};

// ---------------------------------------------------------------------------------------------------------------------
// The disc (disc.h) and its files (files.h, and the stream boot façade)
// ---------------------------------------------------------------------------------------------------------------------
class Disc {
public:
    explicit Disc(const Settings& settings);
    ~Disc();
    // Platform::Disc::Initialise: the image is open. 1 when it is, 0 when not (no path / not an ISO 9660 volume).
    int32_t Initialise();
    int32_t SetMedia(int32_t media);
    // Platform::Disc::WaitReady: Ready (2) with an open image, NotReady (6) otherwise. Never blocks.
    int32_t WaitReady() const;
    bool IsOpen() const;
    const IsoFs& Iso() const { return *iso_; }
    const std::string& Error() const { return error_; }
    int32_t Media() const { return media_; }

private:
    std::unique_ptr<IsoFs> iso_;
    std::string path_, error_;
    int32_t media_ = 2;   // Dvd
    bool opened_ = false, tried_ = false;
};

class Files {
public:
    explicit Files(Disc& disc);
    ~Files();
    // Platform::Files over title::Files: paths as the game gives them ("Crash6\\Crash.bh", either slash, any case);
    // buffers are guest addresses.
    void Reset();
    int32_t Open(const char* path, int32_t flags);
    int32_t Close(int32_t file);
    int32_t Read(int32_t file, uint32_t buffer, int32_t size);
    int32_t Write(int32_t file, uint32_t buffer, int32_t size);
    int32_t Seek(int32_t file, int32_t offset, int32_t whence);
    title::Files& Core() { return *core_; }

private:
    struct Memory;
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<title::Files> core_;
};

// ---------------------------------------------------------------------------------------------------------------------
// The pads (pads.h) over title::Input, sampled once per boundary from the host sampler
// ---------------------------------------------------------------------------------------------------------------------
class Pads {
public:
    static constexpr unsigned kPorts = 2, kSlots = 4;
    // One port/slot's host sample (title::Input::Sample's fields): ACTIVE-HIGH buttons in wire order
    // (SELECT bit 0 ... SQUARE bit 15), sticks 0..255 neutral 128.
    struct Sample {
        bool connected = false;
        uint16_t buttons = 0;
        uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    };
    // The host sampler: called once per boundary on the game thread for every port/slot. The default (host/te_input.cpp)
    // is the engine's SDL InputSystem (gamepad + keyboard/mouse) when SDL input is on, else the neutral pad.
    using Sampler = std::function<Sample(unsigned port, unsigned slot)>;
    // The rumble output (SetActuatorDirect's motors: small on/off, big 0..255). False = unsupported/disconnected.
    using MotorOutput = std::function<bool(unsigned port, unsigned slot, uint8_t small, uint8_t big)>;

    Pads(Clock& clock, const Settings& settings);
    ~Pads();
    void SetSampler(Sampler sampler);
    // A pad log's sampler: wins over SetSampler's (the SDL sampler System::StartServices installs) while set.
    void SetOverrideSampler(Sampler sampler) { override_ = std::move(sampler); }
    // A tap on every sample as the game sees it (a recording).
    using Tap = std::function<void(unsigned port, unsigned slot, const Sample& sample)>;
    void SetSampleTap(Tap tap) { tap_ = std::move(tap); }
    void SetMotorOutput(MotorOutput output) { motors_ = std::move(output); }
    // Buttons held on pad 0 on top of the sampler's (the decomp's g_TestPadButtons shape: the game's button word,
    // pressed 1 -- 0x800 start, 0x100 select, 0x1000 up ... 0x40 cross). For scripted runs.
    void HoldTestButtons(uint32_t buttons) { test_buttons_ = buttons; }

    int32_t Initialise();
    int32_t Open(int32_t port, int32_t slot, uint32_t buffer);
    int32_t Close(int32_t port, int32_t slot);
    int32_t Read(int32_t port, int32_t slot, uint32_t data);
    int32_t GetState(int32_t port, int32_t slot) const;
    int32_t GetRequestState(int32_t port, int32_t slot) const;
    int32_t InfoMode(int32_t port, int32_t slot, int32_t info, int32_t index) const;
    int32_t SetMainMode(int32_t port, int32_t slot, int32_t mode, int32_t lock);
    int32_t InfoActuator(int32_t port, int32_t slot, int32_t actuator, int32_t term) const;
    int32_t SetActuatorAlign(int32_t port, int32_t slot, uint32_t align);
    int32_t SetActuatorDirect(int32_t port, int32_t slot, uint32_t values);
    int32_t InfoPressureMode(int32_t port, int32_t slot) const;
    int32_t EnterPressureMode(int32_t port, int32_t slot);

    // Sample every open port/slot from the sampler and accept the boundary (the clock's hook calls this).
    void SampleBoundary(uint64_t boundary);
    title::Input& Core() { return *core_; }
    uint32_t BufferOf(int32_t port, int32_t slot) const;
    void Serialize(std::vector<uint8_t>& out) const;
    bool Restore(const ps2r::savestate::Blob& blob, std::string* err);

private:
    std::unique_ptr<title::Input> core_;
    Sampler sampler_, override_;
    Tap tap_;
    MotorOutput motors_;
    std::array<uint32_t, kPorts * kSlots> buffers_{};
    uint32_t test_buttons_ = 0;
    Clock& clock_;
    int hook_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// The saves (saves.h) over title::SaveStorage
// ---------------------------------------------------------------------------------------------------------------------
class Saves {
public:
    Saves(Clock& clock, const Settings& settings);
    ~Saves();
    // Platform::Saves::Initialise(region, product): SaveStorage::Start (the memory-card roots) + Initialise.
    int32_t Initialise(uint32_t region, uint32_t product);
    int32_t Update(int32_t port, int32_t slot, uint32_t info);
    void GetResult(uint32_t out, int32_t operation);
    int32_t Format(int32_t port, int32_t slot);
    int32_t CreateSave(int32_t port, int32_t slot, uint32_t save);
    int32_t MeasureSave(int32_t port, int32_t slot, uint32_t save, uint32_t kilobytes);
    int32_t FindFile(int32_t port, int32_t slot, uint32_t file, uint32_t save, uint32_t size, uint32_t entry);
    int32_t Write(int32_t port, int32_t slot, uint32_t save, uint32_t file, uint32_t data, int32_t size);
    int32_t Read(int32_t port, int32_t slot, uint32_t save, uint32_t file, uint32_t buffer, int32_t size);
    static int32_t FileKilobytes(int32_t size);
    static int32_t SaveBytes(int32_t kilobytes, int32_t files);
    title::SaveStorage& Core() { return *Active(); }
    bool Started() const { return started_; }
    void Serialize(std::vector<uint8_t>& out) const;
    bool Restore(const ps2r::savestate::Blob& blob, std::string* err);
    // the memory card in use -- the vanilla card (<mc_dir>) or, while ANY mod is on, the mod-namespaced card
    // (<mc_dir>-mods, seeded from the vanilla card the first time it is used). The switch is taken between operations
    // (never while one is in flight); a modded save never touches the vanilla card.
    bool UsingModsCard() const { return use_mods_; }
    std::string CardDirectory() const;

private:
    struct Outputs { uint32_t value = 0, entry = 0, data = 0, size = 0; };
    void Publish();
    title::SaveStorage* Active();
    void SyncCard();
    std::unique_ptr<title::SaveStorage> core_;        // the vanilla card
    std::unique_ptr<title::SaveStorage> mods_core_;   // the -mods card (made at the first switch)
    std::string mc_dir_, region_, product_;
    Clock& clock_;
    Outputs outputs_;
    bool started_ = false, mods_started_ = false, use_mods_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------
// The I/O processor (io.h): nothing to restart or load natively; its heap is a budget of opaque handles
// ---------------------------------------------------------------------------------------------------------------------
class IoHeap {
public:
    // The IOP's 2 MB; the retail IOPRP300 + drivers leave about 1.2 MB for the sound streams' buffers.
    static constexpr uint32_t kBase = 0x00100000, kSize = 0x00120000;
    uint32_t Allocate(int32_t size);    // 16-aligned opaque address, 0 when it does not fit
    int32_t Free(uint32_t address);     // 0 ok, -1 unknown
    uint32_t Free() const;              // bytes left
    void Reset();
    int32_t LoadDriver(const char* path);   // a positive module id per call; the names are logged (verbose)
    void Serialize(std::vector<uint8_t>& out) const;
    bool Restore(const ps2r::savestate::Blob& blob, std::string* err);

private:
    struct Block { uint32_t address, size; };
    std::vector<Block> blocks_;
    uint32_t next_ = kBase;
    int32_t drivers_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// The registry: construction in order, lazily (a seam body reached before System::Initialise constructs what it needs
// in the same order, so the unit tests of one header need no boot).
// ---------------------------------------------------------------------------------------------------------------------
class Services {
public:
    static Services& Get();
    // Platform::System::Initialise / StartServices
    void Initialise();
    void StartServices();
    bool Initialised() const { return initialised_; }
    bool Started() const { return started_; }

    Clock& clock();
    Disc& disc();
    Files& files();
    Pads& pads();
    Saves& saves();
    IoHeap& io();

    // Tests: destroy everything and read the settings afresh at the next access.
    static void ResetForTest();

    // Save state (savestate.h): the services' own section (which services exist) and, before a restore, the
    // construction of every service the file's system section says existed.
    void RegisterStateSection();
    void PrepareForRestore(const std::vector<ps2r::savestate::Blob>& sections);

private:
    Services() = default;
    std::unique_ptr<Clock> clock_;
    std::unique_ptr<Disc> disc_;
    std::unique_ptr<Files> files_;
    std::unique_ptr<Pads> pads_;
    std::unique_ptr<Saves> saves_;
    std::unique_ptr<IoHeap> io_;
    bool initialised_ = false, started_ = false;
};

}  // namespace te
