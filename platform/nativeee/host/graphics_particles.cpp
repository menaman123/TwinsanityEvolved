// platform/nativeee (host side) -- graphics.h's particle entries: the particle blocks and render tables are
// written into guest memory exactly as the PS2 renderer wrote them (particles.cpp InitParticleBlock / InitParticleRenderTable /
// WriteParticleRenderTable / ChainParticleBlocks / EndParticleBlock -- the game reads nothing back, but the layout is the
// program's input and a save state carries it for free), and DrawParticleBlock runs the VU1 library's own particle program
// bodies (vs_zero_kick class C: the dispatcher stub at entry 0, then the 0x1820 quad emitter -- ZeroKick::RunC / RunCCont)
// over a host VU data image laid out as the retail VIF stream left it: the matrix at 0x4F, the table's head at 0x53, its
// corners and steps at 0x55, the block's GIF tag and 32 particle records at TOP, the node list with the four packets'
// addresses. The kicked GIF packets (8 particles each: UV, RGBAQ, XYZ2 then UV, XYZ2 for the other three corners; ADC on the
// first two) are assembled as the GS assembles a strip and recorded into the material's bucket. The distorting hexagons
// (DrawDistortionBlock, the frame copied as their texture) are not drawn yet.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/zero_kick.h"
#include "graphics_internal.h"
#include "te_display.h"
#include "te_prof.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_host.h"

extern "C" uint64_t te_graphics_presents();   // host/graphics.cpp: frames presented so far

namespace {
using te::gfx::Draw;
using te::gfx::MaterialView;

// ---- the PS2 renderer's constants (particles.cpp) ------------------------------------------------------------------------------
constexpr uint32_t kVuMatrixAddress = 0x4F, kVuHeadAddress = 0x53, kVuStepsAddress = 0x55;
constexpr uint32_t kParticleBlockBytes = 0x430, kHexagonBlockBytes = 0x1B0, kRenderTableBytes = 0x890;
constexpr uint32_t kRenderSteps = 64;
constexpr uint32_t kParticlesPerBlock = 32;
constexpr float kParticleNearClip = 0.5f;
// DMA tag ids (bits 28..30): CNT 1, NEXT 2, RET 6; the VIF codes
constexpr uint32_t kTagId = 0x70000000, kCountTag = 0x10000000, kNextTag = 0x20000000, kReturnTag = 0x60000000;
constexpr uint32_t kVifFlush = 0x11000000, kVifFlushE = 0x10000000, kVifMscnt = 0x17000000;
constexpr uint32_t kVifUnpackV4Count = 0x6C000000, kVifTops = 0x8000;
constexpr uint32_t VifUnpackTo(uint32_t code, uint32_t address, uint32_t count) { return code | (count << 16) | address; }
// GIF: PACKED register descriptors
constexpr uint32_t kGifRgbaq = 1, kGifSt = 2, kGifUv = 3, kGifXyz2 = 5;
constexpr uint32_t kGsPrimTriStrip = 4;
// The VU program's node list and double buffer in the host image (the retail addresses are the material writer's; the
// program reads them from the image, so any non-overlapping places serve): the node at 0x20, the block at 0x100, the
// four output packets (1 + 8 * 9 quadwords each) from 0x180
constexpr uint32_t kNode = 0x20, kTop = 0x100, kPacketBase = 0x180, kPacketStride = 0x50;
constexpr uint32_t kEmitterPc = 0x1720, kClassCTpc = 0x1820;
constexpr uint32_t kQwCount = 1024;
// The game's widescreen option (renderer.h g_WidescreenTv, D_0030AAC4) and 16:9 as the game has it (WideAspect)
constexpr uint32_t kWidescreenTv = 0x0030AAC4;
constexpr float kWideAspect = 0x1.C71C72p+0f;

// ParticleLook as the game lays it out (include/platform/graphics.h): gravity, texture[4], hexagons (bool, padded), the 12
// hexagon corners, 64 steps of {shape[6], colour[4]}
struct GuestLook {
    float gravity;
    int32_t texture[4];
    uint8_t hexagons, pad[3];
    float hexagonCorners[12];
    struct Step { float shape[6]; uint8_t colour[4]; } steps[kRenderSteps];
};
static_assert(sizeof(GuestLook) == 0x18 + 0x30 + 64 * 0x1C, "ParticleLook layout");
// ParticleRenderTable (renderer.h ParticleHeader + the corners, steps and the end quadword)
struct GuestTable {
    uint32_t tag[4];
    float nearClip, gravity, unused18, time;
    float distortion[4];
    uint32_t stepsTag[4];
    int32_t corners[12];
    struct Step { float shape[6]; uint32_t unused18[2]; } steps[kRenderSteps];
    uint32_t end[4];
};
static_assert(offsetof(GuestTable, steps) == 0x70 && offsetof(GuestTable, end) == 0x870, "ParticleRenderTable layout");
static_assert(sizeof(GuestTable) <= kRenderTableBytes, "ParticleRenderTable fits");
// A deferred particle build's input (DeferredBuild::input): the table, the matrix (16 floats), then the chain's blocks
constexpr size_t kInputHead = sizeof(GuestTable) + 64;

template <typename T>
T* GuestMutable(uint32_t address) { return reinterpret_cast<T*>(te::GuestPointer(address, sizeof(T))); }
template <typename T>
const T* Guest(uint32_t address) { return reinterpret_cast<const T*>(te::GuestPointer(address, sizeof(T))); }

void Once(const char* what) {
    static std::vector<std::string> said;
    for (const auto& s : said) if (s == what) return;
    said.emplace_back(what);
    std::fprintf(stderr, "[te.gfx] %s\n", what);
}

uint64_t GifDescriptors(std::initializer_list<uint32_t> regs) {
    uint64_t value = 0;
    unsigned shift = 0;
    for (uint32_t r : regs) { value |= uint64_t(r & 0xF) << shift; shift += 4; }
    return value;
}

}  // namespace

