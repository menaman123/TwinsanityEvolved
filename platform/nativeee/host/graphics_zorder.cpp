// platform/nativeee (host side) -- the Vita's draw order for the GPU's hidden-surface removal (TE_GFX_VITA_ZORDER).
//
// The SGX543 is a tile-based deferred renderer: an OPAQUE fragment (no blend, no discard) is shaded once per pixel
// whatever the overdraw (its ISP keeps only the nearest), but a PUNCH-THROUGH fragment -- one an alpha test may discard
// (the lean programs' LEAN_ATEST 1: ATE on, a test that can fail, AFAIL KEEP) -- is shaded wherever it passes the depth
// test at the moment it arrives, because only its fragment program says whether it is kept. Drawn in the game's order
// (the trees' leaves before the ground they stand on, far canopies before near ones) most punch-through fragments are
// shaded and later covered. So within a run of draws whose result does not depend on their order, the opaque ones are
// drawn first (their order kept: HSR does not need more) and the punch-through ones after them, near to far: their
// fragments behind what is already drawn fail the depth test and are never shaded.
//
// WHY THE PICTURE IS THE SAME. A run holds only draws that test depth GEQUAL or GREATER (larger GS z = nearer), write
// depth, write colour, do not blend, use no stencil, read neither the frame (no screen copy before or in them) nor its
// alpha (DATE), and whose fragments each write colour and depth together or nothing (an alpha test that keeps or
// discards the whole fragment; AFAIL FB_ONLY / ZB_ONLY / RGB_ONLY write every fragment on the Vita: the lean programs
// leave them, or scale their colour, and the depth mask is the draw's). For such draws a pixel ends with the colour and
// depth of its nearest fragment, whatever order they come in -- except where two fragments of the run have EXACTLY the
// same depth at one pixel (then GEQUAL keeps the later one, GREATER the earlier one). Ties between two draws of the same
// surface (a material's second pass over a part: same mesh and matrix, or the same vertices) are systematic, so the
// order never puts a draw ahead of an earlier draw of the same surface: punch-through draws are sorted stably (two
// draws of one surface have the same key) and a run is cut before an opaque draw that repeats a punch-through draw's
// surface earlier in it. A model's own parts are LAYERED too -- an eye patch over an eyeball, a mouth on a face, within
// the Vita's depth quantum over whole regions (seen: in the arena's intro cutscene the white bear lost its eye,
// pictures ~4799-5172, its parts reordered): so (a) the lit meshes and the GPU skins -- the characters, whose parts sit on
// joints of their own -- are not reordered at all (barriers: ZClass), and (b) among the rest, the order never swaps two
// draws of one instance (one model -> screen matrix): a run is cut before an opaque part of a punch-through draw's
// instance earlier in it, and one instance's punch-through draws sort as one group (their relative order kept).
// Coplanar ties between different scenery instances remain the only difference (isolated pixels in the stamped
// recordings of the hub walk, the title's island pan and the arena: a tree's base on the ground).
//
//   TE_GFX_VITA_ZORDER  unset = 2; 0 = the game's order; 1 = punch-through after the others, in their order;
//                       2 = ... sorted near to far (each draw by its nearest point); 3 = ... sorted near to far by runs of
//                       one state (consecutive same-state punch-through draws kept together: the drawing thread's
//                       again-path keeps them cheap); abN[:M] = mode M (default 2) for presents 1..N, the game's order
//                       for N+1..2N, ... (an in-run A/B: every census window of N presents is one arm)
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "graphics_internal.h"

