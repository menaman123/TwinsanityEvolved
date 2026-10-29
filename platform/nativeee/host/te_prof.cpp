// platform/nativeee (host side) -- te_prof.h: the sampling section profiler (TE_PROF=1) and the start-up primitive
// timings (TE_PROF_BENCH=1).
#include "te_prof.h"

#include <algorithm>
#include <atomic>
#include <cfenv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

#include "disc_reader.h"
#include "engine/platform/host/platform.h"
#include "te_mesh_builder.h"
#include "vita_gpu_census.h"

extern "C" {
volatile unsigned short te_prof_section[te::prof::kSlots];
#if defined(__vita__)
// The seam thunks' names (seam_host_thunks.c: always on the Vita lane; the thunks set the ids only when compiled with
// TE_PROF_SEAM, cmake -DTE_PROF_SEAM=ON)
extern const char* const te_prof_seam_names[];
extern const unsigned te_prof_seam_count;
// math_vu0.cpp: the collision maths' unclamped fast paths taken / left to the engine (te_unclamped.h)
void te_unclamped_counts(uint32_t* fast, uint32_t* clamped);
#endif
}

namespace te::prof {
namespace {

constexpr unsigned kHostBase = 1024, kHostMax = 256, kIds = kHostBase + kHostMax;
const char* g_names[kHostMax];
std::atomic<unsigned> g_registered{0};
std::mutex g_register_lock;
thread_local int t_slot = -1;
bool g_active = false;
long g_every = 300;
uint64_t g_window_start = 0;
std::atomic<uint32_t> g_counts[kSlots][kIds];
// the game thread's samples stamped with their tick (TickBegin / TickSections): a ring of the last kTickRing
std::atomic<uint32_t> g_tick{0};
constexpr unsigned kTickRing = 8192;
struct TickSample {
    std::atomic<uint32_t> tick{0};
    std::atomic<uint16_t> id{0};
};
TickSample g_tick_ring[kTickRing];
std::atomic<uint32_t> g_tick_at{0};
// The sampler's period (TE_PROF_US, microseconds). The device: a wake-up every 0.5 ms cost the te-prof thread 2.2 % of a
// core (device-run6 [te.cpu] te-prof 2.89 ms per present at 7.6 fps, ~11 us a wake-up, each one preempting a busy thread at
// priority 72) -- 2 ms there is a quarter of that and still ~2500 samples a thread in a 300-present window at 60 fps.
#if defined(__vita__)
long g_period_us = 2000;
#else
long g_period_us = 500;
#endif
// The slots a thread bound (BindThread): only those are sampled and reported (the Vita's unused builder slots and an idle
// io / movie slot used to print "(own code) 100%" lines and cost the sampler a counter each per sample)
std::atomic<uint32_t> g_bound{0};
// The report's rows per thread (TE_PROF_ROWS, default 16) and the share below which it stops after the first four
// (TE_PROF_MIN, percent, default 0.5)
unsigned g_rows = 16;
double g_min_pct = 0.5;
const char* const kSlotNames[kSlots] = {"game", "render", "build0", "build1", "build2", "build3", "io", "movie", "audio"};

int64_t NowUs() {
#if defined(__vita__)
    return int64_t(sceKernelGetProcessTimeWide());
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

const char* NameOf(unsigned id) {
    if (id == 0) return "(own code)";
    if (id >= kHostBase) {
        const unsigned k = id - kHostBase;
        return k < g_registered.load(std::memory_order_acquire) && g_names[k] ? g_names[k] : "(host ?)";
    }
#if defined(__vita__)
    if (id <= te_prof_seam_count) return te_prof_seam_names[id - 1];
#endif
    return "(seam ?)";
}

void Sampler() {
    ps2r::platform::host::PinCurrentThreadInteractive("te-prof", ps2r::platform::host::ThreadRole::Pacer);
    NoteThread("te-prof");
    for (;;) {
#if defined(__vita__)
        sceKernelDelayThread(SceUInt(g_period_us));
        // the GPU's busy share without a drain (vita_gpu_census.h SampleGpuBusy: two loads of vitaGL's scene
        // fence a sample) -- the per-window GPU time the probe measured by draining the GPU
        te::vita_gpu::SampleGpuBusy();
#else
        std::this_thread::sleep_for(std::chrono::microseconds(g_period_us));
#endif
        const uint32_t bound = g_bound.load(std::memory_order_relaxed);
        for (int s = 0; s < kSlots; ++s) {
            if (!(bound & (1u << s))) continue;
            const unsigned id = te_prof_section[s];
            if (id < kIds) g_counts[s][id].fetch_add(1, std::memory_order_relaxed);
            if (s == kGame && id < kIds) {
                TickSample& t = g_tick_ring[g_tick_at.fetch_add(1, std::memory_order_relaxed) % kTickRing];
                t.tick.store(g_tick.load(std::memory_order_relaxed), std::memory_order_relaxed);
                t.id.store(uint16_t(id), std::memory_order_relaxed);
            }
        }
    }
}

// ---- TE_PROF_BENCH: what a primitive costs here (the emulator's HLE calls are the suspects) ------------------------
template <typename F>
void Time(const char* what, int n, F&& f) {
    const int64_t t0 = NowUs();
    for (int i = 0; i < n; ++i) f(i);
    const int64_t t1 = NowUs();
    std::fprintf(stderr, "[te.prof] bench: %-44s %9.1f ns/op (%d ops, %lld us)\n", what, double(t1 - t0) * 1000.0 / n, n,
                 (long long)(t1 - t0));
}

void Bench() {
    const int n = 20000;
    volatile int64_t sink = 0;
    Time("timer (sceKernelGetProcessTimeWide / steady)", n, [&](int) { sink += NowUs(); });
    Time("std::chrono::steady_clock::now", n, [&](int) { sink += std::chrono::steady_clock::now().time_since_epoch().count(); });
    Time("malloc(48) + free", n, [&](int i) { void* p = std::malloc(48 + (i & 7)); sink += reinterpret_cast<intptr_t>(p); std::free(p); });
    Time("malloc(320) + free", n, [&](int i) { void* p = std::malloc(320 + (i & 7)); sink += reinterpret_cast<intptr_t>(p); std::free(p); });
    {
        struct Big { float m[56]; };
        Big big{};
        Time("std::function(224-byte capture) + call + destroy", n, [&](int i) {
            big.m[0] = float(i);
            std::function<void()> f([big, &sink] { sink += int64_t(big.m[0]); });
            f();
        });
    }
    {
        std::mutex m;
        Time("std::mutex lock + unlock", n, [&](int) { m.lock(); m.unlock(); });
    }
#if defined(__vita__)
    {
        alignas(8) static SceKernelLwMutexWork work;
        sceKernelCreateLwMutex(&work, "te-prof-bench", 0, 0, nullptr);
        Time("sceKernelLockLwMutex + Unlock", n, [&](int) { sceKernelLockLwMutex(&work, 1, nullptr); sceKernelUnlockLwMutex(&work, 1); });
        alignas(8) static SceKernelLwCondWork cond;
        sceKernelCreateLwCond(&cond, "te-prof-bench-cond", 0, &work, nullptr);
        Time("sceKernelSignalLwCondAll (no waiter)", n, [&](int) { sceKernelSignalLwCondAll(&cond); });
        sceKernelDeleteLwCond(&cond);
        sceKernelDeleteLwMutex(&work);
        Time("sceKernelGetThreadId", n, [&](int) { sink += sceKernelGetThreadId(); });
    }
#endif
    {
        std::fenv_t env;
        std::fegetenv(&env);
        Time("fegetenv + fesetenv", n, [&](int) { std::fenv_t e; std::fegetenv(&e); std::fesetenv(&env); });
        Time("fegetround + fesetround x2", n, [&](int) { const int r = std::fegetround(); std::fesetround(FE_TOWARDZERO); std::fesetround(r); });
    }
    {
        volatile float x = 1.0f;
        Time("float multiply-add (1 op, dependent)", 1000000, [&](int) { x = x * 1.0001f + 0.5f; });
        volatile uint32_t u = 1;
        Time("integer add (1 op, dependent, volatile)", 1000000, [&](int i) { u = u + uint32_t(i); });
        volatile float y = 3.0f;
        Time("float divide", 200000, [&](int i) { y = 1.0f / (y + float(i & 3)); });
    }
    {
        // The rigid models' CPU build (title::MeshGeometry::BuildPrepared), per vertex: 50 strips of 30 on screen
        using MG = title::MeshGeometry;
        MG::Mesh mesh;
        for (int s = 0; s < 50; ++s) {
            MG::Strip strip;
            for (int v = 0; v < 30; ++v) {
                MG::Vertex vx{};
                vx.position = {float(v / 2) * 0.1f - 0.75f, float(s) * 0.03f - 0.75f + float(v & 1) * 0.02f, 5.0f + float(v) * 0.01f};
                vx.stq = {float(v) / 30.f, float(s) / 50.f, 1.f};
                vx.colour = {128, 128, 128, 128};
                vx.restart = v < 2;
                strip.vertices.push_back(vx);
            }
            mesh.strips.push_back(std::move(strip));
        }
        MG::Prepared prepared;
        MG::Prepare(mesh, prepared);
        ps2r::gs::DrawIr state{};
        state.tex_enabled = 1;
        state.prim_type = 4;
        state.off_x = 1792.f;
        state.off_y = 1792.f;
        MG::View view;
        // x, y spread over the screen around the GS middle (2048 * 16), w = z
        view.screen = {MG::Vector{3000.f, 0.f, 0.f, 0.f}, MG::Vector{0.f, 3000.f, 0.f, 0.f}, MG::Vector{32768.f, 32768.f, 100000.f, 1.f},
                       MG::Vector{0.f, 0.f, 0.f, 0.f}};
        view.clip = {MG::Vector{1.f, 0.f, 0.f, 0.f}, MG::Vector{0.f, 1.f, 0.f, 0.f}, MG::Vector{0.f, 0.f, 0.1f, 1.f}, MG::Vector{0.f, 0.f, 0.f, 0.f}};
        view.guard = {0.5f, 0.5f, 1.f, 1.f};
        size_t out = 0;
        const MG::Sink sink = [](void* user, const ps2r::gs::VertexRec*, size_t count, bool) { *static_cast<size_t*>(user) += count; };
        const int reps = 20;
        for (int pass = 0; pass < 2; ++pass) {
        const bool was = MG::VectorTransform();
        if (pass == 1) {
            MG::SetVectorTransform(true);
            if (!MG::VectorTransform()) break;   // no vector unit here
            std::fprintf(stderr, "[te.prof] bench: (the same builds through the vector transform)\n");
        }
        view.guard = {0.5f, 0.5f, 1.f, 1.f};
        view.clipped = false;
        int64_t t0 = NowUs();
        for (int r = 0; r < reps; ++r) MG::BuildPrepared(prepared, view, state, nullptr, sink, &out);
        int64_t t1 = NowUs();
        std::fprintf(stderr, "[te.prof] bench: %-44s %9.1f ns/vertex (%u vertices in, %u out)\n", "MeshGeometry::BuildPrepared unclipped",
                     double(t1 - t0) * 1000.0 / double(reps * 1500), unsigned(reps * 1500), unsigned(out));
        view.clipped = true;
        out = 0;
        t0 = NowUs();
        for (int r = 0; r < reps; ++r) MG::BuildPrepared(prepared, view, state, nullptr, sink, &out);
        t1 = NowUs();
        std::fprintf(stderr, "[te.prof] bench: %-44s %9.1f ns/vertex (%u vertices in, %u out)\n", "MeshGeometry::BuildPrepared clipped (no fans)",
                     double(t1 - t0) * 1000.0 / double(reps * 1500), unsigned(reps * 1500), unsigned(out));
        view.guard = {2.f, 2.f, 1.f, 1.f};   // most triangles cross the guard band: fans
        out = 0;
        t0 = NowUs();
        for (int r = 0; r < reps; ++r) MG::BuildPrepared(prepared, view, state, nullptr, sink, &out);
        t1 = NowUs();
        std::fprintf(stderr, "[te.prof] bench: %-44s %9.1f ns/vertex (%u vertices in, %u out)\n", "MeshGeometry::BuildPrepared clipped (fans)",
                     double(t1 - t0) * 1000.0 / double(reps * 1500), unsigned(reps * 1500), unsigned(out));
        MG::SetVectorTransform(was);
        }
    }
    std::fprintf(stderr, "[te.prof] bench done (%lld)\n", (long long)sink);
}

}  // namespace

// ---- the CPU census (the Vita) --------------------------------------------------------------------------------------
namespace {
struct CpuThread {
    std::atomic<const char*> name{nullptr};   // published last: a reader skips a slot being filled
    int uid = 0;
    uint64_t run_us = 0;                        // the census thread's view at the last line
    uint32_t preempted = 0, released = 0;
};
constexpr int kCpuMax = 16;
CpuThread g_cpu[kCpuMax];
std::atomic<int> g_cpu_claimed{0};
uint64_t g_cpu_from = 0;
int64_t g_cpu_wall_us = 0;
// slots 6 / 7 / 8: the disc reader (disc_reader.cpp), the movie decoder (movie_player.cpp), SDL's audio device thread
// (audio_service.cpp's refill)
const char* const kSlotThreadNames[kSlots] = {"te-game", "te-render", "te-build-0", "te-build-1", "te-build-2",
                                              "te-build-3", "te-io", "te-movie", "sdl-audio"};
}  // namespace

void NoteThread(const char* name) {
#if defined(__vita__)
    const int uid = sceKernelGetThreadId();
    const int claimed = std::min(g_cpu_claimed.load(std::memory_order_acquire), kCpuMax);
    for (int i = 0; i < claimed; ++i)
        if (g_cpu[i].name.load(std::memory_order_acquire) && g_cpu[i].uid == uid) return;
    const int slot = g_cpu_claimed.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kCpuMax) return;
    g_cpu[slot].uid = uid;
    g_cpu[slot].name.store(name, std::memory_order_release);
#else
    (void)name;
#endif
}

void CpuCensus(uint64_t presents) {
#if defined(__vita__)
    static const long every = [] {
        const char* v = std::getenv("TE_CPU_EVERY");
        if (!v) v = std::getenv("TE_GFX_SUMMARY_EVERY");
        return v ? std::max(0L, std::atol(v)) : 600L;
    }();
    if (every <= 0) return;
    const int64_t now = NowUs();
    if (g_cpu_wall_us == 0) {   // the first present: the window starts here
        g_cpu_wall_us = now;
        g_cpu_from = presents;
        for (int i = 0; i < std::min(g_cpu_claimed.load(std::memory_order_acquire), kCpuMax); ++i) {
            SceKernelThreadInfo info{};
            info.size = sizeof info;
            if (g_cpu[i].name.load(std::memory_order_acquire) && sceKernelGetThreadInfo(g_cpu[i].uid, &info) >= 0) {
                g_cpu[i].run_us = info.runClocks;
                g_cpu[i].preempted = info.threadPreemptCount;
                g_cpu[i].released = info.threadReleaseCount;
            }
        }
        return;
    }
    if (presents % uint64_t(every) != 0 || presents <= g_cpu_from) return;
    const double wall_us = double(now - g_cpu_wall_us), n = double(presents - g_cpu_from);
    std::string line;
    double busy_us = 0;
    char buf[200];
    for (int i = 0; i < std::min(g_cpu_claimed.load(std::memory_order_acquire), kCpuMax); ++i) {
        const char* name = g_cpu[i].name.load(std::memory_order_acquire);
        if (!name) continue;
        SceKernelThreadInfo info{};
        info.size = sizeof info;
        if (sceKernelGetThreadInfo(g_cpu[i].uid, &info) < 0) continue;   // the thread ended
        const double run = double(info.runClocks - g_cpu[i].run_us);
        const double preempted = double(info.threadPreemptCount - g_cpu[i].preempted);
        const double released = double(info.threadReleaseCount - g_cpu[i].released);
        g_cpu[i].run_us = info.runClocks;
        g_cpu[i].preempted = info.threadPreemptCount;
        g_cpu[i].released = info.threadReleaseCount;
        busy_us += run;
        std::snprintf(buf, sizeof buf, "%s%s %.1f%% (prio %d, %.2f ms, preempted %.1f, waited %.1f)", line.empty() ? "" : "; ",
                      name, wall_us > 0 ? 100.0 * run / wall_us : 0.0, int(info.currentPriority), run / 1000.0 / n,
                      preempted / n, released / n);
        line += buf;
    }
    static uint32_t fast_seen = 0, clamped_seen = 0;
    uint32_t fast = 0, clamped = 0;
    te_unclamped_counts(&fast, &clamped);
    std::fprintf(stderr, "[te.cpu] presents %llu-%llu (%.1f s; per present): %s; the three cores %.1f%% busy with these; "
                 "collision maths unclamped %llu, clamped %llu\n",
                 (unsigned long long)(g_cpu_from + 1), (unsigned long long)presents, wall_us / 1e6, line.c_str(),
                 wall_us > 0 ? 100.0 * busy_us / (3.0 * wall_us) : 0.0, (unsigned long long)uint32_t(fast - fast_seen),
                 (unsigned long long)uint32_t(clamped - clamped_seen));
    fast_seen = fast;
    clamped_seen = clamped;
    te::io::LogWindow(g_cpu_from + 1, presents);   // the disc reader's window (disc_reader.h)
    g_cpu_wall_us = now;
    g_cpu_from = presents;
#else
    (void)presents;
#endif
}

void BindThread(int slot) {
    t_slot = slot >= 0 && slot < kSlots ? slot : -1;
    if (t_slot >= 0) {
        g_bound.fetch_or(1u << t_slot, std::memory_order_relaxed);
        NoteThread(kSlotThreadNames[t_slot]);
    }
}
int ThreadSlot() { return t_slot; }
bool Active() { return g_active; }

unsigned short Register(const char* name) {
    // TE_PROF unset: no section names, no lock (a scope's first run happens mid-game on any thread; on the Vita a pthread
    // mutex is allocated at its first lock, and one made after vitaGL set up its memory is the hazard graphics.cpp's
    // PrimeThreadSync avoids) -- every section is then 0, which nobody samples
    static const bool wanted = [] { const char* v = std::getenv("TE_PROF"); return v && *v == '1'; }();
    if (!wanted) return 0;
    std::lock_guard<std::mutex> lock(g_register_lock);
    const unsigned k = g_registered.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < k; ++i)
        if (std::strcmp(g_names[i], name) == 0) return static_cast<unsigned short>(kHostBase + i);
    if (k >= kHostMax) return 0;
    g_names[k] = name;
    g_registered.store(k + 1, std::memory_order_release);
    return static_cast<unsigned short>(kHostBase + k);
}

