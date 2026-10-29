// platform/nativeee (host side) -- Platform::Graphics over NativeEE's typed-draw surface (engine ps2e_draw.h), the
// slice: the renderer's start, the frame (StartFrame's clear, the 28 buckets' order, Present, the vertical blank the
// game thread waits for), the UI's 2D sprites and strips, the fonts' glyph quads (from the wasm side's Font::Draw) and the
// textures (decoded from the game's resources by gs_texture.cpp). Everything the game hands over arrives as a u32 guest
// address; materials are read from guest memory in the te_gfx_layout.h layout the wasm side writes.
//
// Where graphics.h expects packet building (Present's DMA chain, AllocFrameMemory, the particle blocks, SetUpRenderTarget's
// packet) the function is implemented by its MEANING over typed draws. The scene
// (culling, rigid/placed models, meshes, LOD, sky, shadows) is graphics_scene.cpp over the shared pieces of
// graphics_internal.h; the later slices (skins, particles, decals, screen models, shader animation, screen effects) are
// explicit no-ops here that log once and return the value that keeps the game going;
// the particles (graphics_particles.cpp) and the decals (graphics_decals.cpp) have their own modules.
//
// Environment (judges, CI):   TE_GFX_HEADLESS=1   hidden window (presentation still renders for capture)
//                             TE_GFX_DUMP_DIR=d   write presented frames as PPM into d ...
//                             TE_GFX_DUMP_EVERY=n ... every n-th present (default 0 = none)
//                             TE_GFX_DUMP_AT=k    ... and the k-th present
//                             TE_GFX_DUMP_FROM=a / TE_GFX_DUMP_TO=b  ... the every-n dumps limited to pictures a..b
//                             TE_GFX_LOG=1        log every seam call once
//                             TE_GFX_SUMMARY_EVERY=n  print the counters every n presents (default 600; 0 = off)
//                             PS2E_DRAW_STENCIL_DIAG=1  the engine's stencil histogram at the shadow clear / composite
//                             TE_GFX_NO_SHADOWS=1 the volumes counted but never composited (the caster judge's reference)
//                             TE_GFX_NO_DECALS=1  the decal pass records nothing (the decal judge's reference; graphics_decals.cpp)
//                             TE_GFX_PRESENT_CT16=1 the retail present: the frame shown as the dithered 16-bit copy (--present-ct16)
//                             TE_DECAL_DIAG=1     the decal view / every aged decal (TE_DECAL_DIAG_FROM=present) / the quads drawn
//                             TE_GFX_SHADOW_NO_DATE=1 the volumes counted without the frame-alpha test (the shadow-caster A/B)
//                             TE_GFX_SHADOWS_OFF=1 no shadow volume built, no counter cleared, nothing composited (the
//                                                 shadows' cost reference; graphics_scene.cpp)
//                             Vita only (the counter on vitaGL, gl_device.cpp / ps2e_draw.cpp): TE_GFX_VITA_STENCIL=0 the
//                                                 desktop's stencil-only framebuffer path (refused by vitaGL: no shadow);
//                                                 TE_GFX_VITA_CASTERS=0 the casters not re-stamped (their own volumes darken
//                                                 them); TE_GFX_VITA_SHADOW_FIX=0 the composite's FIX left to the backend's
//                                                 source-alpha stand-in (Cd - Cs: black shadows); TE_GFX_CASTER_CENSUS=n the
//                                                 re-stamp's census every n brackets / presents
//                             TE_GFX_INTERP=0/1   the frames between two pictures at 120/144/240/uncapped interpolated (1) or the
//                                                 newest picture repeated (0); TE_GFX_DUMP_INTERP=1 dumps the interpolated frames
//                                                 too (frame_<picture>_i<fraction in thousandths>.ppm) beside the pictures' dumps
//
// Pictures and presents. A PICTURE is a frame the game produced (one per Present with draws; always 60 a second in play
// mode: the game's tick is the FieldPacer's). A PRESENT is a frame shown on the window. At 60 they are the same frame; at 30
// every other picture is shown (steady); at 120 / 144 / 240 the render thread presents at that cadence (the engine's
// PresentPacer on the host's finest timer) and the frames between two pictures are interpolated by the engine's
// SceneInterpolator (or the newest picture repeated when interpolation is off); uncapped presents as fast as it can with
// the swap interval forced off. The pictures' hashes and dumps are the same at every rate: a picture the cadence does not
// show is RESOLVED (ps2e_draw_capture_frame) when a judge's capture is armed. Deterministic and synchronous runs present
// every picture whatever the setting (there is no pacer clock to interpolate on).
#include <algorithm>
#include <atomic>
#include <type_traits>
#if defined(__vita__)
#include <arm_neon.h>   // (the shadow volumes' box pass, VolumeBoxNeon)
#include <malloc.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
// vitaGL's pool query (vitaGL.h is not included here: this file reaches the GPU through ps2e_draw): size_t
// vglMemFree(vglMemType) with VGL_MEM_VRAM 0, VGL_MEM_RAM 1, VGL_MEM_PHYCONT 2 (the installed r1488: an
// sceClibMspaceMallocStats query of the pool, read-only; called below on the presenting thread, vitaGL's own)
extern "C" size_t vglMemFree(int type);
#include "../../../runner/te_vita.h"   // te::vita::LogRunning (the log's synced start-up ends at the first picture)
#endif
#include <cfenv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "engine/platform/ps2e/present_pacer.h"
#include "engine/platform/host/platform.h"
#include "engine/render/backends/gl/gl_census.h"
#include "engine/render/backends/gl/gl_fill_census.h"
#if defined(__vita__)
#include "engine/render/backends/gl/gl_vertex_stream.h"   // (the vertex copies' census)
#endif
#include "games/slus_20909_crash_twinsanity/hle/graphics/skin_geometry.h"
#include "graphics_internal.h"
#include "gs_texture.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "te_gfx_layout.h"
#include "te_display.h"
#include "te_graphics_hooks.h"
#include "te_harness.h"
#include "te_host.h"
#include "te_load_census.h"
#include "te_mesh_builder.h"
#include "te_prof.h"
#include "te_services.h"
#include "te_sync.h"
#include "te_tick_lock.h"
#include "vita_gpu_census.h"

namespace te::gfx { void RegisterStateSection(); }

namespace {

// ---- the game's structs as they lie in guest memory (include/platform/graphics.h, include/game/*.h) -----------------
struct FrameStart {
    int32_t x, y, width, height, screenWidth, screenHeight;
    uint8_t red, green, blue;
    uint8_t clearColor, clearDepth;
};
struct Rectangle {
    float x, y, width, height;
};
struct Vector2 {
    float x, y;
};
using te::gfx::RenderTargetDescription;
using te::gfx::MaterialView;
using te::gfx::Draw;
using te::gfx::DeferredBuild;
using te::gfx::BuildJob;
// Retail globals the PS2 renderer read (game/symbol_addrs.txt, PAL SLES-52568): the current render target pointer
// (G_FontRendererRel) and the game's screen size (D_00309AE4/8).
constexpr uint32_t kRenderTargetPointer = 0x00309BDC;
constexpr uint32_t kScreenWidth = 0x00309AE4, kScreenHeight = 0x00309AE8;
// renderer.h g_DisplayWidth/g_DisplayHeight (retail G_HorizontalResolution/G_VerticalResolution): the PS2 arm's
// InitDisplay wrote them at StartRenderer; game code reads them (movie.cpp's picture area, particles.cpp's aspect).
constexpr uint32_t kDisplayWidth = 0x0030AAE4, kDisplayHeight = 0x0030AAE8;
// GS coordinates: the screen's middle at 2048 pixels, 16 sub-pixels
constexpr float kGsMiddle = 2048.0f;
constexpr float kGsDepthFront = 16777215.0f;   // the 2D draws' GsFrontDepth, clamped to Z24

template <typename T>
const T* Guest(uint32_t address) {
    return reinterpret_cast<const T*>(te::GuestPointer(address, sizeof(T)));
}
template <typename T>
T* GuestMutable(uint32_t address) {
    return reinterpret_cast<T*>(te::GuestPointer(address, sizeof(T)));
}

bool EnvFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}
long EnvLong(const char* name, long fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::atol(v) : fallback;
}

struct Gfx {
    ps2e_draw* dev = nullptr;
    ps2e_draw_presentation presentation{};   // the display settings, set before or after the device exists
    bool create_failed = false;
    bool pal = false;
    int display_w = 512, display_h = 448;
    FrameStart frame{};
    bool have_frame = false;
    // The frame's buckets: `used` draws of `draws` are this frame's; the rest keep their vertex storage for the
    // next frames (ClearBuckets only resets `used`), so a steady frame records without allocating
    struct Bucket {
        std::vector<Draw> draws;
        size_t used = 0;
#if defined(__vita__)
        // The drawing thread's ORDER of the bucket's draws (ExpandBucket; ZOrderDraws permutes it; SubmitBuckets walks
        // it): pointers to the recorded draws -- in place, a placeholder's record left where it is -- and to the draws the
        // placeholders' builds expand into (`expanded`, the bucket's own records: they stay with the drawing side when the
        // draws swap at a hand-over). `ordered`: `order` is the picture's (cleared when the bucket takes a new picture's
        // draws). The 504-byte records never move: the expansion swapped every draw of a bucket's tail out and back
        // (6 record copies each), the order moved the punch-through and hoisted ones.
        std::vector<Draw*> order;
        std::vector<uint32_t> order_index;   // (the order as built: indices, the expanded ones with kExpandedBit)
        std::vector<Draw> expanded;
        size_t expanded_used = 0;
        bool ordered = false;
        // The records that may hold vertex storage (bit i: draws[i].verts' capacity may be non-zero; clear = none for
        // sure): set where a record takes vertices -- OpenDraw's fresh record (its producer appends), the expansion's joins
        // into a recorded draw -- cleared by ClearBuckets once the record holds none. ClearBuckets trims only these (the
        // others' TrimStorage returns at once): the sweep read a line of EVERY record of the frame set (~1000 a hub picture,
        // a miss each: 0.3-0.6 ms a tick on the device) where ~1 in 5 holds vertices. Travels with `draws`.
        std::vector<uint32_t> holds;
#endif
    };
    Bucket buckets[TE_GFX_BUCKET_COUNT];
    // The frame's deferred builds (ReserveDeferred): `deferred_used` of `deferred` are this frame's; they travel with the
    // buckets at the hand-over (their addresses are what the placeholders and the workers hold)
    std::vector<std::unique_ptr<DeferredBuild>> deferred;
    size_t deferred_used = 0;
    std::unordered_map<uint32_t, uint32_t> textures;   // GameTexture guest address -> ps2e_draw texture
    // the last lookups of `textures`, direct-mapped by address (the game thread's; every change of an entry drops its
    // slot): a material read finds its texture without the map's division (software on the Vita's CPU)
    static constexpr uint32_t kTextureSlots = 256;
    uint32_t texture_keys[kTextureSlots] = {};
    uint32_t texture_values[kTextureSlots] = {};
    // The texture resources as the game handed them (header + upload packet): what a save state carries so a loaded
    // process can decode them again (the game frees the packet's disk-manager block after the upload, as the PS2 did).
    struct TextureSource { te_gfx_texture_header header; std::vector<uint8_t> packet; };
    std::unordered_map<uint32_t, TextureSource> texture_sources;
    // The counters the render thread writes are atomic: the summary and the save state read them from the game thread
    std::atomic<uint64_t> presents{0}, dropped_draws{0}, draws_submitted{0}, presents_with_draws{0}, frame_draws{0},
        frame_triangles{0};
    uint64_t vblanks = 0, draws = 0, texture_failures = 0;
    uint64_t draws_merged = 0;   // draws appended to the bucket's last draw (draws_submitted: handed to the backend)
    uint64_t volume_vertices = 0;   // the Vita: the shadow volumes' counting vertices drawn grouped (SubmitVolumeRun)
    uint64_t mesh_runs = 0, mesh_run_draws = 0;   // the Vita: runs of one mesh range handed over whole, their draws
    uint64_t expanded_segments = 0, expanded_tails = 0;   // the Vita's ExpandBucket: segment draws written; recorded draws
                                                          // from a bucket's first placeholder on (the old form moved them)
    double last_submit_ms = 0, last_present_ms = 0, last_pump_ms = 0;   // the last Present's phases (the clock's slow-tick line reads them)
    // The render thread: in PLAY mode with a window the frame's draws are submitted and presented off the game
    // thread -- the game thread hands over its buckets at Present (the two sets of bucket storage swap: no copy) and
    // continues; the GL work (backend submit ~2.5 ms, the swap, and the window server's stalls under host load) is the
    // render thread's. One frame in flight: a Present that finds the previous frame still drawing waits for it. Every
    // other GL call (textures, resize, captures) runs on the render thread through OnRender, synchronously.
    // Deterministic / headless / dumping runs keep the synchronous path (their frames are judged by present number).
    struct Frame {
        Bucket buckets[TE_GFX_BUCKET_COUNT];
        std::vector<std::unique_ptr<DeferredBuild>> deferred;
        size_t deferred_used = 0;
        bool send = false;
        int screen_w = 0, screen_h = 0;
        uint64_t harness_key = 0;                 // the boundary this present precedes (te_harness.h's key)
        uint64_t picture = 0;                     // the picture's ordinal (1-based, as the game produced them)
        bool have_edge = false;                   // the pacer edge of the field this picture follows (play mode)
        int64_t edge_ns = 0;
        uint64_t field = 0;
        ps2r::ps2e::InterpCamera camera;          // the game's camera this picture was drawn with (the interpolator's)
        uint32_t camera_object = 0;               // the game's camera object: another one = a cut
        std::vector<ps2r::ps2e::InterpItem> items; // the picture's draws as the interpolator reads them (built on the render thread)
        std::vector<uint32_t> retire;              // native meshes retired before this picture: destroyed when its slot is free
        std::vector<std::unique_ptr<te::gfx::RetiredStorage>> retired_storage;   // storage let go before it: freed then too
        uint32_t lock_fields = 0;                  // the tick lock's fields for this picture (te_tick_lock.h: 0 not a picture
                                                   // tick, 1 = 60 Hz, 2 / 3 = the 30 / 20 Hz lock: drawn in full, held that
                                                   // many refreshes; only 0 / 1 pictures are ever replaced before drawn)
        uint32_t pool_ordinal = 0;                 // the Vita: the state pool's ordinal of this picture (StatePool::recording)
    };
    struct RenderThread {
#if defined(__vita__)
        static constexpr int kSlots = 3;           // the Vita (no interpolation, a fixed heap): the two pictures kept + the one
                                                   // the game thread hands over while the newest is drawn
#else
        static constexpr int kSlots = 5;           // the two pictures the presenter keeps (newest and the one before, for the
                                                   // frames between them) + one frame drawing + up to two queued: a late frame
                                                   // (a swap the window server held, a preempted render thread) is absorbed
#endif
        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake, done;
        bool enabled = false, started = false, stop = false;
        bool drawing = false;
        Frame slots[kSlots];
        std::vector<int> queue, free;              // slot indices: queued in order; free for the game thread
        int cur = -1, prev = -1;                   // the presenter's two retained pictures (slot indices; -1 none)
        std::vector<std::function<void()>> jobs;   // OnRender's, run before the next frame / when idle
        bool textures_ready = false;               // a worker decoded a texture (EAGER TEXTURES): made when no picture waits
        uint64_t frames = 0, waits = 0;            // frames drawn; Presents that found no free slot and waited
        double wait_ms = 0, wait_max_ms = 0;       // the game thread's time in those waits
        uint64_t jobs_posted = 0, jobs_done = 0;
        // The presenter: the cadence (te_display's frame rate, taken at the next picture), the pacer, its census
        ps2r::ps2e::PresentPacer pacer;
        int pacer_rate = -1;                       // the rate the pacer is set to (-1: not yet)
        bool interpolate = true;                   // TE_GFX_INTERP (the frames between two pictures; off = the newest repeated)
        bool dump_interp = false;                  // TE_GFX_DUMP_INTERP
        bool probe_half = false;                   // TE_GFX_INTERP_PROBE: at every picture's arrival the half-way frame of (previous,
                                                   // newest) is built and dumped as frame_<picture>_i500.ppm (the quality judge's
                                                   // triplets, independent of the pacer's slots and the host's speed)
        uint64_t interp_frames = 0, snaps = 0;     // frames interpolated; frames shown as the newest picture because of a cut
        double latency_sum_ms = 0, latency_max_ms = 0;   // present instant - the shown content's time, over the presents
        uint64_t latency_n = 0;
        uint64_t interp_bytes = 0;                 // the interpolated frame's vertex storage (the one extra scene copy)
        int target_slot = -1;                      // what the frame target holds: a picture's slot at fraction 1, or an
        double target_fraction = -1;               // interpolated frame (a re-present of the same content skips the submit)
        uint64_t represents = 0;                   // presents that re-showed the frame target as it stood (no submit)
        ps2r::ps2e::SceneInterpolator interpolator;   // the engine's: the frame between two pictures
        std::vector<ps2r::ps2e::InterpOutput> interp_out;
        bool interp_diag = false;                  // TE_GFX_INTERP_DIAG: the matrices' consistency and the census per frame
        uint64_t last_noted_picture = 0;
        double interp_ms_sum = 0, submit_ms_sum = 0, present_ms_sum = 0;   // the interpolated frame's phases (the census)
        uint64_t phase_n = 0;
        // The pictures' arrival ages (arrival instant - the picture's tick edge, ns) over the last 64 pictures, and the
        // DELAY the fraction's clock runs behind: a picture is usable for the frames between it and the one before only
        // once it has arrived, so the shown content runs one tick plus `delay` behind the newest edge, with delay = the
        // ages' 90th percentile (how long after its edge a picture arrives: the game thread's frame work), capped at a tick
        int64_t arrival_ages[64] = {};
        int arrival_count = 0;
        int64_t delay_ns = 0;
        double delay_ms_max = 0;
        ~RenderThread() {
            { std::lock_guard<std::mutex> lock(mutex); stop = true; }
            wake.notify_all();
            if (thread.joinable()) thread.join();
        }
    } rt;
    std::atomic<int> frame_rate{1};                // te_display's setting (0 = 30 .. 4 = 240, 5 = uncapped), read by the render thread
    std::atomic<bool> interp_active{false};        // the presenter interpolates (slot-paced + interpolation on): draws of different tags stay apart
    ps2r::ps2e::InterpTag tag;                     // the tag OpenDraw stamps (SetDrawTag / ClearDrawTag; the game thread's)
    bool tag_untagged = true;                      // `tag` is the untagged InterpTag{} (a compact mesh record's: RecordMeshCompact)
    ps2r::ps2e::InterpTransform tag_transform{};
    bool tag_has_transform = false;
    std::atomic<uint64_t> pictures{0};             // game pictures taken by the presenter (every Present with draws), shown or not
    uint64_t pictures_handed = 0;                  // game pictures handed over (the game thread's count: the ordinals)
    std::chrono::steady_clock::time_point first_present{};
    bool have_first_present = false;
    uint64_t start_frames = 0, clears = 0, sprites = 0, turned_sprites = 0, strips = 0, text_quads = 0, textures_loaded = 0;
    uint64_t scene_mark[4] = {0, 0, 0, 0};
    bool summary_armed = false;
    // Options, read from the environment on the first seam call (not at load: a harness may set them in main)
    bool options_read = false;
    bool headless = false, log = false;
    bool present_ct16 = false;   // the dithered 16-bit present (te::Settings / TE_GFX_PRESENT_CT16; the hook toggles it)
    std::string dump_dir;
    long dump_every = 0, dump_at = 0, summary_every = 600;
    // TE_HARNESS_KEYS=a,b,c: the harness leg hashes only the pictures preceding these boundaries (the capture armed for
    // them alone) -- a paced run's cadence is then unperturbed by the per-present read-back; the other keys log hash 0
    std::vector<uint64_t> harness_keys;
    long dump_from = 0, dump_to = 0;   // TE_GFX_DUMP_FROM / TE_GFX_DUMP_TO: the pictures dumped (every dump_every) lie in [from, to]; 0 = unbounded
    std::vector<uint32_t> mesh_retired;   // native meshes retired since the last hand-over (RetireMesh)
    std::vector<std::unique_ptr<te::gfx::RetiredStorage>> storage_retired;   // storage let go since then (RetireStorage)
    int native_meshes = -1;               // -1 unknown, 0 off (setting or a device without them), 1 on
    // Blended draws of one state join as unblended ones do (one glDrawArrays rasterises and blends its triangles in order,
    // exactly as separate draws of the same state would). Exact where the backend blends in fixed function -- every
    // blended ps2e_draw state (ps2e_draw.cpp: Decoded textures, no FBA on a blended draw, colour clamp on, 32-bit frame),
    // so no draw reads the destination; on by default on the Vita (each draw costs vitaGL's and the emulator's GXM work),
    // TE_GFX_JOIN_BLENDED=0/1 decides, TE_AB=joinblend flips it in-run
#if defined(__vita__)
    std::atomic<bool> join_blended{true};
#else
    std::atomic<bool> join_blended{false};
#endif
    // The shadow volumes' counting draws of a run drawn as one draw per state (SubmitBuckets / SubmitVolumeRun): on by
    // default on the Vita (every draw there costs vitaGL's ~12 us and the engine's state path), TE_GFX_GROUP_VOLUMES=0/1
#if defined(__vita__)
    bool group_volumes = true;
#else
    bool group_volumes = false;
#endif
    uint64_t volume_draws_in = 0, volume_draws_out = 0;   // the drawing thread's census: counting draws grouped -> drawn
    // the drawing thread's CPU per draw by path, 1 draw in 32 timed (the Vita's census; TE_GFX_DRAW_TIMING=0: off, =N: 1 in
    // N, a power of two from 2 -- each timed draw costs two kernel clock calls)
#if defined(__vita__)
    bool draw_timing = true;
#else
    bool draw_timing = false;
#endif
    uint32_t draw_timing_mask = 31;   // a draw is timed when (tick & mask) == 0
    // the Vita: RecordMeshInPlace prefetches the next record's lines (TE_GFX_PREFETCH=0: off)
    bool prefetch_records = true;
#if defined(__vita__)
    // THE LOW-LATENCY HAND-OVER (the Vita, play mode with the drawing thread; TE_VITA_LOW_LATENCY=0: the queue as before).
    // The game thread never waits for the drawing thread: a picture the drawing thread has not taken yet is REPLACED by
    // the next one (dropped, never drawn) -- the game ticks at the clock's pace (its logic, the pads' sample and the audio
    // mix every boundary) while the GPU draws the newest picture it can (an effective frame skip). The drawing thread takes
    // a picture only once the GPU has at most `ll_ahead` frames of its own in flight (TE_VITA_GPU_AHEAD, default 1: the GPU
    // draws one frame while the next is submitted), so the picture it takes is the newest when the GPU can start on it --
    // without the wait the picture would sit behind two queued frames (vitaGL's display queue holds two).
    // Deterministic runs (no drawing thread, or one forced on by TE_GFX_RENDER_THREAD=1) keep the queue: every picture is
    // drawn, in order.
    bool ll_on = true;
    int ll_ahead = 1;
    long ll_render_delay_us = 0;           // TE_VITA_DIAG_RENDER_DELAY_US: the drawing thread sleeps this long per picture (a test knob)
    std::atomic<uint64_t> ll_handed{0};    // pictures the game thread handed over (its count, for the drawing thread's census)
    std::atomic<uint64_t> ll_dropped{0};   // of them, replaced before the drawing thread took them
    std::atomic<uint64_t> ll_boundaries{0};   // the clock's boundaries at the last hand-over
    std::atomic<uint64_t> ll_gpu_wait_us{0};  // the drawing thread's waits for the GPU before taking a picture
    std::atomic<uint64_t> ll_gpu_timeouts{0}; // of them, given up after 100 ms (the picture taken anyway)
    std::atomic<uint64_t> ll_build_wait_us{0};   // the game thread's waits for a replaced picture's builds
#endif
    bool movie_picture = false;          // the frame being recorded holds the movie quad (te_graphics_record_movie_quad)
    double last_present_call_ms = 0;     // the drawing thread's last ps2e_draw_present call (the tick lock's census)
    long diag_delay_from = 0, diag_delay_to = 0;   // TE_VITA_DIAG_RENDER_DELAY_FROM / _TO: the delay's presents (0: all)
    long diag_delay_every = 0;                     // TE_VITA_DIAG_RENDER_DELAY_EVERY: one picture in N delayed (0 / 1: every one)
} g;

#if defined(__vita__)
// The low-latency hand-over is in force (see Gfx::ll_on): the drawing thread runs, the clock is the wall's, 60 (every
// picture presented as it arrives)
bool LowLatency() {
    return g.ll_on && g.rt.started && !te::GetSettings().deterministic && g.frame_rate.load(std::memory_order_relaxed) == 1;
}
#endif

void ExpandDeferred(Gfx::Bucket* buckets, std::vector<std::unique_ptr<DeferredBuild>>& deferred, size_t& used);
void ExpandBucket(Gfx::Bucket* buckets, int b);

