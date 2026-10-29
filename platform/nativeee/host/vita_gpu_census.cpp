// platform/nativeee (host side, the Vita) -- vita_gpu_census.h.
#include "vita_gpu_census.h"

#if defined(__vita__)
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/render/backends/gl/gl_census.h"

// vitaGL r1488's globals, read only: the context it draws on (gxm.c), the fragment notification it passes to every
// sceGxmEndScene with value = the count of scenes ended so far (buffers.c query_fence, gxm.c scene_end), its frame count
// (gxm.c vgl_framecount, vglSwapBuffers' count). Weak: a vitaGL without them still links (their address is then null).
extern "C" {
extern SceGxmContext* gxm_context __attribute__((weak));
extern SceGxmNotification query_fence __attribute__((weak));
}

namespace {

uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }

struct Timed {
    std::atomic<uint32_t> calls{0};
    std::atomic<uint64_t> us{0};
    std::atomic<uint32_t> max_us{0};
    void Note(uint64_t d) {
        calls.fetch_add(1, std::memory_order_relaxed);
        us.fetch_add(d, std::memory_order_relaxed);
        const uint32_t d32 = d > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(d);
        if (d32 > max_us.load(std::memory_order_relaxed)) max_us.store(d32, std::memory_order_relaxed);
    }
    struct Snap { uint32_t calls; uint64_t us; uint32_t max_us; };
    // the per-draw calls (hundreds a frame) are timed one in kSample and scaled: the timer is a kernel call (two per timed
    // call; 1 in 8 cost the drawing thread ~0.2 ms a frame at 800 draws)
    static constexpr uint32_t kSample = 32;
    void NoteSampled(uint64_t d) {
        calls.fetch_add(kSample, std::memory_order_relaxed);
        us.fetch_add(d * kSample, std::memory_order_relaxed);
        const uint32_t d32 = d > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(d);
        if (d32 > max_us.load(std::memory_order_relaxed)) max_us.store(d32, std::memory_order_relaxed);
    }
    Snap Take() {
        return Snap{calls.exchange(0, std::memory_order_relaxed), us.exchange(0, std::memory_order_relaxed),
                    max_us.exchange(0, std::memory_order_relaxed)};
    }
};

Timed g_begin_scene, g_end_scene, g_display_queue, g_finish, g_notification_wait, g_patch_fragment, g_patch_vertex, g_draw;
Timed g_gl_draw_arrays;   // vitaGL's glDrawArrays as a whole (its CPU work per draw, sceGxmDraw included)

// The GXM calls of the window, every one counted (the wrappers below): what the drawing thread hands the GPU library per
// frame. Draws, instanced draws, vertex streams, fragment textures, programs (vertex + fragment), default uniform
// buffers (vitaGL sets a fresh one per draw whose uniforms changed), depth / stencil / raster state.
// (plain counters: every counted call and the census line are the drawing thread's -- vitaGL draws from one thread --
// so a count costs a load, an add and a store, not an atomic read-modify-write)
enum GxmCategory { kGxmDraws, kGxmInstanced, kGxmStreams, kGxmTextures, kGxmPrograms, kGxmUniforms, kGxmState, kGxmCategories };
uint32_t g_gxm_calls[kGxmCategories];

// vitaGL's vglMemType: VRAM, RAM, PHYCONT, BUDGET, EXTERNAL (the newlib heap)
constexpr int kPools = 5;
const char* const kPoolNames[kPools] = {"vram", "ram", "phycont", "budget", "newlib"};
std::atomic<uint32_t> g_alloc_ok[kPools], g_alloc_fail[kPools];
std::atomic<uint64_t> g_alloc_bytes[kPools];
std::atomic<uint32_t> g_frees{0}, g_gl_finish{0}, g_long_sleeps{0};
std::atomic<uint64_t> g_alloc_fail_bytes{0};
// ... and the same since the start (never reset: the load census's per-tick differences, AllocsSoFar)
std::atomic<uint32_t> g_allocs_ever{0}, g_frees_ever{0};
std::atomic<uint64_t> g_alloc_bytes_ever{0};
// the time spent inside vitaGL's allocator and its frees (every call timed: a kernel clock read each side), the window's
std::atomic<uint64_t> g_alloc_us{0}, g_free_us{0}, g_alloc_us_ever{0};
std::atomic<uint32_t> g_alloc_max_us{0};

// the GPU lag at the presents, and the probe
std::atomic<uint64_t> g_lag_sum{0};
std::atomic<uint32_t> g_lag_n{0}, g_lag_max{0};
long g_probe_every = -1;
uint64_t g_presents = 0;
// the last presents' fence marks (the scenes ended up to each one's swap), the drawing thread's own (FramesInFlight)
constexpr uint64_t kPresentMarks = 8;
uint32_t g_present_marks[kPresentMarks] = {};
uint64_t g_marks_valid = 0;
bool g_probe_armed = false;          // the GPU was drained after the previous present: this one is measured
int g_probe_arm = 0;                 // the fast PS2 program's arm of the measured frame
uint64_t g_probe_drained_at = 0;     // when the drain ended (the probe frame's submission starts from an idle GPU)
// per arm of the fast PS2 program's A/B ([0] off, [1] on; without the A/B every probe is the on arm's, or the off arm's
// when the fast program is not built)
std::atomic<uint32_t> g_probe_n[2];
std::atomic<uint64_t> g_probe_tail_us[2], g_probe_submit_us[2];
std::atomic<uint32_t> g_probe_tail_max_us[2];
std::atomic<uint64_t> g_probe_drain_us{0};
std::atomic<uint32_t> g_probe_drains{0};
// the GPU's busy share without a drain (SampleGpuBusy, the profiler's thread): samples, busy samples, the window's clock
std::atomic<uint32_t> g_busy_n{0}, g_busy_yes{0};
uint64_t g_busy_window_us = 0;
// the PER-PRESENT GPU A/B (GlCensus::gpu_ab_arm): each ended scene's arm by its fence number (the drawing
// thread's EndScene), the busy samples by the arm of the scene the GPU is on (the oldest one not finished: its number is
// the fence's done count + 1), the presents per arm
constexpr uint32_t kSceneArms = 64;
std::atomic<int8_t> g_scene_arm[kSceneArms];
std::atomic<uint32_t> g_ab_busy[2];
uint32_t g_ab_presents[2] = {0, 0};

}  // namespace

extern "C" {

// The program pair and the fragment default uniform buffer the GXM context was last given, by anyone (every call in the
// binary goes through these wrappers: vitaGL's and the GL backend's raw mesh draws alike); forgotten at a scene's begin and
// end. The raw mesh draws (engine gl_shader_library.cpp, weak references) skip a set that would change nothing.
const void* te_gxm_last_vertex_program = nullptr;
const void* te_gxm_last_fragment_program = nullptr;
const void* te_gxm_last_fragment_uniforms = nullptr;
// the same for the fragment texture unit 0 -- the SceGxmTexture handed over and its four control words AS
// HANDED OVER (a texture record vitaGL rewrites in place since -- a filter, an upload's new data address -- no longer
// matches them) -- and the vertex stream 0's data pointer: the raw draws' only texture unit and stream. A raw draw
// skips a set whose pointer and words (texture) or pointer (stream) the context already holds.
const void* te_gxm_last_texture0 = nullptr;
uint32_t te_gxm_last_texture0_words[4] = {0, 0, 0, 0};
const void* te_gxm_last_stream0 = nullptr;
// ... and the vertex default uniform buffer -- forgotten also when the vertex program changes (a real
// sceGxmSetVertexProgram) and when GXM reserves one (sceGxmReserveVertexDefaultUniformBuffer: vitaGL's clears and
// scissor masks): a raw mesh draw whose composed vertex uniforms equal the buffer it set last, while the context still
// holds that buffer for the same program, sets none (gl_shader_library.cpp RawSetVertexUniforms)
const void* te_gxm_last_vertex_uniforms = nullptr;

// (a scene's begin and end forget them all)
#define ForgetLastGxmSets()                                                                                \
    do {                                                                                                   \
        te_gxm_last_vertex_program = te_gxm_last_fragment_program = te_gxm_last_fragment_uniforms = nullptr; \
        te_gxm_last_vertex_uniforms = nullptr;                                                             \
        te_gxm_last_texture0 = nullptr;                                                                    \
        te_gxm_last_stream0 = nullptr;                                                                     \
    } while (0)

}  // extern "C"

