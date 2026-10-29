#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "games/slus_20909_crash_twinsanity/hle/graphics/exponent_budget.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

#include "engine/platform/host/platform.h"
#include "engine/vu/vu_float.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
using Vector = ShadowGeometry::Vector;
using Matrix = ShadowGeometry::Matrix;
using Vertex = ShadowGeometry::Vertex;
using Draw = ps2r::gs::DrawIr;
using Screen = ps2r::gs::VertexRec;
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
float Float(uint32_t bits) { float value; std::memcpy(&value, &bits, 4); return value; }
float Finite(float value) {
    const uint32_t bits = Bits(value), exponent = bits & 0x7f800000;
    if (!exponent) return Float(bits & 0x80000000);
    return exponent == 0x7f800000 ? Float((bits & 0x80000000) | 0x7f7fffff) : value;
}
float Product(float a, float b) { return Finite(Finite(a) * Finite(b)); }
float Add(float a, float b) { return Finite(Finite(a) + Finite(b)); }
float Madd(float a, float b, float c) { return Finite(VuFloat::Fma(Finite(a), Finite(b), Finite(c))); }
float Reciprocal(float value) {
    value = Finite(value);
    return value == 0 ? Float((Bits(value) & 0x80000000) | 0x7f7fffff) : Finite(1.f / value);
}
uint32_t Integer(float value) {
    value = Finite(value);
    if (value >= 2147483648.f) return 0x7fffffff;
    if (value < -2147483648.f) return 0x80000000;
    return uint32_t(int32_t(value));
}
Vector Transform(const Matrix& matrix, const Vector& position, bool strip) {
    Vector result;
    const std::array<unsigned, 3> order = strip ? std::array<unsigned, 3>{1, 2, 0}
                                               : std::array<unsigned, 3>{0, 1, 2};
    for (unsigned lane = 0; lane < 4; ++lane) {
        float value = Product(matrix[3][lane], 1);
        for (unsigned row : order) value = Madd(matrix[row][lane], position[row], value);
        result[lane] = value;
    }
    return result;
}
Vector Project(const Matrix& matrix, const Vector& position, bool strip) {
    auto value = Transform(matrix, position, strip);
    const float inverse = Reciprocal(value[3]);
    for (unsigned lane = 0; lane < 3; ++lane) value[lane] = Product(value[lane], inverse);
    return value;
}
Vector Difference(const Vector& a, const Vector& b) {
    Vector result{};
    for (unsigned lane = 0; lane < 3; ++lane) result[lane] = Add(a[lane], -Finite(b[lane]));
    return result;
}
bool Reverse(const Vector& a, const Vector& b) {
    // Winding tests the float cross product BEFORE quantized x/y projection.
    return std::signbit(Madd(-Finite(b[0]), a[1], Product(a[0], b[1])));
}
uint32_t Outside(const Vector& point) {
    const uint32_t wb = Bits(point[3]);
    const int32_t bound = int32_t(wb & 0x7f800000 ? wb & 0x7fffffff : 0x007fffff);
    uint32_t result = 0;
    for (unsigned lane = 0; lane < 3; ++lane) {
        const uint32_t bits = Bits(point[lane]), flipped = bits ^ 0x80000000;
        int32_t positive, negative;
        std::memcpy(&positive, &bits, 4);
        std::memcpy(&negative, &flipped, 4);
        if (positive > bound) result |= 1u << (lane * 2);
        if (negative > bound) result |= 2u << (lane * 2);
    }
    return result;
}
Vector Position(const Vertex& vertex) {
    return {vertex.position[0], vertex.position[1], vertex.position[2], 1};
}
Screen Quantize(const Vector& position, const ShadowGeometry::View& view, uint8_t colour, bool emit) {
    Screen result{};
    result.x = float(Integer(position[0]) & 0xffff) / 16 - view.offset[0];
    result.y = float(Integer(position[1]) & 0xffff) / 16 - view.offset[1];
    volatile uint32_t depth = Integer(Add(0, -Finite(position[2])));
    result.z = float(depth);
    result.r = result.g = result.b = colour;
    result.a = 128;
    result.q = 1;
    result.draw_kick = emit;
    return result;
}
// The strip path's arithmetic over operands already clamped (the outputs of Finite, or inputs clamped once: the view's
// matrices and guard per build, a vertex's position per vertex). Finite is idempotent and the identity on its own
// outputs, so each form below is the plain one's value with the operand clamps that cannot change anything left out:
//   Transform: Product(m[3], 1) = m[3]; Madd(m, p, value) = Finite(Fma(m, p, value))
//   Project:   Product(value, inverse) = Finite(value * inverse); Reciprocal of a clamped value
//   Difference: Finite(a + -b);  Reverse: the sign of Finite(Fma(-b0, a1, Finite(a0 * b1)))
//   Quantize: Integer's own clamp of a clamped value
struct Clamped {
    Matrix screen{}, clip{};
    Vector guard{};
};
Clamped ClampView(const ShadowGeometry::View& view) {
    Clamped c;
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned k = 0; k < 4; ++k) {
            c.screen[r][k] = Finite(view.screen[r][k]);
            c.clip[r][k] = Finite(view.clip[r][k]);
        }
    for (unsigned k = 0; k < 4; ++k) c.guard[k] = Finite(view.guard[k]);
    return c;
}
Vector TransformClamped(const Matrix& matrix, const Vector& position, bool strip) {
    Vector result;
    const std::array<unsigned, 3> order = strip ? std::array<unsigned, 3>{1, 2, 0}
                                               : std::array<unsigned, 3>{0, 1, 2};
    for (unsigned lane = 0; lane < 4; ++lane) {
        float value = matrix[3][lane];
        for (unsigned row : order) value = Finite(VuFloat::Fma(matrix[row][lane], position[row], value));
        result[lane] = value;
    }
    return result;
}
Vector ProjectClamped(const Matrix& matrix, const Vector& position, bool strip) {
    auto value = TransformClamped(matrix, position, strip);
    const float inverse = value[3] == 0 ? Float((Bits(value[3]) & 0x80000000) | 0x7f7fffff) : Finite(1.f / value[3]);
    for (unsigned lane = 0; lane < 3; ++lane) value[lane] = Finite(value[lane] * inverse);
    return value;
}
Vector DifferenceClamped(const Vector& a, const Vector& b) {
    Vector result{};
    for (unsigned lane = 0; lane < 3; ++lane) result[lane] = Finite(a[lane] + -b[lane]);
    return result;
}
bool ReverseClamped(const Vector& a, const Vector& b) {
    return std::signbit(Finite(VuFloat::Fma(-b[0], a[1], Finite(a[0] * b[1]))));
}
uint32_t IntegerClamped(float value) {
    if (value >= 2147483648.f) return 0x7fffffff;
    if (value < -2147483648.f) return 0x80000000;
    return uint32_t(int32_t(value));
}
Screen QuantizeClamped(const Vector& position, const ShadowGeometry::View& view) {
    Screen result{};
    result.x = float(IntegerClamped(position[0]) & 0xffff) / 16 - view.offset[0];
    result.y = float(IntegerClamped(position[1]) & 0xffff) / 16 - view.offset[1];
    volatile uint32_t depth = IntegerClamped(Finite(0.f + -position[2]));
    result.z = float(depth);
    result.a = 128;
    result.q = 1;
    return result;
}
// One published group (a strip's surviving body, prim 4, or a clipped fan, prim 5): Build's DrawIr per group, BuildRuns'
// run of one vertex array. Groups arrive in Build's order; an empty group is never published.
class Groups {
public:
    virtual ~Groups() = default;
    virtual void Add(uint8_t prim, const Screen* verts, size_t count) = 0;
};
// Publish's accounting: the running total bounded by kMaxOutputVertices (false: the build fails, nothing kept)
bool Publish(uint8_t prim, const Screen* verts, size_t count, size_t& total, Groups& out) {
    if (!count) return true;
    total += count;
    if (total > ShadowGeometry::kMaxOutputVertices) return false;
    out.Add(prim, verts, count);
    return true;
}
bool Degenerate(const Screen& a, const Screen& b, const Screen& c) {
    return (a.x == b.x && a.y == b.y) || (a.x == c.x && a.y == c.y) || (b.x == c.x && b.y == c.y);
}
// Sutherland-Hodgman over the six clip planes into a fixed polygon: each plane keeps k of a polygon's n corners and adds
// one per sign change (at most 2 min(k, n - k)), so n grows at most 1.5 times a plane: 3 4 6 9 13 19 28 even for the
// pathological sign patterns rounding can make
struct Polygon {
    static constexpr size_t kCapacity = 28;
    std::array<Vector, kCapacity> corner;
    size_t size = 0;
};
void Clip(const std::array<Vertex, 3>& corners, const Matrix& matrix, Polygon& out) {
    Polygon spare;
    Polygon* polygon = &out;
    Polygon* next = &spare;
    polygon->size = 3;
    for (unsigned i = 0; i < 3; ++i) polygon->corner[i] = Position(corners[i]);
    for (unsigned plane = 0; plane < 6 && polygon->size; ++plane) {
        Vector row;
        for (unsigned i = 0; i < 4; ++i)
            row[i] = Add(matrix[i][3], (plane % 2 ? 1 : -1) * Finite(matrix[i][plane / 2]));
        const auto distance = [&](const Vector& position) {
            float value = Product(row[0], position[0]);
            value = Madd(row[1], position[1], value);
            value = Madd(row[2], position[2], value);
            return Madd(row[3], 1, value);
        };
        next->size = 0;
        Vector previous = polygon->corner[polygon->size - 1];
        float pd = distance(previous);
        for (size_t c = 0; c < polygon->size; ++c) {
            const Vector current = polygon->corner[c];
            const float cd = distance(current);
            if (std::signbit(pd) != std::signbit(cd)) {
                const float inverse = Reciprocal(Add(cd, -Finite(pd)));
                Vector intersection;
                for (unsigned lane = 0; lane < 4; ++lane)
                    intersection[lane] = Product(Madd(-Finite(current[lane]), pd,
                        Product(previous[lane], cd)), inverse);
                next->corner[next->size++] = intersection;
            }
            if (!std::signbit(cd)) next->corner[next->size++] = current;
            previous = current;
            pd = cd;
        }
        std::swap(polygon, next);
    }
    if (polygon != &out) out = *polygon;
}
// A guard-band triangle's fan, winding-free: its corners quantized (without the colour) and the winding it belongs to (the
// fan of the other winding is empty). False: the clipped polygon has fewer than three corners (no fan for either winding).
bool Fan(const std::array<Vertex, 3>& vertices, const ShadowGeometry::View& view, bool& winding,
         std::array<Screen, Polygon::kCapacity>& corners, size_t& count) {
    Polygon polygon;
    Clip(vertices, view.clip, polygon);
    if (polygon.size < 3) return false;
    std::array<Vector, Polygon::kCapacity> projected;
    for (size_t i = 0; i < polygon.size; ++i) projected[i] = Project(view.screen, polygon.corner[i], false);
    const bool reversed = vertices[2].reverse_fan;
    const auto a = reversed ? Difference(projected[0], projected[1])
                            : Difference(projected[1], projected[0]);
    const auto b = reversed ? Difference(projected[2], projected[0])
                            : Difference(projected[2], projected[1]);
    winding = Reverse(a, b);
    for (size_t i = 0; i < polygon.size; ++i) corners[i] = Quantize(projected[i], view, 0, true);
    count = polygon.size;
    return true;
}
void Colour(Screen& vertex, uint8_t colour, bool emit) {
    vertex.r = vertex.g = vertex.b = colour;
    vertex.draw_kick = emit;
}
// One source strip under one view for the windings `windings` selects (bit 0 the dark node's (false), bit 1 the light
// node's (true)), in the order separate passes per winding publish them: winding false's fans then its body, then winding
// true's. The per-vertex work (both transforms, the outcodes, the projection, the winding test, the quantization) and a
// guard-band triangle's clip and projection do not depend on the winding: done once for both. Every value is the one the
// per-winding pass computed (the same operations on the same operands under the same rounding), so the groups are
// identical to it vertex for vertex.
bool Instance(const ShadowGeometry::Strip& strip, const ShadowGeometry::View& view, const Clamped& clamped, unsigned windings,
              size_t& total, Groups& out) {
    // the working set per thread (kilobytes: off the workers' stacks)
    struct Scratch {
        std::vector<Screen> fans;        // winding true's fans while winding false's body is still to come
        std::vector<uint32_t> fan_ends;
        std::array<Screen, 3 * 38> body[2];   // a triangle per vertex at most
        std::array<Screen, 38> quantized[2];
        std::array<Vector, 38> projected;
        std::array<Screen, Polygon::kCapacity> fan;
        std::array<Screen, 3 * (Polygon::kCapacity - 2)> triangles;
    };
    thread_local Scratch scratch;
    const bool both = windings == 3;
    scratch.fans.clear();
    scratch.fan_ends.clear();
    auto& body = scratch.body;
    auto& quantized = scratch.quantized;
    auto& projected = scratch.projected;
    auto& fan = scratch.fan;
    auto& triangles = scratch.triangles;
    size_t body_size[2] = {0, 0};
    unsigned parity[2] = {0, 0};
    std::array<uint32_t, 3> hard{}, guard{};
    for (size_t i = 0; i < strip.vertices.size(); ++i) {
        const auto& vertex = strip.vertices[i];
        const Vector position{Finite(vertex.position[0]), Finite(vertex.position[1]), Finite(vertex.position[2]), 1};
        const auto clipped = TransformClamped(clamped.clip, position, false);
        Vector guarded;
        for (unsigned lane = 0; lane < 4; ++lane) guarded[lane] = Finite(clipped[lane] * clamped.guard[lane]);
        hard[i % 3] = Outside(clipped);
        guard[i % 3] = Outside(guarded);
        bool emit = !vertex.restart;
        if (i >= 2) {
            if (hard[0] & hard[1] & hard[2]) emit = false;
            else if (guard[0] | guard[1] | guard[2]) {
                bool fan_winding = false;
                size_t corners = 0;
                if (emit && Fan({strip.vertices[i - 2], strip.vertices[i - 1], vertex}, view, fan_winding, fan, corners) &&
                    (windings & (fan_winding ? 2u : 1u))) {
                    const uint8_t colour = fan_winding ? 248 : 8;
                    size_t n = 0;
                    Screen first = fan[0];
                    Colour(first, colour, true);
                    for (size_t c = 2; c < corners; ++c) {
                        triangles[n] = first;
                        triangles[n + 1] = fan[c - 1];
                        triangles[n + 2] = fan[c];
                        Colour(triangles[n + 1], colour, true);
                        Colour(triangles[n + 2], colour, true);
                        n += 3;
                    }
                    if (fan_winding && both) {
                        // published after winding false's body
                        scratch.fans.insert(scratch.fans.end(), triangles.begin(), triangles.begin() + std::ptrdiff_t(n));
                        scratch.fan_ends.push_back(uint32_t(scratch.fans.size()));
                    } else if (!Publish(5, triangles.data(), n, total, out)) {
                        return false;
                    }
                }
                emit = false;
            }
        }
        projected[i] = ProjectClamped(clamped.screen, position, true);
        bool reverse = false;
        if (i >= 2) {
            const auto previous = DifferenceClamped(projected[i - 1], projected[i - 2]);
            const auto current = DifferenceClamped(projected[i], projected[i - 1]);
            reverse = i % 2 ? ReverseClamped(current, previous) : ReverseClamped(previous, current);
        }
        const Screen at = QuantizeClamped(projected[i], view);
        for (unsigned w = 0; w < 2; ++w) {
            if (!(windings & (1u << w))) continue;
            const bool winding = w == 1;
            const bool drawn = emit && (i < 2 || reverse == winding);
            quantized[w][i] = at;
            Colour(quantized[w][i], winding ? 248 : 8, drawn);
            if (!drawn) parity[w] = 0;
            else if (i >= 2) {
                const Screen& a = quantized[w][i - 2 + (parity[w] & 1)];
                const Screen& b = quantized[w][i - 1 - (parity[w] & 1)];
                const Screen& c = quantized[w][i];
                if (!Degenerate(a, b, c)) {
                    Screen* o = body[w].data() + body_size[w];
                    o[0] = a;
                    o[1] = b;
                    o[2] = c;
                    body_size[w] += 3;
                }
                ++parity[w];
            }
        }
    }
    if ((windings & 1) && !Publish(4, body[0].data(), body_size[0], total, out)) return false;
    if (windings & 2) {
        uint32_t at = 0;
        for (const uint32_t end : scratch.fan_ends) {
            if (!Publish(5, scratch.fans.data() + at, end - at, total, out)) return false;
            at = end;
        }
        if (!Publish(4, body[1].data(), body_size[1], total, out)) return false;
    }
    return true;
}
// Build's validation of the views and the mesh
bool Valid(const ShadowGeometry::Mesh& mesh, const ShadowGeometry::View* views, size_t count, ShadowGeometry::Result& result) {
    using Result = ShadowGeometry::Result;
    if (!count || count > 20) { result = Result::InvalidGeometry; return false; }
    for (size_t v = 0; v < count; ++v) {
        const auto& view = views[v];
        for (const auto* matrix : {&view.screen, &view.clip})
            for (const auto& row : *matrix)
                for (float value : row) if (!std::isfinite(value)) { result = Result::InvalidGeometry; return false; }
        for (float value : view.guard) if (!std::isfinite(value)) { result = Result::InvalidGeometry; return false; }
        for (float value : view.offset) if (!std::isfinite(value)) { result = Result::InvalidGeometry; return false; }
    }
    size_t vertices = 0;
    for (const auto& strip : mesh.strips) {
        if (strip.vertices.size() < 3 || strip.vertices.size() > 38) { result = Result::InvalidGeometry; return false; }
        vertices += strip.vertices.size();
        if (vertices > ShadowGeometry::kMaxVertices) { result = Result::LimitExceeded; return false; }
        for (const auto& vertex : strip.vertices)
            for (float value : vertex.position) if (!std::isfinite(value)) { result = Result::InvalidGeometry; return false; }
    }
    return true;
}
// Every strip's groups in Build's order: strip, then winding (false, true), then view
bool Strips(const ShadowGeometry::Mesh& mesh, const ShadowGeometry::View* views, size_t count, Groups& out) {
    size_t total = 0;
    std::array<Clamped, 20> clamped;
    for (size_t v = 0; v < count; ++v) clamped[v] = ClampView(views[v]);
    for (const auto& strip : mesh.strips) {
        if (count == 1) {
            if (!Instance(strip, views[0], clamped[0], 3, total, out)) return false;
            continue;
        }
        for (unsigned winding = 1; winding <= 2; ++winding)
            for (size_t v = 0; v < count; ++v)
                if (!Instance(strip, views[v], clamped[v], winding, total, out)) return false;
    }
    return true;
}
bool g_snapshots = true;   // ShadowGeometry::SetSnapshots