namespace te::gfx {

#if defined(__vita__)
namespace {

struct ZOrderOptions {
    bool read = false;
    int mode = 2;
    int ab = 0;   // abN: the arms flip every N presents
    bool fold = false;  // TE_GFX_VITA_ATEST_FOLD=1 (see FoldAlphaTests: exact, but in the hub walk no draw folds -- the
                        // alpha-tested textures have cut-out texels -- so off by default: its lookups cost the drawing thread)
};
ZOrderOptions g_zo;

// The census (the drawing thread's; NotePace reads it, a frame old at most)
struct ZOrderCensus {
    uint64_t frames = 0, draws = 0, barriers = 0, opaque = 0, punch = 0, runs = 0, runs_changed = 0, hoisted = 0,
             punch_moved = 0, cuts = 0, unknown_keys = 0, frames_on = 0;
    uint64_t cuts_instance = 0;   // runs cut before an opaque part of a punch-through draw's instance (InstanceKey)
    uint64_t tested = 0, folded = 0, unknown_floor = 0;   // the alpha-test fold: draws with a test that can fail by its state
};
ZOrderCensus g_zc;

void ReadOptions() {
    g_zo.read = true;
    if (const char* f = std::getenv("TE_GFX_VITA_ATEST_FOLD")) g_zo.fold = f[0] == '1';
    std::fprintf(stderr, "[te.gfx] vita alpha-test fold (TE_GFX_VITA_ATEST_FOLD=%d): %s\n", g_zo.fold ? 1 : 0, g_zo.fold
                 ? "a test no fragment can fail (the vertices' and the texture's smallest alpha) is drawn as no test"
                 : "off");
    const char* v = std::getenv("TE_GFX_VITA_ZORDER");
    if (v && *v) {
        if (std::strncmp(v, "ab", 2) == 0) {
            g_zo.ab = std::max(1, std::atoi(v + 2));
            if (g_zo.ab == 1 && !std::isdigit(static_cast<unsigned char>(v[2]))) g_zo.ab = 300;
            const char* c = std::strchr(v, ':');
            g_zo.mode = c ? std::max(0, std::min(3, std::atoi(c + 1))) : 2;
        } else {
            g_zo.mode = std::max(0, std::min(3, std::atoi(v)));
        }
    }
    static const char* const kNames[4] = {"the game's order", "punch-through draws after the others (their order kept)",
                                          "punch-through draws after the others, near to far",
                                          "punch-through draws after the others, near to far by runs of one state"};
    if (g_zo.ab)
        std::fprintf(stderr, "[te.gfx] vita draw order (TE_GFX_VITA_ZORDER=ab%d:%d): %s for presents 1-%d, the game's order for "
                     "%d-%d, ... (A/B)\n", g_zo.ab, g_zo.mode, kNames[g_zo.mode], g_zo.ab, g_zo.ab + 1, 2 * g_zo.ab);
    else
        std::fprintf(stderr, "[te.gfx] vita draw order (TE_GFX_VITA_ZORDER=%d): %s\n", g_zo.mode, kNames[g_zo.mode]);
}

// 0 = keeps its place and ends a run; 1 = in a run, never discards; 2 = in a run, punch-through (see the file's head)
int ZClass(const Draw& d) {
    const ps2e_draw_state& s = d.state;
    if (d.copy_frame || s.texture == PS2E_DRAW_TEXTURE_FRAME_COPY || s.stencil != PS2E_DRAW_STENCIL_OFF || s.no_colour_write ||
        s.blend || s.dest_alpha_test || !s.depth_write ||
        (s.depth_test != PS2E_DRAW_DEPTH_GEQUAL && s.depth_test != PS2E_DRAW_DEPTH_GREATER))
        return 0;
    if (!d.mesh && d.VertexCount() < 3) return 0;
    // the LIT meshes and the GPU skins (the characters: parts on joints of their own -- an eyeball under a fur patch, a
    // mouth on a face -- layered within the Vita's depth quantum, which InstanceKey's matrix does not group) keep their
    // place: the arena cutscene's white bear lost its eye when its parts were reordered (bucket 3, lit
    // meshes 968/969); the scenery (unlit, prelit) is where the punch-through overdraw is
    if (d.mesh && (d.mesh_params.lit || d.skin_joints)) return 0;
    if (!s.alpha_test || s.alpha_fail != 0) return 1;   // no test, or AFAIL other than KEEP: every fragment is written
    const int ref = s.alpha_reference;
    switch (s.alpha_method) {
        case PS2E_DRAW_ATST_ALWAYS: return 1;
        case PS2E_DRAW_ATST_GEQUAL: return ref <= 0 ? 1 : 2;
        case PS2E_DRAW_ATST_LEQUAL: return ref >= 255 ? 1 : 2;
        default: return 2;   // NEVER, LESS, EQUAL, GREATER, NOTEQUAL: some alpha fails
    }
}

// The draw's surface, for the same-surface rule: a mesh draw by its mesh, range and matrix (its translation row: another
// instance of the mesh stands elsewhere); a vertex draw by its count and first and last vertices' positions (a second pass
// over a CPU-built part repeats its vertices). A collision only cuts a run more (the safe side).
uint64_t SurfaceKey(const Draw& d) {
    uint32_t w[8];
    if (d.mesh) {
        w[0] = 0x6D657368u ^ d.mesh;
        w[1] = uint32_t(d.mesh_params.first);
        w[2] = uint32_t(d.mesh_params.count);
        std::memcpy(&w[3], &d.mesh_params.screen[12], 4 * sizeof(float));
        w[7] = uint32_t(d.skin_joints);
    } else {
        const ps2e_draw_vertex* v = d.Vertices();
        const size_t n = d.VertexCount();
        w[0] = uint32_t(n);
        std::memcpy(&w[1], &v[0].x, 3 * sizeof(float));
        std::memcpy(&w[4], &v[n - 1].x, 3 * sizeof(float));
        w[7] = 0x76657274u;
    }
    uint64_t h = 1469598103934665603ull;
    for (uint32_t x : w) h = (h ^ x) * 1099511628211ull;
    return h;
}

// The draw's nearest GS depth (the sort key: larger = nearer); `known` false when only a stand-in could be had
float NearZ(ps2e_draw* dev, const Draw& d, bool* known) {
    *known = true;
    if (d.mesh) {
        const float* m = d.mesh_params.screen;
        if (!d.skin_joints) {
            float zn = 0.f, zf = 0.f;
            if (ps2e_draw_mesh_depth_range(dev, d.mesh, &d.mesh_params, &zn, &zf)) return zn;
        }
        // a skin (its bind-pose bounds say nothing of where it is) or bounds not known: the model's origin
        *known = false;
        return m[15] > 1e-6f ? m[14] / m[15] : 0.f;
    }
    const ps2e_draw_vertex* v = d.Vertices();
    const size_t n = d.VertexCount();
    const size_t step = std::max<size_t>(1, n / 64);
    float z = -1e30f;
    for (size_t i = 0; i < n; i += step) z = std::max(z, v[i].z);
    z = std::max(z, v[n - 1].z);
    return z;
}

int ModeNow(uint64_t presents) {
    if (!g_zo.ab) return g_zo.mode;
    return ((presents / uint64_t(g_zo.ab)) % 2) == 0 ? g_zo.mode : 0;
}

// The model instance (and joint) a mesh draw belongs to: its whole model -> screen matrix (0: a vertex draw, not known).
// A model's parts drawn through one matrix are LAYERED on purpose (an eye patch over an eyeball, a mouth on a face): such
// parts lie within the Vita's depth quantum of each other over whole regions, so their draw order decides those regions
// (the characters' parts on joints of their own have other matrices: ZClass keeps lit meshes and skins out of the runs
// altogether). The order therefore never swaps two draws of one instance: an opaque draw is not moved before a
// punch-through draw of its own instance (the run is cut there), and the punch-through draws of one instance keep their
// relative order (they sort as one group, by the group's nearest key). A collision only cuts / groups more (safe side).
uint64_t InstanceKey(const Draw& d) {
    if (!d.mesh) return 0;
    uint32_t w[16];
    std::memcpy(w, d.mesh_params.screen, sizeof w);
    // a model's parts come one after the other through one matrix: the previous key again when the matrix's
    // bytes are the previous draw's (the same words hash to the same key; the 64-bit multiplies are the A9's dearest)
    static uint32_t last_w[16];
    static uint64_t last_key = 0;
    if (last_key && std::memcmp(w, last_w, sizeof w) == 0) return last_key;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t x : w) h = (h ^ x) * 1099511628211ull;
    h = h ? h : 1;
    std::memcpy(last_w, w, sizeof w);
    last_key = h;
    return h;
}

// The run's punch-through instances (open addressing, a generation per run: no clearing): each with its nearest key
struct InstanceSet {
    static constexpr uint32_t kSize = 1024;   // a power of two; at most kMax members (then the run is cut: the safe side)
    static constexpr uint32_t kMax = 512;
    uint64_t key[kSize] = {};
    uint32_t gen[kSize] = {};
    float zmax[kSize] = {};
    uint32_t g = 0, n = 0;
    void Reset() { ++g; n = 0; if (!g) { std::memset(gen, 0, sizeof gen); g = 1; } }
    uint32_t Slot(uint64_t k) const {
        uint32_t s = uint32_t(k ^ (k >> 29)) & (kSize - 1);
        while (gen[s] == g && key[s] != k) s = (s + 1) & (kSize - 1);
        return s;
    }
    bool Has(uint64_t k) const { return gen[Slot(k)] == g; }
    bool Add(uint64_t k, float z) {   // false: full
        const uint32_t s = Slot(k);
        if (gen[s] == g) { zmax[s] = std::max(zmax[s], z); return true; }
        if (n >= kMax) return false;
        gen[s] = g; key[s] = k; zmax[s] = z; ++n;
        return true;
    }
    float Near(uint64_t k) const { return zmax[Slot(k)]; }
};
InstanceSet g_inst;

struct Item {
    uint32_t index;   // in the run
    float key;
    uint64_t inst;    // InstanceKey (0: a vertex draw)
};

// THE ALPHA-TEST FOLD. A draw's alpha test whose every fragment passes is no test: drawn without it, the picture is the
// same and the draw takes the lean program without the test -- no discard (the GPU's hidden-surface removal applies to
// it) and fewer instructions. A fragment's PS2 alpha is at least ps2e_draw_alpha_floor (its vertices' smallest alpha and
// its texture's smallest texel through the program's MODULATE, each lowered by one for the rounding of the iterators
// and the filter), so GEQUAL AREF with the floor >= AREF (GREATER: > AREF) cannot fail.
void FoldAlphaTests(ps2e_draw* dev, Draw** draws, size_t used) {
    for (size_t i = 0; i < used; ++i) {
        ps2e_draw_state& s = draws[i]->state;
        if (!s.alpha_test || s.alpha_fail != 0) continue;   // (AFAIL other than KEEP never discards: nothing to gain)
        if (s.alpha_method != PS2E_DRAW_ATST_GEQUAL && s.alpha_method != PS2E_DRAW_ATST_GREATER) continue;
        if (s.alpha_method == PS2E_DRAW_ATST_GEQUAL && s.alpha_reference == 0) continue;   // (never fails: the program knows)
        g_zc.tested++;
        const Draw& d = *draws[i];
        if (d.mesh && d.skin_joints) { g_zc.unknown_floor++; continue; }
        const int lo = ps2e_draw_alpha_floor(dev, &s, d.mesh, d.mesh ? d.mesh_params.lit : 0, d.mesh ? nullptr : d.Vertices(),
                                                d.mesh ? 0 : int(d.VertexCount()));
        if (lo < 0) { g_zc.unknown_floor++; continue; }
        const int ref = s.alpha_reference;
        const bool passes = s.alpha_method == PS2E_DRAW_ATST_GEQUAL ? lo >= ref : lo > ref;
        if (!passes) continue;
        s.alpha_test = 0;
        g_zc.folded++;
    }
}

}  // namespace

