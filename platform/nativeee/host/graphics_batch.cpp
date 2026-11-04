// platform/nativeee (host side) -- the Vita's BATCHING CENSUS and INSTANCING PLAN over the drawing order
// (TE_GFX_VITA_BATCH_CENSUS, TE_GFX_VITA_INSTANCE).
//
// The device's GPU time follows the number of draws (Totem Hokum's village: ~800 draws a picture, ~23 ms of GPU work; the
// hub: ~350, ~13 ms -- the SGX543's per-draw command / tiler side, not its fill). A draw the GPU need not be handed is one
// of several draws of the SAME MESH RANGE under the SAME STATE (texture, programs, tests, blend: one packet) and the same
// UV shift that differ only in their matrix: one instanced draw (gl_shader_library.cpp RawReplayInstances: the matrices on
// an instance stream) draws them all, in their order.
//
// THE CENSUS (every TE_GFX_VITA_BATCH_CENSUS-th picture; 0, the default: off; one line per census window): each bucket's
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
// THE RULES (TE_GFX_VITA_SORT; drawn instanced unless TE_GFX_VITA_INSTANCE names another mode): unset / off -- the box rule
// above when TE_GFX_VITA_INSTANCE asks for a plan; safe -- the box rule with the depth-safe rule (DepthApart: no pixel can
// change by the order either); full -- THE FULL RULE (FullPass): a repeat joins its group across any depth-safe draws, the
// order then deciding only the pixels where two of them tie in depth (the rooftop's distant view in Vita3K: 3180 GXM draws
// a picture -> 761, the plan ~180 replayed draws' time; the safe rule: 2586, the plan ~1700 draws' time).
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
// The instanced draw's own arithmetic (=1): its vertex program reads the matrix rows from an instance stream where the mesh
// path reads uniforms. The compiler had summed the rows in another order there (a vertex a 1/16 pixel off now and then,
// in Vita3K and on the console alike); written in the mesh program's order, its USSE is the mesh program's instruction for
// instruction (gl_shader_sources.cpp kLeanPs2InstVertSrc). =2 draws the plain way.
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
#include "te_prof.h"
#if defined(__vita__)
#include <arm_neon.h>                 // (the plan's boxes: MeshBox)
#include <psp2/kernel/processmgr.h>   // sceKernelGetProcessTimeWide (the plan's own time)
#endif

namespace te::gfx {

#if defined(__vita__)
namespace {

struct BatchOptions {
    bool read = false;
    // TE_GFX_VITA_BATCH_CENSUS=N: 1 picture in N censused (0, the default: none). Off unless asked: a censused picture runs
    // three passes of the box rule in the census's order over every bucket -- on the device's rooftop view of ~3200 draws
    // 92.7 ms of the drawing thread (its log's "the census took 92685 us a sampled frame"), a picture held ~3 refreshes
    // longer once in every 64 of every player's run (the built-in diagnosis set prints the census lines)
    long census_every = 0;
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
    // TE_GFX_VITA_SORT=off|safe|full: the plan's rule, drawn instanced unless TE_GFX_VITA_INSTANCE names another mode.
    // safe = the box rule with the depth-safe rule (no pixel can change by the order); full = THE FULL RULE (FullPass): every
    // repeat joins its tuple's group across any draws that are depth-safe -- the order then decides only the pixels where two
    // such draws tie in depth. Unset / off: TE_GFX_VITA_INSTANCE alone decides (the box rule).
    int sort = 0;             // 0 off, 1 safe, 2 full
};
BatchOptions g_bo;
// TE_GFX_VITA_INSTANCE_CHECK=1: every box the plan makes made again by MeshBoxScalar (a mesh's) and compared bit for bit (and
// every known box's coordinates checked for a -0, which the integer overlap test assumes away), and every planned bucket
// planned again with the crossing tests in the census's order -- the groups compared draw for draw
struct PlanCheck {
    bool on = false;
    uint64_t boxes = 0, boxes_differ = 0, negative_zero = 0, buckets = 0, buckets_differ = 0;
};
PlanCheck g_check;

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
    {
        // The Vita's default is the full rule: a device log of Rooftop Rampage's distant roofs showed 3267 draws a picture --
        // 2468 repeats of 454 tiny meshes -- costing the drawing thread 32 ms of submits (9.8 us a draw) for 10.5 ms of GPU
        // work; the full rule draws them as ~760 and changes only the pixels where two opaque draws tie in depth (4-68 of a
        // 726x544 picture in the emulator, by 1-6 levels). TE_GFX_VITA_SORT=safe (the exact rules) or =off restore the old order.
        const char* v = std::getenv("TE_GFX_VITA_SORT");
        g_bo.sort = !v || !*v ? 2 : std::strcmp(v, "full") == 0 ? 2 : std::strcmp(v, "safe") == 0 ? 1 : 0;
        if (g_bo.sort == 1) g_bo.depth = true;
        const char* inst = std::getenv("TE_GFX_VITA_INSTANCE");
        if (g_bo.sort && !(inst && *inst)) g_bo.mode = 1;
    }
    if (const char* v = std::getenv("TE_GFX_VITA_INSTANCE_DEPTH")) g_bo.depth = v[0] == '1';
    if (const char* v = std::getenv("TE_GFX_VITA_INSTANCE_CHECK")) g_check.on = v[0] == '1';
    if (g_bo.sort)
        std::fprintf(stderr, "[te.gfx] vita instancing plan: TE_GFX_VITA_SORT=%s -- %s\n", g_bo.sort == 2 ? "full" : "safe",
                     g_bo.sort == 2 ? "THE FULL RULE: every repeat of an unlit mesh joins its tuple's group across draws that are "
                                      "depth-safe (pixels change only where two of them tie in depth)"
                                    : "the box rule and the depth-safe rule (no pixel changes by the order)");
    if (g_check.on)
        std::fprintf(stderr, "[te.gfx] vita instancing plan: CHECK on (TE_GFX_VITA_INSTANCE_CHECK=1): every box made again the "
                     "scalar way, every planned bucket planned again in the census's order -- compared\n");
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
    // the plan's repeats not joined (every planned picture), by why (kRef*), and its buckets / those where the casters kept
    // their order (the full rule's)
    uint64_t planned_refused[6] = {};
    uint64_t planned_buckets = 0, planned_keep_casters = 0;
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

// What every pass over a bucket reads of one of its draws, made once per bucket (PlanBucket) -- the census's three passes and
// the plan share it: the kind, the caster flag, the screen box and the depth range (each made when first asked)
struct Fact {
    uint8_t kind = kOther;
    uint8_t box_state = 0;   // 0 not made, 1 known, 2 unknown (anywhere)
    uint8_t caster = 0;      // it may record a shadow caster at its submit (the casters' rule)
    uint8_t z_state = 0;     // 0 not made, 1 depth-safe with a known range, 2 not depth-safe / unknown
    int32_t k[4];            // the box's coordinates as ordered integers (OrderedBits): the overlap test in core registers
    float box[4];
    float z[2];              // the GS Z range its pixels can take (padded: DepthOfItem)
};
// a draw in one pass
struct Item {
    uint64_t key;
    int32_t next;            // the group's next draw (its chain, in order)
    int32_t slot;            // its entry in the planned order
};
struct Slot {
    int32_t head = -1, tail = -1, n = 0;
    bool blocking = false;
    bool caster = false;         // one of its draws may record a shadow caster
    int32_t u[4] = {0, 0, 0, 0};   // the union of a group's boxes (n >= 2), as ordered integers
};
// A float's bits as an integer that orders as the float does: the sign-magnitude bits of a negative value turned
// two's-complement-wise (x < y <=> OrderedBits(x) < OrderedBits(y) for every pair but -0 / +0, which compare equal as floats
// and -1 / 0 here -- no box coordinate is -0: each is a min / max of values padded by +-1, +-3 or the knob's whole pixels,
// and x - x is +0, or one of +-1e30). A NaN is never ordered: such a box is unknown.
inline int32_t OrderedBits(float f) {
    int32_t i;
    std::memcpy(&i, &f, 4);
    return i ^ ((i >> 31) & 0x7FFFFFFF);
}
inline void OrderedBox(const float* b, int32_t* k) {
    for (int c = 0; c < 4; ++c) k[c] = OrderedBits(b[c]);
}
const int32_t kEverywhere[4] = {OrderedBits(-1e30f), OrderedBits(-1e30f), OrderedBits(1e30f), OrderedBits(1e30f)};
struct KeyEntry {
    uint64_t key;
    int32_t slot;
    uint32_t gen;
};
constexpr uint32_t kKeySlots = 8192;   // > 2x the largest bucket (~1100 draws in the title's pan)
std::vector<Fact> g_facts;   // the bucket's (PlanBucket)
std::vector<Item> g_items;   // the pass's (PlanPass)
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
    const ps2e_draw_mesh_params& m = d.Params();
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
    const ps2e_draw_mesh_params& x = a.Params();
    const ps2e_draw_mesh_params& y = b.Params();
    if (a.mesh != b.mesh || x.first != y.first || x.count != y.count || (x.lit != 0) != (y.lit != 0)) return false;
    if (std::memcmp(x.offset, y.offset, sizeof x.offset) != 0 || std::memcmp(x.scroll, y.scroll, sizeof x.scroll) != 0) return false;
    // (two compact records of one state pool entry hold its bytes: ExpandCompact wrote them, and an entry keeps its bytes while
    // a picture names it -- graphics.cpp StatePool)
    if (!(a.pool && a.pool == b.pool) && !SameStateWords(a.state, b.state)) return false;
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
        return d.Params().lit ? kLit : kUnlit;
    }
    return d.VertexCount() >= 3 ? kOther : kEmpty;
}