#if defined(__vita__)
// ---- the Vita's compact mesh records (graphics_internal.h RecordMeshCompact) and their state pool ------------------------
// The device spent ~8.6 us of the game thread per placed model (measured) -- most of it the record's ~11 cache lines, each a
// miss (the drawing thread on another core read them last). A compact record is the header line and the parameters' screen
// / origin / scroll / lit lines; its state -- the same few hundred material states every picture -- is a pool entry's,
// copied in on the drawing thread at the expansion.
//
// The pool: kEntries states kept by their bytes (a chained hash), each with the last picture that used it (`last_use`, the
// game thread's `recording` ordinal) and a generation. The drawing thread publishes `done` = the ordinal of the last
// picture it has finished (expanded and drawn, or set aside never to be drawn: the pictures it takes come in order, the
// replaced ones are never drawn), after its last read of the pool for it; an entry is remade for other bytes only once
// last_use <= done (acquire), so no picture still to be expanded names it. (A slot-paced presenter -- 120 Hz and up, the
// interpolator -- may submit a picture later than its turn: no compact records then.)
struct StatePool {
    static constexpr uint32_t kEntries = 2048;
    static constexpr uint32_t kBuckets = 1024;
    ps2e_draw_state states[kEntries];
    uint32_t hashes[kEntries] = {};
    uint32_t generations[kEntries] = {};
    uint32_t last_use[kEntries] = {};
    uint16_t next[kEntries] = {};       // the hash chain (id + 1; 0 ends it)
    uint16_t heads[kBuckets] = {};
    uint32_t made = 0;                  // ids 1..made exist
    uint32_t cursor = 0;                // the remaking sweep's place
    uint32_t recording = 1;             // the ordinal of the picture being recorded (the game thread's)
    std::atomic<uint32_t> done{0};      // the last picture the drawing thread has finished with (or the game thread, synchronous)
    bool read = false, enabled = true, check = false;
    long ab = 0;                        // TE_GFX_COMPACT=abN: on / off by N presents (a device A/B of the game thread's cost)
    uint64_t presents_on = 0, presents_counted = 0, last_presents_on = 0, last_presents_counted = 0;
    // the census (the game thread's; `expanded` / `compared` / `differ` the drawing thread's)
    uint64_t compact_records = 0, full_records = 0, remade = 0, lookups = 0, found = 0;
    uint64_t last_compact = 0, last_full = 0;
    // (32-bit: a 64-bit atomic's relaxed load / store is an LDREXD / STREXD loop on ARMv7; the census takes differences)
    std::atomic<uint32_t> expanded{0};
    uint32_t last_expanded = 0;
    std::atomic<uint64_t> compared{0}, differ{0};
    void Read() {
        read = true;
        if (const char* v = std::getenv("TE_GFX_COMPACT")) {
            if (v[0] == 'a' && v[1] == 'b') ab = std::max(1L, std::atol(v + 2));
            else enabled = v[0] != '0';
        }
        check = std::getenv("TE_GFX_COMPACT_CHECK") != nullptr;
    }
    // the A/B's arm for the picture being recorded
    bool ArmOn() const { return ab <= 0 || ((g.presents.load(std::memory_order_relaxed) / uint64_t(ab)) & 1u) == 0; }
    static uint32_t Hash(const ps2e_draw_state& s) {
        uint32_t words[sizeof(ps2e_draw_state) / 4];
        std::memcpy(words, &s, sizeof words);
        uint32_t h = 2166136261u;
        for (uint32_t w : words) h = (h ^ w) * 16777619u;
        return h | 1u;   // (0: a free entry)
    }
    void Unlink(uint32_t id) {
        uint16_t* p = &heads[hashes[id - 1] & (kBuckets - 1)];
        while (*p && *p != id) p = &next[*p - 1];
        if (*p) *p = next[id - 1];
        next[id - 1] = 0;
    }
    // The id of `s`'s bytes (made when new), 0 when every entry is still named by a picture not finished
    uint32_t Intern(const ps2e_draw_state& s, uint32_t* generation) {
        if (!read) Read();
        if (!enabled) return 0;
        lookups++;
        const uint32_t h = Hash(s);
        for (uint32_t id = heads[h & (kBuckets - 1)]; id; id = next[id - 1]) {
            if (hashes[id - 1] == h && std::memcmp(&states[id - 1], &s, sizeof s) == 0) {
                found++;
                if (generation) *generation = generations[id - 1];
                return id;
            }
        }
        uint32_t id = 0;
        if (made < kEntries) {
            id = ++made;
        } else {
            // an entry no unfinished picture names (the sweep goes on from its last place; at most 64 looked at)
            const uint32_t finished = done.load(std::memory_order_acquire);
            for (int k = 0; k < 64 && !id; ++k) {
                const uint32_t c = cursor;
                cursor = (cursor + 1) % kEntries;
                if (int32_t(last_use[c] - finished) <= 0) id = c + 1;
            }
            if (!id) return 0;
            Unlink(id);
            remade++;
        }
        std::memcpy(&states[id - 1], &s, sizeof s);
        hashes[id - 1] = h;
        generations[id - 1]++;
        last_use[id - 1] = recording;
        next[id - 1] = heads[h & (kBuckets - 1)];
        heads[h & (kBuckets - 1)] = uint16_t(id);
        if (generation) *generation = generations[id - 1];
        return id;
    }
    bool Current(uint32_t id, uint32_t generation) const { return id && id <= made && generations[id - 1] == generation; }
    void Use(uint32_t id) { last_use[id - 1] = recording; }
    uint32_t InUse() const {
        const uint32_t finished = done.load(std::memory_order_relaxed);
        uint32_t n = 0;
        for (uint32_t k = 0; k < made; ++k) n += int32_t(last_use[k] - finished) > 0 ? 1u : 0u;
        return n;
    }
};
StatePool g_pool;
// Whether the next mesh record may be compact (graphics_internal.h CompactMeshes)
bool CompactRecording() {
    if (!g_pool.read) g_pool.Read();
    return g_pool.enabled && g.tag_untagged && g_pool.ArmOn() && !g.interp_active.load(std::memory_order_relaxed) &&
           !ps2r::ps2e::PresentPacer::PacedBySlots(
               static_cast<ps2r::ps2e::PresentPacer::Rate>(g.frame_rate.load(std::memory_order_relaxed)));
}
// A compact record made whole (the drawing thread, ExpandBucket; or the game thread's synchronous present): the pooled state
// and what RecordMeshInPlace + DrawInstance write for such a draw -- skin_joints 0, the untagged tag (the record was compact
// only while the tag was), the whole frame as the box, the whole mesh. TE_GFX_COMPACT_CHECK: the record is whole already --
// compared instead
void ExpandCompact(Draw& d) {
    const ps2e_draw_state& s = g_pool.states[d.pool - 1];
    // (one thread expands -- the drawing thread, or the game thread's synchronous present: a plain count, no RMW per record)
    g_pool.expanded.store(g_pool.expanded.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (g_pool.check) {
        const ps2r::ps2e::InterpTag untagged{};
        const bool same = std::memcmp(&d.state, &s, sizeof s) == 0 && d.skin_joints == 0 && d.tag.id == untagged.id &&
                          d.tag.part == untagged.part && d.tag.kind == untagged.kind && d.mesh_params.box[0] == -1e9f &&
                          d.mesh_params.box[1] == -1e9f && d.mesh_params.box[2] == 1e9f && d.mesh_params.box[3] == 1e9f &&
                          d.mesh_params.first == 0 && d.mesh_params.count == 0;
        g_pool.compared.fetch_add(1, std::memory_order_relaxed);
        if (!same) {
            const uint64_t n = g_pool.differ.fetch_add(1, std::memory_order_relaxed);
            if (n < 8)
                std::fprintf(stderr, "[te.gfx.vita] COMPACT CHECK: record of mesh %u (pool %u) differs from its expansion: state %s, "
                             "skin_joints %u, tag %llx/%u/%u, box %g %g %g %g, first %d count %d\n",
                             d.mesh, unsigned(d.pool), std::memcmp(&d.state, &s, sizeof s) == 0 ? "same" : "DIFFERENT",
                             unsigned(d.skin_joints), (unsigned long long)d.tag.id, unsigned(d.tag.part), unsigned(d.tag.kind),
                             double(d.mesh_params.box[0]), double(d.mesh_params.box[1]), double(d.mesh_params.box[2]),
                             double(d.mesh_params.box[3]), d.mesh_params.first, d.mesh_params.count);
        }
        return;
    }
    // (each field written only where it differs: a record slot draws the same part the picture before as a rule, so its
    // lines mostly hold these values already -- read, which ZOrderDraws and the submit do next anyway, and left clean. The
    // state compared word by word inline: newlib's memcmp is a call and a loop with branches per word)
    {
        static_assert(sizeof(ps2e_draw_state) % 4 == 0, "the state in words");
        constexpr size_t kWords = sizeof(ps2e_draw_state) / 4;
        // (straight from the two records -- word loads, core registers: copies into locals were two memcpy calls and a NEON
        // XOR-OR with a NEON-to-core transfer at its end on the Cortex-A9)
        const unsigned char* const have = reinterpret_cast<const unsigned char*>(&d.state);
        const unsigned char* const want = reinterpret_cast<const unsigned char*>(&s);
        uint32_t differ = 0;
#pragma GCC novector
        for (size_t k = 0; k < kWords; ++k) {
            uint32_t a, b;
            std::memcpy(&a, have + 4 * k, 4);
            std::memcpy(&b, want + 4 * k, 4);
            differ |= a ^ b;
        }
        if (differ) std::memcpy(&d.state, &s, sizeof s);
    }
    if (d.skin_joints) d.skin_joints = 0;
    const ps2r::ps2e::InterpTag untagged{};
    if (d.tag.id != untagged.id || d.tag.part != untagged.part || d.tag.kind != untagged.kind) d.tag = untagged;
    ps2e_draw_mesh_params& p = d.mesh_params;
    {
        // (the box's bits compared as integers: a float compare is a VCMP + VMRS flag transfer each on the Cortex-A9)
        static const uint32_t kWhole[4] = {0xCE6E6B28u, 0xCE6E6B28u, 0x4E6E6B28u, 0x4E6E6B28u};   // -1e9f, -1e9f, 1e9f, 1e9f
        uint32_t have[4];
        std::memcpy(have, p.box, sizeof have);
        if ((have[0] ^ kWhole[0]) | (have[1] ^ kWhole[1]) | (have[2] ^ kWhole[2]) | (have[3] ^ kWhole[3])) {
            p.box[0] = p.box[1] = -1e9f;
            p.box[2] = p.box[3] = 1e9f;
        }
    }
    if (p.first) p.first = 0;
    if (p.count) p.count = 0;
}
// Gfx::Bucket::holds: record i of the bucket may hold vertex storage from here on
inline void MarkHolds(Gfx::Bucket& b, size_t i) {
    const size_t w = i >> 5;
    if (w >= b.holds.size()) b.holds.resize(w + 1, 0u);
    b.holds[w] |= 1u << (i & 31);
}
#endif

// g.textures through its slots (0: none)
uint32_t TextureSlot(uint32_t address) { return (address >> 4) & (Gfx::kTextureSlots - 1); }
uint32_t TextureHandleOf(uint32_t address) {
    const uint32_t slot = TextureSlot(address);
    if (address && g.texture_keys[slot] == address) return g.texture_values[slot];
    auto it = g.textures.find(address);
    if (it == g.textures.end()) return 0;
    g.texture_keys[slot] = address;
    g.texture_values[slot] = it->second;
    return it->second;
}
void TextureChanged(uint32_t address) {
    if (g.texture_keys[TextureSlot(address)] == address) g.texture_keys[TextureSlot(address)] = 0;
}
void TexturesCleared() {
    for (uint32_t i = 0; i < Gfx::kTextureSlots; ++i) g.texture_keys[i] = 0;
}

void ReadOptions();
std::string PresentCensus();
void Once(const char* what) {
    static std::unordered_map<std::string, bool> said;
    ReadOptions();
    if (!g.log) return;
    bool& s = said[what];
    if (!s) {
        s = true;
        std::fprintf(stderr, "[te.gfx] %s\n", what);
    }
}

void Summary() {
    std::fprintf(stderr,
                 "[te.gfx] summary: presents %llu (with draws %llu), vblanks %llu, StartFrame %llu (clears %llu), sprites %llu, "
                 "turned %llu, strips %llu, text quads %llu, draws %llu (merged %llu, submitted %llu, dropped %llu), textures %llu (failed %llu); scene: rigid %llu, "
                 "placed %llu, sky %llu, shadows %llu, triangles %llu, models decoded %llu (refused %llu, missing %llu), culls %llu; "
                 "skins %llu (blend %llu, triangles %llu, decoded %llu, refused %llu; %.0f ms); shadow passes %llu (odd colours %llu; %.0f ms); particle blocks %llu (triangles %llu, refused %llu); decals aged %llu (culled %llu, blocks %llu, quads %llu, refused %llu); screen models %llu (triangles %llu); material passes %llu; "
                 "render thread %s (frames %llu, waits %llu, %.1f ms, max %.1f)\n",
                 (unsigned long long)g.presents.load(), (unsigned long long)g.presents_with_draws.load(), (unsigned long long)g.vblanks,
                 (unsigned long long)g.start_frames, (unsigned long long)g.clears, (unsigned long long)g.sprites,
                 (unsigned long long)g.turned_sprites, (unsigned long long)g.strips, (unsigned long long)g.text_quads,
                 (unsigned long long)g.draws, (unsigned long long)g.draws_merged, (unsigned long long)g.draws_submitted.load(),
                 (unsigned long long)g.dropped_draws.load(), (unsigned long long)g.textures_loaded,
                 (unsigned long long)g.texture_failures, (unsigned long long)te::gfx::Scene().rigid_models,
                 (unsigned long long)te::gfx::Scene().placed_models, (unsigned long long)te::gfx::Scene().sky_models,
                 (unsigned long long)te::gfx::Scene().shadow_meshes, (unsigned long long)te::gfx::Scene().model_triangles,
                 (unsigned long long)te::gfx::Scene().models_decoded, (unsigned long long)te::gfx::Scene().models_refused,
                 (unsigned long long)te::gfx::Scene().submodels_missing, (unsigned long long)te::gfx::Scene().culls,
                 (unsigned long long)te::gfx::Scene().skins, (unsigned long long)te::gfx::Scene().blend_skins,
                 (unsigned long long)te::gfx::Scene().skin_triangles, (unsigned long long)te::gfx::Scene().skins_decoded,
                 (unsigned long long)te::gfx::Scene().skins_refused, te::gfx::Scene().skin_ms, (unsigned long long)te::gfx::Scene().shadow_passes,
                 (unsigned long long)te::gfx::Scene().shadow_odd_colours, te::gfx::Scene().shadow_ms,
                 (unsigned long long)te::gfx::Scene().particle_blocks, (unsigned long long)te::gfx::Scene().particle_triangles,
                 (unsigned long long)te::gfx::Scene().particle_refused,
                 (unsigned long long)te::gfx::Scene().decals_aged, (unsigned long long)te::gfx::Scene().decals_culled,
                 (unsigned long long)te::gfx::Scene().decal_blocks, (unsigned long long)te::gfx::Scene().decal_quads,
                 (unsigned long long)te::gfx::Scene().decal_refused,
                 (unsigned long long)te::gfx::Scene().screen_models, (unsigned long long)te::gfx::Scene().screen_model_triangles,
                 (unsigned long long)te::gfx::Scene().material_passes,
                 g.rt.enabled ? "on" : "off", (unsigned long long)g.rt.frames, (unsigned long long)g.rt.waits, g.rt.wait_ms,
                 g.rt.wait_max_ms);
    std::fprintf(stderr, "[te.gfx] %s\n", PresentCensus().c_str());
}

// ---- the device --------------------------------------------------------------------------------------------------------
void ReadOptions() {
    if (g.options_read) return;
    te::gfx::RegisterStateSection();
    g.options_read = true;
    g.headless = EnvFlag("TE_GFX_HEADLESS");
    g.log = EnvFlag("TE_GFX_LOG");
    g.dump_dir = std::getenv("TE_GFX_DUMP_DIR") ? std::getenv("TE_GFX_DUMP_DIR") : "";
    g.dump_every = EnvLong("TE_GFX_DUMP_EVERY", 0);
    g.dump_at = EnvLong("TE_GFX_DUMP_AT", 0);
    g.dump_from = EnvLong("TE_GFX_DUMP_FROM", 0);
    if (const char* k = std::getenv("TE_HARNESS_KEYS")) {
        for (const char* p = k; *p;) {
            char* end = nullptr;
            const long long v = std::strtoll(p, &end, 10);
            if (end == p) break;
            if (v > 0) g.harness_keys.push_back(uint64_t(v));
            p = *end == ',' ? end + 1 : end;
        }
    }
    g.dump_to = EnvLong("TE_GFX_DUMP_TO", 0);
    g.summary_every = EnvLong("TE_GFX_SUMMARY_EVERY", 600);
    if (const char* v = std::getenv("TE_GFX_JOIN_BLENDED")) g.join_blended = v[0] == '1';
    if (const char* v = std::getenv("TE_GFX_GROUP_VOLUMES")) g.group_volumes = v[0] == '1';
    if (const char* v = std::getenv("TE_GFX_DRAW_TIMING")) {
        const long n = std::atol(v);
        g.draw_timing = n != 0;
        if (n >= 2) {
            uint32_t p = 1;
            while (p * 2 <= uint32_t(std::min(n, 4096L))) p *= 2;
            g.draw_timing_mask = p - 1;
        }
    }
    if (const char* v = std::getenv("TE_GFX_PREFETCH")) g.prefetch_records = v[0] != '0';
    // TE_GFX_VECTOR_BUILD=1 (the Vita): the rigid models' CPU builds transform through the Advanced SIMD unit
    // (title::MeshGeometry::SetVectorTransform: a lane may differ from the scalar path by a unit in the last place)
    if (EnvFlag("TE_GFX_VECTOR_BUILD")) title::MeshGeometry::SetVectorTransform(true);
    // TE_GFX_SKIN_FAST=0 (the Vita): the clipped plain skins and the blend skins through their VU programs again
    // (title::SkinGeometry::SetFastPaths; by default they take the direct skinning the unclipped plain skins take: the
    // same triangles, within a 1/16-pixel step of the programs' positions, the same colours -- mechwork host check)
    if (const char* v = std::getenv("TE_GFX_SKIN_FAST")) title::SkinGeometry::SetFastPaths(v[0] != '0');
    // TE_GFX_NATIVE_SCROLLED=0 (the Vita): the blended UV-scrolled passes CPU-built every frame again (te::gfx::SetNativeScrolled;
    // by default they are drawn as native meshes like the other unclipped passes)
    if (const char* v = std::getenv("TE_GFX_NATIVE_SCROLLED")) te::gfx::SetNativeScrolled(v[0] != '0');
    // TE_GFX_GPU_SKIN=0 (the Vita): the plain skins CPU-built every frame again (te::gfx::SetGpuSkins; by default a skin
    // part is skinned, transformed and lit by the GPU from its resident mesh and the frame's bone palette)
    if (const char* v = std::getenv("TE_GFX_GPU_SKIN")) te::gfx::SetGpuSkins(v[0] != '0');
    if (const char* v = std::getenv("TE_GFX_GPU_BLEND")) te::gfx::SetGpuBlend(v[0] != '0');
    // TE_GFX_CHUNKED_NATIVE=0 (the Vita): a clipped instance's part that is not wholly in front, and the sky, CPU-built
    // whole every frame again (te::gfx::SetChunkedNative; by default sorted chunk by chunk into native draws, dropped
    // chunks and CPU builds of the chunks that cross the guard band)
    if (const char* v = std::getenv("TE_GFX_CHUNKED_NATIVE")) te::gfx::SetChunkedNative(v[0] != '0');
    // the render thread is on in play mode with a window and no frame dumps; TE_GFX_RENDER_THREAD=0/1 decides
    const bool play = !te::GetSettings().deterministic;
    g.rt.enabled = play && !g.headless && g.dump_dir.empty();
    if (const char* v = std::getenv("TE_GFX_RENDER_THREAD")) g.rt.enabled = v[0] == '1';
#if defined(__vita__)
    if (const char* v = std::getenv("TE_VITA_LOW_LATENCY")) g.ll_on = v[0] != '0';
    g.ll_ahead = int(std::max(0L, std::min(8L, EnvLong("TE_VITA_GPU_AHEAD", 1))));
    g.ll_render_delay_us = std::max(0L, EnvLong("TE_VITA_DIAG_RENDER_DELAY_US", 0));
    // (a window of presents for the test delay: from / to, 0 = unbounded -- the tick lock's engage-and-release test)
    g.diag_delay_from = EnvLong("TE_VITA_DIAG_RENDER_DELAY_FROM", 0);
    g.diag_delay_to = EnvLong("TE_VITA_DIAG_RENDER_DELAY_TO", 0);
    g.diag_delay_every = std::max(0L, EnvLong("TE_VITA_DIAG_RENDER_DELAY_EVERY", 0));
    if (g.rt.enabled && play)
        std::fprintf(stderr, "[te.gfx] vita: low-latency hand-over %s (TE_VITA_LOW_LATENCY): %s; the drawing thread takes a "
                     "picture once the GPU has at most %d frame%s in flight (TE_VITA_GPU_AHEAD)%s\n", g.ll_on ? "on" : "off",
                     g.ll_on ? "the game thread never waits for the drawing thread, a picture not taken yet is replaced by the next"
                             : "the game thread waits when the drawing thread is behind",
                     g.ll_ahead, g.ll_ahead == 1 ? "" : "s",
                     g.ll_render_delay_us ? " -- TEST: the drawing thread sleeps TE_VITA_DIAG_RENDER_DELAY_US per picture" : "");
#endif
    g.present_ct16 = te::GetSettings().present_ct16 || EnvFlag("TE_GFX_PRESENT_CT16");
    if (const char* v = std::getenv("TE_GFX_INTERP")) g.rt.interpolate = v[0] != '0';
    g.rt.dump_interp = EnvFlag("TE_GFX_DUMP_INTERP");
    g.rt.probe_half = EnvFlag("TE_GFX_INTERP_PROBE");
    g.rt.interp_diag = EnvFlag("TE_GFX_INTERP_DIAG");
    {
        // the interpolator's displacement check (two passes over every vertex per frame) runs for the judges
        // (TE_GFX_INTERP_CHECK=1, or any diagnosis), not for a player's frame budget
        auto opt = g.rt.interpolator.GetOptions();
        opt.check_displacement = EnvFlag("TE_GFX_INTERP_CHECK") || g.rt.interp_diag;
        opt.trace_violations = g.rt.interp_diag && EnvFlag("TE_GFX_INTERP_TRACE");
        g.rt.interpolator.SetOptions(opt);
    }
}

// ---- the render thread --------------------------------------------------------------------------------------------
// (the eager textures, below the builder pool: the decoded ones made between pictures, the device's resolver)
struct EagerTextures;
EagerTextures& Eager();
bool EagerTexturesOn();
bool MakeReadyTextures(bool (*stop)());
int ResolveTexture(void*, uint32_t id);
void DropPending(uint32_t id);
void DropAllPending();
void SweepTextureFailures();
std::string EagerCensusLine();
enum class Show { Present, Capture, None };   // a frame shown; resolved for the armed capture only; dropped
void RenderFrame(Gfx::Frame& f, Show show);
void RenderBetween(Gfx::Frame& prev, Gfx::Frame& cur, double fraction, int64_t slot_ns);
void ProbeHalf(Gfx::Frame& prev, Gfx::Frame& cur);
using Steady = std::chrono::steady_clock;
int64_t NowNs() { return ps2r::ps2e::PresentPacer::NowNs(); }
bool CaptureArmed() { return !g.dump_dir.empty() || (te::harness::Armed() && g.harness_keys.empty()); }
// A keyed harness (TE_HARNESS_KEYS): this picture is one of the wanted ones
bool HarnessWants(uint64_t key) {
    if (g.harness_keys.empty()) return te::harness::Armed();
    for (uint64_t k : g.harness_keys) if (k == key) return true;
    return false;
}
using Rate = ps2r::ps2e::PresentPacer::Rate;

// The presenter's picture bookkeeping on the render thread (the mutex held): the arrived slot becomes the newest
// picture, the one before it the previous; the one before that goes back to the game thread.
void RetainPicture(int slot) {
    auto& rt = g.rt;
    if (rt.prev >= 0) {
        // no picture the presenter can still show draws a mesh retired before this one
        for (uint32_t mesh : rt.slots[rt.prev].retire) ps2e_draw_mesh_destroy(g.dev, mesh);
        rt.slots[rt.prev].retire.clear();
        rt.slots[rt.prev].retired_storage.clear();   // (no picture still to be built names it: its builds are all done)
        rt.free.push_back(rt.prev);
    }
    rt.prev = rt.cur;
    rt.cur = slot;
}

// The shown content's time (the pacer's clock) against the present instant: the latency census
void NoteLatency(int64_t content_ns, int64_t now_ns) {
    auto& rt = g.rt;
    const double ms = double(now_ns - content_ns) / 1e6;
    rt.latency_sum_ms += ms;
    rt.latency_n++;
    if (ms > rt.latency_max_ms) rt.latency_max_ms = ms;
}

void RenderLoop() {
    auto& rt = g.rt;
    // with the game thread: interactive work (the engine's per-platform arm: macOS QoS, Windows SetThreadPriority)
    if (!std::getenv("TE_NO_QOS")) ps2r::platform::host::PinCurrentThreadInteractive("te-render", ps2r::platform::host::ThreadRole::Graphics);
    te::prof::BindThread(te::prof::kRender);
    if (!ps2e_draw_bind_thread(g.dev, 1)) std::fprintf(stderr, "[te.gfx] render thread: the GL context did not bind\n");
    ps2r::host::RaiseTimerResolution();   // the slot trains at 120 / 144 / 240 (Windows: the 1 ms period + the waitable timer)
    for (;;) {
        std::unique_lock<std::mutex> lock(rt.mutex);
        // The cadence: the setting as it stands at this picture (the pacer re-set when it changed)
        const int rate_index = g.frame_rate.load(std::memory_order_relaxed);
        if (rate_index != rt.pacer_rate) {
            rt.pacer_rate = rate_index;
            rt.pacer.SetRate(static_cast<Rate>(rate_index));
            g.interp_active.store(rt.interpolate && ps2r::ps2e::PresentPacer::PacedBySlots(static_cast<Rate>(rate_index)), std::memory_order_relaxed);
            rt.interpolator.ResetHistory();
            std::fprintf(stderr, "[te.gfx] presenter: frame rate %s%s\n", ps2r::ps2e::PresentPacer::Name(static_cast<Rate>(rate_index)),
                         ps2r::ps2e::PresentPacer::PacedBySlots(static_cast<Rate>(rate_index)) ? (rt.interpolate ? " (the frames between two pictures interpolated)" : " (the newest picture repeated between pictures)") : "");
        }
        const Rate rate = static_cast<Rate>(rt.pacer_rate);
        const bool slots = ps2r::ps2e::PresentPacer::PacedBySlots(rate) && rt.cur >= 0 && rt.slots[rt.cur].have_edge;
        // Wait for work: a job, a picture, or (slot-paced) the next slot's time less a millisecond
        if (slots) {
            const int64_t slot = rt.pacer.NextSlotNs(NowNs());
            const int64_t wake_at = slot - 1000000;
            rt.wake.wait_until(lock, Steady::time_point(std::chrono::nanoseconds(wake_at)),
                               [&] { return rt.stop || !rt.queue.empty() || !rt.jobs.empty() || NowNs() >= wake_at; });
        } else {
            TE_PROF_SCOPE("rt idle (no picture)");
            rt.wake.wait(lock, [&] { return rt.stop || !rt.queue.empty() || !rt.jobs.empty() || rt.textures_ready; });
        }
        if (rt.stop) break;   // (the slot cadence would otherwise present forever)
        if (!rt.jobs.empty()) {
            std::vector<std::function<void()>> jobs = std::move(rt.jobs);
            rt.jobs.clear();
            lock.unlock();
            for (auto& job : jobs) {
                const int64_t j0 = te::load::NowNs();
                job();
                te::load::NoteRenderJob(double(te::load::NowNs() - j0) / 1e6);
            }
            lock.lock();
            rt.jobs_done += jobs.size();
            rt.done.notify_all();
            continue;
        }
        if (!rt.queue.empty()) {
#if defined(__vita__)
            // The low-latency hand-over: the picture is taken once the GPU has room for it (at most ll_ahead frames of
            // its own in flight) -- meanwhile the game thread may replace it with a newer one (PresentFrame), so the
            // picture drawn is the newest when the GPU can start on it. The jobs posted meanwhile run first (the loop's top).
            // (a picture of the tick lock -- 30 / 20 Hz: te_tick_lock.h -- is taken at once: it is never replaced, and its
            // display slot is its lock's refreshes away)
            if (LowLatency() && rt.slots[rt.queue.front()].lock_fields <= 1) {
                // (a safety net: a wait longer than 100 ms takes the picture anyway -- a fence that does not move must
                // not stall the presenter -- and is counted)
                static int64_t wait_from = 0;
                const int in_flight = te::vita_gpu::FramesInFlight();
                const int64_t now = NowNs();
                if (in_flight > g.ll_ahead && wait_from && now - wait_from > 100000000LL) {
                    g.ll_gpu_timeouts.fetch_add(1, std::memory_order_relaxed);
                    wait_from = 0;
                } else if (in_flight > g.ll_ahead) {
                    if (!wait_from) wait_from = now;
                    lock.unlock();
                    {
                        TE_PROF_SCOPE("rt wait for the GPU (low-latency hand-over)");
                        sceKernelDelayThread(500);
                    }
                    g.ll_gpu_wait_us.fetch_add(uint64_t((NowNs() - now) / 1000), std::memory_order_relaxed);
                    lock.lock();
                    continue;
                } else {
                    wait_from = 0;
                }
            }
#endif
            // A picture arrived
            const int slot = rt.queue.front();
            rt.queue.erase(rt.queue.begin());
            rt.drawing = true;
            RetainPicture(slot);
            Gfx::Frame& f = rt.slots[slot];
            if (f.have_edge) {   // its age, and the delay the fraction's clock keeps (the 90th percentile over the last 64)
                rt.arrival_ages[rt.arrival_count % 64] = NowNs() - f.edge_ns;
                rt.arrival_count++;
                const int n = std::min(rt.arrival_count, 64);
                int64_t sorted[64];
                std::copy(rt.arrival_ages, rt.arrival_ages + n, sorted);
                std::sort(sorted, sorted + n);
                const int64_t period = 1000000000LL / 60;
                const int64_t p90 = sorted[(n * 9) / 10 >= n ? n - 1 : (n * 9) / 10];
                // the delay = the arrival age (its 90th percentile): the shown content then runs exactly one picture plus the
                // arrival behind the newest EDGE, so every slot between two arrivals falls inside the pair (no frame shown twice)
                // -- the one tick of latency the interpolation costs, and no more (clamped at a period)
                rt.delay_ns = std::clamp<int64_t>(p90, 0, period);
                rt.delay_ms_max = std::max(rt.delay_ms_max, double(rt.delay_ns) / 1e6);
            }
            lock.unlock();
            const int64_t taken_ns = NowNs();   // the drawing thread's work on this picture from here (the tick lock's census)
#if defined(__vita__)
            // TE_VITA_DIAG_RENDER_DELAY_US (a test knob): a slow drawing thread / GPU, to see the hand-over's behaviour
            // (TE_VITA_DIAG_RENDER_DELAY_FROM / _TO: only over those presents)
            if (g.ll_render_delay_us > 0) {
                const long p = long(g.presents.load(std::memory_order_relaxed));
                // (TE_VITA_DIAG_RENDER_DELAY_EVERY=N: one picture in N only -- a periodic hiccup, the census's own stall's kind)
                if ((g.diag_delay_from <= 0 || p >= g.diag_delay_from) && (g.diag_delay_to <= 0 || p <= g.diag_delay_to) &&
                    (g.diag_delay_every <= 1 || (p - std::max(0L, g.diag_delay_from)) % g.diag_delay_every == 0))
                    sceKernelDelayThread(SceUInt(g.ll_render_delay_us));
            }
#endif
            // the frame's builds, done by the workers meanwhile: a picture presented as it arrives takes them bucket by
            // bucket as it submits (SubmitBuckets), the others here
            const bool lazy = !ps2r::ps2e::PresentPacer::PacedBySlots(rate) && rt.pacer.ShowsPicture(f.picture);
            if (!lazy) ExpandDeferred(f.buckets, f.deferred, f.deferred_used);
            if (ps2r::ps2e::PresentPacer::PacedBySlots(rate)) {
                // the cadence is the slot train's: the picture is resolved for a judge's capture only (its hash and dump are
                // the same at every rate); the slots show it, and the frames between it and the previous one
                if (f.have_edge) rt.pacer.OnTickEdge(f.edge_ns);
                RenderFrame(f, CaptureArmed() ? Show::Capture : Show::None);
                if (rt.probe_half && rt.prev >= 0 && rt.slots[rt.prev].send && f.send) ProbeHalf(rt.slots[rt.prev], f);
            } else if (rt.pacer.ShowsPicture(f.picture)) {
                // 60 (every picture) and the shown half of 30: the picture presented as it arrives
                const int64_t now = NowNs();
                RenderFrame(f, Show::Present);
                ExpandDeferred(f.buckets, f.deferred, f.deferred_used);   // any left (a picture not sent)
                if (f.have_edge) NoteLatency(f.edge_ns, now);
                {
                    // the tick lock's census and its rise test: the picture's work here, the present call itself excluded
                    const double busy = double(NowNs() - taken_ns) / 1e6;
                    te::tick_lock::NotePictureDrawn(std::max(0.0, busy - g.last_present_call_ms), g.last_present_call_ms);
                }
            } else {
                RenderFrame(f, CaptureArmed() ? Show::Capture : Show::None);   // 30: the picture between two shown ones
            }
#if defined(__vita__)
            // the state pool (StatePool): this picture is finished with -- expanded and drawn, or set aside never to be drawn.
            // (A slot-paced presenter may submit a picture later, RenderBetween: then nothing is published -- the pool's
            // entries are never remade, it fills and the records are whole)
            if (!ps2r::ps2e::PresentPacer::PacedBySlots(rate)) g_pool.done.store(f.pool_ordinal, std::memory_order_release);
#endif
            lock.lock();
            rt.drawing = false;
            rt.frames++;
            rt.done.notify_all();
            continue;
        }
        if (rt.textures_ready && !slots) {
            // (no job, no picture) the textures the workers decoded made now, between pictures: one at a time, a picture or a
            // job that arrives stops it (the rest at the next idle moment, or at their first draw)
            rt.textures_ready = false;
            lock.unlock();
            bool left;
            {
                TE_PROF_SCOPE("rt textures made between pictures");
                left = MakeReadyTextures([] {
                    auto& r = g.rt;
                    std::lock_guard<std::mutex> l(r.mutex);
                    return r.stop || !r.queue.empty() || !r.jobs.empty();
                });
            }
            lock.lock();
            if (left) rt.textures_ready = true;
            continue;
        }
        if (slots) {
            const int64_t now = NowNs();
            const int64_t slot_ns = rt.pacer.NextSlotNs(now);
            if (now < slot_ns) {
                lock.unlock();
                ps2r::ps2e::PresentPacer::SleepUntilNs(slot_ns);   // the last millisecond, finely
                lock.lock();
                if (!rt.queue.empty() || !rt.jobs.empty() || rt.stop) continue;   // a picture arrived meanwhile: it first
            }
            // The frame for this slot: between the previous picture and the newest at the fraction of the pacer's clock --
            // fraction = (now - edge of the newest) / period, clamped to [0, 1]: the shown content is one tick behind the
            // newest picture (the latency the interpolation costs), never ahead of it
            const int cur = rt.cur, prev = rt.prev;
            rt.drawing = true;
            lock.unlock();
            const int64_t t = NowNs();
            const int64_t period = 1000000000LL / 60;   // the game's tick (the FieldPacer's hz is the clock's; 60 by default)
            double fraction = 1.0;
            if (rt.interpolate && prev >= 0 && rt.slots[prev].have_edge && rt.slots[cur].send && rt.slots[prev].send) {
                fraction = double(t - rt.delay_ns - rt.slots[cur].edge_ns) / double(period);
                if (fraction < 0.0) fraction = 0.0;
                if (fraction > 1.0) fraction = 1.0;
            }
            if (rt.interp_diag && rt.slots[cur].picture % 200 == 0)
                std::fprintf(stderr, "[te.gfx.interp] slot: picture %llu edge %s age %.2f ms fraction %.3f prev %d send %d/%d\n", (unsigned long long)rt.slots[cur].picture,
                             rt.slots[cur].have_edge ? "yes" : "NO", rt.slots[cur].have_edge ? double(t - rt.slots[cur].edge_ns) / 1e6 : -1.0, fraction, prev,
                             int(prev >= 0 && rt.slots[prev].send), int(rt.slots[cur].send));
            if (fraction >= 0.97) fraction = 1.0;   // within a third of a millisecond of the newest picture: the picture itself
            RenderBetween(prev >= 0 ? rt.slots[prev] : rt.slots[cur], rt.slots[cur], fraction, slot_ns);
            lock.lock();
            rt.pacer.SlotServed(slot_ns, t);
            rt.drawing = false;
            rt.done.notify_all();
            continue;
        }
        if (rt.stop) break;
    }
    ps2e_draw_bind_thread(g.dev, 0);
}
bool OnRenderThread() { return g.rt.started && std::this_thread::get_id() == g.rt.thread.get_id(); }
// Runs fn on the render thread and waits for it (inline when there is none, or when already on it)
void OnRender(const std::function<void()>& fn) {
    auto& rt = g.rt;
    if (!rt.started || OnRenderThread()) { fn(); return; }
    uint64_t ticket;
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        rt.jobs.push_back(fn);
        ticket = ++rt.jobs_posted;
    }
    rt.wake.notify_one();
    std::unique_lock<std::mutex> lock(rt.mutex);
    rt.done.wait(lock, [&] { return rt.jobs_done >= ticket; });
}
// OnRender without the wait (te_graphics_post_render): RenderLoop runs the posted jobs before the next queued picture
void PostRender(std::function<void()> fn) {
    auto& rt = g.rt;
    if (!rt.started || OnRenderThread()) { fn(); return; }
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        rt.jobs.push_back(std::move(fn));
        ++rt.jobs_posted;
    }
    rt.wake.notify_one();
}
void StartRenderThread() {
    auto& rt = g.rt;
    if (!rt.enabled || rt.started || !g.dev) return;
    if (!ps2e_draw_bind_thread(g.dev, 0)) {   // no context to hand over (a headless device without GL): stay synchronous
        rt.enabled = false;
        return;
    }
    for (int i = 0; i < Gfx::RenderThread::kSlots; ++i) rt.free.push_back(i);
    rt.started = true;
    rt.thread = std::thread(RenderLoop);
}

void PrimeThreadSync();
bool EnsureDevice() {
    ReadOptions();
    if (!g.summary_armed) {
        g.summary_armed = true;
        std::atexit(Summary);
    }
    if (g.dev) return true;
    if (g.create_failed) return false;
    ps2e_draw_config cfg{};
    cfg.frame_width = g.display_w;
    cfg.frame_height = g.display_h;
    // twice the frame's lines in the TV's 4:3 (the shape `auto` shows in a window); the window fits it to the screen
    cfg.window_height = g.display_h * 2;
    cfg.window_width = (cfg.window_height * 4 + 1) / 3;
    cfg.headless = g.headless ? 1 : 0;
    cfg.title = "TwinsanityEvolved";
    // the threads' locks made before the device's memory is (the Vita: below)
    PrimeThreadSync();
    // PS2E_DRAW_STENCIL_DIAG=1 (the cryscave flicker diagnosis): the engine logs the stencil's histogram at the shadow bracket's
    // clear and composite. The toggle lives here because the engine reads no environment outside its config loader.
    cfg.stencil_diag = EnvFlag("PS2E_DRAW_STENCIL_DIAG") ? 1 : 0;
    // The depth: the title's screen matrix maps the clip volume onto Z = GsDepthRange * (1 - z/w) in [0, 2^24) (frame.cpp
    // SetUpRenderTarget; the 2D draws sit at kGsDepthFront = 2^24 - 1), and its ZBUF is PSMZ32 -- the GS compares those
    // integers exactly, so the frame's depth keeps every unit (window depth z / 2^24). The engine's default z / 2^32
    // resolved 128- or 256-unit steps: near-coplanar passes (Ice Climb's caustics on the cave walls, the water's ripples
    // around the floating ice) z-fought their surfaces, and which triangles lost changed as the camera moved.
    // TE_GFX_DEPTH_Z32=1 (diagnosis, A/B) keeps the old mapping.
    cfg.depth_bits = EnvFlag("TE_GFX_DEPTH_Z32") ? 32 : 24;
    g.dev = ps2e_draw_create(&cfg);
    if (!g.dev) {
        g.create_failed = true;
        std::fprintf(stderr, "[te.gfx] ps2e_draw_create failed: no window / GL context; graphics calls are dropped\n");
        return false;
    }
    te::display::Apply();   // the display settings read before the device existed
    std::fprintf(stderr, "[te.gfx] %s\n", te::display::Describe().c_str());
    if (te::prof::DrawBenchWanted()) te::prof::DrawBench(g.dev);   // TE_PROF_BENCH=2: before any frame (the GL thread is this one)
    // TE_GFX_MESH_TEXSWAP=0: a mesh draw that differs from the one before in its texture only takes the full path; =1 the
    // short path for a texture change only; 2 (the default) also when the alpha test / mipmapping differ
    if (const char* v = std::getenv("TE_GFX_MESH_TEXSWAP")) ps2e_draw_set_mesh_texture_swap(g.dev, v[0] >= '0' && v[0] <= '2' ? v[0] - '0' : 2);
#if defined(__vita__)
    // a frame's buckets (their vertices, the builds they point into) live until the frame has been drawn: the device may
    // keep pointers to them (the shadow casters it draws again at the composite) instead of copies
    ps2e_draw_set_retained_vertices(g.dev, 1);
    // the load census's GPU memory allocations per tick (vitaGL's pool allocator, counted by the census wrappers)
    te::load::SetGpuAllocSource([] {
        te::load::GpuAllocs a;
        te::vita_gpu::AllocsSoFar(&a.allocs, &a.bytes, &a.frees, &a.us);
        return a;
    });
#endif
    // the eager textures (TE_GFX_EAGER_TEXTURES): a draw that names a texture not made yet makes it (ResolveTexture)
    if (EagerTexturesOn()) ps2e_draw_set_texture_resolver(g.dev, ResolveTexture, nullptr);
    StartRenderThread();
    if (g.present_ct16) te_graphics_set_present_ct16(1);
    // the judges' dumps / the harness leg read the present back (a keyed harness arms the capture per picture instead)
    if (!g.dump_dir.empty() || (te::harness::Armed() && g.harness_keys.empty())) OnRender([] { ps2e_draw_arm_capture(g.dev, 1); });
    std::fprintf(stderr, "[te.gfx] renderer started: frame %dx%d (%s), %s, render thread %s\n", g.display_w, g.display_h,
                 g.pal ? "PAL" : "NTSC", g.headless ? "headless" : "window", g.rt.started ? "on" : "off");
    return true;
}

// The frame's screen size the game told StartFrame (the frame target), and the GS origin in frame pixels
int ScreenW() { return g.have_frame && g.frame.screenWidth > 0 ? g.frame.screenWidth : g.display_w; }
int ScreenH() { return g.have_frame && g.frame.screenHeight > 0 ? g.frame.screenHeight : g.display_h; }
float GsOriginX() { return kGsMiddle - float(ScreenW() >> 1); }
float GsOriginY() { return kGsMiddle - float(ScreenH() >> 1); }

// The current render target (the game's g_RenderTarget) or null
const RenderTargetDescription* CurrentTarget() {
    const uint32_t* p = Guest<uint32_t>(te::RetailAddress(kRenderTargetPointer));
    if (!p || !*p) return nullptr;
    const auto* t = Guest<RenderTargetDescription>(*p);
    if (!t || t->displayWidth <= 0 || t->displayWidth > 4096 || t->displayHeight <= 0 || t->displayHeight > 4096) return nullptr;
    return t;
}
// draw2d.cpp FrameCorner: the frame's corner along a side, in GS pixels
float FrameCornerX(int offset) {
    const RenderTargetDescription* t = CurrentTarget();
    const int display = t ? t->displayWidth : g.frame.screenWidth;
    return float(int(kGsMiddle - float(display >> 1)) + offset);
}
float FrameCornerY(int offset) {
    const RenderTargetDescription* t = CurrentTarget();
    const int display = t ? t->displayHeight : g.frame.screenHeight;
    return float(int(kGsMiddle - float(display >> 1)) + offset);
}
int TargetOffsetX() {
    const RenderTargetDescription* t = CurrentTarget();
    return t ? t->offsetX : 0;
}
int TargetOffsetY() {
    const RenderTargetDescription* t = CurrentTarget();
    return t ? t->offsetY : 0;
}
// The game's g_ScreenWidth/Height (the UI's scale), the frame's when unset
float GameScreenW() {
    const int32_t* p = Guest<int32_t>(te::RetailAddress(kScreenWidth));
    return p && *p > 0 && *p <= 4096 ? float(*p) : float(ScreenW());
}
float GameScreenH() {
    const int32_t* p = Guest<int32_t>(te::RetailAddress(kScreenHeight));
    return p && *p > 0 && *p <= 4096 ? float(*p) : float(ScreenH());
}

// ---- materials -> render state --------------------------------------------------------------------------------------------
MaterialView ReadMaterial(uint32_t address) {
    MaterialView v;
    v.material = Guest<te_gfx_material>(address);
    if (!v.material || v.material->magic != TE_GFX_MATERIAL_MAGIC) {
        v.material = nullptr;
        return v;
    }
    v.bucket = v.material->bucket < TE_GFX_BUCKET_COUNT ? v.material->bucket : TE_GFX_BUCKET_UI;
    if (v.material->shaderCount && v.material->shaders[0]) {
        v.shader = Guest<te_gfx_shader>(v.material->shaders[0]);
        if (v.shader && v.shader->magic != TE_GFX_SHADER_MAGIC) v.shader = nullptr;
    }
    if (v.shader && v.shader->texture) {
        v.texture = Guest<te_gfx_texture>(v.shader->texture);
        v.texture_handle = TextureHandleOf(v.shader->texture);
    }
    return v;
}

MaterialView ShaderView(const MaterialView& m, uint32_t k) {
    MaterialView v;
    v.material = m.material;
    v.bucket = m.bucket;
    if (!v.material || k >= v.material->shaderCount || k >= TE_GFX_MAX_SHADERS || !v.material->shaders[k]) return v;
    v.shader = Guest<te_gfx_shader>(v.material->shaders[k]);
    if (v.shader && v.shader->magic != TE_GFX_SHADER_MAGIC) v.shader = nullptr;
    if (v.shader && v.shader->texture) {
        v.texture = Guest<te_gfx_texture>(v.shader->texture);
        v.texture_handle = TextureHandleOf(v.shader->texture);
    }
    return v;
}

void AlphaPreset(uint32_t preset, ps2e_draw_state& st) {
    // InitAlphaPresets (shadersettings.cpp): (A - B) * C + D over Cs=0 Cd=1 Zero=2 / As=0 Ad=1 FIX=2
    switch (preset) {
        case TE_GFX_PRESET_MIX: st.blend_a = 0; st.blend_b = 1; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_ADD: st.blend_a = 0; st.blend_b = 2; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_SUBTRACT: st.blend_a = 2; st.blend_b = 0; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_BRIGHTEN: st.blend_a = 1; st.blend_b = 2; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_DARKEN: st.blend_a = 2; st.blend_b = 1; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_SCALE: st.blend_a = 1; st.blend_b = 2; st.blend_c = 0; st.blend_d = 2; break;
        default: st.blend_a = 0; st.blend_b = 2; st.blend_c = 2; st.blend_d = 2; st.blend_fix = 0x80; break;   // the source as it is
    }
}

// The first shader's settings as the draw state (2D: the texture when the shader has one)
// A decoded texture as a device texture with its levels (on the drawing thread)
uint32_t CreateTexture(const te::gfx::DecodedTexture& decoded) {
    const int64_t t0 = te::load::NowNs();
    const uint32_t handle = ps2e_draw_texture_create_gpu_alpha(g.dev, decoded.width, decoded.height, decoded.rgba.data());
    if (handle && !decoded.mips.empty()) {
        std::vector<const void*> levels;
        for (const auto& level : decoded.mips) levels.push_back(level.data());
        ps2e_draw_texture_set_mips(g.dev, handle, int(levels.size()), levels.data());
    }
    te::load::NoteRenderTexture(double(te::load::NowNs() - t0) / 1e6);
    return handle;
}
// The RGBA bytes of a decoded texture and its levels (the load census)
uint64_t DecodedBytes(const te::gfx::DecodedTexture& decoded) {
    uint64_t n = uint64_t(decoded.rgba.size()) * 4;
    for (const auto& level : decoded.mips) n += uint64_t(level.size()) * 4;
    return n;
}

// The shader types whose settings minify through a texture's mips (shadersettings.cpp: the standard styles and the 2D
// shader's own; the unfiltered ones, the sky and type 0 do not)
bool MinifiesThroughMips(uint32_t type) {
    switch (type) {
        case 0x01: case 0x02: case 0x04: case 0x0D: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19:
        case 0x1A: case 0x1B: case 0x1E: case 0x1F: case 0x20:
            return true;
        default:
            return false;
    }
}

ps2e_draw_state StateOf(const MaterialView& m) {
    ps2e_draw_state st{};
    st.gouraud = 1;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = 0;
    st.tcc = 1;
    if (!m.shader) return st;
    const uint64_t s = m.shader->settings;
    st.gouraud = TE_GFX_S_GOURAUD(s) ? 1 : 0;
    st.depth_test = int(TE_GFX_S_DEPTH_TEST(s));
    st.depth_write = TE_GFX_S_NO_DEPTH_WRITES(s) ? 0 : 1;
    st.blend = TE_GFX_S_BLENDS(s) ? 1 : 0;
    if (st.blend) {
        if (TE_GFX_S_OWN_ALPHA(s)) {
            st.blend_a = uint8_t(TE_GFX_S_ALPHA_A(s));
            st.blend_b = uint8_t(TE_GFX_S_ALPHA_B(s));
            st.blend_c = uint8_t(TE_GFX_S_ALPHA_C(s));
            st.blend_d = uint8_t(TE_GFX_S_ALPHA_D(s));
            st.blend_fix = uint8_t(TE_GFX_S_ALPHA_FIX(s));
        } else {
            AlphaPreset(TE_GFX_S_PRESET(s), st);
        }
    }
    st.alpha_test = TE_GFX_S_ALPHA_TEST(s) ? 1 : 0;
    st.alpha_method = int(TE_GFX_S_ALPHA_METHOD(s));
    st.alpha_reference = uint8_t(TE_GFX_S_ALPHA_REFERENCE(s));
    st.alpha_fail = int(TE_GFX_S_ALPHA_FAIL(s));
    st.linear = TE_GFX_S_LINEAR(s) ? 1 : 0;
    // the material's FBA (shadersettings.cpp FrameBufferAlpha: FBA = !noFba) marks the frame's alpha top bit --
    // the shadows' volumes are counted only where it is 0 (StartShadows' DATE), so a caster material with FBA is never
    // darkened (most casters write alpha 0x80 anyway; the lit world writes 0x7E)
    st.fba = TE_GFX_S_NO_FBA(s) ? 0 : 1;
    // TE_GFX_DIAG_FBA=1: each shader's settings once (type, FBA, blend, DATE) -- which materials receive shadows (read once
    // into a plain value: a function-local static's guard was a DMB barrier per draw's state on the Vita; the game thread's)
    static int diagFba = -1;
    if (diagFba < 0) diagFba = std::getenv("TE_GFX_DIAG_FBA") != nullptr ? 1 : 0;
    if (diagFba) {
        static std::unordered_map<const te_gfx_shader*, bool> said;
        if (!said[m.shader]) {
            said[m.shader] = true;
            std::fprintf(stderr, "[te.gfx] shader %p type %#x settings %#llx: fba %d blends %d preset %llu date %llu/%llu textured %d tcc %d\n",
                         (const void*)m.shader, unsigned(m.shader->type), (unsigned long long)s, st.fba, st.blend,
                         (unsigned long long)TE_GFX_S_PRESET(s), (unsigned long long)TE_GFX_S_DESTINATION_TEST(s),
                         (unsigned long long)TE_GFX_S_DESTINATION_MODE(s), TE_GFX_S_TEXTURED(s) ? 1 : 0,
                         m.texture ? (m.texture->texture.hasAlpha ? 1 : 0) : -1);
        }
    }
    if (TE_GFX_S_TEXTURED(s) && m.texture_handle) {
        st.texture = m.texture_handle;
        st.tfx = m.texture ? (m.texture->texture.function & 3) : PS2E_DRAW_TFX_MODULATE;
        st.tcc = m.texture ? (m.texture->texture.hasAlpha ? 1 : 0) : 1;
        st.wrap_u = st.wrap_v = PS2E_DRAW_WRAP_REPEAT;
        // TEX1 as shadersettings.cpp wrote it: a mipmapped texture minifies LINEAR_MIPMAP_NEAREST through its levels
        // (MXL its last, the shader's K and L, the LOD per pixel from Q) under the standard settings and the 2D
        // shader's; the unfiltered, sky and type-0 settings minify with the magnifying filter, level 0 only
        if (m.texture && m.texture->texture.levels > 1 && MinifiesThroughMips(m.shader->type)) {
            st.mip_max_level = m.texture->texture.levels - 1;
            st.mip_k = m.shader->lodK;
            st.mip_l = m.shader->lodL;
            st.mip_minify = 4;   // GS_TEX_LINEAR_MIPMAP_NEAREST
            if (m.shader->type == 0x0D) st.linear = 1;   // the 2D shader magnifies linear whatever its settings say
        }
    }
    return st;
}