// ---- the shared-position build (ShadowGeometry::BuildRunsShared) -----------------------------------------------------------
// Instance's per-vertex values for each distinct position of a mesh under one view: the clip side (both outcodes) for every
// position, the screen side (the projection and the quantized vertex) for the positions a drawn strip uses
struct PositionValues {
    std::vector<uint32_t> hard, guard;
    std::vector<Vector> projected;
    std::vector<Screen> quantized;
    std::vector<uint32_t> used;   // the positions whose screen side is computed
    std::vector<uint8_t> mark;
};
int g_certified = 1;              // ShadowGeometry::SetCertified
// The clamped forms (Instance's own arithmetic, per position)
void ClipSideClamped(const std::vector<std::array<float, 3>>& positions, const Clamped& c, PositionValues& v) {
    for (size_t i = 0; i < positions.size(); ++i) {
        const auto& p = positions[i];
        const Vector position{Finite(p[0]), Finite(p[1]), Finite(p[2]), 1};
        const auto clipped = TransformClamped(c.clip, position, false);
        Vector guarded;
        for (unsigned lane = 0; lane < 4; ++lane) guarded[lane] = Finite(clipped[lane] * c.guard[lane]);
        v.hard[i] = Outside(clipped);
        v.guard[i] = Outside(guarded);
    }
}
void ScreenSideClamped(const std::vector<std::array<float, 3>>& positions, const Clamped& c, const ShadowGeometry::View& view,
                       PositionValues& v) {
    for (const uint32_t i : v.used) {
        const auto& p = positions[i];
        const Vector position{Finite(p[0]), Finite(p[1]), Finite(p[2]), 1};
        v.projected[i] = ProjectClamped(c.screen, position, true);
        v.quantized[i] = QuantizeClamped(v.projected[i], view);
    }
}
#if defined(__vita__)
// The same operations without the clamps (exponent_budget.h), on the positions as stored: the clip side when its budget holds
// (every value of the chain and of the guard products a zero or a normal number: then Finite of a position is the
// position too -- the budget saw its exponent), the screen side when the screen chain's and the reciprocal's do and its
// stored projections pass the audit (the quantized vertex is taken to integers: a denormal truncates to 0 as the signed
// zero its clamp makes of it; under round toward zero nothing overflows to an infinity)
bool ClipBudget(const ExponentRange& positions, const Clamped& c) {
    ExponentRange m, g;
    m.Add(&c.clip[0][0], 16);
    g.Add(c.guard.data(), 4);
    const ChainBudget chain(m, positions);
    const bool ok = chain.ok && chain.ScaledOk(g);
    if (!ok) {
        // diagnosis (TE_GFX_BUDGET_LOG=N): the first N budgets that failed, with their ranges
        static int left = [] { const char* v = std::getenv("TE_GFX_BUDGET_LOG"); return v ? std::atoi(v) : 0; }();
        if (left > 0) {
            --left;
            std::fprintf(stderr, "[te.gfx] shadow clip budget failed: matrix exponents %d..%d, positions %d..%d, guard %d..%d (chain %d..%d ok %d)\n",
                         m.lo, m.hi, positions.lo, positions.hi, g.lo, g.hi, chain.lo, chain.hi, int(chain.ok));
        }
    }
    return ok;
}
bool ScreenBudget(const ExponentRange& positions, const Clamped& c) {
    ExponentRange m;
    m.Add(&c.screen[0][0], 16);
    const ChainBudget chain(m, positions);
    const bool ok = chain.ok && chain.ReciprocalOk();
    if (!ok) {
        static int left = [] { const char* v = std::getenv("TE_GFX_BUDGET_LOG"); return v ? std::atoi(v) : 0; }();
        if (left > 0) {
            --left;
            std::fprintf(stderr, "[te.gfx] shadow screen budget failed: matrix exponents %d..%d, positions %d..%d (chain %d..%d ok %d)\n",
                         m.lo, m.hi, positions.lo, positions.hi, chain.lo, chain.hi, int(chain.ok));
        }
    }
    return ok;
}
bool ScreenAudit(const PositionValues& v) {
    uint32_t bad = 0;
    for (const uint32_t i : v.used)
        bad |= AllClampFree(v.projected[i].data(), 3, 1) ? 0u : 1u;
    return bad == 0;
}
void ClipSidePlain(const std::vector<std::array<float, 3>>& positions, const Clamped& c, PositionValues& v) {
    for (size_t i = 0; i < positions.size(); ++i) {
        const auto& p = positions[i];
        const float position[3] = {p[0], p[1], p[2]};
        Vector clipped, guarded;
        for (unsigned lane = 0; lane < 4; ++lane) {
            float value = c.clip[3][lane];
            for (unsigned row = 0; row < 3; ++row) value = VuFloat::Fma(c.clip[row][lane], position[row], value);
            clipped[lane] = value;
            guarded[lane] = value * c.guard[lane];
        }
        v.hard[i] = Outside(clipped);
        v.guard[i] = Outside(guarded);
    }
}
void ScreenSidePlain(const std::vector<std::array<float, 3>>& positions, const Clamped& c, const ShadowGeometry::View& view,
                     PositionValues& v) {
    static constexpr unsigned kOrder[3] = {1, 2, 0};   // TransformClamped's strip order
    for (const uint32_t i : v.used) {
        const auto& p = positions[i];
        const float position[3] = {p[0], p[1], p[2]};
        Vector value;
        for (unsigned lane = 0; lane < 4; ++lane) {
            float x = c.screen[3][lane];
            for (unsigned row : kOrder) x = VuFloat::Fma(c.screen[row][lane], position[row], x);
            value[lane] = x;
        }
        const float inverse = value[3] == 0 ? Float((Bits(value[3]) & 0x80000000) | 0x7f7fffff) : 1.f / value[3];
        for (unsigned lane = 0; lane < 3; ++lane) value[lane] = value[lane] * inverse;
        v.projected[i] = value;
        Screen q{};
        q.x = float(IntegerClamped(value[0]) & 0xffff) / 16 - view.offset[0];
        q.y = float(IntegerClamped(value[1]) & 0xffff) / 16 - view.offset[1];
        volatile uint32_t depth = IntegerClamped(0.f + -value[2]);
        q.z = float(depth);
        q.a = 128;
        q.q = 1;
        v.quantized[i] = q;
    }
}
#endif
// Instance (both windings, one view) on the per-position values: the same assembly, statement for statement, with each
// vertex's outcodes, projection and quantized form read from `v` at its distinct position instead of computed again
// `plainWinding`: the projections' exponents keep the winding test's differences and products zeros or normal numbers
// (WindingBudget), so its clamps are the identity -- and the outer one never changes the sign the test reads
bool InstanceShared(const ShadowGeometry::Strip& strip, const std::vector<uint16_t>& index, const ShadowGeometry::View& view,
                    const PositionValues& v, size_t& total, Groups& out, bool plainWinding) {
    struct Scratch {
        std::vector<Screen> fans;
        std::vector<uint32_t> fan_ends;
        std::array<Screen, 3 * 38> body[2];
        std::array<Screen, 38> quantized[2];
        std::array<Vector, 38> projected;
        std::array<Screen, Polygon::kCapacity> fan;
        std::array<Screen, 3 * (Polygon::kCapacity - 2)> triangles;
    };
    thread_local Scratch scratch;
    scratch.fans.clear();
    scratch.fan_ends.clear();
    auto& body = scratch.body;
    auto& quantized = scratch.quantized;
    auto& projected = scratch.projected;
    auto& fan = scratch.fan;
    auto& triangles = scratch.triangles;
    size_t body_size[2] = {0, 0};
    unsigned parity[2] = {0, 0};
    std::array<uint32_t, 3> hard{}, guard{};
    for (size_t i = 0; i < strip.vertices.size(); ++i) {
        const auto& vertex = strip.vertices[i];
        const uint32_t at_position = index[i];
        hard[i % 3] = v.hard[at_position];
        guard[i % 3] = v.guard[at_position];
        bool emit = !vertex.restart;
        if (i >= 2) {
            if (hard[0] & hard[1] & hard[2]) emit = false;
            else if (guard[0] | guard[1] | guard[2]) {
                bool fan_winding = false;
                size_t corners = 0;
                if (emit && Fan({strip.vertices[i - 2], strip.vertices[i - 1], vertex}, view, fan_winding, fan, corners)) {
                    const uint8_t colour = fan_winding ? 248 : 8;
                    size_t n = 0;
                    Screen first = fan[0];
                    Colour(first, colour, true);
                    for (size_t c = 2; c < corners; ++c) {
                        triangles[n] = first;
                        triangles[n + 1] = fan[c - 1];
                        triangles[n + 2] = fan[c];
                        Colour(triangles[n + 1], colour, true);
                        Colour(triangles[n + 2], colour, true);
                        n += 3;
                    }
                    if (fan_winding) {
                        // published after winding false's body
                        scratch.fans.insert(scratch.fans.end(), triangles.begin(), triangles.begin() + std::ptrdiff_t(n));
                        scratch.fan_ends.push_back(uint32_t(scratch.fans.size()));
                    } else if (!Publish(5, triangles.data(), n, total, out)) {
                        return false;
                    }
                }
                emit = false;
            }
        }
        projected[i] = v.projected[at_position];
        bool reverse = false;
        if (i >= 2) {
            if (plainWinding) {
                // DifferenceClamped's x / y lanes and ReverseClamped's sign without the clamps (the same operations)
                const float p0 = projected[i - 1][0] + -projected[i - 2][0], p1 = projected[i - 1][1] + -projected[i - 2][1];
                const float c0 = projected[i][0] + -projected[i - 1][0], c1 = projected[i][1] + -projected[i - 1][1];
                reverse = i % 2 ? std::signbit(VuFloat::Fma(-p0, c1, c0 * p1)) : std::signbit(VuFloat::Fma(-c0, p1, p0 * c1));
            } else {
                const auto previous = DifferenceClamped(projected[i - 1], projected[i - 2]);
                const auto current = DifferenceClamped(projected[i], projected[i - 1]);
                reverse = i % 2 ? ReverseClamped(current, previous) : ReverseClamped(previous, current);
            }
        }
        const Screen at = v.quantized[at_position];
        for (unsigned w = 0; w < 2; ++w) {
            const bool winding = w == 1;
            const bool drawn = emit && (i < 2 || reverse == winding);
            quantized[w][i] = at;
            Colour(quantized[w][i], winding ? 248 : 8, drawn);
            if (!drawn) parity[w] = 0;
            else if (i >= 2) {
                const Screen& a = quantized[w][i - 2 + (parity[w] & 1)];
                const Screen& b = quantized[w][i - 1 - (parity[w] & 1)];
                const Screen& c = quantized[w][i];
                if (!Degenerate(a, b, c)) {
                    Screen* o = body[w].data() + body_size[w];
                    o[0] = a;
                    o[1] = b;
                    o[2] = c;
                    body_size[w] += 3;
                }
                ++parity[w];
            }
        }
    }
    if (!Publish(4, body[0].data(), body_size[0], total, out)) return false;
    uint32_t at = 0;
    for (const uint32_t end : scratch.fan_ends) {
        if (!Publish(5, scratch.fans.data() + at, end - at, total, out)) return false;
        at = end;
    }
    return Publish(4, body[1].data(), body_size[1], total, out);
}
}
ShadowGeometry::Result ShadowGeometry::Decode(const uint8_t* bytes, size_t size,
                                              uint32_t vertices, Mesh& out) {
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
    const auto expect = [&](uint32_t value) {
        uint32_t actual;
        return read(actual) && actual == value;
    };
    uint32_t header;
    if (!read(header) || (header >> 16) != 0x6000 || !expect(0) ||
        !expect(0x070000fa) || !expect(0)) return Result::InvalidAsset;
    limit = 16 + size_t(header & 0xffff) * 16;
    if (limit > size || size - limit > 36) return Result::InvalidAsset;
    for (size_t i = limit; i < size; ++i) if (bytes[i]) return Result::InvalidAsset;
    Mesh mesh;
    uint32_t total = 0;
    while (cursor < limit) {
        uint32_t command;
        if (!read(command)) return Result::InvalidAsset;
        if (!command) {
            for (size_t i = cursor; i < limit; ++i) if (bytes[i]) return Result::InvalidAsset;
            break;
        }
        if (command != 0x6c018000) return Result::InvalidAsset;
        uint32_t count;
        if (!read(count) || (count & ~0xffu) != 0x8000) return Result::InvalidAsset;
        count &= 255;
        if (count < 3 || count > 38 || total + count > vertices ||
            !expect(0x30024000) || !expect(0x512) || !expect(0) ||
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
        for (auto& vertex : strip.vertices) {
            uint32_t word;
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (!read(word)) return Result::InvalidAsset;
                if (lane == 3) vertex.restart = word & 0x8000;
            }
        }
        // All eight source default shadow models carry this authored stream.
        // Its bits, not the numerical value of the unused normal, are consumed.
        if (!expect(0x68008005 | count << 16)) return Result::InvalidAsset;
        for (auto& vertex : strip.vertices) {
            uint32_t word;
            for (unsigned lane = 0; lane < 3; ++lane) {
                if (!read(word)) return Result::InvalidAsset;
                if (!lane) vertex.reverse_fan = word & 32;
            }
        }
        if (!expect(0x14000000) || !expect(0x01000101)) return Result::InvalidAsset;
        total += count;
        mesh.strips.push_back(std::move(strip));
    }
    if (total != vertices) return Result::InvalidAsset;
    out = std::move(mesh);
    return Result::Ok;
}
void ShadowGeometry::SetSnapshots(bool on) { g_snapshots = on; }

