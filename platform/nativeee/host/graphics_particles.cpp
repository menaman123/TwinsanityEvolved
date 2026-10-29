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
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/zero_kick.h"
#include "graphics_internal.h"
#include "te_prof.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_host.h"

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
bool RunBlock(const uint8_t* block, const GuestTable& table, const float (*matrix)[4], float time, std::vector<std::vector<uint8_t>>& kicks) {
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
    for (uint32_t s = 0; s < kRenderSteps; ++s) {
        img.SetQw(kVuStepsAddress + 3 + 2 * s, table.steps[s].shape);
        img.SetQw(kVuStepsAddress + 4 + 2 * s, &table.steps[s].shape[4]);
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
    {
        te::prof::Scope emitScope(profEmit);
        // the kicks alone, the program over this image in place (RunCCont's packets without its working copy and flags)
        if (!(full ? ZeroKick::RunCCont(in, run) : ZeroKick::RunCContKicks(in, img.memory.data(), run))) return false;
    }
    kicks = std::move(run.kicks);
    return true;
}
}  // namespace

namespace te::gfx {
void SetParticleFull(bool on) { g_particleFull.store(on, std::memory_order_relaxed); }
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
        (*tag)[1] = blocks[b + 1] & 0x7FFFFFFFu;
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
    auto& scene = te::gfx::Scene();
    static const char* const diagPresent = std::getenv("TE_GFX_DIAG_PRESENT");
    // the counts a worker's blocks reached, folded in here (the census is the game thread's)
    static std::atomic<uint64_t> deferredTriangles{0}, deferredRefused{0};
    scene.particle_triangles += deferredTriangles.exchange(0, std::memory_order_relaxed);
    scene.particle_refused += deferredRefused.exchange(0, std::memory_order_relaxed);
    if (!diagPresent && te::gfx::DeferringBuilds()) {
        // the chain's blocks run on a worker: the blocks, the table and the matrix copied as they are now (the game
        // re-fills the pool next frame); each block one draw, as below
        std::vector<uint8_t> blocks;
        for (int guard = 0; guard < 4096 && blockAddress; ++guard) {
            const uint8_t* block = te::GuestPointer(blockAddress, kParticleBlockBytes);
            if (!block) break;
            blocks.insert(blocks.end(), block, block + kParticleBlockBytes);
            scene.particle_blocks++;
            uint32_t tag, next;
            std::memcpy(&tag, block, 4);
            std::memcpy(&next, block + 4, 4);
            blockAddress = ((tag >> 28) & 7) == 2 ? next : 0;   // NEXT goes on, RET ends the chain
        }
        if (blocks.empty()) return;
        std::array<float, 16> m16;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m16[size_t(r * 4 + c)] = matrix[r][c];
        te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, state);
        te::gfx::PostBuild(build, [&build, blocks = std::move(blocks), table = *t, m16, time, offX, offY, tw, th, textured] {
            TE_PROF_SCOPE("particle build");
            thread_local std::vector<std::vector<uint8_t>> kicks;
            thread_local std::vector<ps2e_draw_vertex> verts;
            const auto* mat = reinterpret_cast<const float(*)[4]>(m16.data());
            for (size_t at = 0; at + kParticleBlockBytes <= blocks.size(); at += kParticleBlockBytes) {
                kicks.clear();
                if (!RunBlock(blocks.data() + at, table, mat, time, kicks)) {
                    deferredRefused++;
                    break;
                }
                verts.clear();
                {
                    TE_PROF_SCOPE("particle: lower packets");
                    for (const auto& packet : kicks) te::gfx::LowerPackedPacket(packet, offX, offY, tw, th, textured, verts);
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
        if (!RunBlock(block, *t, matrix, time, kicks)) {
            scene.particle_refused++;
            static int said = 0;
            if (said++ < 4) std::fprintf(stderr, "[te.gfx] particle block %#x: the program declined\n", address);
            break;
        }
        verts.clear();
        for (const auto& packet : kicks) te::gfx::LowerPackedPacket(packet, offX, offY, tw, th, textured, verts);
        if (!verts.empty()) {
            // the interpolator: particle blocks are the NEWEST picture's as they are. A block is a pool node the game re-fills
            // and re-chains every frame (a particle dies, another is born into its slot; the chain is re-sorted), so the quads
            // of "the same block" do not correspond one to one between two pictures: a lerp paired the wrong particles (the
            // displacement check caught it). Particles are small, short-lived and motion-blurred by nature; shown at the
            // picture rate they read as they do at 60.
            (void)chainIndex;
            Draw& d = te::gfx::OpenDraw(bucket, state);
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
