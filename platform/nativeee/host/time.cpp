// platform/nativeee (host side) -- time.h: Platform::Time over the engine's pacer, and the frame boundary.
//
// The clock is the engine's: an EmuDomain (the one game-visible tick), DerivedClocks (T0 at BUSCLK/256 = 576000 counts a
// second of real time = Platform::Time::TicksPerSecond: 9600 per 60 Hz field, 11520 per 50 Hz one -- the clocks take the
// field rate, DerivedClocks::SetFieldHz -- a pure function of the tick, no wall value), a TickSource
// (Deterministic: a field per boundary, no thread; Play: a host pacer thread notes one tick per 1/field_hz s wall edge and
// the boundary parks for it -- BoundaryProtocol::ParkForPacerTick, the wall-locked boundary) and a TickSync (the
// tick body runs ONLY on the game thread). The pacer thread is the engine's FieldPacer (TickThread's cadence as a
// library class: anchor + k * period, re-anchor when more than a period behind).
//
// Platform::Time::Ticks() therefore moves only at boundaries (9600 a 60 Hz field, 11520 a 50 Hz one): the same shape the
// recomp's title sees under the engine (DerivedClocks: "the wall term does not exist here"), so the game's per-frame time
// budget loops (GameTimeHasTimeLeft) always find time left within a frame and finish their work by its own termination.
// The counts per field follow the field rate: until then they were the 60 Hz field's 9600 at every rate, so at
// the PAL disc's 50 Hz (the pacer firing 50 fields a second) the game's clock (GameTimeStamp = T0 >> 6) advanced 150 units
// a tick for its 180-unit (1/50 s) frames -- the game in slow motion at 5/6 of real time, the sound (the device's real
// time) ahead of it.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "engine/ee/timing/boundary_protocol.h"
#include "engine/platform/host/platform.h"
#include "engine/ee/timing/derived_clocks.h"
#include "engine/ee/timing/emu_domain.h"
#include "engine/ee/timing/field_pacer.h"
#include "engine/ee/timing/tick_source.h"
#include "engine/ee/timing/tick_sync.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "te_load_census.h"
#include "te_prof.h"
#include "te_tick_lock.h"
#include "te_weak.h"
#include "te_services.h"
#include <cstring>
#include <string>
#include <vector>

// The graphics side's phases of the last Present (graphics.cpp), for the slow-tick line; weak: a build without the
// graphics bodies (the tests) has no such symbol (te_weak.h: on COFF an absent body resolves to an empty one)
TE_WEAK_REF_VOID(te_graphics_last_present_phases_ms, (double* submit, double* present, double* pump))

namespace te {

struct Clock::Impl {
    EmuDomain domain;
    DerivedClocks clocks{domain};
    TickSource source;
    TickSync ticks;
    BoundaryProtocol protocol{source, ticks};
    bool play = false;
    uint32_t field_hz = 60;
    uint64_t fields = 0;
    // the game's field count less the pacer's tick count: 0 from a start (they advance together); a restore sets the
    // game's to the state's while the pacer counts on from this run's start -- the edge of a field is the pacer's tick
    // `fields - field_offset` (else every picture's edge after a load lies the state's age in the future)
    int64_t field_offset = 0;
    // The RTC's epoch (seconds since 1970, in LOCAL civil time already) at construction.
    uint64_t rtc_epoch = DerivedClocks::kRtcFixedEpochS;
    // The pacer thread (Play only): the engine's FieldPacer (engine/ee/timing/field_pacer.h) -- the TickThread cadence
    // (anchor + k * 1e9/hz, re-anchor when more than a period behind) as a library class, consumed since the pin
    // nativeee twinsanityevolved@fd5273dc.
    std::unique_ptr<FieldPacer> pacer;
    // The ticks (Census): the game thread's work between boundaries, by the host's steady clock
    using Steady = std::chrono::steady_clock;
    Steady::time_point released{};
    bool have_release = false;
    uint64_t ticks_measured = 0, ticks_over_field = 0;
    double tick_max_ms = 0, tick_sum_ms = 0, tick_log_ms = -1;   // tick_log_ms < 0: TE_TICK_LOG_MS unset, no per-tick log
    uint64_t released_cpu_ns = 0;                                  // the game thread's CPU clock at the release (the EE's own cost)
    double tick_cpu_sum_ms = 0;
    // the game time vs wall (play): the wall instant of Start (the game's clock from 0) and of the latest boundary
    Steady::time_point started_at{}, last_boundary_ns{};
    bool have_last_boundary = false;

