// platform/nativeee (host side) -- the Vita's BATCHING CENSUS and INSTANCING PLAN over the drawing order
// (TE_GFX_VITA_BATCH_CENSUS, TE_GFX_VITA_INSTANCE).
//
// The device's GPU time follows the number of draws (Totem Hokum's village: ~800 draws a picture, ~23 ms of GPU work; the
// hub: ~350, ~13 ms -- the SGX543's per-draw command / tiler side, not its fill). A draw the GPU need not be handed is one
// of several draws of the SAME MESH RANGE under the SAME STATE (texture, programs, tests, blend: one packet) and the same
// UV shift that differ only in their matrix: one instanced draw (gl_shader_library.cpp RawReplayInstances: the matrices on
// an instance stream) draws them all, in their order.
//
// THE CENSUS (every TE_GFX_VITA_BATCH_CENSUS-th picture, default 32; 0 off; one line per census window): each bucket's
// drawing order as SubmitBuckets draws it (after graphics_zorder.cpp's order) --
//   * the unlit native mesh draws, the distinct (mesh, range, state, shift) tuples among them and the draws repeating one;
//   * the repeats right after their tuple's previous draw (what an instanced draw takes without any reordering);
//   * THE PLAN's answer (below): the draws an instanced draw takes when a draw may move back over draws whose screen boxes
//     it does not touch, and why the other repeats may not join (a box it would cross overlaps its own -- the depth-tie /
//     order risk --, a draw of unknown extent or a barrier between, the look-back, the sky's insertion point, a full group);
//   * the same with the lit meshes too (equal lights: a lit instanced program would be needed), the consecutive draws of one
//     state and one matrix but another mesh (one merged mesh would draw both: no program change), of one state with another
//     mesh and matrix (an index concatenation would need pre-transformed vertices), the distinct states.
//
// THE PLAN (TE_GFX_VITA_INSTANCE=1: drawn instanced; =2: the planned order drawn one draw at a time the plain way -- the
// order's own check; =abN[:M]: mode M (default 1) and the default drawing alternating every N pictures, each frame's arm on
// the per-present GPU A/B line -- the device's measurement; unset / 0: off). In each bucket's order a draw of an unlit
// native mesh joins the latest group of its tuple -- drawn at the group's first draw -- when every draw it moves back over
// (each draw drawn between the group's place and its own) has a known screen box disjoint from its own; else it starts a
// group. A group is drawn at its first draw's place, its draws in their order (instanced draws of up to 64).
// WHY NO PIXEL CAN CHANGE BY THE ORDER: a draw writes only pixels inside its screen box (a mesh's: its model-space bounds'
// eight corners through its matrix as the native vertex program takes them, padded 3 pixels for the rasteriser and these
// boxes' own roundings -- MeshBox; a vertex draw's: its vertices padded 1); two draws whose boxes are disjoint touch no
// common pixel -- neither its colour, its depth nor its stencil -- so their order changes no value either reads or writes. A
// moved draw crosses only such draws; the draws of one group keep their order; no draw crosses a screen copy (a later draw
// samples the frame as it stands), a stencil draw (the shadows' counters, the casters' re-stamp), a draw of unknown extent
// (a corner at or behind the eye, a GPU skin) or the sky's insertion point (graphics.cpp SkyMove). THE CASTERS' RECORDS: the
// frame keeps its first 128 shadow casters only (ps2e_draw.cpp CasterRoom), so where a bucket's possible casters outnumber the
// room left, a possible caster never moves over another (the kept set and its order stay); elsewhere two casters swap only
// when their boxes -- the re-stamps' boxes -- are disjoint.
// VITA3K (the picture oracle) IS NOT ORDER-INDEPENDENT HERE: the planned order (=2) drew the Totem route's gameplay pictures
// exactly, but left 1-3-level differences at a few pixels (beside the title logo) of ~3 % of the title's island-pan pictures
// where NO moved pair's boxes touch the changed pixels, the draws' paths are the same and making every GXM set changes
// nothing; boxes 8 px wider (TE_GFX_VITA_INSTANCE_PAD=8) leave 1 picture in ~2800 (title) / ~5800 (Totem) and cost most of
// the joins -- so the margin is a knob, not the default (cause not found; bisected without a result).
// What the order cannot vouch for is the instanced draw's own arithmetic (=1): its vertex program reads the matrix from an
// instance stream (primary attributes) where the mesh path reads uniforms -- the same instruction sequence (form 2),
// but Vita3K's translation of it rounds some vertices otherwise (a few edge pixels). =2 draws the plain way.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "graphics_internal.h"
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>   // sceKernelGetProcessTimeWide (the plan's own time)
#endif