namespace {

// ---- THE GXM LAST-STATE CACHE (TE_GFX_VITA_GXM_CACHE=0: off -- every call reaches GXM, as before the cache) ------
// A GXM context's state is sticky: every setter writes context state that each later draw of the scene reads until it is
// set again (vitaGL relies on it throughout: it sets the depth / stencil / cull / polygon state only when the GL state
// changes, never per draw, re-applies only the viewport and the region clip after a scene's begin (gxm.c scene_reset:
// "sceGxm will reset it"), and binds a default uniform buffer only when the uniforms are dirty -- the draws in between
// read the one bound; Vita3K's sceGxm models it so: the setters store into the context's state, the draw reads it, and
// its own setters drop a value equal to the stored one). A call that hands the context the value it already holds
// therefore changes nothing a draw reads, and is not made. The shadow below is what the context was last given -- by
// anyone: every GXM call in the binary goes through these wrappers (-Wl,--wrap: vitaGL's and the GL backend's alike) --
// kept for vitaGL's context only (gxm_context; the splash screen's and any other context pass through untouched), inside
// a scene only, and forgotten
//   - at every scene's begin and end (all of it: whatever GXM resets there, nothing is assumed to survive a scene);
//   - by the calls that change a cached value other than through its setter: sceGxmSetTwoSidedEnable (the front / back
//     state), sceGxmReserve{Vertex,Fragment}DefaultUniformBuffer and sceGxmSet{Vertex,Fragment}UniformBuffer of the
//     default container (the default uniform buffer), the shader patcher's program releases / unregistrations (a program
//     address freed may be handed out again: the program shadows);
//   - a default uniform buffer also when its program changes (a real program set): only a draw of the same program
//     re-uses the buffer it was given.
// Cached: the front / back depth function, depth write, fragment program enable, stencil function (all six arguments),
// stencil reference, polygon mode, depth bias; the cull mode; the region clip (all five arguments); the vertex and the
// fragment program; the default uniform buffers; the vertex streams' data pointers (16); the fragment textures (16) --
// a texture by its SceGxmTexture's address AND its four control words as handed over (vitaGL rewrites a texture record in
// place: its filters, its data address after an upload). Not cached (passed through): everything else (the viewport,
// vertex textures, visibility tests, the point / line width, the non-default uniform buffers).
int g_gxm_cache = -1;   // -1 not read yet, 0 off, 1 on

constexpr uint32_t kGxmUnknown = 0xFFFFFFFFu;   // (no GXM enum value or region coordinate is all ones)
struct GxmShadow {
    bool in_scene = false;                   // vitaGL's context is inside a scene the wrappers saw begin (and the cache is on)
    uint32_t depth_func[2], depth_write[2], fp_enable[2], stencil_ref[2], polygon[2];   // [0] front, [1] back
    uint32_t stencil[2][6];                  // func, stencil fail, depth fail, depth pass, compare mask, write mask
    uint32_t bias[2][2];                     // factor, units
    uint32_t cull;
    uint32_t clip[5];                        // mode, x min, y min, x max, y max
    const void* vprog;                       // null: unknown (a null handed over is never skipped)
    const void* fprog;
    const void* vuni;                        // the default uniform buffers
    const void* funi;
    const void* stream[16];                  // SCE_GXM_MAX_VERTEX_STREAMS
    const void* tex[16];                     // SCE_GXM_MAX_TEXTURE_UNITS
    uint32_t tex_words[16][4];
};
GxmShadow g_gxm;
// the calls the cache did not make, by census category (the made ones are g_gxm_calls)
uint32_t g_gxm_skipped[kGxmCategories];
// THE DEPTH / STENCIL / RASTER STATE COALESCED UP TO THE NEXT DRAW: inside a scene of vitaGL's context a state
// setter only records the value asked (g_want) -- GXM's context state is read by draws alone (a set between two draws is
// what the second draw and the later ones see, nothing else reads it) -- and the next draw (sceGxmDraw /
// sceGxmDrawInstanced, vitaGL's internal mask and clear quads included) first makes, for every slot whose value asked
// differs from the context's (g_gxm), the one call that sets it; a scene's end makes the pending ones first (no set ever
// crosses a scene's end or begin). vitaGL's GL calls set the state piecewise and immediately (a glStencilFunc re-sends the
// whole stencil function, a glEnable(GL_DEPTH_TEST) the depth function and both write modes, every stencil draw of the GL
// backend enables and then disables the stencil test around itself): the values in between two draws, and a value asked
// and asked back before the next draw, never reach GXM. The calls asked are counted (g_state_asked); made = what GXM got.
GxmShadow g_want;
uint32_t g_state_dirty = 0;    // bit per slot (kSlot*): the value asked is not the context's
uint32_t g_state_asked = 0;    // the state setter calls asked this window (made + not made)
enum StateSlot {
    kSlotDepthFuncF, kSlotDepthFuncB, kSlotDepthWriteF, kSlotDepthWriteB, kSlotFpEnableF, kSlotFpEnableB, kSlotStencilRefF,
    kSlotStencilRefB, kSlotPolygonF, kSlotPolygonB, kSlotCull, kSlotStencilF, kSlotStencilB, kSlotBiasF, kSlotBiasB, kSlotClip,
    kSlots
};

void ForgetGxmState(GxmShadow& s) {
    for (int f = 0; f < 2; ++f) {
        s.depth_func[f] = s.depth_write[f] = s.fp_enable[f] = s.stencil_ref[f] = s.polygon[f] = kGxmUnknown;
        for (uint32_t& w : s.stencil[f]) w = kGxmUnknown;
        s.bias[f][0] = s.bias[f][1] = kGxmUnknown;
    }
    s.cull = kGxmUnknown;
    for (uint32_t& w : s.clip) w = kGxmUnknown;
}
void ForgetGxmShadow(GxmShadow& s) {
    ForgetGxmState(s);
    s.vprog = s.fprog = s.vuni = s.funi = nullptr;
    for (int i = 0; i < 16; ++i) s.stream[i] = s.tex[i] = nullptr;
}
// the context's state and the values asked both unknown, nothing pending (a scene's begin / end, after FlushGxmState)
void ForgetGxmAll() {
    ForgetGxmShadow(g_gxm);
    ForgetGxmState(g_want);
    g_state_dirty = 0;
}

bool GxmCacheOn() {
    if (g_gxm_cache < 0) {
        // TE_GFX_VITA_GXM_CACHE: unset / 1 = the cache and the raw mesh draws' vertex uniforms re-used (gl_shader_library.cpp);
        // 0 = neither (the calls before the cache); wrappers = the cache alone; reuse = the re-use alone (diagnosis)
        const char* e = std::getenv("TE_GFX_VITA_GXM_CACHE");
        g_gxm_cache = (e && (e[0] == '0' || e[0] == 'r')) ? 0 : 1;
        std::fprintf(stderr, "[te.gfx.vita] gxm last-state cache (TE_GFX_VITA_GXM_CACHE): %s\n",
                     g_gxm_cache ? "on (a GXM set that hands vitaGL's context the value it already holds in the scene is not made)"
                                 : "off (every GXM set is made)");
    }
    return g_gxm_cache > 0;
}
// vitaGL's context (null when the vitaGL in the link has no such symbol: nothing is cached)
inline SceGxmContext* VglContext() { return &gxm_context ? gxm_context : nullptr; }
// the cache answers for this call: vitaGL's context, inside a scene (in_scene is only ever set with the cache on)
inline bool Cached(SceGxmContext* c) { return g_gxm.in_scene && c && c == VglContext(); }

}  // namespace