// ---- the kicked packets -> triangles ------------------------------------------------------------------------------------------
namespace {
struct Vertex { float x, y, z, u, v; uint8_t r, g, b, a; bool kick; };

// The last three vertices of a strip, oldest first (a fixed queue: no allocation per packet)
struct Queue {
    Vertex v[3];
    unsigned size = 0;
    void Push(const Vertex& x) {
        if (size < 3) { v[size++] = x; return; }
        v[0] = v[1];
        v[1] = v[2];
        v[2] = x;
    }
};
// The GS strip assembly (VertexKick::AssembleStrip): a vertex without ADC draws the triangle of the last three
void Assemble(Queue& queue, unsigned& parity, const Vertex& v, std::vector<ps2e_draw_vertex>& out, float tw, float th, bool textured) {
    queue.Push(v);
    if (!v.kick) { parity = 0; return; }
    if (queue.size != 3) return;
    const Vertex* q[3] = {&queue.v[0], &queue.v[1], &queue.v[2]};
    if (parity % 2) std::swap(q[0], q[1]);
    const bool degenerate = (q[0]->x == q[1]->x && q[0]->y == q[1]->y) || (q[1]->x == q[2]->x && q[1]->y == q[2]->y) ||
                            (q[0]->x == q[2]->x && q[0]->y == q[2]->y);
    if (!degenerate) {
        for (const Vertex* s : q) {
            ps2e_draw_vertex o{};
            o.x = s->x; o.y = s->y; o.z = s->z;
            if (textured) { o.u = s->u / tw; o.v = s->v / th; }
            o.r = s->r; o.g = s->g; o.b = s->b; o.a = s->a;
            out.push_back(o);
        }
    }
    ++parity;
}

}  // namespace

// One GIF packet (PACKED, the block's descriptors) into triangle-list vertices in frame pixels (shared with the decals)
bool te::gfx::LowerPackedPacket(const std::vector<uint8_t>& packet, float offX, float offY, float tw, float th, bool textured, std::vector<ps2e_draw_vertex>& out) {
    if (packet.size() < 16) return false;
    auto word = [&](size_t qw, unsigned lane) { uint32_t v; std::memcpy(&v, packet.data() + qw * 16 + lane * 4, 4); return v; };
    const uint32_t w0 = word(0, 0), w1 = word(0, 1);
    const uint32_t nloop = w0 & 0x7FFF;
    const uint32_t flg = (w1 >> 26) & 3;
    uint32_t nregs = (w1 >> 28) & 0xF;
    if (!nregs) nregs = 16;
    if (flg != 0) return false;
    const uint64_t regs = uint64_t(word(0, 2)) | uint64_t(word(0, 3)) << 32;
    if (packet.size() < 16 + size_t(nloop) * nregs * 16) return false;
    Queue queue;
    unsigned parity = 0;
    Vertex v{};
    size_t qw = 1;
    for (uint32_t loop = 0; loop < nloop; ++loop) {
        for (uint32_t r = 0; r < nregs; ++r, ++qw) {
            switch ((regs >> (4 * r)) & 0xF) {
                case kGifRgbaq:
                    v.r = uint8_t(word(qw, 0)); v.g = uint8_t(word(qw, 1)); v.b = uint8_t(word(qw, 2)); v.a = uint8_t(word(qw, 3));
                    break;
                case kGifUv:
                    v.u = float(word(qw, 0) & 0x3FFF) / 16.f;
                    v.v = float(word(qw, 1) & 0x3FFF) / 16.f;
                    break;
                case kGifSt: {
                    float s, t, q;
                    std::memcpy(&s, &packet[qw * 16], 4); std::memcpy(&t, &packet[qw * 16 + 4], 4); std::memcpy(&q, &packet[qw * 16 + 8], 4);
                    v.u = q != 0.f ? s / q * tw : 0.f;
                    v.v = q != 0.f ? t / q * th : 0.f;
                    break;
                }
                case kGifXyz2: {
                    v.x = float(word(qw, 0) & 0xFFFF) / 16.f - offX;
                    v.y = float(word(qw, 1) & 0xFFFF) / 16.f - offY;
                    v.z = float(word(qw, 2));
                    v.kick = ((word(qw, 3) >> 15) & 1) == 0;
                    Assemble(queue, parity, v, out, tw, th, textured);
                    break;
                }
                default: break;
            }
        }
    }
    return true;
}

