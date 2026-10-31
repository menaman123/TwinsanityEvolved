// engine/platform/ps2e/ps2e_draw.cpp -- ps2e_draw over Window + the render::Backend the arm supplies.
//
// The surface names no graphics API: the backend behind it is whatever ps2e_draw_backend.h's selected arm creates
// (ps2e_draw_backend_gl.cpp: a GL 4.1 context on the window + GlDevice; ps2e_draw_backend_null.cpp: nothing
// rasterised), and every operation here -- surfaces, uploads, draws, the shadow-volume stencil counter, the frame
// copy, the scanout, the presented-image capture -- is an entry of engine/render/backend.h.
//
// Each submitted draw becomes ONE DrawIr (the record the backend already lowers) whose raster state is the frame
// target's (FRAME at 0, the frame's width, PSMCT32; ZBUF a Z32 buffer; SCISSOR the frame or the state's) and whose
// vertices are the producer's triangle list in frame pixels (DrawIr vertices are post-XYOFFSET pixels, so off_x/off_y
// are 0). A textured draw samples a Decoded, owned TextureSource (the texture the producer created) in STQ mode with
// the texel-space prescale set to the texture's size, so u/v fractions reach the shader as the GS's S/T would.
#include "engine/platform/ps2e/ps2e_draw.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "engine/gs/format/pixel_convert.h"
#include "engine/platform/host/platform.h"
#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/texsrc/texture_source.h"
#include "engine/platform/ps2e/ps2e_draw_backend.h"
#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backend.h"
#include "engine/render/mesh_index.h"

using namespace ps2r;

struct ps2e_draw {
    platform::Window window;
    platform::GlContext context;             // created by the arm that needs one (ps2e_draw_backend_gl.cpp); else uncreated
    std::unique_ptr<render::Backend> device;
    render::SurfaceId color, depth;
    render::CdCopyState cd_copy;
    int frame_w = 0, frame_h = 0;         // the LOGICAL frame (the title's pixels); physical = logical * scale
    int scale = 1;                        // the frame target's internal scale (ps2e_draw_presentation.scale)
    int drawable_w = 0, drawable_h = 0;   // the window's drawable size as the pump last saw it (the window's thread)
    // How the frame is shown. `wanted` is what the producer last set (any thread, under the mutex); the pump
    // applies the window half (mode, size) on the window's thread, the present the rest on the drawing thread.
    std::mutex presentation_mutex;
    ps2e_draw_presentation wanted{};    // all zero = the plain picture (the frame's pixels letterboxed, linear, windowed, scale 1)
    ps2e_draw_presentation applied{};
    int applied_window_mode = PS2E_DRAW_WINDOWED, applied_window_scale = 0;   // the pump's half
    int last_out_x = 0, last_out_y = 0, last_out_w = 0, last_out_h = 0;        // the last present's output rectangle
    bool headless = false;
    bool stencil_diag = false;   // ps2e_draw_config.stencil_diag
    // the GPU phase timer (ps2e_draw_set_gpu_timers): on, and the phase the producer named last (the device's own marks
    // -- the stencil passes, the copies -- hand back to it)
    bool gpu_timers = false;
    int producer_phase = -1;
    // the shadow bracket's region (ps2e_draw_shadow_region): the next bracket's rectangles (frame pixels, exclusive), and
    // the open bracket's -- its clear and mark were made over them, its composite is drawn over them
    std::vector<int> next_shadow_rects, bracket_rects;
    bool next_shadow_region = false, bracket_region = false;
    std::string renderer;         // the backend's GPU / driver name (ps2e_draw_renderer)
    int target_logged_scale = 1;  // the frame target's scale the log last named (MakeFrameTarget)
    bool early_casters = false;  // this frame's casters marked before its translucent draws (kEarlyCasterBit)
    bool in_frame = false;
    bool capture = false;
    bool present_ct16 = false;   // ps2e_draw_set_present_ct16
    uint64_t dimx = render::kDefaultDimx;
    uint64_t presented = 0;
    // the next texture id: atomic -- ps2e_draw_texture_reserve takes ids from any thread; the drawing thread's creations
    // take theirs from the same counter, so a reserving producer keeps the ids a creating one had
    std::atomic<uint32_t> next_texture{1};
    // the producer's resolver of reserved textures not made yet (ps2e_draw_set_texture_resolver; the drawing thread)
    ps2e_draw_texture_resolver texture_resolver = nullptr;
    void* texture_resolver_user = nullptr;
    struct Texture {
        render::SurfaceId surface;
        int w = 0, h = 0;
        int levels = 0;   // the levels below the first (ps2e_draw_texture_set_mips)
        uint8_t max_alpha = 0;   // the largest PS2 alpha uploaded into it (the Vita's casters' re-stamp)
        // the smallest GPU alpha byte any of its texels / levels was given (the Vita's alpha-test fold: ps2e_draw_alpha_floor);
        // 0 = not known (a texture handed over in GPU alpha, or nothing uploaded yet)
        uint8_t min_gpu_alpha = 0;
        bool alpha_seen = false;
        uint64_t bytes = 0, vita_bytes = 0;   // its texels as held here / as the Vita's GL library allocates them (the census)
    };
    std::unordered_map<uint32_t, Texture> textures;
    // the textures' memory (ps2e_draw_texture_memory_census): the sums over `textures`, the Vita figure's peak, and the
    // creations / destructions since create
    uint64_t texture_bytes = 0, texture_vita_bytes = 0, texture_vita_peak = 0;
    uint64_t textures_made = 0, textures_made_vita_bytes = 0, textures_destroyed = 0, textures_destroyed_vita_bytes = 0;
    // the last lookups of `textures`, direct-mapped by id (an element's address holds until it is erased; the erase drops
    // its slot): a draw finds its texture without the map's division (software on the Vita's CPU)
    // (512 slots, an id and its record side by side -- one cache line per lookup; 64 found a hub frame's
    // textures mostly in the map)
    static constexpr uint32_t kTextureSlots = 512;
    struct TextureSlot {
        uint32_t id = 0;
        Texture* ptr = nullptr;
    };
    TextureSlot texture_slots[kTextureSlots] = {};
    uint64_t texture_lookups = 0, texture_misses = 0;   // (the census: PrepareRecord / FindTexture)
    // Movie textures (ps2e_draw_movie_*): ids kMovieIdBase + n (above every texture id, below the frame copy's), each slot's
    // planes (Y U V, or one RGBA image) as CPU-writable surfaces, the slot the draws show
    static constexpr uint32_t kMovieIdBase = 0x40000000u, kMovieIdMask = 0xC0000000u;
    struct Movie {
        int w = 0, h = 0, format = 0, slots = 0, planes_per_slot = 1;
        std::vector<render::SurfaceId> planes;   // slot-major
        int current = -1;
    };
    std::unordered_map<uint32_t, Movie> movies;
    uint32_t next_movie = 0;
    uint64_t movie_drawn = 0, movie_refused = 0;
    // The screen copy (ps2e_draw_copy_frame): a texture of the frame target's physical size, re-made with it; valid
    // once a copy has been taken this frame (PS2E_DRAW_TEXTURE_FRAME_COPY draws before that are refused)
    Texture frame_copy;
    bool frame_copy_valid = false;
    // ps2e_draw_expect_frame_copies: the copies announced for the frame about to begin, and this frame's copies (a frame
    // whose copies were not announced makes the next frame keep its depth anyway)
    int expect_copies = 0;
    int frame_copies = 0;
    bool frame_announced = false;
    ps2e_draw_counters counters{};
    std::vector<uint8_t> scratch;
    gs::DrawIr ir;   // the submit's record, kept so its vertex storage is reused across draws (no allocation per draw)
    // the last mesh draw's state while the next draw may be another mesh under it (the device's DrawMeshAgain): valid
    // while no other draw came between (the draw count it left)
    bool mesh_run = false;
    ps2e_draw_state mesh_run_state{};
    uint64_t mesh_run_draws = 0;
    int texture_swap = 2;   // ps2e_draw_set_mesh_texture_swap: 1 a mesh draw whose state differs from the run's in the texture only,
                            // 2 (the default) in the texture, the alpha test and the mipmapping; 0 none
    // The meshes' model-space bounds (the sky's wait asks each mesh draw's depth: ps2e_draw_mesh_depth_far_above; the Vita's
    // casters, draw order and alpha-test fold read them too)
    struct MeshBounds {
        uint8_t a = 0, la = 0;           // its vertices' largest alpha, unlit and lit
        uint8_t min_a = 0, min_la = 0;   // ... and their smallest (the alpha-test fold: ps2e_draw_alpha_floor)
        float lo[3] = {0.f, 0.f, 0.f}, hi[3] = {0.f, 0.f, 0.f};   // its model-space box
        bool skin = false;               // a skin mesh: its bind-pose box says nothing of where it is drawn
        bool convex = false;             // a skin mesh: every vertex's weights are >= 0 and sum to 1 (its skinned position lies in
                                         // the hull of its joints' images of the bind box: ps2e_draw_skin_depth_far)
    };
    std::unordered_map<uint32_t, MeshBounds> mesh_bounds;
    // the last mesh_bounds lookups, direct-mapped by mesh id (an element's address holds until it is erased; the erase drops
    // its slot): every mesh draw's caster check found its bounds without the map's division (software on the Vita's CPU)
    // (2048 slots, an id and its record side by side; 256 missed most of a hub frame's several hundred meshes)
    static constexpr uint32_t kBoundSlots = 2048;
    struct BoundSlot {
        uint32_t id = 0;
        MeshBounds* ptr = nullptr;
    };
    BoundSlot bound_slots[kBoundSlots] = {};
    uint64_t bound_lookups = 0, bound_misses = 0;   // (the census: FindBounds)
#if PS2R_HOST_VITA
    // The casters' re-stamp (see ResetCasterCounts): the frame's draws that can leave a PS2 alpha of 0x80 or more, kept
    // from the frame's start as their replay states and screen boxes, drawn again at each shadow composite where a
    // volume of that bracket was counted
    struct ScreenBox {
        float x0, y0, x1, y1;   // frame pixels
    };
    struct CasterDraw {
        ps2e_draw_state state;           // the replay's state (ReplayState)
        ScreenBox box;                   // where it drew (a mesh's: its bounds through its matrix)
        uint32_t mesh = 0;               // a mesh draw: its handle and parameters
        ps2e_draw_mesh_params mp{};
        const float* bones = nullptr;    // a skin mesh draw: its palette (valid until the frame is presented) and joints
        int joints = 0;
        const float* disp = nullptr;     // ... and a blend skin part's displacements
        int disp_count = 0;
        const ps2e_draw_vertex* verts = nullptr;   // a vertex draw: the producer's vertices (retained) ...
        uint32_t first = 0, count = 0;   // ... or their copy at `first` in caster_verts
        // a vertex draw: the extent of the vertices the composite's near test reads (every 4th: 0, 4, 8, ...) --
        // outside the padded union none of them is in it, inside it all are: the scan only between (ResetCasterCounts)
        ScreenBox box4{1e30f, 1e30f, -1e30f, -1e30f};
    };
    std::vector<CasterDraw> casters;
    std::vector<ps2e_draw_vertex> caster_verts;
    // THE MESH DRAW PACKETS (render::Backend::TakeMeshPacket / DrawMeshPacket; TE_GFX_VITA_PACKETS=0: none): the packet the
    // backend last recorded for a (state, lit), direct-mapped by the state's hash. A mesh draw that would take the again /
    // texture-swap path (its GL context is the run's) is drawn from its packet when there is one; mesh_run_state stays the
    // state the backend's GL run was set up for (the slow paths' reference), whatever packets were drawn since.
    struct PacketEntry {
        ps2e_draw_state state;
        uint32_t packet = 0;   // 0: empty
        uint8_t lit = 0;
    };
    static constexpr uint32_t kPacketSlots = 1024;
    std::vector<PacketEntry> packet_cache;   // kPacketSlots entries from the first packet kept
    // the cache as 512 sets of 2 ways (entries 2s, 2s + 1; was direct-mapped: the hub's ~130 states a frame met
    // ~8 collisions a frame -- "no packet kept" -- each a slow draw), the set's most recently used way
    std::vector<uint8_t> packet_mru;
    // the GPU-skin draw packets (render::Backend::TakeSkinPacket / DrawSkinPacket), kept per state as the mesh packets
    static constexpr uint32_t kSkinPacketSlots = 256;
    std::vector<PacketEntry> skin_packet_cache;
    uint64_t skin_packets_missing = 0;       // skin draws of the run's context with no packet kept for their state
    bool packets = true;
    uint64_t packets_missing = 0;            // again / swap draws with no packet cached for their state (the census)
    uint64_t packet_repeats = 0;             // packet draws of the previous draw's packet and mesh range (the census)
    uint32_t last_packet = 0, last_packet_mesh = 0, last_packet_first = 0, last_packet_count = 0;
    uint64_t last_packet_draws = 0;          // counters.draws after that packet draw
    // THE PACKET MEMO: the last mesh draw drawn from a packet through the cache's choice -- its state, lit and
    // packet -- valid while what that choice read is unchanged: the run's state and the packet cache (run_gen, bumped at
    // every change of either). A mesh draw of the very same state (byte for byte) and lit while the run's packets are
    // drawable makes the same choice: the memo's packet, without the run-state compares, the probe and the cache lookup.
    bool pk_memo = false;
    ps2e_draw_state pk_memo_state{};
    uint8_t pk_memo_lit = 0;
    uint32_t pk_memo_packet = 0;
    uint64_t pk_memo_gen = 0, run_gen = 1;
    uint64_t pk_memo_hits = 0;               // (the census)
    static constexpr int kMaxInstances = 64;  // ps2e_draw_submit_mesh_instances: instances per instanced draw (the device's)
    // a vertex draw of the run's own state (the same GL context: the executor sets the context from the state) leaves the
    // run's packets drawable: counters.draws after such a draw (the backend checks its context field for field)
    uint64_t packet_run_draws = ~0ull;
    uint64_t full_reasons[5] = {0, 0, 0, 0, 0};   // ps2e_draw_mesh_full_census
    uint64_t caster_mesh_replays = 0;
    std::vector<ScreenBox> volume_boxes;   // the current bracket's counting draws (from its clear to its composite)
    bool volume_boxes_given = false;       // ps2e_draw_set_volume_boxes_given: the producer notes them (grouped volumes)
    const ps2e_draw_vertex* q_finite = nullptr;   // ps2e_draw_submit_finite: these vertices' Qs are known finite
    bool retained = false;        // ps2e_draw_set_retained_vertices
    bool far_background = false;  // ps2e_draw_set_far_background: the moved sky (never a caster)
    bool record_casters = true;   // TE_GFX_VITA_CASTERS=0 (or TE_GFX_VITA_STENCIL=0): no re-stamp
    bool shadow_fix = true;       // TE_GFX_VITA_SHADOW_FIX=0: the composite's FIX left to the backend's source-alpha stand-in
    bool replaying = false;
    std::vector<ps2e_draw_vertex> fix_scratch;
    long census_every = 0;        // TE_GFX_CASTER_CENSUS=N: a census line every N brackets / presents
    uint64_t census_brackets = 0, frame_draws = 0, frame_fix = 0;
    uint64_t frame_fix_blends = 0, frame_fix_blends_textured = 0;   // every GS FIX blend (the backend's source-alpha stand-in)
    uint32_t frame_restamps = 0;  // the casters drawn again this frame (ResetCasterCounts; the budget kMaxRestamps)
    size_t max_casters = 4096;    // the casters a frame keeps (CasterRoom; TE_GFX_VITA_CASTERS_KEEP)
#endif
};