bool DrawBenchWanted() {
    const char* b = std::getenv("TE_PROF_BENCH");
    return b && *b == '2';
}

void StartFromEnv() {
#if defined(__vita__)
    // The Vita runs the game on its own kernel thread (runner/te_fiber.c "te-game", which binds itself in FiberMain);
    // this is the main thread, which waits while the game runs
    t_slot = kGame;
    g_bound.fetch_or(1u << kGame, std::memory_order_relaxed);   // (the te-game kernel thread binds itself in FiberMain)
    NoteThread("main");
#else
    BindThread(kGame);
#endif
    if (const char* b = std::getenv("TE_PROF_BENCH"); b && (*b == '1' || *b == '2')) Bench();
    const char* v = std::getenv("TE_PROF");
    if (!(v && *v == '1') || g_active) return;
    if (const char* e = std::getenv("TE_PROF_EVERY")) g_every = std::max(1L, std::atol(e));
    if (const char* p = std::getenv("TE_PROF_US")) g_period_us = std::min(100000L, std::max(100L, std::atol(p)));
    if (const char* r = std::getenv("TE_PROF_ROWS")) g_rows = unsigned(std::min(64L, std::max(1L, std::atol(r))));
    if (const char* m = std::getenv("TE_PROF_MIN")) g_min_pct = std::max(0.0, std::atof(m));
    g_active = true;
    std::thread(Sampler).detach();
    std::fprintf(stderr, "[te.prof] sampling the game / render / build threads every %.1f ms (TE_PROF_US); a report every %ld presents\n",
                 double(g_period_us) / 1000.0, g_every);
}

