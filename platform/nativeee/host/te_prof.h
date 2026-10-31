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

// ---- the game thread's costly seams, by group (the "[te.prof] ... game seams" line) ---------------------------------------
// The device's game-thread profile has one "(own code)" bucket for the game's code AND the host seam bodies it calls (the
// seam thunks mark their sections only in a TE_PROF_SEAM build). The seams below mark their group's section while they run
// when TE_PROF is on, and count their calls; every TE_PROF_EVERY presents the report gives each group's calls and its
// milliseconds a present (its share of the game thread's samples times the window's wall time). With TE_PROF off a group's
// id is 0: the scope is one load and one predicted branch, nothing written.
enum SeamGroup : uint8_t {
    kSeamRays,        // SetRay / StartRayTriangle / FinishRayTriangle (calls = the ray / triangle tests)
    kSeamCollision,   // the second seam's collision maths: edge tests, vertex differences, point groups, hull supports
    kSeamBoxes,       // the culling: views, links, model loads, cell and box tests, results, clip boxes (calls = box tests)
    kSeamPlace,       // DrawPlacedModel / DrawRigidModel: the instances' draws recorded (calls = instances)
    kSeamSkins,       // DrawSkin / DrawBlendSkin
    kSeamJoints,      // the animations' joint maths: joint matrix, Euler blend, parent product, lerp, slerp, turn
    kSeamMatrices,    // the VU0 matrix seams: products, transforms, rotations, rigid inverses, planes
    kSeamBounds,      // GetBbox / PointsBox / TriangleBounds
    kSeamShadows,     // DrawShadowMesh
    kSeamParticles,   // the particle views and block views
    kSeamDecals,      // the decals' views, ageing and draws
    kSeamSky,         // DrawSky
    kSeamGroups
};
// the groups' section ids (0 while TE_PROF is off) and their calls (the game thread writes, the report reads)
extern unsigned short g_seam_id[kSeamGroups];
void CountSeam(SeamGroup group);

class SeamScope {
public:
    SeamScope(SeamGroup group, bool counted) : id_(g_seam_id[group]) {
        if (__builtin_expect(id_ != 0, 0)) {
            saved_ = te_prof_section[kGame];
            te_prof_section[kGame] = id_;
            if (counted) CountSeam(group);
        }
    }
    ~SeamScope() {
        if (__builtin_expect(id_ != 0, 0)) te_prof_section[kGame] = saved_;
    }
    SeamScope(const SeamScope&) = delete;
    SeamScope& operator=(const SeamScope&) = delete;
private:
    unsigned short id_;
    unsigned short saved_ = 0;
};

}  // namespace te::prof

// TE_SEAM(kSeamRays): the rest of the enclosing seam body is its group's section on the game thread, one call counted;
// TE_SEAM_PART(kSeamRays): the same, not counted (a seam that is part of a counted one's work, e.g. StartRayTriangle)
#define TE_SEAM(group) ::te::prof::SeamScope te_seam_scope_(::te::prof::group, true)
#define TE_SEAM_PART(group) ::te::prof::SeamScope te_seam_scope_(::te::prof::group, false)

#define TE_PROF_CAT2(a, b) a##b
#define TE_PROF_CAT(a, b) TE_PROF_CAT2(a, b)
// TE_PROF_SCOPE("name"): the rest of the enclosing block is the section `name` on this thread
#define TE_PROF_SCOPE(name)                                                                       \
    static const unsigned short TE_PROF_CAT(te_prof_id_, __LINE__) = ::te::prof::Register(name); \
    ::te::prof::Scope TE_PROF_CAT(te_prof_scope_, __LINE__)(TE_PROF_CAT(te_prof_id_, __LINE__))
