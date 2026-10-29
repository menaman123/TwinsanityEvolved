// platform/nativeee (host side) -- te_tick_lock.h: the Vita's adaptive, vsync-aligned 60 / 30 / 20 Hz tick lock.
#include "te_tick_lock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "te_harness.h"
#include "te_host.h"

#if defined(__vita__)
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>
// vitaGL r1488's display side, read (and its swap interval written) by the display callback below: the interval the
// display queue callback waits after setting a picture's frame buffer (vgl.c vsync_interval, read right after the custom
// callback runs: gxm.c display_queue_callback), the display buffers' addresses and the one the next swap queues (gxm.c),
// and the custom callback's setter. Weak: a vitaGL without them still links (their address is then null: no lock).
extern "C" {
extern uint32_t vsync_interval __attribute__((weak));
extern void* gxm_color_surfaces_addr[] __attribute__((weak));
extern unsigned int gxm_back_buffer_index __attribute__((weak));
extern uint8_t gxm_display_buffer_count __attribute__((weak));
void vglSetDisplayCallback(void (*cb)(void* framebuf)) __attribute__((weak));
}
#endif

namespace te::tick_lock {

namespace {

[[maybe_unused]] int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
long EnvLong(const char* name, long fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::strtol(v, nullptr, 10) : fallback;
}
double EnvDouble(const char* name, double fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::atof(v) : fallback;
}
// THE TWO FAMILIES of the lock. At a 60 Hz game rate the pacer's fields are the display's refreshes while locked
// (60 / 30 / 20 Hz: a picture every 1 / 2 / 3 refreshes, held exactly that long). At the PAL disc's own 50 Hz (game.hz = 50,
// --hz 50) the fields are the pacer's 20 ms TIMER edges -- a 60 Hz panel has no 50 Hz vblank -- and every picture is shown
// from the first refresh after it is ready (the display callback's interval 1): 50 a second on 60 refreshes = 4 pictures in
// 5 held one refresh and 1 held two (the cadence 1,1,1,1,2), the game's clock exactly the PAL console's (frameTime 180 a
// tick); the half-rate fallback is 25 Hz (2 timer fields a tick: frameTime 360, the PAL game dropping a frame), its
// pictures held 2 or 3 refreshes (2.4 on average). g_timer_family: the 50 Hz family (set once, before any picture).
std::atomic<bool> g_timer_family{false};
const char* RateName(uint32_t n) {
    if (g_timer_family.load(std::memory_order_relaxed)) return n >= 2 ? "25" : "50";
    return n >= 3 ? "20" : n == 2 ? "30" : "60";
}

enum class Mode : uint8_t { Auto, Fixed, AB };

// A coarse histogram of milliseconds (1 ms buckets to 63, the last one 63+), written by one thread, read and reset by the
// census line (a lost update at the reset is a sample, not a fault)
struct MsHist {
    static constexpr int kBuckets = 64;
    std::atomic<uint32_t> n[kBuckets];
    std::atomic<uint32_t> max_us{0};
    void Note(double ms) {
        if (!(ms >= 0)) return;
        const int b = std::min(kBuckets - 1, int(ms));
        n[b].fetch_add(1, std::memory_order_relaxed);
        const uint32_t us = uint32_t(std::min(ms * 1000.0, 4.0e9));
        if (us > max_us.load(std::memory_order_relaxed)) max_us.store(us, std::memory_order_relaxed);
    }
    // "p50 / p90 / max" of the window, reset
    std::string TakeLine() {
        uint32_t c[kBuckets];
        uint64_t total = 0;
        for (int b = 0; b < kBuckets; ++b) { c[b] = n[b].exchange(0, std::memory_order_relaxed); total += c[b]; }
        const double mx = max_us.exchange(0, std::memory_order_relaxed) / 1000.0;
        if (!total) return "-";
        auto pct = [&](double q) {
            const uint64_t want = uint64_t(q * double(total) + 0.999);
            uint64_t run = 0;
            for (int b = 0; b < kBuckets; ++b) { run += c[b]; if (run >= want) return b + 1; }
            return kBuckets;
        };
        char buf[64];
        std::snprintf(buf, sizeof buf, "<%d / <%d / %.1f", pct(0.5), pct(0.9), mx);
        return buf;
    }
};

struct State {
    // ---- configuration (the game thread's; `active` and the mode are re-derived when the setting changes)
    bool read = false;
    bool vita_play = false;          // the Vita, play mode, a 60 or 50 Hz game rate: the lock may engage
    bool active = false;
    uint32_t hz = 60;                // the game rate (te::Settings::field_hz): 60 = the vblank family, 50 = the timer family
    uint32_t n_max = 3;              // the floor: 3 fields (20 Hz) at 60, 2 (25 Hz) at 50
    Mode mode = Mode::Auto;
    uint32_t fixed = 1;              // Mode::Fixed's lock
    int setting = 0;                 // the FRAME RATE row (0 auto, 1 60, 2 30)
    std::string knob;                // TE_VITA_TICK_LOCK (overrides the setting)
    uint64_t ab_every = 600;
    int ab_arm = -1;                 // Mode::AB: 0 auto, 1 off (60)
    uint64_t lock_from = 0;          // TE_VITA_TICK_LOCK_FROM: the lock waits for this boundary (a test: a pad log at 60 to
                                     // its scene, then the lock); 0 = from the start
    bool held = false;               // before lock_from: every picture tick is one field
    // THE MISS SCORE: each picture tick at the lock weighs its miss -- a slow tick (the game ran past its
    // fields) or a drain 2, a picture held long on the display (at 30 / 20) 2, a picture replaced before drawn (at 60: the
    // drawing thread or the GPU late, the game itself on time) 1 -- and a HICCUP (the first 100 ms of a run of missing
    // ticks: one stall of the drawing thread / the GPU / the card) counts at most hiccup_cap, whatever its length; the
    // ticks of a longer run past its first 100 ms count in full (a sustained shortfall is no hiccup)
    uint32_t down[4] = {0, 12, 20, 0};     // the score over the window that drops the lock from n to n + 1 (60: "6 slow ticks
                                           // of 30"; 30 -> 20 only when the 30 lock misses persistently)
    uint32_t window = 30;                  // picture ticks the misses are counted over
    uint32_t hiccup_cap = 4;
    double down60_p90_ms = 14.0;           // at 60: the game work's 90th percentile over the window above this and a score of
    uint32_t down60_p90_score = 4;         // at least this drop to 30 too (the game cannot hold 60: its misses are not luck)
    double up_ms[4] = {0, 13.0, 28.0, 0};  // [n]: the 90th percentile of a calm second's work stays under it to rise to n
    double up_max_ms[4] = {0, 16.0, 31.0, 0};   // [n]: and no tick of that second over this
    uint64_t dwell_fields = 180;           // fields between two switches at least (3 s: no flip-flopping; 150 at 50 Hz)
    // ---- the policy (the game thread's)
    uint32_t n = 1;                  // the lock: fields per picture tick
    uint32_t plan = 0;
    bool plan_set = false;
    bool peek_eligible = false;      // PeekPlan's picture tick may lock (a streak, a lock state): CommitPlan keeps it
    bool plan_eligible = false;
    bool taken_eligible = false;     // the boundary TakePlan just consumed was an eligible picture tick (the policy's input)
    bool backpressure = false;       // this picture's hand-over waited for the drawing thread (NoteBackpressure)
    bool taken_drain = false;        // the boundary TakePlan consumed is a drain tick: one field more than its lock
    uint32_t taken_lock = 0;         // ... and its lock (the plan's fields before the drain's one)
    bool prev_picture = false;       // the previous boundary was a picture tick
    uint8_t miss_w[32] = {};         // the last 32 picture ticks at this lock: each one's miss weight (0 = none), newest at
                                     // (ticks_at_n - 1) % 32
    uint32_t ticks_at_n = 0;         // picture ticks at the current lock since it was entered
    uint64_t switched_at = 0;        // `fields` at the last switch (the dwell)
    // the last 32 picture ticks at this lock: the game work and the drawing thread's (ms), newest at (ring_at - 1) % 32
    float ring_work[32] = {}, ring_draw[32] = {};
    uint32_t ring_at = 0;
    uint32_t calm = 0;               // consecutive picture ticks whose trailing second was calm (the rise test)
    uint32_t calm_factor = 1;        // the calm seconds the next rise asks for (1, 2, 4, 8, 16)
    uint64_t fields = 0;             // every boundary's fired fields (the policy's clock)
    uint64_t rose_at = 0;            // `fields` at the last rise (0: none pending a verdict)
    uint32_t replaced = 0;           // pictures replaced since the last boundary
    uint64_t display_misses_seen = 0;
    uint32_t expected_display = 0;   // display misses an over-the-lock tick explains (not counted twice)
    uint32_t ineligible_run = 0;     // boundaries since the last eligible picture tick (a load, a movie, a boot screen)
    uint32_t warmup = 0;             // ticks of a fresh window whose misses are not counted (a scene's first ticks)
    double last_draw_ms = 0;         // the drawing thread's last reported work (a tick without a picture drawn keeps it)
};
State S;
// The windows: a drop needs this many ticks at the current lock first (a scene's first ticks are often its load's), and
// they start afresh after this many boundaries of something else
constexpr uint32_t kMinHistory = 8;
constexpr uint32_t kFreshAfter = 30;

// ---- the census (atomics: the game thread writes, the drawing thread's census line reads) -------------------------------
std::atomic<uint32_t> g_n{1};                 // the lock now
std::atomic<int> g_arm{-1};                   // the A/B arm now (-1: no A/B)
std::atomic<bool> g_display_edge{false};      // the pacer follows the display's vblank
std::atomic<uint64_t> g_presents{0};
std::atomic<uint64_t> g_boundaries{0}, g_other{0}, g_other_fields{0};
std::atomic<uint64_t> g_held{0};              // picture ticks outside the lock (a streak's first, loading / boot screens)
std::atomic<uint64_t> g_drains{0};            // drain ticks (a backed-up pipeline: one field more, once)
std::atomic<uint64_t> g_ticks[4], g_tick_fields[4];   // picture ticks / their fields by plan (1..3)
std::atomic<uint64_t> g_miss_over{0}, g_miss_replaced{0}, g_miss_display{0}, g_misses{0};
std::atomic<uint64_t> g_switch[4];            // 0: 60->30, 1: 30->20, 2: 20->30, 3: 30->60
std::atomic<uint32_t> g_calm_factor{1};
MsHist g_work, g_draw, g_present;
std::atomic<uint32_t> g_draw_max_us{0};      // the drawing thread's max work since the policy last read it
// the display (the callback thread)
// A picture's TAG (BeforeSwap -> the display callback): bits 0-1 its lock's fields (0 = not a picture tick, 1..3), bit 2 the
// timer family (50 Hz: the picture shown from the first refresh after it is ready -- interval 1 -- whatever its fields).
// The flip census's classes: 0 other, 1 60 Hz, 2 30 Hz, 3 20 Hz, 5 50 Hz, 6 25 Hz (= the tag)
constexpr uint32_t kTagTimer = 4u;
constexpr int kTagClasses = 8;
std::atomic<uint32_t> g_buffer_tag[8];        // the tag of the picture queued to display buffer i (0: not a picture tick)
std::atomic<bool> g_callback_installed{false};
std::atomic<uint64_t> g_flips{0}, g_flips_by[kTagClasses][5];   // [the picture's tag class][vblanks held 1, 2, 3, 4+ (index 1..4)]
std::atomic<uint64_t> g_display_misses{0};
std::atomic<uint64_t> g_longest_gap_ns{0};
// the window line's previous totals
struct Totals {
    uint64_t boundaries = 0, other = 0, other_fields = 0, held = 0, drains = 0, ticks[4] = {}, tick_fields[4] = {}, over = 0, replaced = 0, display = 0,
             misses = 0, sw[4] = {}, flips = 0, flips_by[kTagClasses][5] = {};
};
Totals g_last;

// THE GAME TIME VS WALL: the game's T0 counts at the latest boundary and that boundary's wall instant (the game
// thread writes them per boundary, the census line reads both: a sequence lock -- odd while a write is in progress)
std::atomic<uint32_t> g_gt_seq{0};
std::atomic<uint64_t> g_gt_t0{0}, g_gt_ns{0}, g_gt_boundary{0};
struct GameTimeMark {
    uint64_t t0 = 0, ns = 0, boundary = 0;
    bool valid = false;
};
GameTimeMark g_gt_first, g_gt_last;   // the census line's (one reader: the drawing thread's NotePace)
bool ReadGameTime(GameTimeMark* m) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const uint32_t s1 = g_gt_seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        m->t0 = g_gt_t0.load(std::memory_order_relaxed);
        m->ns = g_gt_ns.load(std::memory_order_relaxed);
        m->boundary = g_gt_boundary.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_gt_seq.load(std::memory_order_relaxed) == s1) { m->valid = m->ns != 0; return m->valid; }
    }
    return false;
}
// The clause: the fields of game time the window gave (T0 counts / the field's counts) against the wall's, the ratio,
// the drift this window and since the first window. The game's clock is Platform::Time's 576000 counts a second
// (game/include/platform/time.h TicksPerSecond) at every rate: a field of game time is 576000 / hz counts.
std::string GameTimeClause(uint32_t hz) {
    GameTimeMark now;
    if (!ReadGameTime(&now)) return std::string();
    if (!g_gt_first.valid) {
        // the first window: its end is the mark every later window (and the "since" total) counts from
        g_gt_first = g_gt_last = now;
        char b[160];
        std::snprintf(b, sizeof b, "; game time vs wall: measured from boundary %llu (this window's end) at %u Hz",
                      (unsigned long long)now.boundary, hz);
        return b;
    }
    const GameTimeMark prev = g_gt_last;
    g_gt_last = now;
    const double counts_per_s = 576000.0;
    const double game_s = double(now.t0 - prev.t0) / counts_per_s, wall_s = double(now.ns - prev.ns) / 1e9;
    const double all_game_s = double(now.t0 - g_gt_first.t0) / counts_per_s, all_wall_s = double(now.ns - g_gt_first.ns) / 1e9;
    char b[300];
    std::snprintf(b, sizeof b, "; game time vs wall: %.1f game fields / %.1f wall fields at %u Hz (%.3f s of game time in %.3f s, %.1f %%); "
                  "drift %+.1f ms this window, %+.1f ms since boundary %llu",
                  game_s * hz, wall_s * hz, hz, game_s, wall_s, wall_s > 0 ? 100.0 * game_s / wall_s : 0.0, (game_s - wall_s) * 1000.0,
                  (all_game_s - all_wall_s) * 1000.0, (unsigned long long)g_gt_first.boundary);
    return b;
}