ps2e_draw_vertex Vertex(float x, float y, float u, float v, uint32_t colour) {
    ps2e_draw_vertex o{};
    o.x = x;
    o.y = y;
    o.z = kGsDepthFront;
    o.u = u;
    o.v = v;
    o.r = uint8_t(colour);
    o.g = uint8_t(colour >> 8);
    o.b = uint8_t(colour >> 16);
    o.a = uint8_t(colour >> 24);
    return o;
}

// two draws merge when every field the backend reads agrees (field-wise: the struct has padding)
bool SameState(const ps2e_draw_state& a, const ps2e_draw_state& b) {
    if (a.stencil != b.stencil) return false;
    if (a.fba != b.fba || a.dest_alpha_test != b.dest_alpha_test) return false;
    if (a.mip_max_level != b.mip_max_level || a.mip_k != b.mip_k || a.mip_l != b.mip_l || a.mip_minify != b.mip_minify) return false;
    return a.texture == b.texture && a.gouraud == b.gouraud && a.depth_test == b.depth_test && a.depth_write == b.depth_write &&
           a.blend == b.blend && a.blend_a == b.blend_a && a.blend_b == b.blend_b && a.blend_c == b.blend_c &&
           a.blend_d == b.blend_d && a.blend_fix == b.blend_fix && a.alpha_test == b.alpha_test &&
           a.alpha_method == b.alpha_method && a.alpha_reference == b.alpha_reference && a.alpha_fail == b.alpha_fail &&
           a.linear == b.linear && a.wrap_u == b.wrap_u && a.wrap_v == b.wrap_v && a.tfx == b.tfx && a.tcc == b.tcc &&
           a.scissor_x0 == b.scissor_x0 && a.scissor_y0 == b.scissor_y0 && a.scissor_x1 == b.scissor_x1 &&
           a.scissor_y1 == b.scissor_y1 && a.no_colour_write == b.no_colour_write;
}

Draw& OpenDraw(uint32_t bucket, const ps2e_draw_state& state, bool copy_frame = false) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    g.draws++;
    // a draw asking for a fresh frame copy never joins the previous one (its copy would predate that draw's triangles)
    if (b.used && (state.blend == 0 || g.join_blended.load(std::memory_order_relaxed)) && !copy_frame) {
        Draw& last = b.draws[b.used - 1];
        if (last.verts.size() >= 3 && SameState(last.state, state)) {
            // while the presenter interpolates, a draw joins the previous one only within the same object (a rigid object's
            // submodels: one identity) -- two objects in one draw could not be moved apart
            const bool same_object = !g.interp_active.load(std::memory_order_relaxed) ||
                                     (last.tag.id == g.tag.id && last.tag.kind == g.tag.kind &&
                                      (g.tag.kind != ps2r::ps2e::InterpKind::Vertices || last.tag.part == g.tag.part));
            if (same_object) {
                g.draws_merged++;
                return last;
            }
        }
    }
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
#if defined(__vita__)
    MarkHolds(b, b.used - 1);   // (its producer appends vertices: ClearBuckets trims it)
#endif
    d.state = state;
    d.verts.clear();
    d.ext = nullptr;
    d.ext_count = 0;
    d.copy_frame = copy_frame;
    d.pending = nullptr;
    d.mesh = 0;
    d.pool = 0;
    d.tag = g.tag;
    d.has_transform = g.tag_has_transform;
    if (g.tag_has_transform) d.transform = g.tag_transform;
    return d;
}

void SetDrawTag(const ps2r::ps2e::InterpTag& tag, const float* world16, const float* screen16) {
    g.tag = tag;
    g.tag_untagged = tag.id == 0 && tag.part == 0 && tag.kind == ps2r::ps2e::InterpKind::Newest;
    g.tag_has_transform = world16 && screen16;
    if (g.tag_has_transform) {
        std::memcpy(g.tag_transform.world, world16, sizeof g.tag_transform.world);
        std::memcpy(g.tag_transform.screen, screen16, sizeof g.tag_transform.screen);
    }
}
void ClearDrawTag() {
    g.tag = ps2r::ps2e::InterpTag{};
    g.tag_untagged = true;
    g.tag_has_transform = false;
}

void CloseDraw(uint32_t bucket) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    if (!b.used) return;
    Draw& d = b.draws[b.used - 1];
    d.verts.resize(d.verts.size() / 3 * 3);   // whole triangles only (a merged tail must not shift the next draw's)
    if (d.verts.size() < 3) {
        d.verts.clear();
        b.used--;
        g.draws--;
    }
}

void Record(uint32_t bucket, Draw&& d) {
    if (d.verts.size() < 3) return;
    Draw& open = OpenDraw(bucket, d.state);
    open.verts.insert(open.verts.end(), d.verts.begin(), d.verts.end());
    CloseDraw(bucket);
}

// ---- the builds off the game thread (graphics_internal.h DeferredBuild) ---------------------------------------------------
// A few workers (TE_GFX_BUILD_THREADS; by default the logical processors less the game, render and audio threads, from one
// to four, none on a machine of two) take the posted builds in order.
// The build pool's lock and its two conditions. On the Vita they are the kernel's lightweight mutex and conditions with
// their work areas IN the pool (a static, in .bss): vitaGL maps the whole newlib heap into the GPU's view, and Vita3K's
// tracking of the GPU's pages made the heap page under a pthread mutex unreadable mid-game (the pool's lock faulted at
// the hub's loading). Elsewhere the standard ones.
#if defined(__vita__)
class PoolMutex {
public:
    PoolMutex() { sceKernelCreateLwMutex(&work_, "te-build-pool", 0, 0, nullptr); }
    ~PoolMutex() { sceKernelDeleteLwMutex(&work_); }
    PoolMutex(const PoolMutex&) = delete;
    PoolMutex& operator=(const PoolMutex&) = delete;
    void lock() { sceKernelLockLwMutex(&work_, 1, nullptr); }
    void unlock() { sceKernelUnlockLwMutex(&work_, 1); }
    bool try_lock() { return sceKernelTryLockLwMutex(&work_, 1) == 0; }
    SceKernelLwMutexWork* Work() { return &work_; }
private:
    alignas(8) SceKernelLwMutexWork work_;
};
class PoolCondition {
public:
    explicit PoolCondition(PoolMutex& m) { sceKernelCreateLwCond(&work_, "te-build-pool-cond", 0, m.Work(), nullptr); }
    ~PoolCondition() { sceKernelDeleteLwCond(&work_); }
    PoolCondition(const PoolCondition&) = delete;
    PoolCondition& operator=(const PoolCondition&) = delete;
    void wait(std::unique_lock<PoolMutex>&) { sceKernelWaitLwCond(&work_, nullptr); }
    template <typename Pred>
    void wait(std::unique_lock<PoolMutex>& lock, Pred pred) { while (!pred()) wait(lock); }
    void wait_for(std::unique_lock<PoolMutex>&, std::chrono::milliseconds ms) {
        SceUInt32 us = SceUInt32(ms.count() * 1000);
        sceKernelWaitLwCond(&work_, &us);
    }
    void notify_one() { sceKernelSignalLwCond(&work_); }
    void notify_all() { sceKernelSignalLwCondAll(&work_); }
private:
    alignas(8) SceKernelLwCondWork work_;
};
#else
using PoolMutex = std::mutex;
class PoolCondition : public std::condition_variable {
public:
    explicit PoolCondition(PoolMutex&) {}
};
#endif

struct BuildPool {
    // A job: the build's work, the placeholder it fills (done when the work has run) and the poster's floating-point
    // environment (the game thread's: the EE's rounding toward zero), applied around the work
    struct Job {
        BuildJob fn;
        DeferredBuild* build = nullptr;
        std::fenv_t env{};
    };
    std::vector<std::thread> threads;
    PoolMutex mutex;
    PoolCondition wake{mutex}, finished{mutex};
    std::deque<Job> jobs;
    std::atomic<size_t> active{0};
    bool stop = false, started = false;
    std::atomic<int64_t> waited_ns{0}, built_ns{0};   // the waiting thread's time in Wait / Drain; the builds' own time
    // The lean traffic (TE_GFX_POOL_LEAN, default on): every lock, condition signal and clock read is a kernel call on
    // the Vita (a trap into the emulator's kernel in Vita3K), and the previous protocol made about a dozen per build.
    // Lean: the posts are handed to the workers in batches (`staged`, the posting thread's own list, flushed at
    // kStageBatch jobs, before any wait and at the frame's hand-over); a condition is signalled only when a thread sleeps
    // on it (`idle` workers, `waiters` on finished: a Dekker pair with the done flags, all sequentially consistent); a
    // worker takes a few jobs per lock; one clock read per job; the environment set only when it differs.
    std::atomic<bool> lean{true};                  // (atomic: TE_AB=lean flips it at a frame's hand-over)
    int idle = 0;                                  // workers asleep on `wake` (under the lock)
    std::atomic<int> waiters{0};                   // threads asleep (or about to be) on `finished`
    std::vector<Job> staged;                       // posted, not yet handed over (the posting thread's)
    static constexpr size_t kStageBatch = 8, kTake = 4;
    int priority = 0;                              // the Vita: TE_GFX_BUILD_PRIO (0: the Worker rank's)
    // the pool's census per frame (graphics_scene.cpp BuildersCensusLine): jobs posted, run by a waiting thread,
    // skipped (their picture replaced); the finished jobs that signalled the `finished` condition (a lock and a kernel
    // signal each), the waits that slept on it and how often a sleeper woke to a build still not done
    std::atomic<uint64_t> c_posted{0}, c_helped{0}, c_cancelled{0}, c_signals{0}, c_sleeps{0}, c_vain{0};
    // THE TARGETED WAKE (lean): what each sleeper on `finished` waits for -- a build (its slot) or anything (a
    // drain, or no slot free: `sleeping_any`) -- so that a finished job signals the condition only when a sleeper waits for
    // ITS build or for anything. Before, every job's end signalled whenever anyone slept: the drawing thread, asleep on the
    // one build it was about to draw, woke (a kernel signal, a context switch, a builder preempted by the higher-priority
    // drawing thread inside its job's bookkeeping) for every other job, and slept again. A sleeper registers before it
    // checks its condition and a finished job publishes `done` before it reads the registrations (all sequentially
    // consistent): either the sleeper sees the build done or the job sees the sleeper (the Dekker pair `waiters` had).
    // TE_GFX_POOL_TARGETED=0: every job's end signals as before.
    static constexpr int kSleepers = 4;
    std::atomic<const void*> sleeping_on[kSleepers] = {};
    std::atomic<int> sleeping_any{0};
    bool targeted = true;
    // THE BACKGROUND QUEUE (work wanted AHEAD of its use: a texture's decode at its load, a native mesh's preparation):
    // a worker takes a background job only when no frame build is queued, one job at a time -- while frame builds were
    // posted in the last kRecentNs at most max(1, workers - 1) at once (a worker stays free for the picture's builds),
    // during a load (no frame builds) every worker. A thread waiting for builds (HelpUntil) never runs one and Drain does
    // not wait for them: a frame's builds never queue behind a decode. TE_GFX_BACKGROUND_WORKERS=N caps them.
    std::deque<Job> background;
    std::vector<Job> background_staged;            // posted, not handed over yet (the posting thread's: the game thread)
    static constexpr size_t kBackgroundBatch = 16; // handed over this many at once (and at every frame's hand-over)
    int background_running = 0;                    // (under the lock)
    int background_cap = 0;                        // TE_GFX_BACKGROUND_WORKERS (0: the rule above)
    static constexpr int64_t kRecentNs = 50000000; // 50 ms
    std::atomic<int64_t> last_frame_post_ns{0};
    std::atomic<uint64_t> c_background_posted{0}, c_background_run{0}, c_background_ns{0};
    void Start() {
        started = true;
        int n;
        if (const char* v = std::getenv("TE_GFX_BUILD_THREADS")) {
            n = std::max(0, std::min(8, std::atoi(v)));
        } else {
            int cpus = int(std::thread::hardware_concurrency());
            n = cpus >= 3 ? std::max(1, std::min(4, cpus - 3)) : 0;
#if defined(__vita__)
            // the three cores an application gets, shared by the game thread, the render thread and two builders (the
            // builders run below both -- Run -- and take what the other two leave; TE_GFX_BUILD_THREADS=1 for an A/B)
            n = 2;
#endif
        }
#if defined(__vita__)
        // TE_GFX_BUILD_PRIO=64..191: the builders' kernel priority (default the Worker rank, 144; 128 = the render thread's,
        // as before the device measurements) -- a device A/B knob
        if (const char* v = std::getenv("TE_GFX_BUILD_PRIO")) {
            const int p = std::atoi(v);
            if (p >= 64 && p <= 191) priority = p;
        }
#endif
        if (const char* v = std::getenv("TE_GFX_POOL_LEAN")) lean = v[0] != '0';
        if (const char* v = std::getenv("TE_GFX_POOL_TARGETED")) targeted = v[0] != '0';
        if (const char* v = std::getenv("TE_GFX_BACKGROUND_WORKERS")) background_cap = std::max(0, std::min(8, std::atoi(v)));
        staged.reserve(kStageBatch);
        for (int i = 0; i < n; ++i) threads.emplace_back([this, i] { Run(i); });
#if defined(__vita__)
        if (n) std::fprintf(stderr, "[te.gfx] geometry builds: %d worker thread%s (%s hand-over), priority %d%s\n", n, n == 1 ? "" : "s",
                            lean.load() ? "lean" : "per-job", std::getenv("TE_NO_QOS") ? 191 : (priority ? priority : 144),
                            std::getenv("TE_NO_QOS") ? " (TE_NO_QOS)" : "");
#else
        if (n) std::fprintf(stderr, "[te.gfx] geometry builds: %d worker thread%s (%s hand-over)\n", n, n == 1 ? "" : "s", lean.load() ? "lean" : "per-job");
#endif
    }
    // The job's work under its environment: `cur` is the thread's environment as it stands (updated when changed). A build
    // whose picture was replaced before it started is not made (CancelBuild): its storage is never read.
    static bool RunJob(Job& job, std::fenv_t& cur) {
        if (job.build && job.build->cancelled.load(std::memory_order_acquire)) return false;
        if (std::memcmp(&cur, &job.env, sizeof cur) != 0) {
            std::fesetenv(&job.env);
            cur = job.env;
        }
        job.fn();
        return true;
    }
    // A job's end: its placeholder done, the active count down; a sleeper on `finished` woken (lean: only if there is one)
    void Finished(Job& job) {
        if (job.build) job.build->done.store(true, std::memory_order_seq_cst);
        active.fetch_sub(1, std::memory_order_seq_cst);
        bool signal = !lean;
        if (lean && waiters.load(std::memory_order_seq_cst) > 0) {
            signal = !targeted || sleeping_any.load(std::memory_order_seq_cst) > 0;
            for (int k = 0; k < kSleepers && !signal; ++k)
                signal = job.build && sleeping_on[k].load(std::memory_order_seq_cst) == job.build;
        }
        if (signal) {
            c_signals.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<PoolMutex> lock(mutex);
            finished.notify_all();
        }
    }
    // a sleeper's registration (the targeted wake): its slot (-1: counted in sleeping_any); `target` null = anything
    int Register(const void* target) {
        if (target && targeted)
            for (int k = 0; k < kSleepers; ++k) {
                const void* expected = nullptr;
                if (sleeping_on[k].compare_exchange_strong(expected, target, std::memory_order_seq_cst)) return k;
            }
        sleeping_any.fetch_add(1, std::memory_order_seq_cst);
        return -1;
    }
    void Unregister(int slot) {
        if (slot >= 0) sleeping_on[slot].store(nullptr, std::memory_order_seq_cst);
        else sleeping_any.fetch_sub(1, std::memory_order_seq_cst);
    }
    void Run(int index) {
        char name[24];
        std::snprintf(name, sizeof name, "te-build-%d", index);
#if defined(__vita__)
        // The Vita's three cores: the builders run BELOW the render thread (the Worker rank, 144: game 96 > render 128 >
        // builds 144 > the 160 default) -- they take what the game and render threads leave and never time-slice the
        // drawing thread, which waits for their builds only while it has none of its own work (ExpandBucket). The
        // priority changes when work runs, never what it computes.
        constexpr auto kRole = ps2r::platform::host::ThreadRole::Worker;
#else
        constexpr auto kRole = ps2r::platform::host::ThreadRole::Graphics;
#endif
        if (!std::getenv("TE_NO_QOS")) {
            ps2r::platform::host::PinCurrentThreadInteractive(name, kRole);
#if defined(__vita__)
            if (priority) sceKernelChangeThreadPriority(0, priority);
#endif
        }
        te::prof::BindThread(te::prof::kBuild0 + index);
        std::fenv_t cur;
        std::fegetenv(&cur);
        std::vector<Job> mine;
        mine.reserve(kTake);
        std::unique_lock<PoolMutex> lock(mutex);
        for (;;) {
            {
                TE_PROF_SCOPE("pool idle");
                while (!stop && jobs.empty() && !BackgroundTakeable()) {
                    ++idle;
                    wake.wait(lock);
                    --idle;
                }
            }
            if (stop) return;
            if (jobs.empty()) {
                // a background job (no frame build waits): one at a time, the frame's builds looked at again after it
                Job job = std::move(background.front());
                background.pop_front();
                ++background_running;
                lock.unlock();
                {
                    TE_PROF_SCOPE("background job (ahead of its use)");
                    const auto t0 = std::chrono::steady_clock::now();
                    RunJob(job, cur);
                    c_background_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                                              std::memory_order_relaxed);
                    c_background_run.fetch_add(1, std::memory_order_relaxed);
                }
                lock.lock();
                --background_running;
                continue;
            }
            // lean: up to kTake jobs, half of the queue at most (the other worker takes its share)
            const size_t take = lean ? std::max<size_t>(1, std::min(kTake, jobs.size() / 2)) : 1;
            for (size_t k = 0; k < take && !jobs.empty(); ++k) {
                mine.push_back(std::move(jobs.front()));
                jobs.pop_front();
            }
            active.fetch_add(mine.size(), std::memory_order_seq_cst);
            lock.unlock();
            {
                TE_PROF_SCOPE("build job");
                auto t0 = std::chrono::steady_clock::now();
                for (Job& job : mine) {
                    if (!lean) t0 = std::chrono::steady_clock::now();
                    if (!RunJob(job, cur)) c_cancelled.fetch_add(1, std::memory_order_relaxed);
                    const auto t1 = std::chrono::steady_clock::now();
                    built_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(), std::memory_order_relaxed);
                    t0 = t1;
                    Finished(job);
                }
            }
            mine.clear();
            lock.lock();
        }
    }
    // The staged posts handed to the workers (the posting thread; any thread before it waits)
    void Flush() {
        FlushBackground();   // (the game thread's staged background jobs too)
        if (staged.empty()) return;
        NoteFramePosts();
        // The workers are woken with the lock HELD: on the Vita the condition is the kernel's lightweight one (PoolCondition),
        // and a kernel that wants the associated mutex's owner to signal it (unverified on hardware; Vita3K does not check)
        // would refuse an unlocked signal -- the workers would then sleep on while the render thread, which only waits
        // (Wait: help false), waits on their builds and the game thread waits on the render thread's slots. Signalling under
        // the lock is valid for every condition variable; the woken worker takes the lock once this scope ends.
        std::lock_guard<PoolMutex> lock(mutex);
        for (Job& job : staged) jobs.push_back(std::move(job));
        const int sleeping = idle;
        const size_t n = staged.size();
        staged.clear();
        if (sleeping > 1 && n > 1) wake.notify_all();
        else if (sleeping > 0) wake.notify_one();
    }
    // A thread that needs the builds done runs the queued ones itself rather than sleep while the workers catch up;
    // it sleeps only once nothing is left to start (or when it may not run them: `help` false). `ready` is checked with
    // the lock held (and, lean, after the waiter is counted: the Dekker pair with Finished).
    template <typename Ready>
    void HelpUntil(Ready ready, bool help, const void* target) {
        const auto t0 = std::chrono::steady_clock::now();
        if (help) Flush();
        std::fenv_t own;
        std::fegetenv(&own);
        std::unique_lock<PoolMutex> lock(mutex);
        bool slept = false;
        for (;;) {
            waiters.fetch_add(1, std::memory_order_seq_cst);
            const int slot = Register(target);
            if (ready()) {
                Unregister(slot);
                waiters.fetch_sub(1, std::memory_order_seq_cst);
                break;
            }
            if (jobs.empty() || !help) {
                c_sleeps.fetch_add(1, std::memory_order_relaxed);
                if (slept) c_vain.fetch_add(1, std::memory_order_relaxed);
                slept = true;
                finished.wait(lock);
                Unregister(slot);
                waiters.fetch_sub(1, std::memory_order_seq_cst);
                continue;
            }
            Unregister(slot);
            waiters.fetch_sub(1, std::memory_order_seq_cst);
            Job job = std::move(jobs.front());
            jobs.pop_front();
            active.fetch_add(1, std::memory_order_seq_cst);
            lock.unlock();
            {
                TE_PROF_SCOPE("build job");
                const auto j0 = std::chrono::steady_clock::now();
                std::fenv_t cur = own;
                c_helped.fetch_add(1, std::memory_order_relaxed);
                if (!RunJob(job, cur)) c_cancelled.fetch_add(1, std::memory_order_relaxed);
                if (std::memcmp(&cur, &own, sizeof cur) != 0) std::fesetenv(&own);
                built_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - j0).count(),
                                   std::memory_order_relaxed);
            }
            Finished(job);
            lock.lock();
        }
        waited_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                            std::memory_order_relaxed);
    }
    void Post(Job job) {
        c_posted.fetch_add(1, std::memory_order_relaxed);
        if (lean) {
            staged.push_back(std::move(job));
            if (staged.size() >= kStageBatch) Flush();
            return;
        }
        NoteFramePosts();
        { std::lock_guard<PoolMutex> lock(mutex); jobs.push_back(std::move(job)); wake.notify_one(); }   // signalled locked: Flush
    }
    // the frame's builds handed to the workers now (the background rule's clock: one read per batch, not per job)
    void NoteFramePosts() {
        last_frame_post_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                                 std::memory_order_relaxed);
    }
    // Under the lock: a background job may be started now (see `background`)
    bool BackgroundTakeable() const {
        if (background.empty()) return false;
        const int workers = int(threads.size());
        int cap = background_cap ? background_cap : workers;
        if (!background_cap) {
            const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now - last_frame_post_ns.load(std::memory_order_relaxed) < kRecentNs) cap = std::max(1, workers - 1);
        }
        return background_running < cap;
    }
    // A background job (the game thread): run by a worker when no frame build waits -- or here and now when there is no
    // worker. Staged and handed over in batches (a load posts one per texture and per model part, ~2000 in the Totem
    // level's: one lock and one signal a batch, not a kernel call each), and at every frame's hand-over (Flush).
    void PostBackground(BuildJob&& fn) {
        Job job;
        job.fn = std::move(fn);
        std::fegetenv(&job.env);
        c_background_posted.fetch_add(1, std::memory_order_relaxed);
        if (threads.empty()) {
            job.fn();
            return;
        }
        background_staged.push_back(std::move(job));
        if (background_staged.size() >= kBackgroundBatch) FlushBackground();
    }
    void FlushBackground() {
        if (background_staged.empty()) return;
        std::lock_guard<PoolMutex> lock(mutex);
        for (Job& job : background_staged) background.push_back(std::move(job));
        background_staged.clear();
        if (idle > 1) wake.notify_all();   // (signalled locked: Flush)
        else if (idle > 0) wake.notify_one();
    }
    void Wait(const DeferredBuild& build) {
        if (build.done.load(std::memory_order_acquire)) return;
        // the render thread only waits: a build run deep in its frame's call chain outgrew a console thread's stack
        HelpUntil([&] { return build.done.load(std::memory_order_seq_cst); }, !OnRenderThread(), &build);
    }
    void Drain() {
        if (threads.empty()) return;
        HelpUntil([&] { return jobs.empty() && active.load(std::memory_order_seq_cst) == 0; }, !OnRenderThread(), nullptr);
    }
    ~BuildPool() {
        Flush();
        { std::lock_guard<PoolMutex> lock(mutex); stop = true; wake.notify_all(); }   // signalled locked: Flush
        for (auto& t : threads) t.join();
    }
};
BuildPool& Builds() {
    static BuildPool pool;
    if (!pool.started) pool.Start();
    return pool;
}
// The Vita's pthread locks are made at their first use (a small allocation behind each). Made after vitaGL has set up
// its memory, one can land beside memory the GPU reads, where Vita3K's tracking of the GPU's pages fails the lock's
// exclusive store (the build pool's mutex did, mid-game). The render and build threads' locks are therefore used once
// here, before the device exists; elsewhere this costs a lock and a few notifies.
void PrimeThreadSync() {
    const auto prime = [](auto& m, auto& a, auto& b) {
        std::unique_lock<std::remove_reference_t<decltype(m)>> lock(m);
        a.wait_for(lock, std::chrono::milliseconds(0));
        b.wait_for(lock, std::chrono::milliseconds(0));
        lock.unlock();
        a.notify_all();
        b.notify_all();
    };
    BuildPool& pool = Builds();
    prime(pool.mutex, pool.wake, pool.finished);
    prime(g.rt.mutex, g.rt.wake, g.rt.done);
    (void)Eager();   // (the eager textures' lock: made here, before the device's memory)
}

bool DeferringBuilds() { return !Builds().threads.empty(); }

DeferredBuild& ReserveDeferred(uint32_t bucket, const ps2e_draw_state& state) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
    d.state = state;
    d.verts.clear();
    d.ext = nullptr;
    d.ext_count = 0;
    d.copy_frame = false;
    d.mesh = 0;
    d.pool = 0;
    d.tag = g.tag;
    d.has_transform = g.tag_has_transform;
    if (g.tag_has_transform) d.transform = g.tag_transform;
    if (g.deferred_used == g.deferred.size()) g.deferred.push_back(std::make_unique<DeferredBuild>());
    DeferredBuild& build = *g.deferred[g.deferred_used++];
    build.state = state;
    build.verts.clear();
    build.segments.clear();
    build.alternate.clear();
    build.meshes.clear();
    build.done.store(false, std::memory_order_relaxed);
    build.cancelled.store(false, std::memory_order_relaxed);
    d.pending = &build;
    return build;
}

#if defined(__vita__)
// A replaced picture's build (the Vita's low-latency hand-over): not made if no worker has started it yet (RunJob)
void CancelBuild(DeferredBuild& build) { build.cancelled.store(true, std::memory_order_release); }
#endif

void PostBuildJob(DeferredBuild& build, BuildJob&& job) {
    TE_PROF_SCOPE("PostBuild");
    BuildPool::Job j;
    j.fn = std::move(job);
    j.build = &build;
    std::fegetenv(&j.env);
    Builds().Post(std::move(j));
}
// The posts staged for the workers handed over (the frame's hand-over: the drawing thread waits for these builds)
void FlushBuilds() { Builds().Flush(); }

// Work wanted ahead of its use (BuildPool::background): a worker runs it when no frame build waits
void PostBackgroundJob(BuildJob&& job) { Builds().PostBackground(std::move(job)); }

// ---- EAGER TEXTURES (the Vita's default; TE_GFX_EAGER_TEXTURES=0 the synchronous path, =1 elsewhere) ----------------------
// A game texture was decoded (its GS upload chain -> RGBA, ~5 ms of the device's CPU a texture) on the game
// thread at the game's ReadTexture and then made on the drawing thread with the game thread waiting for it (OnRender: up to
// a picture's drawing). The game's chunk reader is time-budgeted, but the port's clock does not move inside a tick (only
// the drive model charges it), so one tick reads every resource whose bytes have landed: a chunk's textures arrived as a
// burst of decodes in ONE tick -- the 100-250 ms ticks at loads and area entries. Now ReadTexture's call reserves the
// device id at once (ps2e_draw_texture_reserve: the id the synchronous creation would have taken -- ids are taken in the
// order of the calls), queues the decode on the builder pool's background queue and returns; the decoded texture is made
// on the drawing thread while it has no picture to draw (MakeReadyTextures), and a draw that names a texture not made yet
// makes THAT one there and then (ResolveTexture: the device's resolver -- decoded on the spot when no worker has started
// it, waited for when one has). The texels, the levels and the device calls that make the texture are the synchronous
// path's, under the game thread's floating-point environment: only when and on which thread they happen changes.
// A decode that fails (the synchronous path's handle 0: the draws untextured) is swept on the game thread (the next load or
// present): its address's handle dropped, as if it had never had one -- draws recorded in between name an id that has no
// texture (refused). No disc texture fails to decode (device logs: failed 0).
struct PendingTexture {
    uint32_t id = 0, address = 0;
    te_gfx_texture_header header{};
    std::vector<uint8_t> packet;         // the upload chain (a copy: the game may reuse its disk-manager block)
    te::gfx::DecodedTexture decoded;
    std::string error;
    std::fenv_t env{};                   // the game thread's floating-point environment (the decode runs under it anywhere)
    int64_t loaded_ns = 0;
    enum : int { kQueued = 0, kDecoding = 1, kDecoded = 2, kFailed = 3 };
    std::atomic<int> state{kQueued};
};
struct EagerTextures {
    bool read = false, on = false;
    bool at_draw_only = false;   // TE_GFX_EAGER_TEXTURES=2 (a diagnosis): no decode ahead -- every texture decoded and made at its first draw
    te::sync::Mutex mutex{"te-textures"};
    std::unordered_map<uint32_t, std::shared_ptr<PendingTexture>> pending;   // reserved id -> its record, until made / dropped
    std::deque<uint32_t> ready;                                              // decoded (or failed), oldest first, to make
    std::vector<std::shared_ptr<PendingTexture>> failed;                     // for the game thread's sweep
    // the census (any thread)
    std::atomic<uint64_t> reserved{0}, decoded_ahead{0}, decoded_at_draw{0}, waited_at_draw{0}, made_between{0}, made_at_draw{0},
        dropped{0}, failures{0}, decode_us{0}, wait_us{0}, make_us{0}, lead_us{0}, lead_n{0};
};
EagerTextures& Eager() {
    // (static storage made before the device exists: PrimeThreadSync -- the lock's work area is in the object, te_sync.h)
    static EagerTextures e;
    return e;
}
bool EagerTexturesOn() {
    EagerTextures& e = Eager();
    if (!e.read) {
        e.read = true;
#if defined(__vita__)
        e.on = true;
#else
        e.on = false;
#endif
        if (const char* v = std::getenv("TE_GFX_EAGER_TEXTURES")) {
            e.on = v[0] != '0';
            e.at_draw_only = v[0] == '2';
        }
        if (std::getenv("TE_GFX_DUMP_TEXTURES")) e.on = false;   // (the dump writes the decoded image at the load)
        std::fprintf(stderr, "[te.gfx] textures: %s (TE_GFX_EAGER_TEXTURES)\n",
                     !e.on ? "decoded and made at their load on the game thread"
                     : e.at_draw_only ? "decoded and made at their first draw only (=2: a diagnosis of the draws' path)"
                                      : "decoded ahead on the builders at their load and made between pictures or at their first draw");
    }
    return e.on;
}
// The decode (whichever thread took the record: kDecoding), under the game thread's floating-point environment
void DecodePending(PendingTexture& t) {
    std::fenv_t own;
    std::fegetenv(&own);
    const bool swap_env = std::memcmp(&own, &t.env, sizeof own) != 0;
    if (swap_env) std::fesetenv(&t.env);
    const int64_t t0 = te::load::NowNs();
    std::string error;
    const bool ok = te::gfx::DecodeTextureResource(t.header, t.packet.data(), t.packet.size(), t.decoded, &error);
    if (swap_env) std::fesetenv(&own);
    const int64_t t1 = te::load::NowNs();
    Eager().decode_us.fetch_add(uint64_t((t1 - t0) / 1000), std::memory_order_relaxed);
    te::load::NoteDecodeElsewhere(double(t1 - t0) / 1e6, ok ? DecodedBytes(t.decoded) : 0);
    if (!ok) t.error = error;
    t.packet.clear();
    t.packet.shrink_to_fit();
    t.state.store(ok ? PendingTexture::kDecoded : PendingTexture::kFailed, std::memory_order_release);
}
// A decoded texture made at its reserved id: CreateTexture's calls (the GL thread)
void MakePending(PendingTexture& t) {
    const int64_t t0 = te::load::NowNs();
    if (ps2e_draw_texture_create_reserved_gpu_alpha(g.dev, t.id, t.decoded.width, t.decoded.height, t.decoded.rgba.data())) {
        if (!t.decoded.mips.empty()) {
            std::vector<const void*> levels;
            for (const auto& level : t.decoded.mips) levels.push_back(level.data());
            ps2e_draw_texture_set_mips(g.dev, t.id, int(levels.size()), levels.data());
        }
    }
    const int64_t t1 = te::load::NowNs();
    te::load::NoteRenderTexture(double(t1 - t0) / 1e6);
    Eager().make_us.fetch_add(uint64_t((t1 - t0) / 1000), std::memory_order_relaxed);
    Eager().lead_us.fetch_add(uint64_t(std::max<int64_t>(0, t1 - t.loaded_ns) / 1000), std::memory_order_relaxed);
    Eager().lead_n.fetch_add(1, std::memory_order_relaxed);
    t.decoded = te::gfx::DecodedTexture{};   // (the device has the texels)
}
void WakeForTextures();
// The background job: the decode ahead of the texture's first draw (a worker)
void QueueDecode(const std::shared_ptr<PendingTexture>& t) {
    if (Eager().at_draw_only) return;   // (=2: the record stays queued -- its first draw decodes and makes it)
    te::gfx::PostBackground([t] {
        int expected = PendingTexture::kQueued;
        if (!t->state.compare_exchange_strong(expected, PendingTexture::kDecoding, std::memory_order_acq_rel)) return;   // (a draw took it)
        {
            TE_PROF_SCOPE("texture: decode ahead (a worker)");
            DecodePending(*t);
        }
        Eager().decoded_ahead.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<te::sync::Mutex> lock(Eager().mutex);
            Eager().ready.push_back(t->id);
        }
        WakeForTextures();
    });
}
// The decoded textures made, oldest first (the drawing thread between pictures; the game thread at a synchronous present),
// until `stop()` (when given) says a picture or a job waits. True when some are left.
bool MakeReadyTextures(bool (*stop)()) {
    EagerTextures& e = Eager();
    for (;;) {
        std::shared_ptr<PendingTexture> t;
        {
            std::lock_guard<te::sync::Mutex> lock(e.mutex);
            if (e.ready.empty()) return false;
            const uint32_t id = e.ready.front();
            e.ready.pop_front();
            auto it = e.pending.find(id);
            if (it == e.pending.end()) continue;   // made at a draw already, or released before it was made
            const int state = it->second->state.load(std::memory_order_acquire);
            if (state != PendingTexture::kDecoded && state != PendingTexture::kFailed) continue;   // (a draw has it)
            t = it->second;
            e.pending.erase(it);
            if (state == PendingTexture::kFailed) {
                e.failed.push_back(t);
                continue;
            }
        }
        MakePending(*t);
        e.made_between.fetch_add(1, std::memory_order_relaxed);
        if (stop && stop()) {
            std::lock_guard<te::sync::Mutex> lock(e.mutex);
            return !e.ready.empty();
        }
    }
}
// The device's resolver (ps2e_draw_set_texture_resolver): a draw names reserved id `id` not made yet -- made here (the GL
// thread), decoded here first if no worker has started it, waited for if one has. 0: no such texture to make.
int ResolveTexture(void*, uint32_t id) {
    EagerTextures& e = Eager();
    std::shared_ptr<PendingTexture> t;
    {
        std::lock_guard<te::sync::Mutex> lock(e.mutex);
        auto it = e.pending.find(id);
        if (it == e.pending.end()) return 0;
        t = it->second;
    }
    TE_PROF_SCOPE("texture: made at its first draw");
    int expected = PendingTexture::kQueued;
    if (t->state.compare_exchange_strong(expected, PendingTexture::kDecoding, std::memory_order_acq_rel)) {
        DecodePending(*t);
        e.decoded_at_draw.fetch_add(1, std::memory_order_relaxed);
    } else if (expected == PendingTexture::kDecoding) {
        const int64_t w0 = te::load::NowNs();
        while (t->state.load(std::memory_order_acquire) == PendingTexture::kDecoding) std::this_thread::sleep_for(std::chrono::microseconds(50));
        e.waited_at_draw.fetch_add(1, std::memory_order_relaxed);
        e.wait_us.fetch_add(uint64_t((te::load::NowNs() - w0) / 1000), std::memory_order_relaxed);
    }
    {
        std::lock_guard<te::sync::Mutex> lock(e.mutex);
        auto it = e.pending.find(id);
        if (it != e.pending.end() && it->second == t) e.pending.erase(it);
        if (t->state.load(std::memory_order_acquire) == PendingTexture::kFailed) {
            e.failed.push_back(t);
            return 0;
        }
    }
    MakePending(*t);
    e.made_at_draw.fetch_add(1, std::memory_order_relaxed);
    return 1;
}
// A reserved id's record dropped (its texture released before it was made: the GL thread, before the destroy)
void DropPending(uint32_t id) {
    EagerTextures& e = Eager();
    std::lock_guard<te::sync::Mutex> lock(e.mutex);
    if (e.pending.erase(id)) e.dropped.fetch_add(1, std::memory_order_relaxed);
}
void DropAllPending() {
    EagerTextures& e = Eager();
    std::lock_guard<te::sync::Mutex> lock(e.mutex);
    e.pending.clear();
    e.ready.clear();
    e.failed.clear();
}
// The game thread: the decodes that failed since, as the synchronous path's handle 0 (logged, the address without a texture)
void SweepTextureFailures() {
    EagerTextures& e = Eager();
    std::vector<std::shared_ptr<PendingTexture>> failed;
    {
        std::lock_guard<te::sync::Mutex> lock(e.mutex);
        if (e.failed.empty()) return;
        failed.swap(e.failed);
    }
    for (const auto& t : failed) {
        g.texture_failures++;
        e.failures.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[te.gfx] texture %#x: decode failed: %s (psm %u upload %u %ux%u levels %u)\n", t->address, t->error.c_str(),
                     t->header.pixelFormat, t->header.uploadFormat, 1u << t->header.widthPower, 1u << t->header.heightPower, t->header.levels);
        auto it = g.textures.find(t->address);
        if (it != g.textures.end() && it->second == t->id) {
            g.textures.erase(it);
            TextureChanged(t->address);
            g.texture_sources.erase(t->address);
            if (g.textures_loaded) g.textures_loaded--;
        }
    }
}
// The census line (the pace window)
std::string EagerCensusLine() {
    EagerTextures& e = Eager();
    if (!e.read || !e.on) return std::string();
    auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
    const uint64_t reserved = take(e.reserved), ahead = take(e.decoded_ahead), at_draw = take(e.decoded_at_draw), waited = take(e.waited_at_draw),
                   between = take(e.made_between), made_draw = take(e.made_at_draw), dropped = take(e.dropped), failures = take(e.failures),
                   decode_us = take(e.decode_us), wait_us = take(e.wait_us), make_us = take(e.make_us), lead_us = take(e.lead_us), lead_n = take(e.lead_n);
    size_t pending = 0;
    {
        std::lock_guard<te::sync::Mutex> lock(e.mutex);
        pending = e.pending.size();
    }
    if (!reserved && !ahead && !at_draw && !between && !made_draw && !pending) return std::string();
    char b[700];
    std::snprintf(b, sizeof b, "[te.load] textures ahead of their use (TE_GFX_EAGER_TEXTURES): %llu loaded (ids reserved, the game thread "
                  "not waiting); decoded on the workers %llu, at a draw %llu (and %llu waited for a worker's decode, %.1f ms), %.1f ms of "
                  "decoding; made between pictures %llu, at their first draw %llu (%.1f ms making); a load to its texture made %.0f ms on "
                  "average; released before made %llu; failed %llu; %u pending now\n",
                  (unsigned long long)reserved, (unsigned long long)ahead, (unsigned long long)at_draw, (unsigned long long)waited,
                  double(wait_us) / 1000.0, double(decode_us) / 1000.0, (unsigned long long)between, (unsigned long long)made_draw,
                  double(make_us) / 1000.0, lead_n ? double(lead_us) / 1000.0 / double(lead_n) : 0.0, (unsigned long long)dropped,
                  (unsigned long long)failures, unsigned(pending));
    return b;
}
// A worker's decode is ready: the drawing thread told (it makes the texture when it has no picture to draw); synchronous
// (no drawing thread) the game thread makes it at its next present
void WakeForTextures() {
    auto& rt = g.rt;
    if (!rt.started) return;
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        rt.textures_ready = true;
    }
    rt.wake.notify_one();
}

