// platform/nativeee (host side) -- te_unclamped.h: the PS2 FPU clamps of the engine's collision maths left out where
// they cannot act (used on the Vita's game thread; header-only so a host test can hold it against the engine).
//
// title::CollisionMath's Product / Sum / Difference (engine hle/math/collision_math.cpp) clamp every operand and every
// result (ps2_float_clamp: an infinity or a NaN becomes the largest finite number of its sign) and pass each result
// through a volatile. On the Cortex-A9 every clamp is a VFP-to-core register transfer and a branch -- most of what these
// functions cost there. Under round-toward-zero -- the EE's mode, the game thread's (runner/main.cpp; the Vita's te-game
// FPSCR reads RZ | FZ | DN in the device dump) -- a product or a sum of FINITE floats is finite: an overflow rounds to the
// largest finite number of its sign, never to infinity, and no NaN can arise. So when every input word is finite every
// clamp is the identity and the volatile changes no value: the same multiplies and adds in the same order give the same
// bits, with flush-to-zero on or off (denormals are the hardware's to flush in both forms: a clamp never touches them).
// GCC joins a product and the following sum into VFPv3's VMLA, which rounds the product and the sum apart (the ARM ARM's
// FPAdd(d, FPMul(n, m)); Vita3K's dynarmic translates it the same way, vfp.cpp vfp_VMLA): the bits of VMUL + VADD. (VFPv3
// has no fused multiply-add for -ffp-contract to form.) Sums are commutative bit for bit.
//   Applies()      the calling thread rounds toward zero (otherwise: the engine's clamped path)
//   FiniteWords()  no word has the all-ones exponent (integer tests on the stored bits: no VFP transfer)
#pragma once
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/math/collision_math.h"

namespace te::unclamped {

inline bool Applies() { return std::fegetround() == FE_TOWARDZERO; }

inline bool FiniteWords(const float* words, size_t n) {
    uint32_t special = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t bits;
        std::memcpy(&bits, words + i, 4);
        special |= uint32_t((bits & 0x7F800000u) == 0x7F800000u);
    }
    return special == 0;
}

// ps2e_vu0_groups_range over title::CollisionMath::ProjectGroup without its clamps: `count` groups of three quadwords
// (four x, four y, four z); per group and lane partial = x * axis0 + y * axis1, value = partial + z * axis2 (Product,
// Product, Sum, Product, Sum); the least / greatest by std::min / std::max in the same order (group, then lane)
inline void GroupsRange(const float* g, int32_t count, const float* axis, float* out) {
    const float a0 = axis[0], a1 = axis[1], a2 = axis[2];
    float least = INFINITY, greatest = -INFINITY;
    for (int32_t i = 0; i < count; ++i) {
        const float* p = g + size_t(i) * 12;
        for (int lane = 0; lane < 4; ++lane) {
            const float x = p[lane] * a0;
            const float y = p[4 + lane] * a1;
            const float partial = x + y;
            const float z = p[8 + lane] * a2;
            const float value = partial + z;
            least = std::min(least, value);
            greatest = std::max(greatest, value);
        }
    }
    out[0] = least;
    out[1] = greatest;
}

// ---- the triangle edge tests (title::CollisionMath::EvaluateStartEdges / EvaluateFinishEdges, collision_math.cpp) ------
// The engine's primitives with their clamps left out: SubtractPoints (Difference x3, w = 0), MultiplyPoints (Product x3,
// w = 0), DotSum (Sum(Sum(x, y), z)); CrossTerms / Cross carry no clamp in the engine either (a volatile only). The w lane
// of an input never reaches the arithmetic, so only x, y, z are checked (FiniteXyz).
using Vector = title::CollisionMath::Vector;
inline bool FiniteXyz(const Vector& v) { return FiniteWords(v.data(), 3); }
inline Vector SubtractPoints(const Vector& a, const Vector& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2], 0}; }
inline Vector MultiplyPoints(const Vector& a, const Vector& b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2], 0}; }
inline Vector CrossTerms(const Vector& a, const Vector& b) {
    Vector value{};
    for (size_t lane = 0; lane < 3; ++lane) value[lane] = a[(lane + 1) % 3] * b[(lane + 2) % 3];
    return value;
}
inline Vector Cross(const Vector& a, const Vector& b) {
    const Vector first = CrossTerms(a, b), second = CrossTerms(b, a);
    Vector value{};
    for (size_t lane = 0; lane < 3; ++lane) value[lane] = first[lane] - second[lane];
    return value;
}
inline float DotSum(const Vector& products) { return (products[0] + products[1]) + products[2]; }

inline bool StartEdgesApply(const Vector& start, const Vector& end, const title::CollisionMath::Triangle& triangle) {
    return Applies() && FiniteXyz(start) && FiniteXyz(end) && FiniteXyz(triangle[0]) && FiniteXyz(triangle[1]) &&
           FiniteXyz(triangle[2]);
}
// EvaluateStartEdges, statement for statement
inline title::CollisionMath::EdgeSetup StartEdges(const Vector& start, const Vector& end,
                                                 const title::CollisionMath::Triangle& triangle) {
    title::CollisionMath::EdgeSetup result{};
    result.direction = SubtractPoints(end, start);
    result.diagonal = SubtractPoints(triangle[2], triangle[0]);
    for (size_t edge = 0; edge < 3; ++edge) {
        result.edges[edge] = SubtractPoints(triangle[(edge + 1) % 3], triangle[edge]);
        result.edge_normals[edge] = Cross(result.direction, result.edges[edge]);
    }
    result.normal = Cross(result.edges[0], result.diagonal);
    result.plane_products[0] = MultiplyPoints(SubtractPoints(start, triangle[0]), result.normal);
    result.plane_products[1] = MultiplyPoints(SubtractPoints(end, triangle[0]), result.normal);
    for (size_t i = 0; i < 2; ++i) result.plane_values[i] = DotSum(result.plane_products[i]);
    constexpr size_t vertices[] = {0, 2, 1, 0, 2, 1};
    for (size_t i = 0; i < 6; ++i) result.pending[i] = SubtractPoints(triangle[vertices[i]], start);
    for (size_t i = 0; i < 2; ++i) result.pending[i] = MultiplyPoints(result.pending[i], result.edge_normals[0]);
    result.last_cross_products = CrossTerms(result.direction, result.edges[2]);
    return result;
}

inline bool FinishEdgesApply(const title::CollisionMath::EdgeSetup& pending) {
    if (!Applies()) return false;
    for (const Vector& v : pending.pending) if (!FiniteXyz(v)) return false;
    return FiniteXyz(pending.edge_normals[1]) && FiniteXyz(pending.edge_normals[2]);
}
// EvaluateFinishEdges, statement for statement
inline title::CollisionMath::EdgeValues FinishEdges(const title::CollisionMath::EdgeSetup& pending) {
    title::CollisionMath::EdgeValues result{pending.pending, {}};
    for (size_t i = 2; i < 6; ++i) result.products[i] = MultiplyPoints(pending.pending[i], pending.edge_normals[i / 2]);
    for (size_t i = 0; i < 6; ++i) result.values[i] = DotSum(result.products[i]);
    return result;
}

}  // namespace te::unclamped