namespace te::gfx {

#if defined(__vita__)
namespace {

struct BatchOptions {
    bool read = false;
    long census_every = 64;   // TE_GFX_VITA_BATCH_CENSUS: 1 picture in N censused (0: none)
    int mode = 0;             // TE_GFX_VITA_INSTANCE: 0 off, 1 instanced, 2 the planned order one draw at a time, 3 = abN
    long ab_every = 1;        // abN: the arms (1: mode ab_arm, 0: the default drawing) alternate every N pictures
    int ab_arm = 1;           // abN:M -- arm 1 is mode M (1 instanced, the default; 2 the planned order one draw at a time)
    int lookback = 128;       // TE_GFX_VITA_INSTANCE_LOOKBACK: the most planned entries a draw moves back over
    long dump = -1;           // TE_GFX_VITA_BATCH_DUMP=P: picture P's groups and refused repeats listed (diagnosis)
    // TE_GFX_VITA_INSTANCE_PAD=N: every box N pixels wider (default 0: MeshBox's 3 px -- the box rule itself). In Vita3K the
    // planned order leaves 1-3-level differences at a few pixels of rare pictures whatever the margin (0: ~3 % of the title
    // pan's pictures, none on the Totem route; 8: 1 picture of ~2800 / ~5800; 32: 1 of ~2800) where no moved pair's boxes
    // touch the changed pixels: the emulator's output depends on the order beyond the pixels the draws cover (bisected)
    float pad = 0.f;
    int cross = 2;            // (diagnosis) TE_GFX_VITA_INSTANCE_CROSS=unlit|mesh|all: what a draw may move over (0 / 1 / 2)
    uint32_t buckets = ~0u;   // (diagnosis) TE_GFX_VITA_INSTANCE_BUCKETS=hex mask: the buckets planned
    // TE_GFX_VITA_INSTANCE_DEPTH=1: the PLAN also lets a draw move back over a draw whose box overlaps its own when the two
    // are DEPTH-SAFE and their depth ranges are disjoint (DepthApart; default 0: the box rule alone). The census counts it
    // either way (its third pass).
    bool depth = false;
};
BatchOptions g_bo;

void ReadOptions() {
    g_bo.read = true;
    if (const char* v = std::getenv("TE_GFX_VITA_BATCH_CENSUS")) g_bo.census_every = std::max(0L, std::atol(v));
    if (const char* v = std::getenv("TE_GFX_VITA_INSTANCE")) {
        if (v[0] == 'a' && v[1] == 'b') {
            g_bo.mode = 3;
            g_bo.ab_every = std::max(1L, std::atol(v + 2));
            const char* c = std::strchr(v, ':');
            g_bo.ab_arm = c && c[1] == '2' ? 2 : 1;
        } else {
            g_bo.mode = std::max(0, std::min(2, std::atoi(v)));
        }
    }
    if (const char* v = std::getenv("TE_GFX_VITA_INSTANCE_LOOKBACK")) g_bo.lookback = std::max(1, std::min(4096, std::atoi(v)));
    if (const char* v = std::getenv("TE_GFX_VITA_BATCH_DUMP")) g_bo.dump = std::atol(v);
    if (const char* v = std::getenv("TE_GFX_VITA_INSTANCE_PAD")) g_bo.pad = float(std::max(0, std::atoi(v)));
    if (const char* v = std::getenv("TE_GFX_VITA_INSTANCE_CROSS"))
        g_bo.cross = std::strcmp(v, "unlit") == 0 ? 0 : std::strcmp(v, "mesh") == 0 ? 1 : 2;
    if (const char* v = std::getenv("TE_GFX_VITA_INSTANCE_BUCKETS")) g_bo.buckets = uint32_t(std::strtoul(v, nullptr, 16));
    if (const char* v = std::getenv("TE_GFX_VITA_INSTANCE_DEPTH")) g_bo.depth = v[0] == '1';
    if (g_bo.depth) std::fprintf(stderr, "[te.gfx] vita instancing plan: the depth-safe rule ON (TE_GFX_VITA_INSTANCE_DEPTH=1)\n");
    if (g_bo.buckets != ~0u) std::fprintf(stderr, "[te.gfx] vita instancing plan (diagnosis): buckets 0x%x only\n", g_bo.buckets);
    if (g_bo.cross != 2)
        std::fprintf(stderr, "[te.gfx] vita instancing plan (diagnosis): a draw moves over %s\n",
                     g_bo.cross == 0 ? "unlit mesh draws only" : "mesh draws only");
    static const char* const kModes[4] = {"off (the default; =1 instanced draws, =2 the planned order drawn one draw at a time, "
                                          "=abN[:M] mode M (1, 2) and the default drawing alternating every N pictures)",
                                          "ON: groups drawn as instanced draws (not picture-identical in Vita3K)",
                                          "the planned order drawn one draw at a time (the order's check: exact)",
                                          "A/B: the plan and the default drawing alternating (the GPU's time per arm: the gpu A/B line)"};
    std::fprintf(stderr, "[te.gfx] vita instancing plan (TE_GFX_VITA_INSTANCE=%s%ld%s): %s; batching census 1 picture in %ld "
                 "(TE_GFX_VITA_BATCH_CENSUS; 0 off), look-back %d entries, boxes %.0f px wider (TE_GFX_VITA_INSTANCE_PAD)\n",
                 g_bo.mode == 3 ? "ab" : "", g_bo.mode == 3 ? g_bo.ab_every : long(g_bo.mode), g_bo.mode != 3 ? "" : g_bo.ab_arm == 2 ?
                 ":2 (arm: the planned order one draw at a time)" : ":1 (arm: instanced)", kModes[g_bo.mode], g_bo.census_every,
                 g_bo.lookback, double(g_bo.pad));
}

// ---- the census (the drawing thread's; NotePace reads it) -------------------------------------------------------------
struct BatchCensus {
    uint64_t frames = 0;                       // pictures censused
    uint64_t draws = 0, unlit = 0, lit = 0, skins = 0, vertex = 0, blocking = 0, copy_textured = 0;
    uint64_t tuples = 0;                       // distinct unlit (mesh, range, state, shift) per bucket, summed
    uint64_t consecutive = 0;                  // unlit draws right after a draw of their tuple
    uint64_t entries = 0;                      // the planned order's entries (a group = 1)
    uint64_t joined = 0, joined_moved = 0;     // draws joined to their tuple's group (that moved back over others)
    uint64_t crossed = 0;                      // entries the joined draws moved back over
    uint64_t refused[6] = {};                  // a repeat not joined: overlap, unknown extent / barrier, look-back, sky, full, (spare)
    uint64_t groups = 0, sizes[5] = {};        // groups of 2+ by size: 2, 3-4, 5-8, 9-16, 17+
    uint64_t lit_tuples = 0, joined_lit = 0;   // with the lit meshes too (equal lights)
    uint64_t merge_same_matrix = 0, concat_other_matrix = 0;
    uint64_t states = 0;                       // distinct (state, lit) per bucket, summed
    uint64_t plan_us = 0, plan_frames = 0;     // the plan's own time when drawn by it (the device's clock)
    uint64_t census_us = 0;                    // the census' own time on its sampled frames (the device's clock)
    uint64_t planned_groups = 0, planned_joined = 0, planned_frames = 0;   // the plan as drawn (every planned picture)
    uint64_t planned_boxes = 0, planned_visits = 0, planned_keys = 0;      // its work: boxes made, entries tested, tuples keyed
    // the DEPTH-SAFE rule's pass (the census's third): joined, groups, the overlapping crossings the depth ranges allowed,
    // the repeats still refused for an overlap (and of them: a draw not depth-safe on either side, ranges that meet,
    // a caster over a caster), the depth ranges made
    uint64_t depth_joined = 0, depth_groups = 0, depth_entries = 0, depth_crossings = 0, depth_refused = 0;
    uint64_t depth_unsafe = 0, depth_meet = 0, depth_casters = 0, depth_ranges = 0;
};
BatchCensus g_bc;
uint64_t g_pictures = 0;
uint64_t g_boxes_made = 0, g_visits = 0, g_keyed = 0;   // (the passes' work counts, read by PlanBucket)

enum Kind : uint8_t { kUnlit, kLit, kOther, kBlock, kEmpty };
enum Refusal { kRefOverlap, kRefBlocking, kRefLookback, kRefSky, kRefFull, kRefCasters };
const char* const kRefusalNames[6] = {"a box it crosses overlaps", "a draw of unknown extent / a barrier between",
                                      "beyond the look-back", "the sky's insertion point between", "its group full (64)",
                                      "a caster crossing a caster where the frame's casters overflow the re-stamp's room"};

struct Item {
    uint64_t key = 0;
    int32_t next = -1;       // the group's next draw (its chain, in order)
    int32_t slot = -1;       // its entry in the planned order
    uint8_t kind = kOther;
    uint8_t box_state = 0;   // 0 not made, 1 known, 2 unknown (anywhere)
    uint8_t caster = 0;      // it may record a shadow caster at its submit (the casters' rule)
    uint8_t z_state = 0;     // 0 not made, 1 depth-safe with a known range, 2 not depth-safe / unknown
    float box[4] = {0, 0, 0, 0};
    float z[2] = {0, 0};     // the GS Z range its pixels can take (padded: DepthOfItem)
};
struct Slot {
    int32_t head = -1, tail = -1, n = 0;
    bool blocking = false;
    bool caster = false;         // one of its draws may record a shadow caster
    float u[4] = {0, 0, 0, 0};   // the union of a group's boxes (n >= 2)
};
struct KeyEntry {
    uint64_t key;
    int32_t slot;
    uint32_t gen;
};
constexpr uint32_t kKeySlots = 8192;   // > 2x the largest bucket (~1100 draws in the title's pan)
std::vector<Item> g_items;
std::vector<Slot> g_slots;
std::vector<KeyEntry> g_keys;
uint32_t g_key_gen = 0;

inline uint64_t Mix(uint64_t h, uint32_t w) { return (h ^ w) * 1099511628211ull; }
inline uint32_t Bits(float f) {
    uint32_t w;
    std::memcpy(&w, &f, 4);
    return w;
}
typedef uint32_t __attribute__((may_alias)) StateWord;
static_assert(sizeof(ps2e_draw_state) % 4 == 0 && alignof(ps2e_draw_state) >= 4, "the state as words");
constexpr int kStateWords = int(sizeof(ps2e_draw_state) / 4);
inline bool SameStateWords(const ps2e_draw_state& a, const ps2e_draw_state& b) {
    const StateWord* x = reinterpret_cast<const StateWord*>(&a);
    const StateWord* y = reinterpret_cast<const StateWord*>(&b);
    uint32_t diff = 0;
    for (int i = 0; i < kStateWords; ++i) diff |= x[i] ^ y[i];
    return diff == 0;
}
inline uint64_t StateHash(const ps2e_draw_state& s, int lit) {
    const StateWord* w = reinterpret_cast<const StateWord*>(&s);
    uint64_t h = 1469598103934665603ull ^ uint64_t(lit);
    for (int i = 0; i < kStateWords; ++i) h = Mix(h, w[i]);
    return h;
}
// the tuple an instanced draw takes: the mesh, its range, the state, the shift (+ the lights of a lit draw). A compact
// record's state is its state pool entry's (graphics.cpp: one entry per content, kept while any picture uses it): keyed by
// the entry, not its 28 words -- equal states under two keys only stay apart (SameTuple compares the words on a hit)
uint64_t TupleKey(const Draw& d, bool with_lights) {
    const ps2e_draw_mesh_params& m = d.mesh_params;
    uint64_t h = d.pool ? Mix(0x9E3779B97F4A7C15ull ^ uint64_t(m.lit ? 1 : 0), 0x706F6F6Cu ^ d.pool) : StateHash(d.state, m.lit ? 1 : 0);
    h = Mix(h, d.mesh);
    h = Mix(h, uint32_t(m.first));
    h = Mix(h, uint32_t(m.count));
    h = Mix(Mix(h, Bits(m.offset[0])), Bits(m.offset[1]));
    h = Mix(Mix(h, Bits(m.scroll[0])), Bits(m.scroll[1]));
    if (with_lights && m.lit) {
        for (int l = 0; l < 3; ++l)
            for (int c = 0; c < 3; ++c) h = Mix(Mix(h, Bits(m.light_dir[l][c])), Bits(m.light_colour[l][c]));
        for (int c = 0; c < 3; ++c) h = Mix(h, Bits(m.ambient[c]));
    }
    return h;
}
// the very tuple (the key's fields compared: a hash collision never groups two draws)
bool SameTuple(const Draw& a, const Draw& b, bool with_lights) {
    const ps2e_draw_mesh_params& x = a.mesh_params;
    const ps2e_draw_mesh_params& y = b.mesh_params;
    if (a.mesh != b.mesh || x.first != y.first || x.count != y.count || (x.lit != 0) != (y.lit != 0)) return false;
    if (std::memcmp(x.offset, y.offset, sizeof x.offset) != 0 || std::memcmp(x.scroll, y.scroll, sizeof x.scroll) != 0) return false;
    if (!SameStateWords(a.state, b.state)) return false;
    if (x.lit && with_lights &&
        (std::memcmp(x.light_dir, y.light_dir, sizeof x.light_dir) != 0 ||
         std::memcmp(x.light_colour, y.light_colour, sizeof x.light_colour) != 0 ||
         std::memcmp(x.ambient, y.ambient, sizeof x.ambient) != 0))
        return false;
    return true;
}

Kind KindOf(const Draw& d) {
    const ps2e_draw_state& s = d.state;
    if (d.copy_frame || s.stencil != PS2E_DRAW_STENCIL_OFF) return kBlock;
    if (d.mesh) {
        if (d.skin_joints) return kBlock;                         // a GPU skin: its bind-pose box says nothing of where it draws
        if (s.texture == PS2E_DRAW_TEXTURE_FRAME_COPY) return kOther;
        return d.mesh_params.lit ? kLit : kUnlit;
    }
    return d.VertexCount() >= 3 ? kOther : kEmpty;
}

// A mesh draw's screen box from its kept model-space bounds (ps2e_draw_mesh_bounds): the eight corners through its matrix
// as MeshBoxOf takes them (row-vector convention, the divide, the 12.4 pixels, the GS origin off) with one reciprocal per
// corner and each axis's products formed once -- padded 3 pixels (MeshBoxOf: 2) for this form's own roundings, which are
// ulps of the pixel coordinates: still a box no vertex of the draw lands outside. Unknown (false): no bounds, a skin mesh,
// a corner at or behind the eye, a coordinate not finite.
bool MeshBox(ps2e_draw* dev, const Draw& d, float out[4]) {
    float lo[3], hi[3];
    if (!ps2e_draw_mesh_bounds(dev, d.mesh, lo, hi)) return false;
    const float* m = d.mesh_params.screen;
    const float ax[2][3] = {{lo[0] * m[0], lo[0] * m[1], lo[0] * m[3]}, {hi[0] * m[0], hi[0] * m[1], hi[0] * m[3]}};
    const float ay[2][3] = {{lo[1] * m[4], lo[1] * m[5], lo[1] * m[7]}, {hi[1] * m[4], hi[1] * m[5], hi[1] * m[7]}};
    const float az[2][3] = {{lo[2] * m[8], lo[2] * m[9], lo[2] * m[11]}, {hi[2] * m[8], hi[2] * m[9], hi[2] * m[11]}};
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int k = 0; k < 8; ++k) {
        const float* px = ax[k & 1];
        const float* py = ay[(k >> 1) & 1];
        const float* pz = az[(k >> 2) & 1];
        const float cx = m[12] + px[0] + py[0] + pz[0];
        const float cy = m[13] + px[1] + py[1] + pz[1];
        const float cw = m[15] + px[2] + py[2] + pz[2];
        if (!(cw > 1e-6f)) return false;
        const float r = 1.f / (cw * 16.f);
        const float sx = cx * r - d.mesh_params.offset[0], sy = cy * r - d.mesh_params.offset[1];
        if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
        x0 = std::min(x0, sx); y0 = std::min(y0, sy);
        x1 = std::max(x1, sx); y1 = std::max(y1, sy);
    }
    out[0] = x0 - 3.f; out[1] = y0 - 3.f; out[2] = x1 + 3.f; out[3] = y1 + 3.f;
    return true;
}

// the draw's screen box, made when first asked (false: unknown -- it overlaps everything)
bool BoxOfItem(ps2e_draw* dev, Draw* const* list, size_t i, const float** out) {
    Item& it = g_items[i];
    if (it.box_state == 0) {
        const Draw& d = *list[i];
        bool known = false;
        if (d.mesh) {
            known = !d.skin_joints && MeshBox(dev, d, it.box);
        } else if (d.VertexCount()) {
            const ps2e_draw_vertex* v = d.Vertices();
            float x0 = v[0].x, y0 = v[0].y, x1 = v[0].x, y1 = v[0].y;
            for (size_t k = 1, n = d.VertexCount(); k < n; ++k) {
                x0 = std::min(x0, v[k].x); y0 = std::min(y0, v[k].y);
                x1 = std::max(x1, v[k].x); y1 = std::max(y1, v[k].y);
            }
            it.box[0] = x0 - 1.f; it.box[1] = y0 - 1.f; it.box[2] = x1 + 1.f; it.box[3] = y1 + 1.f;
            known = true;
        } else {
            it.box[0] = it.box[1] = 1e30f;   // no vertices: no pixel
            it.box[2] = it.box[3] = -1e30f;
            known = true;
        }
        // (a NaN anywhere: unknown)
        if (known && !(it.box[0] == it.box[0] && it.box[1] == it.box[1] && it.box[2] == it.box[2] && it.box[3] == it.box[3]))
            known = false;
        if (known && g_bo.pad > 0.f) {   // (diagnosis: TE_GFX_VITA_INSTANCE_PAD)
            it.box[0] -= g_bo.pad; it.box[1] -= g_bo.pad; it.box[2] += g_bo.pad; it.box[3] += g_bo.pad;
        }
        it.box_state = known ? 1 : 2;
        g_boxes_made++;
    }
    *out = it.box;
    return it.box_state == 1;
}
inline bool Overlap(const float* a, const float* b) {
    return !(a[2] < b[0] || b[2] < a[0] || a[3] < b[1] || b[3] < a[1]);
}

struct PassOut {
    uint64_t tuples = 0, consecutive = 0, entries = 0, joined = 0, joined_moved = 0, crossed = 0;
    uint64_t refused[6] = {};
    uint64_t groups = 0, sizes[5] = {};
    uint64_t depth_crossings = 0, depth_unsafe = 0, depth_meet = 0, depth_casters = 0;   // (the depth rule's pass)
};

// THE DEPTH-SAFE RULE. Two draws whose boxes overlap may still swap when at every pixel both cover the winner is the
// nearer of the two whatever their order: both opaque and depth-writing -- no blend, colour written, no stencil, no
// destination-alpha test, an alpha test only of the discarding kind (AFAIL KEEP: a failed fragment writes nothing) --,
// both testing depth GEQUAL / GREATER (nearer = larger GS Z), and their depth ranges disjoint. Then at a pixel both
// cover, whatever the depth D before them, the result is the larger of {D, Z_a, Z_b} that passes, in either order (no tie
// between them is possible: the ranges do not meet; a tie with D is decided by each one's own test against D, as before).
// A fragment discarded by the alpha test is a pixel the draw does not cover. A depth-only draw is not depth-safe (it would
// hide a farther colour without replacing it), nor a draw that writes no depth (a nearer one would not stop a later
// farther one). Shadow casters keep the box rule between them (their re-stamps).
bool DepthSafeState(const ps2e_draw_state& s) {
    return (s.depth_test == PS2E_DRAW_DEPTH_GEQUAL || s.depth_test == PS2E_DRAW_DEPTH_GREATER) && s.depth_write && !s.blend &&
           !s.no_colour_write && s.stencil == PS2E_DRAW_STENCIL_OFF && !s.dest_alpha_test && (!s.alpha_test || s.alpha_fail == 0) &&
           s.texture != PS2E_DRAW_TEXTURE_FRAME_COPY;
}
// The GS Z range a draw's pixels can take: a native mesh's -- its kept bounds' eight corners through its matrix as the mesh
// vertex program takes them (Z = floor(cz / cw), cw > 0 at every corner, so cw > 0 in the whole box and cz / cw, a linear-
// fractional function, takes its extremes over the box at corners; a triangle's pixels interpolate its vertices' Z) --,
// a vertex draw's -- its vertices' Z --; clamped as the programs clamp (0 .. 2^24 - 1: two draws both at the clamp tie),
// padded 4 Z units + 2^-20 of the value (the CPU's float roundings against the GPU's). False: not depth-safe / unknown.
bool DepthOfItem(ps2e_draw* dev, Draw* const* list, size_t i, const float** out) {
    Item& it = g_items[i];
    if (it.z_state == 0) {
        const Draw& d = *list[i];
        bool known = DepthSafeState(d.state) && !d.copy_frame && !d.skin_joints;
        float z0 = 1e30f, z1 = -1e30f;
        if (known && d.mesh) {
            float lo[3], hi[3];
            known = ps2e_draw_mesh_bounds(dev, d.mesh, lo, hi) != 0;
            const float* m = d.mesh_params.screen;
            for (int k = 0; known && k < 8; ++k) {
                const float x = (k & 1) ? hi[0] : lo[0], y = (k & 2) ? hi[1] : lo[1], z = (k & 4) ? hi[2] : lo[2];
                const float cz = m[14] + x * m[2] + y * m[6] + z * m[10];
                const float cw = m[15] + x * m[3] + y * m[7] + z * m[11];
                if (!(cw > 1e-6f) || !std::isfinite(cz)) { known = false; break; }
                const float zz = cz / cw;
                z0 = std::min(z0, zz);
                z1 = std::max(z1, zz);
            }
        } else if (known && d.VertexCount()) {
            const ps2e_draw_vertex* v = d.Vertices();
            for (size_t k = 0, n = d.VertexCount(); k < n; ++k) {
                z0 = std::min(z0, v[k].z);
                z1 = std::max(z1, v[k].z);
            }
            known = std::isfinite(z0) && std::isfinite(z1);
        } else {
            known = false;
        }
        if (known) {
            constexpr float kTop = 16777215.f;
            z0 = std::floor(std::max(0.f, std::min(kTop, z0)));
            z1 = std::floor(std::max(0.f, std::min(kTop, z1)));
            it.z[0] = z0 - 4.f - std::fabs(z0) * (1.f / 1048576.f);
            it.z[1] = z1 + 4.f + std::fabs(z1) * (1.f / 1048576.f);
            g_bc.depth_ranges++;
        }
        it.z_state = known ? 1 : 2;
    }
    *out = it.z;
    return it.z_state == 1;
}
// may draws a and b (their boxes overlapping) swap? (the depth rule; the casters' re-stamps keep the box rule)
bool DepthApart(ps2e_draw* dev, Draw* const* list, size_t a, size_t b, PassOut& out) {
    if (g_items[a].caster && g_items[b].caster) { out.depth_casters++; return false; }
    const float* za = nullptr;
    const float* zb = nullptr;
    if (!DepthOfItem(dev, list, a, &za) || !DepthOfItem(dev, list, b, &zb)) { out.depth_unsafe++; return false; }
    if (za[1] < zb[0] || zb[1] < za[0]) { out.depth_crossings++; return true; }
    out.depth_meet++;
    return false;
}

// One pass of the plan over a bucket's order (see the file's head). `lit_too`: the lit meshes are candidates too (their
// tuple with their lights). `barrier`: the sky's insertion point (the index of the draw the sky is drawn before; `count`:
// none). Fills g_items / g_slots; `dump` lists the groups and the refused repeats.
void PlanPass(ps2e_draw* dev, Draw* const* list, size_t count, size_t barrier, bool lit_too, PassOut& out, bool dump, long picture,
              int bucket, bool depth = false) {
    g_items.assign(count, Item());
    g_slots.clear();
    if (g_keys.empty()) g_keys.assign(kKeySlots, KeyEntry{0, -1, 0});
    if (++g_key_gen == 0) {
        for (KeyEntry& e : g_keys) e.gen = 0;
        g_key_gen = 1;
    }
    size_t barrier_slot = ~size_t(0);
    int32_t prev_cand = -1;   // the previous draw when it was a candidate (the consecutive census)
    // THE CASTERS' ORDER: the frame keeps its first 128 shadow casters only (ps2e_draw.cpp CasterRoom), so where this
    // bucket's draws that may record one outnumber the room left, WHICH are kept depends on their order -- then a draw that
    // may record one never moves over another that may (the casters' records stay in their order; the others' moves change
    // nothing in them). Where they fit the room, every one is kept whatever the order (and two that swap are box-disjoint:
    // their re-stamps touch no common pixel).
    bool caster_rule = false;
    if (const int room = ps2e_draw_caster_room(dev)) {
        // (a mesh by RecordCasterMesh's tests asked without recording; a vertex draw or a GPU skin by its state alone -- more
        // than record one: the rule is on whenever the bucket might overflow the room)
        int potential = 0;
        for (size_t j = 0; j < count; ++j) {
            const Draw& d = *list[j];
            int c = 0;
            if (d.mesh && !d.skin_joints) c = ps2e_draw_mesh_caster_candidate(dev, &d.state, d.mesh, &d.mesh_params);
            else if (d.mesh || d.VertexCount() >= 3) c = ps2e_draw_state_caster(dev, &d.state);
            g_items[j].caster = uint8_t(c);
            potential += c;
        }
        caster_rule = potential > room;
    }
    for (size_t j = 0; j < count; ++j) {
        if (j == barrier) barrier_slot = g_slots.size();
        const Draw& d = *list[j];
        Item& it = g_items[j];
        it.kind = KindOf(d);
        const bool cand = it.kind == kUnlit || (lit_too && it.kind == kLit);
        if (it.kind == kEmpty) {   // (no vertices, no copy: nothing drawn -- no entry, nothing crossed)
            prev_cand = -1;
            continue;
        }
        int32_t g = -1;
        KeyEntry* e = nullptr;
        if (cand) {
            it.key = TupleKey(d, lit_too);
            g_keyed++;
            uint32_t h = uint32_t(it.key ^ (it.key >> 29)) & (kKeySlots - 1);
            for (;;) {
                KeyEntry& k = g_keys[h];
                if (k.gen != g_key_gen) {
                    k.gen = g_key_gen;
                    k.key = it.key;
                    k.slot = -1;
                    e = &k;
                    out.tuples++;
                    break;
                }
                if (k.key == it.key && k.slot >= 0 && SameTuple(*list[size_t(g_slots[size_t(k.slot)].head)], d, lit_too)) {
                    e = &k;
                    g = k.slot;
                    break;
                }
                h = (h + 1) & (kKeySlots - 1);
            }
            if (prev_cand >= 0 && g_items[size_t(prev_cand)].key == it.key && g >= 0 && size_t(g) + 1 == g_slots.size() &&
                g_slots[size_t(g)].tail == prev_cand)
                out.consecutive++;
        }
        bool joined = false;
        if (g >= 0) {
            // may it move back to the group's place? every entry after the group: a known box disjoint from its own
            Slot& gs = g_slots[size_t(g)];
            int why = -1;
            const float* bj = nullptr;
            int32_t blocker = -1;
            if (gs.n >= 64) why = kRefFull;
            else if (j >= barrier && size_t(g) < barrier_slot) why = kRefSky;
            else if (g_slots.size() - 1 - size_t(g) > size_t(g_bo.lookback)) why = kRefLookback;
            else if (g_slots.size() - 1 > size_t(g) && !BoxOfItem(dev, list, j, &bj)) why = kRefBlocking;
            for (size_t s = size_t(g) + 1; why < 0 && s < g_slots.size(); ++s) {
                const Slot& x = g_slots[s];
                g_visits++;
                if (x.blocking) { why = kRefBlocking; blocker = x.head; break; }
                if (caster_rule && it.caster && x.caster) { why = kRefCasters; blocker = x.head; break; }
                if (x.n == 1) {
                    const float* bx = nullptr;
                    if (!BoxOfItem(dev, list, size_t(x.head), &bx)) { why = kRefBlocking; blocker = x.head; break; }
                    if (Overlap(bj, bx) && !(depth && DepthApart(dev, list, j, size_t(x.head), out))) {
                        why = kRefOverlap; blocker = x.head; break;
                    }
                } else if (Overlap(bj, x.u)) {
                    for (int32_t m = x.head; m >= 0; m = g_items[size_t(m)].next) {
                        const float* bm = nullptr;
                        // (a group's members joined with known boxes but its head, which may be unknown: then it blocks)
                        if (!BoxOfItem(dev, list, size_t(m), &bm)) { why = kRefBlocking; blocker = m; break; }
                        if (Overlap(bj, bm) && !(depth && DepthApart(dev, list, j, size_t(m), out))) {
                            why = kRefOverlap; blocker = m; break;
                        }
                    }
                }
            }
            if (why < 0) {
                // joined: the group's chain, its union box
                const size_t crossed = g_slots.size() - 1 - size_t(g);
                if (gs.n == 1) {
                    const float* bh = nullptr;
                    if (!BoxOfItem(dev, list, size_t(gs.head), &bh)) bh = nullptr;
                    if (bh) std::memcpy(gs.u, bh, sizeof gs.u);
                    else { gs.u[0] = gs.u[1] = -1e30f; gs.u[2] = gs.u[3] = 1e30f; }
                }
                if (!bj) BoxOfItem(dev, list, j, &bj);
                if (it.box_state == 1) {
                    gs.u[0] = std::min(gs.u[0], bj[0]); gs.u[1] = std::min(gs.u[1], bj[1]);
                    gs.u[2] = std::max(gs.u[2], bj[2]); gs.u[3] = std::max(gs.u[3], bj[3]);
                } else {
                    gs.u[0] = gs.u[1] = -1e30f; gs.u[2] = gs.u[3] = 1e30f;
                }
                g_items[size_t(gs.tail)].next = int32_t(j);
                gs.tail = int32_t(j);
                gs.n++;
                gs.caster = gs.caster || it.caster;
                it.slot = g;
                joined = true;
                out.joined++;
                if (crossed) {
                    out.joined_moved++;
                    out.crossed += crossed;
                }
            } else {
                out.refused[why]++;
                if (dump) {
                    const Draw* bd = blocker >= 0 ? list[size_t(blocker)] : nullptr;
                    float bb[4] = {0, 0, 0, 0};
                    if (blocker >= 0) std::memcpy(bb, g_items[size_t(blocker)].box, sizeof bb);
                    std::fprintf(stderr, "[te.gfx.batch] picture %ld bucket %d: #%u mesh %u (tex %u) not joined to its group at #%d (%d "
                                 "draws): %s%s", picture, bucket, unsigned(j), d.mesh, d.state.texture, gs.head, gs.n,
                                 kRefusalNames[why], bd ? "" : "\n");
                    if (bd)
                        std::fprintf(stderr, " -- #%d %s %u box %.0f %.0f %.0f %.0f vs %.0f %.0f %.0f %.0f\n", blocker,
                                     bd->mesh ? "mesh" : "vertices", bd->mesh ? bd->mesh : unsigned(bd->VertexCount()), bb[0],
                                     bb[1], bb[2], bb[3], bj ? bj[0] : 0.f, bj ? bj[1] : 0.f, bj ? bj[2] : 0.f, bj ? bj[3] : 0.f);
                }
            }
        }
        if (!joined) {
            Slot s;
            s.head = s.tail = int32_t(j);
            s.n = 1;
            s.blocking = it.kind == kBlock || (g_bo.cross == 0 && it.kind != kUnlit) ||
                         (g_bo.cross == 1 && it.kind != kUnlit && it.kind != kLit);
            s.caster = it.caster != 0;
            it.slot = int32_t(g_slots.size());
            g_slots.push_back(s);
            if (cand && e) e->slot = it.slot;   // the tuple's latest group
        }
        prev_cand = cand ? int32_t(j) : -1;
    }
    out.entries += g_slots.size();
    if (dump) {
        // (diagnosis) every mesh draw's kept bounds, matrix, shift and both boxes (MeshBox's, ps2e_draw_mesh_screen_box's)
        for (size_t j = 0; j < count; ++j) {
            const Draw& d = *list[j];
            if (!d.mesh || d.skin_joints) continue;
            float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0}, sb[4] = {0, 0, 0, 0};
            const int hb = ps2e_draw_mesh_bounds(dev, d.mesh, lo, hi);
            const int hs = ps2e_draw_mesh_screen_box(dev, d.mesh, &d.mesh_params, sb);
            const float* m = d.mesh_params.screen;
            std::fprintf(stderr, "[te.gfx.batchm] picture %ld bucket %d #%u mesh %u first %d count %d lit %d bounds %d (%.9g %.9g %.9g) "
                         "(%.9g %.9g %.9g) offset %.9g %.9g screen %.9g %.9g %.9g %.9g | %.9g %.9g %.9g %.9g | %.9g %.9g %.9g %.9g | "
                         "%.9g %.9g %.9g %.9g box %d %.3f %.3f %.3f %.3f\n", picture, bucket, unsigned(j), d.mesh,
                         d.mesh_params.first, d.mesh_params.count, d.mesh_params.lit, hb, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
                         d.mesh_params.offset[0], d.mesh_params.offset[1], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9],
                         m[10], m[11], m[12], m[13], m[14], m[15], hs, sb[0], sb[1], sb[2], sb[3]);
        }
    }
    for (const Slot& s : g_slots) {
        if (s.n < 2) continue;
        out.groups++;
        out.sizes[s.n == 2 ? 0 : s.n <= 4 ? 1 : s.n <= 8 ? 2 : s.n <= 16 ? 3 : 4]++;
        if (dump) {
            const Draw& h = *list[size_t(s.head)];
            std::string members;
            for (int32_t m = s.head; m >= 0; m = g_items[size_t(m)].next) members += " #" + std::to_string(m);
            std::fprintf(stderr, "[te.gfx.batch] picture %ld bucket %d: group of %d: mesh %u range %d+%d tex %u%s:%s\n", picture, bucket,
                         s.n, h.mesh, h.mesh_params.first, h.mesh_params.count, h.state.texture, h.mesh_params.lit ? " lit" : "",
                         members.c_str());
        }
    }
}

