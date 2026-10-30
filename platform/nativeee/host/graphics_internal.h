// platform/nativeee (host side) -- what the graphics seam's modules share: graphics.cpp owns the device, the frame's buckets,
// the materials' reading from guest memory and the GS-valued draw state; graphics_scene.cpp (models, culling, sky, shadows)
// records its draws through these. Not a public header: platform/nativeee/host only.
#pragma once
#include <utility>
#include <cstddef>
#include <type_traits>
#include <new>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "engine/platform/ps2e/scene_interpolator.h"
#include "te_gfx_layout.h"

namespace te::gfx {

// The game's structs as they lie in guest memory (include/game/controllers.h RenderTargetDescription, 0x70 bytes)
struct RenderTargetDescription {
    uint32_t controller;
    int32_t offsetX, offsetY, width, height;
    uint32_t clearColor;
    int32_t displayWidth, displayHeight;
    float clipToScreen[4][4];
    int32_t x, y;
    uint32_t packet, unused6C;
};

// A material read from guest memory: the te_gfx_material, its first shader, that shader's texture and host handle, its bucket
struct MaterialView {
    const te_gfx_material* material = nullptr;
    const te_gfx_shader* shader = nullptr;
    const te_gfx_texture* texture = nullptr;
    uint32_t texture_handle = 0;
    uint32_t bucket = TE_GFX_BUCKET_UI;
};

// One recorded draw: a triangle list under a GS-valued state, drawn at Present in its bucket's order. `copy_frame`
// The frame is copied for the screen-copy texture right before this draw (TR WriteScreenCopy at a screen-copy
// material's packet) -- the draw samples what every draw before it in bucket order left in the frame.
struct DeferredBuild;

// The record in 32-byte groups (the Cortex-A9's cache line; the records are 32-byte aligned): the game thread writes a
// record frames after the drawing thread last read it, on another core -- every line it touches is a miss. Line 0 is what
// every reader looks at first and the whole header of the Vita's COMPACT mesh record (RecordMeshCompact: the mesh, its
// state pool entry, the flags, no vertices); line 1 the skin / interpolation fields; lines 2-7 the mesh parameters
// (screen, origin, scroll and the lit flag in lines 2-4); lines 8-11 the state. A compact record's game-thread writes are
// lines 0 and 2-4 (+ the light rows when lit, lines 4-7); the drawing thread fills in the rest from the state pool when it
// expands the bucket (ExpandBucket) -- the record then reads as RecordMeshInPlace would have written it.
struct alignas(32) Draw {
    // ---- line 0
    uint32_t mesh = 0;              // a native mesh's draw (RecordMesh): no vertices here, the device transforms the mesh
                                    // through mesh_params
    // The Vita's compact mesh record (RecordMeshCompact): its state is the state pool's entry `pool` (1-based; 0 = the
    // record holds its own state); the drawing thread copies it in at ExpandBucket, with the fields a native draw of
    // DrawInstance's always has (skin_joints 0, the untagged tag, the whole frame as the box, the whole mesh). Every recycler
    // of a Draw sets it (0 everywhere but RecordMeshCompact).
    uint16_t pool = 0;
    bool copy_frame = false;
    bool has_transform = false;
    DeferredBuild* pending = nullptr;   // a placeholder whose triangles a worker builds (ReserveDeferred); expanded before drawing
    // The Vita's expanded build segments drawn in place: `ext_count` vertices at `ext` in their DeferredBuild's storage
    // (ExpandBucket; the frame keeps its builds until it is drawn), `verts` empty -- no copy of the built vertices on the
    // drawing thread. Every recycler of a Draw clears it (OpenDraw, RecordMesh, ReserveDeferred, ExpandBucket).
    const ps2e_draw_vertex* ext = nullptr;
    uint32_t ext_count = 0;
    std::vector<ps2e_draw_vertex> verts;
    // ---- line 1
    uint32_t skin_joints = 0;       // a GPU-skinned mesh's draw (RecordSkinMesh; the Vita): `mesh` is a skin mesh, drawn
                                    // through `palette` (skin_joints x 3 rows of 4 floats); 0 joints = a native mesh's draw.
                                    // Set wherever `mesh` is (RecordMesh / RecordSkinMesh): a draw without a mesh never reads it
    uint32_t skin_disp = 0;         // displaced vertices (4 floats each after the joints' 12): 0 for a plain skin
    // What the draw IS, for the presenter's interpolation between two pictures (the engine's SceneInterpolator): a rigid
    // object by instance identity with its matrices as drawn, a vertex run paired by identity and part, or the newest
    // picture's draw as it is (untagged). Stamped from the current tag at OpenDraw (SetDrawTag).
    ps2r::ps2e::InterpTag tag;
    // ---- lines 2-7
    alignas(32) ps2e_draw_mesh_params mesh_params{};
    // ---- lines 8-11
    ps2e_draw_state state;
    std::vector<float> palette;     // the joints' rows, then (a blend skin's part) its vertices' displacements
    ps2r::ps2e::InterpTransform transform;
    const ps2e_draw_vertex* Vertices() const { return ext ? ext : verts.data(); }
    size_t VertexCount() const { return ext ? size_t(ext_count) : verts.size(); }
    // an in-place draw's vertices copied into its own storage (before something is appended to it)
    void OwnVertices() {
        if (!ext) return;
        verts.assign(ext, ext + ext_count);
        ext = nullptr;
        ext_count = 0;
    }
};

// The drawing thread's first reads of a record written on the game thread's core (the draw order's class pass):
// its head, its state's and its mesh parameters' first lines asked for ahead (PLD: no effect on any value)
inline void PrefetchDraw(const Draw* d) {
#if defined(__vita__)
    const char* const s = reinterpret_cast<const char*>(&d->state);
    const char* const m = reinterpret_cast<const char*>(&d->mesh_params);
    __builtin_prefetch(d);
    __builtin_prefetch(s);
    __builtin_prefetch(s + 32);
    __builtin_prefetch(s + 64);
    __builtin_prefetch(s + 96);
    __builtin_prefetch(m + 64);   // (lit)
#else
    (void)d;
#endif
}
// ... and every line the submit reads of a mesh record -- its head, its mesh parameters and its state lie in its
// first 384 bytes (twelve 32-byte lines) -- asked for one draw ahead in the submit loop: a draw's ~10 us covers them
inline void PrefetchDrawHot(const Draw* d) {
#if defined(__vita__)
    const char* const p = reinterpret_cast<const char*>(d);
    for (int k = 0; k < 12; ++k) __builtin_prefetch(p + 32 * k);
#else
    (void)d;
#endif
}

bool EnsureDevice();
MaterialView ReadMaterial(uint32_t address);
// The device texture of a GameTexture (guest address; 0 none): what ReadMaterial puts in MaterialView::texture_handle
uint32_t TextureHandleAt(uint32_t address);
// The material's k-th shader as the view's shader (its texture, handle; the same material and bucket); shader null when the
// material has no k-th shader. A material's later shaders are its further passes over the same geometry
MaterialView ShaderView(const MaterialView& m, uint32_t k);
// The first shader's settings as the draw state (the texture when the shader has one)
ps2e_draw_state StateOf(const MaterialView& m);
// (0x10 / 0x11) sample the frame; without it they draw as before (the first pass, untextured by the frame)
// Recording a draw. OpenDraw hands back the bucket's draw to append whole triangles to: the bucket's LAST draw when
// it has this very state and the state is unblended (one glDrawArrays rasterises its triangles in order exactly as
// separate draws of the same state would -- blended states stay one draw each, as the backend may read the destination
// before each), else a fresh draw with the state set. CloseDraw ends the appending (a draw left under one triangle is
// dropped). The reference is valid until the next OpenDraw/Record on any bucket; the vertex storage is the bucket's and is
// kept across frames (a frame's draws cost no allocation after the first frames). Record is the whole-draw form.
Draw& OpenDraw(uint32_t bucket, const ps2e_draw_state& state, bool copy_frame = false);
void CloseDraw(uint32_t bucket);
void Record(uint32_t bucket, Draw&& d);
// Builds off the game thread. A model's or a skin's geometry is pure work over host-side data (the decoded parts) and the
// draw's inputs, so the game thread records a PLACEHOLDER in the bucket where its triangles go -- the state and the tag
// stamped as OpenDraw would -- and a worker builds them into the placeholder's DeferredBuild, one segment per Append (what
// each OpenDraw / CloseDraw would have received). Before the frame is drawn every bucket is replayed through OpenDraw's own
// joining rule with the placeholders' segments in their places, so the draw list is the one the game thread would have
// recorded building them itself. The job runs under the game thread's floating-point environment (its rounding mode). The
// parts a job reads must outlive it: whatever releases or reloads them drains the builds first.
struct DeferredBuild {
    ps2e_draw_state state{};
    std::vector<ps2e_draw_vertex> verts;
    std::vector<uint32_t> segments;   // the vertex count of each Append, in order (whole triangles, at least one)
    // A build whose segments take one of two states (the shadow volumes' counting up and down): alt_state for the
    // segments whose flag in `alternate` is set (none when the vector is empty)
    ps2e_draw_state alt_state{};
    std::vector<uint8_t> alternate;
    // The Vita's chunked native draws (a clipped instance's part drawn chunk by chunk): a segment whose count is 0 stands
    // for the next of `meshes` -- a native mesh range drawn in its place under `state` (no vertices here)
    struct MeshSegment {
        uint32_t mesh = 0;
        ps2e_draw_mesh_params params{};
    };
    std::vector<MeshSegment> meshes;
    std::atomic<bool> done{false};
    // the Vita's low-latency hand-over replaced the picture this build is for before it was drawn: a worker that has not
    // started the build skips it (done all the same; nothing reads its storage) -- graphics.cpp PresentFrame / CancelBuild
    std::atomic<bool> cancelled{false};
};
bool DeferringBuilds();   // workers are running (TE_GFX_BUILD_THREADS: their count; 0 builds on the game thread)
DeferredBuild& ReserveDeferred(uint32_t bucket, const ps2e_draw_state& state);
// A build's work: the callable held in a pooled block (std::function allocated each closure -- they are hundreds of bytes:
// the views, the lights, the state -- and freed it on the worker; on the Vita an allocation and its free cost microseconds,
// two kernel locks each). A block comes from the posting thread's list, refilled from the blocks the workers hand back
// (a lock-free stack); a closure larger than a block takes the heap. Move-only.
class BuildJob {
public:
    static constexpr size_t kBlockBytes = 1536;
    BuildJob() = default;
    template <typename F, typename Fn = std::decay_t<F>, typename = std::enable_if_t<!std::is_same<Fn, BuildJob>::value>>
    explicit BuildJob(F&& f) {
        const bool fits = sizeof(Fn) <= kBlockBytes && alignof(Fn) <= alignof(std::max_align_t);
        void* memory = fits ? AcquireBlock() : ::operator new(sizeof(Fn));
        pooled_ = fits;
        obj_ = new (memory) Fn(std::forward<F>(f));
        call_ = [](void* p) { (*static_cast<Fn*>(p))(); };
        destroy_ = [](void* p) { static_cast<Fn*>(p)->~Fn(); };
    }
    BuildJob(BuildJob&& o) noexcept : obj_(o.obj_), call_(o.call_), destroy_(o.destroy_), pooled_(o.pooled_) { o.obj_ = nullptr; }
    BuildJob& operator=(BuildJob&& o) noexcept {
        if (this != &o) {
            Reset();
            obj_ = o.obj_; call_ = o.call_; destroy_ = o.destroy_; pooled_ = o.pooled_;
            o.obj_ = nullptr;
        }
        return *this;
    }
    BuildJob(const BuildJob&) = delete;
    BuildJob& operator=(const BuildJob&) = delete;
    ~BuildJob() { Reset(); }
    void operator()() { call_(obj_); }
    explicit operator bool() const { return obj_ != nullptr; }
    void Reset() {
        if (!obj_) return;
        destroy_(obj_);
        if (pooled_) ReleaseBlock(obj_);
        else ::operator delete(obj_);
        obj_ = nullptr;
    }
private:
    static void* AcquireBlock();          // the posting thread's (the game thread)
    static void ReleaseBlock(void* block);  // any thread
    void* obj_ = nullptr;
    void (*call_)(void*) = nullptr;
    void (*destroy_)(void*) = nullptr;
    bool pooled_ = false;
};
void PostBuildJob(DeferredBuild& build, BuildJob&& job);
template <typename F>
void PostBuild(DeferredBuild& build, F&& job) { PostBuildJob(build, BuildJob(std::forward<F>(job))); }
void DrainBuilds();       // every posted build finished
// Storage a build may still read when its owner lets it go (a released model's parts and shadow parts): kept until no
// picture handed over can still be built from it -- the frame slot it travels with is free again (graphics.cpp
// RetainPicture: its picture drawn, every build of it done), or the synchronous present that drew it returned
struct RetiredStorage {
    virtual ~RetiredStorage() = default;
};
void RetireStorage(std::unique_ptr<RetiredStorage> storage);

// Work wanted ahead of its use (a texture's decode, a native mesh's preparation: the builder pool's background queue): a
// worker runs it when no frame build waits; never waited for by DrainBuilds. The posting thread is the game thread.
void PostBackgroundJob(BuildJob&& job);
template <typename F>
void PostBackground(F&& job) { PostBackgroundJob(BuildJob(std::forward<F>(job))); }

// A native mesh PREPARED AHEAD of its first draw (graphics_scene.cpp: at the part's load, on a worker): everything the
// mesh's making computes from the part before the GPU's buffers (ps2e_draw_mesh_prepare: the triangle list deduplicated,
// the bounds) -- `compute` is that work, run once by whichever thread gets to it first (a worker; or the drawing thread's
// batch, Ensure, when the part asks for its mesh before a worker reached it). TE_GFX_MESH_AT_LOAD=0: none (the meshes made
// from their lists at the batch, as before).
struct PreparedMesh {
    enum : int { kQueued = 0, kMaking = 1, kMade = 2, kCancelled = 3 };
    std::atomic<int> state{kQueued};
    ps2e_draw_mesh_prepared* mesh = nullptr;      // (owned; null: nothing to make)
    void (*compute)(PreparedMesh&) = nullptr;
    PreparedMesh() = default;
    PreparedMesh(const PreparedMesh&) = delete;
    PreparedMesh& operator=(const PreparedMesh&) = delete;
    virtual ~PreparedMesh() { if (mesh) ps2e_draw_mesh_prepared_free(mesh); }
    // the work here unless another thread has it (then waited for); false when it was cancelled (the part retired)
    bool Ensure();
    // the work here if no thread has started it (a worker's background job: it never waits); true when it made it
    bool TryMake();
    // a queued preparation not started cancelled, one being made waited for (the part's storage goes after this)
    void Settle();
};

// The tag the next OpenDraw stamps (a producer sets it around its draws and clears it after): a rigid object with its
// model->world and model->screen matrices (16 floats each, the title's row-vector convention), a vertex run (no matrices),
// or none. While the presenter interpolates, draws of different tags are not merged into one.
void SetDrawTag(const ps2r::ps2e::InterpTag& tag, const float* world16 = nullptr, const float* screen16 = nullptr);
void ClearDrawTag();
// Whether the presenter interpolates: the rigid tags' matrices (a double-precision inverse per instance) only serve it
bool Interpolating();
// Native meshes (ps2e_draw_mesh_*): the models' triangles kept on the device, drawn through a matrix. NativeMeshes() is
// whether they are in use (a device with the native transform; TE_GFX_NATIVE_MESH=0 turns them off). CreateMeshes makes
// each listed mesh on the drawing thread (0 where the device declines, which turns them off for the session);
// RecordMesh records one draw in a bucket; RetireMesh destroys one once no frame handed over can still draw it.
bool NativeMeshes();
struct MeshRequest {
    std::vector<ps2e_draw_mesh_vertex> vertices;
    uint32_t handle = 0;
    // the mesh prepared ahead (PreparedMesh): made from its prepared form (the same mesh) instead of `vertices`
    std::shared_ptr<PreparedMesh> prepared;
    // a GPU-skinned part's mesh (the Vita: ps2e_draw_skin_mesh_create) instead: its vertices, indices and largest alpha
    std::vector<ps2e_draw_skin_vertex> skin_vertices;
    std::vector<uint16_t> skin_indices;
    int skin_max_alpha = 0;
};
void CreateMeshes(std::vector<MeshRequest>& requests);
// CreateMeshes without waiting for the drawing thread: the meshes are made there before the next picture handed over (the
// drawing thread takes its jobs, in order, ahead of a picture), `requests` filled meanwhile (owned by the caller's shared
// pointer until then); the ticket says when (0: made already, on this thread). WaitMeshes(ticket) waits for them, then
// applies a decline as CreateMeshes does.
struct MeshBatchJob {
    std::vector<MeshRequest> requests;
    bool declined = false;
};
uint64_t CreateMeshesAsync(const std::shared_ptr<MeshBatchJob>& batch);
void WaitMeshes(uint64_t ticket, const MeshBatchJob& batch);
void RecordMesh(uint32_t bucket, const ps2e_draw_state& state, uint32_t mesh, const ps2e_draw_mesh_params& params);
// RecordMesh with the parameters written by the caller straight into the record (no copy of a filled-in stack struct):
// the returned parameters hold an earlier draw's values -- the caller sets every field the draw reads (screen, offset,
// scroll, lit and, when lit, the lights, box, first, count); valid until the next record on any bucket
ps2e_draw_mesh_params& RecordMeshInPlace(uint32_t bucket, const ps2e_draw_state& state, uint32_t mesh);
// THE VITA'S COMPACT MESH RECORDS (graphics.cpp; the game thread). A native mesh draw of DrawInstance's kind -- the state is a
// pooled one, the box the whole frame, the whole mesh (first / count 0), skin_joints 0, the untagged tag -- recorded as its
// header (line 0 of the record: the mesh and the pool entry) plus the parameters the caller writes (screen, offset, scroll,
// lit and, when lit, the lights; NOT box / first / count): ~4 of the record's lines instead of ~11. The drawing thread copies
// the pooled state and the constant fields in when it expands the bucket (ExpandBucket), before any reader sees the record.
// CompactMeshes(): whether the records may be compact now (the Vita, no interpolating presenter -- a picture expanded at
// once or never --, the tag untagged, TE_GFX_COMPACT=0 off); else RecordMeshInPlace. TE_GFX_COMPACT_CHECK=1 (diagnosis):
// every compact record ALSO written whole, and the expansion compares what it would write with it (the census line).
bool CompactMeshes();
ps2e_draw_mesh_params& RecordMeshCompact(uint32_t bucket, uint32_t pool, const ps2e_draw_state& state, uint32_t mesh);
bool CompactCheck();
// The state pool: draw states kept by content for the compact records (the game thread's; the drawing thread reads an entry
// at a picture's expansion). An entry is recycled only once every picture that used it is drawn or dropped (each use stamps
// the picture being recorded; the drawing thread publishes the last picture it has finished). PoolState: the entry of
// `state`'s bytes (made when new; 0 = the pool is full: record whole), `generation` what PoolCurrent checks a kept id with;
// PoolCurrent: the kept (id, generation) still names that entry (not recycled since).
uint32_t PoolState(const ps2e_draw_state& state, uint32_t* generation);
bool PoolCurrent(uint32_t pool, uint32_t generation);
// The census (graphics_scene.cpp SeamCensusLine prints it): compact / whole mesh records, the pool's use; a mesh record
// that could have been compact but went whole (the pool full) is noted by its producer
std::string CompactCensusLine(double presents);
void NoteWholeMeshRecord();
// A GPU-skinned part's draw (the Vita): its skin mesh through `params` (the skin's matrix to the screen, the GS origin, the
// scroll, the lights) and the palette `bones` (`joints` x 12 floats, copied into the draw)
void RecordSkinMesh(uint32_t bucket, const ps2e_draw_state& state, uint32_t mesh, const ps2e_draw_mesh_params& params,
                    const float* bones, uint32_t joints, const float* disp = nullptr, uint32_t disp_count = 0);
// The skins' GPU path (graphics_scene.cpp; the Vita): on unless TE_GFX_GPU_SKIN=0 (read by graphics.cpp ReadOptions) or
// the device has no skinning programs (the first mesh it declines turns it off); the scene's census line per window
void SetGpuSkins(bool on);
void SetGpuBlend(bool on);   // the blend skins' parts too (TE_GFX_GPU_BLEND=1; off by default: a draw per part)
std::string SkinPathCensusLine(double frames);
// The clipped instances' and the sky's per-chunk native draws (graphics_scene.cpp ChunkedBuild; the Vita): on unless
// TE_GFX_CHUNKED_NATIVE=0; the census line per window
void SetChunkedNative(bool on);
std::string ChunkCensusLine(double frames);
// the builders' work per frame (graphics_scene.cpp; the Vita: the shadow volumes, the chunked builds, the CPU
// skins) with the build pool's line (graphics.cpp PoolCensusLine) after it; empty elsewhere
std::string BuildersCensusLine(double frames);
std::string PoolCensusLine(double frames);
void RetireMesh(uint32_t mesh);
// The Vita's draw order for the GPU's hidden-surface removal (graphics_zorder.cpp; TE_GFX_VITA_ZORDER): a bucket's draw
// ORDER (`used` pointers to its draws, graphics.cpp ExpandBucket) permuted on the drawing thread right before the draws are
// submitted -- within each run of draws whose picture does not depend on their order, the alpha-tested (punch-through)
// ones after the others, near to far; the 504-byte records themselves never move. `presents`: the presents so far (the
// knob's in-run A/B). Elsewhere a no-op. The census line per window (empty when never called).
void ZOrderDraws(ps2e_draw* device, Draw** draws, size_t used, uint64_t presents);
std::string ZOrderCensusLine(double frames);
// THE BATCHING CENSUS and THE INSTANCING PLAN over each bucket's drawing order (graphics_batch.cpp; the Vita;
// elsewhere no-ops). BatchCensusFrame: whether this picture is censused (1 in TE_GFX_VITA_BATCH_CENSUS, default 32), asked
// once per picture. InstancePlanMode: TE_GFX_VITA_INSTANCE (0 off, 1 instanced draws, 2 the planned order one draw at a
// time). PlanBucket: the bucket's census (`census`) and, when `plan`, its plan -- per draw of `list` its group's next draw
// and whether it is drawn with its group's first draw (valid until the next call); `barrier`: the index of the draw the
// sky's layers are drawn before (`count`: none). The census lines per window (empty when nothing was censused).
struct BatchPlan {
    std::vector<int32_t> next;      // a group's draws chained in their order from its first (-1: the last / no group)
    std::vector<uint8_t> absorbed;  // 1: drawn at its group's first draw (skipped at its own place)
};
int InstancePlanSetting();   // TE_GFX_VITA_INSTANCE as set: 0 unset / off, 1, 2, 3 = abN[:M]
const char* InstancePlanArmName();   // the A/B's arm 1: "instanced" / "the planned order"
int InstancePlanMode();      // this picture's: 0 off, 1 instanced, 2 the planned order one draw at a time (abN: 1 / 0)
bool BatchCensusFrame(bool picture);
const BatchPlan* PlanBucket(ps2e_draw* device, Draw* const* list, size_t count, size_t barrier, bool census, bool plan,
                            long picture, int bucket);
void NotePlannedFrame();
std::string BatchCensusLine(double frames);
// THE SCREEN COPIES COALESCED (graphics_copies.cpp; TE_GFX_COPY_COALESCE, every platform): a frame-copy draw's copy
// (ps2e_draw_copy_frame -- on the Vita two GPU scene ends, the copy pass and the restore pass) is skipped when the copy
// the frame already holds is, in every texel the draws sampling this copy read, what a new copy would hold: no draw since
// that copy touched the region those draws sample. SubmitBuckets drives it in drawing order: CopiesBeginFrame before the
// frame's first draw (`copy_draws` = the frame's copy_frame draws), CopyWanted at each copy_frame draw `list[i]` (true =
// take the copy; the draws of `list` before `i` not seen yet are noted first), CopiesBucketEnd after each bucket's last
// draw, CopiesDirtyAll for a pass whose extent is not bounded (the Vita's sky layers drawn at a stop). The census line per
// pace window (empty when the window had no copy_frame draw).
void CopiesBeginFrame(ps2e_draw* device, int copy_draws, int screen_w, int screen_h);
// region: the copy to ask for when true (frame pixels x0, y0, x1, y1; x0 > x1 = the whole frame): ps2e_draw_copy_frame_region
bool CopyWanted(Draw* const* list, size_t count, size_t i, float region[4]);
void CopiesBucketEnd(Draw* const* list, size_t count);
void CopiesDirtyAll();
std::string CopyCensusLine(uint64_t first_present, uint64_t last_present);
// graphics_scene.cpp: the meshes the frame's draws asked for, made once per picture (before its hand-over)
void FlushNativeMeshes();
std::string PrepCensusLine();   // (graphics_scene.cpp: the native meshes prepared at their load, the pace window's line)
// TE_AB=particlefull: the particle blocks through the previous path (graphics_particles.cpp)
void SetParticleFull(bool on);
// The blended UV-scrolled passes as native meshes (graphics_scene.cpp; ON by default -- picture-verified on the hub and in
// the Mech arena; TE_GFX_NATIVE_SCROLLED=0 / the off arm of TE_AB=nativescroll: CPU-built every frame as before)
void SetNativeScrolled(bool on);
// The game thread's fast seam paths (graphics_scene.cpp, graphics_decals.cpp, math_vu0.cpp: the culling's matrices made
// when read, the decals' ageing without the VU clamps that cannot act, ...): ON by default, TE_GAMECPU2=0 / the off arm of
// TE_AB=gamecpu2 run them as before. The same answers either way (bit for bit); only the work differs.
bool GameCpuFast();
void SetGameCpuFast(bool on);
// graphics_decals.cpp: the decal ageings DecalMath::Age answered without the VU clamps (the seams' census)
uint64_t DecalsUnclamped();
// The calls of the game thread's other costly seams, per present in the "[te.gfx.calls]" census line beside
// "[te.gfx.seams]" (graphics_scene.cpp SeamCensusLine): a device TE_PROF share / the calls = what one call costs there.
// Plain counters (the game thread's only), one increment a call.
enum SeamCall : uint32_t {
    kCallGetBbox, kCallBboxPoints, kCallTriangleBounds, kCallJointMatrix, kCallEuler, kCallMultiplyByParent, kCallLerp,
    kCallMultiplyMatrices, kCallStartRay, kCallFinishRay, kCallParticleView, kCallSkin, kCallBlendSkin, kCallShadowMesh,
    kCallCount
};
extern uint32_t g_seamCalls[kCallCount];
inline void CountCall(SeamCall c, uint32_t n = 1) { g_seamCalls[c] += n; }
// TE_GFX_REPEAT_CENSUS=1 (diagnosis; graphics_scene.cpp): per seam, how many of a picture's calls had the very input words of
// the previous picture's call at the same place in the call sequence, or of any of its calls -- what a memo of the seam
// would answer from the picture before ("[te.gfx.repeat]" with the seams' census). Off: one load and a branch per call.
extern bool g_repeatCensus;
void NoteRepeatWords(SeamCall c, const void* a, size_t aBytes, const void* b = nullptr, size_t bBytes = 0, const void* d = nullptr,
                     size_t dBytes = 0);
inline void NoteRepeat(SeamCall c, const void* a, size_t aBytes, const void* b = nullptr, size_t bBytes = 0, const void* d = nullptr,
                       size_t dBytes = 0) {
    if (g_repeatCensus) NoteRepeatWords(c, a, aBytes, b, bBytes, d, dBytes);
}
// The frame's camera for the interpolator (graphics_scene.cpp: the game's current RenderView's world->camera and
// camera->screen, and its camera object); false without a view.
bool CurrentCamera(ps2r::ps2e::InterpCamera& out, uint32_t* camera_object);
// The per-frame draw ordinals the scene's identities use (graphics_scene.cpp), reset at the frame's start.
void ResetDrawOrdinals();
// The GS origin in frame pixels (2048 - screen/2) every draw subtracts, and the frame's size
float GsOriginX();
float GsOriginY();
int ScreenW();
int ScreenH();
// The game's current render target (g_RenderTarget) or null
const RenderTargetDescription* CurrentTarget();
// Options: TE_GFX_LOG
bool LogEnabled();
// Counters the scene module adds to (the summary line)
struct SceneCounters {
    uint64_t rigid_models = 0, placed_models = 0, sky_models = 0, shadow_meshes = 0, model_triangles = 0, models_decoded = 0,
             models_refused = 0, submodels_missing = 0, culls = 0;
    // the skinned characters (DrawSkin / DrawBlendSkin instances, their triangles, the submodel packets decoded / refused)
    uint64_t skins = 0, blend_skins = 0, skin_triangles = 0, skins_decoded = 0, skins_refused = 0;
    // shadow brackets composited (StartShadows..ApplyShadows with volumes), strips without a +8/-8 winding colour
    uint64_t shadow_passes = 0, shadow_odd_colours = 0;
    // game-thread time in the skin and shadow bodies (ms; the summary's attribution of their cost)
    double skin_ms = 0, shadow_ms = 0;
    // particle blocks drawn (chain members), their triangles, blocks the VU1 program body declined
    uint64_t particle_blocks = 0, particle_triangles = 0, particle_refused = 0;
    // the decals -- AgeDecal calls (the retail VU0 micro), those it culled (outside the view), the blocks DrawDecals
    // ran through the VU1 program body, the quads it kicked, the blocks the body declined
    uint64_t decals_aged = 0, decals_culled = 0, decal_blocks = 0, decal_quads = 0, decal_refused = 0;
    // the screen models (the skid marks) drawn and their triangles after the near clip
    uint64_t screen_models = 0, screen_model_triangles = 0;
    // a material's later shaders drawn as further passes over the submodel (the crystals' lit pass over the screen copy)
    uint64_t material_passes = 0;
};
SceneCounters& Scene();
// Save states: every decoded submodel's kept packet bytes (the game releases the packet's disk memory after ReadModel),
// and the decode replayed from them on restore
struct ModelSource {
    uint32_t model;
    int32_t index, count, vertices;
    std::vector<uint8_t> packet;
};
std::vector<ModelSource> ModelSources();
void ReplayModelSources(const std::vector<ModelSource>& sources);
// the skins' submodel packets the same way (the disk manager may move the game's block; the host keeps the bytes);
// the blend skins' parts are decoded from their heap packets at the first draw and forgotten on a restore
std::vector<ModelSource> SkinSources();
void ReplaySkinSources(const std::vector<ModelSource>& sources);
// One GIF packet a VU1 program body kicked (PACKED: a GIF tag then NLOOP x NREGS quadwords of RGBAQ / ST / UV / XYZ2) lowered
// to triangle-list vertices in frame pixels as the GS assembles a strip (ADC on XYZ2 withholds the kick); u/v as fractions of
// the texture's size when textured. The particles' and the decals' packets (graphics_particles.cpp, graphics_decals.cpp)
// the screen models' records (graphics_screen_models.cpp) for the save state: the live models' handles, materials and
// vertexes as one blob; restored (every model replaced; an empty blob = none)
std::vector<uint8_t> ScreenModelState();
bool RestoreScreenModelState(const uint8_t* data, size_t size);
size_t ScreenModelCount();
bool LowerPackedPacket(const std::vector<uint8_t>& packet, float offX, float offY, float tw, float th, bool textured,
                       std::vector<ps2e_draw_vertex>& out);
// the device's presentation (te_display.h): set now when the device exists, else kept for its creation; and
// whether the run is headless (the frozen judges' captures: the frame's own pixels, no window changes).
void SetPresentation(const ps2e_draw_presentation& presentation);
bool Headless();
// The presenter's cadence (te_display.h Settings::frame_rate: 0 = 30 .. 4 = 240, 5 = uncapped): the render thread's
// PresentPacer takes it at its next picture. Deterministic / synchronous runs present every picture whatever the value.
void SetFrameRate(int rate);
// video.interp: the frames between two pictures interpolated (TE_GFX_INTERP, when set, wins)
void SetInterpolation(bool on);

}  // namespace te::gfx