extern "C" {

// the depth / stencil / raster values asked and not made yet, made (the state setters' block below)
void FlushGxmState(SceGxmContext* c);

int __real_sceGxmBeginScene(SceGxmContext* context, unsigned int flags, const SceGxmRenderTarget* renderTarget,
                            const SceGxmValidRegion* validRegion, SceGxmSyncObject* vertexSyncObject,
                            SceGxmSyncObject* fragmentSyncObject, const SceGxmColorSurface* colorSurface,
                            const SceGxmDepthStencilSurface* depthStencil);
int __wrap_sceGxmBeginScene(SceGxmContext* context, unsigned int flags, const SceGxmRenderTarget* renderTarget,
                            const SceGxmValidRegion* validRegion, SceGxmSyncObject* vertexSyncObject,
                            SceGxmSyncObject* fragmentSyncObject, const SceGxmColorSurface* colorSurface,
                            const SceGxmDepthStencilSurface* depthStencil) {
    const uint64_t t0 = NowUs();
    ForgetLastGxmSets();
    const bool vgl = context && context == VglContext();
    if (vgl) {
        // (a begin inside a scene is GXM's error; the values asked are made first all the same, as they would have been)
        if (g_gxm.in_scene && g_state_dirty) FlushGxmState(context);
        ForgetGxmAll();
        g_gxm.in_scene = false;
    }
    const int r = __real_sceGxmBeginScene(context, flags, renderTarget, validRegion, vertexSyncObject, fragmentSyncObject,
                                          colorSurface, depthStencil);
    // (the cache opens with the scene: every value unknown, so each first set of the scene is made)
    if (vgl && r == 0 && GxmCacheOn()) g_gxm.in_scene = true;
    g_begin_scene.Note(NowUs() - t0);
    return r;
}

int __real_sceGxmEndScene(SceGxmContext* context, const SceGxmNotification* vertexNotification,
                          const SceGxmNotification* fragmentNotification);
int __wrap_sceGxmEndScene(SceGxmContext* context, const SceGxmNotification* vertexNotification,
                          const SceGxmNotification* fragmentNotification) {
    const uint64_t t0 = NowUs();
    ForgetLastGxmSets();
    if (context && context == VglContext()) {
        // the values asked since the scene's last draw reach the context before its end (no set crosses a scene's end)
        if (g_gxm.in_scene && g_state_dirty) FlushGxmState(context);
        ForgetGxmAll();
        g_gxm.in_scene = false;
    }
    // the per-present GPU A/B: this scene's arm + 1 (0: none; vitaGL's scene_end counted the scene in query_fence.value
    // right before the call)
    if (&query_fence && fragmentNotification == &query_fence)
        g_scene_arm[query_fence.value & (kSceneArms - 1)].store(int8_t(ps2r::render::Census().gpu_ab_arm + 1), std::memory_order_relaxed);
    const int r = __real_sceGxmEndScene(context, vertexNotification, fragmentNotification);
    g_end_scene.Note(NowUs() - t0);
    return r;
}

int __real_sceGxmDisplayQueueAddEntry(SceGxmSyncObject* oldBuffer, SceGxmSyncObject* newBuffer, const void* callbackData);
int __wrap_sceGxmDisplayQueueAddEntry(SceGxmSyncObject* oldBuffer, SceGxmSyncObject* newBuffer, const void* callbackData) {
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmDisplayQueueAddEntry(oldBuffer, newBuffer, callbackData);
    g_display_queue.Note(NowUs() - t0);
    return r;
}

void __real_sceGxmFinish(SceGxmContext* context);
void __wrap_sceGxmFinish(SceGxmContext* context) {
    const uint64_t t0 = NowUs();
    __real_sceGxmFinish(context);
    g_finish.Note(NowUs() - t0);
}

int __real_sceGxmNotificationWait(const SceGxmNotification* notification);
int __wrap_sceGxmNotificationWait(const SceGxmNotification* notification) {
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmNotificationWait(notification);
    g_notification_wait.Note(NowUs() - t0);
    return r;
}

int __real_sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher* shaderPatcher, SceGxmShaderPatcherId programId,
                                                    SceGxmOutputRegisterFormat outputFormat,
                                                    SceGxmMultisampleMode multisampleMode, const SceGxmBlendInfo* blendInfo,
                                                    const SceGxmProgram* vertexProgram,
                                                    SceGxmFragmentProgram** fragmentProgram);
int __wrap_sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher* shaderPatcher, SceGxmShaderPatcherId programId,
                                                    SceGxmOutputRegisterFormat outputFormat,
                                                    SceGxmMultisampleMode multisampleMode, const SceGxmBlendInfo* blendInfo,
                                                    const SceGxmProgram* vertexProgram,
                                                    SceGxmFragmentProgram** fragmentProgram) {
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmShaderPatcherCreateFragmentProgram(shaderPatcher, programId, outputFormat, multisampleMode,
                                                                  blendInfo, vertexProgram, fragmentProgram);
    g_patch_fragment.Note(NowUs() - t0);
    return r;
}

int __real_sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher* shaderPatcher, SceGxmShaderPatcherId programId,
                                                  const SceGxmVertexAttribute* attributes, unsigned int attributeCount,
                                                  const SceGxmVertexStream* streams, unsigned int streamCount,
                                                  SceGxmVertexProgram** vertexProgram);
int __wrap_sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher* shaderPatcher, SceGxmShaderPatcherId programId,
                                                  const SceGxmVertexAttribute* attributes, unsigned int attributeCount,
                                                  const SceGxmVertexStream* streams, unsigned int streamCount,
                                                  SceGxmVertexProgram** vertexProgram) {
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmShaderPatcherCreateVertexProgram(shaderPatcher, programId, attributes, attributeCount, streams,
                                                                streamCount, vertexProgram);
    g_patch_vertex.Note(NowUs() - t0);
    return r;
}

int __real_sceGxmDraw(SceGxmContext* context, SceGxmPrimitiveType primType, SceGxmIndexFormat indexType,
                      const void* indexData, unsigned int indexCount);
int __wrap_sceGxmDraw(SceGxmContext* context, SceGxmPrimitiveType primType, SceGxmIndexFormat indexType,
                      const void* indexData, unsigned int indexCount) {
    static uint32_t n = 0;   // (the drawing thread's: vitaGL draws from one thread)
    if (g_state_dirty && Cached(context)) FlushGxmState(context);   // (the draw's own state first)
    g_gxm_calls[kGxmDraws]++;
    if (++n % Timed::kSample) return __real_sceGxmDraw(context, primType, indexType, indexData, indexCount);
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmDraw(context, primType, indexType, indexData, indexCount);
    g_draw.NoteSampled(NowUs() - t0);
    return r;
}

// Every other GXM call vitaGL (and the GL backend) makes per draw or per state change, counted -- made (g_gxm_calls: the
// per-frame GXM call census, "gxm calls per frame") or, with the last-state cache, not made (g_gxm_skipped: the call
// would have handed vitaGL's context, inside its scene, the value it already holds)
#define TE_GXM_MADE(cat) (g_gxm_calls[cat]++)
#define TE_GXM_SKIPPED(cat) (g_gxm_skipped[cat]++)
int __real_sceGxmDrawInstanced(SceGxmContext* c, SceGxmPrimitiveType p, SceGxmIndexFormat t, const void* d, unsigned int n,
                               unsigned int w);