namespace {

// A texture's record by id through the direct-mapped slots (an element's address holds until it is erased; the erase
// drops its slot), the map on a miss -- which then fills the slot (null: no such texture)
ps2e_draw::Texture* FindTextureSlot(ps2e_draw* d, uint32_t id) {
    ps2e_draw::TextureSlot& slot = d->texture_slots[id & (ps2e_draw::kTextureSlots - 1)];
    d->texture_lookups++;
    if (id && slot.id == id) return slot.ptr;
    d->texture_misses++;
    auto it = d->textures.find(id);
    if (it == d->textures.end()) {
        // a reserved id not made yet (ps2e_draw_texture_reserve): the producer makes it now, or says there is none
        if (!id || !d->texture_resolver || id >= d->next_texture.load(std::memory_order_acquire) ||
            !d->texture_resolver(d->texture_resolver_user, id))
            return nullptr;
        it = d->textures.find(id);
        if (it == d->textures.end()) return nullptr;
    }
    slot.id = id;
    slot.ptr = &it->second;
    return &it->second;
}

constexpr uint8_t kStencilOrigin = 15;
constexpr uint8_t kCasterBit = 0x20;   // the stencil bit above the 5-bit counter: a caster pixel (SeedCasterMask)
constexpr uint8_t kCounterMask = 0x1F; // the 5-bit counter, wrapping as the GS byte did
constexpr uint8_t kInsideBit = 0x10;   // bit 4 set <=> the count is 1..16
constexpr uint8_t kEarlyCasterBit = 0x40;   // a caster before the translucent draws (ps2e_draw_mark_casters_early)

// The stencil test around one counting or composite draw (the GS tested the volumes' NEGATED z against the scene
// depth's byte-NOT copy -- a fragment counts where the volume face is AT OR BEHIND the scene surface, i.e. where the
// ordinary depth test FAILS (the volume's z against the frame's depth, GEQUAL); the composite reads bit 4).
render::StencilState VolumeStencil(const ps2e_draw_state* st) {
    render::StencilState s;
    s.enabled = true;
    if (st->stencil == PS2E_DRAW_STENCIL_INSIDE) {
        s.func = render::StencilState::kEqual;
        s.ref = kInsideBit;
        s.cmp_mask = kInsideBit;
        s.write_mask = 0;
        return s;
    }
    // dest_alpha_test: the pixels the clear marked as casters (or the early mark did) fail the test and keep their count
    if (st->dest_alpha_test) {
        s.func = render::StencilState::kEqual;
        s.ref = 0;
        s.cmp_mask = kCasterBit | kEarlyCasterBit;
    } else {
        s.func = render::StencilState::kAlways;
        s.ref = 0;
        s.cmp_mask = 0xFF;
    }
    s.write_mask = kCounterMask;
    s.depth_fail_op = st->stencil == PS2E_DRAW_STENCIL_COUNT_UP ? render::StencilState::kIncrWrap
                                                                 : render::StencilState::kDecrWrap;
    return s;
}

// ps2e_draw_config.stencil_diag: the stencil's content read back at the shadow bracket's points (the clear, the
// composite) -- a histogram of the counter's values and the share with bit 4 set (the composite's test). The consumer
// decides how the toggle is set (the port: PS2E_DRAW_STENCIL_DIAG); the engine reads no environment outside the config
// loader (CoreSingleGetenvSite). A backend that cannot read its stencil reports that and nothing else.
void StencilProbe(ps2e_draw* d, const char* tag) {
    if (!d->stencil_diag) return;
    const int w = d->frame_w * d->scale, h = d->frame_h * d->scale;
    static std::vector<uint8_t> buf;
    buf.assign(size_t(w) * size_t(h), 0xEE);
    if (!d->device->ReadbackStencil(d->depth, render::Rect{0, 0, w, h}, buf.data())) {
        std::fprintf(stderr, "[ps2e.stencil] present %llu %s: no stencil readback\n", (unsigned long long)d->presented, tag);
        return;
    }
    size_t hist[256] = {}, hist0[256] = {};
    for (uint8_t v : buf) hist[v]++;
    std::memcpy(hist0, hist, sizeof hist);
    size_t inside = 0;
    for (int v = 0; v < 256; ++v) if (v & 0x10) inside += hist[v];
    std::fprintf(stderr, "[ps2e.stencil] present %llu %s: inside %.3f origin %.3f top:", (unsigned long long)d->presented, tag,
                 double(inside) / buf.size(), double(hist[kStencilOrigin]) / buf.size());
    for (int n = 0; n < 4; ++n) {
        int best = -1;
        for (int v = 0; v < 256; ++v) if (hist[v] && (best < 0 || hist[v] > hist[best])) best = v;
        if (best < 0) break;
        std::fprintf(stderr, " %d:%.3f", best, double(hist[best]) / buf.size());
        hist[best] = 0;
    }
    size_t masked = 0;
    for (int v = 0; v < 256; ++v) if (v & kCasterBit) masked += hist0[v];
    std::fprintf(stderr, " casters %.3f\n", double(masked) / buf.size());
    // The frame's alpha (a caster is a pixel whose PS2 alpha is >= 0x80 -- the GPU's 0xFF after the x2 scale): the
    // share of such pixels per 64-row band (top row first) and the most common alpha bytes
    static std::vector<uint8_t> px;
    px.assign(size_t(w) * size_t(h) * 4, 0);
    d->device->Readback(d->color, render::Rect{0, 0, w, h}, px.data(), size_t(w) * 4);
    size_t ahist[256] = {};
    for (size_t i = 3; i < px.size(); i += 4) ahist[px[i]]++;
    std::fprintf(stderr, "[ps2e.stencil] present %llu %s: frame alpha 0xff per band", (unsigned long long)d->presented, tag);
    for (int band = 0; band < h; band += 64) {
        size_t bandSet = 0, n = 0;
        for (int y = band; y < std::min(h, band + 64); ++y)
            for (int x = 0; x < w; ++x) {
                bandSet += px[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] == 0xFF ? 1 : 0;   // rows bottom-up: a share per band either way
                n++;
            }
        std::fprintf(stderr, " %.2f", n ? double(bandSet) / n : 0.0);
    }
    std::fprintf(stderr, " (all %.3f) alpha top:", double(ahist[0xFF]) / (size_t(w) * size_t(h)));
    for (int n = 0; n < 6; ++n) {
        int best = -1;
        for (int v = 0; v < 256; ++v) if (ahist[v] && (best < 0 || ahist[v] > ahist[best])) best = v;
        if (best < 0) break;
        std::fprintf(stderr, " %#x:%.3f", best, double(ahist[best]) / (size_t(w) * size_t(h)));
        ahist[best] = 0;
    }
    std::fprintf(stderr, "\n");
}

// StartShadows' SeedAlpha: the frame's alpha, shrunk into the half-size buffer, was the volume strips' DATE
// source -- a strip counted only where the frame's alpha top bit was 0, so the casters (Crash's skin: alpha 0x80, or a
// material with FBA) were never darkened while the receivers (the lit world: alpha 0x7E) were. The frame's alpha here is
// the backend's GPU-scaled byte (PS2 0x80 -> 0xFF, 0x7E -> 0xFB), so the bit is `alpha == 0xFF`; the backend marks it
// as stencil bit kCasterBit over the whole frame right after the counter's clear (Backend::MarkStencilWhereOpaque), and
// a counting draw with dest_alpha_test skips the pixels that carry it (TEST.DATE with DATM 0). False when the backend
// cannot (the Vita: the submit re-stamps the casters at the composite instead, ResetCasterCounts).
bool SeedCasterMask(ps2e_draw* d) { return d->device->MarkStencilWhereOpaque(d->color, d->depth, kCasterBit); }

// The GPU phase timer's marks (no-ops while it is off): the device's own phases, and the producer's phase handed back
inline void TimerPhase(ps2e_draw* d, int phase) {
    if (d->gpu_timers) d->device->MarkGpuPhase(phase);
}
inline void TimerFill(ps2e_draw* d, int phase, double pixels, uint32_t count = 1) {
    if (d->gpu_timers) d->device->NoteGpuFill(phase, pixels, count);
}
inline void TimerResume(ps2e_draw* d) {
    if (d->gpu_timers && d->producer_phase >= 0) d->device->MarkGpuPhase(d->producer_phase);
}
// A mesh's bounds through the direct-mapped slots, the map on a miss -- which then fills the slot (null: none kept)
const ps2e_draw::MeshBounds* FindBounds(ps2e_draw* d, uint32_t mesh) {
    ps2e_draw::BoundSlot& s = d->bound_slots[mesh & (ps2e_draw::kBoundSlots - 1)];
    d->bound_lookups++;
    if (mesh && s.id == mesh) return s.ptr;
    d->bound_misses++;
    auto it = d->mesh_bounds.find(mesh);
    if (it == d->mesh_bounds.end()) return nullptr;
    s.id = mesh;
    s.ptr = &it->second;
    return &it->second;
}
inline double TargetPixels(const ps2e_draw* d) {
    return double(d->frame_w) * d->scale * double(d->frame_h) * d->scale;
}
#if !PS2R_HOST_VITA
// frame-pixel rectangles (x0, y0, x1, y1, exclusive) as the target's physical texels (the GS's rows are the target's: no
// flip), clipped to the frame; their area in physical pixels (the shadow regions': the Vita takes none)
std::vector<render::Rect> PhysicalRects(const ps2e_draw* d, const int* r, int n, double* area) {
    std::vector<render::Rect> out;
    out.reserve(size_t(std::max(0, n)));
    double a = 0;
    for (int i = 0; i < n; ++i) {
        const int x0 = std::clamp(r[4 * i], 0, d->frame_w), y0 = std::clamp(r[4 * i + 1], 0, d->frame_h);
        const int x1 = std::clamp(r[4 * i + 2], 0, d->frame_w), y1 = std::clamp(r[4 * i + 3], 0, d->frame_h);
        if (x1 <= x0 || y1 <= y0) continue;
        out.push_back(render::Rect{x0 * d->scale, y0 * d->scale, (x1 - x0) * d->scale, (y1 - y0) * d->scale});
        a += double(out.back().w) * out.back().h;
    }
    if (area) *area = a;
    return out;
}
#endif
// the area a triangle list rasterises (frame pixels; the GS's pixels, each scale^2 of the target's)
double TriangleArea(const ps2e_draw_vertex* v, int count) {
    double a = 0;
    for (int i = 0; i + 2 < count; i += 3)
        a += std::fabs(double(v[i + 1].x - v[i].x) * double(v[i + 2].y - v[i].y) -
                       double(v[i + 2].x - v[i].x) * double(v[i + 1].y - v[i].y));
    return a * 0.5;
}

bool MakeFrameTarget(ps2e_draw& d, int w, int h) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    const int scale = std::clamp(d.scale, 1, 4);
    if (uint64_t(w) * scale > 8192 || uint64_t(h) * scale > 8192) return false;
    if (d.color.id) d.device->DestroySurface(d.color);
    if (d.depth.id) d.device->DestroySurface(d.depth);
    render::SurfaceDesc cd;
    cd.width = uint32_t(w * scale);     // physical texels (the backend rasterises through the viewport at the scale)
    cd.height = uint32_t(h * scale);
    cd.scale = uint8_t(scale);
    cd.format = render::SurfaceFormat::Rgba8;
    cd.renderable = true;
    // Depth + stencil in one attachment: the stencil is the shadow volumes' counter (ps2e_draw_stencil); GL binds a
    // depth-only texture and a stencil renderbuffer to one framebuffer only as the same image.
    render::SurfaceDesc dd = cd;
    dd.format = render::SurfaceFormat::DepthStencil24_8;
    dd.renderable = false;
    d.color = d.device->CreateSurface(cd);
    d.depth = d.device->CreateSurface(dd);
    if (d.frame_copy.surface.id) d.device->DestroySurface(d.frame_copy.surface);
    render::SurfaceDesc fc = cd;
    fc.renderable = false;
    d.frame_copy = ps2e_draw::Texture{d.device->CreateSurface(fc), int(cd.width), int(cd.height)};
    if (d.frame_copy.surface.id) {   // the storage specified whole (a pooled texture object may hold another size)
        std::vector<uint8_t> zero(size_t(cd.width) * cd.height * 4, 0);
        d.device->Upload(d.frame_copy.surface, render::Rect{0, 0, int(cd.width), int(cd.height)}, zero.data(), size_t(cd.width) * 4);
    }
    d.frame_copy_valid = false;
    d.cd_copy = render::CdCopyState{};
    d.frame_w = w;
    d.frame_h = h;
    d.scale = scale;
    if (scale != d.target_logged_scale) {
        // a log reader sees the pixels the frame costs (the user's log: 2048x2048 at 4x)
        std::fprintf(stderr, "[ps2e_draw] frame target %ux%u (the frame %dx%d at %dx)\n", cd.width, cd.height, w, h, scale);
        d.target_logged_scale = scale;
    }
    if (!d.color.id || !d.depth.id) return false;
    d.device->Clear(d.color, d.depth, 0x80000000u, 0.0f, render::kClearColor | render::kClearDepth);
    return true;
}

// lround(log2(w)) for a texture side (1..4096) in integers: floor(log2 w), plus one when w >= 2^k * sqrt(2) (w^2 >=
// 2^(2k+1)) -- the same value for every w (checked exhaustively up to 2^20), without two libm calls per draw
uint32_t RoundedLog2(uint32_t w) {
    if (w <= 1) return 0;
    uint32_t k = 31u - uint32_t(__builtin_clz(w));
    if (uint64_t(w) * w >= (uint64_t(1) << (2 * k + 1))) ++k;
    return k;
}

// PS2 alpha bytes -> the GPU alpha the backend's samplers expect (the decoder's one scaling site).
void ScaleAlpha(const uint8_t* in, size_t stride_in, int w, int h, std::vector<uint8_t>& out) {
    out.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = in + size_t(y) * stride_in;
        uint8_t* dst = out.data() + size_t(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = row[x * 4 + 0];
            dst[x * 4 + 1] = row[x * 4 + 1];
            dst[x * 4 + 2] = row[x * 4 + 2];
            dst[x * 4 + 3] = gs::PixelConvert::ScaleAlphaPs2ToGpu(row[x * 4 + 3]);
        }
    }
}

#if PS2R_HOST_VITA
// The largest PS2 alpha byte of an RGBA8 rectangle (the casters' re-stamp: a texture whose alpha never reaches 0x80
// cannot make a caster pixel under DECAL / MODULATE)
uint8_t MaxAlpha(const uint8_t* in, size_t stride_in, int w, int h) {
    uint8_t m = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = in + size_t(y) * stride_in;
        for (int x = 0; x < w; ++x) m = std::max(m, row[x * 4 + 3]);
    }
    return m;
}
// The smallest alpha byte of an RGBA8 rectangle (as it is: the caller converts the scale)
uint8_t MinAlpha(const uint8_t* in, size_t stride_in, int w, int h) {
    uint8_t m = 0xFF;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = in + size_t(y) * stride_in;
        for (int x = 0; x < w; ++x) m = std::min(m, row[x * 4 + 3]);
    }
    return m;
}
#endif

}  // namespace

extern "C" {

ps2e_draw* ps2e_draw_create(const ps2e_draw_config* config) {
    if (!config || config->frame_width <= 0 || config->frame_height <= 0) return nullptr;
    auto d = std::make_unique<ps2e_draw>();
    d->headless = config->headless != 0;
    d->stencil_diag = config->stencil_diag != 0;
#if PS2R_HOST_VITA
    {
        const auto off = [](const char* name) { const char* e = std::getenv(name); return e && e[0] == '0'; };
        d->record_casters = !off("TE_GFX_VITA_CASTERS") && !off("TE_GFX_VITA_STENCIL");
        d->shadow_fix = !off("TE_GFX_VITA_SHADOW_FIX");
        d->packets = !off("TE_GFX_VITA_PACKETS");   // (=abN: the backend switches them in turns)
        if (const char* e = std::getenv("TE_GFX_CASTER_CENSUS")) d->census_every = std::atol(e);
        if (const char* e = std::getenv("TE_GFX_VITA_CASTERS_KEEP"))
            if (std::atol(e) > 0) d->max_casters = size_t(std::min(std::atol(e), 65536L));
        std::fprintf(stderr, "[ps2e] shadows on the Vita: casters %s (at most %u kept a frame), the composite's FIX %s\n",
                     d->record_casters ? "re-stamped at the clear" : "not marked (TE_GFX_VITA_CASTERS=0)", unsigned(d->max_casters),
                     d->shadow_fix ? "through the source alpha" : "left to the backend (TE_GFX_VITA_SHADOW_FIX=0)");
    }
#endif
    const int ww = config->window_width > 0 ? config->window_width : config->frame_width;
    const int wh = config->window_height > 0 ? config->window_height : config->frame_height;
    if (!d->window.Create(d->headless, ww, wh, config->title ? config->title : "ps2e_draw")) return nullptr;
    // the depth mapping: a Z24 producer's Z onto the attachment one step per unit (ps2e_draw_config::depth_bits)
    const bool gs_z24 = config->depth_bits == 24;
    d->device = ps2e::CreateDrawBackend(d->window, d->context, gs_z24);
    if (!d->device) return nullptr;
    d->renderer = d->device->DeviceName();
    std::fprintf(stderr, "[ps2e_draw] depth: GS Z%s -> window depth z / 2^%s\n", gs_z24 ? "24" : "32",
                 gs_z24 ? "24 (every Z unit kept)" : "32");
    // The device's window size is the frame's when headless (the hidden window is 64x64: the capture must be the
    // frame's size so a judge sees the frame, not a thumbnail).
    d->device->SetWindowSize(d->headless ? config->frame_width : d->window.Width(),
                             d->headless ? config->frame_height : d->window.Height());
    if (!MakeFrameTarget(*d, config->frame_width, config->frame_height)) return nullptr;
    if (!d->headless) SDL_GL_GetDrawableSize(d->window.Handle(), &d->drawable_w, &d->drawable_h);   // the window's thread
    return d.release();
}

void ps2e_draw_destroy(ps2e_draw* d) {
    if (!d) return;
    if (d->device) {
        for (auto& t : d->textures) d->device->DestroySurface(t.second.surface);
        if (d->frame_copy.surface.id) d->device->DestroySurface(d->frame_copy.surface);
        if (d->color.id) d->device->DestroySurface(d->color);
        if (d->depth.id) d->device->DestroySurface(d->depth);
        d->device.reset();   // the arm's device releases its objects (the context is still current)
    }
    d->context.Destroy();
    d->window.Destroy();
    delete d;
}

int ps2e_draw_pump(ps2e_draw* d) {
    if (!d) return 0;
    int quit = 0;
    // The window's half of the presentation (the window's thread): the mode and the windowed size
    if (!d->headless) {
        int mode, wscale;
        {
            std::lock_guard<std::mutex> lock(d->presentation_mutex);
            mode = d->wanted.window_mode;
            wscale = d->wanted.window_scale;
        }
        if (mode != d->applied_window_mode) {
            const Uint32 flags = mode == PS2E_DRAW_FULLSCREEN ? SDL_WINDOW_FULLSCREEN_DESKTOP : mode == PS2E_DRAW_BORDERLESS ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
            if (SDL_SetWindowFullscreen(d->window.Handle(), flags) == 0) {
                SDL_SetWindowBordered(d->window.Handle(), mode == PS2E_DRAW_BORDERLESS ? SDL_FALSE : SDL_TRUE);
                d->applied_window_mode = mode;
            } else {
                d->applied_window_mode = mode;   // refused by the window system: not retried every pump
            }
            int w = 0, h = 0;
            SDL_GL_GetDrawableSize(d->window.Handle(), &w, &h);
            if (w > 0 && h > 0) { d->drawable_w = w; d->drawable_h = h; }
            int pw = 0, ph = 0;
            SDL_GetWindowSize(d->window.Handle(), &pw, &ph);
            // (the pixels the present fills: a log reader sees a high-density backing store here)
            std::fprintf(stderr, "[ps2e_draw] window %s: drawable %dx%d pixels (window %dx%d points)\n",
                         mode == PS2E_DRAW_FULLSCREEN ? "fullscreen" : mode == PS2E_DRAW_BORDERLESS ? "borderless" : "windowed",
                         w, h, pw, ph);
        }
        if (wscale > 0 && wscale != d->applied_window_scale) {
            d->applied_window_scale = wscale;
            if (d->applied_window_mode == PS2E_DRAW_WINDOWED) {
                const int ws = std::clamp(wscale, 1, 8);
                // the window sized for the picture at the frame's height times the scale: the TV's 4:3 for a TV shape,
                // the frame's own size for the pixel shape, 16:9 / 16:10 / 21:9 as chosen
                int aspect_mode;
                { std::lock_guard<std::mutex> lock(d->presentation_mutex); aspect_mode = d->wanted.aspect; }
                double aspect = double(d->frame_w) / d->frame_h;
                if (aspect_mode == PS2E_DRAW_ASPECT_4_3) aspect = 4.0 / 3.0;
                else if (aspect_mode == PS2E_DRAW_ASPECT_16_9) aspect = 16.0 / 9.0;
                else if (aspect_mode == PS2E_DRAW_ASPECT_16_10) aspect = 16.0 / 10.0;
                else if (aspect_mode == PS2E_DRAW_ASPECT_21_9) aspect = 21.0 / 9.0;
                d->window.Resize(int(d->frame_h * ws * aspect + 0.5), d->frame_h * ws);
                int w = 0, h = 0;
                SDL_GL_GetDrawableSize(d->window.Handle(), &w, &h);
                if (w > 0 && h > 0) { d->drawable_w = w; d->drawable_h = h; }
            }
        }
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = 1;
        if (e.type == SDL_WINDOWEVENT) {
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) quit = 1;
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) {
                int w = 0, h = 0;
                SDL_GL_GetDrawableSize(d->window.Handle(), &w, &h);
                if (w > 0 && h > 0 && !d->headless) {
                    d->drawable_w = w;
                    d->drawable_h = h;
                }
            }
        }
    }
    // the size asked every pump as well: a maximize / restore / mode change whose event came before the window settled
    // (or never came) must not leave the picture laid out for the old size, anchored at the drawable's bottom-left
    if (!d->headless) {
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(d->window.Handle(), &w, &h);
        if (w > 0 && h > 0) {
            d->drawable_w = w;
            d->drawable_h = h;
        }
    }
    return quit;
}

int ps2e_draw_bind_thread(ps2e_draw* d, int bind) {
    if (!d || !d->context.IsCreated()) return 0;
#if PS2R_HOST_VITA
    // vitaGL has one context and no per-thread binding (SDL's Vita video has no GL to hand over): the frames may be drawn
    // on another thread as they are, one thread at a time -- the render thread's hand-over keeps it so
    (void)bind;
    return 1;
#endif
    if (bind) return d->context.MakeCurrent(d->window) ? 1 : 0;
    return SDL_GL_MakeCurrent(d->window.Handle(), nullptr) == 0 ? 1 : 0;
}

int ps2e_draw_resize_frame(ps2e_draw* d, int w, int h) {
    if (!d || d->in_frame) return 0;
    if (w == d->frame_w && h == d->frame_h) return 1;
    const bool ok = MakeFrameTarget(*d, w, h);
    if (ok && d->headless) d->device->SetWindowSize(w, h);
    return ok ? 1 : 0;
}

void ps2e_draw_frame_size(const ps2e_draw* d, int* w, int* h) {
    if (w) *w = d ? d->frame_w : 0;
    if (h) *h = d ? d->frame_h : 0;
}

namespace {
// A texture's texels as the Vita's GL library (vitaGL r1488) allocates them: level 0 alone in rows of 8 texels at least
// (gpu_alloc_texture); with levels, the whole chain down to 1x1 over the power-of-two size, rows of 8 at least
// (gpu_alloc_mipmaps, which replaces level 0's storage). 4 bytes a texel.
uint64_t VitaTextureBytes(int w, int h, int levels) {
    if (levels <= 0) return uint64_t((uint32_t(w) + 7u) & ~7u) * uint64_t(h) * 4u;
    auto po2 = [](uint32_t v) {
        uint32_t p = 1;
        while (p < v) p <<= 1;
        return p;
    };
    uint32_t pw = po2(uint32_t(w)), ph = po2(uint32_t(h));
    uint64_t size = 0;
    while (pw > 1 && ph > 1) {
        size += uint64_t(std::max(pw, 8u)) * ph * 4u;
        pw /= 2;
        ph /= 2;
    }
    return size + uint64_t(std::max(pw, 8u)) * ph * 4u;
}
// ... and as held here: level 0 and each level given, max(w >> i, 1) x max(h >> i, 1)
uint64_t HeldTextureBytes(int w, int h, int levels) {
    uint64_t size = uint64_t(w) * uint64_t(h) * 4u;
    for (int i = 1; i <= levels; ++i) size += uint64_t(std::max(w >> i, 1)) * uint64_t(std::max(h >> i, 1)) * 4u;
    return size;
}
// A texture's sizes into the device's sums (the census), and out of them
void CountTextureBytes(ps2e_draw* d, ps2e_draw::Texture& t, int levels) {
    d->texture_bytes -= t.bytes;
    d->texture_vita_bytes -= t.vita_bytes;
    t.bytes = HeldTextureBytes(t.w, t.h, levels);
    t.vita_bytes = VitaTextureBytes(t.w, t.h, levels);
    d->texture_bytes += t.bytes;
    d->texture_vita_bytes += t.vita_bytes;
    d->texture_vita_peak = std::max(d->texture_vita_peak, d->texture_vita_bytes);
}
// The texture's storage and record at `id` (an id already taken: the next one, or a reserved one), made with its image
// in GPU alpha bytes when `gpu_rgba8` is given (Backend::CreateTextureWithData: the surface and the whole image's upload,
// in one step where the backend can); false when the device made no surface
bool MakeTextureAt(ps2e_draw* d, uint32_t id, int w, int h, const void* gpu_rgba8 = nullptr) {
    render::SurfaceDesc td;
    td.width = uint32_t(w);
    td.height = uint32_t(h);
    td.format = render::SurfaceFormat::Rgba8;
    td.renderable = false;
    const render::SurfaceId s = gpu_rgba8 ? d->device->CreateTextureWithData(td, gpu_rgba8) : d->device->CreateSurface(td);
    if (!s.id) return false;
    ps2e_draw::Texture& t = d->textures[id] = ps2e_draw::Texture{s, w, h};
    d->counters.textures_alive++;
    CountTextureBytes(d, t, 0);
    d->textures_made++;
    d->textures_made_vita_bytes += t.vita_bytes;
    if (gpu_rgba8) {
        t.max_alpha = 0xFF;   // the GPU's scale: any texel may be a PS2 0x80
        t.min_gpu_alpha = 0;  // (and any may be 0: not known)
        t.alpha_seen = true;
    }
    return true;
}
}  // namespace