void Refresh() {
    State& s = S;
#if defined(__vita__)
    const te::Settings& set = te::GetSettings();
    s.hz = set.field_hz == 50 ? 50 : 60;
    s.vita_play = !set.deterministic && (set.field_hz == 0 || set.field_hz == 60 || set.field_hz == 50);
#else
    s.hz = 60;
    s.vita_play = false;
#endif
    s.n_max = s.hz == 50 ? 2 : 3;   // 20 Hz at 60; 25 Hz -- the PAL game's own half rate -- at 50
    g_timer_family.store(s.hz == 50, std::memory_order_relaxed);
    s.mode = Mode::Auto;
    s.fixed = 1;
    const std::string& k = s.knob;
    // (the knob names the rate: 60 / 50 / off = the full rate, 30 / 25 = the half rate, 20 = the third -- clamped to the
    // family's floor)
    if (k == "60" || k == "50" || k == "off" || k == "0") { s.mode = Mode::Fixed; s.fixed = 1; }
    else if (k == "30" || k == "25") { s.mode = Mode::Fixed; s.fixed = 2; }
    else if (k == "20") { s.mode = Mode::Fixed; s.fixed = std::min<uint32_t>(3, s.n_max); }
    else if (k == "ab") { s.mode = Mode::AB; }
    else if (k == "auto") { s.mode = Mode::Auto; }
    else if (s.setting == 1) { s.mode = Mode::Fixed; s.fixed = 1; }
    else if (s.setting == 2) { s.mode = Mode::Fixed; s.fixed = 2; }
    s.active = s.vita_play && !(s.mode == Mode::Fixed && s.fixed == 1);
    if (s.mode == Mode::Fixed) s.n = s.active ? s.fixed : 1;
    if (!s.active) s.n = 1;
    if (s.mode != Mode::AB) s.ab_arm = -1;
    g_n.store(s.n, std::memory_order_relaxed);
    g_arm.store(s.ab_arm, std::memory_order_relaxed);
}