int __wrap_sceGxmDrawInstanced(SceGxmContext* c, SceGxmPrimitiveType p, SceGxmIndexFormat t, const void* d, unsigned int n,
                               unsigned int w) {
    if (g_state_dirty && Cached(c)) FlushGxmState(c);   // (the draw's own state first)
    TE_GXM_MADE(kGxmInstanced);
    return __real_sceGxmDrawInstanced(c, p, t, d, n, w);
}
int __real_sceGxmSetVertexStream(SceGxmContext* c, unsigned int i, const void* d);
int __wrap_sceGxmSetVertexStream(SceGxmContext* c, unsigned int i, const void* d) {
    const bool cached = Cached(c) && i < 16;
    if (cached && d && g_gxm.stream[i] == d) {
        TE_GXM_SKIPPED(kGxmStreams);
        if (i == 0) te_gxm_last_stream0 = d;
        return 0;
    }
    TE_GXM_MADE(kGxmStreams);
    const int r = __real_sceGxmSetVertexStream(c, i, d);
    if (i == 0) te_gxm_last_stream0 = r == 0 ? d : nullptr;
    if (cached) g_gxm.stream[i] = r == 0 ? d : nullptr;
    return r;
}
int __real_sceGxmSetFragmentTexture(SceGxmContext* c, unsigned int i, const SceGxmTexture* t);
int __wrap_sceGxmSetFragmentTexture(SceGxmContext* c, unsigned int i, const SceGxmTexture* t) {
    const bool cached = Cached(c) && i < 16;
    // the same record with the same four words as the context was given: the texture the draw samples is the same
    if (cached && t && g_gxm.tex[i] == t && std::memcmp(g_gxm.tex_words[i], t, sizeof g_gxm.tex_words[i]) == 0) {
        TE_GXM_SKIPPED(kGxmTextures);
        if (i == 0) {
            te_gxm_last_texture0 = t;
            std::memcpy(te_gxm_last_texture0_words, t, sizeof te_gxm_last_texture0_words);
        }
        return 0;
    }
    TE_GXM_MADE(kGxmTextures);
    const int r = __real_sceGxmSetFragmentTexture(c, i, t);
    if (i == 0) {
        if (r == 0 && t) {
            te_gxm_last_texture0 = t;
            std::memcpy(te_gxm_last_texture0_words, t, sizeof te_gxm_last_texture0_words);
        } else {
            te_gxm_last_texture0 = nullptr;
        }
    }
    if (cached) {
        if (r == 0 && t) {
            g_gxm.tex[i] = t;
            std::memcpy(g_gxm.tex_words[i], t, sizeof g_gxm.tex_words[i]);
        } else {
            g_gxm.tex[i] = nullptr;
        }
    }
    return r;
}
void __real_sceGxmSetVertexProgram(SceGxmContext* c, const SceGxmVertexProgram* p);
void __wrap_sceGxmSetVertexProgram(SceGxmContext* c, const SceGxmVertexProgram* p) {
    const bool cached = Cached(c);
    if (cached && p && g_gxm.vprog == p) {
        TE_GXM_SKIPPED(kGxmPrograms);
        te_gxm_last_vertex_program = p;
        return;
    }
    TE_GXM_MADE(kGxmPrograms);
    // (another program: its default uniform buffer is the one set after this -- the raw draws' buffer is not re-used
    // across a program change, nor is any buffer the shadow holds)
    if (te_gxm_last_vertex_program != p) te_gxm_last_vertex_uniforms = nullptr;
    te_gxm_last_vertex_program = p;
    __real_sceGxmSetVertexProgram(c, p);
    if (cached) {
        if (g_gxm.vprog != p) g_gxm.vuni = nullptr;
        g_gxm.vprog = p;
    }
}
void __real_sceGxmSetFragmentProgram(SceGxmContext* c, const SceGxmFragmentProgram* p);
void __wrap_sceGxmSetFragmentProgram(SceGxmContext* c, const SceGxmFragmentProgram* p) {
    const bool cached = Cached(c);
    if (cached && p && g_gxm.fprog == p) {
        TE_GXM_SKIPPED(kGxmPrograms);
        te_gxm_last_fragment_program = p;
        return;
    }
    TE_GXM_MADE(kGxmPrograms);
    te_gxm_last_fragment_program = p;
    __real_sceGxmSetFragmentProgram(c, p);
    if (cached) {
        if (g_gxm.fprog != p) g_gxm.funi = nullptr;
        g_gxm.fprog = p;
    }
}
int __real_sceGxmSetVertexDefaultUniformBuffer(SceGxmContext* c, const void* b);
int __wrap_sceGxmSetVertexDefaultUniformBuffer(SceGxmContext* c, const void* b) {
    const bool cached = Cached(c);
    if (cached && b && g_gxm.vuni == b) {
        TE_GXM_SKIPPED(kGxmUniforms);
        te_gxm_last_vertex_uniforms = b;
        return 0;
    }
    TE_GXM_MADE(kGxmUniforms);
    const int r = __real_sceGxmSetVertexDefaultUniformBuffer(c, b);
    te_gxm_last_vertex_uniforms = r == 0 ? b : nullptr;
    if (cached) g_gxm.vuni = r == 0 ? b : nullptr;
    return r;
}
int __real_sceGxmSetFragmentDefaultUniformBuffer(SceGxmContext* c, const void* b);
int __wrap_sceGxmSetFragmentDefaultUniformBuffer(SceGxmContext* c, const void* b) {
    const bool cached = Cached(c);
    if (cached && b && g_gxm.funi == b) {
        TE_GXM_SKIPPED(kGxmUniforms);
        te_gxm_last_fragment_uniforms = b;
        return 0;
    }
    TE_GXM_MADE(kGxmUniforms);
    te_gxm_last_fragment_uniforms = b;
    const int r = __real_sceGxmSetFragmentDefaultUniformBuffer(c, b);
    if (cached) g_gxm.funi = r == 0 ? b : nullptr;
    return r;
}
// the calls that change a cached value other than through its own setter -- not counted (they are no per-draw
// calls), each the call unchanged: a default uniform buffer GXM reserves (vitaGL's clears and scissor masks) or a uniform
// buffer set in the default container; the two-sided mode (which face state a primitive reads); the shader patcher's
// program releases (an address it may hand out again)
int __real_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext* c, void** b);
int __wrap_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext* c, void** b) {
    const int r = __real_sceGxmReserveVertexDefaultUniformBuffer(c, b);
    te_gxm_last_vertex_uniforms = nullptr;
    if (c && c == VglContext()) g_gxm.vuni = nullptr;
    return r;
}
int __real_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext* c, void** b);
int __wrap_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext* c, void** b) {
    const int r = __real_sceGxmReserveFragmentDefaultUniformBuffer(c, b);
    // (also the raw draws' record: the context holds GXM's buffer now, not the last raw draw's -- until vitaGL restores
    // its own through sceGxmSetFragmentDefaultUniformBuffer)
    te_gxm_last_fragment_uniforms = nullptr;
    if (c && c == VglContext()) g_gxm.funi = nullptr;
    return r;
}
constexpr unsigned kDefaultUniformContainer = 14;   // SCE_GXM_DEFAULT_UNIFORM_BUFFER_CONTAINER_INDEX
int __real_sceGxmSetVertexUniformBuffer(SceGxmContext* c, unsigned int i, const void* d);
int __wrap_sceGxmSetVertexUniformBuffer(SceGxmContext* c, unsigned int i, const void* d) {
    const int r = __real_sceGxmSetVertexUniformBuffer(c, i, d);
    if (i == kDefaultUniformContainer) {
        te_gxm_last_vertex_uniforms = nullptr;
        if (c && c == VglContext()) g_gxm.vuni = nullptr;
    }
    return r;
}
int __real_sceGxmSetFragmentUniformBuffer(SceGxmContext* c, unsigned int i, const void* d);
int __wrap_sceGxmSetFragmentUniformBuffer(SceGxmContext* c, unsigned int i, const void* d) {
    const int r = __real_sceGxmSetFragmentUniformBuffer(c, i, d);
    if (i == kDefaultUniformContainer) {
        te_gxm_last_fragment_uniforms = nullptr;
        if (c && c == VglContext()) g_gxm.funi = nullptr;
    }
    return r;
}
void __real_sceGxmSetTwoSidedEnable(SceGxmContext* c, SceGxmTwoSidedMode m);
void __wrap_sceGxmSetTwoSidedEnable(SceGxmContext* c, SceGxmTwoSidedMode m) {
    // (the values asked before it reach the context before it, as they would have; then neither the context's front / back
    // state nor the values asked are assumed)
    if (g_state_dirty && Cached(c)) FlushGxmState(c);
    __real_sceGxmSetTwoSidedEnable(c, m);
    if (c && c == VglContext()) {
        ForgetGxmState(g_gxm);
        ForgetGxmState(g_want);
        g_state_dirty = 0;
    }
}
// a released / unregistered program's address: neither the shadow nor the raw draws' record may stand for it any more
#define TE_GXM_FORGET_PROGRAMS()                                              \
    do {                                                                      \
        te_gxm_last_vertex_program = te_gxm_last_fragment_program = nullptr;  \
        te_gxm_last_vertex_uniforms = nullptr;                                \
        g_gxm.vprog = g_gxm.fprog = g_gxm.vuni = g_gxm.funi = nullptr;        \
    } while (0)
