#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/graphics/exponent_budget.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

#if (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#endif

#include "engine/platform/host/platform.h"
#include "engine/vu/vu_float.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
using Vector = MeshGeometry::Vector;
using Matrix = MeshGeometry::Matrix;
using Vertex = MeshGeometry::Vertex;
using Draw = ps2r::gs::DrawIr;
using ScreenVertex = ps2r::gs::VertexRec;

uint32_t Bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}
float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
// Numeric contract of the statically identified type-01 and type-A source programs.
// This is value arithmetic, independent of any hardware register file.
float Finite(float value) {
    const uint32_t bits = Bits(value), exponent = bits & 0x7f800000;
    if (!exponent) return Float(bits & 0x80000000);
    if (exponent == 0x7f800000) return Float((bits & 0x80000000) | 0x7f7fffff);
    return value;
}
float Product(float a, float b) { return Finite(Finite(a) * Finite(b)); }
float Sum(float a, float b) { return Finite(Finite(a) + Finite(b)); }
float MultiplyAdd(float a, float b, float c) {
    return Finite(VuFloat::Fma(Finite(a), Finite(b), Finite(c)));
}
float Reciprocal(float value) {
    value = Finite(value);
    if (value == 0) return Float((Bits(value) & 0x80000000) | 0x7f7fffff);
    return Finite(1.0f / value);
}
uint32_t Integer(float value) {
    value = Finite(value);
    if (value >= 2147483648.0f) return 0x7fffffff;
    if (value < -2147483648.0f) return 0x80000000;
    return uint32_t(int32_t(value));
}
Vector Transform(const Matrix& matrix, const Vector& point, bool strip) {
    Vector result;
    // Strip projection is translation,y,z,x; clip and fan projection are
    // translation,x,y,z. Fusing or reassociating them changes actual vertices.
    const std::array<unsigned, 3> order = strip ? std::array<unsigned, 3>{1, 2, 0}
                                               : std::array<unsigned, 3>{0, 1, 2};
    for (unsigned lane = 0; lane != 4; ++lane) {
        float value = Product(matrix[3][lane], 1);
        for (unsigned row : order) value = MultiplyAdd(matrix[row][lane], point[row], value);
        result[lane] = value;
    }
    return result;
}
uint32_t Outside(const Vector& point) {
    // Compare signed float encodings with |w|, including signed zero and the
    // denormal boundary. This preserves the source's six outside-plane bits.
    const uint32_t w = Bits(point[3]);
    const int32_t bound = int32_t((w & 0x7f800000) ? w & 0x7fffffff : 0x007fffff);
    uint32_t result = 0;
    for (unsigned lane = 0; lane != 3; ++lane) {
        const uint32_t bits = Bits(point[lane]);
        int32_t positive, negative;
        std::memcpy(&positive, &bits, 4);
        const uint32_t flipped = bits ^ 0x80000000;
        std::memcpy(&negative, &flipped, 4);
        if (positive > bound) result |= 1u << (lane * 2);
        if (negative > bound) result |= 2u << (lane * 2);
    }
    return result;
}
bool SamePoint(const ScreenVertex& a, const ScreenVertex& b) { return a.x == b.x && a.y == b.y; }
void Triangle(Draw& draw, const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c) {
    if (!SamePoint(a, b) && !SamePoint(b, c) && !SamePoint(a, c))
        draw.verts.insert(draw.verts.end(), {a, b, c});
}
ScreenVertex Project(const Vector& position, const Vector& attributes, const Vector& colour,
                     const MeshGeometry::View& view, const Draw& state, bool strip, bool draw) {
    const Vector clip = Transform(view.screen, position, strip);
    const float reciprocal = Reciprocal(clip[3]);
    ScreenVertex result{};
    result.x = float(Integer(Product(clip[0], reciprocal)) & 0xffff) / 16 - state.off_x;
    result.y = float(Integer(Product(clip[1], reciprocal)) & 0xffff) / 16 - state.off_y;
    volatile uint32_t depth = Integer(Product(clip[2], reciprocal));
    result.z = float(depth);
    result.s = Product(Sum(attributes[0], view.scroll[0]), reciprocal);
    result.t = Product(Sum(attributes[1], view.scroll[1]), reciprocal);
    result.q = Product(strip ? Sum(attributes[2], 0) : attributes[2], reciprocal);
    result.r = uint8_t(Integer(colour[0]));
    result.g = uint8_t(Integer(colour[1]));
    result.b = uint8_t(Integer(colour[2]));
    result.a = uint8_t(Integer(colour[3]));
    result.draw_kick = draw;
    return result;
}
struct Corner { Vector position, weight; };
// A clipped triangle, held in place (the clipper ran per triangle crossing the guard band with a heap allocation per
// plane): three corners, each plane keeping k of n and adding one per sign change (at most 2 min(k, n - k)) -- at most
// 3n/2, so 3 -> 4 -> 6 -> 9 -> 13 -> 19 -> 28 after the six planes whatever the arithmetic does (nine for a convex
// polygon); a copy moves only the corners in use
constexpr unsigned kMaxCorners = 28;
struct Polygon {
    std::array<Corner, kMaxCorners> corners;
    unsigned count = 0;
    Polygon() = default;
    Polygon(const Polygon& o) : count(o.count) { std::copy_n(o.corners.data(), o.count, corners.data()); }
    Polygon& operator=(const Polygon& o) {
        count = o.count;
        std::copy_n(o.corners.data(), o.count, corners.data());
        return *this;
    }
    void push_back(const Corner& corner) { if (count < corners.size()) corners[count++] = corner; }
    bool empty() const { return count == 0; }
    const Corner& back() const { return corners[count - 1]; }
    const Corner* begin() const { return corners.data(); }
    const Corner* end() const { return corners.data() + count; }
    size_t size() const { return count; }
};
Polygon Clip(const std::array<Vertex, 3>& triangle, const Matrix& matrix) {
    Polygon polygon;
    for (unsigned i = 0; i != 3; ++i) {
        Corner corner{{triangle[i].position[0], triangle[i].position[1], triangle[i].position[2], 0},
                      {0, 0, 0, 1}};
        corner.weight[i] = 1;
        polygon.push_back(corner);
    }
    for (unsigned plane = 0; plane != 6 && !polygon.empty(); ++plane) {
        Vector row;
        for (unsigned i = 0; i != 4; ++i)
            row[i] = Sum(matrix[i][3], (plane % 2 ? 1 : -1) * Finite(matrix[i][plane / 2]));
        const auto distance = [&](const Corner& corner) {
            float value = Product(row[0], corner.position[0]);
            value = MultiplyAdd(row[1], corner.position[1], value);
            value = MultiplyAdd(row[2], corner.position[2], value);
            return MultiplyAdd(row[3], 1, value);
        };
        Polygon next;
        Corner previous = polygon.back();
        float previous_distance = distance(previous);
        for (const auto& current : polygon) {
            const float current_distance = distance(current);
            if (std::signbit(previous_distance) != std::signbit(current_distance)) {
                const float reciprocal = Reciprocal(Finite(current_distance - previous_distance));
                Corner intersection;
                for (unsigned lane = 0; lane != 4; ++lane) {
                    intersection.position[lane] = Product(MultiplyAdd(-current.position[lane],
                        previous_distance, Product(previous.position[lane], current_distance)), reciprocal);
                    intersection.weight[lane] = Product(MultiplyAdd(-current.weight[lane],
                        previous_distance, Product(previous.weight[lane], current_distance)), reciprocal);
                }
                next.push_back(intersection);
            }
            if (!std::signbit(current_distance)) next.push_back(current);
            previous = current;
            previous_distance = current_distance;
        }
        polygon = next;
    }
    return polygon;
}
void Fan(std::vector<ScreenVertex>& out, const std::array<Vertex, 3>& triangle, const MeshGeometry::View& view,
         const Draw& state) {
    const Polygon polygon = Clip(triangle, view.clip);
    std::array<ScreenVertex, kMaxCorners> projected;
    size_t projected_count = 0;
    for (const auto& corner : polygon) {
        Vector stq{}, colour{};
        for (unsigned lane = 0; lane != 4; ++lane) {
            float color = Product(float(triangle[0].colour[lane]), corner.weight[0]);
            color = MultiplyAdd(float(triangle[1].colour[lane]), corner.weight[1], color);
            colour[lane] = MultiplyAdd(float(triangle[2].colour[lane]), corner.weight[2], color);
            if (lane < 3) {
                float attribute = Product(triangle[0].stq[lane], corner.weight[0]);
                attribute = MultiplyAdd(triangle[1].stq[lane], corner.weight[1], attribute);
                stq[lane] = MultiplyAdd(triangle[2].stq[lane], corner.weight[2], attribute);
            }
        }
        projected[projected_count++] = Project(corner.position, stq, colour, view, state, false, true);
    }
    for (size_t i = 2; i < projected_count; ++i)
        out.insert(out.end(), {projected[0], projected[i - 1], projected[i]});
}
void Fan(Draw& draw, const std::array<Vertex, 3>& triangle, const MeshGeometry::View& view) {
    Fan(draw.verts, triangle, view, draw);
}
void Finish(Draw& draw) {
    draw.vertex_count = uint32_t(draw.verts.size());
    draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
    draw.snapshot.verts_incl = 1;
    ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
}
}  // namespace