// the census's other counts over a bucket's order: the kinds, the consecutive merges, the distinct states
void CountBucket(Draw* const* list, size_t count) {
    BatchCensus& c = g_bc;
    c.draws += count;
    if (++g_key_gen == 0) {
        for (KeyEntry& e : g_keys) e.gen = 0;
        g_key_gen = 1;
    }
    const Draw* prev = nullptr;
    for (size_t j = 0; j < count; ++j) {
        const Draw& d = *list[j];
        switch (KindOf(d)) {
            case kUnlit: c.unlit++; break;
            case kLit: c.lit++; break;
            case kBlock: c.blocking++; if (d.mesh && d.skin_joints) c.skins++; break;
            case kOther: if (d.mesh) c.copy_textured++; else c.vertex++; break;
            default: break;
        }
        // the distinct (state, lit) of the bucket
        const uint64_t sh = StateHash(d.state, d.mesh && d.mesh_params.lit ? 1 : 0);
        for (uint32_t h = uint32_t(sh ^ (sh >> 29)) & (kKeySlots - 1);; h = (h + 1) & (kKeySlots - 1)) {
            KeyEntry& k = g_keys[h];
            if (k.gen != g_key_gen) { k.gen = g_key_gen; k.key = sh; c.states++; break; }
            if (k.key == sh) break;
        }
        // consecutive mesh draws of one state (and lit) but another mesh or range: of one matrix (one merged mesh would draw
        // both), of another matrix (an index concatenation: pre-transformed vertices)
        if (prev && d.mesh && prev->mesh && !d.skin_joints && !prev->skin_joints && !d.copy_frame &&
            (d.mesh_params.lit != 0) == (prev->mesh_params.lit != 0) && SameStateWords(d.state, prev->state) &&
            (d.mesh != prev->mesh || d.mesh_params.first != prev->mesh_params.first || d.mesh_params.count != prev->mesh_params.count)) {
            const bool same_matrix = std::memcmp(d.mesh_params.screen, prev->mesh_params.screen, sizeof d.mesh_params.screen) == 0 &&
                                     std::memcmp(d.mesh_params.offset, prev->mesh_params.offset, sizeof d.mesh_params.offset) == 0 &&
                                     std::memcmp(d.mesh_params.scroll, prev->mesh_params.scroll, sizeof d.mesh_params.scroll) == 0;
            if (same_matrix) c.merge_same_matrix++;
            else c.concat_other_matrix++;
        }
        prev = &d;
    }
}

