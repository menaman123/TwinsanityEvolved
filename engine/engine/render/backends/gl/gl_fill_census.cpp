// render/backends/gl/gl_fill_census.cpp -- the Vita's fill census (gl_fill_census.h).
#include "engine/render/backends/gl/gl_fill_census.h"

#include "engine/platform/host/platform.h"

#if PS2R_HOST_VITA
#include PS2R_GL_HEADER
#include <psp2/gxm.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
extern SceGxmContext* gxm_context;   // vitaGL r1488's context (the raw mesh draws use it too: gl_shader_library.cpp)
void scene_reset(void);              // vitaGL's: begins the bound framebuffer's scene if needed, applies a changed scissor
}
#endif

namespace ps2r {
namespace render {

#if PS2R_HOST_VITA
namespace {

constexpr int kMaxDraws = 2048;   // counters per sampled frame (a draw past them is not measured: `over`)
constexpr int kSlots = 2;         // a slot's counters are read back at the next sampled frame, then it is reused
constexpr int kCores = 4;         // the SGX543MP4+: one counter block per core (the counts are summed)
constexpr int kPaths = 6;         // the vertex paths (FillCensusDraw's `path`)
constexpr int kGroups = 32;       // the frame buckets (FillCensusGroup)

// The programs' USSE instruction counts as Sony's compiler made them (a device log of an earlier build), for the
// census's work estimates: the fragment program by code, the vertex program by path
constexpr int kFragInstr[kFillCodes] = {2, 8, 20, 28, 41, 39, 52, 0, 2, 8, 20, 28, 41, 39, 52, 0, 229, 2475};
constexpr int kVertInstr[kPaths] = {74, 33, 43, 11, 15, 86};

struct FillState {
    bool read = false;
    int every = 0;                // TE_GFX_VITA_FILL
    bool ok = false;              // the visibility buffer is set
    uint32_t* buffer = nullptr;   // kCores blocks of kSlots x kMaxDraws counters
    unsigned stride = 0;          // bytes per core block
    uint64_t frames = 0;          // FillCensusFrame calls
    bool sampling = false;        // the frame being drawn is sampled
    int slot = 0;                 // its slot
    int frame_w = 512, frame_h = 512;
    int n[kSlots] = {0, 0};       // the slot's draws
    bool pending[kSlots] = {false, false};
    uint8_t code[kSlots][kMaxDraws];
    uint8_t path[kSlots][kMaxDraws];
    float px[kSlots][kMaxDraws];
    uint32_t verts[kSlots][kMaxDraws];
    float box[4] = {0, 0, -1, -1};
    bool have_box = false;
    unsigned next_verts = 0;
    bool test_on = false;
    // the visibility buffer is handed to GXM after a swap (FillCensusAfterSwap: outside a scene), retried a few
    // times; the frames are sampled once it is set
    bool want_set = false;
    int set_tries = 0;
    uint8_t group_now = 0;            // FillCensusGroup's (the frame bucket of the next draws)
    float next_area = -1.f;           // FillCensusArea's (< 0: not known)
    uint8_t group[kSlots][kMaxDraws];
    float area[kSlots][kMaxDraws];
    // the window (folded at each read-back)
    uint64_t sampled = 0, over = 0;
    uint64_t draws[kFillCodes] = {};
    double box_px[kFillCodes] = {}, passed[kFillCodes] = {};
    double path_verts[kPaths] = {}, path_draws[kPaths] = {};
    // per frame bucket: draws, boxes, the known areas (and the draws they are known for), samples passed, vertices
    double g_draws[kGroups] = {}, g_box[kGroups] = {}, g_area[kGroups] = {}, g_area_draws[kGroups] = {}, g_passed[kGroups] = {},
           g_verts[kGroups] = {}, g_blend_box[kGroups] = {};
    // per frame bucket and program: boxes and samples passed (the bucket line's split)
    double t_box[kGroups][kFillCodes] = {}, t_passed[kGroups][kFillCodes] = {};
    // vitaGL's OWN draws while the counters are on -- the full-screen mask-update quad its scene_reset draws at
    // every scene's start / scissor change (tests.c update_scissor_test) and each glClear's quad (misc.c glClear) -- counted
    // in a counter of their own (the last of a slot) instead of the next or the previous draw's: their samples per frame
    double foreign_passed = 0;
    uint64_t foreign_calls = 0;
    double frame_px = 512.0 * 512.0;
};
FillState g_fs;

void ReadOptions() {
    g_fs.read = true;
    const char* v = std::getenv("TE_GFX_VITA_FILL");
    g_fs.every = v && *v ? std::max(0, std::atoi(v)) : 0;
    if (g_fs.every > 0 && g_fs.every < 8) g_fs.every = 8;   // the read-back needs the GPU done with the slot
    if (!g_fs.every) return;
    if (!gxm_context) {
        std::fprintf(stderr, "[gl] fill census (TE_GFX_VITA_FILL=%d): no GXM context: off\n", g_fs.every);
        g_fs.every = 0;
        return;
    }
    g_fs.stride = unsigned(kSlots * kMaxDraws * sizeof(uint32_t));
    const unsigned bytes = g_fs.stride * kCores;
    // vitaGL's GPU-mapped memory for the CPU (its own occlusion queries' kind: vglForceAlloc), aligned to 64 bytes
    uint8_t* raw = static_cast<uint8_t*>(vglForceAlloc(bytes + 64));
    if (!raw) {
        std::fprintf(stderr, "[gl] fill census: no memory for the visibility counters: off\n");
        g_fs.every = 0;
        return;
    }
    g_fs.buffer = reinterpret_cast<uint32_t*>((reinterpret_cast<uintptr_t>(raw) + 63) & ~uintptr_t(63));
    std::memset(g_fs.buffer, 0, bytes);
    // handed to GXM after the next swap (FillCensusAfterSwap): this is the frame's submission, within a scene -- where
    // sceGxmSetVisibilityBuffer refuses (a device run: "OFF (sceGxmSetVisibilityBuffer refused)")
    g_fs.want_set = true;
}

uint32_t* Counter(int core, int slot, int i) {
    return g_fs.buffer + size_t(core) * (g_fs.stride / sizeof(uint32_t)) + size_t(slot) * kMaxDraws + size_t(i);
}

// a slot's counters into the window's sums
void Fold(int slot) {
    if (!g_fs.pending[slot]) return;
    g_fs.pending[slot] = false;
    g_fs.sampled++;
    {
        uint64_t samples = 0;
        for (int c = 0; c < kCores; ++c) samples += *Counter(c, slot, kMaxDraws - 1);
        g_fs.foreign_passed += double(samples);
    }
    for (int i = 0; i < g_fs.n[slot]; ++i) {
        uint64_t samples = 0;
        for (int c = 0; c < kCores; ++c) samples += *Counter(c, slot, i);
        const int code = g_fs.code[slot][i];
        g_fs.draws[code]++;
        g_fs.box_px[code] += g_fs.px[slot][i];
        g_fs.passed[code] += double(samples);
        const int p = g_fs.path[slot][i];
        g_fs.path_draws[p] += 1.0;
        g_fs.path_verts[p] += double(g_fs.verts[slot][i]);
        const int gr = g_fs.group[slot][i] < kGroups ? g_fs.group[slot][i] : kGroups - 1;
        g_fs.g_draws[gr] += 1.0;
        g_fs.g_box[gr] += g_fs.px[slot][i];
        g_fs.g_passed[gr] += double(samples);
        g_fs.g_verts[gr] += double(g_fs.verts[slot][i]);
        g_fs.t_box[gr][code] += g_fs.px[slot][i];
        g_fs.t_passed[gr][code] += double(samples);
        if (g_fs.area[slot][i] >= 0.f) {
            g_fs.g_area[gr] += g_fs.area[slot][i];
            g_fs.g_area_draws[gr] += 1.0;
        }
    }
}

// vitaGL's own draws from here to the next FillCensusDraw count in the slot's last counter (FillState::foreign_passed)
void ForeignIndex() {
    const unsigned index = unsigned(g_fs.slot * kMaxDraws + (kMaxDraws - 1));
    sceGxmSetFrontVisibilityTestIndex(gxm_context, index);
    sceGxmSetBackVisibilityTestIndex(gxm_context, index);
}

void TestOff() {
    if (!g_fs.test_on) return;
    g_fs.test_on = false;
    sceGxmSetFrontVisibilityTestEnable(gxm_context, SCE_GXM_VISIBILITY_TEST_DISABLED);
    sceGxmSetBackVisibilityTestEnable(gxm_context, SCE_GXM_VISIBILITY_TEST_DISABLED);
}

}  // namespace

bool FillCensusFrame(int frame_w, int frame_h) {
    if (!g_fs.read) ReadOptions();
    g_fs.have_box = false;
    g_fs.next_verts = 0;
    if (!g_fs.every || !g_fs.ok) return false;   // (the buffer is set after a swap: FillCensusAfterSwap)
    TestOff();
    g_fs.sampling = (g_fs.frames++ % uint64_t(g_fs.every)) == 0;
    if (!g_fs.sampling) return false;
    g_fs.frame_w = std::max(1, frame_w);
    g_fs.frame_h = std::max(1, frame_h);
    g_fs.frame_px = double(g_fs.frame_w) * double(g_fs.frame_h);
    // the other slot holds the previous sample's counters (every >= 8 frames ago: done); this one's were read back then
    g_fs.slot ^= 1;
    Fold(g_fs.slot ^ 1);
    Fold(g_fs.slot);   // (only after a sample was skipped: never in a steady run)
    for (int c = 0; c < kCores; ++c) std::memset(Counter(c, g_fs.slot, 0), 0, kMaxDraws * sizeof(uint32_t));
    g_fs.n[g_fs.slot] = 0;
    g_fs.pending[g_fs.slot] = true;
    return true;
}

bool FillCensusSampling() { return g_fs.sampling; }

void FillCensusBox(const float box[4]) {
    if (!g_fs.sampling) return;
    std::memcpy(g_fs.box, box, sizeof g_fs.box);
    g_fs.have_box = true;
}

void FillCensusVerts(unsigned count) {
    if (g_fs.sampling) g_fs.next_verts = count;
}

void FillCensusDraw(int code, double px, int path) {
    if (!g_fs.sampling) return;
    if (g_fs.have_box) {
        g_fs.have_box = false;
        const float x0 = std::max(0.f, std::min(g_fs.box[0], float(g_fs.frame_w)));
        const float x1 = std::max(0.f, std::min(g_fs.box[2], float(g_fs.frame_w)));
        const float y0 = std::max(0.f, std::min(g_fs.box[1], float(g_fs.frame_h)));
        const float y1 = std::max(0.f, std::min(g_fs.box[3], float(g_fs.frame_h)));
        if (g_fs.box[2] >= g_fs.box[0]) px = (x1 > x0 && y1 > y0) ? double(x1 - x0) * double(y1 - y0) : 0.0;
    } else {
        px = std::min(px, g_fs.frame_px);
    }
    const unsigned verts = g_fs.next_verts;
    g_fs.next_verts = 0;
    const int slot = g_fs.slot;
    int& n = g_fs.n[slot];
    if (n >= kMaxDraws - 1 || code < 0 || code >= kFillCodes) {   // (the slot's last counter: vitaGL's own draws)
        g_fs.over++;
        return;
    }
    // vitaGL's scene_reset (which this draw runs first: its glDraw* / the raw draws' RawSubmit) begins the scene and draws
    // its full-screen mask-update quad at a scene's start or a scissor change: run here with the foreign counter set, so
    // that quad does not count as this draw's samples (a device log of the census: the frame's first draw "passed 2.000"
    // frames for one full-frame quad)
    if (!g_fs.test_on) {
        g_fs.test_on = true;
        sceGxmSetFrontVisibilityTestOp(gxm_context, SCE_GXM_VISIBILITY_TEST_OP_INCREMENT);
        sceGxmSetBackVisibilityTestOp(gxm_context, SCE_GXM_VISIBILITY_TEST_OP_INCREMENT);
        sceGxmSetFrontVisibilityTestEnable(gxm_context, SCE_GXM_VISIBILITY_TEST_ENABLED);
        sceGxmSetBackVisibilityTestEnable(gxm_context, SCE_GXM_VISIBILITY_TEST_ENABLED);
    }
    ForeignIndex();
    scene_reset();
    const unsigned index = unsigned(slot * kMaxDraws + n);
    sceGxmSetFrontVisibilityTestIndex(gxm_context, index);
    sceGxmSetBackVisibilityTestIndex(gxm_context, index);
    g_fs.code[slot][n] = uint8_t(code);
    g_fs.path[slot][n] = uint8_t(path >= 0 && path < kPaths ? path : 0);
    g_fs.px[slot][n] = float(px);
    g_fs.verts[slot][n] = verts;
    g_fs.group[slot][n] = g_fs.group_now;
    g_fs.area[slot][n] = g_fs.next_area;
    g_fs.next_area = -1.f;
    ++n;
}

void FillCensusAfterSwap() {
    if (!g_fs.want_set || g_fs.ok || !g_fs.every) return;
    const int r = sceGxmSetVisibilityBuffer(gxm_context, g_fs.buffer, g_fs.stride);
    ++g_fs.set_tries;
    if (r >= 0) {
        g_fs.ok = true;
        g_fs.want_set = false;
        std::fprintf(stderr, "[gl] fill census (TE_GFX_VITA_FILL=%d): on (the visibility buffer set after swap %d) -- one frame in %d, "
                     "every draw its own visibility counter (samples passing its depth / stencil tests), its true screen box, its "
                     "vertices and its frame bucket, per program and per bucket\n", g_fs.every, g_fs.set_tries, g_fs.every);
        return;
    }
    if (g_fs.set_tries < 8) return;   // (retried at the next swaps)
    std::fprintf(stderr, "[gl] fill census (TE_GFX_VITA_FILL=%d): OFF -- sceGxmSetVisibilityBuffer refused %d times after a swap "
                 "(the last: 0x%08X)\n", g_fs.every, g_fs.set_tries, unsigned(r));
    g_fs.want_set = false;
    g_fs.every = 0;
}

void FillCensusGroup(int group) { g_fs.group_now = uint8_t(group < 0 ? 0 : group >= kGroups ? kGroups - 1 : group); }

void FillCensusForeign() {
    if (!g_fs.sampling || !g_fs.test_on) return;
    ForeignIndex();
    g_fs.foreign_calls++;
}

void FillCensusArea(double px) {
    if (g_fs.sampling) g_fs.next_area = px < 0 ? -1.f : float(px);   // (< 0: not known)
}

void FillCensusEnd() {
    if (!g_fs.every) return;
    TestOff();
    g_fs.sampling = false;
    g_fs.have_box = false;
    g_fs.next_verts = 0;
}

std::string FillCensusLines() {
    if (!g_fs.every || !g_fs.sampled) return std::string();
    static const char* const kNames[kFillCodes] = {
        "lean 0 no colour", "lean 1 untextured", "lean 2 untextured, alpha test", "lean 3 modulate", "lean 4 modulate mip",
        "lean 5 modulate, alpha test", "lean 6 modulate mip, alpha test", "lean 7", "skin 0 no colour", "skin 1", "skin 2",
        "skin 3 modulate", "skin 4 modulate mip", "skin 5 modulate, alpha test", "skin 6 modulate mip, alpha test", "skin 7",
        "fast", "full"};
    static const char* const kPathNames[kPaths] = {"generic", "mesh", "lit mesh", "vertices", "raw", "skin (indices)"};
    const double frames = double(g_fs.sampled), unit = g_fs.frame_px * frames;
    double all_draws = 0, all_box = 0, all_passed = 0, pt_passed = 0, pt_box = 0, frag_instr = 0, pt_instr = 0;
    std::string out = "[te.gfx.vita] fill per sampled frame (TE_GFX_VITA_FILL; the GPU's visibility counters): draws, true screen "
                      "boxes and samples passing the depth / stencil tests (frames' worth of the target):";
    for (int c = 0; c < kFillCodes; ++c) {
        if (!g_fs.draws[c]) continue;
        char b[200];
        std::snprintf(b, sizeof b, " %s %.1f draws, boxes %.2f, passed %.3f;", kNames[c], double(g_fs.draws[c]) / frames,
                      g_fs.box_px[c] / unit, g_fs.passed[c] / unit);
        out += b;
        all_draws += double(g_fs.draws[c]);
        all_box += g_fs.box_px[c];
        all_passed += g_fs.passed[c];
        frag_instr += g_fs.passed[c] * kFragInstr[c];
        if (c == 2 || c == 5 || c == 6 || c == kFillSkin + 2 || c == kFillSkin + 5 || c == kFillSkin + 6) {
            pt_passed += g_fs.passed[c];
            pt_box += g_fs.box_px[c];
            pt_instr += g_fs.passed[c] * kFragInstr[c];
        }
    }
    char t[400];
    std::snprintf(t, sizeof t, " ALL %.1f draws, boxes %.2f, passed %.3f (the alpha-tested: boxes %.2f, passed %.3f); %llu frames "
                  "sampled, %llu draws past the counters\n", all_draws / frames, all_box / unit, all_passed / unit, pt_box / unit,
                  pt_passed / unit, (unsigned long long)g_fs.sampled, (unsigned long long)g_fs.over);
    out += t;
    // the vertex side and the instruction-weighted totals (the fragments that passed x their program's instructions: an
    // upper bound of the fragment work -- the deferred renderer shades an opaque pixel once whatever passed before it)
    std::string v = "[te.gfx.vita] fill per sampled frame, vertex programs: vertices by path:";
    double vert_instr = 0;
    for (int p = 0; p < kPaths; ++p) {
        if (!g_fs.path_draws[p]) continue;
        char b[120];
        std::snprintf(b, sizeof b, " %s %.0f (%.1f draws, %d instructions)", kPathNames[p], g_fs.path_verts[p] / frames,
                      g_fs.path_draws[p] / frames, kVertInstr[p]);
        v += b;
        vert_instr += g_fs.path_verts[p] * kVertInstr[p];
    }
    char w[300];
    std::snprintf(w, sizeof w, "; USSE instructions per frame: vertex %.2f M, fragment for the samples passed %.2f M (of them the "
                  "alpha-tested %.2f M; the opaque ones' shading is at most one per visible pixel)\n", vert_instr / frames / 1e6,
                  frag_instr / frames / 1e6, pt_instr / frames / 1e6);
    v += w;
    out += v;
    // per frame bucket (graphics.cpp's buckets: 0 the sky, 2 the opaque scenery, 24 the HUD, 26 the fonts ...): draws,
    // boxes, the rasterised area where it is known (vertex draws: their triangles; "of N" = for how many draws), samples
    // passed, vertices -- frames' worth / per frame
    std::string b = "[te.gfx.vita] fill per sampled frame by bucket (draws, boxes, triangles' area of the vertex draws, passed, vertices):";
    for (int gr = 0; gr < kGroups; ++gr) {
        if (!g_fs.g_draws[gr]) continue;
        char e[200];
        std::snprintf(e, sizeof e, " b%d %.1f, %.2f, %.2f of %.1f, %.3f, %.0f;", gr, g_fs.g_draws[gr] / frames, g_fs.g_box[gr] / unit,
                      g_fs.g_area[gr] / unit, g_fs.g_area_draws[gr] / frames, g_fs.g_passed[gr] / unit, g_fs.g_verts[gr] / frames);
        b += e;
        g_fs.g_draws[gr] = g_fs.g_box[gr] = g_fs.g_area[gr] = g_fs.g_area_draws[gr] = g_fs.g_passed[gr] = g_fs.g_verts[gr] = 0;
        g_fs.g_blend_box[gr] = 0;
    }
    b += "\n";
    out += b;
    // each bucket's split by program (boxes / passed, frames' worth; the parts of at least 0.005 of either) and
    // vitaGL's own quads (counted apart: FillCensusDraw / FillCensusForeign)
    static const char* const kShort[kFillCodes] = {"L0", "L1", "L2", "L3", "L4", "L5", "L6", "L7", "S0", "S1", "S2", "S3", "S4",
                                                   "S5", "S6", "S7", "fast", "full"};
    std::string x = "[te.gfx.vita] fill per sampled frame by bucket and program (L = lean, S = skin; boxes / passed):";
    for (int gr = 0; gr < kGroups; ++gr) {
        std::string part;
        for (int c = 0; c < kFillCodes; ++c) {
            const double bx = g_fs.t_box[gr][c] / unit, ps = g_fs.t_passed[gr][c] / unit;
            g_fs.t_box[gr][c] = g_fs.t_passed[gr][c] = 0;
            if (bx < 0.005 && ps < 0.005) continue;
            char e[64];
            std::snprintf(e, sizeof e, " %s %.2f/%.3f", kShort[c], bx, ps);
            part += e;
        }
        if (part.empty()) continue;
        x += " b" + std::to_string(gr) + ":" + part + ";";
    }
    char f[260];
    std::snprintf(f, sizeof f, "; vitaGL's own quads (the scene-start / scissor mask, clears) and the screen copies' passes "
                  "(%.1f clears / copies a frame) passed %.3f\n", double(g_fs.foreign_calls) / frames, g_fs.foreign_passed / unit);
    x += f;
    out += x;
    g_fs.foreign_passed = 0;
    g_fs.foreign_calls = 0;
    g_fs.sampled = g_fs.over = 0;
    for (int c = 0; c < kFillCodes; ++c) {
        g_fs.draws[c] = 0;
        g_fs.box_px[c] = g_fs.passed[c] = 0;
    }
    for (int p = 0; p < kPaths; ++p) g_fs.path_verts[p] = g_fs.path_draws[p] = 0;
    return out;
}

#else

bool FillCensusFrame(int, int) { return false; }
void FillCensusBox(const float*) {}
bool FillCensusSampling() { return false; }
void FillCensusVerts(unsigned) {}
void FillCensusDraw(int, double, int) {}
void FillCensusEnd() {}
void FillCensusAfterSwap() {}
void FillCensusGroup(int) {}
void FillCensusForeign() {}
void FillCensusArea(double) {}
std::string FillCensusLines() { return std::string(); }

#endif

}  // namespace render
}  // namespace ps2r