MeshGeometry::Result MeshGeometry::Decode(const uint8_t* bytes, size_t size,
                                        uint32_t vertices, Mesh& out, Layout layout) {
    if (!bytes || size < 16 || size > 4 * 1024 * 1024 || vertices < 3)
        return Result::InvalidAsset;
    if (vertices > kMaxVertices) return Result::LimitExceeded;
    size_t cursor = 0, limit = size;
    const auto read = [&](uint32_t& value) {
        if (cursor > limit || limit - cursor < 4) return false;
        value = uint32_t(bytes[cursor]) | uint32_t(bytes[cursor + 1]) << 8 |
                uint32_t(bytes[cursor + 2]) << 16 | uint32_t(bytes[cursor + 3]) << 24;
        cursor += 4;
        return true;
    };
    const auto expect = [&](uint32_t value) { uint32_t actual; return read(actual) && actual == value; };
    uint32_t header;
    if (!read(header) || (header >> 16) != 0x6000 || !expect(0) || !expect(0x070000fa) || !expect(0))
        return Result::InvalidAsset;
    limit = 16 + size_t(header & 0xffff) * 16;
    if (limit > size || size - limit > 36) return Result::InvalidAsset;
    for (size_t i = limit; i < size; ++i) if (bytes[i]) return Result::InvalidAsset;
    Mesh mesh;
    uint32_t total = 0;
    while (cursor < limit) {
        uint32_t command;
        if (!read(command)) return Result::InvalidAsset;
        if (command == 0) {
            for (size_t i = cursor; i < limit; ++i) if (bytes[i]) return Result::InvalidAsset;
            break;
        }
        if (command != 0x6c018000) return Result::InvalidAsset;
        uint32_t count;
        if (!read(count) || (count & ~0xffu) != 0x8000) return Result::InvalidAsset;
        count &= 255;
        if (count < 3 || count > 38 || total + count > vertices) return Result::InvalidAsset;
        if (!expect(0x30024000) || !expect(0x512) || !expect(0) ||
            !expect(0x01000101) || !expect(0x64018001) || !expect(count * 4) ||
            !expect(count | 0x8000) || !expect(0x01000104) ||
            !expect(0x68008003 | count << 16)) return Result::InvalidAsset;
        Strip strip;
        strip.vertices.resize(count);
        for (auto& vertex : strip.vertices)
            for (float& coordinate : vertex.position) {
                uint32_t bits;
                if (!read(bits) || !std::isfinite(coordinate = Float(bits))) return Result::InvalidAsset;
            }
        if (!expect(0x6c008004 | count << 16)) return Result::InvalidAsset;
        for (auto& vertex : strip.vertices)
            for (unsigned lane = 0; lane != 4; ++lane) {
                uint32_t bits;
                if (!read(bits)) return Result::InvalidAsset;
                vertex.colour[lane] = uint8_t(bits);
                if (lane < 3) {
                    if (!std::isfinite(vertex.stq[lane] = Float(bits))) return Result::InvalidAsset;
                } else vertex.restart = bits & 0x8000;
            }
        uint32_t end;
        if (!read(end)) return Result::InvalidAsset;
        if (layout == Layout::Unlit) {
            // Shared authored models can carry normals and a separate packed
            // colour stream for other materials. Type01 never reads those
            // fields; its colour is the low byte of each STQ/ADC word above.
            // Accept only these two enumerated record shapes, once/in order.
            for (const auto& optional : std::array<std::array<uint32_t, 2>, 2>{{
                     {0x68008005u | count << 16, count * 3},
                     {0x6e00c006u | count << 16, count}}}) {
                if (end != optional[0]) continue;
                const size_t bytes = size_t(optional[1]) * 4;
                if (cursor > limit || bytes > limit - cursor) return Result::InvalidAsset;
                cursor += bytes;
                if (!read(end)) return Result::InvalidAsset;
            }
        }
        if (end != 0x14000000 || !expect(0x01000101)) return Result::InvalidAsset;
        total += count;
        mesh.strips.push_back(std::move(strip));
    }
    if (total != vertices) return Result::InvalidAsset;
    out = std::move(mesh);
    return Result::Ok;
}