// A mesh draw's screen box from its kept model-space bounds (ps2e_draw_mesh_bounds): the eight corners through its matrix
// as MeshBoxOf takes them (row-vector convention, the divide, the 12.4 pixels, the GS origin off) with one reciprocal per
// corner and each axis's products formed once -- padded 3 pixels (MeshBoxOf: 2) for this form's own roundings, which are
// ulps of the pixel coordinates: still a box no vertex of the draw lands outside. Unknown (false): no bounds, a skin mesh,
// a corner at or behind the eye, a coordinate not finite. (The plan's form is MeshBox below; this one is its check's
// reference, TE_GFX_VITA_INSTANCE_CHECK.)
__attribute__((noinline)) bool MeshBoxScalar(ps2e_draw* dev, const Draw& d, float out[4]) {
    float lo[3], hi[3];
    if (!ps2e_draw_mesh_bounds(dev, d.mesh, lo, hi)) return false;
    const float* m = d.Params().screen;
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
        const float sx = cx * r - d.Params().offset[0], sy = cy * r - d.Params().offset[1];
        if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
        x0 = std::min(x0, sx); y0 = std::min(y0, sy);
        x1 = std::max(x1, sx); y1 = std::max(y1, sy);
    }
    out[0] = x0 - 3.f; out[1] = y0 - 3.f; out[2] = x1 + 3.f; out[3] = y1 + 3.f;
    return true;
}
// MeshBoxScalar's box and answer with its tests on the NEON unit. The corners' arithmetic is the scalar form's (the same
// products, sums, reciprocal and offsets in the same order, on the VFP: the same values); the tests it branches on per
// corner (cw > 1e-6, both pixel coordinates finite) and its min / max were seven VFP compares and flag transfers a corner on
// the Cortex-A9, each waiting for the VFP pipeline -- here they are NEON lanes, and one transfer a box says known or not.
// NEON flushes a denormal to zero before it compares or takes a min / max: no test here tells the two apart (below 1e-6
// and finite either way), and a min / max that took zero for a denormal changes no padded coordinate (a denormal +-3 is
// +-3). For finite values NEON's min / max return one of their operands, as std::min / std::max do; a -0 against a +0 may
// come out the other way, which the padding makes +-3 as well. The scalar form's 1e30 / -1e30 starts take part.
bool MeshBox(ps2e_draw* dev, const Draw& d, float out[4]) {
    float lo[3], hi[3];
    if (!ps2e_draw_mesh_bounds(dev, d.mesh, lo, hi)) return false;
    const float* m = d.Params().screen;
    const float ax[2][3] = {{lo[0] * m[0], lo[0] * m[1], lo[0] * m[3]}, {hi[0] * m[0], hi[0] * m[1], hi[0] * m[3]}};
    const float ay[2][3] = {{lo[1] * m[4], lo[1] * m[5], lo[1] * m[7]}, {hi[1] * m[4], hi[1] * m[5], hi[1] * m[7]}};
    const float az[2][3] = {{lo[2] * m[8], lo[2] * m[9], lo[2] * m[11]}, {hi[2] * m[8], hi[2] * m[9], hi[2] * m[11]}};
    alignas(16) float sx[8], sy[8], w[8];
    for (int k = 0; k < 8; ++k) {
        const float* px = ax[k & 1];
        const float* py = ay[(k >> 1) & 1];
        const float* pz = az[(k >> 2) & 1];
        const float cx = m[12] + px[0] + py[0] + pz[0];
        const float cy = m[13] + px[1] + py[1] + pz[1];
        const float cw = m[15] + px[2] + py[2] + pz[2];
        const float r = 1.f / (cw * 16.f);   // (made for every corner: a corner the test refuses decides nothing)
        sx[k] = cx * r - d.Params().offset[0];
        sy[k] = cy * r - d.Params().offset[1];
        w[k] = cw;
    }
    const float32x4_t xa = vld1q_f32(sx), xb = vld1q_f32(sx + 4), ya = vld1q_f32(sy), yb = vld1q_f32(sy + 4);
    const float32x4_t wa = vld1q_f32(w), wb = vld1q_f32(w + 4);
    const float32x4_t eps = vdupq_n_f32(1e-6f), top = vdupq_n_f32(3.40282347e38f);
    // cw > 1e-6 as |cw| > 1e-6 with the sign bit clear (this compiler's vcgtq_f32 is a generic vector compare, made a VFP
    // compare per lane without fast math; the absolute compare is the NEON instruction): the same answer for every value
    const uint32x4_t negative_a = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_f32(wa), 31));
    const uint32x4_t negative_b = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_f32(wb), 31));
    uint32x4_t ok = vandq_u32(vbicq_u32(vcagtq_f32(wa, eps), negative_a), vbicq_u32(vcagtq_f32(wb, eps), negative_b));
    ok = vandq_u32(ok, vandq_u32(vcaleq_f32(xa, top), vcaleq_f32(xb, top)));   // |x| <= FLT_MAX: finite
    ok = vandq_u32(ok, vandq_u32(vcaleq_f32(ya, top), vcaleq_f32(yb, top)));
    const uint32x2_t ok2 = vand_u32(vget_low_u32(ok), vget_high_u32(ok));
    if (vget_lane_u32(vpmin_u32(ok2, ok2), 0) == 0) return false;
    const float32x4_t xmn = vminq_f32(xa, xb), xmx = vmaxq_f32(xa, xb), ymn = vminq_f32(ya, yb), ymx = vmaxq_f32(ya, yb);
    const float32x2_t mn = vpmin_f32(vpmin_f32(vget_low_f32(xmn), vget_high_f32(xmn)), vpmin_f32(vget_low_f32(ymn), vget_high_f32(ymn)));
    const float32x2_t mx = vpmax_f32(vpmax_f32(vget_low_f32(xmx), vget_high_f32(xmx)), vpmax_f32(vget_low_f32(ymx), vget_high_f32(ymx)));
    // (x0 - 3, y0 - 3) and (x1 + 3, y1 + 3) on NEON as well: for these finite operands its round-to-nearest sum is the VFP's
    // (a flushed denormal operand gives -3 / 3 as the denormal does; no result is a denormal: x - 3 is 0 or at least an ulp of 3)
    const float32x2_t three = vdup_n_f32(3.f);
    vst1_f32(out, vsub_f32(vmin_f32(mn, vdup_n_f32(1e30f)), three));
    vst1_f32(out + 2, vadd_f32(vmax_f32(mx, vdup_n_f32(-1e30f)), three));
    return true;
}