void DrainBuilds() { Builds().Drain(); }

// the build pool's census line (graphics_scene.cpp BuildersCensusLine appends it)
std::string PoolCensusLine(double frames) {
    if (frames <= 0) return std::string();
    BuildPool& p = Builds();
    const auto take = [frames](std::atomic<uint64_t>& a) { return double(a.exchange(0, std::memory_order_relaxed)) / frames; };
    const double posted = take(p.c_posted), helped = take(p.c_helped), cancelled = take(p.c_cancelled), signals = take(p.c_signals),
                 sleeps = take(p.c_sleeps), vain = take(p.c_vain);
    const double bg_posted = take(p.c_background_posted), bg_run = take(p.c_background_run);
    const double bg_ms = double(p.c_background_ns.exchange(0, std::memory_order_relaxed)) / 1e6 / frames;
    char b[600];
    std::snprintf(b, sizeof b, "[te.gfx.vita] build pool per present: jobs posted %.1f (run by a waiting thread %.1f, skipped: their "
                  "picture replaced %.1f); jobs that signalled the finished condition %.1f; waits slept %.1f (woken to a build "
                  "still not done %.1f); background jobs (ahead of their use) posted %.2f, run %.2f (%.2f ms)\n", posted, helped,
                  cancelled, signals, sleeps, vain, bg_posted, bg_run, bg_ms);
    return b;
}

// OpenDraw's joining rule for a draw of this state and tag appended after `out`'s last draw
bool Joins(const Gfx::Bucket& out, const ps2e_draw_state& state, bool copy_frame, const ps2r::ps2e::InterpTag& tag, bool interp) {
    if (!out.used || (state.blend != 0 && !g.join_blended.load(std::memory_order_relaxed)) || copy_frame) return false;
    const Draw& last = out.draws[out.used - 1];
    if (last.VertexCount() < 3 || !SameState(last.state, state)) return false;
    return !interp || (last.tag.id == tag.id && last.tag.kind == tag.kind &&
                       (tag.kind != ps2r::ps2e::InterpKind::Vertices || last.tag.part == tag.part));
}
Draw& NextDraw(Gfx::Bucket& out) {
    if (out.used == out.draws.size()) out.draws.emplace_back();
    return out.draws[out.used++];
}

// The frame's placeholders replaced by what their builds recorded, every bucket replayed through the joining rule (the draw
// list the game thread would have recorded building them itself); waits for the frame's builds
void ExpandDeferred(Gfx::Bucket* buckets, std::vector<std::unique_ptr<DeferredBuild>>& deferred, size_t& used) {
    if (!used) return;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) ExpandBucket(buckets, b);
    for (size_t i = 0; i < used; ++i) Builds().Wait(*deferred[i]);
    used = 0;
}

// One bucket's placeholders replaced by their builds' draws, each build waited for as it is reached (the drawing thread
// draws a bucket whose builds are done while the workers finish the next one's)
#if defined(__vita__)
// Joins' rule against the draw the order ends in (null: none yet)
bool JoinsDraw(const Draw* last, const ps2e_draw_state& state, bool copy_frame, const ps2r::ps2e::InterpTag& tag, bool interp) {
    if (!last || (state.blend != 0 && !g.join_blended.load(std::memory_order_relaxed)) || copy_frame) return false;
    if (last->VertexCount() < 3 || !SameState(last->state, state)) return false;
    return !interp || (last->tag.id == tag.id && last->tag.kind == tag.kind &&
                       (tag.kind != ps2r::ps2e::InterpKind::Vertices || last->tag.part == tag.part));
}
constexpr uint32_t kExpandedBit = 0x80000000u;   // an order entry naming one of the bucket's `expanded` records
// The Vita: the bucket's draw ORDER (Gfx::Bucket::order), the same draw list the replay through the joining rule gives
// (the desktop's form below moves the records themselves) -- the draws before the first placeholder as they are, then the
// tail: each recorded draw in its place (or joined to the draw before it), each placeholder's segments as draws of the
// bucket's `expanded` records (drawn in place from the build's storage: no interpolation reads the Vita's draws). No record
// is moved; the order names them. Built once per picture (`ordered`): a second submit of the picture reuses it.
void ExpandBucket(Gfx::Bucket* buckets, int b) {
    Gfx::Bucket& bk = buckets[b];
    if (bk.ordered) return;
    bk.ordered = true;
    const bool interp = g.interp_active.load(std::memory_order_relaxed);
    std::vector<uint32_t>& idx = bk.order_index;
    idx.clear();
    bk.expanded_used = 0;
    auto at = [&bk](uint32_t r) -> Draw& { return (r & kExpandedBit) ? bk.expanded[r & ~kExpandedBit] : bk.draws[r]; };
    auto fresh = [&bk]() -> uint32_t {
        if (bk.expanded_used == bk.expanded.size()) bk.expanded.emplace_back();
        return kExpandedBit | uint32_t(bk.expanded_used++);
    };
    size_t head = bk.used;
    // (the compact mesh records made whole first -- their pooled state and constant fields, ExpandCompact -- in the same
    // sweep that finds the first placeholder: every reader from here on sees whole records)
    // the lines that sweep reads of a record -- its header, its skin / tag line, its mesh parameters' box /
    // first / count, its state's four -- asked for two records ahead (PLD: no effect on any value): a record's header is
    // written on the game thread's core, the rest were last touched pictures ago
    const auto prefetch = [](const Draw* r) {
        const char* const s = reinterpret_cast<const char*>(&r->state);
        __builtin_prefetch(r);
        __builtin_prefetch(&r->skin_joints);
        __builtin_prefetch(r->mesh_params.box);
        __builtin_prefetch(s);
        __builtin_prefetch(s + 32);
        __builtin_prefetch(s + 64);
        __builtin_prefetch(s + 96);
    };
    if (bk.used > 0) prefetch(&bk.draws[0]);
    if (bk.used > 1) prefetch(&bk.draws[1]);
    for (size_t i = 0; i < bk.used; ++i) {
        if (i + 2 < bk.used) prefetch(&bk.draws[i + 2]);
        Draw& d = bk.draws[i];
        if (d.pool) ExpandCompact(d);
        if (d.pending && head == bk.used) head = i;
    }
    idx.resize(head);
    for (size_t i = 0; i < head; ++i) idx[i] = uint32_t(i);
    for (size_t i = head; i < bk.used; ++i) {
        Draw& d = bk.draws[i];
        if (!d.pending) {
            if (!d.mesh && !idx.empty() && JoinsDraw(&at(idx.back()), d.state, d.copy_frame, d.tag, interp)) {
                Draw& last = at(idx.back());
                last.OwnVertices();
                last.verts.insert(last.verts.end(), d.verts.begin(), d.verts.end());
                if (!(idx.back() & kExpandedBit)) MarkHolds(bk, idx.back());   // (a recorded draw took vertices: trimmed later)
            } else {
                idx.push_back(uint32_t(i));
            }
            continue;
        }
        const DeferredBuild& build = *d.pending;
        {
            TE_PROF_SCOPE("wait for a build");
            Builds().Wait(build);
        }
        size_t vat = 0, mesh_at = 0;
        for (size_t k = 0; k < build.segments.size(); ++k) {
            const uint32_t count = build.segments[k];
            if (count == 0) {
                // a native mesh range in this place (a chunked instance's): a mesh draw of its own
                if (mesh_at >= build.meshes.size()) continue;
                const DeferredBuild::MeshSegment& ms = build.meshes[mesh_at++];
                const uint32_t r = fresh();
                Draw& o = at(r);
                o.state = build.state;
                o.verts.clear();
                o.ext = nullptr;
                o.ext_count = 0;
                o.copy_frame = false;
                o.pending = nullptr;
                o.pool = 0;
                o.mesh = ms.mesh;
                o.mesh_params = ms.params;
                o.skin_joints = 0;
                o.tag = d.tag;
                o.has_transform = false;
                idx.push_back(r);
                continue;
            }
            const ps2e_draw_state& state = k < build.alternate.size() && build.alternate[k] ? build.alt_state : build.state;
            const ps2e_draw_vertex* const first = build.verts.data() + vat;
            vat += count;
            if (!idx.empty() && JoinsDraw(&at(idx.back()), state, false, d.tag, interp)) {
                Draw& last = at(idx.back());
                if (last.ext && count && last.ext + last.ext_count == first) {
                    last.ext_count += count;   // the next segment of the same storage: the view grows
                } else {
                    last.OwnVertices();
                    last.verts.insert(last.verts.end(), first, first + count);
                    if (!(idx.back() & kExpandedBit)) MarkHolds(bk, idx.back());
                }
            } else {
                // a recycled record: every field the submit reads is set here (`mesh` above all: see the desktop's form)
                const uint32_t r = fresh();
                Draw& o = at(r);
                o.state = state;
                o.verts.clear();
                o.ext = count ? first : nullptr;
                o.ext_count = count;
                o.copy_frame = false;
                o.pending = nullptr;
                o.mesh = 0;
                o.pool = 0;
                o.tag = d.tag;
                o.has_transform = d.has_transform;
                if (d.has_transform) o.transform = d.transform;
                idx.push_back(r);
            }
        }
        d.pending = nullptr;
    }
    g.expanded_segments += bk.expanded_used;
    g.expanded_tails += bk.used - head;
    // (the expanded records are all made: their addresses stand from here on)
    bk.order.resize(idx.size());
    for (size_t k = 0; k < idx.size(); ++k) bk.order[k] = &at(idx[k]);
}
#else
void ExpandBucket(Gfx::Bucket* buckets, int b) {
    const bool interp = g.interp_active.load(std::memory_order_relaxed);
    thread_local Gfx::Bucket scratch[TE_GFX_BUCKET_COUNT];   // each bucket's own: storage does not wander between buckets
    {
        // The draws before the first placeholder are what the replay would give back as they are (they were recorded
        // through the same joining rule): they stay in place, and only the bucket's tail from it is replayed (a bucket's
        // placeholders come late -- the clipped models after the native ones -- so most of a big bucket is not moved)
        Gfx::Bucket& out = buckets[b];
        size_t head = out.used;
        for (size_t i = 0; i < out.used; ++i)
            if (out.draws[i].pending) { head = i; break; }
        if (head == out.used) return;
        Gfx::Bucket& in = scratch[b];
        in.used = 0;
        for (size_t i = head; i < out.used; ++i) std::swap(NextDraw(in), out.draws[i]);
        out.used = head;
        for (size_t i = 0; i < in.used; ++i) {
            Draw& d = in.draws[i];
            if (!d.pending) {
                if (!d.mesh && Joins(out, d.state, d.copy_frame, d.tag, interp)) {
                    Draw& last = out.draws[out.used - 1];
                    last.OwnVertices();
                    last.verts.insert(last.verts.end(), d.verts.begin(), d.verts.end());
                } else {
                    std::swap(NextDraw(out), d);
                }
                continue;
            }
            const DeferredBuild& build = *d.pending;
            {
                TE_PROF_SCOPE("wait for a build");
                Builds().Wait(build);
            }
            size_t at = 0, mesh_at = 0;
            for (size_t k = 0; k < build.segments.size(); ++k) {
                const uint32_t count = build.segments[k];
                if (count == 0) {
                    // a native mesh range in this place (a chunked instance's: the Vita): a mesh draw of its own
                    if (mesh_at >= build.meshes.size()) continue;
                    const DeferredBuild::MeshSegment& ms = build.meshes[mesh_at++];
                    Draw& o = NextDraw(out);
                    o.state = build.state;
                    o.verts.clear();
                    o.ext = nullptr;
                    o.ext_count = 0;
                    o.copy_frame = false;
                    o.pending = nullptr;
                    o.mesh = ms.mesh;
                    o.mesh_params = ms.params;
                    o.skin_joints = 0;
                    o.tag = d.tag;
                    o.has_transform = false;
                    continue;
                }
                const ps2e_draw_state& state = k < build.alternate.size() && build.alternate[k] ? build.alt_state : build.state;
                const auto first = build.verts.begin() + std::ptrdiff_t(at), end = first + std::ptrdiff_t(count);
                at += count;
                if (Joins(out, state, false, d.tag, interp)) {
                    Draw& last = out.draws[out.used - 1];
                    if (last.ext && count && last.ext + last.ext_count == &*first) {
                        last.ext_count += count;   // the next segment of the same storage: the view grows
                    } else {
                        last.OwnVertices();
                        last.verts.insert(last.verts.end(), first, end);
                    }
                } else {
                    // a recycled draw: every field the submit reads is set here -- `mesh` above all: a stale native-mesh
                    // handle left from an earlier frame would draw that old mesh (its old screen matrix, under this
                    // segment's state and texture) in place of these vertices (the Vita, the one host with native meshes)
                    Draw& o = NextDraw(out);
                    o.state = state;
#if defined(__vita__)
                    // drawn in place from the build's storage (no interpolation reads the Vita's draws)
                    o.verts.clear();
                    o.ext = count ? &*first : nullptr;
                    o.ext_count = count;
#else
                    o.verts.assign(first, end);
                    o.ext = nullptr;
                    o.ext_count = 0;
#endif
                    o.copy_frame = false;
                    o.pending = nullptr;
                    o.mesh = 0;
                    o.tag = d.tag;
                    o.has_transform = d.has_transform;
                    if (d.has_transform) o.transform = d.transform;
                }
            }
            d.pending = nullptr;
        }
        in.used = 0;
    }
}
#endif

// A quad of GS-pixel corners (the GS origin taken off) as two triangles
// A sprite showing a whole texture along an axis samples it clamped there: drawn above the PS2's resolution its edge pixels
// fall between texels, and a repeating texture's filter then mixes in the opposite edge -- the lines between the tiles of
// the loading pictures. At the native resolution a pixel's centre is a texel's and the clamp changes nothing.
void ClampWholeTexture(ps2e_draw_state& st, float u0, float v0, float u1, float v1) {
    if (std::min(u0, u1) == 0.f && std::max(u0, u1) == 1.f) st.wrap_u = PS2E_DRAW_WRAP_CLAMP;
    if (std::min(v0, v1) == 0.f && std::max(v0, v1) == 1.f) st.wrap_v = PS2E_DRAW_WRAP_CLAMP;
}

void RecordQuad(uint32_t bucket, const ps2e_draw_state& st, float gx0, float gy0, float gx1, float gy1, float u0, float v0,
                float u1, float v1, uint32_t colour) {
    const float ox = GsOriginX(), oy = GsOriginY();
    const float x0 = gx0 - ox, y0 = gy0 - oy, x1 = gx1 - ox, y1 = gy1 - oy;
    Draw d;
    d.state = st;
    ClampWholeTexture(d.state, u0, v0, u1, v1);
    d.verts = {Vertex(x0, y0, u0, v0, colour), Vertex(x1, y0, u1, v0, colour), Vertex(x0, y1, u0, v1, colour),
               Vertex(x1, y0, u1, v0, colour), Vertex(x1, y1, u1, v1, colour), Vertex(x0, y1, u0, v1, colour)};
    ::Record(bucket, std::move(d));
}

// ---- the frame boundary -------------------------------------------------------------------------------------------------
#if defined(__vita__)
extern "C" int te_input_vita_window_pump_skipped(void);   // te_input.h
#endif
void PumpWindow() {
    TE_PROF_SCOPE("PumpWindow (SDL events)");
#if defined(__vita__)
    // The Vita: the pads' sampler pumps SDL's events at this boundary and the screen has no size or mode to follow -- a
    // second pump here repeated SDL's device polls for nothing (te_input.cpp; TE_VITA_PUMP=1: as before)
    if (te_input_vita_window_pump_skipped()) {
        g.last_pump_ms = 0;
        return;
    }
#endif
    const auto t0 = std::chrono::steady_clock::now();
    const bool closed = g.dev && ps2e_draw_pump(g.dev);
    g.last_pump_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (closed) {
        std::fprintf(stderr, "[te.gfx] window closed: exit\n");
        te::RequestExit(0);
    }
}

// One vertical blank: THE frame boundary, where the one game thread meets the clock (te::Clock::Boundary, the clock contract:
// the pace park at the pacer's field rate, the field tick, the EmuDomain settle, the boundary hooks -- the pads' sample,
// later the movie's present callback and the scheduler drain). Exactly one Boundary() per WaitVSync / Present /
// PresentFromInterrupt, in the place sceGsSyncV had (the services' boundary contract). The window is pumped here
// too. TE_GFX_UNPACED only matters when the clock is deterministic/unpaced itself (te::Settings): the pacing is the
// clock's, not the graphics side's.
void VBlank() {
    g.vblanks++;
    PumpWindow();
    te::Services::Get().clock().Boundary();
}

// The storage kept for the next frames is held to what the frame just drawn needed: the k-th draw of a bucket holds a
// different draw every frame (a merged run of a whole scene one frame, a sprite the next), so storage left at its largest
// grows towards (draws x the largest draw ever recorded) in every frame set -- the title's logo took the sets past 1 GB,
// and the Vita's heap with them. An oversized buffer is replaced by fresh storage of the size it held (its content is
// spent); the draws past the frame's own keep nothing, and only a few of them are kept at all.
// A frame set may keep up to kSpareBytes of such excess (the bound the 1 GB growth needed): within it an oversized buffer
// stays as it is -- the joined runs of blended draws move between draw slots from frame to frame, and trimming every one
// of them each frame cost the game thread a free and the drawing thread a regrowth (copies included) per run.
constexpr size_t kSpareBytes = size_t(2) << 20;
template <typename Vertices>
void TrimStorage(Vertices& verts, size_t keep, size_t& spare_bytes) {
    if (verts.capacity() <= keep * 2 + 64) return;
    const size_t excess = (verts.capacity() - keep) * sizeof(typename Vertices::value_type);
    if (excess <= spare_bytes) {
        spare_bytes -= excess;
        return;
    }
    Vertices fresh;
    fresh.reserve(keep);
    verts.swap(fresh);
}

void ClearBuckets() {
#if defined(__vita__)
    ++g_pool.recording;   // the next picture's records (the one just handed over / dropped kept the previous ordinal)
    g_pool.presents_counted++;
    if (g_pool.enabled && g_pool.ArmOn()) g_pool.presents_on++;
#endif
    constexpr size_t kSpareDraws = 64;
    size_t spare_bytes = kSpareBytes;
#if defined(__vita__)
    // only the records that may hold vertex storage (Bucket::holds), in index order: the others' TrimStorage would return
    // at once (capacity 0), so the trims -- and the spare budget they draw on, in the same order -- are the full sweep's.
    // Every 64th picture the full sweep as a safety net (re-marks what holds storage).
    static uint32_t sweeps = 0;
    const bool full = (sweeps++ & 63) == 0;
    for (auto& b : g.buckets) {
        if (b.draws.size() > b.used + kSpareDraws) b.draws.resize(b.used + kSpareDraws);
        const size_t n = b.draws.size();
        const size_t words = (n + 31) >> 5;
        if (b.holds.size() != words) b.holds.resize(words, 0u);
        if (n & 31) b.holds[words - 1] &= (1u << (n & 31)) - 1u;   // (the records past the end are gone)
        if (full) {
            for (size_t i = 0; i < n; ++i) {
                Draw& d = b.draws[i];
                if (g.prefetch_records && i + 6 < n) __builtin_prefetch(&b.draws[i + 6].verts, 0, 0);
                TrimStorage(d.verts, i < b.used ? d.verts.size() : 0, spare_bytes);
                const uint32_t bit = 1u << (i & 31);
                if (d.verts.capacity()) b.holds[i >> 5] |= bit;
                else b.holds[i >> 5] &= ~bit;
            }
        } else {
            for (size_t w = 0; w < words; ++w) {
                uint32_t bits = b.holds[w];
                while (bits) {
                    const uint32_t k = uint32_t(__builtin_ctz(bits));
                    bits &= bits - 1u;
                    const size_t i = (w << 5) + k;
                    Draw& d = b.draws[i];
                    TrimStorage(d.verts, i < b.used ? d.verts.size() : 0, spare_bytes);
                    if (!d.verts.capacity()) b.holds[w] &= ~(1u << k);
                }
            }
        }
        b.used = 0;   // the storage stays
    }
#else
    for (auto& b : g.buckets) {
        if (b.draws.size() > b.used + kSpareDraws) b.draws.resize(b.used + kSpareDraws);
        const size_t n = b.draws.size();
        for (size_t i = 0; i < n; ++i) TrimStorage(b.draws[i].verts, i < b.used ? b.draws[i].verts.size() : 0, spare_bytes);
        b.used = 0;   // the storage stays
    }
#endif
    for (size_t i = 0; i < g.deferred.size(); ++i) {
        DeferredBuild& build = *g.deferred[i];
        const bool held = i < g.deferred_used;
        TrimStorage(build.verts, held ? build.verts.size() : 0, spare_bytes);
        TrimStorage(build.segments, held ? build.segments.size() : 0, spare_bytes);
    }
    g.deferred_used = 0;
}

// The pictures' dumps, numbered by PICTURE (= by present in a synchronous run: the frozen judges' numbering)
void MaybeDump(uint64_t picture) {
    if (g.dump_dir.empty() || !g.dev) return;
    const bool at = g.dump_at > 0 && picture == uint64_t(g.dump_at);
    const bool in_window = (g.dump_from <= 0 || picture >= uint64_t(g.dump_from)) && (g.dump_to <= 0 || picture <= uint64_t(g.dump_to));
    const bool every = g.dump_every > 0 && in_window && picture % uint64_t(g.dump_every) == 0;
    if (!at && !every) return;
    char path[1024];
    std::snprintf(path, sizeof path, "%s/frame_%06llu.ppm", g.dump_dir.c_str(), static_cast<unsigned long long>(picture));
    // The frame's own counts follow its path (the judges' draws-per-frame floor reads them)
    if (ps2e_draw_write_ppm(g.dev, path))
        std::fprintf(stderr, "[te.gfx] frame %llu -> %s (draws %llu, triangles %llu, scene rigid %llu placed %llu sky %llu shadows %llu)\n",
                     static_cast<unsigned long long>(picture), path, (unsigned long long)g.frame_draws.load(), (unsigned long long)g.frame_triangles.load(),
                     (unsigned long long)(te::gfx::Scene().rigid_models - g.scene_mark[0]), (unsigned long long)(te::gfx::Scene().placed_models - g.scene_mark[1]),
                     (unsigned long long)(te::gfx::Scene().sky_models - g.scene_mark[2]), (unsigned long long)(te::gfx::Scene().shadow_meshes - g.scene_mark[3]));
    g.scene_mark[0] = te::gfx::Scene().rigid_models;
    g.scene_mark[1] = te::gfx::Scene().placed_models;
    g.scene_mark[2] = te::gfx::Scene().sky_models;
    g.scene_mark[3] = te::gfx::Scene().shadow_meshes;
}

// The frame's GL work: the buckets in their order (bucket 0 holds StartFrame's clear), then the scan-out -- shown
// (Present), resolved for the armed capture only (Capture: a picture the cadence does not show), or dropped (None: no GL
// work at all). On the render thread when there is one, else the game thread's.
void BeginFrameTarget(int screen_w, int screen_h) {
    int fw = 0, fh = 0;
    ps2e_draw_frame_size(g.dev, &fw, &fh);
    if (g.have_frame && (fw != screen_w || fh != screen_h)) ps2e_draw_resize_frame(g.dev, screen_w, screen_h);
    ps2e_draw_begin_frame(g.dev, nullptr);
}

// A picture's draws as the interpolator reads them (bucket order), built once per picture (the vector is the slot's)
void BuildInterpItems(Gfx::Frame& f) {
    if (!f.items.empty()) return;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
#if defined(__vita__)
        // (the Vita's drawn list is the bucket's order once it is made: ExpandBucket)
        const bool by_order = f.buckets[b].ordered;
        const size_t n = by_order ? f.buckets[b].order.size() : f.buckets[b].used;
        for (size_t i = 0; i < n; ++i) {
            const Draw& d = by_order ? *f.buckets[b].order[i] : f.buckets[b].draws[i];
#else
        for (size_t i = 0; i < f.buckets[b].used; ++i) {
            const Draw& d = f.buckets[b].draws[i];
#endif
            ps2r::ps2e::InterpItem it;
            it.state = &d.state;
            it.verts = d.verts.data();
            it.count = uint32_t(d.verts.size());
            it.copy_frame = d.copy_frame;
            it.tag = d.tag;
            it.transform = d.has_transform ? &d.transform : nullptr;
            f.items.push_back(it);
        }
    }
}

// The drawing thread's CPU per draw by the path the backend took (the Vita; 1 draw in TE_GFX_DRAW_TIMING (default 32) timed
// with the kernel's microsecond clock -- each timed draw costs two clock calls): ps2e_draw's submit + the GL backend +
// vitaGL, its glDrawArrays included -- [0] mesh full, [1] mesh again, [2] mesh texture swap, [3] vertices (the producer's,
// through the ring), [4] shadow (stencil) vertices, [5] a grouped run of shadow-volume counts (per run), [6] a GPU-skinned
// part's draw, [7] a mesh draw replayed from its packet, [8] a run of one mesh range under one state handed over
// whole (ps2e_draw_submit_mesh_instances; per run)
struct DrawTiming {
    uint64_t us[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0}, n[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t tick = 0;
};
#if defined(__vita__)
constexpr size_t kMaxMeshRun = 256;   // a run's draws handed over at once (the device draws up to 64 a draw)
// two states byte-equal (memcmp's answer, word by word: 28 aligned words)
inline bool SameDrawState(const ps2e_draw_state& a, const ps2e_draw_state& b) {
    typedef uint32_t __attribute__((may_alias)) W;
    static_assert(sizeof(ps2e_draw_state) % 4 == 0 && alignof(ps2e_draw_state) >= 4, "the state compares as words");
    const W* x = reinterpret_cast<const W*>(&a);
    const W* y = reinterpret_cast<const W*>(&b);
    uint32_t diff = 0;
    for (size_t i = 0; i < sizeof(ps2e_draw_state) / 4; ++i) diff |= x[i] ^ y[i];
    return diff == 0;
}
#endif
DrawTiming g_draw_timing;
#if defined(__vita__)
// the drawing thread's vertex copies (the vertex-copies census line): the shadow volumes' group concatenations --
// straight into the vertex ring, or into a group buffer the draw then copies into the ring
struct Render2Census {
    uint64_t volume_inplace_bytes = 0, volume_copied_bytes = 0;
    uint64_t volume_box_neon = 0, volume_box_scalar = 0;   // the volumes' box / Q passes by form (VolumeBoxNeon)
};
Render2Census g_r2;
// A shadow volume's screen box -- the smallest x, y and the largest x, y of its vertices, std::min / std::max in
// vertex order -- and whether a Q is not finite, in one pass with the Advanced SIMD unit's min / max on (x, y) pairs:
// no VFP compare and flag transfer per coordinate (four a vertex in the scalar pass, each waiting for the VFP pipeline).
// NEON's VMIN / VMAX answer as std::min / std::max do for every normal or infinite, non-zero value (the same bits: min /
// max round nothing); they differ for a NaN (std::min keeps the other operand, NEON gives the default NaN), for zeros
// (-0 against +0) and denormals (NEON flushes them) -- a volume with any such x or y is left to the scalar pass (false).
inline bool VolumeBoxNeon(const ps2e_draw_vertex* src, size_t tri, float* x0, float* y0, float* x1, float* y1, uint32_t* bad) {
    float32x2_t mn = vdup_n_f32(1e30f), mx = vdup_n_f32(-1e30f);
    uint32x2_t odd = vdup_n_u32(0);   // a lane that met a NaN, a zero or a denormal
    const uint32x2_t expo = vdup_n_u32(0x7F800000u), none = vdup_n_u32(0);
    uint32_t b = 0;
    for (size_t u = 0; u < tri; ++u) {
        if ((u & 3) == 0) __builtin_prefetch(&src[u + 16]);   // (the builders' lines, ~half a KB ahead)
        const float32x2_t xy = vld1_f32(&src[u].x);           // (x, y: the record's first two words)
        mn = vmin_f32(mn, xy);
        mx = vmax_f32(mx, xy);
        // not equal to itself: a NaN; exponent bits all clear: a zero or a denormal
        odd = vorr_u32(odd, vmvn_u32(vceq_f32(xy, xy)));
        odd = vorr_u32(odd, vceq_u32(vand_u32(vreinterpret_u32_f32(xy), expo), none));
        uint32_t qb;
        std::memcpy(&qb, &src[u].q, sizeof qb);
        b |= (qb & 0x7F800000u) == 0x7F800000u ? 1u : 0u;
    }
    if ((vget_lane_u32(odd, 0) | vget_lane_u32(odd, 1)) != 0) return false;
    *x0 = vget_lane_f32(mn, 0);
    *y0 = vget_lane_f32(mn, 1);
    *x1 = vget_lane_f32(mx, 0);
    *y1 = vget_lane_f32(mx, 1);
    *bad = b;
    return true;
}
#endif
inline uint64_t DrawClockUs() {
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
    return 0;
#endif
}

// A shadow volume's counting draw (ps2e_draw.h: COUNT_UP / COUNT_DOWN, no colour, no depth writes): the stencil's low bits
// alone change (SubmitBuckets groups a run of them by state)
bool CountingDraw(const Draw& d) {
    const ps2e_draw_state& s = d.state;
    return !d.mesh && !d.copy_frame && (s.stencil == PS2E_DRAW_STENCIL_COUNT_UP || s.stencil == PS2E_DRAW_STENCIL_COUNT_DOWN) &&
           s.no_colour_write == 1 && s.depth_write == 0 && d.VertexCount() >= 3;
}

// A run of `n` counting draws drawn as one draw per distinct state, the states in the order they first appear (their
// whole triangles concatenated in run order)
#if defined(__vita__)
// The Vita: ONE pass over each volume's vertices (the builders' output, written on other cores: every line a miss) -- its
// screen box for the casters' re-stamp, its Qs checked finite (the raw draw's condition) -- then the volume's vertices
// appended to their group, in run order; the group's draw skips its Q check (ps2e_draw_submit_finite). THE
// GROUPS CONCATENATED IN PLACE: the run is walked twice -- first its records alone (no vertex read): its windows (a window
// ends where a fifth state appears: the one-pass form drew its groups there) and each window group's vertex total; each
// group's storage then reserved in the device's vertex ring (ps2e_draw_reserve_vertices) and every volume's vertices
// copied straight there after the box / Q pass -- the group drawn where it lies: no group buffer between the builders'
// storage and the GPU's (the one-pass form copied every vertex into a group buffer, then the draw copied the buffer into
// the ring). No room in the ring (or TE_GFX_VITA_VOLUME_INPLACE=0): the group buffer as before. The same windows, groups,
// boxes and vertices in the same order: the same draws.
void SubmitVolumeRun(Draw* const* run, size_t n, uint64_t& triangles) {
    struct Group {
        ps2e_draw_state state;
        ps2e_draw_vertex* v = nullptr;   // (plain storage, kept across frames: appended to without a zeroing pass)
        size_t used = 0, cap = 0;
        bool infinite_q = false;         // a Q not finite among them: drawn by ps2e_draw_submit (its check finds it)
        size_t total = 0;                // the window's vertices of this group (its records' counts)
        ps2e_draw_vertex* ring = nullptr;   // its storage in the vertex ring for the window (null: `v`)
        float box[4] = {1e30f, 1e30f, -1e30f, -1e30f};   // (the fill census: its volumes' boxes' union)
    };
    static std::vector<Group> groups;   // (the drawing thread's alone: not a thread_local, the Vita's emulated TLS)
    static const bool inplace = [] {
        const char* e = std::getenv("TE_GFX_VITA_VOLUME_INPLACE");
        return !(e && e[0] == '0');
    }();
    static const bool neon = [] {   // (TE_GFX_VITA_VOLUME_NEON=0: every volume's box / Q pass scalar, as before)
        const char* e = std::getenv("TE_GFX_VITA_VOLUME_NEON");
        return !(e && e[0] == '0');
    }();
    size_t used = 0;
    auto flush = [&] {
        for (size_t k = 0; k < used; ++k) {
            Group& gr = groups[k];
            // the fill census: the group's true screen box (its volumes' vertices: the union of their boxes -- the same
            // extremes std::min / std::max over all of them give)
            if (ps2r::render::FillCensusSampling() && gr.used) ps2r::render::FillCensusBox(gr.box);
            const ps2e_draw_vertex* const verts = gr.ring ? gr.ring : gr.v;
            const int r = gr.infinite_q ? ps2e_draw_submit(g.dev, &gr.state, verts, int(gr.used))
                                        : ps2e_draw_submit_finite(g.dev, &gr.state, verts, int(gr.used));
            if (r < 0) g.dropped_draws++;
            triangles += gr.used / 3;
            g.volume_vertices += gr.used;
            gr.used = 0;
            gr.infinite_q = false;
            gr.ring = nullptr;
            g.volume_draws_out++;
        }
        used = 0;
    };
    ps2e_draw_set_volume_boxes_given(g.dev, 1);
    size_t i = 0;
    while (i < n) {
        // the window [i, j): its groups by first appearance (at most four states) and their vertex totals
        size_t j = i;
        for (; j < n; ++j) {
            const Draw& d = *run[j];
            size_t k = 0;
            while (k < used && std::memcmp(&groups[k].state, &d.state, sizeof d.state) != 0) ++k;
            if (k == used) {
                if (used == 4) break;   // a fifth state: the window's groups drawn first (any order of the run is exact)
                if (groups.size() <= used) groups.emplace_back();
                Group& ng = groups[used];
                ng.state = d.state;
                ng.used = 0;
                ng.infinite_q = false;
                ng.total = 0;
                ng.ring = nullptr;
                ng.box[0] = ng.box[1] = 1e30f;
                ng.box[2] = ng.box[3] = -1e30f;
                k = used++;
            }
            groups[k].total += d.VertexCount() / 3 * 3;
        }
        // each group's storage: the ring's room, else its own buffer grown to the total
        for (size_t k = 0; k < used; ++k) {
            Group& gr = groups[k];
            gr.ring = inplace && gr.total <= 0x7FFFFFFFu ? ps2e_draw_reserve_vertices(g.dev, int(gr.total)) : nullptr;
            if (!gr.ring && gr.total > gr.cap) {
                void* grown = std::realloc(gr.v, gr.total * sizeof(ps2e_draw_vertex));   // (ps2e_draw_vertex: plain data)
                if (grown) {
                    gr.v = static_cast<ps2e_draw_vertex*>(grown);
                    gr.cap = gr.total;
                }
            }
        }
        // the window's volumes in run order: the box / Q pass over each one's vertices, then the vertices into the group's storage
        for (size_t t = i; t < j; ++t) {
            const Draw& d = *run[t];
            size_t k = 0;
            while (std::memcmp(&groups[k].state, &d.state, sizeof d.state) != 0) ++k;   // (it is one of the window's)
            Group& gr = groups[k];
            const size_t tri = d.VertexCount() / 3 * 3;
            ps2e_draw_vertex* const base = gr.ring ? gr.ring : gr.v;
            if (!base || (!gr.ring && gr.used + tri > gr.cap)) {
                g.dropped_draws++;   // (no storage: as the one-pass form's failed grow)
                continue;
            }
            const ps2e_draw_vertex* src = d.Vertices();
            // (BoxOf's expressions: std::min / std::max in vertex order)
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            uint32_t bad = 0;
            if (!neon || !VolumeBoxNeon(src, tri, &x0, &y0, &x1, &y1, &bad)) {
                // (a coordinate NEON's min / max would not answer as std::min / std::max do -- a NaN, a zero, a
                // denormal: the scalar pass)
                x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                bad = 0;
                for (size_t u = 0; u < tri; ++u) {
                    if ((u & 3) == 0) __builtin_prefetch(&src[u + 16]);   // (the builders' lines, ~half a KB ahead)
                    const ps2e_draw_vertex& sv = src[u];
                    x0 = std::min(x0, sv.x);
                    y0 = std::min(y0, sv.y);
                    x1 = std::max(x1, sv.x);
                    y1 = std::max(y1, sv.y);
                    uint32_t qb;
                    std::memcpy(&qb, &sv.q, sizeof qb);
                    bad |= (qb & 0x7F800000u) == 0x7F800000u ? 1u : 0u;
                }
                g_r2.volume_box_scalar++;
            } else {
                g_r2.volume_box_neon++;
            }
            // (the volume's lines are in the cache from the pass: one block copy -- into the GPU's memory when in place)
            std::memcpy(base + gr.used, src, tri * sizeof(ps2e_draw_vertex));
            if (gr.ring) g_r2.volume_inplace_bytes += tri * sizeof(ps2e_draw_vertex);
            else g_r2.volume_copied_bytes += tri * sizeof(ps2e_draw_vertex);
            gr.used += tri;
            if (bad) gr.infinite_q = true;
            gr.box[0] = std::min(gr.box[0], x0);
            gr.box[1] = std::min(gr.box[1], y0);
            gr.box[2] = std::max(gr.box[2], x1);
            gr.box[3] = std::max(gr.box[3], y1);
            // its own screen box for the casters' re-stamp, as its own submit would have noted it (CountingDraw: >= 3 vertices)
            const float box[4] = {x0, y0, x1, y1};
            ps2e_draw_note_volume_rect(g.dev, box);
        }
        flush();
        i = j;
    }
    ps2e_draw_set_volume_boxes_given(g.dev, 0);
    g.volume_draws_in += n;
}
#else
void SubmitVolumeRun(Draw* const* run, size_t n, uint64_t& triangles) {
    struct Group {
        ps2e_draw_state state;
        std::vector<ps2e_draw_vertex> verts;
    };
    static thread_local std::vector<Group> groups;
    size_t used = 0;
    auto flush = [&] {
        for (size_t k = 0; k < used; ++k) {
            Group& gr = groups[k];
#if defined(__vita__)
            if (ps2r::render::FillCensusSampling() && !gr.verts.empty()) {
                // the fill census: the group's true screen box (its volumes' vertices)
                float bx[4] = {1e30f, 1e30f, -1e30f, -1e30f};
                for (const ps2e_draw_vertex& v : gr.verts) {
                    bx[0] = std::min(bx[0], v.x); bx[1] = std::min(bx[1], v.y);
                    bx[2] = std::max(bx[2], v.x); bx[3] = std::max(bx[3], v.y);
                }
                ps2r::render::FillCensusBox(bx);
            }
#endif
            if (ps2e_draw_submit(g.dev, &gr.state, gr.verts.data(), int(gr.verts.size())) < 0) g.dropped_draws++;
            triangles += gr.verts.size() / 3;
            gr.verts.clear();
            g.volume_draws_out++;
        }
        used = 0;
    };
    // each volume's own screen box for the casters' re-stamp, as its own submit would have noted it
    for (size_t i = 0; i < n; ++i) ps2e_draw_note_volume_box(g.dev, run[i]->Vertices(), int(run[i]->VertexCount()));
    ps2e_draw_set_volume_boxes_given(g.dev, 1);
    for (size_t i = 0; i < n; ++i) {
        const Draw& d = *run[i];
        size_t k = 0;
        while (k < used && std::memcmp(&groups[k].state, &d.state, sizeof d.state) != 0) ++k;
        if (k == used) {
            if (used == 4) {   // a fifth state in one run: the groups so far drawn (any order of the run is exact)
                flush();
                k = 0;
            }
            if (groups.size() <= used) groups.emplace_back();
            groups[used].state = d.state;
            groups[used].verts.clear();
            k = used++;
        }
        const size_t tri = d.VertexCount() / 3 * 3;
        groups[k].verts.insert(groups[k].verts.end(), d.Vertices(), d.Vertices() + tri);
    }
    flush();
    ps2e_draw_set_volume_boxes_given(g.dev, 0);
    g.volume_draws_in += n;
}
#endif

#if defined(__vita__)
// A triangle's area inside the frame [0, w] x [0, h] (Sutherland-Hodgman against the four edges, then the shoelace)
double ClippedTriangleArea(float ax, float ay, float bx, float by, float cx, float cy, float w, float h) {
    float px[10] = {ax, bx, cx}, py[10] = {ay, by, cy};
    int n = 3;
    for (int edge = 0; edge < 4 && n > 0; ++edge) {
        float qx[10], qy[10];
        int m = 0;
        auto inside = [&](float x, float y) {
            return edge == 0 ? x >= 0.f : edge == 1 ? x <= w : edge == 2 ? y >= 0.f : y <= h;
        };
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const bool ii = inside(px[i], py[i]), ij = inside(px[j], py[j]);
            if (ii) { qx[m] = px[i]; qy[m] = py[i]; ++m; }
            if (ii != ij) {
                const float bound = edge == 0 ? 0.f : edge == 1 ? w : edge == 2 ? 0.f : h;
                const bool on_x = edge < 2;
                const float t = on_x ? (bound - px[i]) / (px[j] - px[i]) : (bound - py[i]) / (py[j] - py[i]);
                qx[m] = on_x ? bound : px[i] + t * (px[j] - px[i]);
                qy[m] = on_x ? py[i] + t * (py[j] - py[i]) : bound;
                ++m;
            }
        }
        n = m;
        for (int i = 0; i < n; ++i) { px[i] = qx[i]; py[i] = qy[i]; }
    }
    double a = 0;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        a += double(px[i]) * double(py[j]) - double(px[j]) * double(py[i]);
    }
    return std::fabs(a) * 0.5;
}
// The fill census's true screen box of a draw (a sampled frame's): a mesh's bounds through its matrix, a vertex draw's
// vertices; "not known" (x1 < x0) for a skin or a mesh with a corner at or behind the eye (the draw's own box then counts).
// A vertex draw also hands over its triangles' area inside the frame (the fragments it generates, its overdraw included)
void FillCensusBoxOf(const Draw& d, int screen_w, int screen_h) {
    float bx[4] = {0.f, 0.f, -1.f, -1.f};
    ps2r::render::FillCensusArea(-1.0);   // (not known: a mesh's; a vertex draw's below)
    if (d.mesh) {
        if (d.skin_joints || !ps2e_draw_mesh_screen_box(g.dev, d.mesh, &d.mesh_params, bx)) {
            bx[0] = bx[1] = 0.f;
            bx[2] = bx[3] = -1.f;
        }
    } else if (d.VertexCount()) {
        const ps2e_draw_vertex* v = d.Vertices();
        bx[0] = bx[1] = 1e30f;
        bx[2] = bx[3] = -1e30f;
        for (size_t k = 0, n = d.VertexCount(); k < n; ++k) {
            bx[0] = std::min(bx[0], v[k].x);
            bx[1] = std::min(bx[1], v[k].y);
            bx[2] = std::max(bx[2], v[k].x);
            bx[3] = std::max(bx[3], v[k].y);
        }
        double area = 0;
        const float w = float(std::max(1, screen_w)), h = float(std::max(1, screen_h));
        for (size_t k = 0, n = d.VertexCount() / 3 * 3; k < n; k += 3)
            area += ClippedTriangleArea(v[k].x, v[k].y, v[k + 1].x, v[k + 1].y, v[k + 2].x, v[k + 2].y, w, h);
        ps2r::render::FillCensusArea(area);
    }
    ps2r::render::FillCensusBox(bx);
}

