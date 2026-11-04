#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

namespace title {

// Animation's source quaternion and matrix arithmetic. Its authored
// approximation/rounding differs from ordinary macro math helpers.
class JointMath {
public:
    using Vector = MathLibrary::Vector;
    using Matrix = MathLibrary::Matrix;
    using Basis = std::array<Vector, 3>;
    struct Interpolation {
        Vector rotation;
        float from_weight, to_weight;
        bool curved;
    };
    static Interpolation Interpolate(const Vector& from, const Vector& to, float share);
    // Parent scale compensation precedes composition with the parent matrix.
    // An absent own scale selects the source's distinct unscaled operation.
    static Basis RotationBasis(const Vector& rotation, const Vector& parent_scale,
                               const Vector* own_scale = nullptr);
    // The first three rows use all four lanes. Translation uses xyz and is
    // published as an affine point, irrespective of its incoming w word.
    static Matrix MultiplyByParent(const Matrix& matrix, const Matrix& parent);
    // AnimateJoint's additional joint rotation: the quaternion product of the
    // joint's extra rotation and the animated rotation (standard a70).
    static Vector Turn(const Vector& by, const Vector& rotation);
    struct EulerResult {
        Vector rotation;
        Vector move;
    };
    // AnimateJoint's frame blend: angles and translation interpolated by
    // anglesNext.w, half-angle authored sine/cosine polynomials and the
    // source's Euler-to-quaternion composition (standard 818). move.w is one.
    static EulerResult EulerRotation(const Vector& angles_now, const Vector& angles_next,
                                     const Vector& move_now, const Vector& move_next);
    // AnimateJoint's inline scale blend (TR Lerp): from + (to - from) * share
    // as the generated EE macro sequence evaluates it -- clamped operands, two
    // separate roundings per lane in the CALLER's rounding mode, w = 1.
    static Vector Lerp(const Vector& from, const Vector& to, float share);
    // EE scalar FPU and macro arithmetic: both operands and the result pass the
    // clamp (NaN/Inf to signed maximum, no denormal flush) in the CALLER's
    // rounding mode. AnimateJoint's channel scaling and pose blends use these.
    static float ScalarSum(float a, float b);
    static float ScalarDifference(float a, float b);
    static float ScalarProduct(float a, float b);

private:
    class Rounding;
    static float Float(uint32_t bits);
    static float Finite(float value);
    static float Product(float a, float b);
    static float Sum(float a, float b);
    static float Difference(float a, float b);
    static float MultiplyAdd(float a, float b, float sum);
    static float Reciprocal(float value);
    static float Fold(float angle);
    static float Polynomial(float folded);
    static float Arc(float cosine);
    static float Sine(float angle);
};

}  // namespace title

#if defined(__vita__)
// (the Vita: the definitions in line -- joint_math_bodies.inl)
#define TE_JM_DEF inline __attribute__((always_inline))
#include "games/slus_20909_crash_twinsanity/hle/math/joint_math_bodies.inl"
#endif