// the draw's screen box, made when a pass first asks (false: unknown -- it overlaps everything): its coordinates as ordered
// integers (the overlap test's form; Fact::box keeps the floats)
void MakeBox(ps2e_draw* dev, const Draw& d, Fact& f) {
    bool known = false;
    float* b = f.box;
    if (d.mesh) {
        known = !d.skin_joints && MeshBox(dev, d, b);
        if (g_check.on && !d.skin_joints) {
            float r[4];
            const bool rk = MeshBoxScalar(dev, d, r);
            g_check.boxes++;
            if (rk != known || (known && std::memcmp(r, b, sizeof r) != 0)) g_check.boxes_differ++;
        }
    } else if (d.VertexCount()) {
        const ps2e_draw_vertex* v = d.Vertices();
        float x0 = v[0].x, y0 = v[0].y, x1 = v[0].x, y1 = v[0].y;
        for (size_t k = 1, n = d.VertexCount(); k < n; ++k) {
            x0 = std::min(x0, v[k].x); y0 = std::min(y0, v[k].y);
            x1 = std::max(x1, v[k].x); y1 = std::max(y1, v[k].y);
        }
        b[0] = x0 - 1.f; b[1] = y0 - 1.f; b[2] = x1 + 1.f; b[3] = y1 + 1.f;
        // (a NaN anywhere: unknown -- a mesh's box is finite when MeshBox answers it known)
        known = b[0] == b[0] && b[1] == b[1] && b[2] == b[2] && b[3] == b[3];
    } else {
        b[0] = b[1] = 1e30f;   // no vertices: no pixel
        b[2] = b[3] = -1e30f;
        known = true;
    }
    if (known && g_bo.pad > 0.f) {   // (diagnosis: TE_GFX_VITA_INSTANCE_PAD)
        b[0] -= g_bo.pad; b[1] -= g_bo.pad; b[2] += g_bo.pad; b[3] += g_bo.pad;
    }
    if (known) {
        OrderedBox(b, f.k);
        if (g_check.on)
            for (int c = 0; c < 4; ++c) g_check.negative_zero += Bits(b[c]) == 0x80000000u ? 1 : 0;
    }
    f.box_state = known ? 1 : 2;
    g_boxes_made++;
}
inline bool BoxOf(ps2e_draw* dev, Draw* const* list, size_t i, const int32_t** out) {
    Fact& f = g_facts[i];
    if (f.box_state == 0) MakeBox(dev, *list[i], f);
    *out = f.k;
    return f.box_state == 1;
}
// (the float test !(a[2] < b[0] || b[2] < a[0] || a[3] < b[1] || b[3] < a[1]) on the ordered integers: the same answer)
inline bool Overlap(const int32_t* a, const int32_t* b) {
    return !((a[2] < b[0]) | (b[2] < a[0]) | (a[3] < b[1]) | (b[3] < a[1]));
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
bool DepthSafeState(const ps2e_draw_state& s) { return FactDepthSafe(s); }
// The GS Z range a draw's pixels can take: a native mesh's -- its kept bounds' eight corners through its matrix as the mesh
// vertex program takes them (Z = floor(cz / cw), cw > 0 at every corner, so cw > 0 in the whole box and cz / cw, a linear-
// fractional function, takes its extremes over the box at corners; a triangle's pixels interpolate its vertices' Z) --,
// a vertex draw's -- its vertices' Z --; clamped as the programs clamp (0 .. 2^24 - 1: two draws both at the clamp tie),
// padded 4 Z units + 2^-20 of the value (the CPU's float roundings against the GPU's). False: not depth-safe / unknown.
bool DepthOfItem(ps2e_draw* dev, Draw* const* list, size_t i, const float** out) {
    Fact& it = g_facts[i];
    if (it.z_state == 0) {
        const Draw& d = *list[i];
        bool known = DepthSafeState(d.state) && !d.copy_frame && !d.skin_joints;
        float z0 = 1e30f, z1 = -1e30f;
        if (known && d.mesh) {
            float lo[3], hi[3];
            known = ps2e_draw_mesh_bounds(dev, d.mesh, lo, hi) != 0;
            const float* m = d.Params().screen;
            // the corners' z and w as before (VFP), their tests (cw > 1e-6 as |cw| > 1e-6 with the sign clear, cz finite)
            // and the min / max of the quotients on NEON lanes, as MeshBox does: the same answer and the same range after
            // the clamp (a quotient NEON flushed from a denormal to zero clamps and floors to 0 as the denormal does)
            alignas(16) float cz8[8], cw8[8], q8[8];
            for (int k = 0; known && k < 8; ++k) {
                const float x = (k & 1) ? hi[0] : lo[0], y = (k & 2) ? hi[1] : lo[1], z = (k & 4) ? hi[2] : lo[2];
                cz8[k] = m[14] + x * m[2] + y * m[6] + z * m[10];
                cw8[k] = m[15] + x * m[3] + y * m[7] + z * m[11];
            }
            if (known) {
                const float32x4_t za = vld1q_f32(cz8), zb = vld1q_f32(cz8 + 4), wa = vld1q_f32(cw8), wb = vld1q_f32(cw8 + 4);
                const float32x4_t eps = vdupq_n_f32(1e-6f), top = vdupq_n_f32(3.40282347e38f);
                const uint32x4_t na = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_f32(wa), 31));
                const uint32x4_t nb = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_f32(wb), 31));
                const uint32x4_t ok = vandq_u32(vandq_u32(vbicq_u32(vcagtq_f32(wa, eps), na), vbicq_u32(vcagtq_f32(wb, eps), nb)),
                                                vandq_u32(vcaleq_f32(za, top), vcaleq_f32(zb, top)));
                const uint32x2_t ok2 = vand_u32(vget_low_u32(ok), vget_high_u32(ok));
                known = vget_lane_u32(vpmin_u32(ok2, ok2), 0) != 0;
            }
            if (known) {
                for (int k = 0; k < 8; ++k) q8[k] = cz8[k] / cw8[k];
                const float32x4_t qa = vld1q_f32(q8), qb = vld1q_f32(q8 + 4);
                const float32x4_t qmn = vminq_f32(qa, qb), qmx = vmaxq_f32(qa, qb);
                const float32x2_t mn = vpmin_f32(vget_low_f32(qmn), vget_high_f32(qmn)), mx = vpmax_f32(vget_low_f32(qmx), vget_high_f32(qmx));
                // (lane 0: the least quotient and 1e30; lane 1: minus the greatest and -1e30 -- VNEG flips the sign bit alone)
                const float32x2_t lo2 = vmin_f32(mn, vdup_n_f32(1e30f)), hi2 = vmax_f32(mx, vdup_n_f32(-1e30f));
                alignas(8) float r[2];
                vst1_f32(r, vpmin_f32(lo2, vneg_f32(hi2)));
                z0 = r[0];
                z1 = -r[1];
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
    if (g_facts[a].caster && g_facts[b].caster) { out.depth_casters++; return false; }
    const float* za = nullptr;
    const float* zb = nullptr;
    if (!DepthOfItem(dev, list, a, &za) || !DepthOfItem(dev, list, b, &zb)) { out.depth_unsafe++; return false; }
    if (za[1] < zb[0] || zb[1] < za[0]) { out.depth_crossings++; return true; }
    out.depth_meet++;
    return false;
}