void ZOrderDraws(ps2e_draw* dev, Draw** draws, size_t used, uint64_t presents) {
    if (!g_zo.read) ReadOptions();
    if (g_zo.fold && dev) FoldAlphaTests(dev, draws, used);
    const int mode = ModeNow(presents);
    if (!mode || !dev || used < 2) return;
    // (the drawing thread's alone -- one thread draws at a time: plain statics, not thread_locals, which are the Vita's
    // emulated TLS -- a call chain per access)
    static std::vector<uint8_t> cls;
    static std::vector<uint64_t> surfaces;
    static std::vector<uint32_t> opaque, order;
    static std::vector<Item> punch;
    static std::vector<Draw*> moved;
    cls.resize(used);
    for (size_t i = 0; i < used; ++i) {
        if (i + 3 < used) PrefetchDraw(draws[i + 3]);   // (the records' first touch on this core)
        cls[i] = uint8_t(ZClass(*draws[i]));
    }
    g_zc.draws += used;
    size_t i = 0;
    while (i < used) {
        if (cls[i] == 0) {
            g_zc.barriers++;
            ++i;
            continue;
        }
        // the run [i, j): cut before a draw over the surface of a punch-through draw earlier in it (a material's later pass:
        // the same depths everywhere, so the order of the two must stay -- the opaque draws keep theirs, the punch-through
        // ones are then each over a surface of their own and may go in any order)
        surfaces.clear();
        opaque.clear();
        punch.clear();
        g_inst.Reset();
        bool hoist = false;
        size_t j = i;
        while (j < used && cls[j] != 0) {
            // (a material's passes over a part are recorded one after the other: the last 8 punch-through surfaces of the
            // run are the ones a pass can repeat)
            const uint64_t k = punch.empty() && cls[j] != 2 ? 0 : SurfaceKey(*draws[j]);
            if (!punch.empty()) {
                const size_t from = surfaces.size() > 8 ? surfaces.size() - 8 : 0;
                if (std::find(surfaces.begin() + std::ptrdiff_t(from), surfaces.end(), k) != surfaces.end()) {
                    g_zc.cuts++;
                    break;
                }
            }
            const uint64_t inst = cls[j] == 2 || !punch.empty() ? InstanceKey(*draws[j]) : 0;   // (read only then: below)
            if (cls[j] == 2) {
                bool known = true;
                const float key = NearZ(dev, *draws[j], &known);
                // (more punch-through instances in the run than the set holds: the run ends here -- the safe side)
                if (inst && !g_inst.Add(inst, key)) {
                    g_zc.cuts_instance++;
                    break;
                }
                surfaces.push_back(k);
                if (!known) g_zc.unknown_keys++;
                punch.push_back(Item{uint32_t(j - i), key, inst});
            } else {
                // an opaque part of the instance of a punch-through draw before it (layered with it: InstanceKey): never
                // moved before that draw -- the run ends here
                if (inst && !punch.empty() && g_inst.Has(inst)) {
                    g_zc.cuts_instance++;
                    break;
                }
                if (!punch.empty()) hoist = true;
                opaque.push_back(uint32_t(j - i));
            }
            ++j;
        }
        g_zc.runs++;
        g_zc.opaque += opaque.size();
        g_zc.punch += punch.size();
        const size_t n = j - i;
        if (!punch.empty() && (hoist || (mode >= 2 && punch.size() >= 2))) {
            // the new order: the opaque draws as they came, then the punch-through ones (mode 2: each by its key, near to
            // far; mode 3: runs of consecutive same-state draws, each run by its nearest member, its order kept)
            if (mode == 2) {
                // (one key per instance: its punch-through draws keep their relative order -- InstanceKey)
                for (Item& p : punch)
                    if (p.inst) p.key = g_inst.Near(p.inst);
                std::stable_sort(punch.begin(), punch.end(), [](const Item& a, const Item& b) { return a.key > b.key; });
            } else if (mode == 3) {
                // the runs' keys: the largest of their members'
                size_t a = 0;
                while (a < punch.size()) {
                    size_t b = a + 1;
                    while (b < punch.size() && punch[b].index == punch[b - 1].index + 1 &&
                           std::memcmp(&draws[i + punch[b].index]->state, &draws[i + punch[a].index]->state, sizeof(ps2e_draw_state)) == 0)
                        ++b;
                    float key = punch[a].key;
                    for (size_t k = a + 1; k < b; ++k) key = std::max(key, punch[k].key);
                    for (size_t k = a; k < b; ++k) punch[k].key = key;   // a run's members share its key (stable: kept together)
                    a = b;
                }
                // (then one key per instance, the largest of its members': its draws keep their relative order)
                for (const Item& p : punch)
                    if (p.inst) g_inst.Add(p.inst, p.key);
                for (Item& p : punch)
                    if (p.inst) p.key = g_inst.Near(p.inst);
                std::stable_sort(punch.begin(), punch.end(), [](const Item& x, const Item& y) { return x.key > y.key; });
            }
            order.clear();
            order.insert(order.end(), opaque.begin(), opaque.end());
            for (const Item& p : punch) order.push_back(p.index);
            bool changed = false;
            for (size_t k = 0; k < n && !changed; ++k) changed = order[k] != uint32_t(k);
            if (changed) {
                g_zc.runs_changed++;
                size_t first_punch = n;
                for (const Item& p : punch) first_punch = std::min<size_t>(first_punch, p.index);
                for (uint32_t o : opaque)
                    if (o > first_punch) g_zc.hoisted++;   // an opaque draw now before a punch-through draw it followed
                for (size_t k = opaque.size(); k < n; ++k)
                    if (order[k] != uint32_t(k)) g_zc.punch_moved++;
                // the new order of the run's pointers (the records stay where they are): the punch-through ones out in their
                // new order, the opaque ones after the first punch-through draw moved up (the ones before it stay), the
                // punch-through ones back
                moved.clear();
                for (const Item& p : punch) moved.push_back(draws[i + p.index]);
                size_t dst = first_punch;
                for (uint32_t o : opaque) {
                    if (o < first_punch) continue;
                    if (dst != o) draws[i + dst] = draws[i + o];
                    ++dst;
                }
                for (Draw* p : moved) draws[i + dst++] = p;
                moved.clear();
            }
        }
        i = j;
    }
    g_zc.frames++;
    g_zc.frames_on++;
}