MeshGeometry::Result MeshGeometry::Build(const Mesh& mesh, const std::vector<View>& instances, const Draw& state,
                                       std::vector<Draw>& out) {
    if (!state.tex_enabled || state.use_fst || state.tex.fst || state.ctx || state.fog ||
        state.raster.frame.psm || state.raster.zbuf.psm || state.prim_type != 4 ||
        !std::isfinite(state.off_x) || !std::isfinite(state.off_y))
        return Result::UnsupportedState;
    if (instances.empty() || instances.size() > 20) return Result::InvalidGeometry;
    for (const auto& view : instances) {
    for (const auto* matrix : {&view.screen, &view.clip})
        for (const auto& row : *matrix)
            for (float value : row) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (float value : view.guard) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (float value : view.scroll) if (!std::isfinite(value)) return Result::InvalidGeometry;
    }
    RoundingGuard rounding;
    size_t count = 0, output_count = 0;
    std::vector<Draw> result;
    for (const auto& strip : mesh.strips) {
        const size_t size = strip.vertices.size();
        count += size;
        if (size < 3 || size > 38) return Result::InvalidGeometry;
        if (count > kMaxVertices) return Result::LimitExceeded;
        for (const auto& view : instances) {
        Draw body = state;
        body.verts.clear();
        const auto publish = [&](Draw& draw) {
            output_count += draw.verts.size();
            if (output_count > kMaxOutputVertices) return false;
            if (!draw.verts.empty()) {
                Finish(draw);
                result.push_back(std::move(draw));
            }
            return true;
        };
        std::array<uint32_t, 3> hard{}, guard{};
        std::array<ScreenVertex, 38> projected{};
        unsigned parity = 0;
        for (size_t i = 0; i < size; ++i) {
            const auto& vertex = strip.vertices[i];
            Vector point{vertex.position[0], vertex.position[1], vertex.position[2], 1};
            for (float value : point) if (!std::isfinite(value)) return Result::InvalidGeometry;
            for (float value : vertex.stq) if (!std::isfinite(value)) return Result::InvalidGeometry;
            Vector clipped = Transform(view.clip, point, false), guarded;
            for (unsigned lane = 0; lane != 4; ++lane) guarded[lane] = Product(clipped[lane], view.guard[lane]);
            hard[i % 3] = Outside(clipped);
            guard[i % 3] = Outside(guarded);
            bool emit = !vertex.restart;
            if (view.clipped && i >= 2) {
                if (hard[0] & hard[1] & hard[2]) emit = false;
                else if (guard[0] | guard[1] | guard[2]) {
                    if (emit) {
                        Draw fan = state;
                        fan.prim_type = 5;
                        fan.verts.clear();
                        Fan(fan, {strip.vertices[i - 2], strip.vertices[i - 1], vertex}, view);
                        if (!publish(fan)) return Result::LimitExceeded;
                    }
                    emit = false;
                }
            }
            projected[i] = Project(point, {vertex.stq[0], vertex.stq[1], vertex.stq[2], 0},
                {float(vertex.colour[0]), float(vertex.colour[1]), float(vertex.colour[2]),
                 float(vertex.colour[3])}, view, body, true, emit);
            if (!emit) parity = 0;
            else if (i >= 2) {
                Triangle(body, projected[i - 2 + (parity & 1)], projected[i - 1 - (parity & 1)], projected[i]);
                ++parity;
            }
        }
        if (!publish(body)) return Result::LimitExceeded;
        }
    }
    out = std::move(result);
    return Result::Ok;
}

