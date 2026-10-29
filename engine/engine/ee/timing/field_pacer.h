#pragma once
// FieldPacer: the play-mode field ticker as a LIBRARY class -- the host pacer the
// TickSource's Play policy expects, for consumers that are not the app's
// composition root (a native platform arm over a decompiled title: its one game thread
// meets the boundary through BoundaryProtocol::ParkForPacerTick and needs the
// same pacer app/host/tick_thread.cpp starts in play mode). The cadence is
// TickThread's, with the field rate a parameter:
//
//   * one host thread sleeps to an ABSOLUTE deadline every field period and
//     publishes the tick to the EE side as the timing design fixes it: a pacer
//     is a non-guest thread, so it never runs the tick body itself -- it NOTES
//     a pending tick (TickSync::NotePending, fired on the EE at its next fixed
//     point) and WAKES the waiters (BoundaryProtocol::NotifyTick). No wall value
//     crosses into the time domain: only the count of noted ticks does;
//   * the edges are anchor + k * period with the period the EXACT rational
//     1e9 / hz ns (edge hz is exactly one second after edge 0), so a late wake
//     shortens the following sleep instead of pushing the train later; more
//     than one period behind (a debugger pause, a stall) re-anchors to now
//     rather than firing a burst;
//   * hz is the FIELD rate: 60 (the default game_hz; a perfect 60 Hz
//     console) or 50 (a PAL console's fields);
//   * one field per tick, on the consumer's side too: the game-thread boundary
//     takes its field through BoundaryProtocol::DeliverField -- exactly one
//     noted tick per boundary (the clock contract, rule 3 in the single-field
//     form), a late frame's further noted ticks caught up one per following
//     boundary without parking, bounded by kCatchUpMaxFields (the excess
//     dropped and counted) -- never a multi-field jump of the title's clock.
//
// TickThread (app/host) remains the composition root's pacer and reads the
// TickSource's PeriodUs (turbo); this class is the fixed-rate form for library
// consumers. Stop() joins the thread; the destructor stops it.
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

class BoundaryProtocol;
class TickSync;

class FieldPacer {
public:
    // `on_thread_start` runs first on the pacer thread: the consumer's QoS pin / thread name
    // (ps2r::platform::host::PinCurrentThreadInteractive(name, ThreadRole::Pacer) lives in
    // ps2r_platform, above this library). The sleeps go through the host arm's finest timer
    // (ps2r::host::SleepForNs / RaiseTimerResolution: the Windows 1 ms period + high-resolution
    // waitable timer; nanosleep on POSIX) -- never std::this_thread::sleep_until, whose Win32
    // form rounds to the scheduler quantum.
    FieldPacer(TickSync& ticks, BoundaryProtocol& protocol, uint32_t hz = 60, std::function<void()> on_thread_start = {});
    ~FieldPacer();
    FieldPacer(const FieldPacer&) = delete;
    FieldPacer& operator=(const FieldPacer&) = delete;

    void Start();
    void Stop();
    bool Running() const { return thread_.joinable(); }
    uint32_t Hz() const { return hz_; }

    // Ticks noted so far (each one NotePending + NotifyTick).
    uint64_t Ticks() const { return ticks_noted_.load(std::memory_order_relaxed); }
    // Times the cadence re-anchored to now after falling > one period behind.
    uint64_t Resyncs() const { return resyncs_.load(std::memory_order_relaxed); }

    // The k-th edge of a train anchored at `anchor_ns`, in ns: anchor + k * 1e9 / hz, rounded down per k (pure).
    static int64_t EdgeNs(int64_t anchor_ns, uint64_t k, uint32_t hz);

    // The wall instant (steady-clock ns, the clock ps2r::host::MonotonicNowNs reads) of the edge that noted tick
    // number `tick` (1-based: the n-th tick noted since Start), for a consumer that presents BETWEEN ticks and needs the
    // pacer's own clock for its fraction -- a presenter interpolating two game pictures. Computed from the latest noted
    // edge back along the train's exact period; a tick noted before the last re-anchor is answered on the current train
    // (an approximation the consumer clamps). Returns false before the first tick. Any thread.
    bool EdgeOfTickNs(uint64_t tick, int64_t* edge_ns) const;
    // The train's period in ns (1e9 / hz, rounded down).
    int64_t PeriodNs() const { return static_cast<int64_t>(1000000000ull / (hz_ == 0 ? 60u : hz_)); }

    // An EXTERNAL edge source the train follows while UseExternalEdge(true) -- a display's vertical blank (the Vita's
    // sceDisplayWaitVblankStart), so the fields are the display's refreshes and a consumer that shows one picture every n
    // fields shows it on the same refresh phase every time: `wait` blocks until the next edge; the tick is noted when it
    // returns (edge = that instant) and the timer train is re-anchored on it, so a switch back to the timer continues from
    // the last external edge (no burst, no gap). Set before Start(); `wait` must return within a few periods (Stop()
    // joins after it). Unset or off (the default): the timer train above, unchanged.
    void SetExternalEdge(std::function<void()> wait) { external_wait_ = std::move(wait); }
    void UseExternalEdge(bool on) { use_external_.store(on, std::memory_order_release); }
    bool UsingExternalEdge() const { return use_external_.load(std::memory_order_acquire); }
    // Ticks noted on the external edge (the rest are the timer's).
    uint64_t ExternalTicks() const { return external_ticks_.load(std::memory_order_relaxed); }

private:
    void Loop();

    TickSync& ticks_;
    BoundaryProtocol& protocol_;
    uint32_t hz_;
    std::function<void()> on_thread_start_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> ticks_noted_{0};
    std::atomic<uint64_t> resyncs_{0};
    // The latest noted edge: (tick number, edge ns) published together as one 128-bit-free pair -- the edge is
    // stored first, the tick after (release); a reader takes the tick (acquire) then the edge and re-reads on a
    // change (seqlock in two words).
    std::atomic<int64_t> last_edge_ns_{0};
    std::atomic<uint64_t> last_edge_tick_{0};
    std::function<void()> external_wait_;
    std::atomic<bool> use_external_{false};
    std::atomic<uint64_t> external_ticks_{0};
};