uint32_t ps2e_draw_texture_create(ps2e_draw* d, int w, int h, const void* rgba8) {
    if (!d || w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    const uint32_t id = d->next_texture.fetch_add(1, std::memory_order_acq_rel);
    if (!MakeTextureAt(d, id, w, h)) {
        // a failed creation takes no id (as before the reservations): given back unless one was taken since
        uint32_t expected = id + 1;
        d->next_texture.compare_exchange_strong(expected, id, std::memory_order_acq_rel);
        return 0;
    }
    if (rgba8) ps2e_draw_texture_upload(d, id, 0, 0, w, h, rgba8, size_t(w) * 4);
    return id;
}

uint32_t ps2e_draw_texture_create_gpu_alpha(ps2e_draw* d, int w, int h, const void* rgba8) {
    if (!rgba8) return ps2e_draw_texture_create(d, w, h, nullptr);
    if (!d || w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    const uint32_t id = d->next_texture.fetch_add(1, std::memory_order_acq_rel);
    if (!MakeTextureAt(d, id, w, h, rgba8)) {
        uint32_t expected = id + 1;   // (as ps2e_draw_texture_create: a failed creation takes no id)
        d->next_texture.compare_exchange_strong(expected, id, std::memory_order_acq_rel);
        return 0;
    }
    return id;
}

uint32_t ps2e_draw_texture_reserve(ps2e_draw* d) {
    if (!d) return 0;
    return d->next_texture.fetch_add(1, std::memory_order_acq_rel);
}

int ps2e_draw_texture_create_reserved_gpu_alpha(ps2e_draw* d, uint32_t id, int w, int h, const void* rgba8) {
    if (!d || !id || w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    if (id >= d->next_texture.load(std::memory_order_acquire) || d->textures.count(id)) return 0;   // (not reserved, or made)
    return MakeTextureAt(d, id, w, h, rgba8) ? 1 : 0;
}

void ps2e_draw_set_texture_resolver(ps2e_draw* d, ps2e_draw_texture_resolver resolve, void* user) {
    if (!d) return;
    d->texture_resolver = resolve;
    d->texture_resolver_user = user;
}

int ps2e_draw_texture_upload(ps2e_draw* d, uint32_t texture, int x, int y, int w, int h, const void* rgba8,
                             size_t stride) {
    if (!d || !rgba8 || w <= 0 || h <= 0) return 0;
    auto it = d->textures.find(texture);
    if (it == d->textures.end()) return 0;
    if (x < 0 || y < 0 || x + w > it->second.w || y + h > it->second.h) return 0;
    if (stride == 0) stride = size_t(w) * 4;
    ScaleAlpha(static_cast<const uint8_t*>(rgba8), stride, w, h, d->scratch);
#if PS2R_HOST_VITA
    it->second.max_alpha = std::max(it->second.max_alpha, MaxAlpha(static_cast<const uint8_t*>(rgba8), stride, w, h));
    {
        // the smallest GPU alpha it holds (a running minimum over the uploads: a lower bound of what it holds now)
        const uint8_t m = MinAlpha(d->scratch.data(), size_t(w) * 4, w, h);
        ps2e_draw::Texture& t = it->second;
        t.min_gpu_alpha = t.alpha_seen ? std::min(t.min_gpu_alpha, m) : m;
        t.alpha_seen = true;
    }
#endif
    d->device->Upload(it->second.surface, render::Rect{x, y, w, h}, d->scratch.data(), size_t(w) * 4);
    return 1;
}

int ps2e_draw_texture_set_mips(ps2e_draw* d, uint32_t texture, int count, const void* const* levels) {
    if (!d || count < 0 || count > 6 || (count && !levels)) return 0;
    auto it = d->textures.find(texture);
    if (it == d->textures.end()) return 0;
    ps2e_draw::Texture& t = it->second;
    for (int i = 1; i <= count; ++i) {
        if (!levels[i - 1]) return 0;
        d->device->UploadLevel(t.surface, i, std::max(t.w >> i, 1), std::max(t.h >> i, 1), levels[i - 1]);
#if PS2R_HOST_VITA
        // (the levels come in GPU alpha: their smallest byte bounds what a mip-mapped fetch can return too)
        const int lw = std::max(t.w >> i, 1), lh = std::max(t.h >> i, 1);
        const uint8_t m = MinAlpha(static_cast<const uint8_t*>(levels[i - 1]), size_t(lw) * 4, lw, lh);
        t.min_gpu_alpha = t.alpha_seen ? std::min(t.min_gpu_alpha, m) : m;
        t.alpha_seen = true;
#endif
    }
    d->device->FinishMipChain(t.surface, count, false);
    t.levels = count;
    {
        // (the census: the made figure follows the texture's storage as it now stands)
        const uint64_t before = t.vita_bytes;
        CountTextureBytes(d, t, count);
        d->textures_made_vita_bytes += t.vita_bytes - before;
    }
    return 1;
}

void ps2e_draw_texture_destroy(ps2e_draw* d, uint32_t texture) {
    if (!d) return;
    if ((texture & ps2e_draw::kMovieIdMask) == ps2e_draw::kMovieIdBase) {
        auto m = d->movies.find(texture);
        if (m == d->movies.end()) return;
        for (const render::SurfaceId s : m->second.planes) d->device->DestroySurface(s);
        d->movies.erase(m);
        return;
    }
    auto it = d->textures.find(texture);
    if (it == d->textures.end()) return;
    d->device->DestroySurface(it->second.surface);
    if (d->texture_slots[texture & (ps2e_draw::kTextureSlots - 1)].id == texture)
        d->texture_slots[texture & (ps2e_draw::kTextureSlots - 1)] = ps2e_draw::TextureSlot{};
    d->texture_bytes -= it->second.bytes;
    d->texture_vita_bytes -= it->second.vita_bytes;
    d->textures_destroyed++;
    d->textures_destroyed_vita_bytes += it->second.vita_bytes;
    d->textures.erase(it);
    d->counters.textures_alive--;
}

// ---- movie pictures -------------------------------------------------------------------------------------------------
uint32_t ps2e_draw_movie_create(ps2e_draw* d, int w, int h, int format, int slots) {
    if (!d || !d->device || w <= 0 || h <= 0 || w > 4096 || h > 4096 || slots <= 0 || slots > 64) return 0;
    if (format != PS2E_DRAW_MOVIE_RGBA && format != PS2E_DRAW_MOVIE_YUV420) return 0;
    ps2e_draw::Movie m;
    m.w = w;
    m.h = h;
    m.format = format;
    m.slots = slots;
    m.planes_per_slot = format == PS2E_DRAW_MOVIE_YUV420 ? 3 : 1;
    const auto fail = [&]() {
        for (const render::SurfaceId s : m.planes) d->device->DestroySurface(s);
        return 0u;
    };
    for (int s = 0; s < slots; ++s) {
        for (int p = 0; p < m.planes_per_slot; ++p) {
            render::SurfaceDesc td;
            td.width = uint32_t(p == 0 ? w : (w + 1) / 2);
            td.height = uint32_t(p == 0 ? h : (h + 1) / 2);
            td.format = format == PS2E_DRAW_MOVIE_YUV420 ? render::SurfaceFormat::R8 : render::SurfaceFormat::Rgba8;
            td.renderable = false;
            td.cpu_writable = true;
            const render::SurfaceId id = d->device->CreateSurface(td);
            if (!id.id) return fail();
            m.planes.push_back(id);
            size_t stride = 0;
            if (!d->device->SurfaceMemory(id, &stride) || stride < size_t(td.width) * (p == 0 && format == PS2E_DRAW_MOVIE_RGBA ? 4u : 1u))
                return fail();   // the backend cannot hand its storage out: no movie texture (the caller keeps RGBA uploads)
        }
    }
    const uint32_t id = ps2e_draw::kMovieIdBase + (++d->next_movie & ~ps2e_draw::kMovieIdMask);
    d->movies[id] = std::move(m);
    return id;
}

void* ps2e_draw_movie_plane(ps2e_draw* d, uint32_t movie, int slot, int plane, size_t* stride) {
    if (stride) *stride = 0;
    if (!d) return nullptr;
    auto it = d->movies.find(movie);
    if (it == d->movies.end()) return nullptr;
    const ps2e_draw::Movie& m = it->second;
    if (slot < 0 || slot >= m.slots || plane < 0 || plane >= m.planes_per_slot) return nullptr;
    return d->device->SurfaceMemory(m.planes[size_t(slot) * size_t(m.planes_per_slot) + size_t(plane)], stride);
}

void ps2e_draw_movie_show(ps2e_draw* d, uint32_t movie, int slot) {
    if (!d) return;
    auto it = d->movies.find(movie);
    if (it == d->movies.end()) return;
    it->second.current = slot >= 0 && slot < it->second.slots ? slot : -1;
}

void ps2e_draw_movie_counts(const ps2e_draw* d, uint64_t* drawn, uint64_t* refused) {
    if (drawn) *drawn = d ? d->movie_drawn : 0;
    if (refused) *refused = d ? d->movie_refused : 0;
}

void ps2e_draw_begin_frame(ps2e_draw* d, const ps2e_draw_clear* clear) {
    if (!d) return;
    d->mesh_run = false;
    if (d->in_frame) d->device->EndDrawRun();
    // A changed internal scale takes effect at a frame's start (the target re-made before its draws); the texture
    // filter override with it (the present applies the rest)
    {
        std::lock_guard<std::mutex> lock(d->presentation_mutex);
        const int want_scale = std::clamp(d->wanted.scale, 1, 4);   // 0 = 1
        if (want_scale != d->scale) {
            d->scale = want_scale;
            MakeFrameTarget(*d, d->frame_w, d->frame_h);
        }
        d->applied.texture_filter = d->wanted.texture_filter;
    }
    d->device->BeginFrame();
    d->in_frame = true;
    d->frame_copy_valid = false;
    d->early_casters = false;
    d->bracket_region = d->next_shadow_region = false;   // (a region holds for its own frame's bracket only)
    {
        // the screen copies' scene breaks (Backend::KeepDepthAcrossCopies): this frame's announced copies, or copies the
        // frame before took without an announcement
        const bool prev_unannounced = d->frame_copies > 0 && !d->frame_announced;
        d->frame_announced = d->expect_copies > 0;
        d->device->KeepDepthAcrossCopies(d->color, d->frame_announced || prev_unannounced);
        d->expect_copies = 0;
        d->frame_copies = 0;
    }
    d->counters.triangles = 0;
#if PS2R_HOST_VITA
    d->casters.clear();
    d->caster_verts.clear();
    d->frame_draws = 0;
    d->frame_fix = 0;
    d->frame_fix_blends = 0;
    d->frame_fix_blends_textured = 0;
    d->frame_restamps = 0;
#endif
    if (clear && (clear->clear_colour || clear->clear_depth)) {
        uint32_t flags = 0;
        if (clear->clear_colour) flags |= render::kClearColor;
        if (clear->clear_depth) flags |= render::kClearDepth;
        // Clear's rgba is the attachment's byte order (R in the low byte); the alpha written is the GPU's opaque.
        const uint32_t rgba = uint32_t(clear->red) | uint32_t(clear->green) << 8 | uint32_t(clear->blue) << 16 |
                              0xFFu << 24;
        TimerPhase(d, PS2E_DRAW_PHASE_CLEAR);
        TimerFill(d, PS2E_DRAW_PHASE_CLEAR, TargetPixels(d));
        d->device->Clear(d->color, d->depth, rgba, 0.0f, flags);
    }
    d->device->BeginDrawRun();
}

// The draw's GS record (d->ir, its vertices left as they are) and texture source from the producer's state; 0, or -2
// when the state names a texture that is not there (counted as refused)
#if PS2R_HOST_VITA
namespace {
bool FixDarkening(const ps2e_draw* d, const ps2e_draw_state* st);
}  // namespace
#endif
static int PrepareRecord(ps2e_draw* d, const ps2e_draw_state* st, gs::TextureSource& ts) {
    // The record starts from its defaults each draw; the vertex vector keeps its capacity (a frame's draws allocate
    // nothing once the largest has been seen -- a fresh vector per draw cost a large allocation and its page faults).
    gs::DrawIr& ir = d->ir;
    {
        std::vector<gs::VertexRec> kept = std::move(ir.verts);
        ir = gs::DrawIr{};
        ir.verts = std::move(kept);
    }
    ir.prim_type = 3;
    ir.gouraud = st->gouraud != 0;
    ir.alpha_blend = st->blend != 0;
    ir.use_fst = false;
    ir.ctx = 0;
    ir.off_x = ir.off_y = 0.f;
    ir.raster.frame.fbp = 0;
    ir.raster.frame.fbw = uint32_t(d->frame_w);
    ir.raster.frame.psm = 0;      // PSMCT32
    ir.raster.frame.fbmsk = st->no_colour_write ? 0xFFFFFFFFu : 0u;
#if PS2R_HOST_VITA
    // the Vita's Cd - Cs.FIX: its alpha carries FIX for the blend (FixDarkening), the frame's alpha is left as it was
    if (FixDarkening(d, st)) ir.raster.frame.fbmsk = 0xFF000000u;
#endif
    // A nominal Z32 buffer base apart from the frame: the backend keys depth by the attachment, not the address.
    ir.raster.zbuf.zbp = uint32_t(d->frame_w) * uint32_t(d->frame_h) * 4u;
    ir.raster.zbuf.psm = 0;
    ir.raster.zbuf.zmsk = st->depth_write == 0;
    ir.raster.test.zte = true;
    ir.raster.test.ztst = uint8_t(std::clamp(st->depth_test, 0, 3));
    ir.raster.test.ate = st->alpha_test != 0;
    ir.raster.test.atst = uint8_t(std::clamp(st->alpha_method, 0, 7));
    ir.raster.test.aref = st->alpha_reference;
    ir.raster.test.afail = uint8_t(std::clamp(st->alpha_fail, 0, 3));
    ir.raster.alpha.a = uint8_t(st->blend_a & 3);
    ir.raster.alpha.b = uint8_t(st->blend_b & 3);
    ir.raster.alpha.c = uint8_t(st->blend_c & 3);
    ir.raster.alpha.d = uint8_t(st->blend_d & 3);
    ir.raster.alpha.fix = st->blend_fix;
    ir.raster.colclamp = true;
    // FBA on an unblended draw: the written alpha's top bit (the GPU's 0xFF). A blended draw keeps the fixed-function
    // blend (the backend's FBA arm would read the destination back per draw); its alpha stays the source's.
    ir.raster.fba = st->fba != 0 && st->blend == 0;
    if (st->scissor_x0 == 0 && st->scissor_y0 == 0 && st->scissor_x1 == 0 && st->scissor_y1 == 0) {
        ir.raster.scissor = {0, 0, uint16_t(d->frame_w - 1), uint16_t(d->frame_h - 1)};
    } else {
        ir.raster.scissor = {uint16_t(std::clamp(st->scissor_x0, 0, d->frame_w - 1)),
                             uint16_t(std::clamp(st->scissor_y0, 0, d->frame_h - 1)),
                             uint16_t(std::clamp(st->scissor_x1, 0, d->frame_w - 1)),
                             uint16_t(std::clamp(st->scissor_y1, 0, d->frame_h - 1))};
    }

    const ps2e_draw::Texture* tex = nullptr;
    if (st->texture == PS2E_DRAW_TEXTURE_FRAME_COPY) {
        // The screen copy as the texture -- only once a copy of this frame has been taken
        if (!d->frame_copy_valid || !d->frame_copy.surface.id) {
            d->counters.refused++;
            return -2;
        }
        tex = &d->frame_copy;
    } else if (st->texture) {
        tex = FindTextureSlot(d, st->texture);
        if (!tex) {
            d->counters.refused++;
            return -2;
        }
    }
    if (tex) {
        ir.tex_enabled = true;
        ir.tex.fst = false;
        ir.tex.tfx = uint32_t(std::clamp(st->tfx, 0, 3));
        ir.tex.tcc = st->tcc ? 1u : 0u;
        ir.tex.psm = 0;
        ir.tex.tw = RoundedLog2(uint32_t(tex->w));
        ir.tex.th = RoundedLog2(uint32_t(tex->h));
        ir.tex.tbw = uint32_t(tex->w);
        ir.tex.mmag = st->linear != 0;
        ir.tex.mmin = st->linear ? 1 : 0;
        ir.tex.wms = uint8_t(st->wrap_u ? 1 : 0);
        ir.tex.wmt = uint8_t(st->wrap_v ? 1 : 0);
        ts.enabled = true;
        ts.owner.kind = gs::TextureSource::OwnerKind::Decoded;
        ts.owner.surface = tex->surface;
        ts.owner.owned = true;
        ts.transform.uv_tex_w = ts.transform.uv_pre_w = tex->w;
        ts.transform.uv_tex_h = ts.transform.uv_pre_h = tex->h;
        ts.transform.draw_scale = 1;
        // The title's TEX1 choice, or the Display page's override (ps2e_draw_presentation.texture_filter)
        const bool linear = d->applied.texture_filter == 1 ? false : d->applied.texture_filter == 2 ? true : st->linear != 0;
        ts.sampler.mag_linear = linear;
        ts.sampler.mmin = linear ? 1 : 0;
        ts.sampler.wms = uint8_t(st->wrap_u ? 1 : 0);
        ts.sampler.wmt = uint8_t(st->wrap_v ? 1 : 0);
        // TEX1's mipmapping over the texture's levels: the shader's per-pixel LOD (Q-based, LCM 0) between them
        const int mxl = std::min(st->mip_max_level, tex->levels);
        if (mxl > 0 && st->mip_minify >= 2 && st->mip_minify <= 5) {
            ts.owner.levels = uint8_t(tex->levels);
            ts.transform.mip_levels = uint8_t(mxl);
            ts.transform.mip_lcm = false;
            ts.transform.mip_k = int16_t(int32_t(uint32_t(st->mip_k) << 20) >> 20);   // the 12-bit field, sign-extended
            ts.transform.mip_l = uint8_t(st->mip_l & 3);
            // the Display page's point / bilinear choice keeps the mipmaps, nearest or linear within a level
            ts.sampler.mmin = uint8_t(d->applied.texture_filter == 1 ? 2 : d->applied.texture_filter == 2 ? 4 : st->mip_minify);
            ir.tex.mmin = ts.sampler.mmin;
        }
    }

    return 0;
}

// The frame target the draws go to, and the shadow counter's stencil test around this one draw
static render::DrawParams TargetParams(ps2e_draw* d, const ps2e_draw_state* st) {
    render::DrawParams p;
    p.color = d->color;
    p.depth = d->depth;
    p.render_w = p.target_w = d->frame_w;
    p.render_h = p.target_h = d->frame_h;
    p.scale = uint8_t(d->scale);
    p.has_32bit_content = true;
    p.cd_copy = &d->cd_copy;
    // The counter: the frame's depth attachment is the DepthStencil24_8 surface, and the backend applies the stencil
    // test and update around this one draw (VolumeStencil).
    if (st->stencil == PS2E_DRAW_STENCIL_INSIDE) StencilProbe(d, "composite");
    if (st->stencil != PS2E_DRAW_STENCIL_OFF) p.stencil_state = VolumeStencil(st);
    return p;
}

#if PS2R_HOST_VITA
namespace {
void ResetCasterCounts(ps2e_draw* d);   // below the mesh submits: the casters drawn again into the stencil
// The shadows' composite, Cd - Cs.FIX (ALPHA {2, 0, 2, 1}, untextured): GXM has no constant blend factor, and the GL
// backend stands the source alpha in for FIX on the Vita (blend_ff_map.h) -- the quad's alpha 0x80 made the composite
// Cd - Cs, every shadow nearly black. Such a draw's vertices carry FIX as their alpha (the factor the stand-in reads:
// 0x20 -> 0.25, Cd - 33 as the GS drew it) and its alpha is not written (the frame keeps the receivers' alpha).
bool FixDarkening(const ps2e_draw* d, const ps2e_draw_state* st) {
    return d->shadow_fix && st->blend && st->blend_a == 2 && st->blend_b == 0 && st->blend_c == 2 && st->blend_d == 1 &&
           st->texture == 0 && st->no_colour_write == 0 &&
           (st->stencil == PS2E_DRAW_STENCIL_OFF || st->stencil == PS2E_DRAW_STENCIL_INSIDE);
}

// ---- the casters' re-stamp (the Vita's SeedCasterMask) ----------------------------------------------------------------
// The GS counted a volume strip only where the frame's alpha top bit was 0 at StartShadows (DATE): a pixel whose alpha is
// 0x80 or more -- a skin's, an FBA material's -- is a CASTER, never darkened. The desktop marks those pixels from the
// frame's alpha (MarkStencilWhereOpaque); on the Vita the frame's colour is in the GPU's tiles until its scene ends, so
// every draw of the frame that can leave such an alpha is kept (with its screen box) and, at the bracket's composite,
// drawn again into the stencil alone where a volume of the bracket was counted (ResetCasterCounts): colour, depth writes
// and blending off, depth GEQUAL -- its own surface where it is still the frame's nearest (a nearer receiver drawn after
// it hides it there, as that receiver's alpha replaced the caster's) -- the alpha test keeping the fragments that write
// 0x80 or more (an unblended FBA draw writes the top bit on every fragment it keeps: its own test), the counter put back
// to its origin where it passes. A caster pixel then ends at the origin as the DATE left it (never counted); a caster
// away from every volume keeps the origin anyway (its replay is skipped). A later draw that does not write depth over a
// caster (a translucent effect) does not unmark it, where the GS took that draw's alpha.
struct CasterNeed {
    bool caster = false;   // the draw can leave a PS2 alpha of 0x80 or more
    bool whole = false;    // every fragment it writes does (FBA on an unblended draw)
    int need_av = 0;       // else: a vertex alpha this large is needed for any fragment to (0: none needed)
};

const ps2e_draw::Texture* FindTexture(ps2e_draw* d, uint32_t id) { return FindTextureSlot(d, id); }

// The written alpha's reach (PS2 scale) under the GS's TFX: MODULATE At.Av >> 7, DECAL / HIGHLIGHT2 At, HIGHLIGHT
// At + Av; TCC 0 and untextured: Av
// MODULATE's vertex alpha needed for a texture reach `at` (1..0x80): (0x4000 + at - 1) / at, tabulated
struct NeedAvTable {
    int v[0x81];
};
constexpr NeedAvTable MakeNeedAv() {
    NeedAvTable t{};
    for (int at = 1; at <= 0x80; ++at) t.v[at] = (0x4000 + at - 1) / at;
    return t;
}
constexpr NeedAvTable kNeedAv = MakeNeedAv();
CasterNeed CasterNeedOf(ps2e_draw* d, const ps2e_draw_state* st) {
    CasterNeed n;
    if (st->no_colour_write || st->stencil != PS2E_DRAW_STENCIL_OFF || st->depth_test == PS2E_DRAW_DEPTH_NEVER ||
        st->texture == PS2E_DRAW_TEXTURE_FRAME_COPY)
        return n;
    // a draw that tests no depth and writes none -- the sky, drawn first and left at the far plane: where it stays
    // visible no volume face fails the depth test, so nothing is ever counted there (its ~12k vertices a hub frame were
    // the re-stamp's largest waste)
    if (st->depth_test == PS2E_DRAW_DEPTH_ALWAYS && !st->depth_write) return n;
    // a test that fails every fragment of 0x80 or more, its AFAIL keeping the frame's alpha (FB_ONLY writes it anyway)
    if (st->alpha_test && st->alpha_fail != 1) {
        const int ref = st->alpha_reference;
        switch (st->alpha_method) {
            case PS2E_DRAW_ATST_NEVER: return n;
            case PS2E_DRAW_ATST_LESS: if (ref <= 0x80) return n; break;
            case PS2E_DRAW_ATST_LEQUAL: case PS2E_DRAW_ATST_EQUAL: if (ref < 0x80) return n; break;
            default: break;
        }
    }
    n.caster = true;
    if (st->fba && !st->blend) {
        n.whole = true;
        return n;
    }
    if (!st->texture || !st->tcc) {
        n.need_av = 0x80;
        return n;
    }
    const ps2e_draw::Texture* tex = FindTexture(d, st->texture);
    // the shader reads a texel's alpha back to the PS2's scale at most 0x80 (GPU 0xFF -> 128: the PS2's opaque), so a
    // texture's reach is its largest alpha up to 0x80 -- with the lit world's vertex alpha 0x7E a MODULATE draw stays below
    const int at = std::min(tex ? int(tex->max_alpha) : 0x80, 0x80);
    switch (st->tfx) {
        case PS2E_DRAW_TFX_DECAL:
        case PS2E_DRAW_TFX_HIGHLIGHT2:
            n.caster = at >= 0x80;
            break;
        case PS2E_DRAW_TFX_HIGHLIGHT:
            n.need_av = at >= 0x80 ? 0 : 0x80 - at;
            break;
        default:   // MODULATE: (At * Av) >> 7 >= 0x80 <=> At * Av >= 0x4000
            if (at == 0) {
                n.caster = false;
            } else {
                n.need_av = kNeedAv.v[at];   // (0x4000 + at - 1) / at (a table -- the A9 divides in software)
                if (n.need_av > 255) n.caster = false;
            }
            break;
    }
    return n;
}

// The replay of a caster draw: into the stencil only, where it is still the visible surface, the fragments that write
// a PS2 alpha of 0x80 or more
ps2e_draw_state ReplayState(const ps2e_draw_state* st, bool whole) {
    ps2e_draw_state r = *st;
    r.no_colour_write = 1;
    r.depth_write = 0;
    r.depth_test = PS2E_DRAW_DEPTH_GEQUAL;
    r.blend = 0;
    r.fba = 0;
    r.stencil = PS2E_DRAW_STENCIL_OFF;   // the re-stamp's own stencil state is the caller's
    r.dest_alpha_test = 0;
    if (whole) {
        if (r.alpha_test && r.alpha_fail == 1) r.alpha_test = 0;   // FB_ONLY: its failing fragments are written too
        r.alpha_fail = 0;
        return r;
    }
    int method = PS2E_DRAW_ATST_GEQUAL, ref = 0x80;
    if (st->alpha_test && st->alpha_fail != 1) {
        if (st->alpha_method == PS2E_DRAW_ATST_GEQUAL && st->alpha_reference > 0x80) {
            ref = st->alpha_reference;
        } else if (st->alpha_method == PS2E_DRAW_ATST_GREATER && st->alpha_reference >= 0x80) {
            method = PS2E_DRAW_ATST_GREATER;
            ref = st->alpha_reference;
        } else if (st->alpha_method == PS2E_DRAW_ATST_EQUAL) {
            method = PS2E_DRAW_ATST_EQUAL;
            ref = st->alpha_reference;
        }
    }
    r.alpha_test = 1;
    r.alpha_method = method;
    r.alpha_reference = uint8_t(ref);
    r.alpha_fail = 0;
    return r;
}

// The casters a frame KEEPS: a bound on the records alone (ps2e_draw::max_casters; a frame's draws that can leave a PS2
// alpha of 0x80 or more: the Academy hub keeps up to ~180 a picture, lit FBA scenery drawn before its characters).
// TE_GFX_VITA_CASTERS_KEEP=N bounds them at N (128: the earlier rule, which kept the frame's FIRST 128 -- in the Academy hub
// all of them scenery far from any shadow, so Crash's own parts were never kept and his volume darkened him).
// The re-stamps a frame DRAWS: every draw of a GXM scene that changes its uniforms reserves them from the context's rings
// (vitaGL's defaults: 512 KB of fragment uniforms), which do not turn over within a scene -- the hub's frame already
// takes ~700 draws, and doubling them overran them (other draws' uniforms read: garbage). Only the casters near a counted
// volume are drawn again (ResetCasterCounts: a few a bracket), the characters first; past this budget a frame's later
// ones are not (logged once)
constexpr uint32_t kMaxRestamps = 128;
bool CasterRoom(ps2e_draw* d) {
    if (d->casters.size() < d->max_casters) return true;
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "[ps2e] the casters' re-stamp keeps the first %u casters of a frame (present %llu has more)\n",
                     unsigned(d->max_casters), (unsigned long long)d->presented);
    }
    return false;
}

// The screen box of vertices (frame pixels), widened by a pixel for the rasteriser's rounding
ps2e_draw::ScreenBox BoxOf(const ps2e_draw_vertex* v, int n) {
    ps2e_draw::ScreenBox b{1e30f, 1e30f, -1e30f, -1e30f};
    for (int i = 0; i < n; ++i) {
        b.x0 = std::min(b.x0, v[i].x);
        b.y0 = std::min(b.y0, v[i].y);
        b.x1 = std::max(b.x1, v[i].x);
        b.y1 = std::max(b.y1, v[i].y);
    }
    return ps2e_draw::ScreenBox{b.x0 - 1.f, b.y0 - 1.f, b.x1 + 1.f, b.y1 + 1.f};
}

// A mesh's screen box: its model-space box's corners through its matrix as the native vertex program takes them
// (row-vector convention, the divide, the 12.4 pixels, the GS origin off); a corner at or behind the eye: anywhere
ps2e_draw::ScreenBox MeshBoxOf(const ps2e_draw::MeshBounds& b, const ps2e_draw_mesh_params* mp) {
    if (b.skin) return ps2e_draw::ScreenBox{-1e30f, -1e30f, 1e30f, 1e30f};
    const float* m = mp->screen;
    ps2e_draw::ScreenBox s{1e30f, 1e30f, -1e30f, -1e30f};
    for (int k = 0; k < 8; ++k) {
        const float x = (k & 1) ? b.hi[0] : b.lo[0], y = (k & 2) ? b.hi[1] : b.lo[1], z = (k & 4) ? b.hi[2] : b.lo[2];
        const float cx = m[12] + x * m[0] + y * m[4] + z * m[8];
        const float cy = m[13] + x * m[1] + y * m[5] + z * m[9];
        const float cw = m[15] + x * m[3] + y * m[7] + z * m[11];
        if (!(cw > 1e-6f) || !std::isfinite(cx) || !std::isfinite(cy)) return ps2e_draw::ScreenBox{-1e30f, -1e30f, 1e30f, 1e30f};
        const float sx = cx / cw / 16.f - mp->offset[0], sy = cy / cw / 16.f - mp->offset[1];
        s.x0 = std::min(s.x0, sx);
        s.y0 = std::min(s.y0, sy);
        s.x1 = std::max(s.x1, sx);
        s.y1 = std::max(s.y1, sy);
    }
    return ps2e_draw::ScreenBox{s.x0 - 2.f, s.y0 - 2.f, s.x1 + 2.f, s.y1 + 2.f};
}

void RecordCaster(ps2e_draw* d, const ps2e_draw_state* st, const ps2e_draw_vertex* v, int count) {
    const CasterNeed n = CasterNeedOf(d, st);
    if (!n.caster || !CasterRoom(d)) return;
    const int tri_verts = count / 3 * 3;
    if (n.need_av > 0) {
        // the vertices' alpha sampled -- the first triangle, then about 64 evenly spaced: a skin's 0x80 shows at its first
        // vertex, the lit world's 0x7E nowhere. Reading every vertex of every draw (the world's clipped instances, the sky,
        // the particles: ~50-80k a hub frame) cost the drawing thread ~1 ms a frame (TE_PROF on x86 Vita3K) for the few
        // skins it finds; a draw whose high alpha the samples miss keeps its fragments counted (a rare self-shadow)
        bool hit = false;
        for (int i = 0; i < std::min(3, tri_verts) && !hit; ++i) hit = v[i].a >= n.need_av;
        const int step = std::max(1, tri_verts / 64);
        for (int i = 3; i < tri_verts && !hit; i += step) hit = v[i].a >= n.need_av;
        if (!hit) return;
    }
    if (d->census_every > 0) {
        static int said = 0;
        if (said++ < 24) {
            const ps2e_draw::Texture* tex = st->texture ? FindTexture(d, st->texture) : nullptr;
            int av = 0;
            for (int k = 0; k < tri_verts; ++k) av = std::max(av, int(v[k].a));
            std::fprintf(stderr, "[ps2e.casters] kept vertex draw (present %llu): %d vertices, fba %d blend %d tfx %d tcc %d texture %u "
                         "(max alpha %d), vertex alpha <= %d, atest %d/%d/%d, ztest %d zwrite %d\n", (unsigned long long)d->presented,
                         tri_verts, st->fba, st->blend, st->tfx, st->tcc, st->texture, tex ? int(tex->max_alpha) : -1, av,
                         st->alpha_test, st->alpha_method, st->alpha_reference, st->depth_test, st->depth_write);
        }
    }
    ps2e_draw::CasterDraw c;
    c.state = ReplayState(st, n.whole);
    c.box = ps2e_draw::ScreenBox{1e30f, 1e30f, -1e30f, -1e30f};   // a vertex draw's nearness is tested at the composite
    c.count = uint32_t(tri_verts);
    for (int i = 0; i < tri_verts; i += 4) {   // (the composite's tested vertices; std::min / max keep a NaN out)
        c.box4.x0 = std::min(c.box4.x0, v[i].x);
        c.box4.y0 = std::min(c.box4.y0, v[i].y);
        c.box4.x1 = std::max(c.box4.x1, v[i].x);
        c.box4.y1 = std::max(c.box4.y1, v[i].y);
    }
    if (d->retained) {
        c.verts = v;   // the producer keeps them until the frame is presented (ps2e_draw_set_retained_vertices)
    } else {
        c.first = uint32_t(d->caster_verts.size());
        d->caster_verts.insert(d->caster_verts.end(), v, v + tri_verts);
    }
    d->casters.push_back(c);
}

// a mesh's bounds (null: none kept) through the direct-mapped slots

void RecordCasterMesh(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const ps2e_draw_mesh_params* mp,
                      const float* bones = nullptr, int joints = 0, const float* disp = nullptr, int disp_count = 0) {
    const CasterNeed n = CasterNeedOf(d, st);
    if (!n.caster || !CasterRoom(d)) return;
    const ps2e_draw::MeshBounds* mb = FindBounds(d, mesh);
    if (n.need_av > 0) {
        const int av = !mb ? 0xFF : (mp->lit ? mb->la : mb->a);
        if (av < n.need_av) return;
    }
    if (d->census_every > 0) {
        static int said = 0;
        if (said++ < 24) {
            const ps2e_draw::Texture* tex = st->texture ? FindTexture(d, st->texture) : nullptr;
            std::fprintf(stderr, "[ps2e.casters] kept mesh draw (present %llu): mesh %u lit %d (alpha %d, lit alpha %d), fba %d blend %d "
                         "tfx %d tcc %d texture %u (max alpha %d), atest %d/%d/%d, ztest %d zwrite %d\n", (unsigned long long)d->presented,
                         mesh, mp->lit, !mb ? -1 : int(mb->a), !mb ? -1 : int(mb->la), st->fba, st->blend, st->tfx, st->tcc,
                         st->texture, tex ? int(tex->max_alpha) : -1, st->alpha_test, st->alpha_method, st->alpha_reference,
                         st->depth_test, st->depth_write);
        }
    }
    ps2e_draw::CasterDraw c;
    c.state = ReplayState(st, n.whole);
    c.box = !mb ? ps2e_draw::ScreenBox{-1e30f, -1e30f, 1e30f, 1e30f} : MeshBoxOf(*mb, mp);
    c.mesh = mesh;
    c.mp = *mp;
    c.bones = bones;
    c.joints = joints;
    c.disp = disp;
    c.disp_count = disp_count;
    d->casters.push_back(c);
}

// two states the executor sets the same GL fixed-function context for: equal but for the texture, the alpha test and the
// mipmapping (the fields a texture swap may change: textures, uniforms, the texture's own sampler)
bool SameContextState(const ps2e_draw_state* a, const ps2e_draw_state* b) {
    ps2e_draw_state probe = *a;
    probe.texture = b->texture;
    probe.alpha_test = b->alpha_test;
    probe.alpha_method = b->alpha_method;
    probe.alpha_reference = b->alpha_reference;
    probe.alpha_fail = b->alpha_fail;
    probe.mip_max_level = b->mip_max_level;
    probe.mip_k = b->mip_k;
    probe.mip_l = b->mip_l;
    probe.mip_minify = b->mip_minify;
    return std::memcmp(&probe, b, sizeof probe) == 0;
}
// A counting draw's screen box, kept for the bracket's composite (a long list folds into its union)
void NoteVolumeBox(ps2e_draw* d, const ps2e_draw::ScreenBox& b) {
    if (d->volume_boxes.size() < 512) {
        d->volume_boxes.push_back(b);
        return;
    }
    ps2e_draw::ScreenBox& u = d->volume_boxes.back();
    u = ps2e_draw::ScreenBox{std::min(u.x0, b.x0), std::min(u.y0, b.y0), std::max(u.x1, b.x1), std::max(u.y1, b.y1)};
}
void NoteVolume(ps2e_draw* d, const ps2e_draw_vertex* v, int count) { NoteVolumeBox(d, BoxOf(v, count / 3 * 3)); }
}  // namespace
#endif

// One triangle list under `st` (the submit's body): the stencil state around it is the counter's for st (VolumeStencil),
// or `stencil` when given (the Vita's casters' re-stamp)
static int SubmitTriangles(ps2e_draw* d, const ps2e_draw_state* st, const ps2e_draw_vertex* v, int count,
                           const render::StencilState* stencil) {
    const int tri_verts = (count / 3) * 3;
    gs::DrawIr& ir = d->ir;
    gs::TextureSource ts;
    if (PrepareRecord(d, st, ts) < 0) return -2;
    render::DrawParams p = TargetParams(d, st);
    if (stencil) p.stencil_state = *stencil;
    {
        // the device's raw draw when it takes this one: the vertices as given, their screen box (a vertex whose Q is
        // not finite takes the records, which read it as 1)
        static_assert(sizeof(ps2e_draw_vertex) == sizeof(render::RawVertex), "the producer's vertex is the raw layout");
        if (d->device->DrawsRaw(ir, ts, p)) {
            // Only whether every Q is finite decides it (the exponent's bits, one test per vertex). The raw draw's box
            // feeds nothing that changes the picture -- its record has no vertices, so the destination copy's rectangle
            // is never taken from it, and a ps2e_draw state never blends in the shader anyway (Decoded textures, FBA only
            // unblended, colour clamp on, a 32-bit frame) -- only the result's write rectangle, which this surface does
            // not read: the whole frame stands for it (the per-vertex min / max was a third pass over every vertex)
            const float box[4] = {-1e9f, -1e9f, 1e9f, 1e9f};
            bool finite = true;
#if PS2R_HOST_VITA
            // (ps2e_draw_submit_finite: the producer checked these very vertices in its own pass)
            const int checked = d->q_finite == v ? tri_verts : 0;
#else
            const int checked = 0;
#endif
            for (int i = checked; i < tri_verts; ++i) {
                uint32_t bits;
                std::memcpy(&bits, &v[i].q, sizeof bits);
                if ((bits & 0x7F800000u) == 0x7F800000u) {
                    finite = false;
                    break;
                }
            }
            if (finite) {
                ir.verts.clear();
                ir.vertex_count = uint32_t(tri_verts);
                const render::DrawResult r =
                    d->device->DrawRaw(ir, ts, p, reinterpret_cast<const render::RawVertex*>(v), size_t(tri_verts), box);
                d->counters.draws++;
                if (!r.drew) {
                    d->counters.refused++;
                    return -3;
                }
                d->counters.triangles += uint64_t(tri_verts / 3);
                return tri_verts / 3;
            }
        }
    }
    ir.verts.resize(size_t(tri_verts));
    // Each record assembled in a local and copied out whole (no zeroing pass over the records first)
    struct alignas(4) Rec {
        float x, y, z, s, t, q;
        uint16_t uv_u, uv_v;
        uint8_t r, g, b, a, fog, draw_kick, pad[2];
    };
    static_assert(sizeof(Rec) == sizeof(gs::VertexRec), "Rec mirrors VertexRec's layout");
    uint8_t* out = reinterpret_cast<uint8_t*>(ir.verts.data());
    for (int i = 0; i < tri_verts; ++i) {
        const ps2e_draw_vertex& in = v[i];
        Rec rec;
        rec.x = in.x;
        rec.y = in.y;
        rec.z = in.z;
        // The GS's STQ: a producer giving q has u = S = u'.q, v = T = v'.q (the backend's per-pixel divide makes the
        // mapping perspective-correct); one leaving q 0 gets q = 1 (the affine mapping of its u, v)
        rec.q = (std::isfinite(in.q) && in.q != 0.f) ? in.q : 1.f;
        rec.s = in.u;
        rec.t = in.v;
        // uv_u / uv_v (the UV register's 12.4 texels) stay 0: the record is STQ (tex.fst off), and only the UV mode reads them
        rec.uv_u = rec.uv_v = 0;
        rec.r = in.r;
        rec.g = in.g;
        rec.b = in.b;
        rec.a = in.a;
        rec.fog = 0;
        rec.draw_kick = 1;
        rec.pad[0] = rec.pad[1] = 0;
        std::memcpy(out + size_t(i) * sizeof(Rec), &rec, sizeof(Rec));
    }
    ir.vertex_count = uint32_t(tri_verts);

    const render::DrawResult r = d->device->Draw(ir, ts, p);
    d->counters.draws++;
    if (!r.drew) {
        d->counters.refused++;
        return -3;
    }
    d->counters.triangles += uint64_t(tri_verts / 3);
    return tri_verts / 3;
}

// A draw of a movie texture (ps2e_draw_movie_*): the slot shown drawn over the vertices' box by the device's movie program
static int SubmitMovie(ps2e_draw* d, const ps2e_draw_state* st, const ps2e_draw_vertex* v, int count) {
    d->mesh_run = false;   // (another draw came between: no DrawMeshAgain after it)
    auto it = d->movies.find(st->texture);
    if (it == d->movies.end()) {
        d->counters.refused++;
        return -2;
    }
    const ps2e_draw::Movie& m = it->second;
    if (m.current < 0) {   // nothing shown yet: nothing drawn (as an RGBA texture with nothing uploaded showed nothing)
        d->movie_refused++;
        return 0;
    }
    render::Backend::MoviePicture mp;
    mp.color = d->color;
    mp.depth = d->depth;
    mp.target_w = d->frame_w;
    mp.target_h = d->frame_h;
    mp.scale = d->scale;
    mp.x0 = mp.x1 = v[0].x;
    mp.y0 = mp.y1 = v[0].y;
    mp.u0 = mp.u1 = v[0].u;
    mp.v0 = mp.v1 = v[0].v;
    for (int i = 1; i < count; ++i) {
        mp.x0 = std::min(mp.x0, v[i].x);
        mp.x1 = std::max(mp.x1, v[i].x);
        mp.y0 = std::min(mp.y0, v[i].y);
        mp.y1 = std::max(mp.y1, v[i].y);
        mp.u0 = std::min(mp.u0, v[i].u);
        mp.u1 = std::max(mp.u1, v[i].u);
        mp.v0 = std::min(mp.v0, v[i].v);
        mp.v1 = std::max(mp.v1, v[i].v);
    }
    mp.yuv = m.format == PS2E_DRAW_MOVIE_YUV420;
    for (int p = 0; p < m.planes_per_slot; ++p) mp.planes[p] = m.planes[size_t(m.current) * size_t(m.planes_per_slot) + size_t(p)];
    // the title's TEX1 choice, or the Display page's override (as Translate takes it for a texture)
    mp.linear = d->applied.texture_filter == 1 ? false : d->applied.texture_filter == 2 ? true : st->linear != 0;
    if (!d->device->DrawMoviePicture(mp)) {
        d->movie_refused++;
        d->counters.refused++;
        return -2;
    }
    d->movie_drawn++;
    d->counters.draws++;
    d->counters.triangles += 2;
    return 2;
}

int ps2e_draw_submit(ps2e_draw* d, const ps2e_draw_state* st, const ps2e_draw_vertex* v, int count) {
    if (!d || !st || !v || count < 3) {
        if (d) d->counters.refused++;
        return -1;
    }
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
    if ((st->texture & ps2e_draw::kMovieIdMask) == ps2e_draw::kMovieIdBase) return SubmitMovie(d, st, v, count);
    if (st->stencil == PS2E_DRAW_STENCIL_CLEAR) {
        // The counter's origin (15: counts 1..16 set bit 4, as the GS byte 120 + 8n set bit 7 for n in 1..16)
        TimerPhase(d, PS2E_DRAW_PHASE_COMPOSITE);
#if !PS2R_HOST_VITA
        // a bracket whose region the producer gave (ps2e_draw_shadow_region): the origin and the casters' mark over its
        // rectangles only -- outside them nothing of this bracket reads the counter or the caster bits
        d->bracket_region = d->next_shadow_region && !d->stencil_diag;
        d->bracket_rects.swap(d->next_shadow_rects);
        d->next_shadow_rects.clear();
        d->next_shadow_region = false;
        if (d->bracket_region) {
            double area = 0;
            const std::vector<render::Rect> rects = PhysicalRects(d, d->bracket_rects.data(), int(d->bracket_rects.size() / 4), &area);
            TimerFill(d, PS2E_DRAW_PHASE_COMPOSITE, area);
            if (d->early_casters) {
                render::StencilState origin;
                origin.enabled = true;
                origin.func = render::StencilState::kAlways;
                origin.ref = kStencilOrigin;
                origin.write_mask = kCounterMask | kCasterBit;
                origin.pass_op = render::StencilState::kReplace;
                for (const render::Rect& r : rects) d->device->DrawConstQuad(render::SurfaceId{}, d->depth, r, 0, false, origin);
            } else {
                d->device->ClearStencilIn(d->depth, kStencilOrigin, rects.data(), int(rects.size()));
            }
            d->device->MarkStencilWhereOpaqueIn(d->color, d->depth, kCasterBit, rects.data(), int(rects.size()));
            d->counters.draws++;
            return 0;
        }
#endif
        TimerFill(d, PS2E_DRAW_PHASE_COMPOSITE, TargetPixels(d));   // (the casters' mark: a pass over the whole target)
        StencilProbe(d, "before-clear");
        if (d->early_casters) {
            // the counter and the caster bit to the origin, the early mark kept
            render::StencilState origin;
            origin.enabled = true;
            origin.func = render::StencilState::kAlways;
            origin.ref = kStencilOrigin;
            origin.write_mask = kCounterMask | kCasterBit;
            origin.pass_op = render::StencilState::kReplace;
            const int w = d->frame_w * d->scale, h = d->frame_h * d->scale;
            d->device->DrawConstQuad(render::SurfaceId{}, d->depth, render::Rect{0, 0, w, h}, 0, false, origin);
        } else {
            d->device->ClearStencil(d->depth, kStencilOrigin);
        }
        // (the Vita's backend cannot read the frame here: its casters are re-stamped at the composite, ResetCasterCounts)
        SeedCasterMask(d);
#if PS2R_HOST_VITA
        d->volume_boxes.clear();
#endif
        StencilProbe(d, "after-clear");
        d->counters.draws++;
        return 0;
    }
#if !PS2R_HOST_VITA
    if (st->stencil == PS2E_DRAW_STENCIL_INSIDE && d->bracket_region) {
        // the bracket's composite over its rectangles (one draw each, the state's own scissor kept inside them): the
        // counter is off its origin nowhere else
        d->bracket_region = false;
        TimerPhase(d, PS2E_DRAW_PHASE_COMPOSITE);
        const bool own = st->scissor_x0 | st->scissor_y0 | st->scissor_x1 | st->scissor_y1;
        int drawn = 0;
        for (size_t i = 0; i + 3 < d->bracket_rects.size(); i += 4) {
            ps2e_draw_state rs = *st;
            // the GS scissor: inclusive frame pixels
            rs.scissor_x0 = std::max(d->bracket_rects[i], own ? st->scissor_x0 : 0);
            rs.scissor_y0 = std::max(d->bracket_rects[i + 1], own ? st->scissor_y0 : 0);
            rs.scissor_x1 = std::min(d->bracket_rects[i + 2] - 1, own ? st->scissor_x1 : d->frame_w - 1);
            rs.scissor_y1 = std::min(d->bracket_rects[i + 3] - 1, own ? st->scissor_y1 : d->frame_h - 1);
            if (rs.scissor_x1 < rs.scissor_x0 || rs.scissor_y1 < rs.scissor_y0) continue;
            TimerFill(d, PS2E_DRAW_PHASE_COMPOSITE,
                      double(rs.scissor_x1 - rs.scissor_x0 + 1) * d->scale * double(rs.scissor_y1 - rs.scissor_y0 + 1) * d->scale);
            if (SubmitTriangles(d, &rs, v, count, nullptr) >= 0) drawn++;
        }
        return drawn;
    }
#endif
    if (d->gpu_timers && st->stencil != PS2E_DRAW_STENCIL_OFF) {
        if (st->stencil == PS2E_DRAW_STENCIL_INSIDE) {
            TimerPhase(d, PS2E_DRAW_PHASE_COMPOSITE);
            TimerFill(d, PS2E_DRAW_PHASE_COMPOSITE, TriangleArea(v, count) * d->scale * d->scale);
        } else {
            TimerPhase(d, PS2E_DRAW_PHASE_VOLUMES);
            TimerFill(d, PS2E_DRAW_PHASE_VOLUMES, TriangleArea(v, count) * d->scale * d->scale);
        }
    }
#if PS2R_HOST_VITA
    if (d->record_casters && !d->replaying) {
        if ((st->stencil == PS2E_DRAW_STENCIL_COUNT_UP || st->stencil == PS2E_DRAW_STENCIL_COUNT_DOWN) && !d->volume_boxes_given)
            NoteVolume(d, v, count);
        if (st->stencil == PS2E_DRAW_STENCIL_INSIDE) ResetCasterCounts(d);   // before the composite reads the counter
    }
    d->frame_draws++;
    if (st->blend && st->blend_c == 2) {
        d->frame_fix_blends++;
        if (st->texture) d->frame_fix_blends_textured++;
    }
    if (d->record_casters && !d->replaying && !d->far_background) RecordCaster(d, st, v, count);
    if (FixDarkening(d, st)) {
        d->fix_scratch.assign(v, v + count);
        for (ps2e_draw_vertex& x : d->fix_scratch) x.a = st->blend_fix;
        d->frame_fix++;
        return SubmitTriangles(d, st, d->fix_scratch.data(), count, nullptr);
    }
    // a vertex draw of the mesh run's context between two of its mesh draws (a clipped part's CPU-built run among its
    // material's native draws, the sky's or a chunk's of another texture): the GL context it leaves is the run's -- its
    // packets stay drawable after it (the backend checks the context field for field before it replays one)
    const bool keeps_run = d->packets && d->mesh_run && st->stencil == PS2E_DRAW_STENCIL_OFF &&
                           (d->mesh_run_draws == d->counters.draws || d->packet_run_draws == d->counters.draws) &&
                           SameContextState(&d->mesh_run_state, st);
    const int r = SubmitTriangles(d, st, v, count, nullptr);
    if (keeps_run && r >= 0) d->packet_run_draws = d->counters.draws;
    return r;
#else
    return SubmitTriangles(d, st, v, count, nullptr);
#endif
}

uint32_t ps2e_draw_mesh_create(ps2e_draw* d, const ps2e_draw_mesh_vertex* verts, int count) {
    static_assert(sizeof(ps2e_draw_mesh_vertex) == sizeof(render::MeshVertex), "the mesh vertex is the backend's");
    if (!d || !d->device || !verts || count < 3) return 0;
    const uint32_t id = d->device->CreateMesh(reinterpret_cast<const render::MeshVertex*>(verts), size_t(count / 3 * 3));
    if (id) {
        // its vertices' largest alpha, unlit and lit, and its model-space box (the casters' re-stamp; the sky's wait)
        ps2e_draw::MeshBounds b;
        for (int k = 0; k < 3; ++k) {
            b.lo[k] = 1e30f;
            b.hi[k] = -1e30f;
        }
        b.min_a = b.min_la = 0xFF;
        for (int i = 0; i < count / 3 * 3; ++i) {
            b.a = std::max(b.a, verts[i].a);
            b.la = std::max(b.la, verts[i].la);
            b.min_a = std::min(b.min_a, verts[i].a);
            b.min_la = std::min(b.min_la, verts[i].la);
            const float p[3] = {verts[i].x, verts[i].y, verts[i].z};
            for (int k = 0; k < 3; ++k) {
                b.lo[k] = std::min(b.lo[k], p[k]);
                b.hi[k] = std::max(b.hi[k], p[k]);
            }
        }
        d->mesh_bounds[id] = b;
    }
    return id;
}

void ps2e_draw_mesh_destroy(ps2e_draw* d, uint32_t mesh) {
    if (d) {
        d->mesh_bounds.erase(mesh);
        ps2e_draw::BoundSlot& s = d->bound_slots[mesh & (ps2e_draw::kBoundSlots - 1)];
        if (s.id == mesh) s = ps2e_draw::BoundSlot{};
    }
    if (d && d->device && mesh) d->device->DestroyMesh(mesh);
}

// ---- prepared meshes (ps2e_draw.h ps2e_draw_mesh_prepare) ------------------------------------------------------------
struct ps2e_draw_mesh_prepared {
    std::vector<render::MeshVertex> vertices;   // the distinct vertices (indexed), or the triangle list itself
    std::vector<uint16_t> indices;              // the list's indices into `vertices`; empty: `vertices` is the list
    int count = 0;                              // the triangle list's vertices (whole triangles)
};

ps2e_draw_mesh_prepared* ps2e_draw_mesh_prepare(const ps2e_draw_mesh_vertex* verts, int count) {
    auto* p = new ps2e_draw_mesh_prepared;
    if (!verts || count < 3) return p;   // (count 0: nothing to make, as ps2e_draw_mesh_create refuses it)
    const int n = count / 3 * 3;
    p->count = n;
    const auto* list = reinterpret_cast<const render::MeshVertex*>(verts);
#if PS2R_HOST_VITA
    // the deduplication the Vita's device makes at the creation (GlDevice::CreateMesh: the same function)
    if (render::MeshIndices(list, size_t(n), false, p->vertices, p->indices)) {
        p->vertices.shrink_to_fit();
        return p;
    }
#endif
    p->indices.clear();
    p->vertices.assign(list, list + n);
    return p;
}

uint32_t ps2e_draw_mesh_create_prepared(ps2e_draw* d, const ps2e_draw_mesh_prepared* p) {
    if (!d || !d->device || !p || p->count < 3) return 0;
#if PS2R_HOST_VITA
    uint32_t id = 0;
    if (!p->indices.empty())
        id = d->device->CreateMeshIndexed(p->vertices.data(), p->vertices.size(), p->indices.data(), size_t(p->count));
    if (!id) {
        // the device makes its meshes otherwise (or indexing did not pay): from the list, recovered exactly
        if (p->indices.empty()) {
            id = d->device->CreateMesh(p->vertices.data(), size_t(p->count));
        } else {
            std::vector<render::MeshVertex> list(size_t(p->count));
            for (int i = 0; i < p->count; ++i) list[size_t(i)] = p->vertices[p->indices[size_t(i)]];
            id = d->device->CreateMesh(list.data(), list.size());
        }
    }
    if (id) {
        // the bounds ps2e_draw_mesh_create keeps, made here as there (this thread, its floating-point mode) over the
        // distinct vertices: the list's values without its repeats -- a repeat of a vertex already compared never moves a
        // running min / max (it compares as the value it repeats), so the bounds are the list's
        ps2e_draw::MeshBounds b;
        for (int k = 0; k < 3; ++k) {
            b.lo[k] = 1e30f;
            b.hi[k] = -1e30f;
        }
        b.min_a = b.min_la = 0xFF;
        for (const render::MeshVertex& v : p->vertices) {
            b.a = std::max(b.a, v.a);
            b.la = std::max(b.la, v.la);
            b.min_a = std::min(b.min_a, v.a);
            b.min_la = std::min(b.min_la, v.la);
            const float q[3] = {v.x, v.y, v.z};
            for (int k = 0; k < 3; ++k) {
                b.lo[k] = std::min(b.lo[k], q[k]);
                b.hi[k] = std::max(b.hi[k], q[k]);
            }
        }
        d->mesh_bounds[id] = b;
    }
    return id;
#else
    return ps2e_draw_mesh_create(d, reinterpret_cast<const ps2e_draw_mesh_vertex*>(p->vertices.data()), p->count);
#endif
}

void ps2e_draw_mesh_prepared_free(ps2e_draw_mesh_prepared* p) { delete p; }
int ps2e_draw_mesh_prepared_count(const ps2e_draw_mesh_prepared* p) { return p ? p->count : 0; }
size_t ps2e_draw_mesh_prepared_bytes(const ps2e_draw_mesh_prepared* p) {
    return p ? p->vertices.capacity() * sizeof(render::MeshVertex) + p->indices.capacity() * sizeof(uint16_t) : 0;
}

// A mesh draw's full path under `st` (the stencil state as SubmitTriangles takes it)
static int SubmitMeshFull(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const render::MeshDrawParams& m,
                          const render::StencilState* stencil) {
    d->mesh_run = false;
    gs::DrawIr& ir = d->ir;
    gs::TextureSource ts;
    if (PrepareRecord(d, st, ts) < 0) return -2;
    ir.verts.clear();
    ir.vertex_count = 0;
    render::DrawParams p = TargetParams(d, st);
    if (stencil) p.stencil_state = *stencil;
    const render::DrawResult r = d->device->DrawMesh(ir, ts, p, mesh, m);
    d->counters.draws++;
    if (!r.drew) {
        d->counters.refused++;
        return -3;
    }
    d->mesh_run = true;
    d->mesh_run_state = *st;
    d->mesh_run_draws = d->counters.draws;
#if PS2R_HOST_VITA
    d->run_gen++;   // (the run's state changed: the packet memo's choice read it)
#endif
    return 1;
}

static render::MeshDrawParams MeshParams(const ps2e_draw_mesh_params* mp) {
    // The two structs lay their fields out alike (the asserts): one copy of the whole record, then the two fields whose
    // types differ (`lit`: an int there, a bool here; first / count: negative there = 0) -- every mesh draw makes one
    // (~1000 a frame in the title's island pan), and the field-by-field form zero-filled the 192 bytes first
    static_assert(sizeof(render::MeshDrawParams) == sizeof(ps2e_draw_mesh_params), "the mesh params' layouts");
    static_assert(offsetof(render::MeshDrawParams, offset) == offsetof(ps2e_draw_mesh_params, offset) &&
                  offsetof(render::MeshDrawParams, scroll) == offsetof(ps2e_draw_mesh_params, scroll) &&
                  offsetof(render::MeshDrawParams, lit) == offsetof(ps2e_draw_mesh_params, lit) &&
                  offsetof(render::MeshDrawParams, light_dir) == offsetof(ps2e_draw_mesh_params, light_dir) &&
                  offsetof(render::MeshDrawParams, light_colour) == offsetof(ps2e_draw_mesh_params, light_colour) &&
                  offsetof(render::MeshDrawParams, ambient) == offsetof(ps2e_draw_mesh_params, ambient) &&
                  offsetof(render::MeshDrawParams, box) == offsetof(ps2e_draw_mesh_params, box) &&
                  offsetof(render::MeshDrawParams, first) == offsetof(ps2e_draw_mesh_params, first) &&
                  offsetof(render::MeshDrawParams, count) == offsetof(ps2e_draw_mesh_params, count),
                  "the mesh params' fields at the same offsets");
    static_assert(std::is_trivially_copyable<render::MeshDrawParams>::value, "the mesh params copy as bytes");
    render::MeshDrawParams m;
    std::memcpy(static_cast<void*>(&m), mp, sizeof m);
    m.lit = mp->lit != 0;
    m.first = mp->first > 0 ? uint32_t(mp->first) : 0u;
    m.count = mp->count > 0 ? uint32_t(mp->count) : 0u;
    return m;
}

#if PS2R_HOST_VITA
namespace {
// two states' bytes equal -- memcmp's answer for equality, word by word (the struct is 28 aligned words; every
// mesh draw compared two to four states through the library's memcmp)
typedef uint32_t __attribute__((may_alias)) StateWord;
constexpr int kStateWords = int(sizeof(ps2e_draw_state) / 4);
static_assert(sizeof(ps2e_draw_state) == 112 && alignof(ps2e_draw_state) >= 4, "the state compares as 28 words");
inline bool SameState(const ps2e_draw_state* a, const ps2e_draw_state* b) {
    const StateWord* x = reinterpret_cast<const StateWord*>(a);
    const StateWord* y = reinterpret_cast<const StateWord*>(b);
    uint32_t diff = 0;
    for (int i = 0; i < kStateWords; ++i) diff |= x[i] ^ y[i];
    return diff == 0;
}
// The texture-swap probe's answer without the probe: `run` with st's texture (and, unless texture_only, st's alpha test and
// mipmapping) byte-equal to st -- every word but those fields' compared, alpha_reference's word but its byte (its padding
// came from `run` in the probe)
static_assert(offsetof(ps2e_draw_state, texture) == 0 && offsetof(ps2e_draw_state, alpha_test) == 28 &&
              offsetof(ps2e_draw_state, alpha_method) == 32 && offsetof(ps2e_draw_state, alpha_reference) == 36 &&
              offsetof(ps2e_draw_state, alpha_fail) == 40 && offsetof(ps2e_draw_state, mip_max_level) == 96 &&
              offsetof(ps2e_draw_state, mip_k) == 100 && offsetof(ps2e_draw_state, mip_l) == 104 &&
              offsetof(ps2e_draw_state, mip_minify) == 108, "the swap fields' words");
inline bool SameButSwapFields(const ps2e_draw_state* run, const ps2e_draw_state* st, bool texture_only) {
    const StateWord* x = reinterpret_cast<const StateWord*>(run);
    const StateWord* y = reinterpret_cast<const StateWord*>(st);
    uint32_t diff = 0;
    for (int i = 1; i < 7; ++i) diff |= x[i] ^ y[i];
    if (texture_only) {
        for (int i = 7; i < kStateWords; ++i) diff |= x[i] ^ y[i];
    } else {
        diff |= (x[9] ^ y[9]) & 0xFFFFFF00u;   // (little-endian: the byte at 36 is the word's lowest)
        for (int i = 11; i < 24; ++i) diff |= x[i] ^ y[i];
    }
    return diff == 0;
}
// the packet cache's slot of a (state, lit): FNV-1a over the state's words (its padding included, as the compare of two
// states). (a four-lane form of it -- a quarter of the dependent multiplies -- spread the hub's states worse over
// the 1024 slots: "no packet kept" 8 -> 40 a frame; this one stays)
uint32_t PacketSlot(const ps2e_draw_state* st, uint8_t lit) {
    const StateWord* w = reinterpret_cast<const StateWord*>(st);
    uint32_t h = 2166136261u ^ lit;
    for (int i = 0; i < kStateWords; ++i) h = (h ^ w[i]) * 16777619u;
    return (h ^ (h >> 16)) & (ps2e_draw::kPacketSlots - 1);
}
// the packet the mesh draw just made recorded (render::Backend::TakeMeshPacket), kept for its (state, lit) -- every mesh
// draw of ps2e_draw_submit_mesh takes it (the backend hands over only the packet of the draw just made)
// the cache entry kept for (st, lit), or null -- its set's two ways (the hit becomes the set's most recent)
const ps2e_draw::PacketEntry* FindPacket(ps2e_draw* d, const ps2e_draw_state* st, uint8_t lit) {
    if (d->packet_cache.empty()) return nullptr;
    const uint32_t set = PacketSlot(st, lit) & (ps2e_draw::kPacketSlots / 2 - 1);
    for (uint32_t w = 0; w < 2; ++w) {
        const ps2e_draw::PacketEntry& e = d->packet_cache[2 * set + w];
        if (e.packet && e.lit == lit && SameState(&e.state, st)) {
            d->packet_mru[set] = uint8_t(w);
            return &e;
        }
    }
    return nullptr;
}
void KeepPacket(ps2e_draw* d, const ps2e_draw_state* st, const ps2e_draw_mesh_params* mp) {
    const uint32_t id = d->device->TakeMeshPacket();
    if (!id || !d->packets || st->stencil != PS2E_DRAW_STENCIL_OFF) return;
    if (d->packet_cache.empty()) {
        d->packet_cache.resize(ps2e_draw::kPacketSlots);
        d->packet_mru.assign(ps2e_draw::kPacketSlots / 2, 0);
    }
    const uint8_t lit = mp->lit ? 1 : 0;
    // the way that holds this (state, lit) already, else an empty one, else the set's least recently used
    const uint32_t set = PacketSlot(st, lit) & (ps2e_draw::kPacketSlots / 2 - 1);
    ps2e_draw::PacketEntry* const ways = &d->packet_cache[2 * set];
    int w = -1;
    for (int k = 0; k < 2 && w < 0; ++k)
        if (ways[k].packet && ways[k].lit == lit && SameState(&ways[k].state, st)) w = k;
    for (int k = 0; k < 2 && w < 0; ++k)
        if (!ways[k].packet) w = k;
    if (w < 0) w = d->packet_mru[set] ^ 1;
    ps2e_draw::PacketEntry& e = ways[w];
    e.state = *st;
    e.lit = lit;
    e.packet = id;
    d->packet_mru[set] = uint8_t(w);
    d->run_gen++;   // (the packet memo's choice read the cache)
}
// a mesh draw drawn from `packet`: the run's count, the repeats census (the same packet and mesh range as the draw just
// before: an instanced draw's candidates)
void NotePacketDraw(ps2e_draw* d, uint32_t packet, uint32_t mesh, const render::MeshDrawParams& m) {
    if (d->last_packet_draws == d->counters.draws && d->last_packet == packet && d->last_packet_mesh == mesh &&
        d->last_packet_first == m.first && d->last_packet_count == m.count)
        d->packet_repeats++;
    d->counters.draws++;
    d->mesh_run_draws = d->counters.draws;
    d->last_packet = packet;
    d->last_packet_mesh = mesh;
    d->last_packet_first = m.first;
    d->last_packet_count = m.count;
    d->last_packet_draws = d->counters.draws;
}
}  // namespace
#endif

int ps2e_draw_submit_mesh(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const ps2e_draw_mesh_params* mp) {
    if (!d || !st || !mesh || !mp || st->stencil == PS2E_DRAW_STENCIL_CLEAR) {
        if (d) d->counters.refused++;
        return -1;
    }
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
#if PS2R_HOST_VITA
    d->frame_draws++;
    if (st->blend && st->blend_c == 2) {
        d->frame_fix_blends++;
        if (st->texture) d->frame_fix_blends_textured++;
    }
    if (d->record_casters && !d->replaying) RecordCasterMesh(d, st, mesh, mp);
#endif
    const render::MeshDrawParams m = MeshParams(mp);
    // the run as the slow paths take it (nothing drawn since its last mesh draw), and -- the Vita's packets -- the run past
    // vertex draws of its own state (the backend vouches for its GL context field for field)
    const bool run_live = d->mesh_run && d->mesh_run_draws == d->counters.draws;
#if PS2R_HOST_VITA
    const bool packet_live = run_live || (d->mesh_run && d->packet_run_draws == d->counters.draws);
    const uint8_t lit8 = mp->lit ? 1 : 0;
    bool memo_tried = false;   // (declined: the cache's choice would be the same packet -- not asked twice)
    if (packet_live && d->packets && d->pk_memo && d->pk_memo_gen == d->run_gen && d->pk_memo_lit == lit8 &&
        SameState(&d->pk_memo_state, st)) {
        // the packet memo (see ps2e_draw): the last packet draw's very state, nothing its choice read changed since
        memo_tried = true;
        if (d->device->DrawMeshPacket(d->pk_memo_packet, mesh, m)) {
            d->pk_memo_hits++;
            NotePacketDraw(d, d->pk_memo_packet, mesh, m);
            return 1;
        }
    }
#else
    const bool packet_live = run_live;
#endif
    if (packet_live) {
        // the run's state (the GL context the backend set up last) as it stands, or with another texture: at level 2 (the
        // default) the alpha test and the mipmapping may differ too -- the alpha test feeds only the register uniforms (u_ate
        // / u_atst / u_aref / u_afail) and the mipmapping only the texture's sampler and uniforms, which the short path sets
        // as the full one does (the census: ~3/4 of the full mesh draws a hub frame differ from the draw before in texture +
        // alpha test + mipmaps alone)
#if PS2R_HOST_VITA
        const bool same = SameState(&d->mesh_run_state, st);
#else
        const bool same = std::memcmp(&d->mesh_run_state, st, sizeof *st) == 0;
#endif
        bool swap = false;
        if (!same && d->texture_swap && st->texture && d->mesh_run_state.texture && st->texture != PS2E_DRAW_TEXTURE_FRAME_COPY &&
            d->mesh_run_state.texture != PS2E_DRAW_TEXTURE_FRAME_COPY) {
#if PS2R_HOST_VITA
            // (the probe below, word by word without the probe's copy)
            swap = SameButSwapFields(&d->mesh_run_state, st, d->texture_swap < 2);
#else
            ps2e_draw_state probe = d->mesh_run_state;
            probe.texture = st->texture;
            if (d->texture_swap >= 2) {
                probe.alpha_test = st->alpha_test;
                probe.alpha_method = st->alpha_method;
                probe.alpha_reference = st->alpha_reference;
                probe.alpha_fail = st->alpha_fail;
                probe.mip_max_level = st->mip_max_level;
                probe.mip_k = st->mip_k;
                probe.mip_l = st->mip_l;
                probe.mip_minify = st->mip_minify;
            }
            swap = std::memcmp(&probe, st, sizeof *st) == 0;
#endif
        }
#if PS2R_HOST_VITA
        if ((same || swap) && d->packets && !memo_tried) {
            // a draw the again / texture-swap path would make: its packet (recorded when this state last drew raw), drawn
            // in the run's GL context as it stands -- nothing set up, the run left untouched (the slow paths below stay
            // relative to mesh_run_state)
            const uint8_t lit = lit8;
            const ps2e_draw::PacketEntry* e = FindPacket(d, st, lit);
            if (e) {
                if (d->device->DrawMeshPacket(e->packet, mesh, m)) {
                    // the memo of this choice (its state, lit, packet; what it read: run_gen)
                    d->pk_memo = true;
                    d->pk_memo_state = *st;
                    d->pk_memo_lit = lit;
                    d->pk_memo_packet = e->packet;
                    d->pk_memo_gen = d->run_gen;
                    NotePacketDraw(d, e->packet, mesh, m);
                    return 1;
                }
            } else {
                d->packets_missing++;
            }
        }
#endif
        if (same && run_live) {
            const render::DrawResult again = d->device->DrawMeshAgain(mesh, m);
            if (again.drew) {
                d->counters.draws++;
                d->mesh_run_draws = d->counters.draws;
#if PS2R_HOST_VITA
                KeepPacket(d, st, mp);
#endif
                return 1;
            }
        } else if (swap && run_live) {
            gs::TextureSource ts;
            if (PrepareRecord(d, st, ts) < 0) return -2;
            d->ir.verts.clear();
            d->ir.vertex_count = 0;
            const render::DrawResult swapped = d->device->DrawMeshTexture(d->ir, ts, mesh, m);
            if (swapped.drew) {
                d->counters.draws++;
                d->mesh_run_state = *st;
                d->mesh_run_draws = d->counters.draws;
#if PS2R_HOST_VITA
                d->run_gen++;   // (the run's state changed: the packet memo's choice read it)
                KeepPacket(d, st, mp);
#endif
                return 1;
            }
        }
    }
#if PS2R_HOST_VITA
    {
        // the census: why this draw takes the full path
        int why = 0;
        if (d->mesh_run && !packet_live) {
            why = 1;
        } else if (packet_live) {
            ps2e_draw_state probe = d->mesh_run_state;
            probe.texture = st->texture;
            probe.alpha_test = st->alpha_test;
            probe.alpha_method = st->alpha_method;
            probe.alpha_reference = st->alpha_reference;
            probe.alpha_fail = st->alpha_fail;
            probe.mip_max_level = st->mip_max_level;
            probe.mip_k = st->mip_k;
            probe.mip_l = st->mip_l;
            probe.mip_minify = st->mip_minify;
            const bool swap_fields_only = std::memcmp(&probe, st, sizeof *st) == 0;
            const bool same = std::memcmp(&d->mesh_run_state, st, sizeof *st) == 0;
            why = !swap_fields_only ? 2 : (!same && (!st->texture || !d->mesh_run_state.texture ||
                                                     st->texture == PS2E_DRAW_TEXTURE_FRAME_COPY ||
                                                     d->mesh_run_state.texture == PS2E_DRAW_TEXTURE_FRAME_COPY || d->texture_swap < 2))
                                              ? 3 : 4;
        }
        d->full_reasons[why]++;
    }
#endif
    const int r = SubmitMeshFull(d, st, mesh, m, nullptr);
#if PS2R_HOST_VITA
    if (r > 0) KeepPacket(d, st, mp);
#endif
    return r;
}

int ps2e_draw_submit_mesh_instances(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh,
                                    const ps2e_draw_mesh_params* const* params, int n) {
    int refused = 0;
    if (!params || n <= 0) return 0;
#if PS2R_HOST_VITA
    int k = 0;
    bool declined = false;   // (the device declined this run once: the rest one by one)
    while (k < n) {
        // Once the packet memo holds this state (the run's first draw set it, or an earlier draw of it), the rest of the run
        // (up to kMaxInstances a draw) is what each of its draws would make -- the memo's packet with its own transform:
        // drawn as ONE instanced draw of that packet, then each draw's bookkeeping as ps2e_draw_submit_mesh's around it.
        // And when the memo does not hold it, the choice ps2e_draw_submit_mesh makes next for the run's first draw
        // -- the run's context as it stands (the same state, or a texture swap of it) and the packet kept for (state, unlit)
        // -- asked without drawing: the packet it would draw from, so the WHOLE run is one instanced draw of it (the memo then
        // set as that first draw would have set it; the cache's lookup made once, as the first draw makes it)
        const int rest = n - k;
        const ps2e_draw_mesh_params* const p0 = params[k];
        const bool live = d && d->mesh_run && (d->mesh_run_draws == d->counters.draws || d->packet_run_draws == d->counters.draws);
        if (rest >= 2 && !declined && d && st && mesh && p0 && !p0->lit && d->in_frame && d->packets && live &&
            st->stencil == PS2E_DRAW_STENCIL_OFF) {
            uint32_t packet = 0;
            bool from_cache = false;
            if (d->pk_memo && d->pk_memo_gen == d->run_gen && d->pk_memo_lit == 0 && SameState(&d->pk_memo_state, st)) {
                packet = d->pk_memo_packet;
            } else {
                // (ps2e_draw_submit_mesh's second choice, word for word: the run's state, or its texture swap)
                const bool same = SameState(&d->mesh_run_state, st);
                const bool swap = !same && d->texture_swap && st->texture && d->mesh_run_state.texture &&
                                  st->texture != PS2E_DRAW_TEXTURE_FRAME_COPY &&
                                  d->mesh_run_state.texture != PS2E_DRAW_TEXTURE_FRAME_COPY &&
                                  SameButSwapFields(&d->mesh_run_state, st, d->texture_swap < 2);
                if (same || swap) {
                    if (const ps2e_draw::PacketEntry* e = FindPacket(d, st, 0)) {
                        packet = e->packet;
                        from_cache = true;
                    }
                }
            }
            if (!packet) {
                // (no packet to instance: the first draw the plain way -- it records its packet -- then the rest)
                if (ps2e_draw_submit_mesh(d, st, mesh, params[k]) < 0) refused++;
                ++k;
                continue;
            }
            const int m = std::min(rest, ps2e_draw::kMaxInstances);
            render::MeshInstance inst[ps2e_draw::kMaxInstances];
            for (int i = 0; i < m; ++i) {
                const ps2e_draw_mesh_params* p = params[k + i];
                inst[i] = render::MeshInstance{p->screen, p->offset, p->scroll, p->box};
            }
            const render::MeshDrawParams m0 = MeshParams(p0);   // (the range: the run's first / count)
            if (d->device->DrawMeshPacketInstances(packet, mesh, m0.first, m0.count, inst, m)) {
                if (from_cache) {
                    // the memo of this choice, as the first draw's DrawMeshPacket would have left it
                    d->pk_memo = true;
                    d->pk_memo_state = *st;
                    d->pk_memo_lit = 0;
                    d->pk_memo_packet = packet;
                    d->pk_memo_gen = d->run_gen;
                }
                for (int i = 0; i < m; ++i) {
                    d->frame_draws++;
                    if (st->blend && st->blend_c == 2) {
                        d->frame_fix_blends++;
                        if (st->texture) d->frame_fix_blends_textured++;
                    }
                    if (d->record_casters && !d->replaying) RecordCasterMesh(d, st, mesh, params[k + i]);
                    NotePacketDraw(d, packet, mesh, m0);
                }
                k += m;
                continue;
            }
            declined = true;
        }
        if (ps2e_draw_submit_mesh(d, st, mesh, params[k]) < 0) refused++;
        ++k;
    }
#else
    for (int k = 0; k < n; ++k)
        if (ps2e_draw_submit_mesh(d, st, mesh, params[k]) < 0) refused++;
#endif
    return refused;
}

int ps2e_draw_mesh_instancing(const ps2e_draw* d) { return d && d->device && d->device->InstancedPacketDraws() ? 1 : 0; }

int ps2e_draw_mesh_screen_box(ps2e_draw* d, uint32_t mesh, const ps2e_draw_mesh_params* mp, float box[4]) {
#if PS2R_HOST_VITA
    if (!d || !mesh || !mp || !box) return 0;
    const ps2e_draw::MeshBounds* found = FindBounds(d, mesh);   // (the direct-mapped slots: no map division per draw)
    if (!found) return 0;
    const ps2e_draw::ScreenBox b = MeshBoxOf(*found, mp);
    if (!(b.x1 - b.x0 < 1e29f)) return 0;   // a corner at or behind the eye: anywhere
    box[0] = b.x0;
    box[1] = b.y0;
    box[2] = b.x1;
    box[3] = b.y1;
    return 1;
#else
    (void)d; (void)mesh; (void)mp; (void)box;
    return 0;
#endif
}

int ps2e_draw_mesh_bounds(ps2e_draw* d, uint32_t mesh, float lo[3], float hi[3]) {
#if PS2R_HOST_VITA
    if (!d || !mesh || !lo || !hi) return 0;
    const ps2e_draw::MeshBounds* found = FindBounds(d, mesh);
    if (!found || found->skin) return 0;
    for (int k = 0; k < 3; ++k) {
        lo[k] = found->lo[k];
        hi[k] = found->hi[k];
    }
    return 1;
#else
    (void)d; (void)mesh; (void)lo; (void)hi;
    return 0;
#endif
}

int ps2e_draw_mesh_caster_candidate(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const ps2e_draw_mesh_params* mp) {
#if PS2R_HOST_VITA
    // RecordCasterMesh's own tests, without recording (the room aside: a draw that would be a caster is one whatever the room)
    if (!d || !st || !mesh || !mp || !d->record_casters || d->replaying) return 0;
    const CasterNeed n = CasterNeedOf(d, st);
    if (!n.caster) return 0;
    if (n.need_av > 0) {
        const ps2e_draw::MeshBounds* mb = FindBounds(d, mesh);
        const int av = !mb ? 0xFF : (mp->lit ? mb->la : mb->a);
        if (av < n.need_av) return 0;
    }
    return 1;
#else
    (void)d; (void)st; (void)mesh; (void)mp;
    return 0;
#endif
}

int ps2e_draw_state_caster(ps2e_draw* d, const ps2e_draw_state* st) {
#if PS2R_HOST_VITA
    // CasterNeedOf's answer for the state alone (a vertex draw's vertices' alpha not looked at: 1 = it may record one)
    if (!d || !st || !d->record_casters || d->replaying) return 0;
    return CasterNeedOf(d, st).caster ? 1 : 0;
#else
    (void)d; (void)st;
    return 0;
#endif
}

int ps2e_draw_caster_room(const ps2e_draw* d) {
#if PS2R_HOST_VITA
    if (!d || !d->record_casters) return 0;
    return d->casters.size() < d->max_casters ? int(d->max_casters - d->casters.size()) : 0;
#else
    (void)d;
    return 0;
#endif
}

int ps2e_draw_mesh_depth_range(ps2e_draw* d, uint32_t mesh, const ps2e_draw_mesh_params* mp, float* z_near, float* z_far) {
#if PS2R_HOST_VITA
    if (!d || !mesh || !mp || !z_near || !z_far) return 0;
    const ps2e_draw::MeshBounds* found = FindBounds(d, mesh);   // (the direct-mapped slots: no map division per draw)
    if (!found || found->skin) return 0;
    const ps2e_draw::MeshBounds& b = *found;
    const float* m = mp->screen;
    // each corner's terms are one of two products per axis -- formed once here, then summed per corner in the
    // expression's order (m[14] + x * m[2] + y * m[6] + z * m[10]): the same roundings, the same values, 12 products
    // where the corners formed 48 (the sky's wait asks this of every scene draw, the draw order of every punch-through one)
    const float zx[2] = {b.lo[0] * m[2], b.hi[0] * m[2]}, zy[2] = {b.lo[1] * m[6], b.hi[1] * m[6]};
    const float zz[2] = {b.lo[2] * m[10], b.hi[2] * m[10]};
    const float wx[2] = {b.lo[0] * m[3], b.hi[0] * m[3]}, wy[2] = {b.lo[1] * m[7], b.hi[1] * m[7]};
    const float wz[2] = {b.lo[2] * m[11], b.hi[2] * m[11]};
    float zn = -1e30f, zf = 1e30f;
    for (int k = 0; k < 8; ++k) {
        const int ix = k & 1, iy = (k >> 1) & 1, iz = (k >> 2) & 1;
        const float cz = m[14] + zx[ix] + zy[iy] + zz[iz];
        const float cw = m[15] + wx[ix] + wy[iy] + wz[iz];
        if (!(cw > 1e-6f) || !std::isfinite(cz)) return 0;
        const float sz = cz / cw;
        zn = std::max(zn, sz);
        zf = std::min(zf, sz);
    }
    *z_near = zn;
    *z_far = zf;
    return 1;
#else
    (void)d; (void)mesh; (void)mp; (void)z_near; (void)z_far;
    return 0;
#endif
}

int ps2e_draw_mesh_depth_far_above(ps2e_draw* d, uint32_t mesh, const ps2e_draw_mesh_params* mp, float z_min) {
    if (!d || !mesh || !mp) return -1;
    const ps2e_draw::MeshBounds* found = FindBounds(d, mesh);
    if (!found || found->skin) return -1;
    const ps2e_draw::MeshBounds& b = *found;
    const float* m = mp->screen;
    // ps2e_draw_mesh_depth_range's corners (the same products and sums); a corner is above when its z / w is: z >= z_min * w
    // says so without the division when z_min is a power of two (the product exact, the rounding of z / w monotonic), the
    // division decides the rest -- the same answer as z_far >= z_min, for every corner valid
    int exp = 0;
    const bool pow2 = z_min > 0.f && std::frexp(z_min, &exp) == 0.5f;
    const float zx[2] = {b.lo[0] * m[2], b.hi[0] * m[2]}, zy[2] = {b.lo[1] * m[6], b.hi[1] * m[6]};
    const float zz[2] = {b.lo[2] * m[10], b.hi[2] * m[10]};
    const float wx[2] = {b.lo[0] * m[3], b.hi[0] * m[3]}, wy[2] = {b.lo[1] * m[7], b.hi[1] * m[7]};
    const float wz[2] = {b.lo[2] * m[11], b.hi[2] * m[11]};
    bool below = false;
    for (int k = 0; k < 8; ++k) {
        const int ix = k & 1, iy = (k >> 1) & 1, iz = (k >> 2) & 1;
        const float cz = m[14] + zx[ix] + zy[iy] + zz[iz];
        const float cw = m[15] + wx[ix] + wy[iy] + wz[iz];
        if (!(cw > 1e-6f) || !std::isfinite(cz)) return -1;   // (depth_range: not known, whatever the other corners)
        if (below || (pow2 && cz >= z_min * cw)) continue;
        if (!(cz / cw >= z_min)) below = true;
    }
    return below ? 0 : 1;
}

int ps2e_draw_skin_depth_far(ps2e_draw* d, uint32_t mesh, const ps2e_draw_mesh_params* mp, const float* bones, int joints,
                             float* z_far) {
#if PS2R_HOST_VITA
    if (!d || !mesh || !mp || !bones || joints <= 0 || joints > 64 || !z_far) return 0;
    const ps2e_draw::MeshBounds* b = FindBounds(d, mesh);
    if (!b || !b->skin || !b->convex || !(b->lo[0] <= b->hi[0])) return 0;
    // a vertex = sum of w_k (joint_k's 3x4 . p) with convex weights: inside the hull of every joint's image of the bind box,
    // so its z / w (linear-fractional) is at least the smallest over those images' corners (all in front)
    const float* m = mp->screen;
    float zf = 1e30f;
    for (int j = 0; j < joints; ++j) {
        const float* r = bones + size_t(j) * 12;   // rows k = (axis0[k], axis1[k], axis2[k], t[k])
        for (int k = 0; k < 8; ++k) {
            const float x = (k & 1) ? b->hi[0] : b->lo[0], y = (k & 2) ? b->hi[1] : b->lo[1], z = (k & 4) ? b->hi[2] : b->lo[2];
            const float px = r[0] * x + r[1] * y + r[2] * z + r[3];
            const float py = r[4] * x + r[5] * y + r[6] * z + r[7];
            const float pz = r[8] * x + r[9] * y + r[10] * z + r[11];
            const float cz = m[14] + px * m[2] + py * m[6] + pz * m[10];
            const float cw = m[15] + px * m[3] + py * m[7] + pz * m[11];
            if (!(cw > 1e-6f) || !std::isfinite(cz)) return 0;
            zf = std::min(zf, cz / cw);
        }
    }
    *z_far = zf;
    return 1;
#else
    (void)d; (void)mesh; (void)mp; (void)bones; (void)joints; (void)z_far;
    return 0;
#endif
}

int ps2e_draw_alpha_floor(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, int lit, const ps2e_draw_vertex* v, int count) {
#if PS2R_HOST_VITA
    if (!d || !st) return -1;
    // the vertex colour's alpha byte (PS2 scale), lowered by one for the iterators' rounding
    int va = -1;
    if (mesh) {
        const ps2e_draw::MeshBounds* b = FindBounds(d, mesh);
        if (!b || b->skin) return -1;
        va = lit ? b->min_la : b->min_a;
    } else {
        if (!v || count < 3) return -1;
        va = 0xFF;
        for (int i = 0; i < count; ++i) va = std::min(va, int(v[i].a));
    }
    va = std::max(0, va - 1);
    if (!st->texture) return va;   // untextured: the vertex alpha
    if (st->texture == PS2E_DRAW_TEXTURE_FRAME_COPY || st->tfx != PS2E_DRAW_TFX_MODULATE) return -1;
    if (!st->tcc) return va;       // MODULATE without the texture's alpha: the vertex alpha
    const ps2e_draw::Texture* tex = FindTexture(d, st->texture);
    if (!tex || !tex->alpha_seen) return -1;
    // the lean programs' MODULATE alpha: floor(vc.a * floor(tex.a_gpu * 0.5019685) / 128), at most 255 -- from the smallest
    // texel (a filtered fetch lies between its texels; one byte off for the filter's rounding)
    const int ga = std::max(0, int(tex->min_gpu_alpha) - 1);
    const int ta = int(std::floor(float(ga) * 0.5019685f));
    return std::min(255, (va * ta) / 128);
#else
    (void)d; (void)st; (void)mesh; (void)lit; (void)v; (void)count;
    return -1;
#endif
}

#if PS2R_HOST_VITA
// A skin mesh draw's full path under `st` (the stencil state as SubmitTriangles takes it): the device's DrawSkinMesh
static int SubmitSkinFull(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const render::MeshDrawParams& m,
                          const float* bones, int joints, const float* disp, int disp_count, const render::StencilState* stencil) {
    d->mesh_run = false;   // (no mesh draw takes a skin draw's state as its own)
    gs::DrawIr& ir = d->ir;
    gs::TextureSource ts;
    if (PrepareRecord(d, st, ts) < 0) return -2;
    ir.verts.clear();
    ir.vertex_count = 0;
    render::DrawParams p = TargetParams(d, st);
    if (stencil) p.stencil_state = *stencil;
    const render::DrawResult r = d->device->DrawSkinMesh(ir, ts, p, mesh, m, bones, joints, disp, disp_count);
    d->counters.draws++;
    if (!r.drew) {
        d->counters.refused++;
        return -3;
    }
    return 1;
}
#endif

uint32_t ps2e_draw_skin_mesh_create(ps2e_draw* d, const ps2e_draw_skin_vertex* verts, int count, const uint16_t* indices,
                                    int index_count, int max_alpha) {
    static_assert(sizeof(ps2e_draw_skin_vertex) == sizeof(render::SkinVertex), "the skin vertex is the backend's");
#if PS2R_HOST_VITA
    if (!d || !d->device || !verts || count < 3 || !indices || index_count < 3) return 0;
    const uint32_t id = d->device->CreateSkinMesh(reinterpret_cast<const render::SkinVertex*>(verts), size_t(count), indices,
                                                  size_t(index_count / 3 * 3));
    if (id) {
        // the casters' re-stamp: its largest alpha (a skin's alpha is its vertices' own, lit or not); drawn anywhere
        ps2e_draw::MeshBounds b;
        b.a = b.la = uint8_t(std::max(0, std::min(255, max_alpha)));
        b.skin = true;
        // its bind-pose box and whether its weights are convex (ps2e_draw_skin_depth_far; MeshBoxOf / the depth range keep
        // a skin "anywhere" whatever the box)
        b.convex = true;
        for (int k = 0; k < 3; ++k) { b.lo[k] = 1e30f; b.hi[k] = -1e30f; }
        for (int i = 0; i < count; ++i) {
            const ps2e_draw_skin_vertex& v = verts[i];
            const float p[3] = {v.x, v.y, v.z};
            for (int k = 0; k < 3; ++k) { b.lo[k] = std::min(b.lo[k], p[k]); b.hi[k] = std::max(b.hi[k], p[k]); }
            if (!(v.w0 >= 0.f && v.w1 >= 0.f && v.w2 >= 0.f && std::fabs(v.w0 + v.w1 + v.w2 - 1.f) <= 1e-4f) || v.index != 0.f)
                b.convex = false;
        }
        d->mesh_bounds[id] = b;
    }
    return id;
#else
    (void)d; (void)verts; (void)count; (void)indices; (void)index_count; (void)max_alpha;
    return 0;
#endif
}

int ps2e_draw_submit_skin(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const ps2e_draw_mesh_params* mp,
                          const float* bones, int joints, const float* disp, int disp_count) {
#if PS2R_HOST_VITA
    if (!d || !st || !mesh || !mp || !bones || joints <= 0 || joints > 64 || disp_count < 0 || disp_count > 64 ||
        (disp_count && !disp) || st->stencil == PS2E_DRAW_STENCIL_CLEAR) {
        if (d) d->counters.refused++;
        return -1;
    }
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
    d->frame_draws++;
    if (st->blend && st->blend_c == 2) {
        d->frame_fix_blends++;
        if (st->texture) d->frame_fix_blends_textured++;
    }
    if (d->record_casters && !d->replaying) RecordCasterMesh(d, st, mesh, mp, bones, joints, disp, disp_count);
    render::MeshDrawParams m = MeshParams(mp);
    m.lit = true;
    // a skin of the mesh run's context between two of its mesh draws (Crash among the world's draws): the GL fixed-function
    // context it leaves is the run's -- the run's packets stay drawable after it (the backend checks the context field for
    // field); the slow paths' run ends as before (the skin's program, arrays and uniforms are its own)
    const bool keeps_run = d->packets && d->mesh_run && st->stencil == PS2E_DRAW_STENCIL_OFF &&
                           (d->mesh_run_draws == d->counters.draws || d->packet_run_draws == d->counters.draws) &&
                           SameContextState(&d->mesh_run_state, st);
    // a skin of the run's context: its packet (recorded when this state last drew raw), replayed in the run's GL context as
    // it stands -- the context its full path would set up (the backend checks it field for field); the bookkeeping after
    // it is the full path's (the packets alone stay drawable)
    if (keeps_run) {
        const ps2e_draw::PacketEntry* e =
            d->skin_packet_cache.empty() ? nullptr : &d->skin_packet_cache[PacketSlot(st, 2) & (ps2e_draw::kSkinPacketSlots - 1)];
        if (e && e->packet && std::memcmp(&e->state, st, sizeof *st) == 0) {
            if (d->device->DrawSkinPacket(e->packet, mesh, m, bones, joints, disp, disp_count)) {
                d->counters.draws++;
                d->mesh_run = true;
                d->packet_run_draws = d->counters.draws;
                return 1;
            }
        } else {
            d->skin_packets_missing++;
        }
    }
    const int r = SubmitSkinFull(d, st, mesh, m, bones, joints, disp, disp_count, nullptr);
    if (r > 0) {
        // the packet the draw just made recorded (a raw skin draw's), kept for its state
        const uint32_t id = d->device->TakeSkinPacket();
        if (id && d->packets && st->stencil == PS2E_DRAW_STENCIL_OFF) {
            if (d->skin_packet_cache.empty()) d->skin_packet_cache.resize(ps2e_draw::kSkinPacketSlots);
            ps2e_draw::PacketEntry& e = d->skin_packet_cache[PacketSlot(st, 2) & (ps2e_draw::kSkinPacketSlots - 1)];
            e.state = *st;
            e.lit = 2;
            e.packet = id;
        }
    }
    if (keeps_run && r > 0) {
        d->mesh_run = true;                           // (SubmitSkinFull ended it: kept for the packets alone --
        d->packet_run_draws = d->counters.draws;      //  mesh_run_draws is behind counters.draws, so no slow path takes it)
    }
    return r;
#else
    (void)st; (void)mesh; (void)mp; (void)bones; (void)joints; (void)disp; (void)disp_count;
    if (d) d->counters.refused++;
    return -1;
#endif
}

#if PS2R_HOST_VITA
namespace {
// The casters drawn again into the stencil (see CasterNeed) right before the bracket's composite, where a volume of the
// bracket was counted: the counter put back to its origin under every caster pixel, as the DATE never counted there
bool Overlaps(const ps2e_draw::ScreenBox& a, const ps2e_draw::ScreenBox& b) {
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

// the re-stamp run (ResetCasterCounts): on unless TE_GFX_VITA_RESTAMP_RUN=0; its census (ps2e_draw_restamp_census)
bool RestampRun() {
    static const bool on = [] {
        const char* e = std::getenv("TE_GFX_VITA_RESTAMP_RUN");
        return !(e && e[0] == '0');
    }();
    return on;
}
struct RestampCensus {
    uint64_t runs = 0, mesh_full = 0, mesh_packet = 0, skin_full = 0, skin_packet = 0, vertex = 0;
};
RestampCensus g_restamp;
// the run's last draw is the draw just made (through a full path or a packet: ps2e_draw_submit_mesh's packet_live)
bool RestampRunLive(const ps2e_draw* d) {
    return d->mesh_run && (d->mesh_run_draws == d->counters.draws || d->packet_run_draws == d->counters.draws);
}
// a mesh caster's re-stamp from its state's packet under ps2e_draw_submit_mesh's conditions (the run live, the state the
// run's or a texture swap of it); true: drawn
bool RestampMeshPacket(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const ps2e_draw_mesh_params* mp,
                       const render::MeshDrawParams& m) {
    if (!d->packets || !RestampRunLive(d)) return false;
    const bool same = SameState(&d->mesh_run_state, st);
    bool swap = false;
    if (!same && d->texture_swap && st->texture && d->mesh_run_state.texture && st->texture != PS2E_DRAW_TEXTURE_FRAME_COPY &&
        d->mesh_run_state.texture != PS2E_DRAW_TEXTURE_FRAME_COPY)
        swap = SameButSwapFields(&d->mesh_run_state, st, d->texture_swap < 2);
    if (!same && !swap) return false;
    const ps2e_draw::PacketEntry* e = FindPacket(d, st, mp->lit ? 1 : 0);
    if (!e || !d->device->DrawMeshPacket(e->packet, mesh, m)) return false;
    NotePacketDraw(d, e->packet, mesh, m);
    return true;
}
// a skin caster's re-stamp from its state's skin packet under ps2e_draw_submit_skin's conditions (the run live, the state
// of the run's GL context); true: drawn
bool RestampSkinPacket(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const render::MeshDrawParams& m, const float* bones,
                       int joints, const float* disp, int disp_count) {
    if (!d->packets || !RestampRunLive(d) || !SameContextState(&d->mesh_run_state, st) || d->skin_packet_cache.empty()) return false;
    const ps2e_draw::PacketEntry& e = d->skin_packet_cache[PacketSlot(st, 2) & (ps2e_draw::kSkinPacketSlots - 1)];
    if (!e.packet || std::memcmp(&e.state, st, sizeof *st) != 0) return false;
    if (!d->device->DrawSkinPacket(e.packet, mesh, m, bones, joints, disp, disp_count)) return false;
    d->counters.draws++;
    d->mesh_run = true;
    d->packet_run_draws = d->counters.draws;
    return true;
}
// the packet a skin's full draw just recorded, kept for its state (ps2e_draw_submit_skin's)
void KeepSkinPacket(ps2e_draw* d, const ps2e_draw_state* st) {
    const uint32_t id = d->device->TakeSkinPacket();
    if (!id || !d->packets || st->stencil != PS2E_DRAW_STENCIL_OFF) return;
    if (d->skin_packet_cache.empty()) d->skin_packet_cache.resize(ps2e_draw::kSkinPacketSlots);
    ps2e_draw::PacketEntry& e = d->skin_packet_cache[PacketSlot(st, 2) & (ps2e_draw::kSkinPacketSlots - 1)];
    e.state = *st;
    e.lit = 2;
    e.packet = id;
}

void ResetCasterCounts(ps2e_draw* d) {
    d->census_brackets++;
    unsigned cpu = 0, meshes = 0, vertices = 0;
    if (!d->casters.empty() && !d->volume_boxes.empty()) {
        render::StencilState reset;
        reset.enabled = true;
        reset.func = render::StencilState::kAlways;
        reset.ref = kStencilOrigin;
        reset.cmp_mask = 0xFF;
        reset.write_mask = kCounterMask;
        reset.pass_op = render::StencilState::kReplace;
        reset.depth_fail_op = render::StencilState::kKeep;
        // the volumes' union, padded by 32 pixels: a vertex draw (a skin) is near when one of its vertices lies inside it
        // -- a caster's own volume reaches from its body, so its first vertices usually answer (a full box of every
        // skin's vertices cost the drawing thread ~0.7 ms a frame on x86 Vita3K); a skin's triangles are far smaller
        // than the pad, so one crossing the union without a vertex in the padded box is not expected
        ps2e_draw::ScreenBox all = d->volume_boxes.front();
        for (const ps2e_draw::ScreenBox& v : d->volume_boxes)
            all = ps2e_draw::ScreenBox{std::min(all.x0, v.x0), std::min(all.y0, v.y0), std::max(all.x1, v.x1), std::max(all.y1, v.y1)};
        const ps2e_draw::ScreenBox pad{all.x0 - 32.f, all.y0 - 32.f, all.x1 + 32.f, all.y1 + 32.f};
        d->replaying = true;
        // THE RE-STAMP RUN (TE_GFX_VITA_RESTAMP_RUN=0: each re-stamp its own stencil set and reset, as before): the
        // reset stencil set ONCE around the bracket's re-stamps -- each re-stamp is drawn under the very state its own set gave
        // it, and nothing else draws between them -- so the re-stamps are a run of draws like any other: a mesh caster's
        // re-stamp whose state drew raw before (a packet kept for it: an earlier frame's re-stamp of it) is replayed from
        // its packet where ps2e_draw_submit_mesh would replay one (the run live, its state the run's or a texture swap of
        // it: the GL context its full path would set up), a skin caster's from its skin packet where ps2e_draw_submit_skin
        // would; the full path keeps the packet it records. The same GXM state at every re-stamp draw: the same pictures.
        const bool run = RestampRun();
        const render::StencilState* const each = run ? nullptr : &reset;
        if (run) {
            d->device->ApplyStencilState(reset);
            g_restamp.runs++;
        }
        // THE CHARACTERS FIRST: the skins (GPU skins and the vertex draws -- the CPU-built skin parts, a clipped part's runs)
        // then the scenery meshes. A re-stamp writes the counter's origin wherever its fragments pass, so the order changes
        // no stencil value -- only which casters the frame's budget (kMaxRestamps) reaches when more than it are near
        bool budget_left = d->frame_restamps < kMaxRestamps;
        for (int pass = 0; pass < 2 && budget_left; ++pass)
        for (const ps2e_draw::CasterDraw& c : d->casters) {
            if ((!c.mesh || c.bones) != (pass == 0)) continue;
            bool near = false;
            if (c.mesh) {
                // (outside the volumes' union it overlaps none of them: one test for the far scenery)
                if (Overlaps(c.box, all))
                    for (const ps2e_draw::ScreenBox& v : d->volume_boxes)
                        if (Overlaps(c.box, v)) {
                            near = true;
                            break;
                        }
            } else {
                // every 4th vertex: a skin overlapping the volumes does so with many small triangles, one of whose tested
                // corners lies in the padded union; the far casters (clipped FBA instances: ~12k vertices a hub frame)
                // cost a quarter of a full pass -- answered from those vertices' extent where it decides:
                // wholly outside the padded union none is in it, wholly inside all are (the same answer as the scan)
                const ps2e_draw::ScreenBox& b4 = c.box4;
                if (b4.x1 < pad.x0 || b4.x0 > pad.x1 || b4.y1 < pad.y0 || b4.y0 > pad.y1) {
                    near = false;
                } else if (b4.x0 >= pad.x0 && b4.x1 <= pad.x1 && b4.y0 >= pad.y0 && b4.y1 <= pad.y1) {
                    near = c.count > 0;
                } else {
                    const ps2e_draw_vertex* cv = c.verts ? c.verts : d->caster_verts.data() + c.first;
                    for (uint32_t i = 0; i < c.count && !near; i += 4)
                        near = cv[i].x >= pad.x0 && cv[i].x <= pad.x1 && cv[i].y >= pad.y0 && cv[i].y <= pad.y1;
                }
            }
            if (!near) continue;
            if (d->frame_restamps >= kMaxRestamps) {
                static bool said = false;
                if (!said) {
                    said = true;
                    std::fprintf(stderr, "[ps2e] the casters' re-stamp draws at most %u casters a frame (present %llu has more near "
                                 "its shadow volumes)\n", unsigned(kMaxRestamps), (unsigned long long)d->presented);
                }
                budget_left = false;
                break;
            }
            d->frame_restamps++;
            if (c.mesh) {
                if (c.bones) {
                    render::MeshDrawParams m = MeshParams(&c.mp);
                    m.lit = true;
                    if (run && RestampSkinPacket(d, &c.state, c.mesh, m, c.bones, c.joints, c.disp, c.disp_count)) {
                        g_restamp.skin_packet++;
                    } else {
                        const bool kept_run = run && RestampRunLive(d) && SameContextState(&d->mesh_run_state, &c.state);
                        const int r = SubmitSkinFull(d, &c.state, c.mesh, m, c.bones, c.joints, c.disp, c.disp_count, each);
                        if (run && r > 0) {
                            KeepSkinPacket(d, &c.state);
                            if (kept_run) {
                                d->mesh_run = true;                        // (as ps2e_draw_submit_skin keeps it: for the
                                d->packet_run_draws = d->counters.draws;   //  packets alone)
                            }
                        }
                        g_restamp.skin_full++;
                    }
                } else {
                    const render::MeshDrawParams m = MeshParams(&c.mp);
                    if (run && RestampMeshPacket(d, &c.state, c.mesh, &c.mp, m)) {
                        g_restamp.mesh_packet++;
                    } else {
                        const int r = SubmitMeshFull(d, &c.state, c.mesh, m, each);
                        if (run && r > 0) KeepPacket(d, &c.state, &c.mp);
                        g_restamp.mesh_full++;
                    }
                }
                meshes++;
                d->caster_mesh_replays++;
            } else {
                SubmitTriangles(d, &c.state, c.verts ? c.verts : d->caster_verts.data() + c.first, int(c.count), each);
                cpu++;
                vertices += c.count;
                g_restamp.vertex++;
            }
        }
        if (run) d->device->ResetStencilState();
        d->replaying = false;
        d->mesh_run = false;
    }
    if (d->census_every > 0 && d->census_brackets % uint64_t(d->census_every) == 0) {
        unsigned kept_cpu = 0, kept_vertices = 0;
        for (const ps2e_draw::CasterDraw& c : d->casters)
            if (!c.mesh) {
                kept_cpu++;
                kept_vertices += c.count;
            }
        std::fprintf(stderr, "[ps2e.casters] bracket %llu (present %llu): %u of %u casters re-stamped (%u of %u vertex draws, %u of %u "
                     "vertices; %u meshes; %u this frame, at most %u) over %u volume boxes, after %llu draws; Cd - Cs.FIX draws %llu; "
                     "FIX blends %llu (textured %llu)\n",
                     (unsigned long long)d->census_brackets, (unsigned long long)d->presented, cpu + meshes, unsigned(d->casters.size()),
                     cpu, kept_cpu, vertices, kept_vertices, meshes, d->frame_restamps, unsigned(kMaxRestamps),
                     unsigned(d->volume_boxes.size()), (unsigned long long)d->frame_draws,
                     (unsigned long long)d->frame_fix, (unsigned long long)d->frame_fix_blends,
                     (unsigned long long)d->frame_fix_blends_textured);
    }
}
}  // namespace
#endif

int ps2e_draw_mark_casters_early(ps2e_draw* d) {
    if (!d || !d->in_frame) return 0;
#if PS2R_HOST_VITA
    return 0;   // (the re-stamp keeps these casters already: drawn again where they are still the nearest surface)
#else
    d->mesh_run = false;
    TimerPhase(d, PS2E_DRAW_PHASE_COMPOSITE);
    TimerFill(d, PS2E_DRAW_PHASE_COMPOSITE, TargetPixels(d));
    d->device->ClearStencil(d->depth, 0);
    d->early_casters = d->device->MarkStencilWhereOpaque(d->color, d->depth, kEarlyCasterBit);
    TimerResume(d);
    return d->early_casters ? 1 : 0;
#endif
}

int ps2e_draw_mark_casters_early_in(ps2e_draw* d, const int* rects, int n) {
    if (!d || !d->in_frame) return 0;
#if PS2R_HOST_VITA
    (void)rects;
    (void)n;
    return 0;   // (the re-stamp keeps these casters already)
#else
    if (d->stencil_diag) return ps2e_draw_mark_casters_early(d);
    d->mesh_run = false;
    d->early_casters = false;
    if (n <= 0) return 0;   // no volume this frame: nothing reads the early casters
    double area = 0;
    const std::vector<render::Rect> phys = PhysicalRects(d, rects, n, &area);
    if (phys.empty()) return 0;
    TimerPhase(d, PS2E_DRAW_PHASE_COMPOSITE);
    TimerFill(d, PS2E_DRAW_PHASE_COMPOSITE, area);
    d->device->ClearStencilIn(d->depth, 0, phys.data(), int(phys.size()));
    d->early_casters = d->device->MarkStencilWhereOpaqueIn(d->color, d->depth, kEarlyCasterBit, phys.data(), int(phys.size()));
    TimerResume(d);
    return d->early_casters ? 1 : 0;
#endif
}

void ps2e_draw_shadow_region(ps2e_draw* d, const int* rects, int n) {
    if (!d) return;
    d->next_shadow_rects.assign(rects ? rects : nullptr, rects ? rects + 4 * std::max(0, n) : nullptr);
    d->next_shadow_region = true;
}

void ps2e_draw_restamp_census(const ps2e_draw* d, uint64_t out[6]) {
    if (!out) return;
#if PS2R_HOST_VITA
    (void)d;
    out[0] = g_restamp.runs;
    out[1] = g_restamp.mesh_full;
    out[2] = g_restamp.mesh_packet;
    out[3] = g_restamp.skin_full;
    out[4] = g_restamp.skin_packet;
    out[5] = g_restamp.vertex;
#else
    (void)d;
    for (int k = 0; k < 6; ++k) out[k] = 0;
#endif
}

void ps2e_draw_set_far_background(ps2e_draw* d, int on) {
#if PS2R_HOST_VITA
    if (d) d->far_background = on != 0;
#else
    (void)d;
    (void)on;
#endif
}

void ps2e_draw_set_retained_vertices(ps2e_draw* d, int on) {
#if PS2R_HOST_VITA
    if (d) d->retained = on != 0;
#else
    (void)d;
    (void)on;
#endif
}

void ps2e_draw_set_volume_boxes_given(ps2e_draw* d, int on) {
#if PS2R_HOST_VITA
    if (d) d->volume_boxes_given = on != 0;
#else
    (void)d; (void)on;
#endif
}

void ps2e_draw_note_volume_box(ps2e_draw* d, const ps2e_draw_vertex* v, int count) {
#if PS2R_HOST_VITA
    if (d && d->record_casters && !d->replaying && v && count >= 3) NoteVolume(d, v, count);
#else
    (void)d; (void)v; (void)count;
#endif
}

void ps2e_draw_note_volume_rect(ps2e_draw* d, const float minmax[4]) {
#if PS2R_HOST_VITA
    // (BoxOf's widening by a pixel for the rasteriser's rounding)
    if (d && d->record_casters && !d->replaying && minmax)
        NoteVolumeBox(d, ps2e_draw::ScreenBox{minmax[0] - 1.f, minmax[1] - 1.f, minmax[2] + 1.f, minmax[3] + 1.f});
#else
    (void)d; (void)minmax;
#endif
}

int ps2e_draw_submit_finite(ps2e_draw* d, const ps2e_draw_state* st, const ps2e_draw_vertex* v, int count) {
#if PS2R_HOST_VITA
    if (d) d->q_finite = v;
    const int r = ps2e_draw_submit(d, st, v, count);
    if (d) d->q_finite = nullptr;
    return r;
#else
    return ps2e_draw_submit(d, st, v, count);
#endif
}

ps2e_draw_vertex* ps2e_draw_reserve_vertices(ps2e_draw* d, int count) {
#if PS2R_HOST_VITA
    static_assert(sizeof(ps2e_draw_vertex) == sizeof(render::RawVertex), "the producer's vertex is the raw layout");
    if (!d || !d->device || count < 3) return nullptr;
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
    return reinterpret_cast<ps2e_draw_vertex*>(d->device->ReserveRaw(size_t(count)));
#else
    (void)d;
    (void)count;
    return nullptr;
#endif
}

void ps2e_draw_set_backend_option(ps2e_draw* d, const char* name, int value) {
    if (d && d->device) d->device->SetOption(name, value);
}

void ps2e_draw_set_mesh_texture_swap(ps2e_draw* d, int on) {
    if (d) d->texture_swap = on < 0 ? 0 : on > 2 ? 2 : on;
#if PS2R_HOST_VITA
    if (d) d->run_gen++;   // (the packet memo's choice read the level)
#endif
}

void ps2e_draw_mesh_full_census(const ps2e_draw* d, uint64_t reasons[5], uint64_t* casters) {
#if PS2R_HOST_VITA
    for (int k = 0; k < 5; ++k)
        if (reasons) reasons[k] = d ? d->full_reasons[k] : 0;
    if (casters) *casters = d ? d->caster_mesh_replays : 0;
#else
    (void)d;
    for (int k = 0; k < 5; ++k)
        if (reasons) reasons[k] = 0;
    if (casters) *casters = 0;
#endif
}

void ps2e_draw_skin_packet_census(const ps2e_draw* d, uint64_t* missing) {
#if PS2R_HOST_VITA
    if (missing) *missing = d ? d->skin_packets_missing : 0;
#else
    (void)d;
    if (missing) *missing = 0;
#endif
}

void ps2e_draw_mesh_packet_census(const ps2e_draw* d, uint64_t* missing, uint64_t* repeats) {
#if PS2R_HOST_VITA
    if (missing) *missing = d ? d->packets_missing : 0;
    if (repeats) *repeats = d ? d->packet_repeats : 0;
#else
    (void)d;
    if (missing) *missing = 0;
    if (repeats) *repeats = 0;
#endif
}

void ps2e_draw_lookup_census(const ps2e_draw* d, uint64_t out[5]) {
    if (!out) return;
    out[0] = out[1] = out[2] = out[3] = out[4] = 0;
    if (!d) return;
#if PS2R_HOST_VITA
    out[0] = d->bound_lookups;
    out[1] = d->bound_misses;
    out[4] = d->pk_memo_hits;
#endif
    out[2] = d->texture_lookups;
    out[3] = d->texture_misses;
}

int ps2e_draw_copy_frame(ps2e_draw* d) {
    if (!d || !d->frame_copy.surface.id) return 0;
    d->mesh_run = false;
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
#if PS2R_HOST_VITA
    if (!d->frame_announced && d->frame_copies == 0) {
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr, "[ps2e] a screen copy in a frame that did not announce it (ps2e_draw_expect_frame_copies): "
                         "the depth after it is lost in this frame, kept from the next one on\n");
        }
    }
#endif
    d->frame_copies++;
    // The draws so far are issued (the device draws as it goes; the vertex ring is flushed per draw), so the copy holds
    // every submit before this call and none after it.
    const render::Rect whole{0, 0, d->frame_copy.w, d->frame_copy.h};
    TimerPhase(d, PS2E_DRAW_PHASE_COPIES);
    TimerFill(d, PS2E_DRAW_PHASE_COPIES, double(whole.w) * whole.h);
    d->device->CopyRect(d->color, whole, d->frame_copy.surface, whole, 0);
    TimerResume(d);
    d->frame_copy_valid = true;
    return 1;
}