void Add(BatchCensus& c, const PassOut& p) {
    c.tuples += p.tuples;
    c.consecutive += p.consecutive;
    c.entries += p.entries;
    c.joined += p.joined;
    c.joined_moved += p.joined_moved;
    c.crossed += p.crossed;
    for (int k = 0; k < 6; ++k) c.refused[k] += p.refused[k];
    c.groups += p.groups;
    for (int k = 0; k < 5; ++k) c.sizes[k] += p.sizes[k];
}

}  // namespace

int InstancePlanSetting() {
    if (!g_bo.read) ReadOptions();
    return g_bo.mode;
}

const char* InstancePlanArmName() {
    if (!g_bo.read) ReadOptions();
    return g_bo.mode == 3 && g_bo.ab_arm == 2 ? "the planned order" : "instanced";
}

int InstancePlanMode() {
    if (!g_bo.read) ReadOptions();
    if (g_bo.mode == 3) return ((g_pictures / uint64_t(g_bo.ab_every)) & 1) == 0 ? g_bo.ab_arm : 0;   // (this picture's arm)
    return g_bo.mode;
}

bool BatchCensusFrame(bool picture) {
    if (!g_bo.read) ReadOptions();
    if (!picture) return false;
    const uint64_t n = ++g_pictures;
    const bool on = g_bo.census_every > 0 && n % uint64_t(g_bo.census_every) == 0;
    if (on) g_bc.frames++;
    return on || (g_bo.dump >= 0 && long(n) == g_bo.dump);
}