namespace {
// ---- the VU image for one block --------------------------------------------------------------------------------------------
struct Image {
    std::vector<uint8_t> memory;
    Image() : memory(kQwCount * 16) {}
    void Set(uint32_t qw, unsigned lane, uint32_t value) { std::memcpy(&memory[qw * 16 + lane * 4], &value, 4); }
    void SetQw(uint32_t qw, const void* bytes16) { std::memcpy(&memory[qw * 16], bytes16, 16); }
};

// TE_AB=particlefull (graphics.cpp AbToggle): the previous path for an in-run A/B -- the whole image zeroed per block
// and the full RunCCont (its working copy and MAC / STATUS chain); the same kicks
std::atomic<bool> g_particleFull{false};
// TE_GFX_PARTICLE_UNCLAMPED: the emitter's form. 1 (default): ZeroKick::RunCContKicksUnclamped -- the arithmetic without
// the FMAC clamps when every float the block's program reads is finite (the clamped form otherwise; the same kicks);
// 0: the clamped form for every block; 2: both forms for every block the unclamped one takes, their kicks compared
// (a line for each of the first four blocks that differ and one every 1024 blocks compared)
// (read at the first block: the Vita's args.txt sets its TE_* variables after the static initialisers ran)
int UnclampedMode() {
    static const int mode = [] {
        const char* v = std::getenv("TE_GFX_PARTICLE_UNCLAMPED");
        return v ? std::atoi(v) : 1;
    }();
    return mode;
}
std::atomic<uint64_t> g_blocksUnclamped{0}, g_blocksClamped{0}, g_checked{0}, g_checkDiffered{0}, g_checkTrianglesDiffered{0};
void CheckUnclamped(const Vu1HleInput& in, uint8_t* memory, const Vu1HleOutput& unclamped) {
    Vu1HleOutput clamped;
    const bool ok = ZeroKick::RunCContKicks(in, memory, clamped);
    const bool same = ok && clamped.kicks == unclamped.kicks;
    const uint64_t n = g_checked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!same) {
        // what the kicks draw: the triangles the GS assembly keeps (the frame's offset and the texture's size left at 0 / 1)
        std::vector<ps2e_draw_vertex> a, b;
        for (const auto& packet : clamped.kicks) te::gfx::LowerPackedPacket(packet, 0.f, 0.f, 1.f, 1.f, true, a);
        for (const auto& packet : unclamped.kicks) te::gfx::LowerPackedPacket(packet, 0.f, 0.f, 1.f, 1.f, true, b);
        if (a.size() != b.size() || (!a.empty() && std::memcmp(a.data(), b.data(), a.size() * sizeof a[0]) != 0))
            g_checkTrianglesDiffered.fetch_add(1, std::memory_order_relaxed);
    }
    if (!same && g_checkDiffered.fetch_add(1, std::memory_order_relaxed) < 4) {
        size_t k = 0, at = 0;
        while (k < clamped.kicks.size() && k < unclamped.kicks.size() && clamped.kicks[k] == unclamped.kicks[k]) ++k;
        if (k < clamped.kicks.size() && k < unclamped.kicks.size())
            while (at < clamped.kicks[k].size() && at < unclamped.kicks[k].size() && clamped.kicks[k][at] == unclamped.kicks[k][at]) ++at;
        uint32_t a = 0, b = 0;
        const size_t word = at & ~size_t(3);
        if (k < clamped.kicks.size() && word + 4 <= clamped.kicks[k].size()) std::memcpy(&a, &clamped.kicks[k][word], 4);
        if (k < unclamped.kicks.size() && word + 4 <= unclamped.kicks[k].size()) std::memcpy(&b, &unclamped.kicks[k][word], 4);
        std::fprintf(stderr, "[te.gfx] particle emitter check: block %llu differs (clamped form %s, %u kicks; unclamped %u kicks; "
                     "first difference in kick %u at byte %u: %08x clamped, %08x unclamped)\n", (unsigned long long)n,
                     ok ? "ran" : "declined", unsigned(clamped.kicks.size()), unsigned(unclamped.kicks.size()), unsigned(k),
                     unsigned(at), unsigned(a), unsigned(b));
    }
    if (n % 1024 == 0)
        std::fprintf(stderr, "[te.gfx] particle emitter check: %llu blocks run both ways, %llu differing (%llu in the triangles they "
                     "draw)\n", (unsigned long long)n, (unsigned long long)g_checkDiffered.load(std::memory_order_relaxed),
                     (unsigned long long)g_checkTrianglesDiffered.load(std::memory_order_relaxed));
}
// ---- the triangles that draw nothing, and the particles' fill census --------------------------------------------------------
// The largest vertex alpha (PS2 scale) at which every fragment of a triangle fails the draw's alpha test and is discarded
// whole -- AFAIL KEEP writes neither colour nor alpha nor depth -- or -1 when the state gives no such bound. The fragment's
// alpha (gl_shader_sources.cpp, the PS2 program) is at most its vertex alpha Av: MODULATE with the texture's alpha is
// trunc(Av * At / 128) with At at most 128 (the decoded texel's alpha back on the PS2 scale), MODULATE / DECAL / HIGHLIGHT
// without it (TCC 0) and an untextured draw give Av itself; the vertex colour reaches the program as a normalised byte, and
// between three vertices its interpolated value rounds to at most their largest. So a triangle whose three vertices' alpha
// are at most the bound draws nothing (the frame copy as the texture is left alone: its texels are the frame's own).
int DiscardedAlphaBound(const ps2e_draw_state& s) {
    if (!s.alpha_test || s.alpha_fail != 0 || s.texture == PS2E_DRAW_TEXTURE_FRAME_COPY) return -1;
    if (s.texture && s.tcc && s.tfx != PS2E_DRAW_TFX_MODULATE) return -1;   // (the texel's own alpha, or added to Av)
    switch (s.alpha_method) {
        case PS2E_DRAW_ATST_NEVER: return 255;
        case PS2E_DRAW_ATST_GREATER: return s.alpha_reference;            // passes only above AREF
        case PS2E_DRAW_ATST_GEQUAL:
        case PS2E_DRAW_ATST_EQUAL: return int(s.alpha_reference) - 1;     // passes only at AREF or above it
        default: return -1;
    }
}

// TE_GFX_PARTICLE_CULL: the particle triangles that draw nothing (DiscardedAlphaBound) left out of the draw lists -- the same
// picture, none of their fragments shaded. 1 (default) on; 0 off; abN on and off every N presents (an in-run A/B of the GPU's
// time; read at the first block, as TE_GFX_PARTICLE_UNCLAMPED is)
bool CullOn() {
    static const long mode = [] {
        const char* v = std::getenv("TE_GFX_PARTICLE_CULL");
        if (!v || !*v) return 1L;
        if (std::strncmp(v, "ab", 2) == 0) return -std::max(1L, v[2] ? std::atol(v + 2) : 600L);
        return v[0] == '0' ? 0L : 1L;
    }();
    if (mode >= 0) return mode == 1;
    const bool on = (te_graphics_presents() / uint64_t(-mode)) % 2 == 0;
    static std::atomic<int> last{-1};
    if (last.exchange(on ? 1 : 0, std::memory_order_relaxed) != (on ? 1 : 0))
        std::fprintf(stderr, "[te.gfx] TE_GFX_PARTICLE_CULL A/B: present %llu: %s\n", (unsigned long long)te_graphics_presents(),
                     on ? "on" : "off");
    return on;
}

// TE_GFX_PARTICLE_CENSUS=1: the particles' fill per present in the census window (te::gfx::ParticleFillLine) -- the blocks,
// the triangles drawn and their area clipped to the frame (the fragments the GPU makes for them, overdraw included), the
// triangles left out and the area they had, the share the title-resolution pass can take (TE_GFX_PARTICLE_SCALE: additive
// draws). The areas are summed on the build threads in 1/16 pixel (one atomic add a block)
bool CensusOn() {
    static const bool on = [] { const char* v = std::getenv("TE_GFX_PARTICLE_CENSUS"); return v && v[0] == '1'; }();
    return on;
}
std::atomic<uint64_t> g_fillBlocks{0}, g_fillTriangles{0}, g_fillArea{0}, g_cutTriangles{0}, g_cutArea{0}, g_cutBlocks{0},
    g_additiveArea{0};