void ReadOnce() {
    State& s = S;
    if (s.read) return;
    s.read = true;
    if (const char* v = std::getenv("TE_VITA_TICK_LOCK")) s.knob = v;
    s.ab_every = uint64_t(std::max(30L, EnvLong("TE_VITA_TICK_LOCK_AB_EVERY", 600)));
    s.lock_from = uint64_t(std::max(0L, EnvLong("TE_VITA_TICK_LOCK_FROM", 0)));
    s.held = s.lock_from > 0;
    s.down[1] = uint32_t(std::clamp(EnvLong("TE_VITA_LOCK_DOWN60", 12), 1L, 64L));
    s.down[2] = uint32_t(std::clamp(EnvLong("TE_VITA_LOCK_DOWN30", 20), 1L, 64L));
    s.window = uint32_t(std::clamp(EnvLong("TE_VITA_LOCK_WINDOW", 30), 4L, 32L));
    s.hiccup_cap = uint32_t(std::clamp(EnvLong("TE_VITA_LOCK_HICCUP_CAP", 4), 1L, 64L));
    s.down60_p90_ms = EnvDouble("TE_VITA_LOCK_DOWN60_P90_MS", 14.0);
    s.down60_p90_score = uint32_t(std::clamp(EnvLong("TE_VITA_LOCK_DOWN60_P90_SCORE", 4), 1L, 64L));
    s.up_ms[1] = EnvDouble("TE_VITA_LOCK_UP60_MS", 13.0);
    s.up_ms[2] = EnvDouble("TE_VITA_LOCK_UP30_MS", 28.0);
    s.up_max_ms[1] = EnvDouble("TE_VITA_LOCK_UP60_MAX_MS", 16.0);
    s.up_max_ms[2] = EnvDouble("TE_VITA_LOCK_UP30_MAX_MS", 31.0);
    s.dwell_fields = uint64_t(std::clamp(EnvLong("TE_VITA_LOCK_DWELL_MS", 3000), 0L, 60000L) * 60 / 1000);
    Refresh();
    if (s.hz == 50) {
        // the 50 Hz family's policy: 50 while the ticks fit their 20 ms (a late tick borrows from the next one's
        // park -- the train is the timer's -- and misses only when the lateness passes a whole field); the miss score of
        // TE_VITA_LOCK_DOWN50 (12: six slow ticks of 30) drops 50 -> 25, or 6 with the game work's p90 over 23 ms (the game
        // far over its field -- 6: more than one hiccup's capped 4, so a single stall of the card or the host does not drop
        // the rate for the dwell's 3 s: an emulator run dropped at boundary 2290 on one 67 ms emulator stall with 4); a calm
        // second at 25 (25 ticks: no miss, the game's and the drawing thread's work p90 under 19 ms, none over 24) rises to 50;
        // the dwell and the backoff as at 60, in 50 Hz fields
        s.down[1] = uint32_t(std::clamp(EnvLong("TE_VITA_LOCK_DOWN50", 12), 1L, 64L));
        s.down[2] = 64;   // (no third rate: 25 is the floor)
        s.down60_p90_ms = EnvDouble("TE_VITA_LOCK_DOWN50_P90_MS", 23.0);
        s.down60_p90_score = uint32_t(std::clamp(EnvLong("TE_VITA_LOCK_DOWN50_P90_SCORE", 6), 1L, 64L));
        s.up_ms[1] = EnvDouble("TE_VITA_LOCK_UP50_MS", 19.0);
        s.up_max_ms[1] = EnvDouble("TE_VITA_LOCK_UP50_MAX_MS", 24.0);
        s.dwell_fields = uint64_t(std::clamp(EnvLong("TE_VITA_LOCK_DWELL_MS", 3000), 0L, 60000L) * 50 / 1000);
    }
    if (!s.vita_play) return;
    if (s.hz == 50) {
        const char* what = s.mode == Mode::AB ? "ab (auto and off = 50 alternating)" : s.mode == Mode::Auto ? "auto (50/25, 25 the floor)"
                           : s.fixed == 1 ? "50 (fixed: no fallback)" : "25 (fixed)";
        std::fprintf(stderr, "[te.vita.lock] tick lock: %s from %s; the game rate is the PAL disc's 50 Hz: the pacer is the 20 ms "
                             "TIMER (a 60 Hz panel has no 50 Hz refresh) and every picture is shown from the first refresh after "
                             "it is ready (the display callback's interval 1: 4 pictures in 5 held one refresh, 1 two -- 50 a "
                             "second); a 25 Hz tick parks for 2 timer fields and fires them together, drawn in full and held "
                             "2-3 refreshes. Auto: a miss score over the last %u picture ticks (a slow tick or a drain 2, a "
                             "picture held long 2, a picture replaced 1; a hiccup of up to 100 ms at most %u) of %u drops 50 "
                             "-> 25 (or %u with the game work's p90 over %.1f ms); a calm second at 25 (no miss, the game and "
                             "drawing thread work p90 under %.1f ms, none over %.1f) rises to 50; switches at least %.1f s apart%s\n",
                     what, s.knob.empty() ? "the FRAME RATE setting (display.tick_lock)" : "TE_VITA_TICK_LOCK", s.window, s.hiccup_cap,
                     s.down[1], s.down60_p90_score, s.down60_p90_ms, s.up_ms[1], s.up_max_ms[1], double(s.dwell_fields) / 50.0,
                     s.mode == Mode::AB ? (" -- A/B every " + std::to_string(s.ab_every) + " presents").c_str() : "");
        return;
    }
    const char* what = s.mode == Mode::AB ? "ab (auto and off = 60 alternating)" : s.mode == Mode::Auto ? "auto (60/30, 20 the floor)"
                       : s.fixed == 1 ? "60 (fixed: one refresh a tick)" : s.fixed == 2 ? "30 (fixed)" : "20 (fixed)";
    std::fprintf(stderr, "[te.vita.lock] tick lock: %s from %s; a picture tick at the lock spans its fields (the boundary parks "
                         "for 2 / 3 display refreshes and fires them together), drawn in full (no low-latency replacement) and "
                         "held on the display that many refreshes; the pacer follows the display's vblank while locked. Auto: "
                         "a miss score over the last %u picture ticks (a slow tick or a drain 2, a picture held long 2, a picture "
                         "replaced 1; a hiccup of up to 100 ms at most %u) of %u drops 60 -> 30 (or %u with the game work's p90 "
                         "over %.1f ms), %u drops 30 -> 20; a calm second (no miss, the game and drawing thread work p90 under "
                         "%.1f ms, none over %.1f) rises to 60, p90 under %.1f ms to 30; switches at least %.1f s apart%s\n",
                 what, s.knob.empty() ? "the FRAME RATE setting (display.tick_lock)" : "TE_VITA_TICK_LOCK", s.window, s.hiccup_cap,
                 s.down[1], s.down60_p90_score, s.down60_p90_ms, s.down[2], s.up_ms[1], s.up_max_ms[1], s.up_ms[2],
                 double(s.dwell_fields) / 60.0,
                 s.mode == Mode::AB ? (" -- A/B every " + std::to_string(s.ab_every) + " presents").c_str() : "");
}