// ---- THE SKY AFTER THE OPAQUE SCENE (TE_GFX_VITA_SKY_LAST=0: off) -------------------------------------------------------
// The game draws its sky first (bucket 0: layers with no depth test and no depth write, most of them blended -- the title's
// island pan: 9 draws whose triangles cover 6.2 frames' worth) and then RESETS the depth to the far value with a
// full-frame, colour-less quad at z 0; the scene follows with GEQUAL / GREATER tests. On the device's deferred renderer every
// fragment of a blended layer is shaded, and the sky drawn first is shaded under every pixel the scene covers later (the
// island and the sea: most of the pan's frame). Here the sky's draws wait while the scene's draws that cannot depend on the
// colour beneath them are drawn -- opaque, depth-writing, GEQUAL / GREATER, no stencil, no screen copy, no destination
// alpha test, an alpha test only when it keeps both its colour and its depth (AFAIL KEEP), and every fragment's depth above
// the far value (GREATER; or GEQUAL with the draw's smallest GS z at least kSkyMinZ: a mesh's bounds through its matrix, a
// GPU skin's joints' images of its bind box (convex weights: ps2e_draw_skin_depth_far), a vertex draw's vertices) -- and are
// drawn, in their order, right before the first draw that is not such a draw (or at the frame's end), with their vertices
// at z 0 and a GEQUAL test: they then pass
// exactly where the depth is still the reset's far value, i.e. where none of the draws they waited for wrote (each of those
// writes a depth above it wherever it writes its colour: GEQUAL / GREATER only raise a pixel's depth from the reset's 0).
// EXACT: at a pixel those draws wrote, their last colour stood over the sky's anyway (opaque, every channel: no blend, no
// colour mask) and still does; at a pixel they did not write, the sky's layers composite over the same colour (the frame's
// clear and the layers before them) as before; the depth the scene reads is the reset's either way (the sky writes none),
// and every draw after the insertion point sees the frame as it was. Not the shadows' casters (ps2e_draw_set_far_background:
// as when drawn first with no test). The frame's clear and the reset itself are unchanged.
constexpr float kSkyMinZ = 1024.f;   // GS z: z / 2^31 - 1 is then above -1 in float (the Vita's depth above the reset's 0)
struct SkyMove {
    static constexpr size_t kMaxLayers = 32;
    bool pending = false;
    const Draw* layers[kMaxLayers] = {};   // bucket 0's sky draws in drawing order (the frame's records: alive until it is presented)
    size_t count = 0;
    uint64_t over = 0;               // the scene's draws drawn before the sky this frame
};
struct SkyCensus {
    uint64_t frames = 0, moved = 0, sky_draws = 0, over = 0, no_pattern = 0;
    uint64_t stops[12] = {};         // why the sky was drawn where it was (SkyStop)
};
SkyCensus g_sky;
enum SkyStop { kStopEnd, kStopCopy, kStopStencil, kStopNoColour, kStopBlend, kStopDepthTest, kStopNoDepthWrite,
               kStopAlphaFail, kStopDate, kStopSkin, kStopMeshDepth, kStopNearFar, kStopCount };
const char* const kSkyStopNames[kStopCount] = {"the frame's end", "a screen copy", "a stencil draw", "a depth-only draw",
                                               "a blended draw", "another depth test", "no depth write",
                                               "an alpha test writing its failures", "a destination alpha test", "a skin",
                                               "a mesh not wholly in front", "a depth near the far value"};
bool SkyLastOn() {
    static const bool on = [] { const char* v = std::getenv("TE_GFX_VITA_SKY_LAST"); return !(v && v[0] == '0'); }();
    return on;
}
// bucket 0's last draw: the full-frame depth reset to the far value (no colour, no test, depth written, every vertex at z
// 0, two triangles splitting a rectangle that holds the frame along a diagonal, no scissor)
bool SkyResetQuad(const Draw& d, int w, int h) {
    const ps2e_draw_state& s = d.state;
    if (d.mesh || d.copy_frame || d.VertexCount() != 6 || !s.no_colour_write || s.depth_test != PS2E_DRAW_DEPTH_ALWAYS ||
        !s.depth_write || s.stencil != PS2E_DRAW_STENCIL_OFF || (s.scissor_x0 | s.scissor_y0 | s.scissor_x1 | s.scissor_y1))
        return false;
    const ps2e_draw_vertex* v = d.Vertices();
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int k = 0; k < 6; ++k) {
        if (v[k].z != 0.f) return false;
        x0 = std::min(x0, v[k].x); x1 = std::max(x1, v[k].x);
        y0 = std::min(y0, v[k].y); y1 = std::max(y1, v[k].y);
    }
    if (!(x0 <= 0.f && y0 <= 0.f && x1 >= float(w) && y1 >= float(h))) return false;
    int c[6];
    for (int k = 0; k < 6; ++k) {
        const int cx = v[k].x == x0 ? 0 : v[k].x == x1 ? 1 : -1, cy = v[k].y == y0 ? 0 : v[k].y == y1 ? 1 : -1;
        if (cx < 0 || cy < 0) return false;
        c[k] = cx + 2 * cy;
    }
    auto distinct = [](int a, int b, int e) { return a != b && b != e && a != e; };
    if (!distinct(c[0], c[1], c[2]) || !distinct(c[3], c[4], c[5])) return false;
    const int m1 = (1 << c[0]) | (1 << c[1]) | (1 << c[2]), m2 = (1 << c[3]) | (1 << c[4]) | (1 << c[5]);
    const int shared = m1 & m2;
    return (m1 | m2) == 15 && (shared == 9 || shared == 6);   // all four corners, the two triangles over a diagonal
}
// a sky layer: a vertex draw that tests no depth and writes none (no stencil, no screen copy)
bool SkyLayer(const Draw& d) {
    const ps2e_draw_state& s = d.state;
    return !d.mesh && !d.copy_frame && d.VertexCount() >= 3 && s.depth_test == PS2E_DRAW_DEPTH_ALWAYS && !s.depth_write &&
           s.stencil == PS2E_DRAW_STENCIL_OFF;
}
// a scene draw the sky may wait for (see above); else the reason it may not
int SkyStopOf(const Draw& d) {
    const ps2e_draw_state& s = d.state;
    if (d.copy_frame) return kStopCopy;
    if (s.stencil != PS2E_DRAW_STENCIL_OFF) return kStopStencil;
    if (s.no_colour_write) return kStopNoColour;
    if (s.blend) return kStopBlend;
    if (s.depth_test != PS2E_DRAW_DEPTH_GEQUAL && s.depth_test != PS2E_DRAW_DEPTH_GREATER) return kStopDepthTest;
    if (!s.depth_write) return kStopNoDepthWrite;
    if (s.alpha_test && s.alpha_fail != 0) return kStopAlphaFail;
    if (s.dest_alpha_test) return kStopDate;
    if (s.depth_test == PS2E_DRAW_DEPTH_GREATER) return -1;   // a fragment it writes is above the reset's 0 by its test
    if (d.mesh) {
        if (d.skin_joints) {
            // a GPU skin: the hull of its joints' images of its bind box (convex weights; a blend part's displacements and
            // a skin whose weights are not convex: not bounded)
            float z_far = 0.f;
            if (d.skin_disp || d.palette.size() < size_t(d.skin_joints) * 12 ||
                !ps2e_draw_skin_depth_far(g.dev, d.mesh, &d.mesh_params, d.palette.data(), int(d.skin_joints), &z_far))
                return kStopSkin;
            return z_far >= kSkyMinZ ? -1 : kStopNearFar;
        }
        // (depth_range's z_far >= kSkyMinZ asked directly -- the same answer without the corners' divisions)
        const int above = ps2e_draw_mesh_depth_far_above(g.dev, d.mesh, &d.mesh_params, kSkyMinZ);
        if (above < 0) return kStopMeshDepth;
        return above ? -1 : kStopNearFar;
    }
    const ps2e_draw_vertex* v = d.Vertices();
    for (size_t k = 0, n = d.VertexCount(); k < n; ++k)
        if (!(v[k].z >= kSkyMinZ)) return kStopNearFar;
    return -1;
}
// the waiting sky drawn here: its layers in their order, at z 0 with a GEQUAL test (no depth write)
void FlushSky(SkyMove& sky, int stop, int screen_w, int screen_h, bool fill, int bucket_now, uint64_t& triangles) {
    if (!sky.pending) return;
    sky.pending = false;
    te::gfx::CopiesDirtyAll();   // (the layers' extent is not bounded here: a copy after them is taken)
    static const unsigned short profSky = te::prof::Register("rt the sky after the opaque scene");
    te::prof::Scope scope(profSky, te::prof::ThreadSlot());
    static std::vector<ps2e_draw_vertex> scratch;   // (the device copies a submit's vertices: reused per layer)
    if (fill) ps2r::render::FillCensusGroup(TE_GFX_BUCKET_SKY);
    ps2e_draw_set_far_background(g.dev, 1);
    for (size_t i = 0; i < sky.count; ++i) {
        const Draw& d = *sky.layers[i];
        const size_t n = d.VertexCount();
        scratch.assign(d.Vertices(), d.Vertices() + n);
        for (ps2e_draw_vertex& v : scratch) v.z = 0.f;
        ps2e_draw_state st = d.state;
        st.depth_test = PS2E_DRAW_DEPTH_GEQUAL;   // (its depth_write stays 0)
        if (fill) FillCensusBoxOf(d, screen_w, screen_h);
        if (ps2e_draw_submit(g.dev, &st, scratch.data(), int(n)) < 0) g.dropped_draws++;
        triangles += n / 3;
    }
    ps2e_draw_set_far_background(g.dev, 0);
    if (fill) ps2r::render::FillCensusGroup(bucket_now);
    g_sky.moved++;
    g_sky.sky_draws += sky.count;
    g_sky.over += sky.over;
    g_sky.stops[std::clamp(stop, 0, kStopCount - 1)]++;
}
#endif
std::string SkyCensusLine(double frames) {
#if defined(__vita__)
    if (!SkyLastOn()) return "[te.gfx.vita] the sky after the opaque scene: off (TE_GFX_VITA_SKY_LAST=0)\n";
    if (!g_sky.frames) return std::string();
    std::string stops;
    for (int k = 0; k < kStopCount; ++k) {
        if (!g_sky.stops[k]) continue;
        char b[96];
        std::snprintf(b, sizeof b, "%s%s %llu", stops.empty() ? "" : ", ", kSkyStopNames[k], (unsigned long long)g_sky.stops[k]);
        stops += b;
    }
    char buf[700];
    std::snprintf(buf, sizeof buf, "[te.gfx.vita] the sky after the opaque scene (TE_GFX_VITA_SKY_LAST): %llu of %llu frames (%llu without "
                  "the sky + depth reset pattern); per moved frame %.1f sky layers drawn after %.1f scene draws, at z 0 with a GEQUAL "
                  "test: shaded only where nothing opaque covers them; drawn before: %s\n",
                  (unsigned long long)g_sky.moved, (unsigned long long)g_sky.frames, (unsigned long long)g_sky.no_pattern,
                  g_sky.moved ? double(g_sky.sky_draws) / double(g_sky.moved) : 0.0,
                  g_sky.moved ? double(g_sky.over) / double(g_sky.moved) : 0.0, stops.empty() ? "-" : stops.c_str());
    (void)frames;
    g_sky = SkyCensus{};
    return buf;
#else
    (void)frames;
    return std::string();
#endif
}

uint64_t SubmitBuckets(Gfx::Bucket* buckets, int screen_w, int screen_h, bool count_as_picture) {
#if defined(__vita__)
    // TE_GFX_DRAWHASH=1 (diagnosis): one line per picture with a hash of the CONTENT of the draws the loop below
    // submits, in order -- each draw's bucket, state fields, frame copy, its vertices' bytes (own or a view of a build's), its
    // mesh and the mesh parameters a draw reads (the light rows only when lit), the skin palette -- so two runs of one
    // deterministic route compare their draw lists picture for picture without a picture (Vita3K's read-back is empty): the
    // later seam forms against TE_VITA_LATE_SEAMS=0's
    static const bool drawHash = [] { const char* v = std::getenv("TE_GFX_DRAWHASH"); return v && v[0] == '1'; }();
    uint64_t pictureHash = 1469598103934665603ull, pictureDraws = 0;
    const auto mix = [&pictureHash](const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) pictureHash = (pictureHash ^ q[i]) * 1099511628211ull;
    };
    const auto hashDraw = [&](int b, const Draw& d) {
        const ps2e_draw_state& s = d.state;
        const int fields[] = {b, int(s.texture), s.gouraud, s.depth_test, s.depth_write, s.blend, s.blend_a, s.blend_b, s.blend_c,
                              s.blend_d, s.blend_fix, s.alpha_test, s.alpha_method, s.alpha_reference, s.alpha_fail, s.linear,
                              s.wrap_u, s.wrap_v, s.tfx, s.tcc, s.scissor_x0, s.scissor_y0, s.scissor_x1, s.scissor_y1,
                              s.no_colour_write, s.stencil, s.fba, s.dest_alpha_test, s.mip_max_level, s.mip_k, s.mip_l,
                              s.mip_minify, d.copy_frame ? 1 : 0, int(d.mesh), int(d.VertexCount()), int(d.skin_joints)};
        mix(fields, sizeof fields);
        if (d.VertexCount()) mix(d.Vertices(), d.VertexCount() * sizeof(*d.Vertices()));
        if (d.mesh) {
            const ps2e_draw_mesh_params& m = d.mesh_params;
            mix(m.screen, sizeof m.screen);
            mix(m.offset, sizeof m.offset);
            mix(m.scroll, sizeof m.scroll);
            mix(&m.lit, sizeof m.lit);
            if (m.lit) { mix(m.light_dir, sizeof m.light_dir); mix(m.light_colour, sizeof m.light_colour); mix(m.ambient, sizeof m.ambient); }
            mix(&m.first, sizeof m.first);
            mix(&m.count, sizeof m.count);
            if (d.skin_joints && !d.palette.empty()) mix(d.palette.data(), d.palette.size() * sizeof(float));
        }
        ++pictureDraws;
    };
#endif
    // the frame's screen copies (copy_frame draws): announced on the Vita before its first draw -- the Vita keeps the frame's
    // depth / stencil across the GPU scene break each copy is (ps2e_draw_expect_frame_copies), only in a frame that takes
    // copies -- and coalesced on every platform (graphics_copies.cpp: a copy is skipped where the frame's last one holds the
    // region its draws sample)
    int frameCopies = 0;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b)
        for (size_t i = 0; i < buckets[b].used; ++i) {
#if defined(__vita__)
            if (i + 4 < buckets[b].used) __builtin_prefetch(&buckets[b].draws[i + 4]);   // (the records' first touch)
#endif
            frameCopies += buckets[b].draws[i].copy_frame ? 1 : 0;
        }
    te::gfx::CopiesBeginFrame(g.dev, frameCopies, screen_w, screen_h);
#if defined(__vita__)
    ps2e_draw_expect_frame_copies(g.dev, frameCopies);
    // the fill census (gl_fill_census.h: TE_GFX_VITA_FILL=N): one frame in N, every draw its GPU visibility counter and its
    // true screen box (handed over below)
    const bool fill = ps2r::render::FillCensusFrame(screen_w, screen_h);
    SkyMove sky;   // the sky after the opaque scene (see SkyMove)
#endif
    {
        TE_PROF_SCOPE("rt begin frame");
        BeginFrameTarget(screen_w, screen_h);
    }
    uint64_t submitted = 0;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) submitted += buckets[b].used;
    if (submitted && count_as_picture) g.presents_with_draws++;
    g.frame_draws = submitted;
    g.draws_submitted += submitted;
    uint64_t triangles = 0;
    // TE_GFX_DIAG_SUBMITS=N logs every submit of picture N (the frame dumped as frame_N) across all buckets: the state, the
    // vertex count, the screen box, the mean colour (a darkener is found by its box); =N,K stops the frame after K submits
    static long diagPresent = -1, diagStop = -1;
    static bool diagRead = false;
    if (!diagRead) {
        diagRead = true;
        if (const char* p = std::getenv("TE_GFX_DIAG_SUBMITS")) {
            diagPresent = std::atol(p);
            if (const char* c = std::strchr(p, ',')) diagStop = std::atol(c + 1);
        }
    }
    const bool diag = count_as_picture && diagPresent >= 0 && g.pictures.load() + 1 == uint64_t(diagPresent);
    long submitIndex = 0;
#if defined(__vita__)
    // runs of one mesh range under one state handed over whole (ps2e_draw_submit_mesh_instances): when the device
    // draws them instanced -- not on the fill census's sampled frames (each draw its own box and counter) nor the diagnosis
    const bool instancing = !fill && !diag && ps2e_draw_mesh_instancing(g.dev) != 0;
    // the batching census on its sampled pictures and the instancing plan (graphics_batch.cpp: TE_GFX_VITA_INSTANCE
    // =1 drawn instanced, =2 the planned order drawn one draw at a time) -- never on the fill census's frames nor the diagnosis
    const int planMode = (!fill && !diag) ? te::gfx::InstancePlanMode() : 0;
    const bool planned = planMode == 2 || (planMode == 1 && instancing);
    const bool batchCensus = te::gfx::BatchCensusFrame(count_as_picture) && !diag;
    if (planned) te::gfx::NotePlannedFrame();
    if (te::gfx::InstancePlanSetting() == 3) {
        // TE_GFX_VITA_INSTANCE=abN: the per-present GPU A/B (vita_gpu_census: every scene this frame ends carries its arm)
        ps2r::render::GlCensus& c = ps2r::render::Census();
        c.gpu_ab_arm = te::gfx::InstancePlanMode() != 0 ? 1 : 0;
        c.gpu_ab_name = "the instancing plan";
        c.gpu_ab_arms[0] = "the default drawing";
        c.gpu_ab_arms[1] = te::gfx::InstancePlanArmName();
    }
#endif
    static long xLogFrom = -2, xLogTo = -1;   // TE_GFX_DRAWLOG=A,B lists every draw of pictures A..B (diagnosis)
    if (xLogFrom == -2) {
        xLogFrom = -1;
        if (const char* v = std::getenv("TE_GFX_DRAWLOG")) { xLogFrom = std::atol(v); const char* c = std::strchr(v, ','); xLogTo = c ? std::atol(c + 1) : xLogFrom; }
    }
    const long xPic = long(g.pictures.load()) + 1;
    const bool xLog = count_as_picture && xLogFrom >= 0 && xPic >= xLogFrom && xPic <= xLogTo;
    // TE_GFX_DRAWLOG2=A,B[,S]: every draw of pictures A..B (every S-th picture) with its whole state's hash, the fields a
    // batching decision reads, and its screen box (a mesh's: its bounds through its matrix) -- the draw census's input
    static long x2From = -2, x2To = -1, x2Step = 1;
    if (x2From == -2) {
        x2From = -1;
        if (const char* v = std::getenv("TE_GFX_DRAWLOG2")) {
            x2From = std::atol(v);
            const char* c = std::strchr(v, ',');
            x2To = c ? std::atol(c + 1) : x2From;
            const char* c2 = c ? std::strchr(c + 1, ',') : nullptr;
            x2Step = c2 ? std::max(1L, std::atol(c2 + 1)) : 1;
        }
    }
    const bool x2Log = count_as_picture && x2From >= 0 && xPic >= x2From && xPic <= x2To && (xPic - x2From) % x2Step == 0;
    static const unsigned short profExpand = te::prof::Register("rt expand (copy built vertices)"),
                                profMesh = te::prof::Register("rt submit native mesh"), profVerts = te::prof::Register("rt submit vertices"),
                                profCopy = te::prof::Register("rt frame copy"),
                                profShadow = te::prof::Register("rt shadow clear / volumes / composite (+ the Vita's casters re-stamp)");
    const int profSlot = te::prof::ThreadSlot();   // (once: a thread_local read is the Vita's emulated TLS, per scope below)
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
        {
            te::prof::Scope scope(profExpand, profSlot);
            ExpandBucket(buckets, b);   // its builds, when the picture came with them still running
        }
        // the bucket's draws in drawing order: the Vita's order of pointers (ExpandBucket: the records stay in place), the
        // desktop's expanded records themselves
#if defined(__vita__)
        Draw* const* const list = buckets[b].order.data();
        const size_t count = buckets[b].order.size();
        // the GPU's draw order (graphics_zorder.cpp): the punch-through draws of each order-free run after the others,
        // near to far -- the same picture, fewer shaded fragments on the device's deferred renderer
        if (!diag) {
            static const unsigned short profOrder = te::prof::Register("rt draw order (punch-through after opaque)");
            te::prof::Scope scope(profOrder, profSlot);
            te::gfx::ZOrderDraws(g.dev, buckets[b].order.data(), count, g.presents.load(std::memory_order_relaxed));
        }
#else
        thread_local std::vector<Draw*> desk;
        desk.resize(buckets[b].used);
        for (size_t i = 0; i < buckets[b].used; ++i) desk[i] = &buckets[b].draws[i];
        Draw* const* const list = desk.data();
        const size_t count = desk.size();
#endif
#if defined(__vita__)
        if (drawHash && count_as_picture)
            for (size_t i = 0; i < count; ++i) hashDraw(b, *list[i]);
#endif
        if (xLog) {
            for (size_t i = 0; i < count; ++i) {
                const Draw& d = *list[i];
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (size_t k = 0; k < d.VertexCount(); ++k) {
                    const auto& v = d.Vertices()[k];
                    x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
                }
                std::fprintf(stderr, "XD %ld %d %u t%u bl%d(%d%d%d%d) zt%d zw%d at%d m%u v%u box %.0f %.0f %.0f %.0f\n", xPic, b, unsigned(i), d.state.texture, d.state.blend,
                             d.state.blend_a, d.state.blend_b, d.state.blend_c, d.state.blend_d, d.state.depth_test, d.state.depth_write, d.state.alpha_test,
                             d.mesh, unsigned(d.VertexCount()), x0, y0, x1, y1);
            }
        }
        if (x2Log) {
            for (size_t i = 0; i < count; ++i) {
                const Draw& d = *list[i];
                const ps2e_draw_state& s = d.state;
                const int fields[] = {int(s.texture), s.gouraud, s.depth_test, s.depth_write, s.blend, s.blend_a, s.blend_b, s.blend_c,
                                      s.blend_d, s.blend_fix, s.alpha_test, s.alpha_method, s.alpha_reference, s.alpha_fail, s.linear,
                                      s.wrap_u, s.wrap_v, s.tfx, s.tcc, s.scissor_x0, s.scissor_y0, s.scissor_x1, s.scissor_y1,
                                      s.no_colour_write, s.stencil, s.fba, s.dest_alpha_test, s.mip_max_level, s.mip_k, s.mip_l,
                                      s.mip_minify};
                uint32_t h = 2166136261u;
                for (int f : fields) h = (h ^ uint32_t(f)) * 16777619u;
                float bx[4] = {-1e30f, -1e30f, 1e30f, 1e30f};
                if (d.mesh) {
                    if (!ps2e_draw_mesh_screen_box(g.dev, d.mesh, &d.mesh_params, bx)) { bx[0] = bx[1] = -1e30f; bx[2] = bx[3] = 1e30f; }
                } else if (d.VertexCount()) {
                    bx[0] = bx[1] = 1e30f; bx[2] = bx[3] = -1e30f;
                    for (size_t k = 0; k < d.VertexCount(); ++k) {
                        const auto& v = d.Vertices()[k];
                        bx[0] = std::min(bx[0], v.x); bx[1] = std::min(bx[1], v.y); bx[2] = std::max(bx[2], v.x); bx[3] = std::max(bx[3], v.y);
                    }
                    bx[0] -= 1.f; bx[1] -= 1.f; bx[2] += 1.f; bx[3] += 1.f;
                }
                const char kind = d.copy_frame ? 'C' : d.mesh ? 'M' : s.stencil != PS2E_DRAW_STENCIL_OFF ? 'S' : 'R';
                // 1 pic, 2 bucket, 3 index, 4 kind, 5 mesh, 6 vertices, 7 lit, 8 state hash, 9 texture, 10 blend.abcd.fix,
                // 11 ztest, 12 zwrite, 13 atest.method.ref.fail, 14 tfx, 15 tcc, 16 fba, 17 no colour write, 18 stencil,
                // 19 scissor set, 20 linear, 21 wrap u, 22 wrap v, 23 gouraud, 24 mip levels, 25-28 box
                std::fprintf(stderr, "XD2 %ld %d %u %c %u %u %d %08x %u %d.%d%d%d%d.%d %d %d %d.%d.%d.%d %d %d %d %d %d %d %d %d %d %d %d %g %g %g %g\n",
                             xPic, b, unsigned(i), kind, d.mesh, unsigned(d.VertexCount()), d.mesh ? d.mesh_params.lit : -1, h, s.texture,
                             s.blend, s.blend_a, s.blend_b, s.blend_c, s.blend_d, s.blend_fix, s.depth_test, s.depth_write, s.alpha_test,
                             s.alpha_method, s.alpha_reference, s.alpha_fail, s.tfx, s.tcc, s.fba, s.no_colour_write, s.stencil,
                             (s.scissor_x0 | s.scissor_y0 | s.scissor_x1 | s.scissor_y1) ? 1 : 0, s.linear, s.wrap_u, s.wrap_v,
                             s.gouraud, s.mip_max_level, double(bx[0]), double(bx[1]), double(bx[2]), double(bx[3]));
            }
        }
#if defined(__vita__)
        if (fill) ps2r::render::FillCensusGroup(b);   // (the census folds per frame bucket too)
        if (b == TE_GFX_BUCKET_SKY && !diag && SkyLastOn() && count) {
            // the sky's layers, then the depth reset: the layers wait for the opaque scene (SkyMove). (The bucket's draws in
            // drawing order: `at(k)` = the k-th entry of the bucket's order of pointers -- ExpandBucket's segment records
            // included, after ZOrderDraws -- the same list the loop below draws.)
            const size_t n = count;
            auto at = [&](size_t k) -> const Draw& { return *list[k]; };
            bool layers = n >= 2 && n - 1 <= SkyMove::kMaxLayers && SkyResetQuad(at(n - 1), screen_w, screen_h);
            for (size_t k = 0; layers && k + 1 < n; ++k) layers = SkyLayer(at(k));
            g_sky.frames++;
            if (layers) {
                sky.pending = true;
                sky.count = n - 1;
                for (size_t k = 0; k < sky.count; ++k) sky.layers[k] = &at(k);
            } else {
                g_sky.no_pattern++;
            }
        }
        // the bucket's batching census / instancing plan (graphics_batch.cpp), over the order drawn below; the sky's
        // insertion point (the first draw it stops at while it waits) is a barrier no draw moves over
        const te::gfx::BatchPlan* plan = nullptr;
        size_t skyStopAt = ~size_t(0);   // (planned: the pre-scanned stop, and why -- the loop asks SkyStopOf no second time)
        int skyStopWhy = -1;
        if ((planned || batchCensus) && count && !(b == TE_GFX_BUCKET_SKY && sky.pending)) {
            size_t stop = count;
            if (sky.pending)
                for (size_t k = 0; k < count; ++k) {
                    const int why = SkyStopOf(*list[k]);
                    if (why >= 0) {
                        stop = k;
                        skyStopWhy = why;
                        break;
                    }
                }
            plan = te::gfx::PlanBucket(g.dev, list, count, stop, batchCensus, planned, xPic, b);
            if (plan && sky.pending) skyStopAt = stop;
        }
#endif
        for (size_t i = 0; i < count; ++i) {
            const Draw& d = *list[i];
#if defined(__vita__)
            if (plan && plan->absorbed[i]) {   // drawn with its group, at the group's first draw
                submitIndex++;
                ++g_draw_timing.tick;
                continue;
            }
            if (i + 1 < count) PrefetchDrawHot(list[i + 1]);   // (the next record's lines, written on another core)
            if (sky.pending) {
                if (b == TE_GFX_BUCKET_SKY) {
                    if (i < sky.count) continue;   // a sky layer: drawn by FlushSky (the reset, bucket 0's last, now)
                } else {
                    const int stop = plan ? (i == skyStopAt ? skyStopWhy : -1) : SkyStopOf(d);
                    if (stop >= 0) FlushSky(sky, stop, screen_w, screen_h, fill, b, triangles);
                    else sky.over++;
                }
            }
#endif
            if (diag) {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e38f, z1 = -1e38f;
                double r = 0, gg = 0, bb = 0, a = 0;
                for (size_t k = 0; k < d.VertexCount(); ++k) {
                    const auto& v = d.Vertices()[k];
                    x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
                    z0 = std::min(z0, v.z); z1 = std::max(z1, v.z);
                    r += v.r; gg += v.g; bb += v.b; a += v.a;
                }
                const double n = double(std::max<size_t>(1, d.VertexCount()));
                const auto& st = d.state;
                std::fprintf(stderr, "[te.gfx.submit] present %ld #%ld bucket %d verts %lu tex %u gouraud %d ztest %d zwrite %d blend %d(%d %d %d %d fix %d) atest %d(m%d ref %d fail %d) tfx %d tcc %d nocol %d stencil %d scissor (%d %d)-(%d %d) box (%.0f %.0f)-(%.0f %.0f) z %.3g..%.3g rgba (%.0f %.0f %.0f %.0f)\n",
                             diagPresent, submitIndex, b, (unsigned long)d.VertexCount(), st.texture, st.gouraud, st.depth_test, st.depth_write, st.blend, st.blend_a, st.blend_b, st.blend_c, st.blend_d, st.blend_fix,
                             st.alpha_test, st.alpha_method, st.alpha_reference, st.alpha_fail, st.tfx, st.tcc, st.no_colour_write,
                             st.stencil,
                             st.scissor_x0, st.scissor_y0, st.scissor_x1, st.scissor_y1, x0, y0, x1, y1, z0, z1, r / n, gg / n, bb / n, a / n);
            }
            submitIndex++;
            if (diag && diagStop >= 0 && submitIndex > diagStop) continue;
            float copyRegion[4];
            if (d.copy_frame && te::gfx::CopyWanted(list, count, i, copyRegion)) {
                te::prof::Scope scope(profCopy, profSlot);
                // the screen copy this draw samples (unless the frame's last copy holds its region): the whole frame, or the
                // box of the draws since the last copy (graphics_copies.cpp)
                ps2e_draw_copy_frame_region(g.dev, copyRegion[0] <= copyRegion[2] ? copyRegion : nullptr);
            }
            const bool timed = g.draw_timing && (++g_draw_timing.tick & g.draw_timing_mask) == 0;
#if defined(__vita__)
            if (fill && d.state.stencil == PS2E_DRAW_STENCIL_OFF) FillCensusBoxOf(d, screen_w, screen_h);
#endif
            if (d.mesh) {
                te::prof::Scope scope(profMesh, profSlot);
                const auto& c = ps2r::render::Census();
                const uint64_t f0 = c.mesh_full, a0 = c.mesh_again, x0 = c.mesh_texture, p0 = c.mesh_packet;
                const uint64_t t0 = timed ? DrawClockUs() : 0;
                if (d.skin_joints) {
                    // a GPU-skinned part (its palette lives in the draw until the picture is presented)
                    if (ps2e_draw_submit_skin(g.dev, &d.state, d.mesh, &d.mesh_params, d.palette.data(), int(d.skin_joints),
                                              d.skin_disp ? d.palette.data() + size_t(d.skin_joints) * 12 : nullptr,
                                              int(d.skin_disp)) < 0)
                        g.dropped_draws++;
                    if (timed) {
                        g_draw_timing.us[6] += DrawClockUs() - t0;
                        g_draw_timing.n[6]++;
                    }
                    continue;
                }
#if defined(__vita__)
                if (plan && plan->next[i] >= 0) {
                    // the plan's group (graphics_batch.cpp): its draws -- one tuple: this mesh range, state and shift,
                    // unlit -- in their order, here at its first draw's place; =1 as ps2e_draw_submit_mesh_instances (instanced
                    // once the packet memo holds the state), =2 one draw at a time the plain way
                    static std::vector<const ps2e_draw_mesh_params*> grp;   // (the drawing thread's alone)
                    grp.clear();
                    for (int32_t k = int32_t(i); k >= 0; k = plan->next[size_t(k)]) grp.push_back(&list[size_t(k)]->mesh_params);
                    if (sky.pending && b != TE_GFX_BUCKET_SKY) sky.over += grp.size() - 1;   // (its later draws: waited for)
                    if (planMode == 1) {
                        g.dropped_draws += uint64_t(ps2e_draw_submit_mesh_instances(g.dev, &d.state, d.mesh, grp.data(), int(grp.size())));
                    } else {
                        for (const ps2e_draw_mesh_params* p : grp)
                            if (ps2e_draw_submit_mesh(g.dev, &d.state, d.mesh, p) < 0) g.dropped_draws++;
                    }
                    g.mesh_runs++;
                    g.mesh_run_draws += grp.size();
                    if (timed) {
                        g_draw_timing.us[8] += DrawClockUs() - t0;
                        g_draw_timing.n[8]++;
                    }
                    continue;
                }
                // A RUN of draws of one mesh range under one state, unlit, one right after the other in the drawing
                // order (the packet census's "the previous draw's packet and mesh again": what an instanced draw takes without
                // any reordering) -- handed over as one ps2e_draw_submit_mesh_instances: the same draws in the same order, which
                // the device draws as one instanced draw once they are its packet's. The sky's wait sees each of the run's
                // draws as this loop would (a draw it stops at ends the run before it: its flush comes first).
                if (!plan && instancing && te::gfx::InstancePlanSetting() == 0 && !d.copy_frame && !d.mesh_params.lit &&
                    d.state.stencil == PS2E_DRAW_STENCIL_OFF) {
                    size_t j = i + 1;
                    while (j < count && j - i < kMaxMeshRun) {
                        const Draw& e = *list[j];
                        if (e.mesh != d.mesh || e.skin_joints || e.copy_frame || e.mesh_params.lit ||
                            e.mesh_params.first != d.mesh_params.first || e.mesh_params.count != d.mesh_params.count ||
                            std::memcmp(e.mesh_params.offset, d.mesh_params.offset, sizeof d.mesh_params.offset) != 0 ||
                            std::memcmp(e.mesh_params.scroll, d.mesh_params.scroll, sizeof d.mesh_params.scroll) != 0 ||
                            !SameDrawState(e.state, d.state))
                            break;
                        if (sky.pending && b != TE_GFX_BUCKET_SKY && SkyStopOf(e) >= 0) break;
                        ++j;
                    }
                    if (j - i >= 2) {
                        static std::vector<const ps2e_draw_mesh_params*> run;   // (the drawing thread's alone)
                        run.clear();
                        for (size_t k = i; k < j; ++k) run.push_back(&list[k]->mesh_params);
                        if (sky.pending && b != TE_GFX_BUCKET_SKY) sky.over += j - i - 1;   // (the run's later draws: waited for)
                        g.dropped_draws += uint64_t(ps2e_draw_submit_mesh_instances(g.dev, &d.state, d.mesh, run.data(), int(j - i)));
                        g.mesh_runs++;
                        g.mesh_run_draws += j - i;
                        g_draw_timing.tick += uint32_t(j - i - 1);   // (a tick per draw, as the loop's own)
                        submitIndex += long(j - i - 1);
                        if (timed) {
                            g_draw_timing.us[8] += DrawClockUs() - t0;
                            g_draw_timing.n[8]++;
                        }
                        i = j - 1;
                        continue;
                    }
                }
#endif
                if (ps2e_draw_submit_mesh(g.dev, &d.state, d.mesh, &d.mesh_params) < 0) g.dropped_draws++;
                if (timed) {
                    const int k = c.mesh_full != f0 ? 0 : c.mesh_again != a0 ? 1 : c.mesh_texture != x0 ? 2 : c.mesh_packet != p0 ? 7 : -1;
                    if (k >= 0) {
                        g_draw_timing.us[k] += DrawClockUs() - t0;
                        g_draw_timing.n[k]++;
                    }
                }
                continue;
            }
            te::prof::Scope scope(d.state.stencil != PS2E_DRAW_STENCIL_OFF ? profShadow : profVerts, profSlot);
            if (g.group_volumes && !diag && CountingDraw(d)) {
                // A run of shadow-volume counting draws (between the counter's clear and its composite: the volumes of a
                // bracket, their count-up and count-down halves alternating per volume): drawn as one draw per state
                // (count up, count down, each with its caster test or not) in the order the states first appear. Exact:
                // a counting draw writes no colour and no depth, only the stencil's low 5 bits (+1 / -1 wrapping, where
                // its depth test fails), and its stencil test reads only the caster bit above them -- each fragment's
                // effect is the same whatever was counted before it, and the increments commute; the run's draws are
                // all such draws, so their order within the run is free. 40 draws a hub frame become 2.
                size_t j = i;
                while (j < count && CountingDraw(*list[j])) ++j;
                if (j - i >= 2) {
                    const uint64_t t0 = timed ? DrawClockUs() : 0;
                    SubmitVolumeRun(list + i, j - i, triangles);
                    if (timed) {
                        g_draw_timing.us[5] += DrawClockUs() - t0;
                        g_draw_timing.n[5]++;
                    }
                    submitIndex += long(j - i - 1);
                    i = j - 1;
                    continue;
                }
            }
            const uint64_t t0 = timed ? DrawClockUs() : 0;
            if (ps2e_draw_submit(g.dev, &d.state, d.Vertices(), int(d.VertexCount())) < 0) g.dropped_draws++;
            if (timed) {
                const int k = d.state.stencil != PS2E_DRAW_STENCIL_OFF ? 4 : 3;
                g_draw_timing.us[k] += DrawClockUs() - t0;
                g_draw_timing.n[k]++;
            }
            triangles += d.VertexCount() / 3;
        }
        te::gfx::CopiesBucketEnd(list, count);   // (the bucket's draws since its last copy draw: the coalescer's notes)
    }