int __real_sceGxmShaderPatcherReleaseVertexProgram(SceGxmShaderPatcher* s, SceGxmVertexProgram* p);
int __wrap_sceGxmShaderPatcherReleaseVertexProgram(SceGxmShaderPatcher* s, SceGxmVertexProgram* p) {
    TE_GXM_FORGET_PROGRAMS();
    return __real_sceGxmShaderPatcherReleaseVertexProgram(s, p);
}
int __real_sceGxmShaderPatcherReleaseFragmentProgram(SceGxmShaderPatcher* s, SceGxmFragmentProgram* p);
int __wrap_sceGxmShaderPatcherReleaseFragmentProgram(SceGxmShaderPatcher* s, SceGxmFragmentProgram* p) {
    TE_GXM_FORGET_PROGRAMS();
    return __real_sceGxmShaderPatcherReleaseFragmentProgram(s, p);
}
int __real_sceGxmShaderPatcherUnregisterProgram(SceGxmShaderPatcher* s, SceGxmShaderPatcherId id);
int __wrap_sceGxmShaderPatcherUnregisterProgram(SceGxmShaderPatcher* s, SceGxmShaderPatcherId id) {
    TE_GXM_FORGET_PROGRAMS();
    return __real_sceGxmShaderPatcherUnregisterProgram(s, id);
}
int __real_sceGxmShaderPatcherForceUnregisterProgram(SceGxmShaderPatcher* s, SceGxmShaderPatcherId id);
int __wrap_sceGxmShaderPatcherForceUnregisterProgram(SceGxmShaderPatcher* s, SceGxmShaderPatcherId id) {
    TE_GXM_FORGET_PROGRAMS();
    return __real_sceGxmShaderPatcherForceUnregisterProgram(s, id);
}
#undef TE_GXM_FORGET_PROGRAMS
// the depth / stencil / raster setters (coalesced up to the next draw -- see g_want): inside a scene of vitaGL's context
// the value asked is recorded and its slot marked pending when it is not the context's; FlushGxmState (the next draw, the
// scene's end) makes the calls. Elsewhere (another context, outside a scene, the cache off) the call is made at once.
#define TE_GXM_STATE1(name, T, field, bit)                       \
    void __real_##name(SceGxmContext* c, T v);                   \
    void __wrap_##name(SceGxmContext* c, T v) {                  \
        g_state_asked++;                                         \
        if (Cached(c)) {                                         \
            g_want.field = uint32_t(v);                          \
            if (g_gxm.field == uint32_t(v)) g_state_dirty &= ~(1u << (bit)); \
            else g_state_dirty |= 1u << (bit);                   \
            return;                                              \
        }                                                        \
        TE_GXM_MADE(kGxmState);                                  \
        __real_##name(c, v);                                     \
    }
TE_GXM_STATE1(sceGxmSetFrontDepthFunc, SceGxmDepthFunc, depth_func[0], kSlotDepthFuncF)
TE_GXM_STATE1(sceGxmSetBackDepthFunc, SceGxmDepthFunc, depth_func[1], kSlotDepthFuncB)
TE_GXM_STATE1(sceGxmSetFrontDepthWriteEnable, SceGxmDepthWriteMode, depth_write[0], kSlotDepthWriteF)
TE_GXM_STATE1(sceGxmSetBackDepthWriteEnable, SceGxmDepthWriteMode, depth_write[1], kSlotDepthWriteB)
TE_GXM_STATE1(sceGxmSetFrontFragmentProgramEnable, SceGxmFragmentProgramMode, fp_enable[0], kSlotFpEnableF)
TE_GXM_STATE1(sceGxmSetBackFragmentProgramEnable, SceGxmFragmentProgramMode, fp_enable[1], kSlotFpEnableB)
TE_GXM_STATE1(sceGxmSetFrontStencilRef, unsigned int, stencil_ref[0], kSlotStencilRefF)
TE_GXM_STATE1(sceGxmSetBackStencilRef, unsigned int, stencil_ref[1], kSlotStencilRefB)
TE_GXM_STATE1(sceGxmSetFrontPolygonMode, SceGxmPolygonMode, polygon[0], kSlotPolygonF)
TE_GXM_STATE1(sceGxmSetBackPolygonMode, SceGxmPolygonMode, polygon[1], kSlotPolygonB)
TE_GXM_STATE1(sceGxmSetCullMode, SceGxmCullMode, cull, kSlotCull)
#undef TE_GXM_STATE1
// a stencil function's six arguments, all of them the context's: not set again
inline bool SameStencil(const uint32_t* s, SceGxmStencilFunc f, SceGxmStencilOp a, SceGxmStencilOp b, SceGxmStencilOp d,
                        unsigned char cm, unsigned char wm) {
    return s[0] == uint32_t(f) && s[1] == uint32_t(a) && s[2] == uint32_t(b) && s[3] == uint32_t(d) && s[4] == uint32_t(cm) &&
           s[5] == uint32_t(wm);
}
inline void PutStencil(uint32_t* s, SceGxmStencilFunc f, SceGxmStencilOp a, SceGxmStencilOp b, SceGxmStencilOp d,
                       unsigned char cm, unsigned char wm) {
    s[0] = uint32_t(f);
    s[1] = uint32_t(a);
    s[2] = uint32_t(b);
    s[3] = uint32_t(d);
    s[4] = uint32_t(cm);
    s[5] = uint32_t(wm);
}
inline void MarkSlot(bool same, int bit) {
    if (same) g_state_dirty &= ~(1u << bit);
    else g_state_dirty |= 1u << bit;
}
void __real_sceGxmSetFrontStencilFunc(SceGxmContext* c, SceGxmStencilFunc f, SceGxmStencilOp a, SceGxmStencilOp b, SceGxmStencilOp d,
                                      unsigned char cm, unsigned char wm);
void __wrap_sceGxmSetFrontStencilFunc(SceGxmContext* c, SceGxmStencilFunc f, SceGxmStencilOp a, SceGxmStencilOp b, SceGxmStencilOp d,
                                      unsigned char cm, unsigned char wm) {
    g_state_asked++;
    if (Cached(c)) {
        PutStencil(g_want.stencil[0], f, a, b, d, cm, wm);
        MarkSlot(SameStencil(g_gxm.stencil[0], f, a, b, d, cm, wm), kSlotStencilF);
        return;
    }
    TE_GXM_MADE(kGxmState);
    __real_sceGxmSetFrontStencilFunc(c, f, a, b, d, cm, wm);
}
void __real_sceGxmSetBackStencilFunc(SceGxmContext* c, SceGxmStencilFunc f, SceGxmStencilOp a, SceGxmStencilOp b, SceGxmStencilOp d,
                                     unsigned char cm, unsigned char wm);
void __wrap_sceGxmSetBackStencilFunc(SceGxmContext* c, SceGxmStencilFunc f, SceGxmStencilOp a, SceGxmStencilOp b, SceGxmStencilOp d,
                                     unsigned char cm, unsigned char wm) {
    g_state_asked++;
    if (Cached(c)) {
        PutStencil(g_want.stencil[1], f, a, b, d, cm, wm);
        MarkSlot(SameStencil(g_gxm.stencil[1], f, a, b, d, cm, wm), kSlotStencilB);
        return;
    }
    TE_GXM_MADE(kGxmState);
    __real_sceGxmSetBackStencilFunc(c, f, a, b, d, cm, wm);
}
void __real_sceGxmSetFrontDepthBias(SceGxmContext* c, int f, int u);
void __wrap_sceGxmSetFrontDepthBias(SceGxmContext* c, int f, int u) {
    g_state_asked++;
    if (Cached(c)) {
        g_want.bias[0][0] = uint32_t(f);
        g_want.bias[0][1] = uint32_t(u);
        MarkSlot(g_gxm.bias[0][0] == uint32_t(f) && g_gxm.bias[0][1] == uint32_t(u), kSlotBiasF);
        return;
    }
    TE_GXM_MADE(kGxmState);
    __real_sceGxmSetFrontDepthBias(c, f, u);
}
void __real_sceGxmSetBackDepthBias(SceGxmContext* c, int f, int u);
void __wrap_sceGxmSetBackDepthBias(SceGxmContext* c, int f, int u) {
    g_state_asked++;
    if (Cached(c)) {
        g_want.bias[1][0] = uint32_t(f);
        g_want.bias[1][1] = uint32_t(u);
        MarkSlot(g_gxm.bias[1][0] == uint32_t(f) && g_gxm.bias[1][1] == uint32_t(u), kSlotBiasB);
        return;
    }
    TE_GXM_MADE(kGxmState);
    __real_sceGxmSetBackDepthBias(c, f, u);
}
void __real_sceGxmSetRegionClip(SceGxmContext* c, SceGxmRegionClipMode m, unsigned int x0, unsigned int y0, unsigned int x1,
                                unsigned int y1);