void Switch(uint32_t to, uint64_t boundary, uint32_t misses, const char* why) {
    State& s = S;
    const uint32_t from = s.n;
    if (to == from) return;
    const int k = from == 1 && to == 2 ? 0 : from == 2 && to == 3 ? 1 : from == 3 && to == 2 ? 2 : from == 2 && to == 1 ? 3 : -1;
    if (k >= 0) g_switch[k].fetch_add(1, std::memory_order_relaxed);
    if (to > from) {
        // a drop within 6 s of a rise: that rise came too soon -- the next one waits twice as long
        if (s.rose_at && s.fields - s.rose_at < 6ull * s.hz) s.calm_factor = std::min(16u, s.calm_factor * 2);
        s.rose_at = 0;
    } else {
        s.rose_at = std::max<uint64_t>(1, s.fields);
    }
    s.switched_at = std::max<uint64_t>(1, s.fields);
    g_calm_factor.store(s.calm_factor, std::memory_order_relaxed);
    std::fprintf(stderr, "[te.vita.lock] %s -> %s Hz at boundary %llu (%s; miss score %u over the last %u picture ticks)\n", RateName(from),
                 RateName(to), (unsigned long long)boundary, why, misses, std::min(s.ticks_at_n, s.window));
    s.n = to;
    std::memset(s.miss_w, 0, sizeof s.miss_w);
    s.ticks_at_n = 0;
    s.calm = 0;
    g_n.store(to, std::memory_order_relaxed);
}

#if defined(__vita__)
// vitaGL's display queue callback, right before it sets `framebuf` for the next refresh and waits vsync_interval refreshes:
// the picture's own interval (its lock), and the flip census
uint16_t g_last_vcount = 0;
int64_t g_last_flip_ns = 0;
uint32_t g_last_tag = 0;
bool g_have_flip = false;
void OnDisplay(void* framebuf) {
    uint32_t tag = 0;
    if (&gxm_color_surfaces_addr && &gxm_display_buffer_count) {
        const int count = std::min<int>(gxm_display_buffer_count, 8);
        for (int i = 0; i < count; ++i)
            if (gxm_color_surfaces_addr[i] == framebuf) { tag = g_buffer_tag[i].load(std::memory_order_acquire); break; }
    }
    // a locked picture of the vblank family is held its fields' worth of refreshes; the timer family's (50 / 25 Hz) and the
    // 60 Hz pictures one refresh at least -- the next picture is shown from the refresh after it is ready
    const uint32_t fields = tag & 3u;
    if (&vsync_interval) vsync_interval = (tag & kTagTimer) ? 1u : fields >= 2 ? fields : 1u;
    const uint16_t vc = uint16_t(sceDisplayGetVcount());
    const int64_t now = NowNs();
    if (g_have_flip) {
        // the previous picture was on the screen from the refresh after its callback to the one after this callback
        const uint32_t held = uint16_t(vc - g_last_vcount);
        const uint32_t idx = std::min<uint32_t>(std::max<uint32_t>(held, 1), 4);
        g_flips_by[g_last_tag & 7u][idx].fetch_add(1, std::memory_order_relaxed);
        // held longer than its lock: the vblank family's N refreshes; the timer family's ceil(N x 60 / 50) -- a 50 Hz
        // picture 2 refreshes at most, a 25 Hz one 3
        const uint32_t last = g_last_tag & 3u;
        const uint32_t limit = (g_last_tag & kTagTimer) ? (last * 6 + 4) / 5 : last;
        if (last >= 1 && fields >= 1 && held > limit) g_display_misses.fetch_add(1, std::memory_order_relaxed);
        const uint64_t gap = uint64_t(now - g_last_flip_ns);
        if (gap > g_longest_gap_ns.load(std::memory_order_relaxed)) g_longest_gap_ns.store(gap, std::memory_order_relaxed);
    }
    g_flips.fetch_add(1, std::memory_order_relaxed);
    g_have_flip = true;
    g_last_vcount = vc;
    g_last_flip_ns = now;
    g_last_tag = tag;
}
#endif

}  // namespace