// One pass of the plan over a bucket's order (see the file's head), over the bucket's facts (MakeFacts). `lit_too`: the lit
// meshes are candidates too (their tuple with their lights). `barrier`: the sky's insertion point (the index of the draw the
// sky is drawn before; `count`: none). `caster_rule`: MakeFacts' answer. Fills g_items / g_slots; `dump` lists the groups
// and the refused repeats.
// `backward`: a repeat's crossing tests from the latest entry back to its group's (else from its group's on, the census's
// order). A repeat joins when no entry it would cross refuses it -- the same groups in either order; the order decides only
// which refusal is met first (the census's reasons) and how soon: the entries right before a repeat are as a rule its own
// instance's earlier parts, which overlap it, so a refused repeat is found at once instead of after ~20 entries.
void PlanPass(ps2e_draw* dev, Draw* const* list, size_t count, size_t barrier, bool lit_too, bool caster_rule, PassOut& out, bool dump,
              long picture, int bucket, bool depth, bool backward) {
    g_items.assign(count, Item{0, -1, -1});
    g_slots.clear();
    if (g_keys.empty()) g_keys.assign(kKeySlots, KeyEntry{0, -1, 0});
    if (++g_key_gen == 0) {
        for (KeyEntry& e : g_keys) e.gen = 0;
        g_key_gen = 1;
    }
    size_t barrier_slot = ~size_t(0);
    int32_t prev_cand = -1;   // the previous draw when it was a candidate (the consecutive census)
    for (size_t j = 0; j < count; ++j) {
        if (j == barrier) barrier_slot = g_slots.size();
        const Draw& d = *list[j];
        const Fact& f = g_facts[j];
        Item& it = g_items[j];
        const bool cand = f.kind == kUnlit || (lit_too && f.kind == kLit);
        if (f.kind == kEmpty) {   // (no vertices, no copy: nothing drawn -- no entry, nothing crossed)
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
            const int32_t* bj = nullptr;
            int32_t blocker = -1;
            if (gs.n >= 64) why = kRefFull;
            else if (j >= barrier && size_t(g) < barrier_slot) why = kRefSky;
            else if (g_slots.size() - 1 - size_t(g) > size_t(g_bo.lookback)) why = kRefLookback;
            else if (g_slots.size() - 1 > size_t(g) && !BoxOf(dev, list, j, &bj)) why = kRefBlocking;
            const size_t first = size_t(g) + 1, crossing = why < 0 ? g_slots.size() - first : 0;
            for (size_t n = 0; why < 0 && n < crossing; ++n) {
                const Slot& x = g_slots[backward ? first + crossing - 1 - n : first + n];
                g_visits++;
                if (x.blocking) { why = kRefBlocking; blocker = x.head; break; }
                if (caster_rule && f.caster && x.caster) { why = kRefCasters; blocker = x.head; break; }
                if (x.n == 1) {
                    const int32_t* bx = nullptr;
                    if (!BoxOf(dev, list, size_t(x.head), &bx)) { why = kRefBlocking; blocker = x.head; break; }
                    if (Overlap(bj, bx) && !(depth && DepthApart(dev, list, j, size_t(x.head), out))) {
                        why = kRefOverlap; blocker = x.head; break;
                    }
                } else if (Overlap(bj, x.u)) {
                    for (int32_t m = x.head; m >= 0; m = g_items[size_t(m)].next) {
                        const int32_t* bm = nullptr;
                        // (a group's members joined with known boxes but its head, which may be unknown: then it blocks)
                        if (!BoxOf(dev, list, size_t(m), &bm)) { why = kRefBlocking; blocker = m; break; }
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
                    const int32_t* bh = nullptr;
                    std::memcpy(gs.u, BoxOf(dev, list, size_t(gs.head), &bh) ? bh : kEverywhere, sizeof gs.u);
                }
                if (!bj) BoxOf(dev, list, j, &bj);
                if (f.box_state == 1) {
                    gs.u[0] = std::min(gs.u[0], bj[0]); gs.u[1] = std::min(gs.u[1], bj[1]);
                    gs.u[2] = std::max(gs.u[2], bj[2]); gs.u[3] = std::max(gs.u[3], bj[3]);
                } else {
                    std::memcpy(gs.u, kEverywhere, sizeof gs.u);
                }
                g_items[size_t(gs.tail)].next = int32_t(j);
                gs.tail = int32_t(j);
                gs.n++;
                gs.caster = gs.caster || f.caster;
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
                    float bb[4] = {0, 0, 0, 0}, bf[4] = {0, 0, 0, 0};
                    if (blocker >= 0) std::memcpy(bb, g_facts[size_t(blocker)].box, sizeof bb);
                    if (bj) std::memcpy(bf, f.box, sizeof bf);
                    std::fprintf(stderr, "[te.gfx.batch] picture %ld bucket %d: #%u mesh %u (tex %u) not joined to its group at #%d (%d "
                                 "draws): %s%s", picture, bucket, unsigned(j), d.mesh, d.state.texture, gs.head, gs.n,
                                 kRefusalNames[why], bd ? "" : "\n");
                    if (bd)
                        std::fprintf(stderr, " -- #%d %s %u box %.0f %.0f %.0f %.0f vs %.0f %.0f %.0f %.0f\n", blocker,
                                     bd->mesh ? "mesh" : "vertices", bd->mesh ? bd->mesh : unsigned(bd->VertexCount()), bb[0],
                                     bb[1], bb[2], bb[3], bf[0], bf[1], bf[2], bf[3]);
                }
            }
        }
        if (!joined) {
            Slot s;
            s.head = s.tail = int32_t(j);
            s.n = 1;
            s.blocking = f.kind == kBlock || (g_bo.cross == 0 && f.kind != kUnlit) ||
                         (g_bo.cross == 1 && f.kind != kUnlit && f.kind != kLit);
            s.caster = f.caster != 0;
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
            const int hs = ps2e_draw_mesh_screen_box(dev, d.mesh, &d.Params(), sb);
            const float* m = d.Params().screen;
            std::fprintf(stderr, "[te.gfx.batchm] picture %ld bucket %d #%u mesh %u first %d count %d lit %d bounds %d (%.9g %.9g %.9g) "
                         "(%.9g %.9g %.9g) offset %.9g %.9g screen %.9g %.9g %.9g %.9g | %.9g %.9g %.9g %.9g | %.9g %.9g %.9g %.9g | "
                         "%.9g %.9g %.9g %.9g box %d %.3f %.3f %.3f %.3f\n", picture, bucket, unsigned(j), d.mesh,
                         d.Params().first, d.Params().count, d.Params().lit, hb, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
                         d.Params().offset[0], d.Params().offset[1], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9],
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
                         s.n, h.mesh, h.Params().first, h.Params().count, h.state.texture, h.Params().lit ? " lit" : "",
                         members.c_str());
        }
    }
}

