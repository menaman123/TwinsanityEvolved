#include "games/slus_20909_crash_twinsanity/hle/graphics/skin_geometry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>

#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_float.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/qw_ptr.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/skinned_lit_strip.h"

namespace title {
namespace {
using Qw = SkinGeometry::Qw;
using Draw = ps2r::gs::DrawIr;
using Vertex = ps2r::gs::VertexRec;

uint32_t Word(const uint8_t* bytes, size_t at) {
    uint32_t value;
    std::memcpy(&value, bytes + at, 4);
    return value;
}
// The VIF unpack's element widening (vif_unpack.cpp ReadElement): signed
// formats sign-extend (usn = 0 is the only form the skins use).
uint32_t Widen(const uint8_t* at, unsigned bits) {
    if (bits == 32) return Word(at, 0);
    if (bits == 16) {
        uint16_t raw;
        std::memcpy(&raw, at, 2);
        return uint32_t(int32_t(int16_t(raw)));
    }
    return uint32_t(int32_t(int8_t(*at)));
}

// The batch image the VIF stream leaves in the double buffer (TOPS-relative):
// QW0 the tag, QW1 the count, QW2 the scale, QW3.. the vertices (stride 4).
struct BatchImage {
    std::vector<Qw> qw;
    std::vector<bool> written;
};

bool SetImageQw(BatchImage& image, uint32_t at, const Qw& value) {
    if (at >= image.qw.size()) return false;
    image.qw[at] = value;
    image.written[at] = true;
    return true;
}

// The last three vertices of a batch's strip (a ring: `count` pushed since Clear, the oldest at count % 3)
struct Queue {
    Vertex v[3];
    unsigned count = 0;
    void Clear() { count = 0; }
};
// The GS strip assembly of VertexKick::AssembleStrip over one batch's output
// (the ADC bit restarts the strip; a degenerate triangle still advances the
// parity; even parity q0 q1 q2, odd q1 q0 q2).
void Assemble(Queue& queue, unsigned& parity, const Vertex& v, bool draw, std::vector<Vertex>& out) {
    queue.v[queue.count % 3] = v;
    ++queue.count;
    if (!draw) {
        parity = 0;
        return;
    }
    if (queue.count < 3) return;
    const Vertex& q0 = queue.v[queue.count % 3];
    const Vertex& q1 = queue.v[(queue.count + 1) % 3];
    const Vertex& q2 = queue.v[(queue.count + 2) % 3];
    const bool degenerate = (q0.x == q1.x && q0.y == q1.y) || (q1.x == q2.x && q1.y == q2.y) ||
                            (q0.x == q2.x && q0.y == q2.y);
    if (!degenerate) {
        if (parity % 2 == 0) {
            out.push_back(q0);
            out.push_back(q1);
        } else {
            out.push_back(q1);
            out.push_back(q0);
        }
        out.push_back(q2);
    }
    ++parity;
}

constexpr uint32_t kJointsBase = 13;        // g_VuJoints: QW[13 + 4 * joint]
constexpr uint32_t kSkinData = 78;          // g_VuSkinData: the data rows after the material's buffer
constexpr uint32_t kSkinClip = 11;          // g_VuSkinClip: + the clipped mode's rows
constexpr uint32_t kEyeRow = 77;            // g_VuSkinEye (unused by the lit emitter; zero)
constexpr uint32_t kScaleSkinPc = 0xcb0 / 8, kSkin1040Pc = 0x1040 / 8, kClipMarkPc = 0xb30 / 8, kEmitterPc = 0x1720 / 8,
                   kExitPc = 0x90 / 8;
constexpr uint32_t kQwCount = 1024;
}  // namespace

namespace {
// The batch the VIF stream leaves at TOP, closed by `close` (an MSCAL, or the end of an unterminated part's stream)
SkinGeometry::Result CloseBatch(BatchImage& image, uint32_t vertices, uint32_t& total, std::vector<SkinGeometry::Batch>& batches) {
    using Result = SkinGeometry::Result;
    SkinGeometry::Batch batch;
    batch.tag = image.qw[0];
    batch.count = image.qw[1];
    batch.scale = image.qw[2];
    const uint32_t n = batch.count[0] & 0xffff;
    if (!image.written[0] || !image.written[1] || !image.written[2] || n < 12 || (n & 3) ||
        n / 4 > SkinGeometry::kMaxBatchVertices)
        return Result::InvalidAsset;
    const uint32_t count = n / 4;
    // The tag: NLOOP = the vertices, EOP, PRE with PRIM type 4, PACKED, NREG 3 of ST/RGBAQ/XYZ2.
    if ((batch.tag[0] & 0x7fff) != count || !(batch.tag[0] & 0x8000) || batch.tag[1] != 0x30024000 ||
        batch.tag[2] != 0x512 || batch.tag[3] != 0)
        return Result::InvalidAsset;
    for (uint32_t q = 3; q < 3 + 4 * count; ++q) if (!image.written[q]) return Result::InvalidAsset;
    batch.vertices.assign(image.qw.begin() + 3, image.qw.begin() + 3 + 4 * count);
    total += count;
    if (vertices && total > vertices) return Result::InvalidAsset;
    batches.push_back(std::move(batch));
    std::fill(image.written.begin(), image.written.end(), false);
    return Result::Ok;
}

// The skin packet stream: a RET (or END) tag whose two VIF words and the stream after it are the fixed authored shape.
// `terminated`: every batch closes with MSCAL 0 (a skin submodel); else the one batch is left open at the stream's end
// (a blend part: the chain's programs follow). `vertices` 0 = the stream's own count.
SkinGeometry::Result DecodeStream(const uint8_t* bytes, size_t size, uint32_t vertices, bool terminated, SkinGeometry::Part& out) {
    using Result = SkinGeometry::Result;
    using Part = SkinGeometry::Part;
    // `size` is the resource's declared byte count (SkinRead's sizes[i] / BlendPart::packetSize): not 16-aligned in the
    // authored assets (10492, 796, ...); the DMA moves the tag's qwc quadwords and whatever follows them is never read.
    if (!bytes || size < 32 || size > 4 * 1024 * 1024 || (vertices && vertices < 3)) return Result::InvalidAsset;
    if (vertices > SkinGeometry::kMaxVertices) return Result::LimitExceeded;
    // The tag: qwc = the whole VIF stream; its own two VIF words are part of the stream (MARK/NOP in the skins).
    const uint32_t tag = Word(bytes, 0);
    const uint32_t id = (tag >> 28) & 7;
    if ((id != 6 && id != 7) || Word(bytes, 4) != 0) return Result::InvalidAsset;
    const size_t limit = 16 + size_t(tag & 0xffff) * 16;
    if (limit > size) return Result::InvalidAsset;
    Part part;
    uint32_t total = 0, cl = 1, wl = 1, mode = 0;
    Qw row{};
    BatchImage image;
    image.qw.assign(3 + 4 * SkinGeometry::kMaxBatchVertices, Qw{});
    image.written.assign(image.qw.size(), false);
    bool any = false;
    size_t cursor = 8;
    while (cursor + 4 <= limit) {
        const uint32_t code = Word(bytes, cursor);
        cursor += 4;
        const uint32_t cmd = (code >> 24) & 0x7f;
        if (code & 0x80000000u) return Result::InvalidAsset;  // no interrupts in an authored packet
        if (cmd == 0x00 || cmd == 0x07 || cmd == 0x10 || cmd == 0x11 || cmd == 0x13) continue;  // NOP / MARK / FLUSHes
        if (cmd == 0x01) {                                     // STCYCL
            cl = code & 0xff;
            wl = (code >> 8) & 0xff;
            if (!cl || !wl || wl > cl) return Result::InvalidAsset;
            continue;
        }
        if (cmd == 0x05) {                                     // STMOD
            mode = code & 3;
            if (mode > 1) return Result::InvalidAsset;
            continue;
        }
        if (cmd == 0x30) {                                     // STROW
            if (cursor + 16 > limit) return Result::InvalidAsset;
            for (unsigned k = 0; k < 4; ++k) row[k] = Word(bytes, cursor + 4 * k);
            cursor += 16;
            continue;
        }
        if (cmd == 0x14) {                                     // MSCAL: the batch is complete
            if ((code & 0xffff) != 0 || !any || !terminated) return Result::InvalidAsset;
            const Result closed = CloseBatch(image, vertices, total, part.batches);
            if (closed != Result::Ok) return closed;
            any = false;
            continue;
        }
        if ((cmd & 0x60) != 0x60) return Result::InvalidAsset;  // anything but an UNPACK
        const uint32_t format = cmd & 0xf;
        const bool masked = (cmd >> 4) & 1;
        uint32_t num = (code >> 16) & 0xff;
        if (!num) num = 256;
        const uint32_t imm = code & 0xffff;
        const uint32_t address = imm & 0x3ff;
        const bool usn = (imm >> 14) & 1, tops = (imm >> 15) & 1;
        unsigned bits = 0, elements = 0;
        switch (format) {
            case 0xc: bits = 32; elements = 4; break;
            case 0x4: bits = 32; elements = 2; break;
            case 0xd: bits = 16; elements = 4; break;
            case 0xe: bits = 8; elements = 4; break;
            default: return Result::InvalidAsset;
        }
        if (masked || usn || !tops) return Result::InvalidAsset;
        const size_t payload = (size_t(num) * elements * bits / 8 + 3) & ~size_t(3);
        if (cursor + payload > limit) return Result::InvalidAsset;
        // The write cycle (cl >= wl): `wl` writes then `cl - wl` skipped quadwords.
        uint32_t at = address, write_cycle = 0;
        const uint8_t* src = bytes + cursor;
        for (uint32_t k = 0; k < num; ++k) {
            Qw vec{};
            for (unsigned e = 0; e < elements; ++e) vec[e] = Widen(src + (size_t(k) * elements + e) * (bits / 8), bits);
            if (elements == 2) {
                vec[2] = vec[0];
                vec[3] = vec[1];
            }
            if (mode == 1) for (unsigned f = 0; f < 4; ++f) vec[f] += row[f];
            if (!SetImageQw(image, at, vec)) return Result::InvalidAsset;
            any = true;
            ++at;
            if (++write_cycle >= wl) {
                write_cycle = 0;
                at += cl - wl;
            }
        }
        cursor += payload;
    }
    if (cursor != limit) return Result::InvalidAsset;
    if (!terminated) {
        if (!any || !part.batches.empty()) return Result::InvalidAsset;
        const Result closed = CloseBatch(image, vertices, total, part.batches);
        if (closed != Result::Ok) return closed;
        any = false;
    }
    if (any || part.batches.empty() || (vertices && total != vertices)) return Result::InvalidAsset;
    part.vertices = total;
    out = std::move(part);
    return Result::Ok;
}
}  // namespace

SkinGeometry::Result SkinGeometry::Decode(const uint8_t* bytes, size_t size, uint32_t vertices, Part& out) {
    if (vertices < 3) return Result::InvalidAsset;
    return DecodeStream(bytes, size, vertices, true, out);
}

SkinGeometry::Result SkinGeometry::DecodePart(const uint8_t* bytes, size_t size, uint32_t vertices, Part& out) {
    return DecodeStream(bytes, size, vertices, false, out);
}

namespace {
// The programs a batch runs before the emitter: a skin submodel's fused scale + skin (0xcb0); a blend part's scale pass
// (0x12a0) or displacement passes (0x1400 then 0x15a0 per shape) then the float-input skin (0x1040).
struct BatchProgram {
    const SkinGeometry::BlendInputs* blend = nullptr;   // null: a skin submodel
};

// The 0x1400 / 0x15a0 displacement arithmetic (DisplacePrefix::RunDisplaceInt / RunDisplaceFloat as values, the FMAC
// rule: operands clamped, results clamped on store): the first shape converts and scales the int vertices as the scale
// pass does and adds its row; each further shape adds its row to the float position. row = ITOF0(int8 xyzw) * factors.xyz
// * weight.
bool RunDisplace(QwPtr m, uint32_t top, const SkinGeometry::Shape& shape, const std::array<float, 3>& factors, bool first) {
    const uint32_t count = m.W(top + 1, 0) & 0xFFFF;
    if (count == 0 || (count & 3) != 0) return false;
    const uint32_t vertices = count / 4;
    if (!shape.rows || shape.count < vertices) return false;
    const float sx = vu1lib::CF(m.F(top + 2, 0)), sy = vu1lib::CF(m.F(top + 2, 1));
    const float r8[4] = {vu1lib::CF(factors[0]), vu1lib::CF(factors[1]), vu1lib::CF(factors[2]), vu1lib::CF(shape.weight)};
    for (uint32_t j = 0; j < vertices; ++j) {
        const uint32_t v = top + 3 + 4 * j;
        float f5[4], f6[4], f9[4];
        for (int k = 0; k < 4; ++k) {
            const int8_t d = int8_t(shape.rows[size_t(j) * 4 + size_t(k)]);   // the V4-8 unpack sign-extends
            f9[k] = float(int32_t(d));                                        // ITOF0
            if (first) {
                f5[k] = float(int32_t(m.W(v, k)));
                f6[k] = float(int32_t(m.W(v + 1, k)));
            }
        }
        if (first) {
            for (int k = 0; k < 4; ++k) f5[k] = vu1lib::CF(vu1lib::CF(f5[k]) * sx);   // MULx vf05, vf11
            for (int k = 0; k < 4; ++k) f6[k] = vu1lib::CF(vu1lib::CF(f6[k]) * sy);   // MULy vf06, vf11
        }
        for (int k = 0; k < 3; ++k) f9[k] = vu1lib::CF(vu1lib::CF(f9[k]) * r8[k]);     // MUL.xyz vf09, vf08
        for (int k = 0; k < 3; ++k) f9[k] = vu1lib::CF(vu1lib::CF(f9[k]) * r8[3]);     // MULw.xyz vf09, vf08
        for (int k = 0; k < 3; ++k) {                                                   // ADD.xyz vf05, vf09, vf05
            const float p = first ? vu1lib::CF(f5[k]) : vu1lib::CF(vu1lib::bits2f(m.W(v, k)));
            const float res = vu1lib::CF(vu1lib::CF(f9[k]) + p);
            m.SetW(v, k, vu1lib::f2bits(res));
        }
        if (first) {
            m.SetW(v, 3, vu1lib::f2bits(f5[3]));
            for (int k = 0; k < 4; ++k) m.SetW(v + 1, k, vu1lib::f2bits(f6[k]));
        }
    }
    return true;
}

#if defined(__vita__)
// The Vita's skinning: what the 0xcb0 scale + skin and the 0x1720 lit emitter compute, done per vertex in plain floats
// straight from the batch (no VU memory image, no clamped operands, the CPU's rounding) -- the faithful programs cost
// the Vita's builders most of their frame. The plain skins, clipped or not, and the blend skins (SetFastPaths(false), the
// TE_GFX_SKIN_FAST=0 knob: the clipped plain skins and the blend skins through their VU programs again, as before).
//   position, normal x: QW0 ints x the batch's x scale; S, T, normal y z: QW1 ints x its y scale; QW2: the three weights
//   (each one's low 9 bits its bone's quadword in the palette), w = the flags (bits 0-1 the bone count, 15 ADC); QW3 the
//   vertex colour. Bones blend in order; the normal turns by the first bone; the colour is lit by the three lights'
//   (direction . normal, at least 0) times their colours plus the ambient, times the vertex colour, at most 255.
//   A blend skin (`blend`): every shape adds its int8 row x factors.xyz x weight to the scaled position (the 0x1400 /
//   0x15a0 displacement passes), then it skins as a plain skin (the 0x1040 float-input skin = the same blend of bones).
//   Clipped (in.clipped: the 0xb30 clip mark): the skinned position through the matrix to the camera, scaled by the clip
//   vector, judged against its w as the CLIP instruction judges (VuFloat::ClipNf of the clamped lanes); a vertex whose
//   judgement or either of the two before it in the batch is outside takes the ADC bit (the triangle it closes is not
//   drawn) -- the guard-band rule of the program; nothing is cut.
// The batches' triangle lists appended to `verts`, each batch's end in `ends` (a batch with no triangle adds none): the
// runs FastSkin publishes one DrawIr each (SkinGeometry::BuildRuns hands them over without the DrawIr per batch)
std::atomic<bool> g_fastPaths{true};

SkinGeometry::Result FastSkinRuns(const SkinGeometry::Part& part, const SkinGeometry::Inputs& in, const Draw& state,
                                  std::vector<Vertex>& verts, std::vector<uint32_t>& ends,
                                  const SkinGeometry::BlendInputs* blend = nullptr) {
    using Result = SkinGeometry::Result;
    const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
    float screen[4][4], light[3][3], colour[3][3], ambient[3];
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) screen[r][k] = F(in.to_screen[r][k]);
    for (int l = 0; l < 3; ++l)
        for (int k = 0; k < 3; ++k) { light[l][k] = F(in.lights[l][k]); colour[l][k] = F(in.colours[l][k]); }
    for (int k = 0; k < 3; ++k) ambient[k] = F(in.ambient[k]);
    // the clip mark's rows: the matrix to the camera and the clip (guard) vector
    const bool clipped = in.clipped;
    float camera[4][4] = {}, guard[4] = {};
    if (clipped) {
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) camera[r][k] = F(in.to_camera[r][k]);
        for (int k = 0; k < 4; ++k) guard[k] = F(in.clip[k]);
    }
    // a blend skin's shapes: each one's factors x weight per lane (the displacement's two products, in their order); the
    // caller (DrawBlendSkin) passes at most 64
    struct ShapeScale { const uint8_t* rows; uint32_t count; float factor[3]; float weight; };
    ShapeScale shapes[64];
    size_t shapeCount = 0;
    if (blend) {
        if (blend->shapes.size() > 64) return Result::LimitExceeded;
        for (const auto& sh : blend->shapes) {
            ShapeScale& s = shapes[shapeCount++];
            s.rows = sh.rows;
            s.count = sh.count;
            for (int k = 0; k < 3; ++k) s.factor[k] = blend->factors[size_t(k)];
            s.weight = sh.weight;
        }
    }
    const size_t palette = in.joints.size();
    Queue queue;
    const size_t first = verts.size();
    for (const auto& batch : part.batches) {
        const uint32_t count = batch.Vertices();
        if (count < 3 || count > SkinGeometry::kMaxBatchVertices || batch.vertices.size() != 4 * size_t(count))
            return Result::InvalidGeometry;
        for (size_t s = 0; s < shapeCount; ++s)   // RunDisplace's refusal: a shape without a row for every vertex
            if (!shapes[s].rows || shapes[s].count < count) return Result::InvalidGeometry;
        const float sx = F(batch.scale[0]), sy = F(batch.scale[1]);
        const size_t start = verts.size();
        queue.Clear();
        unsigned parity = 0;
        unsigned outside1 = 0, outside2 = 0;   // the clip mark's window: the two judgements before this vertex's
        for (uint32_t j = 0; j < count; ++j) {
            const auto& q0 = batch.vertices[4 * j];
            const auto& q1 = batch.vertices[4 * j + 1];
            const auto& q2 = batch.vertices[4 * j + 2];
            const auto& q3 = batch.vertices[4 * j + 3];
            float px = float(int32_t(q0[0])) * sx, py = float(int32_t(q0[1])) * sx, pz = float(int32_t(q0[2])) * sx;
            const float nx = float(int32_t(q0[3])) * sx, ny = float(int32_t(q1[2])) * sy, nz = float(int32_t(q1[3])) * sy;
            const float s0 = float(int32_t(q1[0])) * sy, t0 = float(int32_t(q1[1])) * sy;
            for (size_t s = 0; s < shapeCount; ++s) {
                const uint8_t* row = shapes[s].rows + size_t(j) * 4;   // V4-8: sign-extended
                px += float(int32_t(int8_t(row[0]))) * shapes[s].factor[0] * shapes[s].weight;
                py += float(int32_t(int8_t(row[1]))) * shapes[s].factor[1] * shapes[s].weight;
                pz += float(int32_t(int8_t(row[2]))) * shapes[s].factor[2] * shapes[s].weight;
            }
            const uint32_t flags = q2[3] & 0xFFFFu;
            const uint32_t bones = flags & 3u;
            const int used = bones == 1 ? 1 : bones == 2 ? 2 : 3;
            float po[3] = {0.f, 0.f, 0.f}, n[3] = {0.f, 0.f, 0.f};
            for (int b = 0; b < used; ++b) {
                const size_t at = q2[b] & 0x1FFu;
                if (at + 3 >= palette) continue;
                const float w = F(q2[b]);
                const auto& c0 = in.joints[at];
                const auto& c1 = in.joints[at + 1];
                const auto& c2 = in.joints[at + 2];
                const auto& tr = in.joints[at + 3];
                for (int k = 0; k < 3; ++k)
                    po[k] += w * (F(tr[k]) + F(c0[k]) * px + F(c1[k]) * py + F(c2[k]) * pz);
                if (b == 0)
                    for (int k = 0; k < 3; ++k) n[k] = F(c0[k]) * nx + F(c1[k]) * ny + F(c2[k]) * nz;
            }
            float T[4];
            for (int k = 0; k < 4; ++k) T[k] = screen[3][k] + screen[0][k] * po[0] + screen[1][k] * po[1] + screen[2][k] * po[2];
            const float Q = T[3] != 0.f ? 1.0f / T[3] : 3.4028235e38f;
            float lit[3] = {ambient[0], ambient[1], ambient[2]};
            for (int l = 0; l < 3; ++l) {
                const float d = std::max(0.f, light[l][0] * n[0] + light[l][1] * n[1] + light[l][2] * n[2]);
                for (int k = 0; k < 3; ++k) lit[k] += colour[l][k] * d;
            }
            Vertex v{};
            const auto channel = [](float value) { return uint8_t(int32_t(std::min(value, 255.f))); };
            v.r = channel(lit[0] * float(int32_t(q3[0])));
            v.g = channel(lit[1] * float(int32_t(q3[1])));
            v.b = channel(lit[2] * float(int32_t(q3[2])));
            v.a = uint8_t(std::min<int32_t>(int32_t(q3[3]), 255));
            v.s = (s0 + in.scroll[0]) * Q;
            v.t = (t0 + in.scroll[1]) * Q;
            v.q = Q;
            const auto fixed = [](float value) {
                if (!(std::fabs(value) < 2147483648.f)) return value < 0.f ? 0x80000000u : 0x7FFFFFFFu;
                return uint32_t(int32_t(value));
            };
            v.x = float(uint16_t(fixed(T[0] * Q))) / 16.0f - state.off_x;
            v.y = float(uint16_t(fixed(T[1] * Q))) / 16.0f - state.off_y;
            v.z = float(fixed(T[2] * Q));
            bool adc = (flags & 0x8000u) != 0;
            if (clipped) {
                // the clip mark: XformClip (to the camera) then GuardScale (x the clip vector), judged by CLIP on the
                // clamped lanes; the window of this judgement and the two before it in the batch
                uint32_t g[4];
                for (int k = 0; k < 4; ++k) {
                    const float c = camera[3][k] + camera[0][k] * po[0] + camera[1][k] * po[1] + camera[2][k] * po[2];
                    const float scaled = VuFloat::ClampFloat(guard[k] * c);
                    std::memcpy(&g[k], &scaled, 4);
                }
                const unsigned outside = VuFloat::ClipNf(g[0], g[1], g[2], g[3]) != 0 ? 1u : 0u;
                if (outside | outside1 | outside2) adc = true;
                outside2 = outside1;
                outside1 = outside;
            }
            v.draw_kick = adc ? 0 : 1;
            Assemble(queue, parity, v, !adc, verts);
        }
        if (verts.size() - first > SkinGeometry::kMaxVertices * 24) return Result::LimitExceeded;
        if (verts.size() == start) continue;
        ends.push_back(uint32_t(verts.size()));
    }
    return Result::Ok;
}