void __wrap_sceGxmSetRegionClip(SceGxmContext* c, SceGxmRegionClipMode m, unsigned int x0, unsigned int y0, unsigned int x1,
                                unsigned int y1) {
    g_state_asked++;
    if (Cached(c)) {
        uint32_t* w = g_want.clip;
        w[0] = uint32_t(m);
        w[1] = x0;
        w[2] = y0;
        w[3] = x1;
        w[4] = y1;
        const uint32_t* k = g_gxm.clip;
        MarkSlot(k[0] == w[0] && k[1] == x0 && k[2] == y0 && k[3] == x1 && k[4] == y1, kSlotClip);
        return;
    }
    TE_GXM_MADE(kGxmState);
    __real_sceGxmSetRegionClip(c, m, x0, y0, x1, y1);
}
// The pending slots' calls, each with the value asked last (the next draw's state); the context then holds them
void FlushGxmState(SceGxmContext* c) {
    uint32_t d = g_state_dirty;
    g_state_dirty = 0;
    const GxmShadow& w = g_want;
    GxmShadow& h = g_gxm;
    while (d) {
        const int bit = __builtin_ctz(d);
        d &= d - 1;
        TE_GXM_MADE(kGxmState);
        switch (bit) {
            case kSlotDepthFuncF: __real_sceGxmSetFrontDepthFunc(c, SceGxmDepthFunc(w.depth_func[0])); h.depth_func[0] = w.depth_func[0]; break;
            case kSlotDepthFuncB: __real_sceGxmSetBackDepthFunc(c, SceGxmDepthFunc(w.depth_func[1])); h.depth_func[1] = w.depth_func[1]; break;
            case kSlotDepthWriteF:
                __real_sceGxmSetFrontDepthWriteEnable(c, SceGxmDepthWriteMode(w.depth_write[0]));
                h.depth_write[0] = w.depth_write[0];
                break;
            case kSlotDepthWriteB:
                __real_sceGxmSetBackDepthWriteEnable(c, SceGxmDepthWriteMode(w.depth_write[1]));
                h.depth_write[1] = w.depth_write[1];
                break;
            case kSlotFpEnableF:
                __real_sceGxmSetFrontFragmentProgramEnable(c, SceGxmFragmentProgramMode(w.fp_enable[0]));
                h.fp_enable[0] = w.fp_enable[0];
                break;
            case kSlotFpEnableB:
                __real_sceGxmSetBackFragmentProgramEnable(c, SceGxmFragmentProgramMode(w.fp_enable[1]));
                h.fp_enable[1] = w.fp_enable[1];
                break;
            case kSlotStencilRefF: __real_sceGxmSetFrontStencilRef(c, w.stencil_ref[0]); h.stencil_ref[0] = w.stencil_ref[0]; break;
            case kSlotStencilRefB: __real_sceGxmSetBackStencilRef(c, w.stencil_ref[1]); h.stencil_ref[1] = w.stencil_ref[1]; break;
            case kSlotPolygonF: __real_sceGxmSetFrontPolygonMode(c, SceGxmPolygonMode(w.polygon[0])); h.polygon[0] = w.polygon[0]; break;
            case kSlotPolygonB: __real_sceGxmSetBackPolygonMode(c, SceGxmPolygonMode(w.polygon[1])); h.polygon[1] = w.polygon[1]; break;
            case kSlotCull: __real_sceGxmSetCullMode(c, SceGxmCullMode(w.cull)); h.cull = w.cull; break;
            case kSlotStencilF:
            case kSlotStencilB: {
                const int f = bit == kSlotStencilF ? 0 : 1;
                const uint32_t* v = w.stencil[f];
                if (f == 0)
                    __real_sceGxmSetFrontStencilFunc(c, SceGxmStencilFunc(v[0]), SceGxmStencilOp(v[1]), SceGxmStencilOp(v[2]),
                                                     SceGxmStencilOp(v[3]), (unsigned char)v[4], (unsigned char)v[5]);
                else
                    __real_sceGxmSetBackStencilFunc(c, SceGxmStencilFunc(v[0]), SceGxmStencilOp(v[1]), SceGxmStencilOp(v[2]),
                                                    SceGxmStencilOp(v[3]), (unsigned char)v[4], (unsigned char)v[5]);
                std::memcpy(h.stencil[f], v, sizeof h.stencil[f]);
                break;
            }
            case kSlotBiasF: __real_sceGxmSetFrontDepthBias(c, int(w.bias[0][0]), int(w.bias[0][1])); std::memcpy(h.bias[0], w.bias[0], sizeof h.bias[0]); break;
            case kSlotBiasB: __real_sceGxmSetBackDepthBias(c, int(w.bias[1][0]), int(w.bias[1][1])); std::memcpy(h.bias[1], w.bias[1], sizeof h.bias[1]); break;
            case kSlotClip:
                __real_sceGxmSetRegionClip(c, SceGxmRegionClipMode(w.clip[0]), w.clip[1], w.clip[2], w.clip[3], w.clip[4]);
                std::memcpy(h.clip, w.clip, sizeof h.clip);
                break;
            default: break;
        }
    }
}
#undef TE_GXM_MADE
#undef TE_GXM_SKIPPED

// vitaGL's draw entry point (called by the GL backend: its references are the wrapped ones)
void __real_glDrawArrays(unsigned int mode, int first, int count);
void __wrap_glDrawArrays(unsigned int mode, int first, int count) {
    static uint32_t n = 0;
    if (++n % Timed::kSample) { __real_glDrawArrays(mode, first, count); return; }
    const uint64_t t0 = NowUs();
    __real_glDrawArrays(mode, first, count);
    g_gl_draw_arrays.NoteSampled(NowUs() - t0);
}

// vitaGL's pool allocator (mem_utils.c; the gpu_alloc_mapped_* fallback chains call it once per pool tried)
void* __real_vgl_memalign(size_t alignment, size_t size, int type);
void* __wrap_vgl_memalign(size_t alignment, size_t size, int type) {
    const uint64_t t0 = NowUs();
    void* r = __real_vgl_memalign(alignment, size, type);
    const uint64_t d = NowUs() - t0;
    g_alloc_us.fetch_add(d, std::memory_order_relaxed);
    g_alloc_us_ever.fetch_add(d, std::memory_order_relaxed);
    if (d > g_alloc_max_us.load(std::memory_order_relaxed)) g_alloc_max_us.store(uint32_t(std::min<uint64_t>(d, 0xFFFFFFFFu)), std::memory_order_relaxed);
    if (type >= 0 && type < kPools) {
        if (r) {
            g_alloc_ok[type].fetch_add(1, std::memory_order_relaxed);
            g_alloc_bytes[type].fetch_add(size, std::memory_order_relaxed);
            g_allocs_ever.fetch_add(1, std::memory_order_relaxed);
            g_alloc_bytes_ever.fetch_add(size, std::memory_order_relaxed);
        } else {
            g_alloc_fail[type].fetch_add(1, std::memory_order_relaxed);
            if (type == 1) g_alloc_fail_bytes.fetch_add(size, std::memory_order_relaxed);
        }
    }
    return r;
}

void __real_vgl_free(void* ptr);
void __wrap_vgl_free(void* ptr) {
    if (ptr) {
        g_frees.fetch_add(1, std::memory_order_relaxed);
        g_frees_ever.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t t0 = NowUs();
    __real_vgl_free(ptr);
    g_free_us.fetch_add(NowUs() - t0, std::memory_order_relaxed);
}

void __real_glFinish(void);
void __wrap_glFinish(void) {
    g_gl_finish.fetch_add(1, std::memory_order_relaxed);
    __real_glFinish();
}

// vitaGL's unsafe allocator sleeps a second per retry; nothing else in the tree sleeps half a second or more mid-run
int __real_sceKernelDelayThread(SceUInt delay);
int __wrap_sceKernelDelayThread(SceUInt delay) {
    if (delay >= 500000) g_long_sleeps.fetch_add(1, std::memory_order_relaxed);
    return __real_sceKernelDelayThread(delay);
}

}  // extern "C"