ShadowGeometry::Result ShadowGeometry::Build(const Mesh& mesh, const std::vector<View>& views,
                                             std::vector<Draw>& out) {
    Result result = Result::Ok;
    if (!Valid(mesh, views.data(), views.size(), result)) return result;
    // each group one DrawIr (with its record snapshot unless SetSnapshots(false))
    class Draws final : public Groups {
    public:
        std::vector<Draw> output;
        void Add(uint8_t prim, const Screen* verts, size_t count) override {
            Draw draw;
            draw.prim_type = prim;
            draw.verts.assign(verts, verts + count);
            draw.vertex_count = uint32_t(count);
            if (g_snapshots) {
                draw.snapshot.verts_incl = 1;
                draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
                ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
            }
            output.push_back(std::move(draw));
        }
    } draws;
    draws.output.reserve(mesh.strips.size() * 2 * views.size());   // the strips' bodies (fans add to it)
    RoundingGuard rounding;
    if (!Strips(mesh, views.data(), views.size(), draws)) return Result::LimitExceeded;
    out = std::move(draws.output);
    return Result::Ok;
}

ShadowGeometry::Result ShadowGeometry::BuildRuns(const Mesh& mesh, const View& view, std::vector<ps2r::gs::VertexRec>& verts,
                                                 std::vector<uint32_t>& ends) {
    verts.clear();
    ends.clear();
    Result result = Result::Ok;
    if (!Valid(mesh, &view, 1, result)) return result;
    class Runs final : public Groups {
    public:
        std::vector<ps2r::gs::VertexRec>* verts = nullptr;
        std::vector<uint32_t>* ends = nullptr;
        void Add(uint8_t, const Screen* v, size_t count) override {
            verts->insert(verts->end(), v, v + count);
            ends->push_back(uint32_t(verts->size()));
        }
    } runs;
    runs.verts = &verts;
    runs.ends = &ends;
    RoundingGuard rounding;
    if (!Strips(mesh, &view, 1, runs)) {
        verts.clear();
        ends.clear();
        return Result::LimitExceeded;
    }
    return Result::Ok;
}

