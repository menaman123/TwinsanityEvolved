// platform/nativeee (host side) -- the LOAD CENSUS: what a tick of the game thread did that a chunk load / an area entry
// costs (the game's resources made into the host's -- textures decoded and made, model parts decoded, native meshes asked
// for and posted -- and the waits that come with them: the drawing thread making a texture or a mesh batch, the builds drained
// before a model is replaced, the disc reader), and the drawing thread's side of the same work. Nothing here changes what
// anything computes or when: it counts and times.
//
//   per tick      the slow-tick line (TE_TICK_LOG_MS=x: `[twinsanityevolved] tick: boundary N took Y ms (...)`) carries the tick's
//                 load work after its phases: ` load: textures 12 decoded (840 KB, 55.1 ms) made 12 (waited 3.2 ms); ...`
//   per window    `[te.load] presents A-B: ...` with the pace census (TE_GFX_SUMMARY_EVERY): the window's ticks over
//                 TE_LOAD_LONG_MS (default 25) -- how many, the longest, what they did -- and the window's whole load work
//                 ("texture decode: N textures, M KB, T ms"), the drawing thread's texture / mesh creation time, vitaGL's
//                 allocations in the long ticks (the Vita)
//
// The game thread notes its own work only (the first thread that ends a tick is the game thread); the drawing thread's
// notes go to their own counters (atomics: the window line may be printed by either thread).
#pragma once
#include <cstdint>
#include <string>

namespace te::load {

enum Work : int {
    kTexDecode = 0,   // a texture's GS upload decoded to RGBA (+ its levels): count, ms, bytes of RGBA
    kTexMake,         // the game thread's wait while the drawing thread made it (OnRender): count, ms
    kTexRelease,      // the game thread's wait at a texture's release (the drawing thread destroying it)
    kModelDecode,     // a model part's packet decoded and prepared (DecodePacket + MeshPrepare): count, ms
    kModelWait,       // the builds drained / the mesh batches taken before a model part is (re)placed
    kMeshAsk,         // parts that asked for their native mesh this picture (their first draw; CPU-built this picture)
    kMeshPost,        // FlushNativeMeshes: the asked meshes composed (MeshVertices, MakeChunks) and posted: count = meshes, bytes = vertices
    kMeshWait,        // the game thread's wait for the drawing thread's mesh batches (ResolveMeshBatches / WaitMeshes)
    kDiscWait,        // the game thread's wait for the disc reader (a landing before the reader had the bytes)
    kModelRelease,    // a model released (the builds drained, the mesh batches taken, its parts' meshes retired)
    kWorkKinds
};

// The calling thread's work of kind `w` (ignored unless the calling thread is the game thread)
void Note(Work w, double ms, uint32_t count = 1, uint64_t bytes = 0);
bool OnGameThread();

// A scope timed into Note(w) at its end (count / bytes as given, or set before the end)
class Timed {
public:
    explicit Timed(Work w, uint32_t count = 1, uint64_t bytes = 0);
    ~Timed();
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;
    uint32_t count;
    uint64_t bytes;
private:
    Work w_;
    int64_t t0_;
};

// The drawing thread's side (any thread but the game thread): textures made (their GPU textures created, levels
// uploaded), mesh batches made (meshes, of them the dedup's time), and the longest single job
void NoteRenderTexture(double ms);
// a texture decoded off the game thread (the eager textures: a worker ahead of its use, or the drawing thread at its first
// draw): any thread
void NoteDecodeElsewhere(double ms, uint64_t bytes);
void NoteRenderMeshes(uint32_t meshes, double ms);
void NoteRenderJob(double ms);

// The vitaGL allocations so far (the Vita's census wrappers; zero elsewhere), read at every tick's end
struct GpuAllocs { uint64_t allocs = 0, bytes = 0, frees = 0, us = 0; };
void SetGpuAllocSource(GpuAllocs (*source)());

// The game thread at a boundary: the tick that ended took `tick_ms` of work. Folds the tick's notes into the window and
// starts the next tick; `line` (when not null and the tick had load work) gets the slow-tick line's suffix.
void EndTick(uint64_t boundary, double tick_ms, std::string* line);

// The window's line (and the window started over); empty when nothing was noted
std::string WindowLine(uint64_t from, uint64_t to);

// Monotonic nanoseconds (the census clock)
int64_t NowNs();

}  // namespace te::load