    void StartPacer() {
        // The pacer thread at the interactive quality of service (with the game and render threads): a pacer the
        // host starves (default QoS under load) misses edges -- the clock census's `late` and `timeouts`
        if (!pacer) pacer.reset(new FieldPacer(ticks, protocol, field_hz, [] {
            // the engine's per-platform arm (macOS QoS, Windows SetThreadPriority, Linux the name)
            if (!std::getenv("TE_NO_QOS")) ps2r::platform::host::PinCurrentThreadInteractive("te-pacer", ps2r::platform::host::ThreadRole::Pacer);
            te::prof::NoteThread("te-pacer");   // the CPU census (the Vita)
        }));
#if defined(__vita__)
        // the tick lock's edges (te_tick_lock.h): while the lock is engaged the fields are the display's refreshes, so a
        // picture held N refreshes always starts on the same phase (the timer otherwise: one refresh a tick). At the PAL disc's
        // 50 Hz there is no such edge (a 60 Hz panel): the fields are this pacer's 20 ms timer at every rate of the lock
        if (field_hz == 60 && !pacer->Running()) pacer->SetExternalEdge([] { te::tick_lock::WaitDisplayEdge(); });
#endif
        if (!pacer->Running()) pacer->Start();
    }
    void StopPacer() {
        if (pacer) pacer->Stop();
    }
};

Clock::Clock(const Settings& settings) : impl_(new Impl) {
    Impl& i = *impl_;
    i.play = !settings.deterministic;
    i.field_hz = settings.field_hz == 0 ? 60u : settings.field_hz;
    // The derived clocks count real time at this field rate (T0: 576000 a second = 9600 a 60 Hz field, 11520 a 50 Hz
    // one): set before any count is read (the domain is at tick 0 here: every count is tick x counts-per-field)
    i.clocks.SetFieldHz(i.field_hz);
    i.source.SetPolicy(i.play ? TickSource::Policy::Play : TickSource::Policy::Deterministic);
    i.source.SetMode(TickSource::Mode::Realtime);
    if (const char* ms = std::getenv("TE_TICK_LOG_MS")) i.tick_log_ms = std::atof(ms);
    // Deterministic profile: the quantum pinned at 1 (a perfect console); Play: the pacer release (every noted tick
    // is one elapsed wall field, rides 1:1).
    i.domain.SetRegimePin(1);
    i.domain.SetPacerRelease(i.play);
    i.clocks.SetRtcFixed(true);
    if (i.play) {
        // The host's local civil time at construction as the RTC epoch (then advanced by the tick only).
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        // Re-encode the LOCAL fields as if UTC so EpochToCivil gives them back.
        const int y = local.tm_year + 1900, m = local.tm_mon + 1, d = local.tm_mday;
        auto days_from_civil = [](int64_t y, unsigned m, unsigned d) -> int64_t {
            y -= m <= 2;
            const int64_t era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<int64_t>(doe) - 719468;
        };
        i.rtc_epoch = static_cast<uint64_t>(days_from_civil(y, m, d)) * 86400u + local.tm_hour * 3600u +
                      local.tm_min * 60u + local.tm_sec;
    }
    // The tick body: the ONE place a field happens (BoundarySequencer::InstallPhase3Steps's body without the INTC).
    i.ticks.SetBody([this](TickSync::Cause cause) -> uint64_t {
        Impl& im = *impl_;
        const uint64_t raw = im.source.Tick();
        if (cause == TickSync::Cause::Deferred) im.domain.OnDeferredTick(raw);
        else im.domain.OnRawTick(raw);
        ++im.fields;
        return raw;
    });
    i.domain.ReAnchor(0);
    // Through the registry's current instance (not `this`): Services::ResetForTest rebuilds the services, the
    // section registration is once per process.
    savestate::Register(savestate::kTagClock, "te::Clock", false,
                        [](std::vector<uint8_t>& out) { Services::Get().clock().Serialize(out); },
                        [](const savestate::Blob& blob, std::string* err) { return Services::Get().clock().Restore(blob, err); });
}

// The clock's state at a boundary: the boundary/field counts, the Start offset, the RTC epoch, the raw tick and the
// EmuDomain's logical snapshot (its detector included). The pacer (Play) is wall-side and is not state: a loaded
// clock paces from its next boundary. (payload 2) + the derived clocks' counts at the saved tick (their
// anchor): a state loaded under another field rate continues every count from its saved value at the loading rate --
// the game's T0 (Ticks) never jumps across the load (a 60 Hz state in a 50 Hz run: tick x 11520 would be ahead of the
// saved tick x 9600 by tick x 1920 -- one frame of minutes of game time).
void Clock::Serialize(std::vector<uint8_t>& out) const {
    const Impl& i = *impl_;
    const EmuDomain::Snapshot d = i.domain.Capture();
    savestate::Put(out, uint32_t(2));
    savestate::Put(out, boundaries_);
    savestate::Put(out, i.fields);
    savestate::Put(out, uint8_t(started_ ? 1 : 0));
    savestate::Put(out, start_counts_);
    savestate::Put(out, i.rtc_epoch);
    savestate::Put(out, i.source.RawCount());
    savestate::Put(out, d.tick); savestate::Put(out, d.base); savestate::Put(out, d.boundary_raw);
    savestate::Put(out, d.arrival_raw); savestate::Put(out, d.boundaries); savestate::Put(out, d.lead);
    savestate::Put(out, d.regime); savestate::Put(out, d.pin);
    savestate::Put(out, uint8_t(d.have_boundary)); savestate::Put(out, uint8_t(d.bypass)); savestate::Put(out, uint8_t(d.pacer_release));
    savestate::Put(out, d.detector.n); savestate::Put(out, d.detector.samples); savestate::Put(out, d.detector.over);
    savestate::Put(out, d.detector.under); savestate::Put(out, uint8_t(d.detector.fast));
    savestate::Put(out, uint32_t(i.play ? 1 : 0));
    savestate::Put(out, i.field_hz);
    const DerivedClocks::Anchor a = i.clocks.AnchorAt(i.domain.DomainTick());
    savestate::Put(out, a.tick);
    for (uint64_t c : a.counts) savestate::Put(out, c);
    savestate::Put(out, a.cop0);
}

bool Clock::Restore(const savestate::Blob& blob, std::string* err) {
    Impl& i = *impl_;
    savestate::PayloadReader in(blob.payload);
    uint32_t version = 0;
    uint64_t raw = 0;
    uint8_t started = 0, have_boundary = 0, bypass = 0, pacer_release = 0, fast = 0;
    EmuDomain::Snapshot d;
    uint32_t play = 0, hz = 60;
    auto fail = [&](const char* why) { if (err) *err = std::string("te::Clock: ") + why; return false; };
    if (!in.Get(&version) || (version != 1 && version != 2)) return fail("payload version");
    if (!in.Get(&boundaries_) || !in.Get(&i.fields) || !in.Get(&started) || !in.Get(&start_counts_) ||
        !in.Get(&i.rtc_epoch) || !in.Get(&raw) || !in.Get(&d.tick) || !in.Get(&d.base) || !in.Get(&d.boundary_raw) ||
        !in.Get(&d.arrival_raw) || !in.Get(&d.boundaries) || !in.Get(&d.lead) || !in.Get(&d.regime) || !in.Get(&d.pin) ||
        !in.Get(&have_boundary) || !in.Get(&bypass) || !in.Get(&pacer_release) || !in.Get(&d.detector.n) ||
        !in.Get(&d.detector.samples) || !in.Get(&d.detector.over) || !in.Get(&d.detector.under) || !in.Get(&fast) ||
        !in.Get(&play) || !in.Get(&hz))
        return fail("payload truncated");
    DerivedClocks::Anchor anchor;
    if (version >= 2) {
        if (!in.Get(&anchor.tick)) return fail("payload truncated");
        for (uint64_t& c : anchor.counts) if (!in.Get(&c)) return fail("payload truncated");
        if (!in.Get(&anchor.cop0)) return fail("payload truncated");
    }
    d.have_boundary = have_boundary != 0;
    d.bypass = bypass != 0;
    d.pacer_release = i.play;   // the loading run's policy, not the saved one
    d.detector.fast = fast != 0;
    started_ = started != 0;
    in_frame_ = 0;   // a state is a boundary's
    i.source.ResetForRestore(raw);
    i.have_release = false;   // the tick after a load is not a frame's work
    if (!EmuDomain::Validate(d, raw) || !i.domain.Restore(d, raw)) return fail("EmuDomain snapshot rejected");
    if (version < 2) {
        // payload 1 (before the 50 Hz clock fix): every count was tick x its 60 Hz field's at every rate
        anchor.tick = i.domain.DomainTick();
        for (uint32_t c = 0; c < 4; ++c) anchor.counts[c] = anchor.tick * DerivedClocks::kCountsPerTick[c];
        anchor.cop0 = anchor.tick * DerivedClocks::kCop0CountsPerTick;
    }
    // the clocks continue from the state's counts at the loading run's field rate (the same rate: the same counts)
    i.clocks.SetAnchor(anchor);
    if ((play != 0) != i.play || hz != i.field_hz)
        std::fprintf(stderr, "[twinsanityevolved] clock: state saved under %s %u Hz, loading under %s %u Hz (the game's clock continues "
                     "from the state's T0 %llu at %llu counts a field)\n",
                     play ? "play" : "deterministic", hz, i.play ? "play" : "deterministic", i.field_hz,
                     (unsigned long long)anchor.counts[2], (unsigned long long)i.clocks.CountsPerTick(2));
    if (started_ && i.play) i.StartPacer();
    i.field_offset = i.pacer ? int64_t(i.fields) - int64_t(i.pacer->Ticks()) : 0;
    return true;
}

Clock::~Clock() { impl_->StopPacer(); }

void Clock::Start() {
    if (started_) return;
    started_ = true;
    start_counts_ = T0Counts();
    impl_->started_at = Impl::Steady::now();
    impl_->have_last_boundary = false;
    if (impl_->play) impl_->StartPacer();
}

bool Clock::LatestFieldEdge(int64_t* edge_ns, uint64_t* field) const {
    const Impl& i = *impl_;
    if (!i.play || !i.pacer || i.fields == 0) return false;
    const int64_t tick = int64_t(i.fields) - i.field_offset;
    if (tick <= 0 || !i.pacer->EdgeOfTickNs(uint64_t(tick), edge_ns)) return false;
    if (field) *field = i.fields;
    return true;
}

double Clock::MsSinceRelease() const {
    const Impl& i = *impl_;
    if (!i.have_release) return 0.0;
    return std::chrono::duration<double, std::milli>(Impl::Steady::now() - i.released).count();
}

uint64_t Clock::T0Counts() const { return impl_->clocks.T0Counts(); }
uint64_t Clock::DomainTick() const { return impl_->domain.DomainTick(); }
uint64_t Clock::Fields() const { return impl_->fields; }
uint64_t Clock::Ticks() const { return T0Counts() - start_counts_ + in_frame_; }
uint64_t Clock::TicksPerField() const { return impl_->clocks.CountsPerTick(2); }   // 576000 / field_hz: 9600 at 60, 11520 at 50

void Clock::ChargeInFrame(uint64_t ticks) {
    // The cap is the GAME's frame budget, not the field's: the PAL game folds its headroom modulo 576000 / 50 = 11520
    // ticks (GameTimeHasTimeLeft's frameTicks) while the port's fields are 9600 at 60 Hz -- a charge capped at the
    // field could never expire that budget (the spin at boot frame 68 on the first drive-paced read). 11520 covers
    // either disc's frame; the NTSC game's loops expire at 9600 before reaching it.
    in_frame_ = std::min(kChargeCapTicks, in_frame_ + ticks);
}

void Clock::Boundary() {
    Impl& i = *impl_;
    in_frame_ = 0;   // the frame's charges end with it (the next frame's Ticks() start at the field's T0)
    // the tick lock's plan for this boundary (te_tick_lock.h; 0 = not a picture tick: one field): consumed every boundary
    const uint32_t planned = tick_lock::TakePlan();
    if (i.play && planned > 0) tick_lock::DiagGameDelay(boundaries_);   // a test knob (off unless TE_VITA_DIAG_GAME_DELAY_US)
    double work_ms = -1;
    if (i.have_release) {
        // This frame's work: from the previous boundary's release (after its park) to now
        const double ms = std::chrono::duration<double, std::milli>(Impl::Steady::now() - i.released).count();
        work_ms = ms;
        i.ticks_measured++;
        i.tick_sum_ms += ms;
        const uint64_t cpu = ps2r::host::ThreadCpuTimeNs();
        if (cpu && i.released_cpu_ns) i.tick_cpu_sum_ms += double(cpu - i.released_cpu_ns) / 1e6;
        if (ms > i.tick_max_ms) i.tick_max_ms = ms;
        if (ms > 1000.0 / double(i.field_hz ? i.field_hz : 60u)) i.ticks_over_field++;
        // the load census (te_load_census.h): the tick's load work folded into the window; its text for the slow-tick line
        const bool log_tick = i.tick_log_ms >= 0 && ms > i.tick_log_ms;
        std::string load_work;
        load::EndTick(boundaries_ + 1, ms, log_tick ? &load_work : nullptr);
        if (log_tick) {
            // The phases: the draws' submission and the present/hand-over on this thread, the window's event pump, the rest;
            // then what the tick did that a load costs (textures, model parts, native meshes, the waits that come with them)
            double submit = 0, present = 0, pump = 0;
            if (TE_WEAK_PRESENT(te_graphics_last_present_phases_ms)) te_graphics_last_present_phases_ms(&submit, &present, &pump);
            // (TE_PROF: the game thread's sections sampled during this tick)
            const std::string sections = prof::TickSections(boundaries_ + 1, 4);
            std::fprintf(stderr, "[twinsanityevolved] tick: boundary %llu took %.1f ms (draw submit %.1f, present %.1f, pump %.1f, game %.1f)%s%s%s\n",
                         (unsigned long long)(boundaries_ + 1), ms, submit, present, pump, std::max(0.0, ms - submit - present - pump),
                         load_work.c_str(), sections.empty() ? "" : " prof: ", sections.c_str());
        }
    } else {
        load::EndTick(boundaries_ + 1, 0.0, nullptr);   // (an unmeasured tick: its notes start over)
    }
    if (i.play && !(i.pacer && i.pacer->Running())) i.StartPacer();   // a boundary before Time::Initialise (the pad setup's WaitVSync)
    i.domain.SetBypass(i.source.Bypass());
    i.domain.NoteArrival(i.source.RawCount());
    uint32_t fired = 1;
    if (i.play) {
        // Wall-locked: park for the pacer's edge (an early frame), then fire every noted tick as one elapsed field; a
        // late frame finds its tick(s) noted and does not park. A stopped pacer (timeout) still gets its one field.
        // A picture tick of the tick lock (planned 2 / 3: the Vita's 30 / 20 Hz) parks until that many edges are noted
        // and fires them together: the tick spans the lock's fields (more when it ran past them -- the lock's miss).
        {
            TE_PROF_SCOPE("park (the pacer's edge)");
            if (planned > 1) i.protocol.ParkForPacerTicks(planned);
            else i.protocol.ParkForPacerTick();
        }
        const int n = i.ticks.FirePending();
        if (n == 0) i.ticks.FireInline(TickSync::Cause::Boundary);
        fired = n > 0 ? uint32_t(n) : 1u;
    } else {
        i.ticks.FireInline(TickSync::Cause::Boundary);
        i.ticks.FirePending();
    }
    i.domain.OnBoundary(i.source.RawCount());
    const uint64_t boundary = ++boundaries_;
    if (i.play) {
        tick_lock::NoteBoundary(boundary, planned, fired, work_ms);
        tick_lock::NoteGameTime(boundary, i.clocks.T0Counts());   // the window line's game time vs wall (te_tick_lock.h)
        i.last_boundary_ns = Impl::Steady::now();   // the exit census's game time vs wall (GetCensus)
        i.have_last_boundary = true;
        // the pacer's edges: the display's vblank while the lock is engaged, the timer otherwise (te_tick_lock.h)
        if (i.pacer) i.pacer->UseExternalEdge(tick_lock::WantDisplayEdge());
    }
    {
        // The hooks: a copy, so a hook may add/remove hooks. Destroyed before the checkpoint on purpose.
        const auto hooks = hooks_;
        for (const Entry& e : hooks) {
            if (prof::Active()) {
                // each hook its own profiler section (TE_PROF): its name, kept for the process
                static std::vector<std::pair<int, unsigned short>> ids;
                unsigned short id = 0;
                for (const auto& p : ids) if (p.first == e.id) id = p.second;
                if (!id) { id = prof::Register(strdup(("hook " + e.name).c_str())); ids.emplace_back(e.id, id); }
                prof::Scope scope(id);
                e.hook(boundary);
                continue;
            }
            e.hook(boundary);
        }
    }
    i.released = Impl::Steady::now();   // the next tick is measured from here (a save at the checkpoint counts to it)
    prof::TickBegin(boundary + 1);      // (TE_PROF: the next tick's samples, for its slow-tick line)
    i.released_cpu_ns = ps2r::host::ThreadCpuTimeNs();
    i.have_release = true;
    // THE save-state checkpoint (savestate.h): the game fiber may be suspended inside this call and resumed in another
    // process. Nothing of this frame is used after it returns -- keep it the last statement.
    savestate::Checkpoint(boundary);
}

int Clock::AddBoundaryHook(const std::string& name, Hook hook) {
    const int id = next_hook_++;
    hooks_.push_back({id, name, std::move(hook)});
    return id;
}

void Clock::RemoveBoundaryHook(int id) {
    for (size_t k = 0; k < hooks_.size(); ++k) {
        if (hooks_[k].id == id) { hooks_.erase(hooks_.begin() + static_cast<long>(k)); return; }
    }
}

Clock::Civil Clock::LocalTime() const {
    const Impl& i = *impl_;
    // The epoch advanced by whole emulated seconds (field_hz fields each: 60, or the PAL console's 50 -- at 50 Hz a second
    // of 60 fields was 1.2 s of the game's time and the clock ran 50/60 of real time)
    const uint64_t seconds = i.rtc_epoch + i.domain.DomainTick() / (i.field_hz ? i.field_hz : 60u);
    const DerivedClocks::RtcTime t = DerivedClocks::EpochToCivil(seconds);
    return Civil{t.year, t.month, t.day, t.hour, t.minute, t.second};
}

Clock::Census Clock::GetCensus() const {
    const Impl& i = *impl_;
    const uint64_t game_counts = started_ ? T0Counts() - start_counts_ : 0;
    const double wall_s = (i.play && started_ && i.have_last_boundary)
                              ? std::chrono::duration<double>(i.last_boundary_ns - i.started_at).count() : 0.0;
    return Census{boundaries_, i.fields, i.protocol.PaceParks(), i.protocol.PaceLate(), i.protocol.PaceTimeouts(),
                  i.protocol.PaceSleptUs(), i.play, i.field_hz, i.ticks_measured, i.ticks_over_field, i.tick_max_ms,
                  i.tick_sum_ms, i.protocol.PaceCatchUp(), i.protocol.PaceDroppedFields(), i.tick_cpu_sum_ms,
                  game_counts, wall_s, i.clocks.CountsPerTick(2)};
}

}  // namespace te

// ---------------------------------------------------------------------------------------------------------------------
// The seam bodies (time.h)
// ---------------------------------------------------------------------------------------------------------------------
extern "C" {

void ps2e_time_initialise(void) { te::Services::Get().clock().Start(); }

ps2e_u64 ps2e_time_ticks(void) { return te::Services::Get().clock().Ticks(); }

}  // extern "C"