void Configure(int setting) {
    State& s = S;
    s.setting = std::clamp(setting, 0, 2);
    if (s.read) {   // a change from the Options row: applied from the next picture tick
        const uint32_t before = s.n;
        Refresh();
        if (s.vita_play) {
            const bool timer = s.hz == 50;
            std::fprintf(stderr, "[te.vita.lock] FRAME RATE %s: %s%s\n",
                         s.setting == 0 ? (timer ? "auto (50/25)" : "auto (60/30)") : s.setting == 1 ? (timer ? "50" : "60") : (timer ? "25" : "30"),
                         s.active ? (s.mode == Mode::Fixed ? "the lock fixed at " : "adaptive, now ") : "the lock off (",
                         s.active ? (std::string(RateName(s.n)) + " Hz").c_str() : (std::string(RateName(before)) + " Hz before)").c_str());
        }
    }
}

bool Active() {
    ReadOnce();
    return S.active;
}

// The game states the lock serves (te_harness.h StateName): the title, the menus, play, the cutscenes, the pause, the
// gallery, game over, the credits. Not the boot, the loading screens (LoadingTitle / LoadingLevel), NewGame, StartingPlay,
// the movies, the restarts: a load's ticks step the loader once per tick, so a lock there only stretches the load (the
// title's load ran 8 % longer at a fixed 30 on x86 in the emulator), and nothing in a loading picture moves smoothly
bool LockState() {
    unsigned state = 0, last = 0, next = 0;
    if (!te::harness::ReadGameState(&state, &last, &next)) return false;
    switch (state) {
    case 7: case 8: case 12: case 13: case 15: case 16: case 17: case 18: case 19: return true;
    default: return false;
    }
}

uint32_t PeekPlan(bool picture) {
    State& s = S;
    s.peek_eligible = false;
    if (!picture) return 0;
    ReadOnce();
    if (!s.active || s.held) return 1;
    // the first picture tick after another boundary is one field (the streak starts), as is one outside the lock's states
    if (!s.prev_picture || !LockState()) return 1;
    s.peek_eligible = true;
    return s.n;
}

void CommitPlan(uint32_t fields) {
    State& s = S;
    s.plan = fields;
    s.plan_set = true;
    s.plan_eligible = s.peek_eligible && fields > 0;
    s.peek_eligible = false;
    // A DRAIN: a locked picture whose hand-over had to wait for the drawing thread -- the pipeline is a picture or more
    // behind. With the game making a picture every N refreshes and the display taking one every N, a backlog never drains
    // by itself (each later hand-over waits again, the game's ticks then run on the drawing thread's frees, a period more
    // latency for good -- measured: 60 of 60 hand-overs waited after a 20 Hz stretch). One tick of N + 1 fields
    // makes one picture less, and the queues drain.
    s.taken_drain = false;
    if (s.backpressure && s.plan_eligible && fields >= 2) {
        s.plan = fields + 1;
        s.taken_drain = true;   // (TakePlan keeps it for NoteBoundary)
    }
    s.taken_lock = fields;
    s.backpressure = false;
}

void NoteReplaced() { ++S.replaced; }

void NoteBackpressure() { S.backpressure = true; }

uint32_t TakePlan() {
    State& s = S;
    const uint32_t plan = s.plan_set ? s.plan : 0;
    s.taken_eligible = s.plan_set && s.plan_eligible;
    if (!s.plan_set) { s.taken_drain = false; s.taken_lock = 0; }
    s.plan_set = false;
    s.plan_eligible = false;
    s.plan = 0;
    return plan;
}