// THE FULL RULE (TE_GFX_VITA_SORT=full): a draw is DEPTH-SAFE when its state is (DepthSafeState: opaque, depth-writing,
// tested GEQUAL / GREATER, no stencil, no destination-alpha test, no frame copy) -- two such draws drawn in either order
// leave every pixel as it was but where both cover it at the SAME depth (GEQUAL: the later one wins a tie, GREATER the
// earlier one; at any other depth the nearer wins whatever the order, and the depth written is the larger either way). A
// repeat of an unlit mesh (no alpha test: the punch-through draws keep their place after the opaque ones) joins the latest
// group of its tuple when every entry it moves back over is depth-safe -- a draw that is not (a blended or a stencil draw,
// a copy, a depth-only or no-depth-write draw) is a wall no later draw moves back over, as the sky's insertion point is;
// where the frame's casters may overflow the re-stamp's room (`caster_rule`) a caster does not move over a caster. No
// screen box or depth range is made: the walls are a running slot index (one comparison a repeat). Fills g_items / g_slots
// as PlanPass does; the refused repeats by why (kRefBlocking: a wall between).
// The full rule's per-state answers: bit 0 depth-safe (DepthSafeState), bit 1 a stencil draw, bit 2 a frame-copy texture,
// bit 3 alpha-tested. A pooled record's state is its state pool entry's bytes while a picture names it (graphics.cpp
// StatePool: ids 1..2048), so its answers are made once per entry and pass -- the state's 112 bytes read once a state, not
// once a draw.
constexpr uint32_t kPoolIds = 2048;
uint32_t g_pool_gen[kPoolIds + 1];   // the pass whose answers g_pool_bits holds
uint8_t g_pool_bits[kPoolIds + 1];
inline uint32_t StateBitsOf(const ps2e_draw_state& s) { return FactStateBits(s); }
static_assert(kFactDepthSafe == 1 && kFactStencil == 2 && kFactFrameCopy == 4 && kFactAlphaTest == 8, "the state bits' meaning");
static_assert(int(kFactUnlit) == int(kUnlit) && int(kFactLit) == int(kLit) && int(kFactOther) == int(kOther) &&
                  int(kFactBlock) == int(kBlock) && int(kFactEmpty) == int(kEmpty),
              "the facts' kinds are the plan's");
inline uint32_t StateBits(const Draw& d, uint32_t gen) {
    if (d.pool && d.pool <= kPoolIds) {
        if (g_pool_gen[d.pool] == gen) return g_pool_bits[d.pool];
        const uint32_t b = StateBitsOf(d.state);
        g_pool_gen[d.pool] = gen;
        g_pool_bits[d.pool] = uint8_t(b);
        return b;
    }
    return StateBitsOf(d.state);
}
// a group's tuple as words (the mesh, its range, the shift's floats, the state pool entry): a repeat compares these, not
// the group's first record (SameTuple then only for the draws without a pool entry) -- graphics_internal.h FactTuple
using TupleWords = DrawTuple;
inline void TupleWordsOf(const Draw& d, TupleWords& t) {
    const ps2e_draw_mesh_params& m = d.Params();
    FactTuple(d, m, m.first, m.count, t);
}
std::vector<TupleWords> g_slot_tuples;   // (FullPass's: a slot's tuple)

