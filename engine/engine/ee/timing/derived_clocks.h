#pragma once
// DerivedClocks: every game-observable clock as a PURE FUNCTION of the
// EmuDomain tick (plus fixed configuration). No accessor reads a host clock;
// two runs with the same boundary/tick sequence read identical values.
//
// The nine clocks (ARCH A4 5.1), each with its OLD source and the domain it
// now derives from:
//  (1) EE timers T0-T3 COUNT   -- emu ticks x counts-per-tick[CLKS]
//      (BUSCLK 147.456 MHz: /1 = 2457600, /16 = 153600, /256 = 9600 counts per
//      60 Hz field; CLKS=3 HBLANK = 262 lines/field). OLD ee_timers.cpp
//      synthesised the count from the SyncV count (9600/tick) plus a WALL
//      sub-tick interpolation; the wall term does not exist here (the
//      profile's t0_tickpaced pin is the only shape). Register file, 16-bit
//      window, compare/overflow are 3D2's EeTimers, reading TimerCounts().
//  (2) VBlank tick             -- the emu tick itself (EE-synchronous; OLD
//      ps2_vblank.cpp's wall worker is the play pacer of TickSource, never a
//      value source).
//  (3) frame boundary / SyncV field parity -- boundary count from EmuDomain
//      (OLD gs_hle.cpp g_sceGsSyncV_count); FIELD = parity of the emu tick.
//  (4) frame-budget anchor     -- emu-domain T0 counts at the last boundary
//      (OLD main.cpp 0x191bd0 hook's frame-top snapshot; the title supplies
//      the anchor VALUE the game stored, this clock supplies the domain).
//  (5) COP0 Count              -- emu ticks x EE core counts per field
//      (EE core 294.912 MHz = 2 x BUSCLK: 4915200 per 60 Hz field); the mtc0
//      reseat offset belongs to SyscallTable::HandleMtc0 (OLD syscalls.cpp).
//  (6) RTC                     -- fixed 2005-06-15 12:00:00 UTC when
//      game.rtc_fixed (deterministic profile); otherwise that epoch advanced
//      by emu ticks / 60 s -- still the emu domain, never the host date
//      (OLD sce_hle sceCdReadClock + PS2_RTC_FIXED).
//  (7) IOP heartbeat           -- one beat per round(usec / 16667) emu ticks
//      (OLD turbo.h PS2_HB_TICKCLOCK re-clocking of STREAM.IRX's wall beat).
//  (8) CDVD latency / refill epoch -- the emu tick, metered at 256 KB per tick
//      (OLD ps2_vblank.h delivery meter + PS2_CDVD_BUDGET_KB=256).
//  (9) audio                   -- the DEVICE clock is the rate master and is
//      NOT derived here: it never enters the emu domain and no emu value is
//      stretched to it (OLD mixer rule). Recorded by
//      AudioRateMasterIsDevice() so the rule is visible where the other eight
//      live.
//
// THE FIELD RATE (SetFieldHz; 60 unless set). An emu tick is one field of the
// video mode: 1/60 s on an NTSC console, 1/50 s on a PAL one -- and the
// hardware clocks count REAL time either way (BUSCLK 147.456 MHz, the EE core
// 294.912 MHz, the T0 that a PAL game's clock reads at BUSCLK/256 = 576000 a
// second). So the counts per tick are the clock's rate over the field rate:
// 9600 T0 counts a field at 60, 11520 at 50 (a PAL game's GameTimeStamp,
// T0 >> 6, then advances 180 units a field -- its 1/50 s -- not the 150 of a
// 60 Hz field, which ran the PAL game at 50/60 of real time); the H-BLANK
// counter counts the scan lines of a field (262 NTSC, 312 PAL). The 60 Hz
// values below (kCountsPerTick, kCop0CountsPerTick) are the default and stay
// what every 60 Hz run reads, bit for bit. SetFieldHz re-anchors: each count
// continues from its value at the current tick at the new rate (a save state
// loaded under another rate keeps every clock continuous -- SetAnchor).
#include <cstdint>
#include <functional>

#include "engine/ee/timing/emu_domain.h"

class SyscallTable;

class DerivedClocks {
public:
    // BUSCLK-derived counts per 60 Hz field by T*_MODE.CLKS (hardware facts; the default field rate's).
    static constexpr uint64_t kCountsPerTick[4] = {2457600ull, 153600ull, 9600ull, 262ull};
    static constexpr uint64_t kCop0CountsPerTick = 4915200ull;  // EE core 294.912 MHz / 60
    static constexpr uint32_t kFieldUs = 16667;                  // NTSC field period, us
    static constexpr uint32_t kCdvdKbPerTick = 256;              // per 60 Hz field (15360 KB a second); x 60 / hz a field otherwise
    // 2005-06-15 12:00:00 UTC as seconds since 1970-01-01 (the fixed RTC).
    static constexpr uint64_t kRtcFixedEpochS = 1118836800ull;
    // The clocks' own rates (hardware facts, per second of real time).
    static constexpr uint64_t kBusClkHz = 147456000ull;    // BUSCLK
    static constexpr uint64_t kCop0Hz = 294912000ull;      // the EE core = 2 x BUSCLK
    static constexpr uint32_t kDefaultFieldHz = 60;
    // Counts per field of a T*_MODE.CLKS selector at a field rate: BUSCLK / 1, / 16, / 256 over the field rate
    // (2457600 / 153600 / 9600 at 60; 2949120 / 184320 / 11520 at 50); CLKS 3 (H-BLANK) the lines of a field --
    // 262 at 60 (NTSC 525 / 2, the half line dropped), 312 at 50 (PAL 625 / 2, the same rule).
    static constexpr uint64_t CountsPerField(uint32_t clks, uint32_t hz) {
        return (clks & 3u) == 3u ? (hz == 50u ? 312ull : 262ull)
                                 : kBusClkHz / ((clks & 3u) == 0u ? 1ull : (clks & 3u) == 1u ? 16ull : 256ull) / (hz ? hz : kDefaultFieldHz);
    }
    static constexpr uint64_t Cop0CountsPerField(uint32_t hz) { return kCop0Hz / (hz ? hz : kDefaultFieldHz); }