// a triangle's area inside the frame (0..w x 0..h; frame pixels): the polygon clipped against the four edges
double ClippedArea(const ps2e_draw_vertex* t, float w, float h) {
    float px[9], py[9], qx[9], qy[9];
    int n = 3;
    for (int i = 0; i < 3; ++i) { px[i] = t[i].x; py[i] = t[i].y; }
    for (int edge = 0; edge < 4 && n; ++edge) {
        int m = 0;
        auto inside = [&](float x, float y) { return edge == 0 ? x >= 0.f : edge == 1 ? x <= w : edge == 2 ? y >= 0.f : y <= h; };
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const bool a = inside(px[i], py[i]), b = inside(px[j], py[j]);
            if (a) { qx[m] = px[i]; qy[m] = py[i]; ++m; }
            if (a != b) {
                float s;
                if (edge < 2) {
                    const float e = edge == 0 ? 0.f : w;
                    s = (e - px[i]) / (px[j] - px[i]);
                    qx[m] = e;
                    qy[m] = py[i] + s * (py[j] - py[i]);
                } else {
                    const float e = edge == 2 ? 0.f : h;
                    s = (e - py[i]) / (py[j] - py[i]);
                    qx[m] = px[i] + s * (px[j] - px[i]);
                    qy[m] = e;
                }
                ++m;
            }
        }
        n = m;
        std::memcpy(px, qx, sizeof(float) * size_t(n));
        std::memcpy(py, qy, sizeof(float) * size_t(n));
    }
    double a = 0;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        a += double(px[i]) * py[j] - double(px[j]) * py[i];
    }
    return std::fabs(a) * 0.5;
}

bool AdditiveParticles(const ps2e_draw_state& s) { return te::gfx::AdditiveParticleState(s); }

// A block's lowered triangles: those that draw nothing (bound >= 0) taken out in place, the census's sums noted
void FilterBlock(std::vector<ps2e_draw_vertex>& verts, int bound, bool additive, float w, float h) {
    const bool census = CensusOn();
    if (bound < 0 && !census) return;
    const size_t n = verts.size() / 3 * 3;
    size_t kept = 0, cut = 0;
    double area = 0, cutArea = 0;
    for (size_t i = 0; i < n; i += 3) {
        const ps2e_draw_vertex* t = &verts[i];
        const bool nothing = bound >= 0 && t[0].a <= bound && t[1].a <= bound && t[2].a <= bound;
        if (census) (nothing ? cutArea : area) += ClippedArea(t, w, h);
        if (nothing) {
            ++cut;
            continue;
        }
        if (kept != i) std::memmove(&verts[kept], t, 3 * sizeof(ps2e_draw_vertex));
        kept += 3;
    }
    verts.resize(kept);
    if (cut) g_cutTriangles.fetch_add(cut, std::memory_order_relaxed);
    if (cut && !kept) g_cutBlocks.fetch_add(1, std::memory_order_relaxed);
    if (!census) return;
    g_fillBlocks.fetch_add(1, std::memory_order_relaxed);
    g_fillTriangles.fetch_add(kept / 3, std::memory_order_relaxed);
    g_fillArea.fetch_add(uint64_t(area * 16.0 + 0.5), std::memory_order_relaxed);
    g_cutArea.fetch_add(uint64_t(cutArea * 16.0 + 0.5), std::memory_order_relaxed);
    if (additive) g_additiveArea.fetch_add(uint64_t(area * 16.0 + 0.5), std::memory_order_relaxed);
}

// The particles' widths on a picture drawn for another wide shape (te_display.h WidescreenAspect, with the game's widescreen
// option on): the table's quads are as wide in the frame as the game made them for 16:9 -- its projection's squeeze is not in
// them -- so drawn for 32:9 they would show twice as wide as on a 16:9 TV. 16:9's share of the shape's width keeps a 16:9
// TV's; 1 (the table as it is) at auto, 4:3, 16:9 and with the option off.
float ParticleWidths() {
    const float shape = te::display::WidescreenAspect();
    if (!(shape > 0.f)) return 1.f;
    const uint8_t* on = te::GuestPointer(kWidescreenTv, 1);
    return on && *on ? kWideAspect / shape : 1.f;
}

