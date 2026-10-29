// load_census.cpp -- see te_load_census.h.
#include "te_load_census.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace te::load {
namespace {

// The game thread: the first thread that ends a tick (before the first boundary every note is taken: the boot's loads run
// on the game thread before its first frame)
std::atomic<bool> g_game_known{false};
std::thread::id g_game_id;

// This tick's notes (the game thread's own)
struct TickNotes {
    double ms[kWorkKinds] = {};
    uint32_t n[kWorkKinds] = {};
    uint64_t bytes[kWorkKinds] = {};
    bool any = false;
};
TickNotes g_tick;

GpuAllocs (*g_alloc_source)() = nullptr;
GpuAllocs g_allocs_at_tick;   // the totals at the last tick's end (the game thread's)

double LongMs() {
    static const double ms = [] {
        const char* v = std::getenv("TE_LOAD_LONG_MS");
        const double x = v ? std::atof(v) : 25.0;
        return x > 0 ? x : 25.0;
    }();
    return ms;
}

// The window (written at the ticks' ends by the game thread and by the drawing thread's notes; read and started over by
// the window line on either): relaxed atomics -- a census, a value off by a tick at a window's edge is harmless
struct Window {
    std::atomic<uint64_t> ticks{0}, tick_us{0}, long_ticks{0}, long_us{0}, long_max_us{0}, long_max_boundary{0};
    std::atomic<uint64_t> work_us[kWorkKinds] = {}, work_n[kWorkKinds] = {}, work_bytes[kWorkKinds] = {};
    std::atomic<uint64_t> lwork_us[kWorkKinds] = {}, lwork_n[kWorkKinds] = {};
    std::atomic<uint64_t> long_allocs{0}, long_alloc_bytes{0}, long_frees{0}, allocs{0}, alloc_bytes{0}, frees{0};
    std::atomic<uint64_t> rt_tex_n{0}, rt_tex_us{0}, rt_batches{0}, rt_meshes{0}, rt_mesh_us{0}, rt_job_max_us{0};
    std::atomic<uint64_t> dec_n{0}, dec_us{0}, dec_bytes{0};   // decoded off the game thread
};
Window g_win;
// the window's longest tick's own line (its work), guarded by a flag: the game thread writes it, the window line reads it
std::atomic_flag g_longest_lock = ATOMIC_FLAG_INIT;
char g_longest[512] = {};

uint64_t Us(double ms) { return ms > 0 ? uint64_t(ms * 1000.0 + 0.5) : 0; }
void AtomicMax(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
uint64_t Take(std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); }

// The tick's work as text (the slow-tick line's suffix and the window's longest tick)
std::string Describe(const TickNotes& t, const GpuAllocs* allocs) {
    std::string s;
    char b[256];
    auto add = [&](const char* text) { if (!s.empty()) s += "; "; s += text; };
    if (t.n[kTexDecode] || t.n[kTexMake]) {
        std::snprintf(b, sizeof b, "textures %u decoded (%.0f KB, %.1f ms), made %u (waited %.1f ms)", t.n[kTexDecode],
                      double(t.bytes[kTexDecode]) / 1024.0, t.ms[kTexDecode], t.n[kTexMake], t.ms[kTexMake]);
        add(b);
    }
    if (t.n[kTexRelease]) {
        std::snprintf(b, sizeof b, "texture releases %u (waited %.1f ms)", t.n[kTexRelease], t.ms[kTexRelease]);
        add(b);
    }
    if (t.n[kModelDecode] || t.ms[kModelWait] > 0) {
        std::snprintf(b, sizeof b, "model parts %u (%.1f ms; waits %.1f ms)", t.n[kModelDecode], t.ms[kModelDecode], t.ms[kModelWait]);
        add(b);
    }
    if (t.n[kMeshAsk] || t.n[kMeshPost] || t.ms[kMeshWait] > 0) {
        std::snprintf(b, sizeof b, "meshes asked %u, posted %u (%llu vertices, %.1f ms), waited %.1f ms", t.n[kMeshAsk], t.n[kMeshPost],
                      (unsigned long long)t.bytes[kMeshPost], t.ms[kMeshPost], t.ms[kMeshWait]);
        add(b);
    }
    if (t.n[kModelRelease]) {
        std::snprintf(b, sizeof b, "model releases %u (%.1f ms)", t.n[kModelRelease], t.ms[kModelRelease]);
        add(b);
    }
    if (t.n[kDiscWait]) {
        std::snprintf(b, sizeof b, "disc waits %u (%.1f ms)", t.n[kDiscWait], t.ms[kDiscWait]);
        add(b);
    }
    if (allocs && (allocs->allocs || allocs->frees)) {
        std::snprintf(b, sizeof b, "GPU memory allocations %llu (%.0f KB, %.1f ms), frees %llu", (unsigned long long)allocs->allocs,
                      double(allocs->bytes) / 1024.0, double(allocs->us) / 1000.0, (unsigned long long)allocs->frees);
        add(b);
    }
    return s;
}

}  // namespace

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool OnGameThread() {
    return !g_game_known.load(std::memory_order_acquire) || std::this_thread::get_id() == g_game_id;
}