void NoteBoundary(uint64_t boundary, uint32_t parked, uint32_t fired, double work_ms) {
    State& s = S;
    // the tick's lock: what the plan asked (a drain tick parked one field more than its lock, on purpose)
    const bool drain = parked > 0 && s.taken_drain;
    const uint32_t planned = drain ? s.taken_lock : parked;
    s.taken_drain = false;
    s.fields += fired;
    g_boundaries.fetch_add(1, std::memory_order_relaxed);
    const bool picture = planned > 0;
    s.prev_picture = picture;
    if (!picture) {
        ++s.ineligible_run;
        g_other.fetch_add(1, std::memory_order_relaxed);
        g_other_fields.fetch_add(fired, std::memory_order_relaxed);
        g_display_edge.store(false, std::memory_order_relaxed);   // movies / loads: the timer's fields (one refresh a tick)
        return;
    }
    const uint32_t p = std::min<uint32_t>(planned, 3);
    g_ticks[p].fetch_add(1, std::memory_order_relaxed);
    g_tick_fields[p].fetch_add(fired, std::memory_order_relaxed);
    if (!s.read || !s.active) {
        s.replaced = 0;
        g_display_edge.store(false, std::memory_order_relaxed);
        return;
    }
    if (s.held) {
        s.replaced = 0;
        if (boundary + 1 >= s.lock_from) {
            s.held = false;
            std::fprintf(stderr, "[te.vita.lock] TE_VITA_TICK_LOCK_FROM %llu: the lock engages from here (%s Hz)\n",
                         (unsigned long long)s.lock_from, RateName(s.n));
        }
        g_display_edge.store(!s.held && s.n >= 2 && s.hz == 60, std::memory_order_relaxed);
        return;
    }
    if (!s.taken_eligible) {
        // a picture tick outside the lock (the streak's first, a loading screen, a boot screen): one field, no policy input
        ++s.ineligible_run;
        s.replaced = 0;
        g_held.fetch_add(1, std::memory_order_relaxed);
        g_draw_max_us.store(0, std::memory_order_relaxed);
        s.display_misses_seen = g_display_misses.load(std::memory_order_relaxed);
        g_display_edge.store(false, std::memory_order_relaxed);
        return;
    }
    if (s.ineligible_run >= kFreshAfter) {
        // back from half a second or more of other boundaries (a load, a movie, a boot screen): the windows start afresh --
        // the first ticks of a scene just loaded are not the scene's cadence (the title's first tick after its load took
        // 151 ms on x86 and dropped the lock for a second)
        std::memset(s.miss_w, 0, sizeof s.miss_w);
        s.ticks_at_n = 0;
        s.calm = 0;
        s.expected_display = 0;
        s.warmup = kMinHistory;
    }
    s.ineligible_run = 0;
    if (work_ms >= 0) g_work.Note(work_ms);
    // the drawing thread's work since the last boundary (its max; none drawn: the last one stands)
    const uint32_t draw_us = g_draw_max_us.exchange(0, std::memory_order_relaxed);
    if (draw_us) s.last_draw_ms = draw_us / 1000.0;
    const uint64_t dm = g_display_misses.load(std::memory_order_relaxed);
    uint64_t new_display = dm - s.display_misses_seen;
    s.display_misses_seen = dm;
    const bool over = fired > parked;   // (a drain tick's extra field is its plan, not a miss)
    if (drain) g_drains.fetch_add(1, std::memory_order_relaxed);
    const bool replaced = s.replaced != 0;
    s.replaced = 0;
    // A picture held long on the display is a miss of its own only at the lock (2 / 3): at 60 the late tick or the replaced
    // picture behind it is the miss already; and at the lock a tick over its fields holds the picture before it long too --
    // that display miss (it arrives a picture or two later) is the same event, not a second one
    bool held_long = false;
    if (planned >= 2) {
        if (over || drain) s.expected_display = std::min<uint32_t>(s.expected_display + 1, 2);
        while (new_display > 0 && s.expected_display > 0) { --new_display; --s.expected_display; }
        held_long = new_display > 0;
    } else {
        s.expected_display = 0;
    }
    const bool miss = over || replaced || held_long || drain;   // (a drain: the pipeline was a picture behind)
    if (over) g_miss_over.fetch_add(1, std::memory_order_relaxed);
    if (replaced) g_miss_replaced.fetch_add(1, std::memory_order_relaxed);
    if (held_long) g_miss_display.fetch_add(1, std::memory_order_relaxed);
    if (miss) g_misses.fetch_add(1, std::memory_order_relaxed);
    // the A/B arms (by presents): auto, then off (60), ...
    if (s.mode == Mode::AB) {
        const int arm = int((g_presents.load(std::memory_order_relaxed) / s.ab_every) % 2);
        if (arm != s.ab_arm) {
            s.ab_arm = arm;
            g_arm.store(arm, std::memory_order_relaxed);
            std::fprintf(stderr, "[te.vita.lock] A/B: presents from %llu -- the tick lock %s\n",
                         (unsigned long long)g_presents.load(std::memory_order_relaxed) + 1,
                         arm == 0 ? "auto" : s.hz == 50 ? "off (50)" : "off (60)");
            if (arm == 1) Switch(1, boundary, 0, "the A/B's off arm");
        }
    }
    const bool adaptive = s.mode == Mode::Auto || (s.mode == Mode::AB && s.ab_arm == 0);
    if (!adaptive) {
        if (s.mode == Mode::Fixed && s.n != s.fixed) Switch(s.fixed, boundary, 0, "fixed");
    } else if (planned == s.n) {
        // a tick at the current lock: the windows
        ++s.ticks_at_n;
        const bool counted = miss && s.warmup == 0;   // a fresh window's first ticks: the load's tail, not the scene
        if (s.warmup) --s.warmup;
        // the tick's weight (the score's input): a slow tick / a drain / a picture held long 2, a picture replaced 1
        const uint8_t weight = !counted ? 0 : (over || drain || held_long) ? 2 : replaced ? 1 : 0;
        s.miss_w[(s.ticks_at_n - 1) % 32] = weight;
        s.ring_work[s.ring_at % 32] = float(work_ms >= 0 ? work_ms : 1e6);
        s.ring_draw[s.ring_at % 32] = float(s.last_draw_ms);
        ++s.ring_at;
        // THE SCORE over the window's ticks: a run of missing ticks scores its weights, but its first 100 ms of ticks (one
        // stall -- 6 ticks at 60, 3 at 30, 2 at 20; 5 at 50, 2 at 25) together at most hiccup_cap
        const uint32_t span = std::min(s.ticks_at_n, s.window);
        const uint32_t hiccup_ticks = std::max<uint32_t>(1, (s.hz / 10u) / s.n);
        uint32_t misses = 0, run_len = 0, run_head = 0, capped = 0;
        auto close_run = [&] {
            if (!run_len) return;
            if (run_head > s.hiccup_cap) { capped += run_head - s.hiccup_cap; run_head = s.hiccup_cap; }
            misses += run_head;
            run_len = run_head = 0;
        };
        // (walked oldest to newest, so a run's FIRST ticks are its hiccup part)
        for (uint32_t k = span; k-- > 0;) {
            const uint8_t w = s.miss_w[(s.ticks_at_n - 1 - k) % 32];
            if (!w) { close_run(); continue; }
            if (run_len < hiccup_ticks) run_head += w;
            else misses += w;
            ++run_len;
        }
        close_run();
        uint32_t window_misses = 0;   // (the ticks with a miss in the window, for the reason line)
        for (uint32_t k = 0; k < span; ++k) window_misses += s.miss_w[(s.ticks_at_n - 1 - k) % 32] ? 1 : 0;
        if (s.rose_at && s.fields - s.rose_at >= 10ull * s.hz) {   // held 10 s at the rate it rose to: the rise was right
            s.calm_factor = 1;
            s.rose_at = 0;
            g_calm_factor.store(1, std::memory_order_relaxed);
        }
        const bool dwelt = !s.switched_at || s.fields - s.switched_at >= s.dwell_fields;
        // at 60: the game work's 90th percentile over the window (the p90 rule)
        double p90_work = 0;
        if (s.n == 1 && span >= kMinHistory) {
            float w[32];
            const uint32_t m = std::min<uint32_t>(span, std::min<uint32_t>(s.ring_at, 32));
            for (uint32_t k = 0; k < m; ++k) w[k] = s.ring_work[(s.ring_at - 1 - k) % 32];
            std::sort(w, w + m);
            p90_work = w[std::min<uint32_t>(m - 1, (m * 9) / 10)];
        }
        const bool p90_drop = s.n == 1 && p90_work > s.down60_p90_ms && misses >= s.down60_p90_score;
        if (s.n < s.n_max && dwelt && s.ticks_at_n >= kMinHistory && (misses >= s.down[s.n] || p90_drop)) {
            char why[260];
            std::snprintf(why, sizeof why, "the last tick: work %.1f ms, fired %u of %u fields%s%s; %u of the window's ticks missed, "
                          "%u of the score capped as hiccups%s", work_ms, fired, parked,
                          replaced ? ", a picture replaced" : "", held_long ? ", a picture held long on the display" : "",
                          window_misses, capped, p90_drop && misses < s.down[s.n] ? "; the game work's p90 over the window over the limit" : "");
            Switch(s.n + 1, boundary, misses, why);
        } else if (s.n > 1) {
            // calm: over the trailing second at this lock (30 ticks at 30 Hz, 20 at 20 Hz; 25 at 25 Hz) no miss, the 90th
            // percentile of the game work and of the drawing thread's work under the rate above's threshold and no tick far
            // over it
            const uint32_t second = std::min<uint32_t>(32u, s.hz / s.n);
            bool calm = false;
            if (s.ticks_at_n >= second && window_misses == 0) {
                float w[32], d[32];
                for (uint32_t k = 0; k < second; ++k) {
                    w[k] = s.ring_work[(s.ring_at - 1 - k) % 32];
                    d[k] = s.ring_draw[(s.ring_at - 1 - k) % 32];
                }
                std::sort(w, w + second);
                std::sort(d, d + second);
                const uint32_t p90 = (second * 9) / 10;
                const double up = s.up_ms[s.n - 1], up_max = s.up_max_ms[s.n - 1];
                calm = w[p90] < up && d[p90] < up && w[second - 1] < up_max && d[second - 1] < up_max;
            }
            s.calm = calm ? s.calm + 1 : 0;
            // a calm second, times the backoff (after a rise that had to be taken back: 2, 4, 8, 16 seconds), and the dwell
            const uint32_t need = 1 + second * (s.calm_factor - 1);
            if (s.calm >= need && dwelt) {
                char why[240];
                std::snprintf(why, sizeof why, "a calm %u s: game and drawing thread work p90 under %.1f ms, none over %.1f ms; "
                              "%.1f s since the last switch", s.calm_factor, s.up_ms[s.n - 1], s.up_max_ms[s.n - 1],
                              s.switched_at ? double(s.fields - s.switched_at) / double(s.hz) : 0.0);
                Switch(s.n - 1, boundary, misses, why);
            }
        }
    }
    // (the vblank family only: at 50 Hz the fields are the timer's at every rate)
    g_display_edge.store(s.active && s.n >= 2 && s.hz == 60, std::memory_order_relaxed);
}