#if defined(__vita__)
    if (sky.pending) FlushSky(sky, kStopEnd, screen_w, screen_h, fill, TE_GFX_BUCKET_COUNT - 1, triangles);
    if (drawHash && count_as_picture)
        std::fprintf(stderr, "[te.drawhash] picture %llu: %016llx (%llu draws)\n", (unsigned long long)(g.pictures.load() + 1),
                     (unsigned long long)pictureHash, (unsigned long long)pictureDraws);
#endif
    g.frame_triangles = triangles;
    return submitted;
}

// The pace over the presents since the last summary: the rate they were shown at, the longest gap between two of them
// and how many gaps were longer than a 60 Hz field (the running average from the first present hides a slow stretch)
struct Pace {
    Steady::time_point start, last;
    uint64_t frames = 0, slow = 0;
    double longest_ms = 0, submit_ms = 0, present_ms = 0;
    int64_t waited_ns = 0, built_ns = 0;   // the build pool's counters at the window's start
};
Pace g_pace;

void NotePace(uint64_t presents) {
    // (the window's census below runs on the thread that presented -- the drawing thread in play mode: its cost is
    // measured and logged with it, so a device log shows what the diagnostics themselves take from the frame)
    [[maybe_unused]] const bool census_window = g.summary_every > 0 && presents % uint64_t(g.summary_every) == 0 && g_pace.frames >= 1;
    [[maybe_unused]] const auto census_t0 = census_window ? Steady::now() : Steady::time_point{};
    te::prof::OnPresent(presents);
    te::prof::CpuCensus(presents);   // the Vita: each thread's CPU / priority / preemptions per window (te_prof.h)
    const auto now = Steady::now();
    BuildPool& pool = Builds();
    if (g_pace.frames == 0) {
        g_pace.start = g_pace.last = now;
        g_pace.waited_ns = pool.waited_ns.load(std::memory_order_relaxed);
        g_pace.built_ns = pool.built_ns.load(std::memory_order_relaxed);
    } else {
        const double gap = std::chrono::duration<double, std::milli>(now - g_pace.last).count();
        g_pace.longest_ms = std::max(g_pace.longest_ms, gap);
        if (gap > 1000.0 / 60.0 + 1.0) ++g_pace.slow;
        g_pace.last = now;
        g_pace.submit_ms += g.last_submit_ms;
        g_pace.present_ms += g.last_present_ms;
    }
    ++g_pace.frames;
    if (g.summary_every > 0 && presents % uint64_t(g.summary_every) == 0 && g_pace.frames > 1) {
        const double s = std::chrono::duration<double>(g_pace.last - g_pace.start).count();
        const double gaps = double(g_pace.frames - 1);
        const double frame_ms = s * 1000.0 / gaps;
        // the frame's time on the drawing thread: the draws' submission, the present (the GPU's finish + the swap); the
        // rest is the game's own (and, synchronous, the window pump and the pacing park). The geometry builds: their own
        // time on every thread together, and the drawing / game thread's time on them (helping or waiting)
        const double built = double(pool.built_ns.load(std::memory_order_relaxed) - g_pace.built_ns) / 1e6 / gaps;
#if defined(__vita__)
        // the newlib heap (the Vita's fixed block): in use and the most it has reached; and what vitaGL's GPU pools have
        // left (CDRAM, the uncached user RAM it took at vglInitExtended -- 15 MB less on the device than in Vita3K -- and
        // phycont): when they run out vitaGL falls back to the newlib heap (cached memory the GPU then reads without a
        // cache write-back), so this line is the first warning. Every TE_GFX_HEAP_EVERY-th window (default 4):
        // mallinfo walks newlib's free lists and vglMemFree each of vitaGL's pools (sceClibMspaceMallocStats) -- on the
        // drawing thread, between two pictures; the line says what it took
        static const long heap_every = std::max(0L, EnvLong("TE_GFX_HEAP_EVERY", 4));
        static uint64_t heap_windows = 0;
        if (heap_every > 0 && heap_windows++ % uint64_t(heap_every) == 0) {
            const auto h0 = Steady::now();
            const struct mallinfo heap = mallinfo();
            const double v0 = vglMemFree(0) / 1048576.0, v1 = vglMemFree(1) / 1048576.0, v2 = vglMemFree(2) / 1048576.0;
            const double heap_ms = std::chrono::duration<double, std::milli>(Steady::now() - h0).count();
            std::fprintf(stderr, "[te.gfx] heap: %.1f MB in use, %.1f MB reached; vitaGL free: vram %.1f MB, ram %.1f MB, phycont %.1f MB "
                         "(this census %.2f ms; every %ld windows: TE_GFX_HEAP_EVERY)\n",
                         heap.uordblks / 1048576.0, heap.arena / 1048576.0, v0, v1, v2, heap_ms, heap_every);
        }
#endif
        const double waited = double(pool.waited_ns.load(std::memory_order_relaxed) - g_pace.waited_ns) / 1e6 / gaps;
        std::fprintf(stderr, "[te.gfx] pace: presents %llu-%llu at %.1f fps, longest gap %.1f ms, %llu of %llu gaps over a field; "
                     "a frame %.2f ms = submit %.2f + present %.2f + the rest %.2f; builds %.2f ms (waited on %.2f)\n",
                     (unsigned long long)(presents - g_pace.frames + 1), (unsigned long long)presents,
                     s > 0 ? gaps / s : 0.0, g_pace.longest_ms, (unsigned long long)g_pace.slow,
                     (unsigned long long)(g_pace.frames - 1), frame_ms, g_pace.submit_ms / gaps, g_pace.present_ms / gaps,
                     std::max(0.0, frame_ms - (g_pace.submit_ms + g_pace.present_ms) / gaps), built, waited);
        {
            // the screen copies: asked, taken (and why), coalesced (graphics_copies.cpp; only a window with copy draws)
            const std::string copies = te::gfx::CopyCensusLine(presents - g_pace.frames + 1, presents);
            if (!copies.empty()) std::fputs(copies.c_str(), stderr);
        }
        {
            // the load census (te_load_census.h): the window's long ticks and what they did, the textures / model parts /
            // native meshes made and the waits that came with them (the Vita always; elsewhere TE_LOAD_CENSUS=1)
#if defined(__vita__)
            static const bool load_census = !(std::getenv("TE_LOAD_CENSUS") && std::getenv("TE_LOAD_CENSUS")[0] == '0');
#else
            static const bool load_census = EnvFlag("TE_LOAD_CENSUS");
#endif
            if (load_census) {
                const std::string line = te::load::WindowLine(presents - g_pace.frames + 1, presents);
                if (!line.empty()) std::fputs(line.c_str(), stderr);
                const std::string eager = EagerCensusLine();
                if (!eager.empty()) std::fputs(eager.c_str(), stderr);
                const std::string prep = te::gfx::PrepCensusLine();
                if (!prep.empty()) std::fputs(prep.c_str(), stderr);
            }
        }
#if defined(__vita__)
        {
            // the hand-over: the game's pictures against the drawing thread's presents (the low-latency mode drops the ones
            // replaced before they were taken), the clock's boundaries (the game's tick rate), and the waits
            static uint64_t last_handed = 0, last_dropped = 0, last_bounds = 0, last_gpu_us = 0, last_build_us = 0, last_waits = 0,
                            last_timeouts = 0;
            const uint64_t timeouts = g.ll_gpu_timeouts.load(std::memory_order_relaxed);
            const uint64_t handed = g.ll_handed.load(std::memory_order_relaxed), dropped = g.ll_dropped.load(std::memory_order_relaxed);
            const uint64_t bounds = g.ll_boundaries.load(std::memory_order_relaxed), gpu_us = g.ll_gpu_wait_us.load(std::memory_order_relaxed);
            const uint64_t build_us = g.ll_build_wait_us.load(std::memory_order_relaxed), waits = g.rt.waits;
            const double win = s > 0 ? s : 1.0;
            const uint64_t made = handed - last_handed, drop = dropped - last_dropped;
            std::fprintf(stderr, "[te.gfx.vita] hand-over (low-latency %s): pictures made %llu (%.1f a second), presented %llu, "
                         "replaced before drawn %llu (%.0f%%); game boundaries %.1f a second; the drawing thread waited %.2f ms "
                         "per present for the GPU (at most %d frame%s in flight; %llu waits given up at 100 ms) before taking a picture; the "
                         "game thread waited %.2f ms per picture for replaced pictures' builds and %llu times for the drawing thread\n",
                         LowLatency() ? "on" : "off", (unsigned long long)made, double(made) / win,
                         (unsigned long long)g_pace.frames, (unsigned long long)drop, made ? 100.0 * double(drop) / double(made) : 0.0,
                         double(bounds - last_bounds) / win, double(gpu_us - last_gpu_us) / 1000.0 / double(g_pace.frames),
                         g.ll_ahead, g.ll_ahead == 1 ? "" : "s", (unsigned long long)(timeouts - last_timeouts),
                         made ? double(build_us - last_build_us) / 1000.0 / double(made) : 0.0,
                         (unsigned long long)(waits - last_waits));
            last_handed = handed;
            last_dropped = dropped;
            last_bounds = bounds;
            last_gpu_us = gpu_us;
            last_build_us = build_us;
            last_waits = waits;
            last_timeouts = timeouts;
        }
        {
            // the tick lock (te_tick_lock.h): the cadence the game and the display kept in this window
            const std::string lock_line = te::tick_lock::WindowLine(presents - g_pace.frames + 1, presents, s);
            if (!lock_line.empty()) std::fputs(lock_line.c_str(), stderr);
        }
        {
            // what the window's frames cost in GL calls (render/backends/gl/gl_census.h), per frame
            static ps2r::render::GlCensus last{};
            static uint64_t last_merged = 0;
            static uint64_t last_vin = 0, last_vout = 0;
            const ps2r::render::GlCensus now_c = ps2r::render::Census();
            const double f = double(g_pace.frames);
            auto per = [&](uint64_t a, uint64_t b) { return double(a - b) / f; };
            std::fprintf(stderr, "[te.gfx] gl per frame: draws converted %.1f raw %.1f mesh %.1f + again %.1f + texture %.1f, glDrawArrays %.1f, "
                         "state calls %.1f, uniform calls %.1f (glUniform sent %.1f), te_size uniforms %.1f (skipped %.1f); recorded draws joined %.1f\n",
                         per(now_c.draws_converted, last.draws_converted), per(now_c.draws_raw, last.draws_raw),
                         per(now_c.mesh_full, last.mesh_full), per(now_c.mesh_again, last.mesh_again),
                         per(now_c.mesh_texture, last.mesh_texture), per(now_c.gl_draws, last.gl_draws),
                         per(now_c.state_calls, last.state_calls), per(now_c.uniform_calls, last.uniform_calls),
                         per(now_c.uniform_sends, last.uniform_sends),
                         per(now_c.size_uniforms, last.size_uniforms), per(now_c.size_skipped, last.size_skipped),
                         per(g.draws_merged, last_merged));
            static uint64_t last_vverts = 0, last_seg = 0, last_tails = 0;
            std::fprintf(stderr, "[te.gfx] draws per frame by the drawing thread's batching: shadow-volume counting draws %.1f drawn as %.1f "
                         "(%.0f vertices); state calls by path: mesh full %.1f, again %.1f, texture %.1f, raw %.1f; the draw order: "
                         "%.1f segment draws expanded, %.1f recorded draws after a bucket's first placeholder (left in place)\n",
                         per(g.volume_draws_in, last_vin), per(g.volume_draws_out, last_vout), per(g.volume_vertices, last_vverts),
                         per(now_c.state_by_path[0], last.state_by_path[0]), per(now_c.state_by_path[1], last.state_by_path[1]),
                         per(now_c.state_by_path[2], last.state_by_path[2]), per(now_c.state_by_path[3], last.state_by_path[3]),
                         per(g.expanded_segments, last_seg), per(g.expanded_tails, last_tails));
            last_vin = g.volume_draws_in;
            last_vout = g.volume_draws_out;
            last_vverts = g.volume_vertices;
            last_seg = g.expanded_segments;
            last_tails = g.expanded_tails;
            {
                // the drawing thread's CPU per draw by path (sampled; the window's means)
                static DrawTiming last_t;
                auto mean = [&](int k) {
                    const uint64_t n = g_draw_timing.n[k] - last_t.n[k];
                    return n ? double(g_draw_timing.us[k] - last_t.us[k]) / double(n) : 0.0;
                };
                std::fprintf(stderr, "[te.gfx.vita] drawing thread CPU per draw (1 in %u timed): mesh full %.1f us, again %.1f, "
                             "texture swap %.1f, packet %.1f, vertices %.1f, shadow %.1f, a grouped volume run %.1f, GPU skin %.1f, "
                             "a run of one mesh range %.1f (submit + backend + vitaGL)\n", unsigned(g.draw_timing_mask + 1), mean(0),
                             mean(1), mean(2), mean(7), mean(3), mean(4), mean(5), mean(6), mean(8));
                last_t = g_draw_timing;
            }
            {
                // the mesh draw packets (ps2e_draw.h, gl_shader_library.cpp RawReplay): drawn, recorded, and why an again /
                // texture-swap draw was not drawn from one
                static uint64_t last_missing = 0, last_repeats = 0;
                uint64_t missing = 0, repeats = 0;
                if (g.dev) ps2e_draw_mesh_packet_census(g.dev, &missing, &repeats);
                std::fprintf(stderr, "[te.gfx.vita] mesh draw packets per frame: drawn %.1f (of them the previous draw's packet and "
                             "mesh again %.1f) (across vertex draws %.1f), recorded %.1f; not drawn from one: no packet kept %.1f, off "
                             "%.1f, the run's context moved %.1f, stale %.1f, the texture record changed %.1f, blend / target format "
                             "%.1f, screen size %.1f, other %.1f; this window's arms: packets on in %llu of %llu presents, the native "
                             "meshes indexed in %llu\n",
                             per(now_c.mesh_packet, last.mesh_packet), per(repeats, last_repeats),
                             per(now_c.packet_across, last.packet_across), per(now_c.packets_recorded, last.packets_recorded),
                             per(missing, last_missing), per(now_c.packet_declined[0], last.packet_declined[0]),
                             per(now_c.packet_declined[2], last.packet_declined[2]), per(now_c.packet_declined[3], last.packet_declined[3]),
                             per(now_c.packet_declined[4], last.packet_declined[4]), per(now_c.packet_declined[5], last.packet_declined[5]),
                             per(now_c.packet_declined[6], last.packet_declined[6]), per(now_c.packet_declined[7], last.packet_declined[7]),
                             (unsigned long long)(now_c.presents_packets_on - last.presents_packets_on),
                             (unsigned long long)(now_c.presents_counted - last.presents_counted),
                             (unsigned long long)(now_c.presents_meshes_indexed - last.presents_meshes_indexed));
                last_missing = missing;
                last_repeats = repeats;
                // the mesh draws that took the full path, by why (ps2e_draw_mesh_full_census), and the casters' re-stamps
                static uint64_t last_reasons[5] = {0, 0, 0, 0, 0}, last_casters = 0;
                uint64_t reasons[5] = {0, 0, 0, 0, 0}, casters = 0;
                if (g.dev) ps2e_draw_mesh_full_census(g.dev, reasons, &casters);
                std::fprintf(stderr, "[te.gfx.vita] full mesh draws per frame by why: no run %.1f, another draw since the run's last %.1f, "
                             "another context %.1f, a swap the short path does not take %.1f, declined by the device %.1f; casters "
                             "re-stamped at the shadow composites %.1f\n",
                             per(reasons[0], last_reasons[0]), per(reasons[1], last_reasons[1]), per(reasons[2], last_reasons[2]),
                             per(reasons[3], last_reasons[3]), per(reasons[4], last_reasons[4]), per(casters, last_casters));
                for (int k = 0; k < 5; ++k) last_reasons[k] = reasons[k];
                last_casters = casters;
                // the drawing thread's per-draw savings: the raw draws' GXM sets skipped, the lookups past their
                // direct-mapped slots, the packet memo, the instanced packet draws
                static uint64_t last_lk[5] = {0, 0, 0, 0, 0}, last_runs = 0, last_run_draws = 0;
                uint64_t lk[5] = {0, 0, 0, 0, 0};
                if (g.dev) ps2e_draw_lookup_census(g.dev, lk);
                std::fprintf(stderr, "[te.gfx.vita] raw draws per frame: GXM sets skipped (the context held them): textures "
                             "%.1f of %.1f, streams %.1f of %.1f; lookups past their slots: meshes %.1f of %.1f, mesh bounds %.1f of "
                             "%.1f, textures %.1f of %.1f; mesh draws from the packet memo %.1f; runs of one mesh range handed over "
                             "whole %.1f (%.1f draws); instanced packet draws %.1f (drawing %.1f mesh draws; declined %.1f)\n",
                             per(now_c.raw_texture_skipped, last.raw_texture_skipped), per(now_c.raw_texture_sets, last.raw_texture_sets),
                             per(now_c.raw_stream_skipped, last.raw_stream_skipped), per(now_c.raw_stream_sets, last.raw_stream_sets),
                             per(now_c.mesh_lookup_misses, last.mesh_lookup_misses), per(now_c.mesh_lookups, last.mesh_lookups),
                             per(lk[1], last_lk[1]), per(lk[0], last_lk[0]), per(lk[3], last_lk[3]), per(lk[2], last_lk[2]),
                             per(lk[4], last_lk[4]), per(g.mesh_runs, last_runs), per(g.mesh_run_draws, last_run_draws),
                             per(now_c.instanced_draws, last.instanced_draws), per(now_c.instanced_instances, last.instanced_instances),
                             per(now_c.instanced_declined, last.instanced_declined));
                for (int k = 0; k < 5; ++k) last_lk[k] = lk[k];
                last_runs = g.mesh_runs;
                last_run_draws = g.mesh_run_draws;
                // the full paths' CPU split (gl_draw_executor.cpp SplitTimer; 1 in 16 of each path timed)
                auto sp = [&](int path, int k) {
                    const uint64_t n = now_c.split_n[path] - last.split_n[path];
                    return n ? double(now_c.split_us[path][k] - last.split_us[path][k]) / double(n) : 0.0;
                };
                std::fprintf(stderr, "[te.gfx.vita] full draws' CPU split (1 in 16 timed, us a draw): mesh full: state %.1f, program "
                             "+ raw draw %.1f, GL draw + rest %.1f; vertex draws: state %.1f, upload %.1f, program %.1f, GL draw + rest "
                             "%.1f\n", sp(0, 0), sp(0, 2), sp(0, 3), sp(1, 0), sp(1, 1), sp(1, 2), sp(1, 3));
                {
                    // the vertices' way into the GPU's memory -- copied there from a producer's storage
                    // (the ring copy), drawn where a caller wrote them, the shadow volumes' group concatenations
                    static uint64_t last_copied = 0, last_inplace = 0, last_vin = 0, last_vcp = 0, last_bn = 0, last_bs = 0;
                    const uint64_t copied = ps2r::render::GlVertexStream::CopiedBytes();
                    const uint64_t inplace = ps2r::render::GlVertexStream::InPlaceBytes();
                    std::fprintf(stderr, "[te.gfx.vita] vertex copies per frame: vertex bytes into the GPU's memory: copied from a "
                                 "producer's storage %.1f KB, drawn where they were written %.1f KB; the shadow volumes' groups "
                                 "concatenated straight into the vertex ring %.1f KB, into group buffers (then copied) %.1f KB; the "
                                 "volumes' box / Q passes %.1f with NEON's min / max, %.1f scalar (a NaN, zero or denormal met)\n",
                                 per(copied, last_copied) / 1024.0, per(inplace, last_inplace) / 1024.0,
                                 per(g_r2.volume_inplace_bytes, last_vin) / 1024.0, per(g_r2.volume_copied_bytes, last_vcp) / 1024.0,
                                 per(g_r2.volume_box_neon, last_bn), per(g_r2.volume_box_scalar, last_bs));
                    last_bn = g_r2.volume_box_neon;
                    last_bs = g_r2.volume_box_scalar;
                    last_copied = copied;
                    last_inplace = inplace;
                    last_vin = g_r2.volume_inplace_bytes;
                    last_vcp = g_r2.volume_copied_bytes;
                    // the casters' re-stamp runs (ps2e_draw ResetCasterCounts): the brackets' stencil set once, the re-stamps
                    // by path
                    static uint64_t last_rs[6] = {0, 0, 0, 0, 0, 0};
                    uint64_t rs[6] = {0, 0, 0, 0, 0, 0};
                    ps2e_draw_restamp_census(g.dev, rs);
                    std::fprintf(stderr, "[te.gfx.vita] casters' re-stamp runs per frame (TE_GFX_VITA_RESTAMP_RUN): %.1f brackets (the "
                                 "stencil set once each); mesh casters %.1f full + %.1f from their packets, skin casters %.1f full "
                                 "+ %.1f from their packets, vertex casters %.1f\n", per(rs[0], last_rs[0]), per(rs[1], last_rs[1]),
                                 per(rs[2], last_rs[2]), per(rs[3], last_rs[3]), per(rs[4], last_rs[4]), per(rs[5], last_rs[5]));
                    for (int k = 0; k < 6; ++k) last_rs[k] = rs[k];
                }
            }
            if (now_c.uniform_checked != last.uniform_checked)
                std::fprintf(stderr, "[te.gfx] uniform check (TE_GFX_UNIFORM_CHECK=1): %llu draws checked in the window, %llu uniforms "
                             "found not holding their value (all runs so far: %llu)\n",
                             (unsigned long long)(now_c.uniform_checked - last.uniform_checked),
                             (unsigned long long)(now_c.uniform_mismatch - last.uniform_mismatch),
                             (unsigned long long)now_c.uniform_mismatch);
            // the memory side (render/backends/gl/gl_census.h) and the drawing thread's split of the present
            const double pres = std::max(1.0, double(now_c.presents - last.presents));
            const uint64_t copy_b = now_c.ring_copy_bytes - last.ring_copy_bytes, copy_us = now_c.ring_copy_us - last.ring_copy_us;
            std::fprintf(stderr, "[te.gfx.vita] memory per frame: vertex ring %.1f KB (peak frame %.0f KB, storage re-specified %llu "
                         "x, %.0f KB; the copies into it %.0f MB/s, 1 in 32 timed), texture uploads %.2f (%.1f KB, %llu re-specified "
                         "whole), scanout quad uploads %llu (%llu presents kept it), native meshes alive %llu (+%llu created, %.0f KB)\n",
                         per(now_c.ring_bytes, last.ring_bytes) / 1024.0, now_c.ring_frame_peak / 1024.0,
                         (unsigned long long)(now_c.ring_respecs - last.ring_respecs),
                         (now_c.ring_respec_bytes - last.ring_respec_bytes) / 1024.0,
                         copy_us ? double(copy_b) / double(copy_us) : 0.0, per(now_c.tex_uploads, last.tex_uploads),
                         per(now_c.tex_upload_bytes, last.tex_upload_bytes) / 1024.0,
                         (unsigned long long)(now_c.tex_respecified - last.tex_respecified),
                         (unsigned long long)(now_c.buffer_subdata - last.buffer_subdata),
                         (unsigned long long)(now_c.scanout_same - last.scanout_same), (unsigned long long)now_c.meshes_alive,
                         (unsigned long long)(now_c.meshes_created - last.meshes_created), (now_c.mesh_bytes - last.mesh_bytes) / 1024.0);
            std::fprintf(stderr, "[te.gfx.vita] present per frame (drawing thread): draw part %.2f ms + swap %.2f ms (longest swap %.1f "
                         "ms)\n%s\n", double(now_c.present_draw_us - last.present_draw_us) / 1000.0 / pres,
                         double(now_c.swap_us - last.swap_us) / 1000.0 / pres, now_c.swap_max_us / 1000.0,
                         te::vita_gpu::WindowLines(pres).c_str());
            {
                // the PS2 program variants (the fast program: GlShaderLibrary::Ps2FastProgram) and the A/B's arms
                const uint64_t fast = now_c.draws_ps2_fast - last.draws_ps2_fast, full = now_c.draws_ps2_full - last.draws_ps2_full;
                std::string arms;
                for (int arm = 1; arm >= 0; --arm) {
                    const uint64_t n = now_c.arm_presents[arm] - last.arm_presents[arm];
                    if (!n) continue;
                    char a[200];
                    std::snprintf(a, sizeof a, "; %s %s: %llu presents, %.1f ms present to present, swap %.2f ms",
                                  now_c.ab_name, arm ? "on" : "off", (unsigned long long)n,
                                  double(now_c.arm_interval_us[arm] - last.arm_interval_us[arm]) / 1000.0 / double(n),
                                  double(now_c.arm_swap_us[arm] - last.arm_swap_us[arm]) / 1000.0 / double(n));
                    arms += a;
                }
                std::fprintf(stderr, "[te.gfx.vita] PS2 program per frame: lean %.1f, fast %.1f, full %.1f draws (%.0f%% fast)%s\n",
                             per(now_c.draws_ps2_lean, last.draws_ps2_lean),
                             per(now_c.draws_ps2_fast, last.draws_ps2_fast), per(now_c.draws_ps2_full, last.draws_ps2_full),
                             fast + full ? 100.0 * double(fast) / double(fast + full) : 0.0, arms.c_str());
                // the lean programs' census (gl_shader_library.cpp): per variant, the fallbacks and why
                std::fputs(ps2r::render::Ps2ProgramCensusLines(f).c_str(), stderr);
            }
            {
                // the skins' GPU path: the scene's parts by path (graphics_scene.cpp) and the device's skinned draws
                std::fprintf(stderr, "[te.gfx.vita] GPU skinning per frame: %.1f draws (%.0f triangles), %.1f refused by the device; %s",
                             per(now_c.skin_draws, last.skin_draws), per(now_c.skin_indices, last.skin_indices) / 3.0,
                             per(now_c.skin_refused, last.skin_refused), te::gfx::SkinPathCensusLine(f).c_str());
                std::fputs(ps2r::render::SkinProgramCensusLines(f).c_str(), stderr);
                {
                    // the skins' raw GXM draws and packet replays (gl_shader_library.cpp RawSkinSubmit / SkinReplay)
                    static uint64_t last_skin_missing = 0;
                    uint64_t skin_missing = 0;
                    if (g.dev) ps2e_draw_skin_packet_census(g.dev, &skin_missing);
                    std::fprintf(stderr, "[te.gfx.vita] GPU-skin draws per frame: from packets %.1f (declined %.1f, none kept for the "
                                 "state %.1f), raw %.1f (declined %.1f), through vitaGL the rest\n",
                                 per(now_c.skin_packet, last.skin_packet), per(now_c.skin_packet_declined, last.skin_packet_declined),
                                 per(skin_missing, last_skin_missing), per(now_c.skin_raw, last.skin_raw),
                                 per(now_c.skin_raw_declined, last.skin_raw_declined));
                    last_skin_missing = skin_missing;
                }
                if (now_c.copies != last.copies)
                    std::fprintf(stderr, "[te.gfx.vita] screen copies per frame (the GPU's; TE_GFX_VITA_COPY_RESTORE): %.2f drawn -- the "
                                 "frame's scene continued after %.2f (no scene-start mask update, nothing restored; the copy "
                                 "texture's sampling re-stated as a restore left it after %.2f: TE_GFX_VITA_COPY_SAMPLE), restored "
                                 "after %.2f (the mask update + the restore pass); copy passes %.0f K pixels a frame (%.2f whole)\n",
                                 per(now_c.copies, last.copies), per(now_c.copies_continued, last.copies_continued),
                                 per(now_c.copies_restated, last.copies_restated),
                                 per(now_c.copies_restored, last.copies_restored),
                                 per(now_c.copy_pixels, last.copy_pixels) / 1000.0, per(now_c.copy_whole, last.copy_whole));
                std::fputs(te::gfx::ChunkCensusLine(f).c_str(), stderr);
                std::fputs(te::gfx::ZOrderCensusLine(f).c_str(), stderr);
                std::fputs(te::gfx::BatchCensusLine(f).c_str(), stderr);   // (graphics_batch.cpp)
                std::fputs(SkyCensusLine(f).c_str(), stderr);
                std::fputs(ps2r::render::FillCensusLines().c_str(), stderr);
                if (now_c.mesh_list_vertices)
                    std::fprintf(stderr, "[te.gfx.vita] native meshes made so far: %llu triangle-list vertices, %llu in their vertex "
                                 "buffers (%.1f%%: the distinct ones when TE_GFX_VITA_MESH_INDEX indexes them); made on the drawing "
                                 "thread in %.1f ms so far, %.1f ms of it deduplicating\n",
                                 (unsigned long long)now_c.mesh_list_vertices, (unsigned long long)now_c.mesh_stored_vertices,
                                 100.0 * double(now_c.mesh_stored_vertices) / double(now_c.mesh_list_vertices),
                                 double(now_c.mesh_create_us) / 1000.0, double(now_c.mesh_index_us) / 1000.0);
                if (now_c.skin_mesh_vertices[0] + now_c.skin_mesh_vertices[1] + now_c.skin_mesh_vertices[2])
                    std::fprintf(stderr, "[te.gfx.vita] GPU skin meshes made so far: vertices of meshes weighing at most 1 bone %llu, 2 "
                                 "bones %llu, 3 bones %llu; of meshes with a negative colour lane %llu, of blend parts %llu\n",
                                 (unsigned long long)now_c.skin_mesh_vertices[0], (unsigned long long)now_c.skin_mesh_vertices[1],
                                 (unsigned long long)now_c.skin_mesh_vertices[2],
                                 (unsigned long long)now_c.skin_mesh_negative_vertices,
                                 (unsigned long long)now_c.skin_mesh_blend_vertices);
            }
            ps2r::render::Census().ring_frame_peak = 0;
            ps2r::render::Census().swap_max_us = 0;
            last = now_c;
            last_merged = g.draws_merged;
        }
        if (census_window)
            std::fprintf(stderr, "[te.gfx.vita] this window's census lines took %.2f ms on the %s thread\n",
                         std::chrono::duration<double, std::milli>(Steady::now() - census_t0).count(),
                         OnRenderThread() ? "drawing" : "presenting");
#endif
        g_pace = Pace{};
    }
}

// A shown frame's bookkeeping: the count, the pace, the periodic summary
void NotePresented() {
    const uint64_t presents = ++g.presents;
    te::tick_lock::NotePresents(presents);
    if (!g.have_first_present) {
        g.have_first_present = true;
        g.first_present = Steady::now();
#if defined(__vita__)
        // the log's synced start-up ends here: from now on no thread -- this one, the drawing thread, above all: its pace
        // census every TE_GFX_SUMMARY_EVERY presents is ~40 lines -- waits on the memory card (runner/te_vita.cpp)
        te::vita::LogRunning();
#endif
    }
    NotePace(presents);
    // The runner's --run-seconds watchdog leaves with _exit (no atexit): a periodic summary keeps the counts visible
    if (g.summary_every > 0 && presents % uint64_t(g.summary_every) == 0) Summary();
}

void RenderFrame(Gfx::Frame& f, Show show) {
    const auto t0 = Steady::now();
    ++g.pictures;
    const uint64_t picture = f.picture ? f.picture : g.pictures.load();   // (the synchronous path has no hand-over: the count)
    auto& rt = g.rt;
    if (show == Show::None && !g.harness_keys.empty() && te::harness::Armed() && HarnessWants(f.harness_key)) show = Show::Capture;
    if (show == Show::None) {
        g.frame_draws = 0;
        g.frame_triangles = 0;
        rt.target_slot = -1;   // the frame target does not hold this picture
        if (te::harness::Armed()) te::harness::OnPresentSkipped(f.harness_key);
        return;
    }
    if (f.send) {
        SubmitBuckets(f.buckets, f.screen_w, f.screen_h, true);
    } else {
        g.frame_draws = 0;
        g.frame_triangles = 0;
    }
    rt.target_slot = int(&f - rt.slots);
    rt.target_fraction = 1.0;
    const auto t1 = Steady::now();
    const bool keyed = !g.harness_keys.empty() && te::harness::Armed();
    const bool wanted = keyed && HarnessWants(f.harness_key);
    if (wanted) ps2e_draw_arm_capture(g.dev, 1);   // this picture alone is read back
    {
        TE_PROF_SCOPE("ps2e_draw_present");
        // the tick lock (te_tick_lock.h): the display buffer this swap queues holds the picture for its lock's refreshes
        if (show == Show::Present) te::tick_lock::BeforeSwap(f.lock_fields);
        if (show == Show::Present) ps2e_draw_present(g.dev);
        else ps2e_draw_capture_frame(g.dev);
    }
#if defined(__vita__)
    if (show == Show::Present) te::vita_gpu::OnPresented();   // the GPU's lag at this swap (+ the periodic probe)
#endif
    const auto t2 = Steady::now();
    g.last_present_call_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    if (!OnRenderThread()) {
        g.last_submit_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        g.last_present_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    }
    // The harness leg (te_harness.h): this picture's hash + descriptor under the key of the boundary it precedes
    if (!keyed || wanted) te::harness::OnPresent(g.dev, f.harness_key);
    else te::harness::OnPresentSkipped(f.harness_key);
    if (wanted && g.dump_dir.empty()) ps2e_draw_arm_capture(g.dev, 0);
    if (show == Show::Present) NotePresented();
    MaybeDump(picture);
}

