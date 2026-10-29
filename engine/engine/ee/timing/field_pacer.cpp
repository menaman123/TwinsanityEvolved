#include "engine/ee/timing/field_pacer.h"

#include <chrono>

#include "engine/ee/timing/boundary_protocol.h"
#include "engine/ee/timing/tick_sync.h"
#include "engine/platform/host/platform.h"

FieldPacer::FieldPacer(TickSync& ticks, BoundaryProtocol& protocol, uint32_t hz, std::function<void()> on_thread_start)
    : ticks_(ticks), protocol_(protocol), hz_(hz == 0 ? 60u : hz), on_thread_start_(std::move(on_thread_start)) {}

FieldPacer::~FieldPacer() { Stop(); }

void FieldPacer::Start() {
    if (thread_.joinable()) return;
    stop_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { Loop(); });
}

void FieldPacer::Stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

int64_t FieldPacer::EdgeNs(int64_t anchor_ns, uint64_t k, uint32_t hz) {
    return anchor_ns + static_cast<int64_t>((static_cast<unsigned long long>(k) * 1000000000ull) / (hz == 0 ? 60u : hz));
}

bool FieldPacer::EdgeOfTickNs(uint64_t tick, int64_t* edge_ns) const {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t t0 = last_edge_tick_.load(std::memory_order_acquire);
        if (t0 == 0) return false;
        const int64_t e = last_edge_ns_.load(std::memory_order_acquire);
        const uint64_t t1 = last_edge_tick_.load(std::memory_order_acquire);
        if (t0 != t1) continue;   // the pacer published between the two reads: again
        const int64_t period = PeriodNs();
        if (edge_ns) *edge_ns = tick >= t0 ? e + static_cast<int64_t>(tick - t0) * period : e - static_cast<int64_t>(t0 - tick) * period;
        return true;
    }
    return false;
}

void FieldPacer::Loop() {
    using Clock = std::chrono::steady_clock;
    // The pacer IS the wall clock the game runs on (TickThread's QoS rule): the consumer pins it here.
    if (on_thread_start_) on_thread_start_();
    // The finest timer the OS has for the edge train (Win32: timeBeginPeriod(1) + the
    // high-resolution waitable timer behind ps2r::host::SleepForNs; POSIX: nanosleep).
    ps2r::host::RaiseTimerResolution();
    auto sleep_until = [](Clock::time_point deadline) {
        for (;;) {
            const auto now = Clock::now();
            if (now >= deadline) return;
            ps2r::host::SleepForNs(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count()));
        }
    };
    auto to_ns = [](Clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    };
    auto edge_at = [&](Clock::time_point anchor, uint64_t k) {
        return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
            std::chrono::nanoseconds(EdgeNs(to_ns(anchor), k, hz_))));
    };
    auto anchor = Clock::now();
    uint64_t k = 1;
    auto next = edge_at(anchor, k);
    while (!stop_.load(std::memory_order_acquire)) {
        if (external_wait_ && use_external_.load(std::memory_order_acquire)) {
            // The external edge (SetExternalEdge): the display's refresh is the field. The tick is noted at the instant
            // the wait returned, and the timer train re-anchored there (a switch back continues from this edge).
            external_wait_();
            if (stop_.load(std::memory_order_acquire)) break;
            const auto edge = Clock::now();
            ticks_.NotePending(TickSync::Cause::Demand);
            protocol_.NotifyTick();
            const uint64_t noted = ticks_noted_.fetch_add(1, std::memory_order_release) + 1;
            last_edge_ns_.store(to_ns(edge), std::memory_order_release);
            last_edge_tick_.store(noted, std::memory_order_release);
            external_ticks_.fetch_add(1, std::memory_order_relaxed);
            anchor = edge;
            k = 1;
            next = edge_at(anchor, k);
            continue;
        }
        sleep_until(next);
        if (stop_.load(std::memory_order_acquire)) break;
        ticks_.NotePending(TickSync::Cause::Demand);
        protocol_.NotifyTick();
        const uint64_t noted = ticks_noted_.fetch_add(1, std::memory_order_release) + 1;
        // the edge this tick stood on, for EdgeOfTickNs (the edge first, then the tick that validates it)
        last_edge_ns_.store(to_ns(next), std::memory_order_release);
        last_edge_tick_.store(noted, std::memory_order_release);
        const auto now = Clock::now();
        ++k;
        next = edge_at(anchor, k);
        if (next < now) {   // more than one period behind: re-anchor, no burst
            anchor = now;
            k = 1;
            next = edge_at(anchor, k);
            resyncs_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