// ---- prepared builds -----------------------------------------------------------------------------------------
// Every operation below is one of Build's, on operands Build would have flushed on the way in (Finite is
// idempotent, Finite(x) * 1 == Finite(x), so flushing the mesh and the matrices once and then applying the
// same multiply-adds in the same order yields the same bits). The lane-parallel transform is the same fused
// multiply-add per lane (the AArch64 SIMD unit rounds by FPCR like the scalar one; the rounding guard
// covers both) with the flush as integer selects.
namespace {
// AArch64 only: ARMv7's NEON has no fused multiply-add (vfmaq_n_f32), so the 32-bit ARM targets (the Vita) take the
// scalar path, which is the same fused arithmetic per lane.
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && defined(__aarch64__)
#define TITLE_MESH_NEON 1
#else
#define TITLE_MESH_NEON 0
#endif

inline Vector FlushVector(const Vector& v) { return {Finite(v[0]), Finite(v[1]), Finite(v[2]), Finite(v[3])}; }
inline Matrix FlushMatrix(const Matrix& m) {
    return {FlushVector(m[0]), FlushVector(m[1]), FlushVector(m[2]), FlushVector(m[3])};
}

// ARMv7 (the Vita): the lane-parallel transform on request (MeshGeometry::SetVectorTransform). ARMv7's Advanced SIMD has
// no fused multiply-add but neither does the Vita's scalar path (VuFloat::Fma is the product and the sum rounded apart
// there); its unit always rounds to nearest (the scalar path runs under the rounding guard's toward zero), so a lane may
// differ from the scalar one by a unit in the last place -- a 1/16 pixel projection keeps the picture.
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(__aarch64__) && defined(__vita__)
#define TITLE_MESH_NEON_V7 1
#else
#define TITLE_MESH_NEON_V7 0
#endif
bool g_vector_transform = false;

#if TITLE_MESH_NEON || TITLE_MESH_NEON_V7
inline float32x4_t FiniteLanes(float32x4_t value) {
    const uint32x4_t bits = vreinterpretq_u32_f32(value);
    const uint32x4_t exponent = vandq_u32(bits, vdupq_n_u32(0x7f800000u));
    const uint32x4_t sign = vandq_u32(bits, vdupq_n_u32(0x80000000u));
    const uint32x4_t zero_exponent = vceqq_u32(exponent, vdupq_n_u32(0));
    const uint32x4_t full_exponent = vceqq_u32(exponent, vdupq_n_u32(0x7f800000u));
    const uint32x4_t saturated = vorrq_u32(sign, vdupq_n_u32(0x7f7fffffu));
    const uint32x4_t result = vbslq_u32(zero_exponent, sign, vbslq_u32(full_exponent, saturated, bits));
    return vreinterpretq_f32_u32(result);
}
#endif

// Transform on flushed operands: value = Finite(m[3]); for row in order: value = Finite(fma(m[row], p[row], value))
inline Vector TransformFlushed(const Matrix& matrix, const Vector& point, bool strip) {
    const std::array<unsigned, 3> order = strip ? std::array<unsigned, 3>{1, 2, 0}
                                               : std::array<unsigned, 3>{0, 1, 2};
#if TITLE_MESH_NEON
    float32x4_t value = vld1q_f32(matrix[3].data());
    for (unsigned row : order)
        value = FiniteLanes(vfmaq_n_f32(value, vld1q_f32(matrix[row].data()), point[row]));
    Vector result;
    vst1q_f32(result.data(), value);
    return result;
#else
#if TITLE_MESH_NEON_V7
    if (g_vector_transform) {
        float32x4_t value = vld1q_f32(matrix[3].data());
        for (unsigned row : order) value = FiniteLanes(vmlaq_n_f32(value, vld1q_f32(matrix[row].data()), point[row]));
        Vector result;
        vst1q_f32(result.data(), value);
        return result;
    }
#endif
    Vector result;
    for (unsigned lane = 0; lane != 4; ++lane) {
        float value = matrix[3][lane];
        for (unsigned row : order) value = Finite(VuFloat::Fma(matrix[row][lane], point[row], value));
        result[lane] = value;
    }
    return result;
#endif
}

// Project on a transformed (flushed) point: Build's Project with the colour bytes passed through (Integer(float(b)) == b)
inline ScreenVertex ProjectFlushed(const Vector& clip, const Vector& stq, const std::array<uint8_t, 4>& colour,
                                   const Vector& scroll, const Draw& state, bool draw) {
    const float reciprocal = Reciprocal(clip[3]);
    ScreenVertex result{};
    result.x = float(Integer(Finite(clip[0] * reciprocal)) & 0xffff) / 16 - state.off_x;
    result.y = float(Integer(Finite(clip[1] * reciprocal)) & 0xffff) / 16 - state.off_y;
    volatile uint32_t depth = Integer(Finite(clip[2] * reciprocal));
    result.z = float(depth);
    result.s = Finite(Finite(stq[0] + scroll[0]) * reciprocal);
    result.t = Finite(Finite(stq[1] + scroll[1]) * reciprocal);
    result.q = Finite(Finite(stq[2] + 0.0f) * reciprocal);
    result.r = colour[0];
    result.g = colour[1];
    result.b = colour[2];
    result.a = colour[3];
    result.draw_kick = draw;
    return result;
}

// ---- the two-pass build (MeshGeometry::SetCertified) ----------------------------------------------------------
int g_certified = 1;                         // MeshGeometry::SetCertified
// builds clamp-free, builds clamped, vertices that differed (mode 2), builds compared with the one-pass loop and those
// that differed (mode 4)
std::atomic<uint64_t> g_certified_census[5] = {};
#if defined(__vita__)
// The first pass: each vertex's values as the one-pass loop computes them (the outcodes of its clip transform and guard
// scaling when the view is clipped; its projected vertex, the draw bit left for the second pass)
struct VertexValues {
    std::vector<ScreenVertex> projected;
    std::vector<uint8_t> hard, guarded;
};
void VertexPassClamped(const MeshGeometry::Prepared& prepared, bool clipped_view, const Matrix& screen, const Matrix& clip,
                       const Vector& guard, const Vector& scroll, const std::array<uint8_t, 4>* colours, const Draw& state,
                       VertexValues& out) {
    for (size_t at = 0; at < prepared.vertex_count; ++at) {
        const Vector& point = prepared.positions[at];
        const std::array<uint8_t, 4>& colour = colours ? colours[at] : prepared.colours[at];
        if (clipped_view) {
            const Vector clipped = TransformFlushed(clip, point, false);
            Vector banded;
            for (unsigned lane = 0; lane != 4; ++lane) banded[lane] = Finite(clipped[lane] * guard[lane]);
            out.hard[at] = uint8_t(Outside(clipped));
            out.guarded[at] = uint8_t(Outside(banded));
        }
        out.projected[at] = ProjectFlushed(TransformFlushed(screen, point, true), prepared.stq[at], colour, scroll, state, false);
    }
}
#if defined(__vita__)
// The same operations without the clamps (exponent_budget.h): every operand is flushed already -- Prepare's positions and
// STQ, the build's matrices, guard and scroll. Taken when the build's budget holds (PlainBudget: every value of the two
// transforms' chains, the guard products, the reciprocals and the S / T / Q sums a zero or a normal number) and the pass's
// stored S, T, Q -- the only results no budget bounds that are not taken to an integer -- pass the audit (PlainAudit). The
// integer screen position and depth need neither: Integer truncates a denormal to 0 as it truncates the signed zero its
// clamp makes of it, and under round toward zero nothing overflows to an infinity (an overflow rounds to FLT_MAX, which
// the clamp keeps).
bool PlainBudget(const MeshGeometry::Prepared& prepared, bool clipped_view, const Matrix& screen, const Matrix& clip,
                 const Vector& guard, const Vector& scroll) {
    ExponentRange position, stq, ms, mc, g, sc;
    for (size_t at = 0; at < prepared.vertex_count; ++at) {
        position.Add(prepared.positions[at].data(), 3);
        stq.Add(prepared.stq[at].data(), 3);
    }
    ms.Add(&screen[0][0], 16);
    mc.Add(&clip[0][0], 16);
    g.Add(guard.data(), 4);
    sc.Add(scroll.data(), 2);
    const ChainBudget toScreen(ms, position);
    const ChainBudget toClip(mc, position);
    // S + scroll, T + scroll, Q + 0: a nonzero sum of two values of exponents >= L is >= 2^(L - 23)
    ExponentRange sums = stq;
    sums.Add(sc);
    const bool screenOk = toScreen.ok && toScreen.ReciprocalOk();
    const bool clipOk = !clipped_view || (toClip.ok && toClip.ScaledOk(g));
    const bool sumsOk = sums.AllZero() || (sums.lo >= -103 && sums.hi <= 124);
    if (!(screenOk && clipOk && sumsOk)) {
        // diagnosis (TE_GFX_BUDGET_LOG=N): the first N budgets that failed, with their ranges
        static int left = [] { const char* v = std::getenv("TE_GFX_BUDGET_LOG"); return v ? std::atoi(v) : 0; }();
        if (left > 0) {
            --left;
            std::fprintf(stderr, "[te.gfx] mesh budget failed (%u vertices): screen matrix %d..%d, clip matrix %d..%d, positions %d..%d, "
                         "guard %d..%d, stq + scroll %d..%d -> screen %d clip %d sums %d\n", unsigned(prepared.vertex_count), ms.lo, ms.hi,
                         mc.lo, mc.hi, position.lo, position.hi, g.lo, g.hi, sums.lo, sums.hi, int(screenOk), int(clipOk), int(sumsOk));
        }
        return false;
    }
    return true;
}
bool PlainAudit(const std::vector<ScreenVertex>& projected, size_t count) {
    static_assert(sizeof(ScreenVertex) % 4 == 0 && offsetof(ScreenVertex, s) == 12 && offsetof(ScreenVertex, t) == 16 &&
                  offsetof(ScreenVertex, q) == 20, "S, T and Q: three floats at word 3 of the record");
    if (!count) return true;
    const float* first = &projected[0].s;
    const size_t stride = sizeof(ScreenVertex) / 4;
    return AllClampFree(first, count, stride) && AllClampFree(first + 1, count, stride) && AllClampFree(first + 2, count, stride);
}
inline uint32_t IntegerPlain(float value) {
    if (value >= 2147483648.0f) return 0x7fffffff;
    if (value < -2147483648.0f) return 0x80000000;
    return uint32_t(int32_t(value));
}
void VertexPassPlain(const MeshGeometry::Prepared& prepared, bool clipped_view, const Matrix& screen, const Matrix& clip,
                     const Vector& guard, const Vector& scroll, const std::array<uint8_t, 4>* colours, const Draw& state,
                     VertexValues& out) {
    static constexpr unsigned kClipOrder[3] = {0, 1, 2}, kStripOrder[3] = {1, 2, 0};   // TransformFlushed's two orders
    for (size_t at = 0; at < prepared.vertex_count; ++at) {
        const Vector& point = prepared.positions[at];
        const std::array<uint8_t, 4>& colour = colours ? colours[at] : prepared.colours[at];
        if (clipped_view) {
            Vector clipped, banded;
            for (unsigned lane = 0; lane != 4; ++lane) {
                float value = clip[3][lane];
                for (unsigned row : kClipOrder) value = VuFloat::Fma(clip[row][lane], point[row], value);
                clipped[lane] = value;
                banded[lane] = value * guard[lane];
            }
            out.hard[at] = uint8_t(Outside(clipped));
            out.guarded[at] = uint8_t(Outside(banded));
        }
        Vector c;
        for (unsigned lane = 0; lane != 4; ++lane) {
            float value = screen[3][lane];
            for (unsigned row : kStripOrder) value = VuFloat::Fma(screen[row][lane], point[row], value);
            c[lane] = value;
        }
        const float reciprocal = c[3] == 0 ? Float((Bits(c[3]) & 0x80000000) | 0x7f7fffff) : 1.0f / c[3];
        const Vector& stq = prepared.stq[at];
        ScreenVertex result{};
        result.x = float(IntegerPlain(c[0] * reciprocal) & 0xffff) / 16 - state.off_x;
        result.y = float(IntegerPlain(c[1] * reciprocal) & 0xffff) / 16 - state.off_y;
        volatile uint32_t depth = IntegerPlain(c[2] * reciprocal);
        result.z = float(depth);
        result.s = (stq[0] + scroll[0]) * reciprocal;
        result.t = (stq[1] + scroll[1]) * reciprocal;
        result.q = (stq[2] + 0.0f) * reciprocal;
        result.r = colour[0];
        result.g = colour[1];
        result.b = colour[2];
        result.a = colour[3];
        result.draw_kick = false;
        out.projected[at] = result;
    }
}
#endif
// The second pass: BuildPrepared's strip loop on the first pass's values (its emit / fan / assembly logic unchanged)
void StripPass(const MeshGeometry::Prepared& prepared, const MeshGeometry::View& view, const Draw& state,
               const std::array<uint8_t, 4>* colours, const VertexValues& values, MeshGeometry::Sink sink, void* user) {
    std::array<ScreenVertex, 38 * 3> body;
    std::vector<ScreenVertex> fan;
    for (const auto& range : prepared.strips) {
        const size_t first = range.first, size = range.count;
        size_t body_count = 0;
        std::array<uint32_t, 3> hard{}, guarded{};
        std::array<ScreenVertex, 38> projected;
        unsigned parity = 0;
        for (size_t i = 0; i < size; ++i) {
            const size_t at = first + i;
            bool emit = !prepared.restart[at];
            if (view.clipped) {
                hard[i % 3] = values.hard[at];
                guarded[i % 3] = values.guarded[at];
                if (i >= 2) {
                    if (hard[0] & hard[1] & hard[2]) emit = false;
                    else if (guarded[0] | guarded[1] | guarded[2]) {
                        if (emit) {
                            std::array<Vertex, 3> triangle;
                            for (unsigned k = 0; k != 3; ++k) {
                                const size_t v = at - 2 + k;
                                triangle[k].position = {prepared.positions[v][0], prepared.positions[v][1], prepared.positions[v][2]};
                                triangle[k].stq = {prepared.stq[v][0], prepared.stq[v][1], prepared.stq[v][2]};
                                triangle[k].colour = colours ? colours[v] : prepared.colours[v];
                                triangle[k].restart = prepared.restart[v] != 0;
                            }
                            fan.clear();
                            Fan(fan, triangle, view, state);
                            if (!fan.empty()) sink(user, fan.data(), fan.size(), true);
                        }
                        emit = false;
                    }
                }
            }
            projected[i] = values.projected[at];
            projected[i].draw_kick = emit;
            if (!emit) parity = 0;
            else if (i >= 2) {
                const ScreenVertex& a = projected[i - 2 + (parity & 1)];
                const ScreenVertex& b = projected[i - 1 - (parity & 1)];
                const ScreenVertex& c = projected[i];
                if (!SamePoint(a, b) && !SamePoint(b, c) && !SamePoint(a, c)) {
                    body[body_count++] = a;
                    body[body_count++] = b;
                    body[body_count++] = c;
                }
                ++parity;
            }
        }
        if (body_count) sink(user, body.data(), body_count, false);
    }
}
// The two-pass build (BuildPrepared's, the view and state validated, the operands flushed by the caller)
void BuildTwoPass(const MeshGeometry::Prepared& prepared, const MeshGeometry::View& view, const Draw& state, const Matrix& screen,
                  const Matrix& clip, const Vector& guard, const Vector& scroll, const std::array<uint8_t, 4>* colours,
                  MeshGeometry::Sink sink, void* user, int mode) {
    thread_local VertexValues values;
    values.projected.resize(prepared.vertex_count);
    if (view.clipped) {
        values.hard.resize(prepared.vertex_count);
        values.guarded.resize(prepared.vertex_count);
    }
    bool certified = false;
    if ((mode == 1 || mode == 2) && PlainBudget(prepared, view.clipped, screen, clip, guard, scroll)) {
        VertexPassPlain(prepared, view.clipped, screen, clip, guard, scroll, colours, state, values);
        certified = PlainAudit(values.projected, prepared.vertex_count);
        if (mode == 2 && certified) {
            // diagnosis: the clamped forms too, compared vertex by vertex
            const VertexValues plain = values;
            VertexPassClamped(prepared, view.clipped, screen, clip, guard, scroll, colours, state, values);
            uint64_t differ = 0;
            for (size_t at = 0; at < prepared.vertex_count; ++at) {
                bool same = std::memcmp(&plain.projected[at], &values.projected[at], sizeof(ScreenVertex)) == 0;
                if (view.clipped) same = same && plain.hard[at] == values.hard[at] && plain.guarded[at] == values.guarded[at];
                differ += same ? 0 : 1;
            }
            g_certified_census[2].fetch_add(differ, std::memory_order_relaxed);
        }
    }
    if (!certified) VertexPassClamped(prepared, view.clipped, screen, clip, guard, scroll, colours, state, values);
    g_certified_census[certified ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
    StripPass(prepared, view, state, colours, values, sink, user);
}
#endif
}  // namespace

void MeshGeometry::SetCertified(int mode) { g_certified = mode; }
void MeshGeometry::CertifiedCensus(uint64_t out[5]) {
    for (int k = 0; k < 5; ++k) out[k] = g_certified_census[k].exchange(0, std::memory_order_relaxed);
}

void MeshGeometry::SetVectorTransform(bool on) { g_vector_transform = on && TITLE_MESH_NEON_V7; }
bool MeshGeometry::VectorTransform() { return g_vector_transform; }

MeshGeometry::Result MeshGeometry::Prepare(const Mesh& mesh, Prepared& out) {
    Prepared result;
    size_t count = 0;
    for (const auto& strip : mesh.strips) {
        const size_t size = strip.vertices.size();
        count += size;
        if (size < 3 || size > 38) return Result::InvalidGeometry;
        if (count > kMaxVertices) return Result::LimitExceeded;
        for (const auto& vertex : strip.vertices) {
            for (float value : vertex.position) if (!std::isfinite(value)) return Result::InvalidGeometry;
            for (float value : vertex.stq) if (!std::isfinite(value)) return Result::InvalidGeometry;
        }
    }
    result.strips.reserve(mesh.strips.size());
    result.positions.reserve(count);
    result.stq.reserve(count);
    result.colours.reserve(count);
    result.restart.reserve(count);
    for (const auto& strip : mesh.strips) {
        result.strips.push_back({uint32_t(result.positions.size()), uint32_t(strip.vertices.size())});
        for (const auto& vertex : strip.vertices) {
            result.positions.push_back(FlushVector({vertex.position[0], vertex.position[1], vertex.position[2], 1}));
            result.stq.push_back(FlushVector({vertex.stq[0], vertex.stq[1], vertex.stq[2], 0}));
            result.colours.push_back(vertex.colour);
            result.restart.push_back(vertex.restart ? 1 : 0);
        }
    }
    result.vertex_count = count;
    out = std::move(result);
    return Result::Ok;
}

namespace {
// BuildPrepared's one-pass loop (the view and state validated, the operands flushed by the caller)
void BuildOnePass(const MeshGeometry::Prepared& prepared, const MeshGeometry::View& view, const Draw& state, const Matrix& screen,
                  const Matrix& clip, const Vector& guard, const Vector& scroll, const std::array<uint8_t, 4>* colours,
                  MeshGeometry::Sink sink, void* user) {
    std::array<ScreenVertex, 38 * 3> body;
    std::vector<ScreenVertex> fan;
    for (const auto& range : prepared.strips) {
        const size_t first = range.first, size = range.count;
        size_t body_count = 0;
        std::array<uint32_t, 3> hard{}, guarded{};
        std::array<ScreenVertex, 38> projected;
        unsigned parity = 0;
        for (size_t i = 0; i < size; ++i) {
            const size_t at = first + i;
            const Vector& point = prepared.positions[at];
            const std::array<uint8_t, 4>& colour = colours ? colours[at] : prepared.colours[at];
            bool emit = !prepared.restart[at];
            if (view.clipped) {
                const Vector clipped = TransformFlushed(clip, point, false);
                Vector banded;
                for (unsigned lane = 0; lane != 4; ++lane) banded[lane] = Finite(clipped[lane] * guard[lane]);
                hard[i % 3] = Outside(clipped);
                guarded[i % 3] = Outside(banded);
                if (i >= 2) {
                    if (hard[0] & hard[1] & hard[2]) emit = false;
                    else if (guarded[0] | guarded[1] | guarded[2]) {
                        if (emit) {
                            // The clipped triangle's fan, from the mesh's vertices (the flushed values are what
                            // Clip's and Project's own flushes would have made of the originals)
                            std::array<Vertex, 3> triangle;
                            for (unsigned k = 0; k != 3; ++k) {
                                const size_t v = at - 2 + k;
                                triangle[k].position = {prepared.positions[v][0], prepared.positions[v][1], prepared.positions[v][2]};
                                triangle[k].stq = {prepared.stq[v][0], prepared.stq[v][1], prepared.stq[v][2]};
                                triangle[k].colour = colours ? colours[v] : prepared.colours[v];
                                triangle[k].restart = prepared.restart[v] != 0;
                            }
                            fan.clear();
                            Fan(fan, triangle, view, state);
                            if (!fan.empty()) sink(user, fan.data(), fan.size(), true);
                        }
                        emit = false;
                    }
                }
            }
            projected[i] = ProjectFlushed(TransformFlushed(screen, point, true), prepared.stq[at], colour, scroll, state, emit);
            if (!emit) parity = 0;
            else if (i >= 2) {
                const ScreenVertex& a = projected[i - 2 + (parity & 1)];
                const ScreenVertex& b = projected[i - 1 - (parity & 1)];
                const ScreenVertex& c = projected[i];
                if (!SamePoint(a, b) && !SamePoint(b, c) && !SamePoint(a, c)) {
                    body[body_count++] = a;
                    body[body_count++] = b;
                    body[body_count++] = c;
                }
                ++parity;
            }
        }
        if (body_count) sink(user, body.data(), body_count, false);
    }
}
}  // namespace

MeshGeometry::Result MeshGeometry::BuildPrepared(const Prepared& prepared, const View& view, const Draw& state,
                                                const std::array<uint8_t, 4>* colours, Sink sink, void* user) {
    if (!state.tex_enabled || state.use_fst || state.tex.fst || state.ctx || state.fog ||
        state.raster.frame.psm || state.raster.zbuf.psm || state.prim_type != 4 ||
        !std::isfinite(state.off_x) || !std::isfinite(state.off_y))
        return Result::UnsupportedState;
    for (const auto* matrix : {&view.screen, &view.clip})
        for (const auto& row : *matrix)
            for (float value : row) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (float value : view.guard) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (float value : view.scroll) if (!std::isfinite(value)) return Result::InvalidGeometry;
    if (!sink) return Result::InvalidGeometry;
    RoundingGuard rounding;
    const Matrix screen = FlushMatrix(view.screen), clip = FlushMatrix(view.clip);
    const Vector guard = FlushVector(view.guard);
    const Vector scroll{Finite(view.scroll[0]), Finite(view.scroll[1]), 0, 0};
#if defined(__vita__)
    {
        // the two-pass build: the clamp-free arithmetic where the exponent budget and the audit allow it (mode 1;
        // 2 both, compared), mode 3 the two passes on the clamped forms (the structure alone)
        const int mode = g_certified;
        if (mode == 4 && !g_vector_transform) {
            // diagnosis: the two passes (clamp-free where allowed) and the one-pass loop, their sink calls compared; the
            // one-pass output is the build's
            struct Capture {
                std::vector<ScreenVertex> verts;
                std::vector<std::pair<uint32_t, bool>> calls;
                static void Sink(void* u, const ScreenVertex* v, size_t n, bool fan) {
                    auto* c = static_cast<Capture*>(u);
                    c->verts.insert(c->verts.end(), v, v + n);
                    c->calls.emplace_back(uint32_t(n), fan);
                }
            };
            thread_local Capture two, one;
            two.verts.clear();
            two.calls.clear();
            one.verts.clear();
            one.calls.clear();
            BuildTwoPass(prepared, view, state, screen, clip, guard, scroll, colours, &Capture::Sink, &two, 1);
            BuildOnePass(prepared, view, state, screen, clip, guard, scroll, colours, &Capture::Sink, &one);
            const bool same = two.calls == one.calls && two.verts.size() == one.verts.size() &&
                              (one.verts.empty() || std::memcmp(two.verts.data(), one.verts.data(), one.verts.size() * sizeof(ScreenVertex)) == 0);
            g_certified_census[3].fetch_add(1, std::memory_order_relaxed);
            if (!same) g_certified_census[4].fetch_add(1, std::memory_order_relaxed);
            size_t at = 0;
            for (const auto& call : one.calls) {
                sink(user, one.verts.data() + at, call.first, call.second);
                at += call.first;
            }
            return Result::Ok;
        }
        if (mode && !g_vector_transform) {
            BuildTwoPass(prepared, view, state, screen, clip, guard, scroll, colours, sink, user, mode);
            return Result::Ok;
        }
    }
#endif
    BuildOnePass(prepared, view, state, screen, clip, guard, scroll, colours, sink, user);
    return Result::Ok;
}

}  // namespace title