bool RunBlock(const uint8_t* block, const GuestTable& table, const float (*matrix)[4], float time, float widths,
              std::vector<std::vector<uint8_t>>& kicks) {
    static const unsigned short profPrep = te::prof::Register("particle: block image"), profStub = te::prof::Register("particle: VU stub (RunC)"),
                                profEmit = te::prof::Register("particle: VU emitter (RunCCont)");
    te::prof::Scope scope(profPrep);
    // One image per thread, all zero when made. A block's run writes only the stub's QW9 and the four output packets (the
    // emitter's stores are its packets' quadwords: the node list below fixes them); everything else written here is
    // rewritten whole, at the same places, for every block. Zeroing those two again leaves the image a fresh one would be
    // (the 16 KB fill per block was the larger part of the set-up).
    thread_local Image img;
    const bool full = g_particleFull.load(std::memory_order_relaxed);
    if (full) {
        std::fill(img.memory.begin(), img.memory.end(), uint8_t(0));
    } else {
        std::memset(&img.memory[9 * 16], 0, 16);
        std::memset(&img.memory[size_t(kPacketBase) * 16], 0, size_t(4 * kPacketStride) * 16);
    }
    for (uint32_t r = 0; r < 4; ++r) img.SetQw(kVuMatrixAddress + r, matrix[r]);
    const float head[4] = {kParticleNearClip, table.gravity, table.unused18, time};
    img.SetQw(kVuHeadAddress, head);
    img.SetQw(kVuHeadAddress + 1, table.distortion);
    for (uint32_t q = 0; q < 3; ++q) img.SetQw(kVuStepsAddress + q, &table.corners[q * 4]);
    if (widths == 1.f) {
        for (uint32_t s = 0; s < kRenderSteps; ++s) {
            img.SetQw(kVuStepsAddress + 3 + 2 * s, table.steps[s].shape);
            img.SetQw(kVuStepsAddress + 4 + 2 * s, &table.steps[s].shape[4]);
        }
    } else {
        // a step's shape: the corner (x, y), the edge to the next corner (x, y), the opposite corner (x, y): the across terms
        // scaled, each word of the first four keeping its low byte (the step's colour, packed there by the table's writer)
        const auto across = [widths](float& v, bool colour) {
            uint32_t was, now;
            std::memcpy(&was, &v, 4);
            const float scaled = v * widths;
            std::memcpy(&now, &scaled, 4);
            if (colour) now = (now & ~0xFFu) | (was & 0xFFu);
            std::memcpy(&v, &now, 4);
        };
        for (uint32_t s = 0; s < kRenderSteps; ++s) {
            float step[8];
            std::memcpy(step, &table.steps[s], sizeof step);
            across(step[0], true);
            across(step[2], true);
            across(step[4], false);
            img.SetQw(kVuStepsAddress + 3 + 2 * s, step);
            img.SetQw(kVuStepsAddress + 4 + 2 * s, &step[4]);
        }
    }
    // the block: its GIF tag then the 32 particle records (the VIF unpack of quadwords - 1 to TOPS)
    const uint32_t quadwords = kParticleBlockBytes / 16 - 1;   // the RET tag's count: GIF tag + particles
    for (uint32_t q = 0; q < quadwords - 1; ++q) img.SetQw(kTop + q, block + 16 + q * 16);
    // the dispatcher's node list: QW3.x = the node; the node's call (the emitter), then the four packets' addresses the
    // program walks from node + 4 down to node + 1
    img.Set(3, 0, kNode);
    img.Set(kNode, 0, kEmitterPc / 8);
    for (uint32_t p = 0; p < 4; ++p) img.Set(kNode + 1 + p, 0, kPacketBase + p * kPacketStride);
    VUregs regs{};
    regs.VF[0].I[3] = 0x3f800000u;   // vf00 = (0, 0, 0, 1)
    Vu1HleInput in{};
    in.micro = nullptr;
    in.data = img.memory.data();
    in.start_pc = 0;
    in.top = in.tops = kTop;
    in.itop = in.itops = 0;
    in.regs = &regs;
    Vu1HleOutput stub;
    {
        te::prof::Scope stubScope(profStub);
        if (!ZeroKick::RunC(in, stub) || !stub.has_regs || stub.regs.TPC != kClassCTpc) return false;
    }
    for (const auto& w : stub.data_writes)
        if (w.addr + w.bytes.size() <= img.memory.size()) std::memcpy(&img.memory[w.addr], w.bytes.data(), w.bytes.size());
    VUregs entry = stub.regs;
    in.regs = &entry;
    in.start_pc = kClassCTpc;
    in.data_writes = false;   // only the kicks are read (the image is rebuilt for the next block)
    Vu1HleOutput run;
    run.kicks.reserve(4);   // (a block's four packets)
    {
        te::prof::Scope emitScope(profEmit);
        // the kicks alone, the program over this image in place (RunCContKicks' packets without its working copy and flags)
        if (full) {
            if (!ZeroKick::RunCCont(in, run)) return false;
        } else if (UnclampedMode() == 0) {
            if (!ZeroKick::RunCContKicks(in, img.memory.data(), run)) return false;
        } else {
            // (the check: the particles as the block holds them, before the unclamped form clamps them in place)
            uint8_t pairs[66 * 16];
            if (UnclampedMode() == 2) std::memcpy(pairs, &img.memory[size_t(kTop + 1) * 16], sizeof pairs);
            bool unclamped = false;
            if (!ZeroKick::RunCContKicksUnclamped(in, img.memory.data(), run, &unclamped)) return false;
            (unclamped ? g_blocksUnclamped : g_blocksClamped).fetch_add(1, std::memory_order_relaxed);
            if (UnclampedMode() == 2 && unclamped) {
                std::memcpy(&img.memory[size_t(kTop + 1) * 16], pairs, sizeof pairs);
                CheckUnclamped(in, img.memory.data(), run);
            }
        }
    }
    kicks = std::move(run.kicks);
    return true;
}
}  // namespace

namespace te::gfx {
bool AdditiveParticleState(const ps2e_draw_state& s) {
    return s.blend && s.blend_a == 0 && s.blend_b == 2 && (s.blend_c == 0 || s.blend_c == 2) && s.blend_d == 1 && !s.depth_write &&
           s.stencil == PS2E_DRAW_STENCIL_OFF && !s.no_colour_write && s.texture != PS2E_DRAW_TEXTURE_FRAME_COPY;
}
void SetParticleFull(bool on) { g_particleFull.store(on, std::memory_order_relaxed); }
std::string ParticleCensusLine(double frames) {
    if (frames <= 0) return std::string();
    const double u = double(g_blocksUnclamped.exchange(0, std::memory_order_relaxed)) / frames;
    const double c = double(g_blocksClamped.exchange(0, std::memory_order_relaxed)) / frames;
    char b[256];
    std::snprintf(b, sizeof b, "[te.gfx.vita] particle builds per present: blocks %.1f by the emitter's form: unclamped %.1f, "
                  "clamped %.1f (TE_GFX_PARTICLE_UNCLAMPED %d)\n", u + c, u, c, UnclampedMode());
    return b;
}
std::string ParticleFillLine(double frames, int frame_w, int frame_h, int scale) {
    const uint64_t cut = g_cutTriangles.exchange(0, std::memory_order_relaxed), cutBlocks = g_cutBlocks.exchange(0, std::memory_order_relaxed);
    const uint64_t blocks = g_fillBlocks.exchange(0, std::memory_order_relaxed), tris = g_fillTriangles.exchange(0, std::memory_order_relaxed);
    const double area = double(g_fillArea.exchange(0, std::memory_order_relaxed)) / 16.0;
    const double cutArea = double(g_cutArea.exchange(0, std::memory_order_relaxed)) / 16.0;
    const double additive = double(g_additiveArea.exchange(0, std::memory_order_relaxed)) / 16.0;
    if (frames <= 0 || (!blocks && !cut)) return std::string();
    const double frame = double(std::max(1, frame_w)) * double(std::max(1, frame_h)), s2 = double(scale) * double(scale);
    char b[512];
    if (!CensusOn()) {
        std::snprintf(b, sizeof b, "[te.gfx] particles per present: %.1f triangles left out that draw nothing (TE_GFX_PARTICLE_CULL %s), "
                      "%.1f blocks with none left\n", double(cut) / frames, CullOn() ? "on" : "off", double(cutBlocks) / frames);
        return b;
    }
    std::snprintf(b, sizeof b, "[te.gfx] particles per present (TE_GFX_PARTICLE_CENSUS): blocks %.1f, triangles drawn %.1f covering %.2f "
                  "frames (%.2f Mpx at %dx; additive %.2f frames), left out that draw nothing %.1f triangles covering %.2f frames "
                  "(%.2f Mpx; %.1f blocks with none left; TE_GFX_PARTICLE_CULL %s)\n", double(blocks) / frames, double(tris) / frames,
                  area / frame / frames, area * s2 / 1e6 / frames, scale, additive / frame / frames, double(cut) / frames,
                  cutArea / frame / frames, cutArea * s2 / 1e6 / frames, double(cutBlocks) / frames, CullOn() ? "on" : "off");
    return b;
}
}