int ps2e_draw_copy_frame_region(ps2e_draw* d, const float box[4]) {
    if (!d || !d->frame_copy.surface.id) return 0;
    // the frame's first copy is whole: the texture holds the frame before then nowhere
    if (!box || !d->frame_copy_valid || !(box[2] >= box[0]) || !(box[3] >= box[1])) return ps2e_draw_copy_frame(d);
    d->mesh_run = false;
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
    // the region in the target's physical pixels: the frame's GS rows are the target's rows (the scanout flips them for the
    // window), every pixel the box touches, padded one frame pixel
    const int s = std::clamp(d->scale, 1, 4);
    const auto lo = [s](float v, int limit) { return std::clamp(int(std::floor(v)) - 1, 0, limit) * s; };
    const auto hi = [s](float v, int limit) { return std::clamp(int(std::ceil(v)) + 1, 0, limit) * s; };
    const int x0 = lo(box[0], d->frame_w), y0 = lo(box[1], d->frame_h);
    const int x1 = std::min(hi(box[2], d->frame_w), d->frame_copy.w), y1 = std::min(hi(box[3], d->frame_h), d->frame_copy.h);
    d->frame_copies++;
    if (x1 <= x0 || y1 <= y0) return 1;   // nothing of the frame inside: the texture holds it already
    const render::Rect r{x0, y0, x1 - x0, y1 - y0};
    TimerPhase(d, PS2E_DRAW_PHASE_COPIES);
    TimerFill(d, PS2E_DRAW_PHASE_COPIES, double(r.w) * r.h);
    d->device->CopyRect(d->color, r, d->frame_copy.surface, r, 0);
    TimerResume(d);
    return 1;
}