ShadowGeometry::Result ShadowGeometry::Share(const Mesh& mesh, Shared& out) {
    Shared shared;
    std::map<std::array<uint32_t, 3>, uint32_t> seen;
    shared.index.resize(mesh.strips.size());
    for (size_t s = 0; s < mesh.strips.size(); ++s) {
        auto& index = shared.index[s];
        index.reserve(mesh.strips[s].vertices.size());
        for (const auto& vertex : mesh.strips[s].vertices) {
            std::array<uint32_t, 3> key;
            for (unsigned k = 0; k < 3; ++k) key[k] = Bits(vertex.position[k]);
            auto it = seen.find(key);
            if (it == seen.end()) {
                if (shared.positions.size() >= 65535) return Result::LimitExceeded;
                it = seen.emplace(key, uint32_t(shared.positions.size())).first;
                shared.positions.push_back(vertex.position);
            }
            index.push_back(uint16_t(it->second));
        }
    }
    ExponentRange range;
    for (const auto& position : shared.positions) range.Add(position.data(), 3);
    shared.exponent_lo = range.lo;
    shared.exponent_hi = range.hi;
    out = std::move(shared);
    return Result::Ok;
}

ShadowGeometry::Result ShadowGeometry::BuildRunsShared(const Mesh& mesh, const Shared& shared, const View& view,
                                                       std::vector<ps2r::gs::VertexRec>& verts, std::vector<uint32_t>& ends,
                                                       SharedCensus* census) {
    verts.clear();
    ends.clear();
    Result result = Result::Ok;
    if (!Valid(mesh, &view, 1, result)) return result;
    if (shared.index.size() != mesh.strips.size()) return Result::InvalidGeometry;
    for (size_t s = 0; s < mesh.strips.size(); ++s)
        if (shared.index[s].size() != mesh.strips[s].vertices.size()) return Result::InvalidGeometry;
    class Runs final : public Groups {
    public:
        std::vector<ps2r::gs::VertexRec>* verts = nullptr;
        std::vector<uint32_t>* ends = nullptr;
        void Add(uint8_t, const Screen* v, size_t count) override {
            verts->insert(verts->end(), v, v + count);
            ends->push_back(uint32_t(verts->size()));
        }
    } runs;
    runs.verts = &verts;
    runs.ends = &ends;
    RoundingGuard rounding;
    const Clamped clamped = ClampView(view);
    thread_local PositionValues values;
    PositionValues& v = values;
    const size_t n = shared.positions.size();
    v.hard.resize(n);
    v.guard.resize(n);
    v.projected.resize(n);
    v.quantized.resize(n);
    v.mark.assign(n, 0);
    v.used.clear();
    [[maybe_unused]] const int mode = g_certified;   // (the clamp-free phases are the Vita's)
    // the clip side of every distinct position
    bool certified = false;
#if defined(__vita__)
    ExponentRange range;
    range.lo = shared.exponent_lo;
    range.hi = shared.exponent_hi;
    if ((mode == 1 || mode == 2 || mode == 4) && ClipBudget(range, clamped)) {
        ClipSidePlain(shared.positions, clamped, v);
        certified = true;
        if (mode == 2) {
            // diagnosis: the clamped forms too, compared
            const std::vector<uint32_t> hard = v.hard, guard = v.guard;
            ClipSideClamped(shared.positions, clamped, v);
            if (census)
                for (size_t i = 0; i < n; ++i) census->check_differ += (hard[i] != v.hard[i] || guard[i] != v.guard[i]) ? 1 : 0;
        }
    }
#endif
    if (!certified) ClipSideClamped(shared.positions, clamped, v);
    // the strips Instance draws something of: not every vertex outside one plane of the clip test (Instance drops each
    // triangle whose three corners share an outside bit -- with every vertex sharing one, each of the strip's triangles)
    thread_local std::vector<uint8_t> drawn;
    drawn.assign(mesh.strips.size(), 0);
    size_t skipped = 0;
    for (size_t s = 0; s < mesh.strips.size(); ++s) {
        uint32_t common = ~0u;
        for (const uint16_t p : shared.index[s]) common &= v.hard[p];
        if (common) { ++skipped; continue; }
        drawn[s] = 1;
        for (const uint16_t p : shared.index[s])
            if (!v.mark[p]) {
                v.mark[p] = 1;
                v.used.push_back(p);
            }
    }
    // the screen side of the positions those strips use
    bool screenCertified = false;
#if defined(__vita__)
    if ((mode == 1 || mode == 2 || mode == 4) && ScreenBudget(range, clamped)) {
        ScreenSidePlain(shared.positions, clamped, view, v);
        screenCertified = ScreenAudit(v);
        if (mode == 2 && screenCertified) {
            std::vector<Vector> projected;
            std::vector<Screen> quantized;
            for (const uint32_t i : v.used) {
                projected.push_back(v.projected[i]);
                quantized.push_back(v.quantized[i]);
            }
            ScreenSideClamped(shared.positions, clamped, view, v);
            if (census)
                for (size_t k = 0; k < v.used.size(); ++k) {
                    const uint32_t i = v.used[k];
                    census->check_differ += (std::memcmp(&projected[k], &v.projected[i], sizeof(Vector)) != 0 ||
                                             std::memcmp(&quantized[k], &v.quantized[i], sizeof(Screen)) != 0) ? 1 : 0;
                }
        }
    }
#endif
    if (!screenCertified) ScreenSideClamped(shared.positions, clamped, view, v);
    if (census) {
        census->positions += n;
        census->projected += v.used.size();
        census->strips += mesh.strips.size();
        census->strips_skipped += skipped;
        for (const auto& strip : mesh.strips) census->vertices += strip.vertices.size();
        if (certified && screenCertified) census->certified++;
        else census->exact++;
    }
    // the winding test's budget: every projected x / y a zero or a normal number (the clamped forms' always are, the
    // clamp-free ones passed the audit) with nonzero exponents in [E, F]: a nonzero difference of two of them is a multiple
    // of 2^(E - 23) under 2^(F + 2), a product of two differences at least 2^(2E - 46) and under 2^(2F + 4)
    bool plainWinding = false;
#if defined(__vita__)
    if (mode == 1 || mode == 2 || mode == 4) {
        ExponentRange xy;
        for (const uint32_t i : v.used) xy.Add(v.projected[i].data(), 2);
        plainWinding = xy.AllZero() || (2 * xy.lo - 46 >= -126 && 2 * xy.hi + 4 <= 126);
    }
#endif
    size_t total = 0;
    for (size_t s = 0; s < mesh.strips.size(); ++s) {
        if (!drawn[s]) continue;
        if (!InstanceShared(mesh.strips[s], shared.index[s], view, v, total, runs, plainWinding)) {
            verts.clear();
            ends.clear();
            return Result::LimitExceeded;
        }
    }
    return Result::Ok;
}

void ShadowGeometry::SetCertified(int mode) { g_certified = mode; }
int ShadowGeometry::Certified() { return g_certified; }
}  // namespace title