// A frame between two pictures (the slot trains): at fraction 1 the newest picture as it is; below it the interpolated
// scene when the interpolator is on (the engine's SceneInterpolator), else the newest picture again.
void RenderBetween(Gfx::Frame& prev, Gfx::Frame& cur, double fraction, int64_t slot_ns) {
    (void)prev;
    (void)slot_ns;
    auto& rt = g.rt;
    const int64_t period = 1000000000LL / 60;
    const int cur_slot = int(&cur - rt.slots);
    // The latency is noted only while the newest picture is fresh (within two ticks): a load screen or a frame the game
    // did not present keeps the old picture on the window, which is not the presenter's latency
    auto note = [&](double shown_fraction) {
        const int64_t now = NowNs();
        if (cur.have_edge && now - cur.edge_ns <= 2 * period) NoteLatency(cur.edge_ns - period + int64_t(shown_fraction * double(period)), now);
    };
    if (fraction >= 1.0 || !rt.interpolate) {
        // the newest picture as it is: when the frame target already holds it (it was resolved at its arrival, or shown at
        // the previous slot) the scan-out is re-presented without a submit
        if (rt.target_slot == cur_slot && rt.target_fraction == 1.0) {
            rt.represents++;
        } else {
            if (cur.send) SubmitBuckets(cur.buckets, cur.screen_w, cur.screen_h, false);
            rt.target_slot = cur_slot;
            rt.target_fraction = 1.0;
        }
        ps2e_draw_present(g.dev);
#if defined(__vita__)
        te::vita_gpu::OnPresented();
#endif
        NotePresented();
        note(1.0);
        if (rt.interpolate && rt.last_noted_picture != cur.picture) {   // the displacement check measures the next frame from this picture
            BuildInterpItems(cur);
            ps2r::ps2e::InterpScene sc;
            sc.items = cur.items.data();
            sc.count = cur.items.size();
            sc.ordinal = cur.picture;
            rt.interpolator.NoteShownPicture(sc);
            rt.last_noted_picture = cur.picture;
        }
        return;
    }
    // The interpolated frame: the two pictures as the interpolator reads them (built once per picture), the frame at the
    // fraction, submitted in the newest picture's draw order
    BuildInterpItems(prev);
    BuildInterpItems(cur);
    auto scene_of = [](const Gfx::Frame& f) {
        ps2r::ps2e::InterpScene sc;
        sc.items = f.items.data();
        sc.count = f.items.size();
        sc.camera = f.camera;
        sc.off_x = kGsMiddle - float(f.screen_w >> 1);
        sc.off_y = kGsMiddle - float(f.screen_h >> 1);
        sc.screen_scale = 16.f;   // the title's toScreen lands on the GS's 12.4 raster (the mesh builder divides by 16)
        sc.ordinal = f.picture;
        return sc;
    };
    ps2r::ps2e::InterpScene sp = scene_of(prev), sc = scene_of(cur);
    sc.cut = prev.camera_object != cur.camera_object;   // another camera object: a cut (the game switched cameras)
    const auto ti0 = Steady::now();
    const bool interpolated = rt.interpolator.Interpolate(sp, sc, fraction, rt.interp_out);
    const auto ti1 = Steady::now();
    if (!interpolated) rt.snaps++;
    else rt.interp_frames++;
#if defined(__vita__)
    {
        // the interpolated frame's screen copies announced like SubmitBuckets' (the depth kept across their scene breaks)
        int copies = 0;
        for (const auto& o : rt.interp_out) copies += (o.count >= 3 && o.copy_frame) ? 1 : 0;
        ps2e_draw_expect_frame_copies(g.dev, copies);
    }
#endif
    BeginFrameTarget(cur.screen_w, cur.screen_h);
    uint64_t submitted = 0;
    for (const auto& o : rt.interp_out) {
        if (o.count < 3) continue;
        if (o.copy_frame) ps2e_draw_copy_frame(g.dev);
        if (ps2e_draw_submit(g.dev, o.state, o.verts, int(o.count)) < 0) g.dropped_draws++;
        submitted++;
    }
    g.draws_submitted += submitted;
    rt.target_slot = cur_slot;
    rt.target_fraction = interpolated ? fraction : 1.0;
    const auto ti2 = Steady::now();
    ps2e_draw_present(g.dev);
#if defined(__vita__)
    te::vita_gpu::OnPresented();
#endif
    const auto ti3 = Steady::now();
    rt.interp_ms_sum += std::chrono::duration<double, std::milli>(ti1 - ti0).count();
    rt.submit_ms_sum += std::chrono::duration<double, std::milli>(ti2 - ti1).count();
    rt.present_ms_sum += std::chrono::duration<double, std::milli>(ti3 - ti2).count();
    rt.phase_n++;
    NotePresented();
    note(interpolated ? fraction : 1.0);
    // the one extra scene copy: the previous picture's vertex storage kept beside the newest, and the interpolator's scratch
    uint64_t prev_bytes = 0;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b)
        for (size_t i = 0; i < prev.buckets[b].used; ++i) prev_bytes += prev.buckets[b].draws[i].verts.size() * sizeof(ps2e_draw_vertex);
    rt.interp_bytes = std::max<uint64_t>(rt.interp_bytes, prev_bytes + rt.interpolator.ScratchBytes());
    if (rt.dump_interp && !g.dump_dir.empty()) {
        const bool in_window = (g.dump_from <= 0 || cur.picture >= uint64_t(g.dump_from)) && (g.dump_to <= 0 || cur.picture <= uint64_t(g.dump_to));
        if (in_window && interpolated) {
            char path[1024];
            std::snprintf(path, sizeof path, "%s/frame_%06llu_i%03d.ppm", g.dump_dir.c_str(), (unsigned long long)cur.picture, int(fraction * 1000.0 + 0.5));
            ps2e_draw_write_ppm(g.dev, path);
        }
    }
    if (rt.interp_diag && cur.picture % 100 == 0 && interpolated) {
        // fraction 1 on this very pair must reproduce the newest picture: the worst vertex error per kind
        std::vector<ps2r::ps2e::InterpOutput> check;
        ps2r::ps2e::SceneInterpolator probe;
        probe.Interpolate(sp, sc, 1.0, check);
        double worst = 0; size_t worst_item = 0; uint32_t worst_v = 0; size_t compared = 0;
        for (size_t i = 0; i < check.size() && i < cur.items.size(); ++i) {
            if (check[i].verts == cur.items[i].verts) continue;
            for (uint32_t v = 0; v < check[i].count; ++v) {
                const double e = std::hypot(double(check[i].verts[v].x - cur.items[i].verts[v].x), double(check[i].verts[v].y - cur.items[i].verts[v].y));
                compared++;
                if (e > worst) { worst = e; worst_item = i; worst_v = v; }
            }
        }
        std::fprintf(stderr, "[te.gfx.interp] picture %llu fraction-1 check: %zu vertices re-projected, worst %.3f px (item %zu v %u", (unsigned long long)cur.picture, compared, worst, worst_item, worst_v);
        if (compared && worst_item < cur.items.size() && worst_v < check[worst_item].count) {
            const auto& a = cur.items[worst_item].verts[worst_v];
            const auto& b = check[worst_item].verts[worst_v];
            std::fprintf(stderr, ": drawn (%.2f %.2f z %.0f q %.5f) got (%.2f %.2f z %.0f)", a.x, a.y, a.z, a.q, b.x, b.y, b.z);
            if (const auto* tr = cur.items[worst_item].transform) {
                // the drawn vertex back through S^-1: what w does the inverse give, and does S forward reproduce it
                double S[16], Sinv[16];
                for (int c = 0; c < 16; ++c) S[c] = tr->screen[c];
                if (ps2r::ps2e::SceneInterpolator::Invert4(S, Sinv)) {
                    const double X = (a.x + sc.off_x) * 16.0, Y = (a.y + sc.off_y) * 16.0, Z = a.z;
                    double m[4] = {0, 0, 0, 0};
                    for (int c = 0; c < 4; ++c) m[c] = X * Sinv[c] + Y * Sinv[4 + c] + Z * Sinv[8 + c] + Sinv[12 + c];
                    std::fprintf(stderr, " | S^-1 -> w %.5f model (%.3f %.3f %.3f); tag %llx count %u", m[3], m[0] / m[3], m[1] / m[3], m[2] / m[3],
                                 (unsigned long long)cur.items[worst_item].tag.id, cur.items[worst_item].count);
                    double W[16], t[3], q[4], scl[3], R[16];
                    for (int c = 0; c < 16; ++c) W[c] = tr->world[c];
                    const bool dec = ps2r::ps2e::SceneInterpolator::DecomposeTrs(W, t, q, scl);
                    double rt_err = -1;
                    if (dec) {
                        ps2r::ps2e::SceneInterpolator::ComposeTrs(t, q, scl, R);
                        rt_err = 0;
                        for (int c = 0; c < 16; ++c) rt_err = std::max(rt_err, std::fabs(R[c] - W[c]));
                    }
                    std::fprintf(stderr, " | world rows (%.4g %.4g %.4g %.4g) (%.4g %.4g %.4g %.4g) (%.4g %.4g %.4g %.4g) (%.4g %.4g %.4g %.4g) TRS round trip err %.4g scale (%.3g %.3g %.3g)",
                                 W[0], W[1], W[2], W[3], W[4], W[5], W[6], W[7], W[8], W[9], W[10], W[11], W[12], W[13], W[14], W[15], rt_err, scl[0], scl[1], scl[2]);
                    // world . world->screen (the interpolator's camera) vs the drawn S
                    double ws[16], prod[16], cerr = 0;
                    double cam[16], c2s[16];
                    for (int c = 0; c < 16; ++c) { cam[c] = cur.camera.to_camera[c]; c2s[c] = cur.camera.camera_to_screen[c]; }
                    ps2r::ps2e::SceneInterpolator::Multiply4(cam, c2s, ws);
                    ps2r::ps2e::SceneInterpolator::Multiply4(W, ws, prod);
                    for (int c = 0; c < 16; ++c) cerr = std::max(cerr, std::fabs(prod[c] - S[c]));
                    std::fprintf(stderr, " | world.cam.c2s vs drawn S err %.4g (camera valid %d)", cerr, int(cur.camera.valid));
                }
            }
        }
        std::fprintf(stderr, ")\n");
    }
    if (rt.interp_diag) {
        const auto& st = rt.interpolator.Stats();
        size_t tagged_rigid = 0, tagged_vertices = 0, untagged = 0;
        for (const auto& it : cur.items) {
            if (it.tag.id == 0 || it.tag.kind == ps2r::ps2e::InterpKind::Newest) untagged++;
            else if (it.tag.kind == ps2r::ps2e::InterpKind::Rigid) tagged_rigid++;
            else tagged_vertices++;
        }
        std::fprintf(stderr, "[te.gfx.interp] picture %llu items: rigid %zu vertex runs %zu untagged %zu (splitting %d)\n", (unsigned long long)cur.picture, tagged_rigid, tagged_vertices, untagged, int(g.interp_active.load()));
        if (cur.picture % 300 == 0)
            for (const auto& it : cur.items)
                if (it.tag.kind == ps2r::ps2e::InterpKind::Vertices)
                    std::fprintf(stderr, "[te.gfx.interp]   vertex run %llx part %u count %u\n", (unsigned long long)it.tag.id, it.tag.part, it.count);
        std::fprintf(stderr, "[te.gfx.interp] picture %llu fraction %.3f %s: rigid %llu vertices %llu newest %llu unmatched %llu snaps %llu unprojectable %llu; displacement checks %llu violations %llu\n",
                     (unsigned long long)cur.picture, fraction, interpolated ? "interpolated" : "SNAPPED",
                     (unsigned long long)st.rigid_items, (unsigned long long)st.vertex_items, (unsigned long long)st.newest_items,
                     (unsigned long long)st.unmatched, (unsigned long long)st.object_snaps, (unsigned long long)st.unprojectable,
                     (unsigned long long)st.displacement_checks, (unsigned long long)st.displacement_violations);
    }
}

// The quality judge's probe (TE_GFX_INTERP_PROBE): the half-way frame between two pictures built and dumped at the newest's
// arrival, whatever the pacer's slots do -- frame_<picture>_i500.ppm beside the pictures' dumps. The frame target then
// holds the probe, not a picture (the next slot re-submits).
void ProbeHalf(Gfx::Frame& prev, Gfx::Frame& cur) {
    auto& rt = g.rt;
    if (g.dump_dir.empty()) return;
    const bool in_window = (g.dump_from <= 0 || cur.picture >= uint64_t(g.dump_from)) && (g.dump_to <= 0 || cur.picture <= uint64_t(g.dump_to));
    if (!in_window) return;
    BuildInterpItems(prev);
    BuildInterpItems(cur);
    auto scene_of = [](const Gfx::Frame& f) {
        ps2r::ps2e::InterpScene sc;
        sc.items = f.items.data();
        sc.count = f.items.size();
        sc.camera = f.camera;
        sc.off_x = kGsMiddle - float(f.screen_w >> 1);
        sc.off_y = kGsMiddle - float(f.screen_h >> 1);
        sc.screen_scale = 16.f;
        sc.ordinal = f.picture;
        return sc;
    };
    ps2r::ps2e::InterpScene sp = scene_of(prev), sc = scene_of(cur);
    sc.cut = prev.camera_object != cur.camera_object;
    static ps2r::ps2e::SceneInterpolator probe;   // its own history: the presenter's displacement census is untouched
    static std::vector<ps2r::ps2e::InterpOutput> out;
    const bool interpolated = probe.Interpolate(sp, sc, 0.5, out);
    if (!interpolated) return;   // a cut: no half frame to judge
    BeginFrameTarget(cur.screen_w, cur.screen_h);
    for (const auto& o : out) {
        if (o.count < 3) continue;
        if (o.copy_frame) ps2e_draw_copy_frame(g.dev);
        ps2e_draw_submit(g.dev, o.state, o.verts, int(o.count));
    }
    ps2e_draw_capture_frame(g.dev);
    char path[1024];
    std::snprintf(path, sizeof path, "%s/frame_%06llu_i500.ppm", g.dump_dir.c_str(), (unsigned long long)cur.picture);
    ps2e_draw_write_ppm(g.dev, path);
    rt.target_slot = -1;
}

// Present: the frame handed to the render thread (the bucket storage swapped, one frame in flight) or drawn here, then
// the blank
// TE_AB=lean|vector|joinblend|texswap|particlefull|skinvu|nativescroll|draws|gamecpu2 (+ TE_AB_EVERY=N, default 120 pictures): an
// in-run A/B of one performance knob, flipped every N
// pictures (off, on, off, ...) and logged as `[te.ab] picture P: <knob> on|off` -- the two arms then share the machine's
// load (a loaded host swamps a comparison of separate runs). Applied right after the frame's builds are handed over.
void AbToggle() {
    static const char* knob = std::getenv("TE_AB");
    if (!knob || !*knob) return;
    static const long every = std::max(1L, EnvLong("TE_AB_EVERY", 120));
    uint64_t n = g.pictures_handed;
    bool on;
#if defined(__vita__)
    // The low-latency hand-over replaces pictures the drawing thread has not taken: the pictures handed run ahead of the
    // presents every census window counts (TE_GFX_SUMMARY_EVERY, te.cpu, te.prof). There the arms flip by PRESENTS --
    // presents 1..N off, N+1..2N on, ... as the backend's abN switches count them -- so with TE_AB_EVERY a multiple of
    // TE_GFX_SUMMARY_EVERY every window of the device log lies in one arm (the flip lands at the first picture made after
    // the window's last present). Elsewhere (and in deterministic runs) by pictures, as before.
    if (LowLatency()) {
        static int arm_now = -1;
        const uint64_t p = g.presents.load(std::memory_order_relaxed);
        const int arm = int((p / uint64_t(every)) % 2);
        if (arm == arm_now) return;
        arm_now = arm;
        on = arm == 1;
        std::fprintf(stderr, "[te.ab] presents from %llu (the low-latency hand-over: the arms by presents)\n",
                     (unsigned long long)p + 1);
    } else
#endif
    {
        if (n % uint64_t(every) != 0) return;
        on = (n / uint64_t(every)) % 2 == 1;
    }
    if (std::strcmp(knob, "lean") == 0) Builds().lean = on;
    else if (std::strcmp(knob, "vector") == 0) title::MeshGeometry::SetVectorTransform(on);
    else if (std::strcmp(knob, "joinblend") == 0) g.join_blended = on;
    else if (std::strcmp(knob, "texswap") == 0) { if (g.dev) OnRender([on] { ps2e_draw_set_mesh_texture_swap(g.dev, on ? 2 : 0); }); }
    else if (std::strcmp(knob, "particlefull") == 0) te::gfx::SetParticleFull(on);
    else if (std::strcmp(knob, "skinvu") == 0) title::SkinGeometry::SetFastPaths(!on);   // on = the VU programs (the earlier form)
    else if (std::strcmp(knob, "nativescroll") == 0) te::gfx::SetNativeScrolled(on);    // on = the scrolled blends native
    else if (std::strcmp(knob, "gamecpu2") == 0) te::gfx::SetGameCpuFast(on);          // on = the fast game-thread seams
    else if (std::strcmp(knob, "gpuskin") == 0) te::gfx::SetGpuSkins(on);              // on = the skins on the GPU
    else if (std::strcmp(knob, "chunked") == 0) te::gfx::SetChunkedNative(on);         // on = clipped parts / sky by chunks
    else if (std::strcmp(knob, "gpublend") == 0) te::gfx::SetGpuBlend(on);             // on = the blend skins' parts too
    else if (std::strcmp(knob, "gpu") == 0) {                                             // both of the above together
        te::gfx::SetGpuSkins(on);
        te::gfx::SetChunkedNative(on);
    }
    else if (std::strcmp(knob, "draws") == 0) {
        // on = the drawing thread's lean path (the lean per-draw sequence, the texture + alpha test + mips short path,
        // the shadow volumes grouped); off = as before
        if (g.dev) OnRender([on] {
            ps2e_draw_set_backend_option(g.dev, "draw_legacy", on ? 0 : 1);
            ps2e_draw_set_mesh_texture_swap(g.dev, on ? 2 : 1);
            g.group_volumes = on;
        });
    }
    else return;
    std::fprintf(stderr, "[te.ab] picture %llu: %s %s\n", (unsigned long long)n, knob, on ? "on" : "off");
}

void PresentFrame(bool send) {
    FlushBuilds();   // the frame's staged builds handed to the workers: the drawing thread waits for them
    AbToggle();
    if (EagerTexturesOn()) SweepTextureFailures();   // (the eager textures' failed decodes: the synchronous path's handle 0)
    // The tick lock (te_tick_lock.h): whether this present ends a picture tick (draws, not a movie picture) and the fields
    // its boundary will span (0 = not a picture tick, 1 = 60 Hz, 2 / 3 = the lock) -- committed right before that boundary
    const bool movie_picture = g.movie_picture;
    g.movie_picture = false;
    const uint32_t lock_fields = te::tick_lock::PeekPlan(send && !movie_picture);
    if (!EnsureDevice()) {
        ClearBuckets();
        return;
    }
    auto& rt = g.rt;
    // The harness leg's key: the boundary this present precedes (read on the game thread, before any hand-over)
    const uint64_t harness_key = te::Services::Get().clock().BoundaryCount() + 1;
    {
        TE_PROF_SCOPE("present: FlushNativeMeshes");
        te::gfx::FlushNativeMeshes();   // the meshes this picture's models asked for, ready for the next picture's draws
    }
    if (rt.started) {
        TE_PROF_SCOPE("present: hand-over");
        const auto t0 = Steady::now();
#if defined(__vita__)
        // The low-latency hand-over (Gfx::ll_on): a picture still waiting for the drawing thread is replaced by this one.
        // Its slot is taken out of the queue (the drawing thread cannot take it any more), its builds are waited for
        // (cancelled first where not started: their storage comes back to this thread with the slot) and the slot is
        // refilled below; the meshes retired before it stay on the slot's retire list, so they are destroyed only once no
        // picture the presenter can show is older than this one -- the list the replaced picture would have kept.
        int replaced = -1;
        bool skip = false;   // a present without draws while a picture waits: the waiting one stays, this one is dropped
        if (LowLatency()) {
            std::lock_guard<std::mutex> lock(rt.mutex);
            if (!rt.queue.empty()) {
                if (send) {
                    // (the tick lock draws every picture: a 30 / 20 Hz picture is never replaced, nor replaces one -- the
                    // queue's backpressure is the lock's pacing; the 60 Hz pictures as before)
                    if (lock_fields <= 1 && rt.slots[rt.queue.back()].lock_fields <= 1) {
                        replaced = rt.queue.back();
                        rt.queue.pop_back();
                    }
                } else {
                    skip = true;
                }
            }
        }
        if (skip) {
            // (its retired meshes stay on g.mesh_retired: the next picture handed over carries them; its builds, posted at
            // FlushBuilds above, finish -- or are cancelled -- before ClearBuckets reuses their storage)
            for (size_t i = 0; i < g.deferred_used; ++i) CancelBuild(*g.deferred[i]);
            for (size_t i = 0; i < g.deferred_used; ++i) Builds().Wait(*g.deferred[i]);
            ++g.pictures_handed;
            g.ll_handed.store(g.pictures_handed, std::memory_order_relaxed);
            g.ll_dropped.fetch_add(1, std::memory_order_relaxed);
            g.ll_boundaries.store(harness_key - 1, std::memory_order_relaxed);
            g.last_submit_ms = 0;
            g.last_present_ms = std::chrono::duration<double, std::milli>(Steady::now() - t0).count();
            {
                TE_PROF_SCOPE("present: ClearBuckets");
                ClearBuckets();
            }
            te::gfx::ResetDrawOrdinals();
            te::tick_lock::CommitPlan(lock_fields);
            VBlank();
            return;
        }
        if (replaced >= 0) {
            TE_PROF_SCOPE("hand-over: the replaced picture's builds");
            const auto b0 = Steady::now();
            Gfx::Frame& old = rt.slots[replaced];
            for (size_t i = 0; i < old.deferred_used; ++i) CancelBuild(*old.deferred[i]);
            for (size_t i = 0; i < old.deferred_used; ++i) Builds().Wait(*old.deferred[i]);
            g.ll_dropped.fetch_add(1, std::memory_order_relaxed);
            te::tick_lock::NoteReplaced();
            g.ll_build_wait_us.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Steady::now() - b0).count()),
                                         std::memory_order_relaxed);
        }
#endif
        {
            std::unique_lock<std::mutex> lock(rt.mutex);
            int slot = -1;
#if defined(__vita__)
            slot = replaced;
#endif
            if (slot < 0 && rt.free.empty()) {   // one drawing and two queued: the render thread is more than two frames behind
                TE_PROF_SCOPE("hand-over wait (render thread behind)");
                rt.waits++;
                if (lock_fields >= 2) te::tick_lock::NoteBackpressure();   // the lock's pipeline behind: a drain tick next
                rt.done.wait(lock, [&] { return !rt.free.empty(); });
                const double waited = std::chrono::duration<double, std::milli>(Steady::now() - t0).count();
                rt.wait_ms += waited;
                rt.wait_max_ms = std::max(rt.wait_max_ms, waited);
            }
            if (slot < 0) {
                slot = rt.free.back();
                rt.free.pop_back();
            }
            Gfx::Frame& f = rt.slots[slot];
            for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
                std::swap(g.buckets[b].draws, f.buckets[b].draws);
                std::swap(g.buckets[b].used, f.buckets[b].used);
#if defined(__vita__)
                std::swap(g.buckets[b].holds, f.buckets[b].holds);   // (the records' storage marks travel with them)
                f.buckets[b].ordered = false;   // a new picture's draws: its order is made when it is drawn (ExpandBucket)
#endif
            }
            std::swap(g.deferred, f.deferred);
            std::swap(g.deferred_used, f.deferred_used);
            f.retire.insert(f.retire.end(), g.mesh_retired.begin(), g.mesh_retired.end());
            g.mesh_retired.clear();
            for (auto& r : g.storage_retired) f.retired_storage.push_back(std::move(r));
            g.storage_retired.clear();
            f.send = send;
            f.lock_fields = lock_fields;
#if defined(__vita__)
            f.pool_ordinal = g_pool.recording;   // (the compact records' pool entries this picture names: StatePool)
#endif
            f.screen_w = ScreenW();
            f.screen_h = ScreenH();
            f.harness_key = harness_key;
            f.items.clear();   // this picture's draws for the interpolator are listed on the render thread when needed
            f.picture = ++g.pictures_handed;   // its ordinal (the game thread's count: the render thread's g.pictures lags it)
            // the pacer edge of the field this picture follows (the presenter's clock for the frames between pictures)
            f.have_edge = te::Services::Get().clock().LatestFieldEdge(&f.edge_ns, &f.field);
            if (rt.interp_diag && f.picture % 200 == 0)
                std::fprintf(stderr, "[te.gfx.interp] hand-over: picture %llu boundary %llu field %llu edge age %.2f ms, work since the release %.2f ms\n", (unsigned long long)f.picture,
                             (unsigned long long)harness_key - 1, (unsigned long long)f.field, f.have_edge ? double(NowNs() - f.edge_ns) / 1e6 : -1.0,
                             te::Services::Get().clock().MsSinceRelease());
            f.camera = ps2r::ps2e::InterpCamera{};
            f.camera_object = 0;
            te::gfx::CurrentCamera(f.camera, &f.camera_object);
            rt.queue.push_back(slot);
#if defined(__vita__)
            g.ll_handed.store(g.pictures_handed, std::memory_order_relaxed);
            g.ll_boundaries.store(harness_key - 1, std::memory_order_relaxed);
#endif
        }
        rt.wake.notify_one();
        // The phases the slow-tick line reports: no submit on this thread; "present" = the hand-over (the wait, if any)
        g.last_submit_ms = 0;
        g.last_present_ms = std::chrono::duration<double, std::milli>(Steady::now() - t0).count();
    } else {
        // synchronous (deterministic / headless / dumping): every picture presented whatever the frame-rate setting
        static Gfx::Frame sync;
        for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
            std::swap(g.buckets[b].draws, sync.buckets[b].draws);
            std::swap(g.buckets[b].used, sync.buckets[b].used);
#if defined(__vita__)
            std::swap(g.buckets[b].holds, sync.buckets[b].holds);
            sync.buckets[b].ordered = false;   // (as the hand-over's: this picture's order made at its expansion)
#endif
        }
        sync.send = send;
        sync.lock_fields = lock_fields;
        sync.screen_w = ScreenW();
        sync.screen_h = ScreenH();
        sync.harness_key = harness_key;
        sync.picture = 0;
        // (eager textures, no drawing thread: the decoded ones made here, the rest at their first draw)
        if (EagerTexturesOn()) MakeReadyTextures(nullptr);
        ExpandDeferred(sync.buckets, g.deferred, g.deferred_used);
        RenderFrame(sync, Show::Present);
#if defined(__vita__)
        g_pool.done.store(g_pool.recording, std::memory_order_release);   // this picture expanded and drawn (StatePool)
#endif
        for (uint32_t mesh : g.mesh_retired) ps2e_draw_mesh_destroy(g.dev, mesh);   // drawn for the last time just now
        g.mesh_retired.clear();
        g.storage_retired.clear();   // (the synchronous present waited for every build of the picture)
        for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
            std::swap(g.buckets[b].draws, sync.buckets[b].draws);
            std::swap(g.buckets[b].used, sync.buckets[b].used);
#if defined(__vita__)
            std::swap(g.buckets[b].holds, sync.buckets[b].holds);
#endif
        }
    }
    {
        TE_PROF_SCOPE("present: ClearBuckets");
        ClearBuckets();
    }
    te::gfx::ResetDrawOrdinals();   // the next picture's draw identities count from the start
    te::tick_lock::CommitPlan(lock_fields);   // the boundary below is this picture tick's (te_tick_lock.h)
    VBlank();
}

// The census row the runner prints at exit and the harness records:
//   present=<rate>:<shown fps>:<interpolated frames>:<snaps>:<latency ms>:<late presents>
std::string PresentCensus() {
    auto& rt = g.rt;
    double shown_fps = 0;
    if (g.have_first_present) {
        const double s = std::chrono::duration<double>(Steady::now() - g.first_present).count();
        if (s > 0.5) shown_fps = double(g.presents.load() - 1) / s;
    }
    const auto c = rt.pacer.Count();
    char buf[2048];
    const auto& st = rt.interpolator.Stats();
    std::snprintf(buf, sizeof buf, "present=%s:%.1f:%llu:%llu:%.2f:%llu (pictures %llu, presents %llu, re-presented %llu, latency max %.2f ms, delay %.2f ms (max %.2f), interpolated frame %.2f + %.2f + %.2f ms (interpolate, submit, present), interpolated scene %llu bytes; rigid %llu vertex runs %llu newest %llu unmatched %llu object snaps %llu unprojectable %llu count mismatches %llu run jumps %llu; displacement checks %llu violations %llu)",
                  ps2r::ps2e::PresentPacer::Name(static_cast<Rate>(g.frame_rate.load())), shown_fps,
                  (unsigned long long)rt.interp_frames, (unsigned long long)rt.snaps,
                  rt.latency_n ? rt.latency_sum_ms / double(rt.latency_n) : 0.0, (unsigned long long)c.late,
                  (unsigned long long)g.pictures.load(), (unsigned long long)g.presents.load(), (unsigned long long)rt.represents,
                  rt.latency_max_ms, double(rt.delay_ns) / 1e6, rt.delay_ms_max,
                  rt.phase_n ? rt.interp_ms_sum / double(rt.phase_n) : 0.0, rt.phase_n ? rt.submit_ms_sum / double(rt.phase_n) : 0.0, rt.phase_n ? rt.present_ms_sum / double(rt.phase_n) : 0.0,
                  (unsigned long long)rt.interp_bytes,
                  (unsigned long long)st.rigid_items, (unsigned long long)st.vertex_items, (unsigned long long)st.newest_items,
                  (unsigned long long)st.unmatched, (unsigned long long)st.object_snaps, (unsigned long long)st.unprojectable,
                  (unsigned long long)st.vertex_count_mismatch, (unsigned long long)st.vertex_run_jumps, (unsigned long long)st.displacement_checks, (unsigned long long)st.displacement_violations);
    return buf;
}

}  // namespace

// ---- shared with graphics_scene.cpp (graphics_internal.h) ----------------------------------------------------------------
namespace te::gfx {
bool EnsureDevice() { return ::EnsureDevice(); }
MaterialView ReadMaterial(uint32_t address) { return ::ReadMaterial(address); }
uint32_t TextureHandleAt(uint32_t address) { return address ? ::TextureHandleOf(address) : 0; }
ps2e_draw_state StateOf(const MaterialView& m) { return ::StateOf(m); }
Draw& OpenDraw(uint32_t bucket, const ps2e_draw_state& state, bool copy_frame) { return ::OpenDraw(bucket, state, copy_frame); }
MaterialView ShaderView(const MaterialView& m, uint32_t k) { return ::ShaderView(m, k); }
void CloseDraw(uint32_t bucket) { ::CloseDraw(bucket); }
void SetDrawTag(const ps2r::ps2e::InterpTag& tag, const float* world16, const float* screen16) { ::SetDrawTag(tag, world16, screen16); }
void ClearDrawTag() { ::ClearDrawTag(); }
bool Interpolating() { return g.interp_active.load(std::memory_order_relaxed); }
bool NativeMeshes() {
    if (g.native_meshes < 0) {
        const char* v = std::getenv("TE_GFX_NATIVE_MESH");
        g.native_meshes = (v && v[0] == '0') ? 0 : 1;
    }
    return g.native_meshes == 1;
}
// A prepared mesh's work here unless a worker has it (then waited for: the worker is mid-way, at its lower priority)
bool PreparedMesh::Ensure() {
    int expected = kQueued;
    if (state.compare_exchange_strong(expected, kMaking, std::memory_order_acq_rel)) {
        compute(*this);
        state.store(kMade, std::memory_order_release);
        return true;
    }
    while (expected == kMaking) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        expected = state.load(std::memory_order_acquire);
    }
    return expected == kMade;
}
bool PreparedMesh::TryMake() {
    int expected = kQueued;
    if (!state.compare_exchange_strong(expected, kMaking, std::memory_order_acq_rel)) return false;
    compute(*this);
    state.store(kMade, std::memory_order_release);
    return true;
}
void PreparedMesh::Settle() {
    int expected = kQueued;
    if (state.compare_exchange_strong(expected, kCancelled, std::memory_order_acq_rel)) return;
    while (expected == kMaking) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        expected = state.load(std::memory_order_acquire);
    }
}
// A request's mesh made (the drawing thread): from its prepared form (the same mesh: ps2e_draw_mesh_create_prepared), or
// from its list
uint32_t MakeRequestMesh(MeshRequest& r) {
    if (r.prepared) return r.prepared->Ensure() && r.prepared->mesh ? ps2e_draw_mesh_create_prepared(g.dev, r.prepared->mesh) : 0;
    return ps2e_draw_mesh_create(g.dev, r.vertices.data(), int(r.vertices.size()));
}
// the vertices the request's mesh is made of (a decline's test: as many as the list had)
size_t RequestVertices(const MeshRequest& r) {
    return r.prepared ? size_t(r.prepared->mesh ? ps2e_draw_mesh_prepared_count(r.prepared->mesh) : 0) : r.vertices.size();
}
void CreateMeshes(std::vector<MeshRequest>& requests) {
    if (requests.empty() || !::EnsureDevice()) return;
    bool declined = false;
    te::load::Timed waited(te::load::kMeshWait, 0);
    OnRender([&] {
        const int64_t t0 = te::load::NowNs();
        for (MeshRequest& r : requests) {
            if (!r.skin_vertices.empty()) {   // a skin part's (a decline turns the skins' GPU path off, not the meshes')
                r.handle = ps2e_draw_skin_mesh_create(g.dev, r.skin_vertices.data(), int(r.skin_vertices.size()),
                                                      r.skin_indices.data(), int(r.skin_indices.size()), r.skin_max_alpha);
                continue;
            }
            r.handle = MakeRequestMesh(r);
            if (!r.handle && RequestVertices(r) >= 3) declined = true;
        }
        te::load::NoteRenderMeshes(uint32_t(requests.size()), double(te::load::NowNs() - t0) / 1e6);
    });
    if (declined) g.native_meshes = 0;   // a device without the native transform: the CPU builds from here on
}
uint64_t CreateMeshesAsync(const std::shared_ptr<MeshBatchJob>& batch) {
    if (!batch || batch->requests.empty() || !::EnsureDevice()) return 0;
    auto make = [batch] {
        const int64_t t0 = te::load::NowNs();
        for (MeshRequest& r : batch->requests) {
            if (!r.skin_vertices.empty()) {   // a skin part's (a decline turns the skins' GPU path off, not the meshes')
                r.handle = ps2e_draw_skin_mesh_create(g.dev, r.skin_vertices.data(), int(r.skin_vertices.size()),
                                                      r.skin_indices.data(), int(r.skin_indices.size()), r.skin_max_alpha);
                r.skin_vertices.clear();
                r.skin_vertices.shrink_to_fit();
                r.skin_indices.clear();
                r.skin_indices.shrink_to_fit();
                continue;
            }
            r.handle = MakeRequestMesh(r);
            if (!r.handle && RequestVertices(r) >= 3) batch->declined = true;
            r.vertices.clear();
            r.vertices.shrink_to_fit();   // uploaded: the copy is not needed any more
            if (r.prepared && r.prepared->mesh) {
                // (the GPU has the mesh: the prepared vertices are not needed any more)
                ps2e_draw_mesh_prepared_free(r.prepared->mesh);
                r.prepared->mesh = nullptr;
            }
        }
        te::load::NoteRenderMeshes(uint32_t(batch->requests.size()), double(te::load::NowNs() - t0) / 1e6);
    };
    auto& rt = g.rt;
    if (!rt.started || OnRenderThread()) {   // no drawing thread (or on it): made here and now, as CreateMeshes
        make();
        if (batch->declined) g.native_meshes = 0;
        return 0;
    }
    uint64_t ticket;
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        rt.jobs.push_back(make);
        ticket = ++rt.jobs_posted;
    }
    rt.wake.notify_one();
    return ticket;
}
void WaitMeshes(uint64_t ticket, const MeshBatchJob& batch) {
    if (ticket) {
        auto& rt = g.rt;
        std::unique_lock<std::mutex> lock(rt.mutex);
        if (rt.jobs_done < ticket) {
            TE_PROF_SCOPE("mesh wait (drawing thread behind)");
            te::load::Timed waited(te::load::kMeshWait, 0);
            rt.done.wait(lock, [&] { return rt.jobs_done >= ticket || rt.stop; });   // (stopped: never made)
        }
    }
    if (batch.declined) g.native_meshes = 0;   // a device without the native transform: the CPU builds from here on
}
void RecordMesh(uint32_t bucket, const ps2e_draw_state& state, uint32_t mesh, const ps2e_draw_mesh_params& params) {
    RecordMeshInPlace(bucket, state, mesh) = params;
}
ps2e_draw_mesh_params& RecordMeshInPlace(uint32_t bucket, const ps2e_draw_state& state, uint32_t mesh) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    g.draws++;
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
#if defined(__vita__)
    // the next record's lines on their way into the cache while this one is filled (the bucket's storage was last touched
    // frames ago, by the other core: every store of a record missed): the lines a mesh record writes (TE_GFX_PREFETCH=0: off)
    if (g.prefetch_records && b.used < b.draws.size()) {
        // (the record's members a mesh draw writes come first: graphics_internal.h Draw -- everything before the palette)
        const Draw& n = b.draws[b.used];
        const char* const p0 = reinterpret_cast<const char*>(&n);
        const char* const p1 = reinterpret_cast<const char*>(&n.palette);
        for (const char* p = p0; p < p1; p += 32) __builtin_prefetch(p, 1, 3);
    }
#endif
    // (in the record's member order: one sequential sweep of its first lines)
    d.mesh = mesh;
    d.pool = 0;
    d.skin_joints = 0;
    d.copy_frame = false;
    d.has_transform = false;
    d.pending = nullptr;
    d.ext = nullptr;
    d.ext_count = 0;
    d.verts.clear();
    d.tag = g.tag;
    d.state = state;
    return d.mesh_params;
}
#if defined(__vita__)
bool CompactMeshes() { return ::CompactRecording(); }
bool CompactCheck() { return g_pool.check; }
uint32_t PoolState(const ps2e_draw_state& state, uint32_t* generation) { return g_pool.Intern(state, generation); }
bool PoolCurrent(uint32_t pool, uint32_t generation) { return g_pool.Current(pool, generation); }
// The compact record: line 0 written here (the record's header), the caller writes the parameters' screen, offset, scroll,
// lit (+ the lights); ExpandCompact fills in the rest on the drawing thread. (`state` = the pool entry's bytes: written
// whole as well only under TE_GFX_COMPACT_CHECK, for the expansion to compare with.)
ps2e_draw_mesh_params& RecordMeshCompact(uint32_t bucket, uint32_t pool, const ps2e_draw_state& state, uint32_t mesh) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : uint32_t(TE_GFX_BUCKET_UI)];
    g.draws++;
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
    if (g.prefetch_records && b.used < b.draws.size()) {
        // the next record's header and the parameters' first lines (what the next compact record writes)
        const char* const n = reinterpret_cast<const char*>(&b.draws[b.used]);
        __builtin_prefetch(n, 1, 3);
        __builtin_prefetch(n + 64, 1, 3);
        __builtin_prefetch(n + 96, 1, 3);
        __builtin_prefetch(n + 128, 1, 3);
    }
    d.mesh = mesh;
    d.pool = uint16_t(pool);
    d.copy_frame = false;
    d.has_transform = false;
    d.pending = nullptr;
    d.ext = nullptr;
    d.ext_count = 0;
    if (!d.verts.empty()) d.verts.clear();
    g_pool.Use(pool);
    g_pool.compact_records++;
    if (g_pool.check) {
        // the whole record as RecordMeshInPlace writes it (+ the caller's box / first / count): what the expansion compares
        d.skin_joints = 0;
        d.tag = g.tag;
        d.state = state;
    }
    return d.mesh_params;
}
std::string CompactCensusLine(double presents) {
    const uint64_t c = g_pool.compact_records, f = g_pool.full_records, k = g_pool.compared.load(std::memory_order_relaxed),
                   x = g_pool.differ.load(std::memory_order_relaxed);
    const uint32_t e = g_pool.expanded.load(std::memory_order_relaxed);
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "compact mesh records per present: %.1f (whole: the pool full %.1f), expanded %.1f; state pool %u entries made, "
                  "%llu remade, %llu lookups (%llu found), %u in use by the last %u pictures%s",
                  double(c - g_pool.last_compact) / presents, double(f - g_pool.last_full) / presents,
                  double(uint32_t(e - g_pool.last_expanded)) / presents, g_pool.made, (unsigned long long)g_pool.remade,
                  (unsigned long long)g_pool.lookups, (unsigned long long)g_pool.found, g_pool.InUse(),
                  g_pool.recording - g_pool.done.load(std::memory_order_relaxed), "");
    std::string out = buf;
    if (g_pool.ab > 0) {
        std::snprintf(buf, sizeof buf, "; this window's arms (TE_GFX_COMPACT=ab%ld): compact in %llu of %llu pictures", g_pool.ab,
                      (unsigned long long)(g_pool.presents_on - g_pool.last_presents_on),
                      (unsigned long long)(g_pool.presents_counted - g_pool.last_presents_counted));
        out += buf;
    }
    g_pool.last_presents_on = g_pool.presents_on;
    g_pool.last_presents_counted = g_pool.presents_counted;
    if (g_pool.check) {
        std::snprintf(buf, sizeof buf, "; CHECK (TE_GFX_COMPACT_CHECK): %llu expansions compared with the whole record, %llu differ",
                      (unsigned long long)k, (unsigned long long)x);
        out += buf;
    }
    g_pool.last_compact = c;
    g_pool.last_full = f;
    g_pool.last_expanded = e;
    return out;
}
void NoteWholeMeshRecord() { g_pool.full_records++; }
#else
bool CompactMeshes() { return false; }
bool CompactCheck() { return false; }
uint32_t PoolState(const ps2e_draw_state&, uint32_t* generation) {
    if (generation) *generation = 0;
    return 0;
}
bool PoolCurrent(uint32_t, uint32_t) { return false; }
ps2e_draw_mesh_params& RecordMeshCompact(uint32_t bucket, uint32_t, const ps2e_draw_state& state, uint32_t mesh) {
    return RecordMeshInPlace(bucket, state, mesh);
}
std::string CompactCensusLine(double) { return std::string(); }
void NoteWholeMeshRecord() {}
#endif
void RecordSkinMesh(uint32_t bucket, const ps2e_draw_state& state, uint32_t mesh, const ps2e_draw_mesh_params& params,
                    const float* bones, uint32_t joints, const float* disp, uint32_t disp_count) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    g.draws++;
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
    d.state = state;
    d.verts.clear();
    d.ext = nullptr;
    d.ext_count = 0;
    d.copy_frame = false;
    d.pending = nullptr;
    d.pool = 0;
    d.tag = g.tag;
    d.has_transform = false;
    d.mesh = mesh;
    d.mesh_params = params;
    d.palette.assign(bones, bones + size_t(joints) * 12);
    if (disp && disp_count) d.palette.insert(d.palette.end(), disp, disp + size_t(disp_count) * 4);
    d.skin_joints = joints;
    d.skin_disp = disp && disp_count ? disp_count : 0;
}
void RetireMesh(uint32_t mesh) {
    if (mesh) g.mesh_retired.push_back(mesh);
}
void Record(uint32_t bucket, Draw&& d) { ::Record(bucket, std::move(d)); }
bool DeferringBuilds() { return ::DeferringBuilds(); }
DeferredBuild& ReserveDeferred(uint32_t bucket, const ps2e_draw_state& state) { return ::ReserveDeferred(bucket, state); }
void PostBuildJob(DeferredBuild& build, BuildJob&& job) { ::PostBuildJob(build, std::move(job)); }
void PostBackgroundJob(BuildJob&& job) { ::PostBackgroundJob(std::move(job)); }
void RetireStorage(std::unique_ptr<RetiredStorage> storage) {
    // (no drawing thread: the builds are drained at every present -- freed at the next one, as with a drawing thread)
    if (storage) g.storage_retired.push_back(std::move(storage));
}
// The blocks: the posting thread's list, refilled whole from the stack the releasing threads push onto (one consumer takes
// the whole stack at once, so the pushes' compare-exchange has no ABA case)
namespace {
struct FreeBlock { FreeBlock* next; };
std::atomic<FreeBlock*> g_returned_blocks{nullptr};
FreeBlock* g_local_blocks = nullptr;
}
void* BuildJob::AcquireBlock() {
    if (!g_local_blocks) g_local_blocks = g_returned_blocks.exchange(nullptr, std::memory_order_acquire);
    if (FreeBlock* b = g_local_blocks) {
        g_local_blocks = b->next;
        return b;
    }
    return ::operator new(kBlockBytes);
}
void BuildJob::ReleaseBlock(void* block) {
    FreeBlock* b = static_cast<FreeBlock*>(block);
    FreeBlock* head = g_returned_blocks.load(std::memory_order_relaxed);
    do {
        b->next = head;
    } while (!g_returned_blocks.compare_exchange_weak(head, b, std::memory_order_release, std::memory_order_relaxed));
}
void DrainBuilds() { ::DrainBuilds(); }
std::string PoolCensusLine(double frames) { return ::PoolCensusLine(frames); }
float GsOriginX() { return ::GsOriginX(); }
float GsOriginY() { return ::GsOriginY(); }
int ScreenW() { return ::ScreenW(); }
int ScreenH() { return ::ScreenH(); }
const RenderTargetDescription* CurrentTarget() { return ::CurrentTarget(); }
bool LogEnabled() { ReadOptions(); return g.log; }
SceneCounters& Scene() { static SceneCounters counters; return counters; }
}  // namespace te::gfx