const BatchPlan* PlanBucket(ps2e_draw* device, Draw* const* list, size_t count, size_t barrier, bool census, bool plan,
                            long picture, int bucket) {
    if (!g_bo.read) ReadOptions();
    static BatchPlan out;
    if (plan && bucket >= 0 && bucket < 32 && !(g_bo.buckets & (1u << bucket))) plan = false;   // (diagnosis: the bucket mask)
    if (!census && !plan) return nullptr;
    const bool dump = g_bo.dump >= 0 && picture == g_bo.dump;
    const uint64_t t0 = plan ? sceKernelGetProcessTimeWide() : 0;
    const uint64_t t0c = census && !plan ? sceKernelGetProcessTimeWide() : 0;   // (the census' pass A when nothing is drawn by it)
    const uint64_t b0 = g_boxes_made, v0 = g_visits, k0 = g_keyed;
    PassOut a;
    PlanPass(device, list, count, barrier, false, a, dump, picture, bucket, g_bo.depth);
    const BatchPlan* result = nullptr;
    if (plan) {
        out.next.assign(count, -1);
        out.absorbed.assign(count, 0);
        for (const Slot& s : g_slots) {
            if (s.n < 2) continue;
            for (int32_t m = s.head; m >= 0; m = g_items[size_t(m)].next) {
                out.next[size_t(m)] = g_items[size_t(m)].next;
                if (m != s.head) out.absorbed[size_t(m)] = 1;
            }
        }
        g_bc.plan_us += sceKernelGetProcessTimeWide() - t0;
        g_bc.planned_groups += a.groups;
        g_bc.planned_joined += a.joined;
        g_bc.planned_boxes += g_boxes_made - b0;
        g_bc.planned_visits += g_visits - v0;
        g_bc.planned_keys += g_keyed - k0;
        result = &out;
    }
    if (census) {
        const uint64_t c0 = sceKernelGetProcessTimeWide();
        Add(g_bc, a);
        CountBucket(list, count);
        PassOut b;   // the lit meshes too (their lights equal)
        PlanPass(device, list, count, barrier, true, b, false, picture, bucket);
        g_bc.lit_tuples += b.tuples;
        g_bc.joined_lit += b.joined;
        // the depth-safe rule's pass (unlit, as pass A), unless pass A already was it
        PassOut dz;
        if (g_bo.depth) dz = a;
        else PlanPass(device, list, count, barrier, false, dz, false, picture, bucket, true);
        g_bc.depth_joined += dz.joined;
        g_bc.depth_groups += dz.groups;
        g_bc.depth_entries += dz.entries;
        g_bc.depth_crossings += dz.depth_crossings;
        g_bc.depth_refused += dz.refused[kRefOverlap];
        g_bc.depth_unsafe += dz.depth_unsafe;
        g_bc.depth_meet += dz.depth_meet;
        g_bc.depth_casters += dz.depth_casters;
        // (the census' time: its passes; pass A is the plan's when the plan is drawn, else the census')
        g_bc.census_us += sceKernelGetProcessTimeWide() - c0 + (plan ? 0 : c0 - t0c);
    }
    return result;
}