namespace te::vita_gpu {

void OnPresented() {
    if (g_probe_every < 0) {
        // OFF BY DEFAULT: a probe DRAINS the GPU twice (after a present, and after the measured frame's swap) --
        // on the device in the island pan 38 ms each on the drawing thread (device-run8.log "drains 3 (38.2 ms each)"),
        // pictures replaced or held long: misses for the tick lock, a visible hitch. TE_GFX_VITA_GPU_PROBE=N (or the older
        // TE_VITA_GPU_PROBE_EVERY=N) probes every N presents.
        const char* e = std::getenv("TE_GFX_VITA_GPU_PROBE");
        if (!(e && *e)) e = std::getenv("TE_VITA_GPU_PROBE_EVERY");
        g_probe_every = e && *e ? std::max(0L, std::atol(e)) : 0;
        if (g_probe_every > 0)
            std::fprintf(stderr, "[te.gfx.vita] gpu probe: every %ld presents (TE_GFX_VITA_GPU_PROBE) the GPU is drained, then the next "
                         "frame's GPU time after its swap is measured (two deliberate GPU drains a probe)\n", g_probe_every);
        else
            std::fprintf(stderr, "[te.gfx.vita] gpu probe: off (TE_GFX_VITA_GPU_PROBE=N: every N presents; each probe drains the GPU "
                         "twice); the GPU's lag at each swap is measured without a drain\n");
    }
    ++g_presents;
    {
        // the per-present GPU A/B: the frame just presented was drawn in this arm (the next frame's BeginFrame sets its own)
        const int arm = ps2r::render::Census().gpu_ab_arm;
        if (arm == 0 || arm == 1) g_ab_presents[arm]++;
    }
    if (&query_fence && query_fence.address) {
        const uint32_t issued = query_fence.value;
        const uint32_t done = *static_cast<volatile const uint32_t*>(query_fence.address);
        const uint32_t lag = issued - done;
        // this present's last scene (FramesInFlight)
        g_present_marks[g_presents % kPresentMarks] = issued;
        g_marks_valid = std::min<uint64_t>(g_marks_valid + 1, kPresentMarks);
        if (lag < 1000) {
            g_lag_sum.fetch_add(lag, std::memory_order_relaxed);
            g_lag_n.fetch_add(1, std::memory_order_relaxed);
            if (lag > g_lag_max.load(std::memory_order_relaxed)) g_lag_max.store(lag, std::memory_order_relaxed);
        }
    }
    if (g_probe_every <= 0 || !&gxm_context || !gxm_context) return;
    if (g_probe_armed) {
        // this frame was submitted onto an idle GPU: what the GPU still had to do after its swap returned
        const uint64_t t0 = NowUs();
        __real_sceGxmFinish(gxm_context);
        const uint64_t t1 = NowUs();
        const int arm = g_probe_arm;
        g_probe_n[arm].fetch_add(1, std::memory_order_relaxed);
        g_probe_tail_us[arm].fetch_add(t1 - t0, std::memory_order_relaxed);
        g_probe_submit_us[arm].fetch_add(t0 - g_probe_drained_at, std::memory_order_relaxed);
        const uint32_t tail = uint32_t(std::min<uint64_t>(t1 - t0, 0xFFFFFFFFull));
        if (tail > g_probe_tail_max_us[arm].load(std::memory_order_relaxed)) g_probe_tail_max_us[arm].store(tail, std::memory_order_relaxed);
        g_probe_armed = false;
    } else if (g_presents % uint64_t(g_probe_every) == 0) {
        // drain: the next frame starts on an idle GPU (the drain itself = the GPU work queued behind this present)
        const uint64_t t0 = NowUs();
        __real_sceGxmFinish(gxm_context);
        g_probe_drained_at = NowUs();
        g_probe_drain_us.fetch_add(g_probe_drained_at - t0, std::memory_order_relaxed);
        g_probe_drains.fetch_add(1, std::memory_order_relaxed);
        // the arm the measured (next) frame is drawn with: the A/B flips at a present, this one's already happened
        g_probe_arm = ps2r::render::Census().ps2_fast_active ? 1 : 0;
        g_probe_armed = true;
    }
}

std::string WindowLines(double frames) {
    if (frames <= 0) frames = 1;
    auto ms = [frames](uint64_t us) { return double(us) / 1000.0 / frames; };
    const Timed::Snap bs = g_begin_scene.Take(), es = g_end_scene.Take(), dq = g_display_queue.Take(),
                      fin = g_finish.Take(), nw = g_notification_wait.Take(), pf = g_patch_fragment.Take(),
                      pv = g_patch_vertex.Take(), dr = g_draw.Take(), gda = g_gl_draw_arrays.Take();
    char a[760];
    std::snprintf(a, sizeof a,
                  "[te.gfx.vita] gxm per frame (draw timings sampled 1 in 32): glDrawArrays %.1f (%.2f ms in vitaGL, max %.2f), "
                  "display queue add %.2f ms (max %.1f), end scene %.2f ms (%.1f calls, max %.1f), "
                  "begin scene %.2f ms (%.1f calls, max %.1f), draws %.1f (%.2f ms, max %.1f), patcher fragment %.2f (%.2f ms) "
                  "vertex %.2f (%.2f ms), finish %u (%.1f ms), notification waits %u (%.1f ms)",
                  gda.calls / frames, ms(gda.us), gda.max_us / 1000.0, ms(dq.us), dq.max_us / 1000.0, ms(es.us), es.calls / frames, es.max_us / 1000.0, ms(bs.us), bs.calls / frames,
                  bs.max_us / 1000.0, dr.calls / frames, ms(dr.us), dr.max_us / 1000.0, pf.calls / frames, ms(pf.us),
                  pv.calls / frames, ms(pv.us), fin.calls, fin.us / 1000.0, nw.calls, nw.us / 1000.0);
    const uint32_t lag_n = g_lag_n.exchange(0, std::memory_order_relaxed);
    const uint64_t lag_sum = g_lag_sum.exchange(0, std::memory_order_relaxed);
    const uint32_t lag_max = g_lag_max.exchange(0, std::memory_order_relaxed);
    const uint64_t pdrain = g_probe_drain_us.exchange(0, std::memory_order_relaxed);
    const uint32_t pdrains = g_probe_drains.exchange(0, std::memory_order_relaxed);
    std::string b;
    {
        char head[360];
        // the busy share (sampled by the profiler's thread; no sample without TE_PROF=1) over the window's wall time
        const uint64_t now_us = NowUs();
        const double window_s = g_busy_window_us ? double(now_us - g_busy_window_us) / 1e6 : 0.0;
        g_busy_window_us = now_us;
        const uint32_t bn = g_busy_n.exchange(0, std::memory_order_relaxed), by = g_busy_yes.exchange(0, std::memory_order_relaxed);
        char busy[160] = "";
        if (bn >= 20 && window_s > 0)
            std::snprintf(busy, sizeof busy, "; busy %.0f%% of the window (%u samples, no drain): %.1f ms of GPU work a present",
                          100.0 * by / bn, bn, 1000.0 * window_s * double(by) / double(bn) / frames);
        // the per-present GPU A/B (GlCensus::gpu_ab_arm): each arm's busy samples over the window's wall time, per present of
        // that arm -- the same scenes in both arms (they alternate every present)
        char ab[420] = "";
        {
            const uint32_t b0 = g_ab_busy[0].exchange(0, std::memory_order_relaxed), b1 = g_ab_busy[1].exchange(0, std::memory_order_relaxed);
            const uint32_t p0 = g_ab_presents[0], p1 = g_ab_presents[1];
            g_ab_presents[0] = g_ab_presents[1] = 0;
            const ps2r::render::GlCensus& gc = ps2r::render::Census();
            if (bn >= 20 && window_s > 0 && p0 && p1) {
                const double per = 1000.0 * window_s / double(bn);   // ms of wall time a sample stands for
                std::snprintf(ab, sizeof ab, "\n[te.gfx.vita] gpu A/B per present (%s, alternating): %s %.2f ms of GPU work a present "
                              "(%u presents), %s %.2f ms (%u presents); difference %+.2f ms", gc.gpu_ab_name, gc.gpu_ab_arms[1],
                              per * b1 / p1, p1, gc.gpu_ab_arms[0], per * b0 / p0, p0, per * b1 / p1 - per * b0 / p0);
            }
        }
        std::snprintf(head, sizeof head, "[te.gfx.vita] gpu: lag at the swap %.2f scenes (max %u); drains %u (%.1f ms each)%s",
                      lag_n ? double(lag_sum) / lag_n : 0.0, lag_max, pdrains, pdrains ? pdrain / 1000.0 / pdrains : 0.0, busy);
        b = head;
        for (int arm = 1; arm >= 0; --arm) {
            const uint32_t pn = g_probe_n[arm].exchange(0, std::memory_order_relaxed);
            const uint64_t ptail = g_probe_tail_us[arm].exchange(0, std::memory_order_relaxed);
            const uint64_t psub = g_probe_submit_us[arm].exchange(0, std::memory_order_relaxed);
            const uint32_t pmax = g_probe_tail_max_us[arm].exchange(0, std::memory_order_relaxed);
            if (!pn) continue;
            char p[260];
            std::snprintf(p, sizeof p, "; probe x%u (%s %s): after a drained frame's swap the GPU still needed "
                          "%.1f ms (max %.1f), the drawing thread took %.1f ms from the drain to that swap", pn,
                          ps2r::render::Census().ab_name, arm ? "on" : "off", ptail / 1000.0 / pn, pmax / 1000.0, psub / 1000.0 / pn);
            b += p;
        }
        b += ab;   // (its own line: "\n[te.gfx.vita] gpu A/B per present ...", or nothing)
    }
    std::string c = "[te.gfx.vita] vitaGL allocations in the window:";
    for (int i = 0; i < kPools; ++i) {
        const uint32_t ok = g_alloc_ok[i].exchange(0, std::memory_order_relaxed);
        const uint32_t fail = g_alloc_fail[i].exchange(0, std::memory_order_relaxed);
        const uint64_t bytes = g_alloc_bytes[i].exchange(0, std::memory_order_relaxed);
        char p[96];
        std::snprintf(p, sizeof p, " %s %u (%.0f KB, %u failed)", kPoolNames[i], ok, bytes / 1024.0, fail);
        c += p;
    }
    char d[200];
    char t[160];
    std::snprintf(t, sizeof t, "; %.2f ms in the allocator (the longest call %u us), %.2f ms in its frees",
                  double(g_alloc_us.exchange(0, std::memory_order_relaxed)) / 1000.0, g_alloc_max_us.exchange(0, std::memory_order_relaxed),
                  double(g_free_us.exchange(0, std::memory_order_relaxed)) / 1000.0);
    c += t;
    std::snprintf(d, sizeof d, "; RAM misses %.0f KB; frees %u; glFinish %u; 1-second allocator sleeps %u",
                  g_alloc_fail_bytes.exchange(0, std::memory_order_relaxed) / 1024.0,
                  g_frees.exchange(0, std::memory_order_relaxed), g_gl_finish.exchange(0, std::memory_order_relaxed),
                  g_long_sleeps.exchange(0, std::memory_order_relaxed));
    c += d;
    {
        uint32_t n[kGxmCategories];
        uint64_t total = 0;
        for (int i = 0; i < kGxmCategories; ++i) {
            n[i] = g_gxm_calls[i];
            g_gxm_calls[i] = 0;
            total += n[i];
        }
        // (the patcher and scene calls are in the line above; the counted ones are the per-draw / per-state ones)
        char e[300];
        std::snprintf(e, sizeof e, "\n[te.gfx.vita] gxm calls per frame: %.1f = draws %.1f + instanced %.1f + vertex streams %.1f + "
                      "fragment textures %.1f + programs %.1f + default uniform buffers %.1f + depth/stencil/raster state %.1f",
                      total / frames, n[kGxmDraws] / frames, n[kGxmInstanced] / frames, n[kGxmStreams] / frames,
                      n[kGxmTextures] / frames, n[kGxmPrograms] / frames, n[kGxmUniforms] / frames, n[kGxmState] / frames);
        c += e;
        // the calls the last-state cache did not make (they would have handed the context what it held), and the
        // raw mesh draws' vertex uniforms re-used (their composed contents = the buffer the context held for their program)
        uint32_t k[kGxmCategories];
        uint64_t skipped = 0;
        for (int i = 0; i < kGxmCategories; ++i) {
            k[i] = g_gxm_skipped[i];
            g_gxm_skipped[i] = 0;
        }
        // (the state setters asked, made at once or at the next draw -- coalesced -- or never: the difference is not made)
        k[kGxmState] = g_state_asked > n[kGxmState] ? g_state_asked - n[kGxmState] : 0;
        g_state_asked = 0;
        for (int i = 0; i < kGxmCategories; ++i) skipped += k[i];
        static uint64_t last_vsets = 0, last_vkept = 0;
        const ps2r::render::GlCensus& gc = ps2r::render::Census();
        const uint64_t vsets = gc.raw_vertex_uniform_sets - last_vsets, vkept = gc.raw_vertex_uniform_kept - last_vkept;
        last_vsets = gc.raw_vertex_uniform_sets;
        last_vkept = gc.raw_vertex_uniform_kept;
        char f[420];
        std::snprintf(f, sizeof f, "\n[te.gfx.vita] gxm last-state cache per frame: skipped %.1f of %.1f asked (%.1f%%) = vertex streams "
                      "%.1f + fragment textures %.1f + programs %.1f + default uniform buffers %.1f + depth/stencil/raster state %.1f "
                      "(%s); raw mesh draws' vertex uniforms re-used %.1f of %.1f (no buffer reserved, none set)",
                      skipped / frames, (skipped + total) / frames, skipped + total ? 100.0 * double(skipped) / double(skipped + total) : 0.0,
                      k[kGxmStreams] / frames, k[kGxmTextures] / frames, k[kGxmPrograms] / frames, k[kGxmUniforms] / frames,
                      k[kGxmState] / frames, g_gxm_cache > 0 ? "on" : g_gxm_cache == 0 ? "off: TE_GFX_VITA_GXM_CACHE=0" : "not read yet",
                      vkept / frames, vsets / frames);
        c += f;
    }
    return std::string(a) + "\n" + b + "\n" + c;
}

void SampleGpuBusy() {
    if (!&query_fence || !query_fence.address) return;
    const uint32_t i = *static_cast<volatile const uint32_t*>(&query_fence.value);
    const uint32_t d = *static_cast<volatile const uint32_t*>(query_fence.address);
    g_busy_n.fetch_add(1, std::memory_order_relaxed);
    if (int32_t(i - d) > 0) {
        g_busy_yes.fetch_add(1, std::memory_order_relaxed);
        // the per-present GPU A/B: the GPU is on the oldest scene not finished (d + 1)
        const int arm = g_scene_arm[(d + 1) & (kSceneArms - 1)].load(std::memory_order_relaxed) - 1;
        if (arm == 0 || arm == 1) g_ab_busy[arm].fetch_add(1, std::memory_order_relaxed);
    }
}

int FramesInFlight() {
    if (!&query_fence || !query_fence.address || !g_marks_valid) return -1;
    const uint32_t done = *static_cast<volatile const uint32_t*>(query_fence.address);
    // the newest presents first: a present is in flight while its last scene is past the GPU's finished count
    int n = 0;
    for (uint64_t k = 0; k < g_marks_valid; ++k) {
        const uint32_t mark = g_present_marks[(g_presents - k) % kPresentMarks];
        if (int32_t(mark - done) <= 0) break;
        ++n;
    }
    return n;
}

bool SceneFence(uint32_t* issued, uint32_t* done) {
    if (!&query_fence || !query_fence.address) return false;
    // the issued count first: a scene the GPU finishes between the two reads only makes `done` larger
    const uint32_t i = *static_cast<volatile const uint32_t*>(&query_fence.value);
    const uint32_t d = *static_cast<volatile const uint32_t*>(query_fence.address);
    if (issued) *issued = i;
    if (done) *done = d;
    return true;
}

void AllocsSoFar(uint64_t* allocs, uint64_t* bytes, uint64_t* frees, uint64_t* us) {
    if (us) *us = g_alloc_us_ever.load(std::memory_order_relaxed);
    if (allocs) *allocs = g_allocs_ever.load(std::memory_order_relaxed);
    if (bytes) *bytes = g_alloc_bytes_ever.load(std::memory_order_relaxed);
    if (frees) *frees = g_frees_ever.load(std::memory_order_relaxed);
}

}  // namespace te::vita_gpu

#else

namespace te::vita_gpu {
void OnPresented() {}
void SampleGpuBusy() {}
int FramesInFlight() { return -1; }
bool SceneFence(uint32_t*, uint32_t*) { return false; }
std::string WindowLines(double) { return std::string(); }
void AllocsSoFar(uint64_t* allocs, uint64_t* bytes, uint64_t* frees, uint64_t* us) {
    if (us) *us = 0;
    if (allocs) *allocs = 0;
    if (bytes) *bytes = 0;
    if (frees) *frees = 0;
}
}  // namespace te::vita_gpu

#endif