// Blanks the graphics side passed through (one per WaitVSync / Present): diagnostics; the clock is te::Clock's.
extern "C" uint64_t te_graphics_vblanks() { return g.vblanks; }
// The last Present's phases on the game thread (the clock's `tick:` line, when a tick is slow): the draws' submission to
// the backend and the scan-out/swap; the rest of the tick is the game's own work
extern "C" void te_graphics_last_present_phases_ms(double* submit, double* present, double* pump) {
    if (submit) *submit = g.last_submit_ms;
    if (present) *present = g.last_present_ms;
    if (pump) *pump = g.last_pump_ms;
}
extern "C" uint64_t te_graphics_presents() { return g.presents.load(); }
// The presenter's census row (the runner's exit lines / the harness): present=rate:shown_fps:interp_frames:snaps:latency_ms:late_presents
extern "C" void te_graphics_present_census(char* out, size_t size) {
    const std::string row = PresentCensus();
    if (!out || !size) return;
    std::snprintf(out, size, "%s", row.c_str());
}

// ---- save state (savestate.h, kTagGraphics): the renderer's host-side state the game cannot rebuild -- the textures
// (decoded again from their kept header + packet), the display mode, the counters. The frame's buckets are empty at a
// boundary (Present cleared them before the blank), so nothing of the frame is state.
namespace te::gfx {
void RegisterStateSection() {
    static const bool once = [] {
        savestate::Register(savestate::kTagGraphics, "graphics host", false,
            [](std::vector<uint8_t>& out) {
                savestate::Put(out, uint32_t(5));
                savestate::Put(out, uint8_t(g.pal ? 1 : 0));
                savestate::Put(out, int32_t(g.display_w)); savestate::Put(out, int32_t(g.display_h));
                savestate::Put(out, uint8_t(g.have_frame ? 1 : 0));
                savestate::PutBytes(out, &g.frame, sizeof g.frame);
                savestate::Put(out, g.presents.load()); savestate::Put(out, g.vblanks); savestate::Put(out, g.draws);
                savestate::Put(out, uint32_t(g.texture_sources.size()));
                for (const auto& [address, source] : g.texture_sources) {
                    savestate::Put(out, address);
                    savestate::PutBytes(out, &source.header, sizeof source.header);
                    savestate::Put(out, uint32_t(source.packet.size()));
                    savestate::PutBytes(out, source.packet.data(), source.packet.size());
                }
                // version 2: the scene's decoded models' packets (graphics_scene.cpp keeps the bytes: the game released them)
                const auto models = ModelSources();
                savestate::Put(out, uint32_t(models.size()));
                for (const ModelSource& m : models) {
                    savestate::Put(out, m.model); savestate::Put(out, m.index); savestate::Put(out, m.count); savestate::Put(out, m.vertices);
                    savestate::Put(out, uint32_t(m.packet.size()));
                    savestate::PutBytes(out, m.packet.data(), m.packet.size());
                }
                // version 3: the skins' submodel packets, the same shape
                const auto skins = SkinSources();
                savestate::Put(out, uint32_t(skins.size()));
                for (const ModelSource& m : skins) {
                    savestate::Put(out, m.model); savestate::Put(out, m.index); savestate::Put(out, m.count); savestate::Put(out, m.vertices);
                    savestate::Put(out, uint32_t(m.packet.size()));
                    savestate::PutBytes(out, m.packet.data(), m.packet.size());
                }
                // version 4: the screen models (the skid marks' strips the game holds handles to)
                const auto screen = ScreenModelState();
                savestate::Put(out, uint32_t(screen.size()));
                savestate::PutBytes(out, screen.data(), screen.size());
                // version 5: the pictures' count (the dumps' numbering; = presents in a synchronous run)
                savestate::Put(out, g.pictures.load());
            },
            [](const savestate::Blob& blob, std::string* err) {
                savestate::PayloadReader in(blob.payload);
                uint32_t version = 0, count = 0;
                uint8_t pal = 0, have_frame = 0;
                int32_t w = 0, h = 0;
                FrameStart frame{};
                uint64_t presents = 0, vblanks = 0, draws = 0;
                auto fail = [&](const char* why) { if (err) *err = std::string("graphics: ") + why; return false; };
                if (!in.Get(&version) || version < 2 || version > 5 || !in.Get(&pal) || !in.Get(&w) || !in.Get(&h) || !in.Get(&have_frame) ||
                    !in.GetBytes(&frame, sizeof frame) || !in.Get(&presents) || !in.Get(&vblanks) || !in.Get(&draws) || !in.Get(&count) ||
                    count > 65536)
                    return fail("payload");
                g.pal = pal != 0;
                g.display_w = w; g.display_h = h;
                g.have_frame = have_frame != 0;
                g.frame = frame;
                g.presents = presents; g.vblanks = vblanks; g.draws = draws;
                g.pictures = presents;   // a state before version 5: every present was a picture
                g.pictures_handed = presents;
                OnRender([] {
                    DropAllPending();   // (the eager textures not made yet: their ids are not the loaded state's)
                    for (auto& [address, handle] : g.textures) if (g.dev) ps2e_draw_texture_destroy(g.dev, handle);
                });
                g.textures.clear();
                TexturesCleared();
                g.texture_sources.clear();
                if (g.have_frame && EnsureDevice()) {
                    OnRender([] {
                        int fw = 0, fh = 0;
                        ps2e_draw_frame_size(g.dev, &fw, &fh);
                        if (fw != ScreenW() || fh != ScreenH()) ps2e_draw_resize_frame(g.dev, ScreenW(), ScreenH());
                    });
                }
                for (uint32_t k = 0; k < count; ++k) {
                    uint32_t address = 0;
                    Gfx::TextureSource source;
                    if (!in.Get(&address) || !in.GetBytes(&source.header, sizeof source.header) ||
                        !savestate::GetVector(in, source.packet, 64u << 20))
                        return fail("payload truncated (textures)");
                    te::gfx::DecodedTexture decoded;
                    std::string error;
                    if (!EnsureDevice()) continue;   // the bytes are consumed: the models follow them in the payload
                    if (!te::gfx::DecodeTextureResource(source.header, source.packet.data(), source.packet.size(), decoded, &error)) {
                        std::fprintf(stderr, "[te.gfx] savestate: texture %#x: decode failed: %s\n", address, error.c_str());
                        continue;
                    }
                    uint32_t handle = 0;
                    OnRender([&] { handle = CreateTexture(decoded); });
                    if (!handle) continue;
                    g.textures[address] = handle;
                    TextureChanged(address);
                    g.texture_sources[address] = std::move(source);
                }
                uint32_t model_count = 0;
                if (!in.Get(&model_count) || model_count > 1u << 20) return fail("payload truncated (models)");
                std::vector<ModelSource> models(model_count);
                for (ModelSource& m : models)
                    if (!in.Get(&m.model) || !in.Get(&m.index) || !in.Get(&m.count) || !in.Get(&m.vertices) ||
                        !savestate::GetVector(in, m.packet, 64u << 20))
                        return fail("payload truncated (models)");
                ReplayModelSources(models);
                std::vector<ModelSource> skins;
                if (version >= 3) {
                    uint32_t skin_count = 0;
                    if (!in.Get(&skin_count) || skin_count > 1u << 20) return fail("payload truncated (skins)");
                    skins.resize(skin_count);
                    for (ModelSource& m : skins)
                        if (!in.Get(&m.model) || !in.Get(&m.index) || !in.Get(&m.count) || !in.Get(&m.vertices) ||
                            !savestate::GetVector(in, m.packet, 64u << 20))
                            return fail("payload truncated (skins)");
                }
                ReplaySkinSources(skins);
                if (version >= 4) {
                    std::vector<uint8_t> screen;
                    if (!savestate::GetVector(in, screen, 64u << 20) || !RestoreScreenModelState(screen.data(), screen.size()))
                        return fail("payload truncated (screen models)");
                } else {
                    RestoreScreenModelState(nullptr, 0);
                }
                if (version >= 5) {
                    uint64_t pictures = 0;
                    if (!in.Get(&pictures)) return fail("payload truncated (pictures)");
                    g.pictures = pictures;
                    g.pictures_handed = pictures;
                }
                return true;
            });
        return true;
    }();
    (void)once;
}

void SetPresentation(const ps2e_draw_presentation& p) {
    g.presentation = p;
    if (g.dev) ps2e_draw_set_presentation(g.dev, &g.presentation);   // thread-safe: the pump / the next frame apply it
}

bool Headless() {
    ReadOptions();
    return g.headless;
}

void SetFrameRate(int rate) {
    g.frame_rate.store(std::clamp(rate, 0, 5), std::memory_order_relaxed);
    if (g.rt.started) g.rt.wake.notify_one();   // the render thread re-sets its pacer at its next pass
}
void SetInterpolation(bool on) {
    if (std::getenv("TE_GFX_INTERP")) return;   // the variable wins (read at the device's start)
    g.rt.interpolate = on;
}

}  // namespace te::gfx

extern "C" {

// ---- devices / display -------------------------------------------------------------------------------------------------
void ps2e_graphics_reset_devices(void) { Once("ResetDevices: no hardware to reset"); }
void ps2e_graphics_reset_path(void) { Once("ResetPath: no GIF path"); }
void ps2e_graphics_wait_v_sync(void) { EnsureDevice(); VBlank(); }
ps2e_s32 ps2e_graphics_is_pal_display(void) { return g.pal ? 1 : 0; }
void ps2e_graphics_wait_idle(void) { Once("WaitIdle: draws are synchronous"); }

void ps2e_graphics_start_renderer(ps2e_s32 height, ps2e_s32 pal) {
    g.pal = pal != 0;
    g.display_w = 512;
    g.display_h = height > 0 && height <= 1024 ? height : (g.pal ? 512 : 448);
    // The game's g_DisplayWidth/g_DisplayHeight, as the PS2 arm's InitDisplay set them (the movie's picture area)
    if (uint32_t* w = GuestMutable<uint32_t>(te::RetailAddress(kDisplayWidth))) *w = uint32_t(g.display_w);
    if (uint32_t* h = GuestMutable<uint32_t>(te::RetailAddress(kDisplayHeight))) *h = uint32_t(g.display_h);
    if (g.dev) {
        OnRender([] { ps2e_draw_resize_frame(g.dev, g.display_w, g.display_h); });
    } else {
        EnsureDevice();
    }
}
void ps2e_graphics_finish_renderer_start(void) { Once("FinishRendererStart"); }
// MoveDisplay (the graphic options' "centre screen" page: the screen position): the picture moved on the display by an
// offset between -1 and 1 each way -- the PS2 moved DISPLAY DX/DY by up to 256 VCK (64 frame pixels) across and 32 lines
// down; the port moves the presented picture by the same frame pixels and persists it (ps2r.ini).
void ps2e_graphics_move_display(ps2e_u32 offset) {
    const Vector2* o = Guest<Vector2>(offset);
    if (!o) return;
    te::display::MoveDisplay(o->x, o->y);
}
// (StepAnimations: the wasm side, graphics_resources.cpp -- shader animation)

// frame.cpp SetUpRenderTarget: the target's clip-to-screen matrix and its corner in GS pixels; the packet it kept is not
// needed (the draws carry their own state).
void ps2e_graphics_set_up_render_target(ps2e_u32 target) {
    auto* t = GuestMutable<RenderTargetDescription>(target);
    if (!t) return;
    const float left = kGsMiddle - float(t->displayWidth) * 0.5f + float(t->offsetX);
    const float top = kGsMiddle - float(t->displayHeight) * 0.5f + float(t->offsetY);
    const float height = float(t->height), width = float(t->width);
    for (auto& row : t->clipToScreen)
        for (float& v : row) v = 0.f;
    t->clipToScreen[0][0] = width * 8.0f;
    t->clipToScreen[1][1] = -height * 8.0f;
    t->clipToScreen[2][2] = -8388607.0f;
    t->clipToScreen[3][0] = (left + width * 0.5f) * 16.0f;
    t->clipToScreen[3][1] = (top + height * 0.5f) * 16.0f;
    t->clipToScreen[3][2] = 8388607.0f;
    t->clipToScreen[3][3] = 1.0f;
    t->x = int32_t(left);
    t->y = int32_t(top);
}

// ---- present ---------------------------------------------------------------------------------------------------------
void ps2e_graphics_present(ps2e_u32 commands) { PresentFrame(commands != 0); }
void ps2e_graphics_present_from_interrupt(ps2e_u32 commands) {
    (void)commands;
    PresentFrame(true);
}
void ps2e_graphics_wait_sent(void) { Once("WaitSent: draws are synchronous"); }
// SubmitBuckets is wasm-side (graphics_resources.cpp): the frame's memory starts over there; the buckets are drawn at Present
void ps2e_graphics_finish_frame(void) { Once("FinishFrame"); }
void ps2e_graphics_finish_scene(ps2e_s32 effects) {
    (void)effects;
    Once("FinishScene: screen effects are not drawn yet; 2D draws are already in their buckets");
}
// ResetBuckets (wasm side) -> the frame's buckets start over
void ps2e_gfx2_frame_reset(ps2e_s32 movie) {
    (void)movie;
    ClearBuckets();
}

// StartFrame: the frame's clear as the first draw of bucket 0 -- the strips of the frame's rectangle in the clear colour
// (colour and/or depth), as frame.cpp wrote them.
void ps2e_graphics_start_frame(ps2e_u32 frame) {
    const FrameStart* f = Guest<FrameStart>(frame);
    if (!f) return;
    g.frame = *f;
    g.have_frame = true;
    g.start_frames++;
    EnsureDevice();
    if (!f->clearColor && !f->clearDepth) return;
    g.clears++;
    ps2e_draw_state st{};
    st.gouraud = 0;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = f->clearDepth ? 1 : 0;
    st.no_colour_write = f->clearColor ? 0 : 1;
    const uint32_t colour = uint32_t(f->red) | uint32_t(f->green) << 8 | uint32_t(f->blue) << 16 | 0x80u << 24;
    const float ox = GsOriginX(), oy = GsOriginY();
    const float x0 = float(f->x) - ox, y0 = float(f->y) - oy, x1 = float(f->x + f->width) - ox, y1 = float(f->y + f->height) - oy;
    Draw d;
    d.state = st;
    auto V = [&](float x, float y) {
        ps2e_draw_vertex v = Vertex(x, y, 0, 0, colour);
        v.z = 0.f;   // the GS far plane: the depth clear
        return v;
    };
    d.verts = {V(x0, y0), V(x1, y0), V(x0, y1), V(x1, y0), V(x1, y1), V(x0, y1)};
    ::Record(TE_GFX_BUCKET_SKY, std::move(d));
}

void ps2e_graphics_use_helper_programs(ps2e_s32 set, ps2e_s32 wait) {
    (void)set;
    (void)wait;
    Once("UseHelperPrograms: VU0 microcode sets have no meaning here (Platform::Math is native)");
}

// ---- 2D (draw2d.cpp) -------------------------------------------------------------------------------------------------
void ps2e_graphics_draw_sprite(ps2e_u32 material, ps2e_s32 colour, ps2e_u32 place, ps2e_u32 texture) {
    const Rectangle* p = Guest<Rectangle>(place);
    const Rectangle* t = Guest<Rectangle>(texture);
    if (!p || !t) return;
    const MaterialView m = ::ReadMaterial(material);
    const float sw = GameScreenW(), sh = GameScreenH();
    const float left = p->x * sw + FrameCornerX(TargetOffsetX());
    const float top = p->y * sh + FrameCornerY(TargetOffsetY());
    const float right = left + p->width * sw, bottom = top + p->height * sh;
    g.sprites++;
    if (g.log && g.sprites <= 64)
        std::fprintf(stderr, "[te.gfx] sprite #%llu material %#x kind %u bucket %u shader %#x settings %#llx tex %#x handle %u colour %#x "
                     "place (%.3f %.3f %.3f %.3f) -> gs (%.1f %.1f)-(%.1f %.1f) origin (%.0f %.0f) uv (%.2f %.2f %.2f %.2f)\n",
                     (unsigned long long)g.sprites, material, m.material ? m.material->kind : 99u, m.bucket,
                     m.material ? m.material->shaders[0] : 0u, m.shader ? (unsigned long long)m.shader->settings : 0ull,
                     m.shader ? m.shader->texture : 0u, m.texture_handle, uint32_t(colour), p->x, p->y, p->width, p->height,
                     left, top, right, bottom, GsOriginX(), GsOriginY(), t->x, t->y, t->width, t->height);
    RecordQuad(m.bucket, ::StateOf(m), left, top, right, bottom, t->x, t->y, t->x + t->width, t->y + t->height, uint32_t(colour));
}

void ps2e_graphics_draw_turned_sprite(ps2e_u32 material, ps2e_s32 colour, ps2e_u32 corners, ps2e_u32 texture) {
    const Vector2* c = reinterpret_cast<const Vector2*>(te::GuestPointer(corners, sizeof(Vector2) * 4));
    const Rectangle* t = Guest<Rectangle>(texture);
    if (!c || !t) return;
    const MaterialView m = ::ReadMaterial(material);
    const float sw = GameScreenW(), sh = GameScreenH();
    const float left = FrameCornerX(0), top = FrameCornerY(0);
    const float ox = GsOriginX(), oy = GsOriginY();
    // The corners show the texture area's corner, the corner a height from it, the corner a width from it and the opposite
    const Vector2 uv[4] = {{t->x, t->y}, {t->x, t->y + t->height}, {t->x + t->width, t->y}, {t->x + t->width, t->y + t->height}};
    ps2e_draw_vertex v[4];
    for (int i = 0; i < 4; ++i) v[i] = Vertex(c[i].x * sw + left - ox, c[i].y * sh + top - oy, uv[i].x, uv[i].y, uint32_t(colour));
    g.turned_sprites++;
    Draw d;
    d.state = ::StateOf(m);
    ClampWholeTexture(d.state, t->x, t->y, t->x + t->width, t->y + t->height);
    d.verts = {v[0], v[1], v[2], v[1], v[3], v[2]};   // the strip 0 1 2 3 as triangles
    ::Record(m.bucket, std::move(d));
}

void ps2e_graphics_draw_strip(ps2e_u32 material, ps2e_s32 count, ps2e_u32 places, ps2e_u32 colours) {
    if (count < 3 || count > 4096) return;
    const Vector2* p = reinterpret_cast<const Vector2*>(te::GuestPointer(places, sizeof(Vector2) * size_t(count)));
    const uint32_t* c = reinterpret_cast<const uint32_t*>(te::GuestPointer(colours, 4 * size_t(count)));
    if (!p || !c) return;
    const MaterialView m = ::ReadMaterial(material);
    const float left = FrameCornerX(TargetOffsetX()) - GsOriginX(), top = FrameCornerY(TargetOffsetY()) - GsOriginY();
    g.strips++;
#if defined(__vita__)
    // the triangles appended straight to the bucket's open draw (what Record does with a temporary Draw: its vertex vector
    // was a heap allocation and its free per call on the Vita, and a copy)
    ps2e_draw_state state = ::StateOf(m);
    state.texture = 0;
    Draw& open = ::OpenDraw(m.bucket, state);
    open.verts.reserve(open.verts.size() + size_t(count - 2) * 3);
    for (int i = 2; i < count; ++i)
        for (int k = i - 2; k <= i; ++k) open.verts.push_back(Vertex(p[k].x + left, p[k].y + top, 0, 0, c[k]));
    ::CloseDraw(m.bucket);
#else
    Draw d;
    d.state = ::StateOf(m);
    d.state.texture = 0;
    d.verts.reserve(size_t(count - 2) * 3);
    for (int i = 2; i < count; ++i) {
        // a strip's triangle i: vertices i-2, i-1, i
        for (int k = i - 2; k <= i; ++k) d.verts.push_back(Vertex(p[k].x + left, p[k].y + top, 0, 0, c[k]));
    }
    ::Record(m.bucket, std::move(d));
#endif
}

// Font::Draw (wasm side) -> the text item about to be expanded into quads: the string, where (screen fractions) and how it is
// aligned. Kept for the text-draw census only (the diag line of the quads that follow names the string).
struct TextItemCensus { uint32_t text = 0; float x = 0, y = 0; int alignment = 0; };
static TextItemCensus g_text_item;

void ps2e_gfx2_text_item(ps2e_u32 text, ps2e_f32 x, ps2e_f32 y, ps2e_s32 alignment) {
    g_text_item.text = text;
    g_text_item.x = x;
    g_text_item.y = y;
    g_text_item.alignment = alignment;
}

// Font::Draw (wasm side) -> the glyph quads of one page material: GS pixel corners, texels of the page
void ps2e_gfx2_text_quads(ps2e_u32 material, ps2e_u32 quads, ps2e_s32 count) {
    if (count <= 0 || count > 4096) return;
    const auto* q = reinterpret_cast<const te_gfx_text_quad*>(te::GuestPointer(quads, sizeof(te_gfx_text_quad) * size_t(count)));
    if (!q) return;
    const MaterialView m = ::ReadMaterial(material);
    ps2e_draw_state st = ::StateOf(m);
    float tw = 1.f, th = 1.f;
    if (m.texture) {
        tw = float(1 << m.texture->texture.widthPower);
        th = float(1 << m.texture->texture.heightPower);
    }
    const float ox = GsOriginX(), oy = GsOriginY();
    g.text_quads += uint64_t(count);
    static const bool textDiag = std::getenv("TE_GFX_DIAG_TEXT") != nullptr;
#if defined(__vita__)
    // (TE_GFX_DIAG_PRESENT read once: a getenv every call -- the environment's linear scan -- on the Vita)
    static const char* const diagPresent = std::getenv("TE_GFX_DIAG_PRESENT");
    if (const char* diag = textDiag ? "all" : diagPresent) {
#else
    if (const char* diag = textDiag ? "all" : std::getenv("TE_GFX_DIAG_PRESENT")) {
#endif
        if (textDiag || g.presents.load() + 1 == uint64_t(std::atol(diag))) {
            // The TEXT-QUAD HASH (the language judge): FNV-1a over every glyph's texel rectangle and its size -- the glyph sequence of
            // the string drawn, independent of where on the frame, how large and in what colour it is drawn; two draws of the same string
            // from the same font hash alike, a translation differs.
            uint64_t h = 1469598103934665603ull;
            auto mix = [&h](float f) { uint32_t v; std::memcpy(&v, &f, 4); for (int i = 0; i < 4; ++i) { h ^= uint8_t(v >> (8 * i)); h *= 1099511628211ull; } };
            // The BOX of every quad of this call in frame pixels (the census: where the string is on the frame), and the frame's size
            float bx0 = q[0].x0, by0 = q[0].y0, bx1 = q[0].x1, by1 = q[0].y1;
            for (int i = 0; i < count; ++i) {
                mix(q[i].u0); mix(q[i].v0); mix(q[i].u1); mix(q[i].v1);   // texels only: the selected row breathes (its size varies)
                bx0 = std::min(bx0, q[i].x0); by0 = std::min(by0, q[i].y0); bx1 = std::max(bx1, q[i].x1); by1 = std::max(by1, q[i].y1);
            }
            const char* text = g_text_item.text ? te::GuestString(g_text_item.text, 128) : nullptr;
            std::fprintf(stderr, "[te.gfx.diag] present %llu text %d quads material %#x tex %u tfx %d tcc %d colour %08x first (%.1f %.1f)-(%.1f %.1f) glyph %.1fx%.1f quadhash %016llx"
                         " box (%.1f %.1f)-(%.1f %.1f) frame %dx%d at (%.3f %.3f) align %#x string '%s'\n", (unsigned long long)g.presents.load() + 1, count,
                         material, st.texture, st.tfx, st.tcc, q[0].colour, q[0].x0 / 16.f - ox, q[0].y0 / 16.f - oy, q[0].x1 / 16.f - ox, q[0].y1 / 16.f - oy,
                         (q[0].x1 - q[0].x0) / 16.f, (q[0].y1 - q[0].y0) / 16.f, (unsigned long long)h,
                         bx0 / 16.f - ox, by0 / 16.f - oy, bx1 / 16.f - ox, by1 / 16.f - oy, ScreenW(), ScreenH(), g_text_item.x, g_text_item.y,
                         unsigned(g_text_item.alignment), text ? text : "");
        }
    }
#if defined(__vita__)
    // the quads appended straight to the bucket's open draw (Record's temporary Draw: a heap allocation, its free and a
    // copy per call on the Vita)
    Draw& open = ::OpenDraw(m.bucket, st);
    open.verts.reserve(open.verts.size() + size_t(count) * 6);
    for (int i = 0; i < count; ++i) {
        const float x0 = q[i].x0 / 16.f - ox, y0 = q[i].y0 / 16.f - oy, x1 = q[i].x1 / 16.f - ox, y1 = q[i].y1 / 16.f - oy;
        const float u0 = q[i].u0 / 16.f / tw, v0 = q[i].v0 / 16.f / th, u1 = q[i].u1 / 16.f / tw, v1 = q[i].v1 / 16.f / th;
        const uint32_t colour = q[i].colour;
        open.verts.push_back(Vertex(x0, y0, u0, v0, colour));
        open.verts.push_back(Vertex(x1, y0, u1, v0, colour));
        open.verts.push_back(Vertex(x0, y1, u0, v1, colour));
        open.verts.push_back(Vertex(x1, y0, u1, v0, colour));
        open.verts.push_back(Vertex(x1, y1, u1, v1, colour));
        open.verts.push_back(Vertex(x0, y1, u0, v1, colour));
    }
    ::CloseDraw(m.bucket);
#else
    Draw d;
    d.state = st;
    d.verts.reserve(size_t(count) * 6);
    for (int i = 0; i < count; ++i) {
        const float x0 = q[i].x0 / 16.f - ox, y0 = q[i].y0 / 16.f - oy, x1 = q[i].x1 / 16.f - ox, y1 = q[i].y1 / 16.f - oy;
        const float u0 = q[i].u0 / 16.f / tw, v0 = q[i].v0 / 16.f / th, u1 = q[i].u1 / 16.f / tw, v1 = q[i].v1 / 16.f / th;
        const uint32_t colour = q[i].colour;
        d.verts.push_back(Vertex(x0, y0, u0, v0, colour));
        d.verts.push_back(Vertex(x1, y0, u1, v0, colour));
        d.verts.push_back(Vertex(x0, y1, u0, v1, colour));
        d.verts.push_back(Vertex(x1, y0, u1, v0, colour));
        d.verts.push_back(Vertex(x1, y1, u1, v1, colour));
        d.verts.push_back(Vertex(x0, y1, u0, v1, colour));
    }
    ::Record(m.bucket, std::move(d));
#endif
}

// ---- textures (wasm side ReadTexture -> the decoded image) -----------------------------------------------------------
ps2e_s32 ps2e_gfx2_texture_loaded(ps2e_u32 texture, ps2e_u32 header, ps2e_u32 image, ps2e_s32 size) {
    const auto* h = Guest<te_gfx_texture_header>(header);
    const uint8_t* bytes = te::GuestPointer(image, size_t(size));
    if (!h || !bytes || size <= 0 || !EnsureDevice()) return 0;
    if (EagerTexturesOn() && h->widthPower <= 10 && h->heightPower <= 10) {
        // EAGER TEXTURES (see Eager): the id reserved, the decode queued, nothing waited for. (A size power out of range
        // is the decode's first refusal, answered here as the synchronous path does: below.)
        TE_PROF_SCOPE("texture: id reserved, decode queued (ahead of its use)");
        SweepTextureFailures();
        te::load::Timed timed(te::load::kTexMake, 0);   // (the game thread's time here: the copies and the queueing)
        auto old = g.textures.find(texture);
        if (old != g.textures.end()) {
            const uint32_t stale = old->second;
            PostRender([stale] {
                DropPending(stale);
                ps2e_draw_texture_destroy(g.dev, stale);
            });
            g.textures.erase(old);
            TextureChanged(texture);
        }
        const uint32_t handle = ps2e_draw_texture_reserve(g.dev);
        if (!handle) return 0;
        auto t = std::make_shared<PendingTexture>();
        t->id = handle;
        t->address = texture;
        t->header = *h;
        t->packet.assign(bytes, bytes + size);
        std::fegetenv(&t->env);
        t->loaded_ns = te::load::NowNs();
        {
            std::lock_guard<te::sync::Mutex> lock(Eager().mutex);
            Eager().pending[handle] = t;
        }
        Eager().reserved.fetch_add(1, std::memory_order_relaxed);
        QueueDecode(t);
        g.textures[texture] = handle;
        TextureChanged(texture);
        g.texture_sources[texture] = Gfx::TextureSource{*h, std::vector<uint8_t>(bytes, bytes + size)};
        g.textures_loaded++;
        if (g.log)
            std::fprintf(stderr, "[te.gfx] texture %#x: %dx%d psm %u (upload %u) levels %u -> handle %u (decoded ahead)\n", texture,
                         1 << h->widthPower, 1 << h->heightPower, h->pixelFormat, h->uploadFormat, h->levels, handle);
        return ps2e_s32(handle);
    }
    te::gfx::DecodedTexture decoded;
    std::string error;
    bool decoded_ok;
    {
        TE_PROF_SCOPE("texture: decode (the GS memory -> RGBA)");
        te::load::Timed timed(te::load::kTexDecode);
        decoded_ok = te::gfx::DecodeTextureResource(*h, bytes, size_t(size), decoded, &error);
        timed.bytes = DecodedBytes(decoded);
    }
    if (!decoded_ok) {
        g.texture_failures++;
        std::fprintf(stderr, "[te.gfx] texture %#x: decode failed: %s (psm %u upload %u %ux%u levels %u)\n", texture, error.c_str(),
                     h->pixelFormat, h->uploadFormat, 1u << h->widthPower, 1u << h->heightPower, h->levels);
        return 0;
    }
    auto old = g.textures.find(texture);
    if (old != g.textures.end()) {
        const uint32_t stale = old->second;
        te::load::Timed timed(te::load::kTexRelease);
        OnRender([stale] { ps2e_draw_texture_destroy(g.dev, stale); });
        g.textures.erase(old);
        TextureChanged(texture);
    }
    if (const char* dir = std::getenv("TE_GFX_DUMP_TEXTURES")) {
        char path[1024];
        std::snprintf(path, sizeof path, "%s/texture_%08x.ppm", dir, texture);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", decoded.width, decoded.height);
            for (uint32_t px : decoded.rgba) { const uint8_t rgb[3] = {uint8_t(px), uint8_t(px >> 8), uint8_t(px >> 16)}; std::fwrite(rgb, 1, 3, f); }
            std::fclose(f);
        }
    }
    uint32_t handle = 0;
    {
        TE_PROF_SCOPE("texture: made on the drawing thread (the wait)");
        te::load::Timed timed(te::load::kTexMake);
        OnRender([&] { handle = CreateTexture(decoded); });
    }
    if (!handle) return 0;
    g.textures[texture] = handle;
    TextureChanged(texture);
    g.texture_sources[texture] = Gfx::TextureSource{*h, std::vector<uint8_t>(bytes, bytes + size)};
    g.textures_loaded++;
    if (g.log)
        std::fprintf(stderr, "[te.gfx] texture %#x: %dx%d psm %u (upload %u) levels %u -> handle %u (%u transfers, %u bytes)\n", texture,
                     decoded.width, decoded.height, h->pixelFormat, h->uploadFormat, h->levels, handle, decoded.transfers,
                     decoded.image_bytes);
    return ps2e_s32(handle);
}

void ps2e_gfx2_texture_released(ps2e_u32 texture) {
    g.texture_sources.erase(texture);
    auto it = g.textures.find(texture);
    if (it == g.textures.end()) return;
    const uint32_t handle = it->second;
    if (g.dev && EagerTexturesOn()) {
        // (eager: posted, not waited for -- run in order with the creations, before the next picture handed over)
        PostRender([handle] {
            DropPending(handle);
            ps2e_draw_texture_destroy(g.dev, handle);
        });
    } else if (g.dev) {
        te::load::Timed timed(te::load::kTexRelease);
        OnRender([handle] { ps2e_draw_texture_destroy(g.dev, handle); });
    }
    g.textures.erase(it);
    TextureChanged(texture);
}

// ---- culling, models, sky, shadows: graphics_scene.cpp ---------------------------------------------------------------
// ---- screen models, particles, decals ------------------------------------------------------------------------------------
// (DrawSkin / DrawBlendSkin: graphics_scene.cpp; the screen models -- BeginScreenModel .. DrawScreenModel, the skid
// marks: graphics_screen_models.cpp)

// (the particles: graphics_particles.cpp; the decals -- LoadDecalView / LoadDecalType / AgeDecal / DrawDecals /
// InitDecalBlock / InitDecalUvPacket: graphics_decals.cpp)

}  // extern "C"

// ---- the movie owner's hooks (movie_player.cpp; declared in te_graphics_hooks.h) ------------------------------------
// The device the movie's picture texture is created on (on demand, as every other draw's), and the picture's quad
// into the movies bucket (27, where the PS2 player's sprites went): corners in FRAME pixels (the game's Movie::Area is
// in display pixels, no GS origin), u/v fractions, the texture's colours as they are (TFX decal, no blending, in front
// of everything and written -- gsvalues.h TexturedSprite in white), bilinear.
extern "C" ps2e_draw* te_graphics_device(void) { return EnsureDevice() ? g.dev : nullptr; }
// the CT16 present toggled on the device (on the render thread when there is one); an engine without the CT16
// scanout keeps the request and reports 0 (the frame stays 32-bit)
extern "C" int te_graphics_set_present_ct16(int on) {
    g.present_ct16 = on != 0;
    if (!EnsureDevice()) return 1;
    OnRender([] { ps2e_draw_set_present_ct16(g.dev, g.present_ct16 ? 1 : 0, 0); });
    static bool said = false;
    if (!said) { said = true; std::fprintf(stderr, "[te.gfx] present: %s\n", g.present_ct16 ? "CT16 (the retail dithered 16-bit copy)" : "the frame's colours"); }
    return 1;
}
extern "C" int te_graphics_present_ct16(void) { return g.present_ct16 ? 1 : 0; }
extern "C" void te_graphics_on_render(void (*fn)(void* user), void* user) { OnRender([fn, user] { fn(user); }); }
extern "C" void te_graphics_post_render(void (*fn)(void* user), void* user) { PostRender([fn, user] { fn(user); }); }
extern "C" void te_graphics_record_movie_quad(uint32_t texture, float x0, float y0, float x1, float y1, float u0, float v0,
                                              float u1, float v1) {
    ps2e_draw_state st{};
    st.texture = texture;
    st.gouraud = 0;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = 1;
    st.blend = 0;
    st.tfx = PS2E_DRAW_TFX_DECAL;
    st.tcc = 1;
    st.linear = 1;
    st.wrap_u = PS2E_DRAW_WRAP_CLAMP;
    st.wrap_v = PS2E_DRAW_WRAP_CLAMP;
    Draw d;
    d.state = st;
    const uint32_t white = 0x80FFFFFFu;
    d.verts = {Vertex(x0, y0, u0, v0, white), Vertex(x1, y0, u1, v0, white), Vertex(x0, y1, u0, v1, white),
               Vertex(x1, y0, u1, v0, white), Vertex(x1, y1, u1, v1, white), Vertex(x0, y1, u0, v1, white)};
    ::Record(TE_GFX_BUCKET_MOVIES, std::move(d));
    g.movie_picture = true;   // this frame is a movie picture: never a picture tick of the tick lock (te_tick_lock.h)
}