void NotePlannedFrame() { g_bc.planned_frames++; }

std::string BatchCensusLine(double frames) {
    if (!g_bo.read) return std::string();
    static BatchCensus last;
    const BatchCensus now = g_bc;
    const uint64_t nf = now.frames - last.frames;
    std::string line;
    if (nf) {
        const double f = double(nf);
        auto per = [&](uint64_t a, uint64_t b) { return double(a - b) / f; };
        char b[2000];
        std::snprintf(b, sizeof b, "[te.gfx.vita] batch census per sampled frame (TE_GFX_VITA_BATCH_CENSUS=%ld: %llu frames; each "
                      "bucket's drawing order): draws %.1f = unlit mesh %.1f + lit mesh %.1f + vertex %.1f + frame-copy meshes %.1f + "
                      "blocking %.1f (GPU skins %.1f, stencil / copies the rest); unlit tuples (mesh, range, state, shift) %.1f -> "
                      "repeats %.1f, of them right after a draw of their tuple %.1f; THE PLAN (moving back only over draws whose "
                      "screen boxes (+%.0f px) it does not touch, look-back %d): %.1f draws -> %.1f entries: %.1f joined (%.1f moved back "
                      "over %.1f entries) in %.1f groups (2: %.1f, 3-4: %.1f, 5-8: %.1f, 9-16: %.1f, 17+: %.1f); repeats not "
                      "joined: overlap %.1f, unknown extent / barrier %.1f, look-back %.1f, sky %.1f, full %.1f, the casters' order %.1f; lit meshes too "
                      "(equal lights): %.1f tuples, %.1f joined; consecutive of one state, another mesh: one matrix %.1f (a merged "
                      "mesh), other matrices %.1f; distinct states %.1f; the census took %.0f us a sampled frame (the drawing thread)\n",
                      g_bo.census_every, (unsigned long long)nf, per(now.draws, last.draws), per(now.unlit, last.unlit),
                      per(now.lit, last.lit), per(now.vertex, last.vertex), per(now.copy_textured, last.copy_textured),
                      per(now.blocking, last.blocking), per(now.skins, last.skins), per(now.tuples, last.tuples),
                      per(now.unlit - now.tuples, last.unlit - last.tuples), per(now.consecutive, last.consecutive), double(g_bo.pad), g_bo.lookback,
                      per(now.draws, last.draws), per(now.entries, last.entries), per(now.joined, last.joined), per(now.joined_moved, last.joined_moved), per(now.crossed, last.crossed),
                      per(now.groups, last.groups), per(now.sizes[0], last.sizes[0]), per(now.sizes[1], last.sizes[1]),
                      per(now.sizes[2], last.sizes[2]), per(now.sizes[3], last.sizes[3]), per(now.sizes[4], last.sizes[4]),
                      per(now.refused[kRefOverlap], last.refused[kRefOverlap]),
                      per(now.refused[kRefBlocking], last.refused[kRefBlocking]),
                      per(now.refused[kRefLookback], last.refused[kRefLookback]), per(now.refused[kRefSky], last.refused[kRefSky]),
                      per(now.refused[kRefFull], last.refused[kRefFull]), per(now.refused[kRefCasters], last.refused[kRefCasters]),
                      per(now.lit_tuples, last.lit_tuples),
                      per(now.joined_lit, last.joined_lit), per(now.merge_same_matrix, last.merge_same_matrix),
                      per(now.concat_other_matrix, last.concat_other_matrix), per(now.states, last.states),
                      per(now.census_us, last.census_us));
        line = b;
        char z[700];
        std::snprintf(z, sizeof z, "[te.gfx.vita] batch census, THE DEPTH-SAFE RULE (a draw may also cross a draw whose box overlaps its "
                      "own when both are opaque depth-writing GEQUAL / GREATER draws whose depth ranges are disjoint; casters keep the box "
                      "rule; TE_GFX_VITA_INSTANCE_DEPTH=%d in the plan): %.1f joined (box rule alone %.1f) in %.1f groups, %.1f entries; "
                      "overlapping crossings the depth ranges allowed %.1f; repeats still refused for an overlap %.1f (the crossed pair: "
                      "not depth-safe %.1f, ranges meeting %.1f, caster over caster %.1f); depth ranges made %.1f a sampled frame\n",
                      g_bo.depth ? 1 : 0, per(now.depth_joined, last.depth_joined), per(now.joined, last.joined),
                      per(now.depth_groups, last.depth_groups), per(now.depth_entries, last.depth_entries),
                      per(now.depth_crossings, last.depth_crossings), per(now.depth_refused, last.depth_refused),
                      per(now.depth_unsafe, last.depth_unsafe), per(now.depth_meet, last.depth_meet),
                      per(now.depth_casters, last.depth_casters), per(now.depth_ranges, last.depth_ranges));
        line += z;
    }
    const uint64_t pf = now.planned_frames - last.planned_frames;
    if (pf) {
        char b[600];
        const double p = double(pf);
        std::snprintf(b, sizeof b, "[te.gfx.vita] instancing plan per frame (TE_GFX_VITA_INSTANCE=%s%ld): %.1f groups drawing %.1f "
                      "joined draws at their group's first draw; the plan's work: %.1f tuples keyed, %.1f screen boxes made, %.1f "
                      "entries tested; it took %.1f us a frame on the drawing thread (%llu frames)\n",
                      g_bo.mode == 3 ? "ab" : "", g_bo.mode == 3 ? g_bo.ab_every : long(g_bo.mode),
                      double(now.planned_groups - last.planned_groups) / p, double(now.planned_joined - last.planned_joined) / p,
                      double(now.planned_keys - last.planned_keys) / p, double(now.planned_boxes - last.planned_boxes) / p,
                      double(now.planned_visits - last.planned_visits) / p, double(now.plan_us - last.plan_us) / p,
                      (unsigned long long)pf);
        line += b;
    }
    (void)frames;
    last = now;
    return line;
}

#else

int InstancePlanSetting() { return 0; }
const char* InstancePlanArmName() { return ""; }
int InstancePlanMode() { return 0; }
bool BatchCensusFrame(bool) { return false; }
const BatchPlan* PlanBucket(ps2e_draw*, Draw* const*, size_t, size_t, bool, bool, long, int) { return nullptr; }
void NotePlannedFrame() {}
std::string BatchCensusLine(double) { return std::string(); }

#endif

}  // namespace te::gfx