void TickBegin(uint64_t tick) {
    if (g_active) g_tick.store(uint32_t(tick), std::memory_order_relaxed);
}

std::string TickSections(uint64_t tick, int rows) {
    if (!g_active) return std::string();
    const uint32_t want = uint32_t(tick);
    std::vector<std::pair<uint32_t, unsigned>> counts;   // (samples, id)
    uint32_t total = 0;
    for (unsigned k = 0; k < kTickRing; ++k) {
        if (g_tick_ring[k].tick.load(std::memory_order_relaxed) != want) continue;
        const unsigned id = g_tick_ring[k].id.load(std::memory_order_relaxed);
        ++total;
        bool found = false;
        for (auto& c : counts)
            if (c.second == id) { ++c.first; found = true; break; }
        if (!found) counts.emplace_back(1u, id);
    }
    if (!total) return std::string();
    std::sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string out;
    char buf[160];
    for (size_t i = 0; i < counts.size() && int(i) < rows; ++i) {
        std::snprintf(buf, sizeof buf, "%s%s %.0f%% (%u)", i ? ", " : "", NameOf(counts[i].second),
                      100.0 * double(counts[i].first) / double(total), counts[i].first);
        out += buf;
    }
    return out;
}

void OnPresent(uint64_t presents) {
    if (!g_active || presents % uint64_t(g_every) != 0) return;
    const uint64_t from = g_window_start + 1;
    g_window_start = presents;
    const uint32_t bound = g_bound.load(std::memory_order_relaxed);
    for (int s = 0; s < kSlots; ++s) {
        if (!(bound & (1u << s))) continue;
        std::vector<std::pair<uint32_t, unsigned>> rows;
        uint64_t total = 0;
        for (unsigned id = 0; id < kIds; ++id) {
            const uint32_t c = g_counts[s][id].exchange(0, std::memory_order_relaxed);
            if (c) { rows.emplace_back(c, id); total += c; }
        }
        if (!total) continue;
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string line;
        char buf[160];
        for (size_t i = 0; i < rows.size() && i < g_rows; ++i) {
            const double pct = 100.0 * double(rows[i].first) / double(total);
            if (i >= 4 && pct < g_min_pct) break;
            std::snprintf(buf, sizeof buf, "%s%s %.1f%%", i ? ", " : "", NameOf(rows[i].second), pct);
            line += buf;
        }
        std::fprintf(stderr, "[te.prof] presents %llu-%llu %s (%llu samples): %s\n", (unsigned long long)from,
                     (unsigned long long)presents, kSlotNames[s], (unsigned long long)total, line.c_str());
    }
}

}  // namespace te::prof