std::string ZOrderCensusLine(double frames) {
    if (!g_zo.read) return std::string();
    static ZOrderCensus last;
    const ZOrderCensus now = g_zc;
    const double f = std::max(1.0, frames);
    auto per = [&](uint64_t a, uint64_t b) { return double(a - b) / f; };
    char b[900];
    std::snprintf(b, sizeof b, "[te.gfx.vita] draw order (TE_GFX_VITA_ZORDER %s%d): %llu of the window's frames reordered; per frame "
                  "%.1f draws: %.1f outside the runs, runs %.1f (%.1f changed, %.1f cut before a pass over a punch-through "
                  "surface, %.1f before a part of a punch-through draw's instance), in them %.1f opaque (%.1f now before "
                  "punch-through draws they followed) + %.1f punch-through "
                  "(%.1f moved; keys from a stand-in %.1f); alpha-test fold: %.1f of %.1f draws with a GEQUAL / GREATER test drawn "
                  "without it (their floor known for %.1f)\n",
                  g_zo.ab ? "ab" : "", g_zo.ab ? g_zo.ab : g_zo.mode, (unsigned long long)(now.frames_on - last.frames_on),
                  per(now.draws, last.draws), per(now.barriers, last.barriers), per(now.runs, last.runs),
                  per(now.runs_changed, last.runs_changed), per(now.cuts, last.cuts), per(now.cuts_instance, last.cuts_instance),
                  per(now.opaque, last.opaque),
                  per(now.hoisted, last.hoisted), per(now.punch, last.punch), per(now.punch_moved, last.punch_moved),
                  per(now.unknown_keys, last.unknown_keys), per(now.folded, last.folded), per(now.tested, last.tested),
                  per(now.tested - now.unknown_floor, last.tested - last.unknown_floor));
    last = now;
    return b;
}

#else

void ZOrderDraws(ps2e_draw*, Draw**, size_t, uint64_t) {}
std::string ZOrderCensusLine(double) { return std::string(); }

#endif

}  // namespace te::gfx