void ps2e_draw_expect_frame_copies(ps2e_draw* d, int copies) {
    if (d) d->expect_copies = copies > 0 ? copies : 0;
}

namespace {
// The present's body: the frame target scanned out -- to the window and swapped (to_window), or resolved for the armed
// capture only (ps2e_draw_capture_frame: a picture the producer measures but does not show).
void Scanout(ps2e_draw* d, bool to_window);
}  // namespace

void ps2e_draw_present(ps2e_draw* d) {
    if (!d) return;
    d->mesh_run = false;
#if PS2R_HOST_VITA
    if (d->census_every > 0 && (d->presented + 1) % uint64_t(d->census_every) == 0)
        std::fprintf(stderr, "[ps2e.frame] present %llu: %llu draws, %u casters kept, GS FIX blends %llu (textured %llu), "
                     "Cd - Cs.FIX draws %llu\n", (unsigned long long)(d->presented + 1), (unsigned long long)d->frame_draws,
                     unsigned(d->casters.size()), (unsigned long long)d->frame_fix_blends,
                     (unsigned long long)d->frame_fix_blends_textured, (unsigned long long)d->frame_fix);
#endif
    Scanout(d, true);
    d->presented++;
}

void ps2e_draw_capture_frame(ps2e_draw* d) {
    if (!d) return;
    Scanout(d, false);
}