extern "C" {

void ps2e_graphics_init_particle_graphics(void) {
    // The effects' disk node, the wave shader (type 0x1C) and the distortion material are the game's / wasm side's
}

void ps2e_graphics_init_particle_block(ps2e_u32 blockAddress, ps2e_s32 hexagons) {
    const uint32_t bytes = hexagons ? kHexagonBlockBytes : kParticleBlockBytes;
    uint8_t* block = te::GuestPointer(blockAddress, bytes);
    if (!block) return;
    const uint32_t quadwords = bytes / 16 - 1;
    auto* words = reinterpret_cast<uint32_t*>(block);
    auto* last = reinterpret_cast<uint32_t*>(block + quadwords * 16);
    for (int k = 0; k < 4; ++k) last[k] = 0;
    words[0] = kReturnTag | quadwords;
    words[1] = 0;
    words[2] = 0;
    words[3] = VifUnpackTo(kVifUnpackV4Count | kVifTops, 0, quadwords - 1);
    // the GIF tag: EOP, PRE with the tri strip, PACKED, the loops and registers of a particle (8 x 9) or a hexagon (6 x 13)
    uint32_t* gif = words + 4;
    const uint32_t loops = hexagons ? 6 : 8, nregs = hexagons ? 13 : 9;
    gif[0] = loops | 0x8000;                                   // NLOOP, EOP
    gif[1] = (1u << 14) | (kGsPrimTriStrip << 15) | (nregs << 28);   // PRE, PRIM, FLG packed, NREG
    const uint64_t regs = hexagons
        ? GifDescriptors({kGifSt, kGifRgbaq, kGifXyz2, kGifSt, kGifXyz2, kGifRgbaq, kGifSt, kGifXyz2, kGifRgbaq, kGifSt, kGifXyz2, kGifSt, kGifXyz2})
        : GifDescriptors({kGifUv, kGifRgbaq, kGifXyz2, kGifUv, kGifXyz2, kGifUv, kGifXyz2, kGifUv, kGifXyz2});
    gif[2] = uint32_t(regs);
    gif[3] = uint32_t(regs >> 32);
    if (hexagons) last[3] = kVifMscnt;
    else last[0] = kVifMscnt;
}

void ps2e_graphics_end_particle_block(ps2e_u32 blockAddress) {
    auto* tag = GuestMutable<uint32_t[4]>(blockAddress);
    if (!tag) return;
    (*tag)[1] = 0;
    (*tag)[0] = ((*tag)[0] & ~kTagId) | kReturnTag;
}

void ps2e_graphics_chain_particle_blocks(ps2e_u32 blocksAddress, ps2e_s32 count) {
    if (count <= 0 || count > 4096) return;
    const uint32_t* blocks = reinterpret_cast<const uint32_t*>(te::GuestPointer(blocksAddress, 4 * size_t(count)));
    if (!blocks) return;
    ps2e_graphics_end_particle_block(blocks[count - 1]);
    for (int32_t b = count - 2; b >= 0; --b) {
        auto* tag = GuestMutable<uint32_t[4]>(blocks[b]);
        if (!tag) continue;
        (*tag)[0] = ((*tag)[0] & ~kTagId) | kNextTag;
        // the next block's address as the game gave it: the PS2 cleared bit 31 there (the scratchpad flag; the blocks are
        // in main memory), which a module lane's address never has (the 32 MB) and the native lane's pointer has as its
        // own top bit (0x8xxxxxxx) -- masked, a chain stopped after its first block (GuestPointer refused the rest)
        (*tag)[1] = blocks[b + 1];
    }
}

void ps2e_graphics_init_particle_render_table(ps2e_u32 tableAddress) {
    auto* t = GuestMutable<GuestTable>(tableAddress);
    if (!t) return;
    constexpr uint32_t kHeadQuadwords = 2;
    constexpr uint32_t kStepsQuadwords = (offsetof(GuestTable, end) - offsetof(GuestTable, corners)) / 16;
    t->tag[0] = kCountTag | kHeadQuadwords;
    t->tag[1] = 0;
    t->tag[2] = kVifFlush;
    t->tag[3] = VifUnpackTo(kVifUnpackV4Count, kVuHeadAddress, kHeadQuadwords);
    t->stepsTag[0] = kReturnTag | (kStepsQuadwords + 1);
    t->stepsTag[1] = 0;
    t->stepsTag[2] = kVifFlush;
    t->stepsTag[3] = VifUnpackTo(kVifUnpackV4Count, kVuStepsAddress, kStepsQuadwords);
    t->end[0] = kVifFlushE;
    t->end[1] = t->end[2] = t->end[3] = 0;   // the MSCAL of the material's program is the writer's (the host runs the body)
}

void ps2e_graphics_write_particle_render_table(ps2e_u32 tableAddress, ps2e_u32 lookAddress) {
    auto* t = GuestMutable<GuestTable>(tableAddress);
    const auto* look = Guest<GuestLook>(lookAddress);
    if (!t || !look) return;
    t->gravity = look->gravity;
    int32_t* c = t->corners;
    c[0] = c[4] = look->texture[0];
    c[2] = c[6] = look->texture[2];
    c[1] = c[3] = look->texture[1];
    c[5] = c[7] = look->texture[3];
    for (uint32_t s = 0; s < kRenderSteps; ++s) {
        float* entry = t->steps[s].shape;
        for (unsigned v = 0; v < 6; ++v) entry[v] = look->steps[s].shape[v];
        auto* bytes = reinterpret_cast<uint8_t*>(entry);
        for (unsigned ch = 0; ch < 4; ++ch) bytes[ch * 4] = look->steps[s].colour[ch];
    }
    auto* hexagon = reinterpret_cast<float*>(t->corners);
    if (!look->hexagons) {
        hexagon[8] = hexagon[9] = hexagon[10] = hexagon[11] = 0.f;
        return;
    }
    for (unsigned v = 0; v < 12; ++v) hexagon[v] = look->hexagonCorners[v];
}

// DrawParticleBlock (particles.cpp): the block's chain (NEXT tags) drawn with the system's table through the matrix in the
// material at the time
void ps2e_graphics_draw_particle_block(ps2e_u32 blockAddress, ps2e_u32 tableAddress, ps2e_u32 matrixAddress, ps2e_u32 materialAddress, ps2e_f32 time) {
    static const bool noDraw = std::getenv("TE_GFX_NO_DRAW") != nullptr;
    if (noDraw) return;
    auto* t = GuestMutable<GuestTable>(tableAddress);
    const auto* matrix = reinterpret_cast<const float(*)[4]>(te::GuestPointer(matrixAddress, 64));
    if (!t || !matrix || !te::gfx::EnsureDevice()) return;
    // DrawParticleBlock wrote these into the table's head for VU1
    t->nearClip = kParticleNearClip;
    t->time = time;
    const MaterialView m = te::gfx::ReadMaterial(materialAddress);
    ps2e_draw_state state = te::gfx::StateOf(m);
    const uint32_t bucket = m.material ? m.bucket : TE_GFX_BUCKET_UI;
    const bool textured = state.texture != 0 && m.texture;
    const float tw = textured ? float(1 << m.texture->texture.widthPower) : 1.f;
    const float th = textured ? float(1 << m.texture->texture.heightPower) : 1.f;
    const float offX = te::gfx::GsOriginX(), offY = te::gfx::GsOriginY();
    const float widths = ParticleWidths();
    // the triangles that draw nothing (TE_GFX_PARTICLE_CULL) by the draw's own state; the frame's size for the census
    const int bound = CullOn() ? DiscardedAlphaBound(state) : -1;
    const bool additive = AdditiveParticles(state);
    const float frameW = float(te::gfx::ScreenW()), frameH = float(te::gfx::ScreenH());
    auto& scene = te::gfx::Scene();
    static const char* const diagPresent = std::getenv("TE_GFX_DIAG_PRESENT");
    // the counts a worker's blocks reached, folded in here (the census is the game thread's)
    static std::atomic<uint64_t> deferredTriangles{0}, deferredRefused{0};
    scene.particle_triangles += deferredTriangles.exchange(0, std::memory_order_relaxed);
    scene.particle_refused += deferredRefused.exchange(0, std::memory_order_relaxed);
    if (!diagPresent && te::gfx::DeferringBuilds()) {
        // the chain's blocks run on a worker: the table, the matrix and the blocks copied as they are now (the game re-fills
        // the pool next frame) into the build's own input storage, which it keeps from frame to frame (the copies cost no
        // allocation once it has grown, and the job is small enough for the pool's blocks); each block one draw, as below
        size_t count = 0;
        for (uint32_t a = blockAddress; count < 4096 && a;) {
            const uint8_t* block = te::GuestPointer(a, kParticleBlockBytes);
            if (!block) break;
            ++count;
            uint32_t tag, next;
            std::memcpy(&tag, block, 4);
            std::memcpy(&next, block + 4, 4);
            a = ((tag >> 28) & 7) == 2 ? next : 0;   // NEXT goes on, RET ends the chain
        }
        if (!count) return;
        te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, state);
        build.particles = true;
        std::vector<uint8_t>& input = build.input;
        input.resize(kInputHead + count * kParticleBlockBytes);
        std::memcpy(input.data(), t, sizeof(GuestTable));
        std::memcpy(input.data() + sizeof(GuestTable), matrix, 64);
        uint8_t* to = input.data() + kInputHead;
        for (size_t k = 0; k < count; ++k, to += kParticleBlockBytes) {
            const uint8_t* block = te::GuestPointer(blockAddress, kParticleBlockBytes);
            std::memcpy(to, block, kParticleBlockBytes);
            scene.particle_blocks++;
            uint32_t next;
            std::memcpy(&next, block + 4, 4);
            blockAddress = next;   // (the walk above: the blocks before the last are NEXT tags)
        }
        te::gfx::PostBuild(build, [&build, time, widths, offX, offY, tw, th, textured, bound, additive, frameW, frameH] {
            TE_PROF_SCOPE("particle build");
            thread_local GuestTable table;
            thread_local std::vector<std::vector<uint8_t>> kicks;
            thread_local std::vector<ps2e_draw_vertex> verts;
            const std::vector<uint8_t>& in = build.input;
            float m16[16];
            std::memcpy(&table, in.data(), sizeof table);
            std::memcpy(m16, in.data() + sizeof table, sizeof m16);
            const auto* mat = reinterpret_cast<const float(*)[4]>(m16);
            for (size_t at = kInputHead; at + kParticleBlockBytes <= in.size(); at += kParticleBlockBytes) {
                kicks.clear();
                if (!RunBlock(in.data() + at, table, mat, time, widths, kicks)) {
                    deferredRefused++;
                    break;
                }
                verts.clear();
                {
                    TE_PROF_SCOPE("particle: lower packets");
                    for (const auto& packet : kicks) te::gfx::LowerPackedPacket(packet, offX, offY, tw, th, textured, verts);
                    FilterBlock(verts, bound, additive, frameW, frameH);
                }
                const size_t n = verts.size() / 3 * 3;
                if (!n) continue;
                build.verts.insert(build.verts.end(), verts.begin(), verts.begin() + std::ptrdiff_t(n));
                build.segments.push_back(uint32_t(n));
                deferredTriangles += n / 3;
            }
        });
        return;
    }
    static std::vector<std::vector<uint8_t>> kicks;
    static std::vector<ps2e_draw_vertex> verts;
    uint32_t address = blockAddress;
    uint32_t chainIndex = 0;   // the interpolator's part: the block's place in the system's chain
    for (int guard = 0; guard < 4096 && address; ++guard, ++chainIndex) {
        const uint8_t* block = te::GuestPointer(address, kParticleBlockBytes);
        if (!block) break;
        uint32_t tag;
        std::memcpy(&tag, block, 4);
        kicks.clear();
        if (!RunBlock(block, *t, matrix, time, widths, kicks)) {
            scene.particle_refused++;
            static int said = 0;
            if (said++ < 4) std::fprintf(stderr, "[te.gfx] particle block %#x: the program declined\n", address);
            break;
        }
        verts.clear();
        for (const auto& packet : kicks) te::gfx::LowerPackedPacket(packet, offX, offY, tw, th, textured, verts);
        FilterBlock(verts, bound, additive, frameW, frameH);
        if (!verts.empty()) {
            // the interpolator: particle blocks are the NEWEST picture's as they are. A block is a pool node the game re-fills
            // and re-chains every frame (a particle dies, another is born into its slot; the chain is re-sorted), so the quads
            // of "the same block" do not correspond one to one between two pictures: a lerp paired the wrong particles (the
            // displacement check caught it). Particles are small, short-lived and motion-blurred by nature; shown at the
            // picture rate they read as they do at 60.
            (void)chainIndex;
            Draw& d = te::gfx::OpenDraw(bucket, state);
            d.particles = true;
            d.verts.insert(d.verts.end(), verts.begin(), verts.end());
            te::gfx::CloseDraw(bucket);
            scene.particle_triangles += verts.size() / 3;
        }
        if (const char* diag = diagPresent) {
            extern uint64_t te_graphics_presents();
            if (te_graphics_presents() + 1 == uint64_t(std::atol(diag))) {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                double r = 0, g = 0, b = 0, a = 0;
                for (const auto& v : verts) { x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y); r += v.r; g += v.g; b += v.b; a += v.a; }
                const size_t n = verts.size() ? verts.size() : 1;
                std::fprintf(stderr, "[te.gfx.diag] present %s particles block %#x bucket %u shader %#x tex %u (%gx%g) blend %d(%u %u %u %u) tris %zu box (%.0f %.0f)-(%.0f %.0f) rgba (%.0f %.0f %.0f %.0f) kicks %zu\n",
                             diag, address, bucket, m.shader ? unsigned(m.shader->type) : 0u, state.texture, tw, th, state.blend, state.blend_a, state.blend_b, state.blend_c, state.blend_d,
                             verts.size() / 3, x0, y0, x1, y1, r / n, g / n, b / n, a / n, kicks.size());
                // TE_GFX_DIAG_PARTICLES=1 adds the block's 32 records (two quadwords each, as floats) and every output triangle
                if (std::getenv("TE_GFX_DIAG_PARTICLES")) {
                    for (int i = 0; i < 4; ++i) std::fprintf(stderr, "[te.gfx.diag]   matrix row %d (%g %g %g %g)\n", i, matrix[i][0], matrix[i][1], matrix[i][2], matrix[i][3]);
                    std::fprintf(stderr, "[te.gfx.diag]   time %g nearClip %g gravity %g\n", time, t->nearClip, t->gravity);
                    for (uint32_t pIdx = 0; pIdx < kParticlesPerBlock; ++pIdx) {
                        float f[8];
                        std::memcpy(f, block + 32 + pIdx * 32, 32);
                        uint32_t w[8];
                        std::memcpy(w, block + 32 + pIdx * 32, 32);
                        if (!w[0] && !w[1] && !w[2] && !w[3] && !w[4] && !w[5] && !w[6] && !w[7]) continue;
                        std::fprintf(stderr, "[te.gfx.diag]   record %u: (%g %g %g %g | %g %g %g %g) hex (%08x %08x %08x %08x | %08x %08x %08x %08x)\n", pIdx,
                                     f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                    }
                    for (size_t v = 0; v + 2 < verts.size(); v += 3)
                        std::fprintf(stderr, "[te.gfx.diag]   tri (%.1f %.1f %.3g) (%.1f %.1f %.3g) (%.1f %.1f %.3g) rgba (%u %u %u %u)\n", verts[v].x, verts[v].y, verts[v].z,
                                     verts[v + 1].x, verts[v + 1].y, verts[v + 1].z, verts[v + 2].x, verts[v + 2].y, verts[v + 2].z, verts[v].r, verts[v].g, verts[v].b, verts[v].a);
                }
            }
        }
        scene.particle_blocks++;
        const uint32_t id = (tag >> 28) & 7;
        uint32_t next;
        std::memcpy(&next, block + 4, 4);
        address = id == 2 ? next : 0;   // NEXT goes on, RET ends the chain
    }
}

void ps2e_graphics_draw_distortion_block(ps2e_u32 block, ps2e_u32 table, ps2e_u32 matrix, ps2e_u32 material, ps2e_f32 time,
                                         ps2e_f32 distortionX, ps2e_f32 distortionY) {
    (void)block; (void)table; (void)matrix; (void)material; (void)time; (void)distortionX; (void)distortionY;
    Once("DrawDistortionBlock: the distorting hexagons (the frame as their texture) are not drawn yet");
}

}  // extern "C"
