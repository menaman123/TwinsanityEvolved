#include "engine/ee/timing/derived_clocks.h"

#include "engine/ee/hle/kernl_timer.h"
#include "engine/ee/hle/syscall_table.h"

uint64_t DerivedClocks::RtcEpochSeconds() const {
    if (rtc_fixed_) return kRtcFixedEpochS;
    return kRtcFixedEpochS + domain_.DomainTick() / (field_hz_ ? field_hz_ : kDefaultFieldHz);
}

void DerivedClocks::SetFieldHz(uint32_t hz) {
    if (hz == 0) hz = kDefaultFieldHz;
    // every count continues from its value at this tick (the default anchor at tick 0 holds 0s: tick x rate)
    anchor_ = AnchorAt(domain_.DomainTick());
    field_hz_ = hz;
    for (uint32_t c = 0; c < 4; ++c) per_tick_[c] = CountsPerField(c, hz);
    cop0_per_tick_ = Cop0CountsPerField(hz);
}

DerivedClocks::Anchor DerivedClocks::AnchorAt(uint64_t tick) const {
    Anchor a;
    a.tick = tick;
    for (uint32_t c = 0; c < 4; ++c) a.counts[c] = CountsAt(c, tick);
    a.cop0 = Cop0At(tick);
    return a;
}

// Proleptic Gregorian civil date from seconds since 1970-01-01 (days-from-civil
// inverse; valid for the whole positive range the RTC can reach).
DerivedClocks::RtcTime DerivedClocks::EpochToCivil(uint64_t seconds) {
    const uint64_t days = seconds / 86400u;
    const uint32_t sod = static_cast<uint32_t>(seconds % 86400u);
    // Howard Hinnant's civil_from_days, shifted so the era starts on March 1.
    const int64_t z = static_cast<int64_t>(days) + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    RtcTime t;
    t.year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    t.month = static_cast<int>(m);
    t.day = static_cast<int>(d);
    t.hour = static_cast<int>(sod / 3600u);
    t.minute = static_cast<int>((sod / 60u) % 60u);
    t.second = static_cast<int>(sod % 60u);
    return t;
}

DerivedClocks::RtcTime DerivedClocks::Rtc() const { return EpochToCivil(RtcEpochSeconds()); }

uint32_t DerivedClocks::HeartbeatTicksFor(uint32_t usec) {
    const uint32_t ticks = (usec + kFieldUs / 2) / kFieldUs;  // round(usec / 16667)
    return ticks == 0 ? 1u : ticks;
}

uint32_t DerivedClocks::HeartbeatTicksFor(uint32_t usec, uint32_t hz) {
    // the 60 Hz rule exactly (its 16667 us field); another rate's field is 1e6 / hz us (20000 at 50)
    if (hz == 0 || hz == kDefaultFieldHz) return HeartbeatTicksFor(usec);
    const uint32_t field_us = 1000000u / hz;
    const uint32_t ticks = (usec + field_us / 2) / field_us;
    return ticks == 0 ? 1u : ticks;
}

uint64_t DerivedClocks::IopHeartbeatBeats(uint32_t usec) const {
    return domain_.DomainTick() / HeartbeatTicksFor(usec, field_hz_);
}

uint64_t DerivedClocks::CdvdBudgetKbSince(uint64_t epoch_tick) const {
    const uint64_t now = domain_.DomainTick();
    const uint64_t ticks = now > epoch_tick ? now - epoch_tick : 0;
    // 256 KB a 60 Hz field = the drive's KB a second; a 50 Hz field is longer (307.2 KB): the same KB a second
    if (field_hz_ == kDefaultFieldHz) return ticks * kCdvdKbPerTick;
    return ticks * kCdvdKbPerTick * kDefaultFieldHz / (field_hz_ ? field_hz_ : kDefaultFieldHz);
}

void DerivedClocks::InstallKernelClocks(SyscallTable& table, std::function<void(uint64_t)> wait) const {
    const DerivedClocks* self = this;
    KernlTimer::SetClock([self] { return self->T0Counts(); }, std::move(wait));
    table.SetTickClock([self] { return self->VblankCount(); });
}