// THE PLAN KEPT FROM THE PICTURE BEFORE (TE_GFX_VITA_PLAN_KEEP; unset / 1 = on, 0 = every picture planned). The full rule's
// plan is a function of its bucket's inputs alone: per entry of the drawing order its kind and state answers and, for a
// candidate, its tuple's words -- the mesh, its range, its shift's bits, its state pool entry (FullPass reads nothing else of
// a pooled candidate) -- and the sky's insertion point. Where those are the picture before's word for word the plan is the
// same: kept, not made again (the rooftop's far view: every picture; a moving camera changes the matrices, which the plan
// does not read). Never kept: the casters' rule in force (it reads the records), a candidate without a pool entry (its
// tuple compares states), a censused picture.
struct KeptPlan {
    bool valid = false;
    size_t count = 0, barrier = 0;
    std::vector<uint32_t> words;   // per entry: its kind and state bits; a candidate's 8 tuple words after it
    size_t used = 0;               // (the words in use: the storage is sized for the largest case)
    BatchPlan plan;
    PassOut out;                   // the plan's census numbers
};
constexpr int kKeptBuckets = 32;
KeptPlan g_kept[kKeptBuckets];
uint64_t g_kept_hits = 0, g_kept_asked = 0;
bool PlanKeepOn() {
    static const bool on = [] {
        const char* v = std::getenv("TE_GFX_VITA_PLAN_KEEP");
        return !(v && v[0] == '0');
    }();
    return on;
}
inline uint32_t KeptWord(const DrawFact& f) { return uint32_t(f.kind) | (uint32_t(f.sbits & 0x1Fu) << 8); }
inline bool KeptCandidate(uint32_t w) {
    const uint32_t kind = w & 0xFFu, sb = (w >> 8) & 15u;
    const bool copy = ((w >> 8) & kFactCopyDraw) != 0;
    return (sb & kFactDepthSafe) && !copy && kind == kFactUnlit && !(sb & kFactAlphaTest);
}
// the bucket's inputs as kept, against the picture's: true when every word is the same (and no candidate lacks a pool entry)
bool KeptMatches(const KeptPlan& kp, size_t count, size_t barrier, const PictureFacts& pf) {
    if (!kp.valid || kp.count != count || kp.barrier != barrier) return false;
    const uint32_t* w = kp.words.data();
    const uint32_t* const end = w + kp.used;
    for (size_t j = 0; j < count; ++j) {
        const uint32_t at = pf.perm[j];
        const uint32_t word = KeptWord(pf.facts[at]);
        if (w == end || *w++ != word) return false;
        if (!KeptCandidate(word)) continue;
        const DrawTuple& t = pf.tuples[at];
        if (!t.w[7] || end - w < 8) return false;
        uint32_t differ = 0;
        for (int k = 0; k < 8; ++k) differ |= w[k] ^ t.w[k];
        if (differ) return false;
        w += 8;
    }
    return w == end;
}
// `keep` (with the facts): the inputs it reads written for the next picture's KeptMatches, as it reads them
void FullPass(Draw* const* list, size_t count, size_t barrier, bool caster_rule, PassOut& out, const PictureFacts* pf,
              KeptPlan* keep = nullptr) {
    g_items.assign(count, Item{0, -1, -1});
    g_slots.clear();
    g_slot_tuples.clear();
    if (g_keys.empty()) g_keys.assign(kKeySlots, KeyEntry{0, -1, 0});
    if (++g_key_gen == 0) {
        for (KeyEntry& e : g_keys) e.gen = 0;
        std::memset(g_pool_gen, 0, sizeof g_pool_gen);
        g_key_gen = 1;
    }
    const uint32_t gen = g_key_gen;
    int32_t wall = -1;          // the latest slot no draw moves back over (a draw not depth-safe)
    int32_t sky_wall = -1;      // the last slot before the sky's insertion point
    int32_t last_caster = -1;   // the latest slot holding a draw that may record a caster (the casters' rule)
    // (the kinds: MakeFacts' where the casters' rule may apply -- it made the casters' flags --, else made here: KindOf's
    // answer from the record's own fields and the state's bits -- or the picture's facts: the same answers, made once with
    // the records' expansion, no record read here but a candidate's state without a pool entry)
    const bool facts = caster_rule;
    TupleWords t{};
    uint32_t* kw = nullptr;
    bool keep_ok = keep && pf;
    if (keep_ok) {
        keep->valid = false;
        if (keep->words.size() < count * 9) keep->words.resize(count * 9);
        kw = keep->words.data();
    }
    for (size_t j = 0; j < count; ++j) {
        if (j == barrier) sky_wall = int32_t(g_slots.size()) - 1;
        const Draw& d = *list[j];
        Item& it = g_items[j];
        uint32_t sb;
        uint8_t kind;
        bool copy;
        const DrawFact* f = nullptr;
        if (pf) {
            const uint32_t at = pf->perm[j];
            f = &pf->facts[at];
            sb = f->sbits & 15u;
            copy = (f->sbits & kFactCopyDraw) != 0;
            kind = facts ? g_facts[j].kind : f->kind;
            if (keep_ok) {
                const uint32_t word = KeptWord(*f);
                *kw++ = word;
                if (KeptCandidate(word)) {
                    const DrawTuple& kt = pf->tuples[at];
                    std::memcpy(kw, kt.w, sizeof kt.w);
                    kw += 8;
                    if (!kt.w[7]) keep_ok = false;   // (a candidate without a pool entry: never kept)
                }
            }
        } else {
            sb = StateBits(d, gen);
            copy = d.copy_frame;
            if (facts) {
                kind = g_facts[j].kind;
            } else {
                kind = d.copy_frame || (sb & 2u) ? kBlock
                       : d.mesh ? (d.skin_joints ? kBlock : (sb & 4u) ? kOther : d.Params().lit ? kLit : kUnlit)
                                : d.VertexCount() >= 3 ? kOther : kEmpty;
            }
        }
        if (kind == kEmpty) continue;   // (no vertices, no copy: nothing drawn -- no entry, nothing crossed)
        const bool caster = facts && g_facts[j].caster;
        const bool safe = (sb & 1u) && !copy;
        const bool cand = safe && kind == kUnlit && !(sb & 8u);
        int32_t g = -1;
        KeyEntry* e = nullptr;
        if (cand) {
            if (f) t = pf->tuples[pf->perm[j]];
            else TupleWordsOf(d, t);
            // (the key: 32-bit multiply-xor steps over the tuple's words -- the state's when it has no pool entry)
            uint32_t h = t.w[7] ? 0x811C9DC5u : uint32_t(StateHash(d.state, 0));
            for (uint32_t x : t.w) h = (h ^ x) * 16777619u;
            it.key = h ^ (h >> 15);
            g_keyed++;
            uint32_t at = uint32_t(it.key ^ (it.key >> 29)) & (kKeySlots - 1);
            for (;;) {
                KeyEntry& k = g_keys[at];
                if (k.gen != gen) {
                    k.gen = gen;
                    k.key = it.key;
                    k.slot = -1;
                    e = &k;
                    out.tuples++;
                    break;
                }
                if (k.key == it.key && k.slot >= 0) {
                    const TupleWords& u = g_slot_tuples[size_t(k.slot)];
                    uint32_t differ = 0;
                    for (int w = 0; w < 8; ++w) differ |= u.w[w] ^ t.w[w];
                    bool same = differ == 0;
                    // (no pool entry: the states' words decide, as SameTuple compares them)
                    if (same && !t.w[7]) same = SameTuple(*list[size_t(g_slots[size_t(k.slot)].head)], d, false);
                    if (same) {
                        e = &k;
                        g = k.slot;
                        break;
                    }
                }
                at = (at + 1) & (kKeySlots - 1);
            }
        }
        bool joined = false;
        if (g >= 0) {
            Slot& gs = g_slots[size_t(g)];
            const int why = gs.n >= 64 ? kRefFull : g <= sky_wall ? kRefSky : g <= wall ? kRefBlocking
                            : caster_rule && caster && last_caster > g ? kRefCasters : -1;
            if (why < 0) {
                const size_t crossed = g_slots.size() - 1 - size_t(g);
                g_items[size_t(gs.tail)].next = int32_t(j);
                gs.tail = int32_t(j);
                gs.n++;
                if (caster) {
                    gs.caster = true;
                    last_caster = std::max(last_caster, g);
                }
                it.slot = g;
                joined = true;
                out.joined++;
                if (crossed) {
                    out.joined_moved++;
                    out.crossed += crossed;
                }
            } else {
                out.refused[why]++;
            }
        }
        if (!joined) {
            Slot s;
            s.head = s.tail = int32_t(j);
            s.n = 1;
            s.blocking = !safe;
            s.caster = caster;
            it.slot = int32_t(g_slots.size());
            if (!safe) wall = it.slot;
            if (caster) last_caster = it.slot;
            g_slots.push_back(s);
            g_slot_tuples.push_back(t);   // (a candidate's tuple; another slot's words are never read)
            if (cand && e) e->slot = it.slot;   // the tuple's latest group
        }
    }
    out.entries += g_slots.size();
    for (const Slot& s : g_slots) {
        if (s.n < 2) continue;
        out.groups++;
        out.sizes[s.n == 2 ? 0 : s.n <= 4 ? 1 : s.n <= 8 ? 2 : s.n <= 16 ? 3 : 4]++;
    }
    if (keep_ok) {
        keep->used = size_t(kw - keep->words.data());
        keep->count = count;
        keep->barrier = barrier;
        keep->valid = true;
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
        const uint64_t sh = StateHash(d.state, d.mesh && d.Params().lit ? 1 : 0);
        for (uint32_t h = uint32_t(sh ^ (sh >> 29)) & (kKeySlots - 1);; h = (h + 1) & (kKeySlots - 1)) {
            KeyEntry& k = g_keys[h];
            if (k.gen != g_key_gen) { k.gen = g_key_gen; k.key = sh; c.states++; break; }
            if (k.key == sh) break;
        }
        // consecutive mesh draws of one state (and lit) but another mesh or range: of one matrix (one merged mesh would draw
        // both), of another matrix (an index concatenation: pre-transformed vertices)
        if (prev && d.mesh && prev->mesh && !d.skin_joints && !prev->skin_joints && !d.copy_frame &&
            (d.Params().lit != 0) == (prev->Params().lit != 0) && SameStateWords(d.state, prev->state) &&
            (d.mesh != prev->mesh || d.Params().first != prev->Params().first || d.Params().count != prev->Params().count)) {
            const bool same_matrix = std::memcmp(d.Params().screen, prev->Params().screen, sizeof d.Params().screen) == 0 &&
                                     std::memcmp(d.Params().offset, prev->Params().offset, sizeof d.Params().offset) == 0 &&
                                     std::memcmp(d.Params().scroll, prev->Params().scroll, sizeof d.Params().scroll) == 0;
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

// The bucket's facts for its passes: every draw's kind, and -- when the frame keeps a bounded number of shadow casters --
// which draws may record one; the boxes and depth ranges are made when a pass first asks. Answers the casters' rule:
// THE CASTERS' ORDER: the frame keeps its first shadow casters only, up to a bound (ps2e_draw.cpp CasterRoom), so where this
// bucket's draws that may record one outnumber the room left, WHICH are kept depends on their order -- then a draw that
// may record one never moves over another that may (the casters' records stay in their order; the others' moves change
// nothing in them). Where they fit the room, every one is kept whatever the order (and two that swap are box-disjoint:
// their re-stamps touch no common pixel).
bool MakeFacts(ps2e_draw* dev, Draw* const* list, size_t count) {
    g_facts.assign(count, Fact());
    for (size_t j = 0; j < count; ++j) g_facts[j].kind = KindOf(*list[j]);
    bool caster_rule = false;
    if (const int room = ps2e_draw_caster_room(dev)) {
        // (a mesh by RecordCasterMesh's tests asked without recording; a vertex draw or a GPU skin by its state alone -- more
        // than record one: the rule is on whenever the bucket might overflow the room)
        int potential = 0;
        for (size_t j = 0; j < count; ++j) {
            const Draw& d = *list[j];
            int c = 0;
            if (d.mesh && !d.skin_joints) c = ps2e_draw_mesh_caster_candidate(dev, &d.state, d.mesh, &d.Params());
            else if (d.mesh || d.VertexCount() >= 3) c = ps2e_draw_state_caster(dev, &d.state);
            g_facts[j].caster = uint8_t(c);
            potential += c;
        }
        caster_rule = potential > room;
    }
    return caster_rule;
}
// The full rule's facts: where the bucket could overflow the casters' room (more draws than room left), every draw's kind
// and casters' flag as MakeFacts makes them (the casters' rule may apply); elsewhere the rule is off whatever the flags say
// and none is made (FullPass makes each draw's kind itself)
bool MakeFactsFull(ps2e_draw* dev, Draw* const* list, size_t count) {
    const int room = ps2e_draw_caster_room(dev);
    if (room > 0 && count > size_t(room)) return MakeFacts(dev, list, count);
    return false;
}

const BatchPlan* PlanBucket(ps2e_draw* device, Draw* const* list, size_t count, size_t barrier, bool census, bool plan,
                            long picture, int bucket, const PictureFacts* facts) {
    if (!g_bo.read) ReadOptions();
    static BatchPlan out;
    if (plan && bucket >= 0 && bucket < 32 && !(g_bo.buckets & (1u << bucket))) plan = false;   // (diagnosis: the bucket mask)
    if (!census && !plan) return nullptr;
    const bool dump = g_bo.dump >= 0 && picture == g_bo.dump;
    const uint64_t t0 = plan ? sceKernelGetProcessTimeWide() : 0;
    const uint64_t t0c = census && !plan ? sceKernelGetProcessTimeWide() : 0;   // (the census' pass A when nothing is drawn by it)
    const uint64_t b0 = g_boxes_made, v0 = g_visits, k0 = g_keyed;
    const bool full = plan && g_bo.sort == 2;
    // (a censused picture's passes read every draw's casters flag -- the depth rule's caster-over-caster test does whatever
    // the room -- so it makes them all; the full rule alone needs them only where the room may overflow)
    // (the plan's phases on the profile's rows of their own: the facts, the groups, the plan handed over, the census)
    static const unsigned short prof_facts = te::prof::Register("rt plan: the draws' facts"),
                                prof_groups = te::prof::Register("rt plan: the groups"),
                                prof_out = te::prof::Register("rt plan: the plan handed over"),
                                prof_census = te::prof::Register("rt batching census");
    const int prof_slot = te::prof::ThreadSlot();
    bool caster_rule;
    {
        te::prof::Scope scope(prof_facts, prof_slot);
        caster_rule = full && !census ? MakeFactsFull(device, list, count) : MakeFacts(device, list, count);
    }
    // the plan kept from the picture before (KeptPlan): the same inputs word for word, the same plan
    KeptPlan* kept = nullptr;
    if (full && facts && !census && !caster_rule && PlanKeepOn() && bucket >= 0 && bucket < kKeptBuckets) {
        kept = &g_kept[bucket];
        g_kept_asked++;
        bool hit;
        {
            te::prof::Scope scope(prof_groups, prof_slot);
            hit = KeptMatches(*kept, count, barrier, *facts);
        }
        if (hit) {
            g_kept_hits++;
            const PassOut& k = kept->out;
            g_bc.plan_us += sceKernelGetProcessTimeWide() - t0;
            g_bc.planned_groups += k.groups;
            g_bc.planned_joined += k.joined;
            for (int r = 0; r < 6; ++r) g_bc.planned_refused[r] += k.refused[r];
            g_bc.planned_buckets++;
            return &kept->plan;
        }
    }
    // pass A: the plan (its crossing tests backward: the first refusal found soonest) -- on a censused picture in the
    // census's order, its reasons counted as before. With the full rule (TE_GFX_VITA_SORT=full) the plan is FullPass and a
    // censused picture's box-rule pass is made after it.
    PassOut a;
    {
        te::prof::Scope scope(prof_groups, prof_slot);
        if (full) FullPass(list, count, barrier, caster_rule, a, facts, kept);
        else PlanPass(device, list, count, barrier, false, caster_rule, a, dump, picture, bucket, g_bo.depth, !census);
    }
    const BatchPlan* result = nullptr;
    if (plan) {
        te::prof::Scope scope(prof_out, prof_slot);
        BatchPlan& dst = kept ? kept->plan : out;
        dst.next.assign(count, -1);
        dst.absorbed.assign(count, 0);
        for (const Slot& s : g_slots) {
            if (s.n < 2) continue;
            for (int32_t m = s.head; m >= 0; m = g_items[size_t(m)].next) {
                dst.next[size_t(m)] = g_items[size_t(m)].next;
                if (m != s.head) dst.absorbed[size_t(m)] = 1;
            }
        }
        if (kept && kept->valid) kept->out = a;   // (its inputs written by FullPass: the next picture's)
        g_bc.plan_us += sceKernelGetProcessTimeWide() - t0;
        g_bc.planned_groups += a.groups;
        g_bc.planned_joined += a.joined;
        g_bc.planned_boxes += g_boxes_made - b0;
        g_bc.planned_visits += g_visits - v0;
        g_bc.planned_keys += g_keyed - k0;
        for (int k = 0; k < 6; ++k) g_bc.planned_refused[k] += a.refused[k];
        g_bc.planned_buckets++;
        if (caster_rule) g_bc.planned_keep_casters++;
        result = &dst;
        if (g_check.on && !census && !full) {
            // the same bucket planned in the census's order: the same groups, draw for draw
            const uint64_t b1 = g_boxes_made, v1 = g_visits, k1 = g_keyed;
            PassOut f;
            PlanPass(device, list, count, barrier, false, caster_rule, f, false, picture, bucket, g_bo.depth, false);
            static std::vector<int32_t> next;
            static std::vector<uint8_t> absorbed;
            next.assign(count, -1);
            absorbed.assign(count, 0);
            for (const Slot& s : g_slots) {
                if (s.n < 2) continue;
                for (int32_t m = s.head; m >= 0; m = g_items[size_t(m)].next) {
                    next[size_t(m)] = g_items[size_t(m)].next;
                    if (m != s.head) absorbed[size_t(m)] = 1;
                }
            }
            const bool same = f.joined == a.joined && f.groups == a.groups && next == out.next && absorbed == out.absorbed;
            g_check.buckets++;
            if (!same) g_check.buckets_differ++;
            g_boxes_made = b1; g_visits = v1; g_keyed = k1;   // (the check's own work is not the plan's)
        }
    }
    if (census) {
        te::prof::Scope scope(prof_census, prof_slot);
        const uint64_t c0 = sceKernelGetProcessTimeWide();
        if (full) {   // (the census's pass A: the box rule in its order, as the census counts it)
            a = PassOut();
            PlanPass(device, list, count, barrier, false, caster_rule, a, dump, picture, bucket, g_bo.depth, false);
        }
        Add(g_bc, a);
        CountBucket(list, count);
        PassOut b;   // the lit meshes too (their lights equal)
        PlanPass(device, list, count, barrier, true, caster_rule, b, false, picture, bucket, false, false);
        g_bc.lit_tuples += b.tuples;
        g_bc.joined_lit += b.joined;
        // the depth-safe rule's pass (unlit, as pass A), unless pass A already was it
        PassOut dz;
        if (g_bo.depth) dz = a;
        else PlanPass(device, list, count, barrier, false, caster_rule, dz, false, picture, bucket, true, false);
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
        char b[1100];
        const double p = double(pf);
        auto rp = [&](int k) { return double(now.planned_refused[k] - last.planned_refused[k]) / p; };
        static uint64_t last_hits = 0, last_asked = 0;
        const uint64_t hits = g_kept_hits - last_hits, asked = g_kept_asked - last_asked;
        last_hits = g_kept_hits;
        last_asked = g_kept_asked;
        std::snprintf(b, sizeof b, "[te.gfx.vita] instancing plan per frame (TE_GFX_VITA_INSTANCE=%s%ld, %s): %.1f groups drawing %.1f "
                      "joined draws at their group's first draw; the plan's work: %.1f tuples keyed, %.1f screen boxes made, %.1f "
                      "entries tested; it took %.1f us a frame on the drawing thread (%llu frames); repeats not joined: overlap %.1f, "
                      "a draw of unknown extent / not depth-safe between %.1f, look-back %.1f, the sky %.1f, full %.1f, the casters' "
                      "order %.1f (buckets keeping the casters' order %llu of %llu); plans kept from the picture before "
                      "(TE_GFX_VITA_PLAN_KEEP) %llu of %llu asked\n",
                      g_bo.mode == 3 ? "ab" : "", g_bo.mode == 3 ? g_bo.ab_every : long(g_bo.mode),
                      g_bo.sort == 2 ? "the full rule" : g_bo.depth ? "the box rule + the depth-safe rule" : "the box rule",
                      double(now.planned_groups - last.planned_groups) / p, double(now.planned_joined - last.planned_joined) / p,
                      double(now.planned_keys - last.planned_keys) / p, double(now.planned_boxes - last.planned_boxes) / p,
                      double(now.planned_visits - last.planned_visits) / p, double(now.plan_us - last.plan_us) / p,
                      (unsigned long long)pf, rp(kRefOverlap), rp(kRefBlocking), rp(kRefLookback), rp(kRefSky), rp(kRefFull),
                      rp(kRefCasters), (unsigned long long)(now.planned_keep_casters - last.planned_keep_casters),
                      (unsigned long long)(now.planned_buckets - last.planned_buckets), (unsigned long long)hits,
                      (unsigned long long)asked);
        line += b;
    }
    if (g_check.on) {
        static PlanCheck lc;
        char b[400];
        std::snprintf(b, sizeof b, "[te.gfx.vita] instancing plan CHECK (TE_GFX_VITA_INSTANCE_CHECK=1): %llu buckets planned again in the "
                      "census's order, %llu with other groups; %llu boxes made again the scalar way, %llu differ; -0 coordinates %llu\n",
                      (unsigned long long)(g_check.buckets - lc.buckets), (unsigned long long)(g_check.buckets_differ - lc.buckets_differ),
                      (unsigned long long)(g_check.boxes - lc.boxes), (unsigned long long)(g_check.boxes_differ - lc.boxes_differ),
                      (unsigned long long)(g_check.negative_zero - lc.negative_zero));
        line += b;
        lc = g_check;
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
const BatchPlan* PlanBucket(ps2e_draw*, Draw* const*, size_t, size_t, bool, bool, long, int, const PictureFacts*) { return nullptr; }
void NotePlannedFrame() {}
std::string BatchCensusLine(double) { return std::string(); }

#endif

}  // namespace te::gfx