SkinGeometry::Result FastSkin(const SkinGeometry::Part& part, const SkinGeometry::Inputs& in, const Draw& state, std::vector<Draw>& out,
                              const SkinGeometry::BlendInputs* blend = nullptr) {
    using Result = SkinGeometry::Result;
    thread_local std::vector<Vertex> verts;
    thread_local std::vector<uint32_t> ends;
    verts.clear();
    ends.clear();
    const Result r = FastSkinRuns(part, in, state, verts, ends, blend);
    if (r != Result::Ok) return r;
    size_t at = 0;
    for (const uint32_t end : ends) {
        Draw draw = state;
        draw.verts.assign(verts.begin() + std::ptrdiff_t(at), verts.begin() + std::ptrdiff_t(end));
        draw.vertex_count = uint32_t(draw.verts.size());
        out.push_back(std::move(draw));
        at = end;
    }
    return Result::Ok;
}
#endif

SkinGeometry::Result BuildBatches(const SkinGeometry::Part& part, const SkinGeometry::Inputs& inputs, const BatchProgram& program,
                                  const Draw& state, std::vector<Draw>& out) {
    using Result = SkinGeometry::Result;
    if (!state.tex_enabled || state.use_fst || state.tex.fst || state.ctx || state.fog || state.prim_type != 4 ||
        state.raster.frame.psm || state.raster.zbuf.psm)
        return Result::UnsupportedState;
    if (inputs.joints.empty() || (inputs.joints.size() & 3) || inputs.joints.size() / 4 > SkinGeometry::kMaxJoints ||
        part.batches.empty())
        return Result::InvalidGeometry;
#if defined(__vita__)
    if (!program.blend && !inputs.clipped) return FastSkin(part, inputs, state, out);
    if (g_fastPaths.load(std::memory_order_relaxed)) return FastSkin(part, inputs, state, out, program.blend);
#endif
    // The image: the header (QW1.x = the output list, QW3.x = the material's
    // buffer), the joints at 13.., the material's buffer B (the dispatcher
    // nodes, then the data rows at B+78..), the batch at `top`, the output at
    // `list` -- every region bounded inside the 1024 quadwords.
    const uint32_t joints_end = kJointsBase + uint32_t(inputs.joints.size());
    const uint32_t buffer = (joints_end + 15) & ~15u;
    const uint32_t top = buffer + 0x60;
    const uint32_t list = top + 3 + 4 * SkinGeometry::kMaxBatchVertices + 1;
    if (list + 1 + 3 * SkinGeometry::kMaxBatchVertices > kQwCount) return Result::InvalidGeometry;
    // one image per thread, cleared for every skin as a fresh one would be (the programs read what they were given)
    thread_local std::vector<uint8_t> memory(kQwCount * 16);
    std::fill(memory.begin(), memory.end(), uint8_t(0));
    QwPtr m{memory.data()};
    const auto set = [&](uint32_t qw, const Qw& value) { for (int k = 0; k < 4; ++k) m.SetW(qw, k, value[k]); };
    const auto f2bits = [](float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; };
    set(1, {list, 0, 0, 0});
    set(3, {buffer, 0, 0, 0});
    for (size_t j = 0; j < inputs.joints.size(); ++j) set(kJointsBase + uint32_t(j), inputs.joints[j]);
    // The dispatcher nodes (WriteParameterEntries / WriteShaderPacket): the
    // scale+skin node (a blend part's float-input skin), the clip-mark node when clipped, the emitter with its
    // colour, payload and an empty register packet, then the exit node.
    uint32_t node = buffer;
    set(node, {program.blend ? kSkin1040Pc : kScaleSkinPc, 0, node + 1, 0});
    ++node;
    if (inputs.clipped) {
        set(node, {kClipMarkPc, 0, node + 1, 0});
        ++node;
    }
    const uint32_t emitter = node;
    set(emitter, {kEmitterPc, 0, emitter + 4, 1});
    set(emitter + 1, {0x80, 0x80, 0x80, 0x80});                                   // the shader's colour (unused by 0x1720)
    set(emitter + 2, {f2bits(inputs.scroll[0]), f2bits(inputs.scroll[1]), 0, 0});  // the UV payload
    set(emitter + 3, {0x00008000, 0x10000000, 0xe, 0});                            // an empty A+D register packet (EOP)
    set(emitter + 4, {kExitPc, 0, 0, 0});
    if (emitter + 4 >= buffer + kEyeRow) return Result::InvalidGeometry;
    // The skin's data rows: to screen, the lights transposed, the colours, the ambient; the clip rows.
    for (uint32_t r = 0; r < 4; ++r) set(buffer + kSkinData + r, inputs.to_screen[r]);
    for (uint32_t r = 0; r < 3; ++r) {
        Qw column{};
        for (uint32_t k = 0; k < 3; ++k) column[k] = inputs.lights[k][r];
        set(buffer + kSkinData + 4 + r, column);
    }
    for (uint32_t r = 0; r < 3; ++r) set(buffer + kSkinData + 7 + r, inputs.colours[r]);
    set(buffer + kSkinData + 10, inputs.ambient);
    if (inputs.clipped) {
        for (uint32_t r = 0; r < 4; ++r) set(buffer + kSkinData + kSkinClip + r, inputs.to_camera[r]);
        set(buffer + kSkinData + kSkinClip + 4, inputs.clip);
    }
    std::vector<Draw> result;
    size_t output = 0;
    VuChopRoundGuard rounding;
    Queue queue;
    for (const auto& batch : part.batches) {
        const uint32_t count = batch.Vertices();
        if (count < 3 || count > SkinGeometry::kMaxBatchVertices || batch.vertices.size() != 4 * size_t(count))
            return Result::InvalidGeometry;
        // The batch's quadwords, exactly as the VIF left them; the stale tail
        // the pipelined scale pass reads past the vertices is zero here.
        std::memset(memory.data() + size_t(top) * 16, 0, size_t(list - top) * 16);
        set(top, batch.tag);
        set(top + 1, batch.count);
        set(top + 2, batch.scale);
        for (uint32_t q = 0; q < 4 * count; ++q) set(top + 3 + q, batch.vertices[q]);
        std::memset(memory.data() + size_t(list) * 16, 0, size_t(1 + 3 * SkinGeometry::kMaxBatchVertices) * 16);
        if (!program.blend) {
            if (!SkinnedLitStrip::RunSkinCb0(m, top)) return Result::InvalidGeometry;
        } else {
            const auto& blend = *program.blend;
            if (blend.shapes.empty()) {
                if (!SkinnedLitStrip::RunScalePhase(m, top, nullptr)) return Result::InvalidGeometry;   // program 6 (0x12a0)
            } else {
                for (size_t i = 0; i < blend.shapes.size(); ++i)
                    if (!RunDisplace(m, top, blend.shapes[i], blend.factors, i == 0)) return Result::InvalidGeometry;
            }
            if (!SkinnedLitStrip::RunSkin1040(m, top, 0)) return Result::InvalidGeometry;   // pos.w (the stale vf19.w) is never read
        }
        if (inputs.clipped && !SkinnedLitStrip::RunClipMark0B30(m, top)) return Result::InvalidGeometry;
        // the emitter's body: the strip packet in memory, read back below (the kicks would only copy it)
        if (!SkinnedLitStrip::RunSkinnedLitBody(m, emitter, top)) return Result::InvalidGeometry;
        // The strip packet is at `list`: QW[list+1+3j] ST, +1 RGBAQ, +2 XYZ2 (the
        // PACKED lanes the GS reads: VertexKick::PackedXyz, PackedSt, PackedRgbaq).
        Draw draw = state;
        draw.verts.clear();
        draw.verts.reserve(size_t(count) * 3);   // a triangle per vertex at most (no regrowth while assembling)
        queue.Clear();
        unsigned parity = 0;
        for (uint32_t j = 0; j < count; ++j) {
            const uint32_t st = list + 1 + 3 * j;
            Vertex v{};
            v.s = m.F(st, 0);
            v.t = m.F(st, 1);
            v.q = m.F(st, 2);
            v.r = uint8_t(m.W(st + 1, 0));
            v.g = uint8_t(m.W(st + 1, 1));
            v.b = uint8_t(m.W(st + 1, 2));
            v.a = uint8_t(m.W(st + 1, 3));
            v.x = float(uint16_t(m.W(st + 2, 0))) / 16.0f - state.off_x;
            v.y = float(uint16_t(m.W(st + 2, 1))) / 16.0f - state.off_y;
            v.z = float(m.W(st + 2, 2));
            const bool adc = (m.W(st + 2, 3) >> 15) & 1;
            v.draw_kick = adc ? 0 : 1;
            Assemble(queue, parity, v, !adc, draw.verts);
        }
        output += draw.verts.size();
        if (output > SkinGeometry::kMaxVertices * 24) return Result::LimitExceeded;
        if (draw.verts.empty()) continue;
        draw.vertex_count = uint32_t(draw.verts.size());
        result.push_back(std::move(draw));
    }
    out.insert(out.end(), std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
    return Result::Ok;
}
}  // namespace