namespace {
void Scanout(ps2e_draw* d, bool to_window) {
    if (!d->in_frame) {
        d->device->BeginFrame();
        d->device->BeginDrawRun();
        d->in_frame = true;
    }
    d->device->EndDrawRun();
    // The drawing thread's half of the presentation -- the swap interval, the picture's shape / fit / move, the
    // scan-out filter (the internal scale and the texture filter change at a frame's start: ps2e_draw_begin_frame).
    ps2e_draw_presentation want;
    {
        std::lock_guard<std::mutex> lock(d->presentation_mutex);
        want = d->wanted;
    }
    if (want.vsync != d->applied.vsync && d->context.IsCreated()) d->context.SetSwapInterval(want.vsync ? 1 : 0);
    const int filter_in_use = d->applied.texture_filter;   // the frame's draws were sampled with it; begin_frame moves it
    d->applied = want;
    d->applied.texture_filter = filter_in_use;
    render::ScanoutDesc sd;
    sd.source = d->color;
    sd.src = {0, 0, d->frame_w * d->scale, d->frame_h * d->scale};   // physical texels
    // The drawable size the window's thread noted (the pump), else asked once here (the creating thread, before any pump)
    int ww = d->frame_w, wh = d->frame_h;
    if (!d->headless && d->drawable_w > 0 && d->drawable_h > 0) {
        ww = d->drawable_w;
        wh = d->drawable_h;
    }
    if (ww <= 0 || wh <= 0) {
        ww = d->frame_w;
        wh = d->frame_h;
    }
    // The picture's SHAPE: the frame's own pixels by default, the console's TV (4:3: the frame is
    // anamorphic, 512 wide for a 4:3 TV at 448 or 512 lines) or another the player chose, or the window's shape.
    double aspect = double(d->frame_w) / d->frame_h;
    switch (want.aspect) {
    case PS2E_DRAW_ASPECT_4_3: aspect = 4.0 / 3.0; break;
    case PS2E_DRAW_ASPECT_16_9: aspect = 16.0 / 9.0; break;
    case PS2E_DRAW_ASPECT_16_10: aspect = 16.0 / 10.0; break;
    case PS2E_DRAW_ASPECT_21_9: aspect = 21.0 / 9.0; break;
    case PS2E_DRAW_ASPECT_WINDOW: aspect = double(ww) / wh; break;
    default: break;
    }
    int dw = 1, dh = 1;
    if (want.fit == PS2E_DRAW_FIT_FILL) {
        // the window covered, the picture's shape kept (its edges cropped)
        if (double(ww) / wh > aspect) { dw = ww; dh = std::max(1, int(ww / aspect + 0.5)); }
        else { dh = wh; dw = std::max(1, int(wh * aspect + 0.5)); }
    } else {
        // letterbox: the largest picture of that shape inside the window
        if (double(ww) / wh > aspect) { dh = wh; dw = std::max(1, int(wh * aspect + 0.5)); }
        else { dw = ww; dh = std::max(1, int(ww / aspect + 0.5)); }
        if (want.fit == PS2E_DRAW_FIT_INTEGER) {
            // whole multiples of the logical frame's height (the width follows the shape)
            const int k = std::max(1, dh / d->frame_h);
            dh = k * d->frame_h;
            dw = std::max(1, int(dh * aspect + 0.5));
        }
    }
    // MoveDisplay: the picture moved by frame pixels, scaled as the picture is
    const int ox = int(std::lround(double(want.offset_x) * dw / d->frame_w));
    const int oy = int(std::lround(double(want.offset_y) * dh / d->frame_h));
    sd.dst = {(ww - dw) / 2 + ox, (wh - dh) / 2 + oy, dw, dh};
    sd.filter = want.nearest ? render::PresentationFilter::Nearest : render::PresentationFilter::Linear;
    sd.dither16 = d->present_ct16;   // The retail CT16 scanout
    sd.dimx = d->dimx;
    sd.to_window = to_window;
    d->last_out_x = sd.dst.x;
    d->last_out_y = sd.dst.y;
    d->last_out_w = dw;
    d->last_out_h = dh;
    d->device->SetWindowSize(ww, wh);
    TimerFill(d, PS2E_DRAW_PHASE_PRESENT, double(dw) * dh);   // (the device marks the phase itself)
    d->device->Present(sd);
    d->device->EndFrame();
    d->in_frame = false;
}
}  // namespace