bool WantDisplayEdge() { return g_display_edge.load(std::memory_order_relaxed); }

void NoteGameTime(uint64_t boundary, uint64_t t0_counts) {
    // (the game thread: the one writer) odd while the three words change
    const uint32_t s = g_gt_seq.load(std::memory_order_relaxed);
    g_gt_seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_gt_t0.store(t0_counts, std::memory_order_relaxed);
    g_gt_ns.store(uint64_t(NowNs()), std::memory_order_relaxed);
    g_gt_boundary.store(boundary, std::memory_order_relaxed);
    g_gt_seq.store(s + 2, std::memory_order_release);
}

void DiagGameDelay(uint64_t boundary) {
#if defined(__vita__)
    static const long us = EnvLong("TE_VITA_DIAG_GAME_DELAY_US", 0);
    if (us <= 0) return;
    static const long from = EnvLong("TE_VITA_DIAG_GAME_DELAY_FROM", 0), to = EnvLong("TE_VITA_DIAG_GAME_DELAY_TO", 0);
    if (te::GetSettings().deterministic) return;
    const long b = long(boundary) + 1;   // the boundary this tick ends
    if ((from > 0 && b < from) || (to > 0 && b > to)) return;
    sceKernelDelayThread(SceUInt(us));
#else
    (void)boundary;
#endif
}

void WaitDisplayEdge() {
#if defined(__vita__)
    sceDisplayWaitVblankStart();
#endif
}

void BeforeSwap(uint32_t fields) {
#if defined(__vita__)
    if (te::GetSettings().deterministic) return;
    if (!g_callback_installed.load(std::memory_order_relaxed)) {
        g_callback_installed.store(true, std::memory_order_relaxed);
        if (&vglSetDisplayCallback && &gxm_back_buffer_index && &vsync_interval) {
            vglSetDisplayCallback(&OnDisplay);
            std::fprintf(stderr, "[te.vita.lock] display callback installed: each picture held for its lock's refreshes "
                                 "(vitaGL's swap interval per picture); the flip census on\n");
        } else {
            std::fprintf(stderr, "[te.vita.lock] vitaGL has no display callback / swap interval symbols: the lock keeps "
                                 "interval 1\n");
        }
    }
    if (&gxm_back_buffer_index) {
        // the tag: the lock's fields, and the timer family's bit (a 50 / 25 Hz picture: interval 1, shown when it is ready)
        const uint32_t f = std::min<uint32_t>(fields, 3);
        const uint32_t tag = f | (f && g_timer_family.load(std::memory_order_relaxed) ? kTagTimer : 0u);
        g_buffer_tag[gxm_back_buffer_index & 7u].store(tag, std::memory_order_release);
    }
#else
    (void)fields;
#endif
}

void NotePictureDrawn(double work_ms, double present_ms) {
    g_draw.Note(work_ms);
    g_present.Note(present_ms);
    const uint32_t us = uint32_t(std::min(std::max(work_ms, 0.0) * 1000.0, 4.0e9));
    if (us > g_draw_max_us.load(std::memory_order_relaxed)) g_draw_max_us.store(us, std::memory_order_relaxed);
}

void NotePresents(uint64_t presents) { g_presents.store(presents, std::memory_order_relaxed); }