void Note(Work w, double ms, uint32_t count, uint64_t bytes) {
    if (w < 0 || w >= kWorkKinds || !OnGameThread()) return;
    g_tick.ms[w] += ms;
    g_tick.n[w] += count;
    g_tick.bytes[w] += bytes;
    g_tick.any = true;
}

Timed::Timed(Work w, uint32_t c, uint64_t b) : count(c), bytes(b), w_(w), t0_(NowNs()) {}
Timed::~Timed() { Note(w_, double(NowNs() - t0_) / 1e6, count, bytes); }

void NoteRenderTexture(double ms) {
    g_win.rt_tex_n.fetch_add(1, std::memory_order_relaxed);
    g_win.rt_tex_us.fetch_add(Us(ms), std::memory_order_relaxed);
}
void NoteRenderMeshes(uint32_t meshes, double ms) {
    g_win.rt_batches.fetch_add(1, std::memory_order_relaxed);
    g_win.rt_meshes.fetch_add(meshes, std::memory_order_relaxed);
    g_win.rt_mesh_us.fetch_add(Us(ms), std::memory_order_relaxed);
}
void NoteRenderJob(double ms) { AtomicMax(g_win.rt_job_max_us, Us(ms)); }
void NoteDecodeElsewhere(double ms, uint64_t bytes) {
    g_win.dec_n.fetch_add(1, std::memory_order_relaxed);
    g_win.dec_us.fetch_add(Us(ms), std::memory_order_relaxed);
    g_win.dec_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void SetGpuAllocSource(GpuAllocs (*source)()) { g_alloc_source = source; }

void EndTick(uint64_t boundary, double tick_ms, std::string* line) {
    if (!g_game_known.load(std::memory_order_relaxed)) {
        g_game_id = std::this_thread::get_id();
        g_game_known.store(true, std::memory_order_release);
    }
    GpuAllocs delta;
    if (g_alloc_source) {
        const GpuAllocs now = g_alloc_source();
        delta.allocs = now.allocs - g_allocs_at_tick.allocs;
        delta.bytes = now.bytes - g_allocs_at_tick.bytes;
        delta.frees = now.frees - g_allocs_at_tick.frees;
        delta.us = now.us - g_allocs_at_tick.us;
        g_allocs_at_tick = now;
    }
    const bool is_long = tick_ms > LongMs();
    g_win.ticks.fetch_add(1, std::memory_order_relaxed);
    g_win.tick_us.fetch_add(Us(tick_ms), std::memory_order_relaxed);
    g_win.allocs.fetch_add(delta.allocs, std::memory_order_relaxed);
    g_win.alloc_bytes.fetch_add(delta.bytes, std::memory_order_relaxed);
    g_win.frees.fetch_add(delta.frees, std::memory_order_relaxed);
    for (int k = 0; k < kWorkKinds; ++k) {
        if (!g_tick.n[k] && g_tick.ms[k] <= 0) continue;
        g_win.work_us[k].fetch_add(Us(g_tick.ms[k]), std::memory_order_relaxed);
        g_win.work_n[k].fetch_add(g_tick.n[k], std::memory_order_relaxed);
        g_win.work_bytes[k].fetch_add(g_tick.bytes[k], std::memory_order_relaxed);
        if (is_long) {
            g_win.lwork_us[k].fetch_add(Us(g_tick.ms[k]), std::memory_order_relaxed);
            g_win.lwork_n[k].fetch_add(g_tick.n[k], std::memory_order_relaxed);
        }
    }
    if (is_long) {
        g_win.long_ticks.fetch_add(1, std::memory_order_relaxed);
        g_win.long_us.fetch_add(Us(tick_ms), std::memory_order_relaxed);
        g_win.long_allocs.fetch_add(delta.allocs, std::memory_order_relaxed);
        g_win.long_alloc_bytes.fetch_add(delta.bytes, std::memory_order_relaxed);
        g_win.long_frees.fetch_add(delta.frees, std::memory_order_relaxed);
        if (Us(tick_ms) > g_win.long_max_us.load(std::memory_order_relaxed)) {
            g_win.long_max_us.store(Us(tick_ms), std::memory_order_relaxed);
            g_win.long_max_boundary.store(boundary, std::memory_order_relaxed);
            const std::string what = Describe(g_tick, &delta);
            while (g_longest_lock.test_and_set(std::memory_order_acquire)) {}
            std::snprintf(g_longest, sizeof g_longest, "%s", what.empty() ? "no load work" : what.c_str());
            g_longest_lock.clear(std::memory_order_release);
        }
    }
    if (line && (g_tick.any || delta.allocs || delta.frees)) {
        const std::string what = Describe(g_tick, &delta);
        if (!what.empty()) *line = " load: " + what;
    }
    g_tick = TickNotes{};
}

std::string WindowLine(uint64_t from, uint64_t to) {
    Window& w = g_win;
    const uint64_t ticks = Take(w.ticks), tick_us = Take(w.tick_us), long_ticks = Take(w.long_ticks), long_us = Take(w.long_us);
    const uint64_t long_max_us = Take(w.long_max_us), long_max_b = Take(w.long_max_boundary);
    uint64_t us[kWorkKinds], n[kWorkKinds], bytes[kWorkKinds], lus[kWorkKinds], ln[kWorkKinds];
    for (int k = 0; k < kWorkKinds; ++k) {
        us[k] = Take(w.work_us[k]);
        n[k] = Take(w.work_n[k]);
        bytes[k] = Take(w.work_bytes[k]);
        lus[k] = Take(w.lwork_us[k]);
        ln[k] = Take(w.lwork_n[k]);
    }
    const uint64_t la = Take(w.long_allocs), lab = Take(w.long_alloc_bytes), lf = Take(w.long_frees);
    const uint64_t a = Take(w.allocs), ab = Take(w.alloc_bytes), f = Take(w.frees);
    const uint64_t rtn = Take(w.rt_tex_n), rtus = Take(w.rt_tex_us), rtb = Take(w.rt_batches), rtm = Take(w.rt_meshes),
                   rtmus = Take(w.rt_mesh_us), rtmax = Take(w.rt_job_max_us);
    const uint64_t decn = Take(w.dec_n), decus = Take(w.dec_us), decb = Take(w.dec_bytes);
    char longest[512];
    while (g_longest_lock.test_and_set(std::memory_order_acquire)) {}
    std::snprintf(longest, sizeof longest, "%s", g_longest);
    g_longest[0] = 0;
    g_longest_lock.clear(std::memory_order_release);
    if (!ticks) return std::string();
    auto ms = [](uint64_t u) { return double(u) / 1000.0; };
    char b[2400];
    int at = std::snprintf(b, sizeof b, "[te.load] presents %llu-%llu: %llu ticks (%.1f ms of work); over %.0f ms %llu (%.0f ms)",
                           (unsigned long long)from, (unsigned long long)to, (unsigned long long)ticks, ms(tick_us), LongMs(),
                           (unsigned long long)long_ticks, ms(long_us));
    if (long_ticks && at > 0 && at < int(sizeof b))
        at += std::snprintf(b + at, sizeof b - size_t(at), ", the longest %.1f ms at boundary %llu (%s)", ms(long_max_us),
                            (unsigned long long)long_max_b, longest[0] ? longest : "no load work");
    if (at > 0 && at < int(sizeof b))
        at += std::snprintf(b + at, sizeof b - size_t(at),
                            "; texture decode: %llu textures, %.0f KB, %.1f ms this window on the game thread + %llu, %.0f KB, %.1f ms off it "
                            "(the game thread's time to have them made %.1f ms over %llu; %llu releases waited %.1f ms); model parts %llu "
                            "decoded in %.1f ms (waits %.1f ms); native meshes asked %llu, posted %llu (%llu vertices) in %.1f ms, waited %.1f "
                            "ms; disc waits %llu (%.1f ms); model releases %llu (%.1f ms)",
                            (unsigned long long)n[kTexDecode], double(bytes[kTexDecode]) / 1024.0, ms(us[kTexDecode]), (unsigned long long)decn,
                            double(decb) / 1024.0, ms(decus), ms(us[kTexMake]),
                            (unsigned long long)n[kTexMake], (unsigned long long)n[kTexRelease], ms(us[kTexRelease]),
                            (unsigned long long)n[kModelDecode], ms(us[kModelDecode]), ms(us[kModelWait]), (unsigned long long)n[kMeshAsk],
                            (unsigned long long)n[kMeshPost], (unsigned long long)bytes[kMeshPost], ms(us[kMeshPost]), ms(us[kMeshWait]),
                            (unsigned long long)n[kDiscWait], ms(us[kDiscWait]), (unsigned long long)n[kModelRelease], ms(us[kModelRelease]));
    if (long_ticks && at > 0 && at < int(sizeof b))
        at += std::snprintf(b + at, sizeof b - size_t(at),
                            "; in the long ticks: textures %llu (%.1f ms decoding, %.1f ms waiting), model parts %llu (%.1f ms, waits %.1f ms), "
                            "meshes posted %llu (%.1f ms, waited %.1f ms), disc waits %.1f ms, GPU memory allocations %llu (%.0f KB) frees %llu",
                            (unsigned long long)ln[kTexDecode], ms(lus[kTexDecode]), ms(lus[kTexMake]), (unsigned long long)ln[kModelDecode],
                            ms(lus[kModelDecode]), ms(lus[kModelWait]), (unsigned long long)ln[kMeshPost], ms(lus[kMeshPost]),
                            ms(lus[kMeshWait]), ms(lus[kDiscWait]), (unsigned long long)la, double(lab) / 1024.0, (unsigned long long)lf);
    if (at > 0 && at < int(sizeof b))
        at += std::snprintf(b + at, sizeof b - size_t(at),
                            "; GPU memory allocations %llu (%.0f KB) frees %llu; the drawing thread made %llu textures in %.1f ms and %llu mesh "
                            "batches (%llu meshes) in %.1f ms, its longest job %.1f ms\n",
                            (unsigned long long)a, double(ab) / 1024.0, (unsigned long long)f, (unsigned long long)rtn, ms(rtus),
                            (unsigned long long)rtb, (unsigned long long)rtm, ms(rtmus), ms(rtmax));
    if (at >= int(sizeof b)) b[sizeof b - 2] = '\n', b[sizeof b - 1] = 0;
    return b;
}

}  // namespace te::load