uint64_t ps2e_draw_frames_presented(const ps2e_draw* d) { return d ? d->presented : 0; }

void ps2e_draw_set_presentation(ps2e_draw* d, const ps2e_draw_presentation* p) {
    if (!d || !p) return;
    std::lock_guard<std::mutex> lock(d->presentation_mutex);
    d->wanted = *p;
    d->wanted.scale = std::clamp(p->scale, 0, 4);
}

void ps2e_draw_get_presentation(const ps2e_draw* d, ps2e_draw_presentation* out) {
    if (!out) return;
    if (!d) { *out = ps2e_draw_presentation{}; return; }
    std::lock_guard<std::mutex> lock(const_cast<ps2e_draw*>(d)->presentation_mutex);
    *out = d->wanted;
}

void ps2e_draw_last_output_rect(const ps2e_draw* d, int* x, int* y, int* w, int* h) {
    if (x) *x = d ? d->last_out_x : 0;
    if (y) *y = d ? d->last_out_y : 0;
    if (w) *w = d ? d->last_out_w : 0;
    if (h) *h = d ? d->last_out_h : 0;
}

void ps2e_draw_set_present_ct16(ps2e_draw* d, int enable, uint64_t dimx) {
    if (!d) return;
    d->present_ct16 = enable != 0;
    d->dimx = dimx ? dimx : render::kDefaultDimx;
}
int ps2e_draw_present_ct16(const ps2e_draw* d) { return d && d->present_ct16 ? 1 : 0; }