std::string WindowLine(uint64_t from, uint64_t to, double seconds) {
    if (!S.read || !S.vita_play) return std::string();   // (the game thread's first picture read the settings long before)
    Totals t;
    t.boundaries = g_boundaries.load(); t.other = g_other.load(); t.other_fields = g_other_fields.load(); t.held = g_held.load(); t.drains = g_drains.load();
    for (int i = 0; i < 4; ++i) { t.ticks[i] = g_ticks[i].load(); t.tick_fields[i] = g_tick_fields[i].load(); t.sw[i] = g_switch[i].load(); }
    t.over = g_miss_over.load(); t.replaced = g_miss_replaced.load(); t.display = g_miss_display.load(); t.misses = g_misses.load();
    t.flips = g_flips.load();
    for (int a = 0; a < kTagClasses; ++a) for (int b = 0; b < 5; ++b) t.flips_by[a][b] = g_flips_by[a][b].load();
    const Totals& l = g_last;
    const double s = seconds > 0 ? seconds : 1.0;
    uint64_t ticks = 0, tick_fields = 0;
    for (int i = 1; i <= 3; ++i) { ticks += t.ticks[i] - l.ticks[i]; tick_fields += t.tick_fields[i] - l.tick_fields[i]; }
    auto held = [&](int tag) {
        char b[96];
        uint64_t n[5];
        uint64_t sum = 0;
        for (int k = 1; k <= 4; ++k) { n[k] = t.flips_by[tag][k] - l.flips_by[tag][k]; sum += n[k]; }
        if (!sum) return std::string();
        static const char* const kNames[kTagClasses] = {"other", "60 Hz", "30 Hz", "20 Hz", "?", "50 Hz", "25 Hz", "16.7 Hz"};
        std::snprintf(b, sizeof b, " %s pictures %llu/%llu/%llu/%llu;", kNames[tag],
                      (unsigned long long)n[1], (unsigned long long)n[2], (unsigned long long)n[3], (unsigned long long)n[4]);
        return std::string(b);
    };
    const bool timer = S.hz == 50;
    std::string flips_held = timer ? held(5) + held(6) + held(1) + held(2) + held(0) : held(1) + held(2) + held(3) + held(0);
    const int arm = g_arm.load();
    const char* mode = S.mode == Mode::AB ? (arm == 1 ? (timer ? "ab: off (50)" : "ab: off (60)") : "ab: auto") : S.mode == Mode::Auto ? "auto"
                       : RateName(S.fixed);
    char buf[1400];
    if (timer) {
        // the 50 Hz family: 50 / 25 Hz picture ticks on the pacer's timer; the display shows each picture from the first
        // refresh after it is ready (a 50 Hz picture held 1 refresh 4 times in 5 and 2 once -- 1/2 counts 240/60 in 300)
        std::snprintf(buf, sizeof buf,
                      "[te.vita.lock] presents %llu-%llu (%.1f s): tick lock %s, now %s Hz (the 50 Hz game rate); picture ticks %.1f a "
                      "second (50/25 Hz: %llu/%llu; outside the lock %llu), fields per tick %.2f; other boundaries %llu (%llu fields); "
                      "display flips %.1f a second, held 1/2/3/4+ refreshes:%s longest flip gap %.1f ms; misses %llu (ticks over their "
                      "lock %llu, pictures replaced %llu, held long on the display %llu, drains %llu); switches 50->25 %llu, 25->50 %llu "
                      "(calm asked %ux); tick work ms p50/p90/max %s; drawing thread per picture ms %s (+ present %s); pacer on the "
                      "timer (50 Hz)",
                      (unsigned long long)from, (unsigned long long)to, seconds, mode, RateName(g_n.load()), double(ticks) / s,
                      (unsigned long long)(t.ticks[1] - l.ticks[1]), (unsigned long long)(t.ticks[2] - l.ticks[2]),
                      (unsigned long long)(t.held - l.held), ticks ? double(tick_fields) / double(ticks) : 0.0,
                      (unsigned long long)(t.other - l.other), (unsigned long long)(t.other_fields - l.other_fields),
                      double(t.flips - l.flips) / s, flips_held.empty() ? " none;" : flips_held.c_str(),
                      g_longest_gap_ns.exchange(0) / 1e6, (unsigned long long)(t.misses - l.misses), (unsigned long long)(t.over - l.over),
                      (unsigned long long)(t.replaced - l.replaced), (unsigned long long)(t.display - l.display), (unsigned long long)(t.drains - l.drains),
                      (unsigned long long)(t.sw[0] - l.sw[0]), (unsigned long long)(t.sw[3] - l.sw[3]), g_calm_factor.load(),
                      g_work.TakeLine().c_str(), g_draw.TakeLine().c_str(), g_present.TakeLine().c_str());
        g_last = t;
        return std::string(buf) + GameTimeClause(S.hz) + "\n";
    }
    std::snprintf(buf, sizeof buf,
                  "[te.vita.lock] presents %llu-%llu (%.1f s): tick lock %s, now %s Hz; picture ticks %.1f a second (60/30/20 Hz: "
                  "%llu/%llu/%llu; outside the lock %llu), fields per tick %.2f; other boundaries %llu (%llu fields); display flips %.1f a second, held "
                  "1/2/3/4+ refreshes:%s longest flip gap %.1f ms; misses %llu (ticks over their lock %llu, pictures replaced %llu, "
                  "held long on the display %llu, drains %llu); switches 60->30 %llu, 30->20 %llu, 20->30 %llu, 30->60 %llu (calm asked %ux); "
                  "tick work ms p50/p90/max %s; drawing thread per picture ms %s (+ present %s); pacer on the %s",
                  (unsigned long long)from, (unsigned long long)to, seconds, mode, RateName(g_n.load()), double(ticks) / s,
                  (unsigned long long)(t.ticks[1] - l.ticks[1]), (unsigned long long)(t.ticks[2] - l.ticks[2]),
                  (unsigned long long)(t.ticks[3] - l.ticks[3]), (unsigned long long)(t.held - l.held),
                  ticks ? double(tick_fields) / double(ticks) : 0.0,
                  (unsigned long long)(t.other - l.other), (unsigned long long)(t.other_fields - l.other_fields),
                  double(t.flips - l.flips) / s, flips_held.empty() ? " none;" : flips_held.c_str(),
                  g_longest_gap_ns.exchange(0) / 1e6, (unsigned long long)(t.misses - l.misses), (unsigned long long)(t.over - l.over),
                  (unsigned long long)(t.replaced - l.replaced), (unsigned long long)(t.display - l.display), (unsigned long long)(t.drains - l.drains),
                  (unsigned long long)(t.sw[0] - l.sw[0]), (unsigned long long)(t.sw[1] - l.sw[1]), (unsigned long long)(t.sw[2] - l.sw[2]),
                  (unsigned long long)(t.sw[3] - l.sw[3]), g_calm_factor.load(), g_work.TakeLine().c_str(), g_draw.TakeLine().c_str(),
                  g_present.TakeLine().c_str(), g_display_edge.load() ? "display's vblank" : "timer");
    g_last = t;
    return std::string(buf) + GameTimeClause(S.hz) + "\n";
}

std::string ExitLine() {
    ReadOnce();
    char buf[600];
    uint64_t ticks = 0, fields = 0;
    for (int i = 1; i <= 3; ++i) { ticks += g_ticks[i].load(); fields += g_tick_fields[i].load(); }
    const bool timer = S.hz == 50;
    const char* what = !S.vita_play ? "not on this platform / profile"
                       : S.active ? (S.mode == Mode::Auto ? (timer ? "auto (50/25)" : "auto") : S.mode == Mode::AB ? "ab" : RateName(S.fixed))
                                  : (timer ? "off (50)" : "off (60)");
    if (timer && S.vita_play) {
        std::snprintf(buf, sizeof buf,
                      "tick lock: %s at the 50 Hz game rate; picture ticks %llu (50/25 Hz: %llu/%llu, %.2f fields a tick; outside the lock %llu), "
                      "other boundaries %llu; misses %llu (over the lock %llu, replaced %llu, held long %llu, drains %llu); switches 50->25 %llu, "
                      "25->50 %llu; display flips %llu",
                      what, (unsigned long long)ticks, (unsigned long long)g_ticks[1].load(), (unsigned long long)g_ticks[2].load(),
                      ticks ? double(fields) / double(ticks) : 0.0, (unsigned long long)g_held.load(), (unsigned long long)g_other.load(),
                      (unsigned long long)g_misses.load(), (unsigned long long)g_miss_over.load(), (unsigned long long)g_miss_replaced.load(),
                      (unsigned long long)g_miss_display.load(), (unsigned long long)g_drains.load(), (unsigned long long)g_switch[0].load(),
                      (unsigned long long)g_switch[3].load(), (unsigned long long)g_flips.load());
        return buf;
    }
    std::snprintf(buf, sizeof buf,
                  "tick lock: %s; picture ticks %llu (60/30/20 Hz: %llu/%llu/%llu, %.2f fields a tick; outside the lock %llu), other boundaries %llu; "
                  "misses %llu (over the lock %llu, replaced %llu, held long %llu, drains %llu); switches 60->30 %llu, 30->20 %llu, 20->30 %llu, "
                  "30->60 %llu; display flips %llu",
                  what,
                  (unsigned long long)ticks, (unsigned long long)g_ticks[1].load(), (unsigned long long)g_ticks[2].load(),
                  (unsigned long long)g_ticks[3].load(), ticks ? double(fields) / double(ticks) : 0.0, (unsigned long long)g_held.load(),
                  (unsigned long long)g_other.load(),
                  (unsigned long long)g_misses.load(), (unsigned long long)g_miss_over.load(), (unsigned long long)g_miss_replaced.load(),
                  (unsigned long long)g_miss_display.load(), (unsigned long long)g_drains.load(), (unsigned long long)g_switch[0].load(), (unsigned long long)g_switch[1].load(),
                  (unsigned long long)g_switch[2].load(), (unsigned long long)g_switch[3].load(), (unsigned long long)g_flips.load());
    return buf;
}

}  // namespace te::tick_lock