    struct RtcTime {
        int year, month, day, hour, minute, second;
    };

    // Every count's value at one tick: the clocks advance from it at the field rate (CountsAt). The default anchor is
    // tick 0 with every count 0 (the counts are tick x counts-per-field). A save state records the anchor at its tick
    // (AnchorAt) so the loading run continues each clock from there (SetAnchor), whatever its own field rate.
    struct Anchor {
        uint64_t tick = 0;
        uint64_t counts[4] = {0, 0, 0, 0};   // by CLKS
        uint64_t cop0 = 0;
    };

    explicit DerivedClocks(const EmuDomain& domain) : domain_(domain) {}

    void SetRtcFixed(bool fixed) { rtc_fixed_ = fixed; }
    bool RtcFixed() const { return rtc_fixed_; }

    // The field rate (an emu tick = 1 / hz s; 0 = 60). Re-anchored at the current tick: every count continues from its
    // value now, at the new rate. Call it before the first count is read (te::Clock: at construction).
    void SetFieldHz(uint32_t hz);
    uint32_t FieldHz() const { return field_hz_; }
    uint64_t CountsPerTick(uint32_t clks) const { return per_tick_[clks & 3u]; }
    uint64_t Cop0CountsPerTick() const { return cop0_per_tick_; }
    // The anchor that reproduces every count at `tick` (a save state's), and installing one (its load).
    Anchor AnchorAt(uint64_t tick) const;
    void SetAnchor(const Anchor& anchor) { anchor_ = anchor; }

    // (1) 64-bit unwrapped timer count for a CLKS selector. Domain: emu ticks.
    uint64_t TimerCounts(uint32_t clks) const { return CountsAt(clks, domain_.DomainTick()); }
    uint64_t T0Counts() const { return TimerCounts(2); }
    // (2) Domain: emu ticks.
    uint64_t VblankCount() const { return domain_.DomainTick(); }
    // (3) Domain: boundary count / emu tick parity.
    uint64_t FrameBoundaryCount() const { return domain_.BoundaryCount(); }
    uint32_t SyncVFieldParity() const { return static_cast<uint32_t>(domain_.DomainTick() & 1u); }
    // (4) Domain: emu T0 counts at the last boundary.
    uint64_t BudgetAnchorCounts() const { return CountsAt(2, domain_.LastBoundaryTick()); }
    // (5) Domain: emu ticks x EE core counts per field (free-run model).
    uint64_t Cop0Count() const { return Cop0At(domain_.DomainTick()); }
    // (6) Domain: fixed epoch (+ emu seconds when not pinned: field_hz ticks a second).
    uint64_t RtcEpochSeconds() const;
    RtcTime Rtc() const;
    static RtcTime EpochToCivil(uint64_t seconds);
    // (7) Domain: emu ticks / round(usec / 16667) at 60 (round(usec / the field's us) at another rate).
    static uint32_t HeartbeatTicksFor(uint32_t usec);
    static uint32_t HeartbeatTicksFor(uint32_t usec, uint32_t hz);
    uint64_t IopHeartbeatBeats(uint32_t usec) const;
    // (8) Domain: emu ticks (256 KB refill budget per 60 Hz tick: the same KB a second at any field rate).
    uint64_t CdvdEpochTick() const { return domain_.DomainTick(); }
    uint64_t CdvdBudgetKbSince(uint64_t epoch_tick) const;
    // (9) Not derived: the device clock is the rate master.
    static constexpr bool AudioRateMasterIsDevice() { return true; }

    // Installs the kernel consumers: KernlTimer::SetClock (T0 counts + the
    // boundary wait hook the caller supplies) and SyscallTable::SetTickClock
    // (alarms count emu ticks).
    // LIFETIME: the installed closures capture `this` (and this object holds a
    // reference to its EmuDomain). Both must outlive every consumer call --
    // the composition root owns them for the whole run; a test that installs
    // from a stack object resets the consumers (KernlTimer::ResetForTest(),
    // SetTickClock(nullptr)) before the object dies.
    void InstallKernelClocks(SyscallTable& table, std::function<void(uint64_t)> wait) const;

private:
    // A count at `tick`: the anchor's value plus the ticks since it at the field rate (a tick before the anchor: minus)
    uint64_t CountsAt(uint32_t clks, uint64_t tick) const {
        const uint32_t c = clks & 3u;
        return tick >= anchor_.tick ? anchor_.counts[c] + (tick - anchor_.tick) * per_tick_[c]
                                    : anchor_.counts[c] - (anchor_.tick - tick) * per_tick_[c];
    }
    uint64_t Cop0At(uint64_t tick) const {
        return tick >= anchor_.tick ? anchor_.cop0 + (tick - anchor_.tick) * cop0_per_tick_
                                    : anchor_.cop0 - (anchor_.tick - tick) * cop0_per_tick_;
    }

    const EmuDomain& domain_;
    bool rtc_fixed_ = true;
    uint32_t field_hz_ = kDefaultFieldHz;
    uint64_t per_tick_[4] = {kCountsPerTick[0], kCountsPerTick[1], kCountsPerTick[2], kCountsPerTick[3]};
    uint64_t cop0_per_tick_ = kCop0CountsPerTick;
    Anchor anchor_;
};