SkinGeometry::Result SkinGeometry::Build(const Part& part, const Inputs& inputs, const Draw& state,
                                         std::vector<Draw>& out) {
    return BuildBatches(part, inputs, BatchProgram{}, state, out);
}

SkinGeometry::Result SkinGeometry::BuildRuns(const Part& part, const Inputs& inputs, const Draw& state,
                                             std::vector<ps2r::gs::VertexRec>& verts, std::vector<uint32_t>& ends) {
#if defined(__vita__)
    // BuildBatches' checks, then its Vita fast path for a plain skin (a clipped one while the fast paths are on)
    if (!state.tex_enabled || state.use_fst || state.tex.fst || state.ctx || state.fog || state.prim_type != 4 ||
        state.raster.frame.psm || state.raster.zbuf.psm)
        return Result::UnsupportedState;
    if (inputs.joints.empty() || (inputs.joints.size() & 3) || inputs.joints.size() / 4 > kMaxJoints || part.batches.empty())
        return Result::InvalidGeometry;
    if (inputs.clipped && !g_fastPaths.load(std::memory_order_relaxed)) return Result::UnsupportedState;
    verts.clear();
    ends.clear();
    return FastSkinRuns(part, inputs, state, verts, ends);
#else
    (void)part; (void)inputs; (void)state; (void)verts; (void)ends;
    return Result::UnsupportedState;
#endif
}

