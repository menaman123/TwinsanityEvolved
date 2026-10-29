// platform/nativeee (host side) -- a sampling section profiler for targets without a host profiler (the Vita in its
// emulator). TE_PROF=1 starts a sampler thread that, every ~0.5 ms, reads the section each profiled thread is in and
// counts it; every TE_PROF_EVERY presents (default 300; te::prof::OnPresent from the pace bookkeeping) one line per
// thread gives the sections' shares of that window. Off (TE_PROF unset) the sections cost a plain store each.
//
//   threads (slots): 0 the game thread (its seam thunks mark section = the import's number when the thunks are compiled
//   with TE_PROF_SEAM: a Vita profiling build, cmake -DTE_PROF_SEAM=ON), 1 the render thread, 2.. the geometry builders,
//   6 the disc reader, 7 the movie decoder, 8 the audio device thread (its refill mixes the real-time producer's ticks).
//   Section 0 = the thread's own code (the game thread: the wasm module, and its imports without TE_PROF_SEAM).
//   TE_PROF_BENCH=1 also times a few primitives at start-up (the emulator's cost per kernel call, lock, allocation...).
#pragma once
#include <cstdint>
#include <string>

extern "C" {
extern volatile unsigned short te_prof_section[];
}
struct ps2e_draw;   // engine/platform/ps2e/ps2e_draw.h

namespace te::prof {

constexpr int kSlots = 9;
// 2.. the geometry builders, 6 the disc reader ("io"), 7 the movie decoder, 8 the audio device thread ("audio": the
// sdl-audio thread whose refill mixes the real-time producer's ticks, audio_service.cpp)
constexpr int kGame = 0, kRender = 1, kBuild0 = 2, kAudio = 8;

// The calling thread's slot (-1: not profiled). Set once per thread.
void BindThread(int slot);
int ThreadSlot();
// The CPU census (the Vita; a no-op elsewhere): every noted thread's kernel-accounted CPU time, priority, preemptions
// and waits per pace window -- "[te.cpu] presents A-B (S s): te-game 41% (prio 96, 12.3 ms a present, preempted 3.1 /
// waited 2.0 a present), ...". Which thread is CPU-bound and which is waiting, on the device, without TE_PROF.
// NoteThread: report the calling thread under `name` (a literal; once per thread; BindThread notes the profiled slots).
void NoteThread(const char* name);
// The pace bookkeeping's present count: a census line every TE_CPU_EVERY presents (default TE_GFX_SUMMARY_EVERY, else
// 600; 0 = off)
void CpuCensus(uint64_t presents);
// A host section's number, by name (once per call site: a function-local static)
unsigned short Register(const char* name);
// Start the sampler if TE_PROF is set (and run the TE_PROF_BENCH timings); binds the calling thread as the game thread
void StartFromEnv();
bool Active();
// The pace bookkeeping's present count: a report every TE_PROF_EVERY presents
void OnPresent(uint64_t presents);
// The game thread's samples by tick (the slow-tick line's attribution): TickBegin(n) stamps the game thread's samples
// from here with tick n (the clock, at a boundary's release); TickSections(n, rows) gives the sections of the samples stamped
// n (the last ~16 s of samples are kept): "(own code) 62% (31), texture: ... 20% (10), ..." -- empty without TE_PROF
void TickBegin(uint64_t tick);
std::string TickSections(uint64_t tick, int rows);
// TE_PROF_BENCH=2 (the Vita): the per-draw cost model, timed once at the device's creation (te_draw_bench.cpp)
void DrawBench(::ps2e_draw* device);
bool DrawBenchWanted();

class Scope {
public:
    explicit Scope(unsigned short id) : slot_(ThreadSlot()) {
        if (slot_ >= 0) {
            saved_ = te_prof_section[slot_];
            te_prof_section[slot_] = id;
        }
    }
    // the same with the thread's slot already known (ThreadSlot() read once by a hot loop: a thread_local is the Vita's
    // emulated TLS -- a DMB and a pthread_getspecific chain per read)
    Scope(unsigned short id, int slot) : slot_(slot) {
        if (slot_ >= 0) {
            saved_ = te_prof_section[slot_];
            te_prof_section[slot_] = id;
        }
    }
    ~Scope() {
        if (slot_ >= 0) te_prof_section[slot_] = saved_;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    int slot_;
    unsigned short saved_ = 0;
};

}  // namespace te::prof

#define TE_PROF_CAT2(a, b) a##b
#define TE_PROF_CAT(a, b) TE_PROF_CAT2(a, b)
// TE_PROF_SCOPE("name"): the rest of the enclosing block is the section `name` on this thread
#define TE_PROF_SCOPE(name)                                                                       \
    static const unsigned short TE_PROF_CAT(te_prof_id_, __LINE__) = ::te::prof::Register(name); \
    ::te::prof::Scope TE_PROF_CAT(te_prof_scope_, __LINE__)(TE_PROF_CAT(te_prof_id_, __LINE__))