void ps2e_draw_arm_capture(ps2e_draw* d, int armed) {
    if (!d) return;
    d->capture = armed != 0;
    d->device->ArmPresentCapture(d->capture);
}

int ps2e_draw_read_presented(ps2e_draw* d, uint8_t* out, size_t capacity, int* w, int* h) {
    if (!d) return 0;
    std::vector<uint8_t> img;
    int iw = 0, ih = 0;
    if (!d->device->ReadPresented(&img, &iw, &ih) || iw <= 0 || ih <= 0) {
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (w) *w = iw;
    if (h) *h = ih;
    const size_t need = size_t(iw) * ih * 4;
    if (!out || capacity < need) return 0;
    // ReadPresented is bottom-up (GL rows); the contract is top row first.
    for (int y = 0; y < ih; ++y)
        std::memcpy(out + size_t(y) * iw * 4, img.data() + size_t(ih - 1 - y) * iw * 4, size_t(iw) * 4);
    return 1;
}

int ps2e_draw_write_ppm(ps2e_draw* d, const char* path) {
    if (!d || !path) return 0;
    int w = 0, h = 0;
    ps2e_draw_read_presented(d, nullptr, 0, &w, &h);
    if (w <= 0 || h <= 0) return 0;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    if (!ps2e_draw_read_presented(d, rgba.data(), rgba.size(), &w, &h)) return 0;
    FILE* f = std::fopen(path, "wb");
    if (!f) return 0;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (size_t i = 0; i < rgba.size(); i += 4) std::fwrite(&rgba[i], 1, 3, f);
    std::fclose(f);
    return 1;
}

int ps2e_draw_write_target_ppm(ps2e_draw* d, const char* path) {
    if (!d || !path || !d->color.id) return 0;
    const int w = d->frame_w * d->scale, h = d->frame_h * d->scale;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    d->device->Readback(d->color, render::Rect{0, 0, w, h}, rgba.data(), size_t(w) * 4);   // (the GS's rows are the target's)
    FILE* f = std::fopen(path, "wb");
    if (!f) return 0;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (size_t i = 0; i < rgba.size(); i += 4) std::fwrite(&rgba[i], 1, 3, f);
    std::fclose(f);
    return 1;
}

void ps2e_draw_set_gpu_timers(ps2e_draw* d, int mode) {
    if (!d || !d->device) return;
    d->device->SetOption("gpu_timers", mode);
    d->gpu_timers = d->device->GpuTimersOn();
}

int ps2e_draw_gpu_timers_on(const ps2e_draw* d) { return d && d->gpu_timers ? 1 : 0; }

void ps2e_draw_gpu_phase(ps2e_draw* d, int phase) {
    if (!d || !d->gpu_timers) return;
    d->producer_phase = phase;
    d->device->MarkGpuPhase(phase);
}

size_t ps2e_draw_gpu_timer_lines(ps2e_draw* d, double wall_ms, char* out, size_t capacity) {
    if (!d || !d->gpu_timers || !out || !capacity) return 0;
    const std::string lines = d->device->GpuTimerLines(wall_ms);
    const size_t n = std::min(lines.size(), capacity - 1);
    std::memcpy(out, lines.data(), n);
    out[n] = 0;
    return n;
}

const char* ps2e_draw_renderer(const ps2e_draw* d) { return d ? d->renderer.c_str() : ""; }

void ps2e_draw_target_size(const ps2e_draw* d, int* w, int* h, int* scale) {
    if (w) *w = d ? d->frame_w * d->scale : 0;
    if (h) *h = d ? d->frame_h * d->scale : 0;
    if (scale) *scale = d ? d->scale : 0;
}

void ps2e_draw_window_size(const ps2e_draw* d, int* pw, int* ph, int* dw, int* dh) {
    const bool shown = d && !d->headless;
    if (pw) *pw = shown ? d->window.Width() : 0;
    if (ph) *ph = shown ? d->window.Height() : 0;
    if (dw) *dw = shown ? d->drawable_w : 0;
    if (dh) *dh = shown ? d->drawable_h : 0;
}

void ps2e_draw_counts(const ps2e_draw* d, ps2e_draw_counters* out) {
    if (!out) return;
    *out = d ? d->counters : ps2e_draw_counters{};
}

void ps2e_draw_texture_memory_census(ps2e_draw* d, ps2e_draw_texture_memory* out, int reset_peak) {
    if (!out) return;
    *out = ps2e_draw_texture_memory{};
    if (!d) return;
    out->alive = d->textures.size();
    out->bytes = d->texture_bytes;
    out->vita_bytes = d->texture_vita_bytes;
    out->vita_peak = d->texture_vita_peak;
    out->made = d->textures_made;
    out->made_vita_bytes = d->textures_made_vita_bytes;
    out->destroyed = d->textures_destroyed;
    out->destroyed_vita_bytes = d->textures_destroyed_vita_bytes;
    if (reset_peak) d->texture_vita_peak = d->texture_vita_bytes;
}

}  // extern "C"