SkinGeometry::Result SkinGeometry::BuildBlend(const Part& part, const Inputs& inputs, const BlendInputs& blend,
                                              const Draw& state, std::vector<Draw>& out) {
    BatchProgram program;
    program.blend = &blend;
    return BuildBatches(part, inputs, program, state, out);
}

SkinGeometry::Result SkinGeometry::BuildBlendRuns(const Part& part, const Inputs& inputs, const BlendInputs& blend,
                                                  const Draw& state, std::vector<ps2r::gs::VertexRec>& verts,
                                                  std::vector<uint32_t>& ends) {
#if defined(__vita__)
    if (!g_fastPaths.load(std::memory_order_relaxed)) return Result::UnsupportedState;
    if (!state.tex_enabled || state.use_fst || state.tex.fst || state.ctx || state.fog || state.prim_type != 4 ||
        state.raster.frame.psm || state.raster.zbuf.psm)
        return Result::UnsupportedState;
    if (inputs.joints.empty() || (inputs.joints.size() & 3) || inputs.joints.size() / 4 > kMaxJoints || part.batches.empty())
        return Result::InvalidGeometry;
    verts.clear();
    ends.clear();
    return FastSkinRuns(part, inputs, state, verts, ends, &blend);
#else
    (void)part; (void)inputs; (void)blend; (void)state; (void)verts; (void)ends;
    return Result::UnsupportedState;
#endif
}

void SkinGeometry::SetFastPaths(bool on) {
#if defined(__vita__)
    g_fastPaths.store(on, std::memory_order_relaxed);
#else
    (void)on;
#endif
}

}  // namespace title
