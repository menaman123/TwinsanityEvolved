#include "games/slus_20909_crash_twinsanity/hle/math/joint_math.h"

#include <cfenv>
#include <cmath>
#include <cstring>

#include "engine/vu/vu_float.h"

namespace title {

// The Vita: the operations as the CPU does them, unclamped and in its own rounding -- the exact form (operands and
// results clamped through memory, the VU's rounding) cost the game thread milliseconds a frame there; elsewhere exact.
#if defined(__vita__)
class JointMath::Rounding { public: Rounding() {} };
#else
class JointMath::Rounding {
public:
    Rounding() : previous_(std::fegetround()) {
        if (previous_ != FE_TOWARDZERO) std::fesetround(FE_TOWARDZERO);
    }
    ~Rounding() {
        if (previous_ != FE_TOWARDZERO) std::fesetround(previous_);
    }
private:
    int previous_;
};
#endif

float JointMath::Float(uint32_t bits) {
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
float JointMath::Finite(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const auto magnitude = bits & 0x7fffffff;
    if (magnitude >= 0x7f800000) bits = (bits & 0x80000000) | 0x7f7fffff;
    else if (magnitude < 0x00800000) bits &= 0x80000000;
    return Float(bits);
}
#if defined(__vita__)
namespace {
// newlib's truncf (libm/common/sf_trunc.c, the eboot's) on a PRODUCT, inline and without a branch. newlib: s = the
// unbiased exponent; s < 0 (|x| < 1): the sign alone; 0 <= s < 23: the bits below the binary point cleared; s >= 23:
// x itself (whole already) -- and s == 128 (NaN, infinity): x + x. Here 0xff800000 >> s (arithmetic) = ~(0x007fffff >> s)
// for 0 <= s <= 22 and all ones from 23 on, which keeps a NaN's or an infinity's bits as they are: the same as x + x
// whenever x is the result of a VFP multiply -- infinity + infinity is that infinity, and a multiply's NaN is already
// quiet (FPSCR.DN off: x + x of a quiet NaN is that NaN) or the default NaN (DN on: x + x is the default NaN too). The
// fold's only input is such a product (|angle| times the fold's scale). Checked over every one of the 2^32 angles in
// every rounding mode with FZ and DN off / on: cpuwork gamecodework/cull/foldtest.cpp.
inline float TruncOfProduct(float x) {
    uint32_t w;
    std::memcpy(&w, &x, sizeof(w));
    const int32_t s = int32_t((w >> 23) & 0xffu) - 127;
    const int32_t shift = s < 0 ? 0 : (s > 23 ? 23 : s);
    const uint32_t mask = uint32_t(int32_t(0xff800000u) >> shift) & (0x80000000u | ~uint32_t(s >> 31));
    const uint32_t bits = w & mask;
    float whole;
    std::memcpy(&whole, &bits, sizeof(whole));
    return whole;
}
}  // namespace
// The fold, the polynomial and the sine inline where they are used (EulerRotation's six polynomials, Interpolate's
// three): one block of independent chains for the compiler to interleave (a dependent VMLA waits 8 cycles on the A9)
#define TE_JM_INLINE __attribute__((always_inline)) inline
float JointMath::Product(float a, float b) { return a * b; }
float JointMath::Sum(float a, float b) { return a + b; }
float JointMath::Difference(float a, float b) { return a - b; }
float JointMath::MultiplyAdd(float a, float b, float sum) { return a * b + sum; }
#else
float JointMath::Product(float a, float b) {
    volatile float left = Finite(a), right = Finite(b), result = left * right;
    return Finite(result);
}
float JointMath::Sum(float a, float b) {
    volatile float left = Finite(a), right = Finite(b), result = left + right;
    return Finite(result);
}
float JointMath::Difference(float a, float b) {
    volatile float left = Finite(a), right = Finite(b), result = left - right;
    return Finite(result);
}
float JointMath::MultiplyAdd(float a, float b, float sum) {
    volatile float left = Finite(a), right = Finite(b), accumulated = Finite(sum);
    // VuFloat::Fma: std::fma's result (the Vita: its product and sum rounded apart; newlib's fmaf is software)
    return Finite(::VuFloat::Fma(left, right, accumulated));
}
#define TE_JM_INLINE
#endif
#if defined(__vita__)
// The exact form's values (Finite, the zero test, the division, Finite) without its volatile stores and loads and with
// the zero test on the clamped bits: Finite leaves no NaN and turns a denormal into its signed zero, so `divisor == 0`
// is (bits & 0x7fffffff) == 0 -- a VFP compare would be a VMRS flag transfer on the Cortex-A9
float JointMath::Reciprocal(float value) {
    const float divisor = Finite(value);
    uint32_t bits;
    std::memcpy(&bits, &divisor, sizeof(bits));
    if ((bits & 0x7fffffffu) == 0) return Float((bits & 0x80000000u) ? 0xff7fffff : 0x7f7fffff);
    return Finite(1.0f / divisor);
}
#else
float JointMath::Reciprocal(float value) {
    volatile float divisor = Finite(value);
    if (divisor == 0) return Float(std::signbit(divisor) ? 0xff7fffff : 0x7f7fffff);
    volatile float result = 1.0f / divisor;
    return Finite(result);
}
#endif

TE_JM_INLINE float JointMath::Fold(float angle) {
    const float magnitude = std::fabs(angle);
    const float scale = Float(0xbe22f987);
    float residual = Product(magnitude, scale);
#if defined(__vita__)
    // The whole turns taken off by truncation. The program does it with the magic number below under the VU's chop
    // (the exact form's Rounding); the Vita's fast form runs in the caller's rounding -- to nearest -- under which the
    // magic number rounds to the NEAREST whole turn: past half a turn the fold then leaves the polynomial's range and
    // sine / cosine come out wrong by up to 0.4 (every EulerRotation half angle beyond pi: one picture of a spinning
    // object -- the title's logo -- drawn in a wrong rotation). Truncating is what the chop gives, in any rounding.
    // std::trunc is a libcall on the Cortex-A9 (VFPv3 has no VRINTZ): newlib's truncf of the product, inline and
    // without its branches (TruncOfProduct: the same bits), so the folds of an EulerRotation's six polynomials are no
    // longer six calls the compiler cannot schedule across
    residual = TruncOfProduct(residual);
#else
    const float magic = Float(0x4b400000);
    residual = MultiplyAdd(-1, magic, residual);
    residual = MultiplyAdd(1, magic, residual);
#endif
    residual = MultiplyAdd(-magnitude, scale, residual);
    residual = MultiplyAdd(-1, 0.5f, residual);
    return Difference(std::fabs(residual), 0.25f);
}

TE_JM_INLINE float JointMath::Polynomial(float folded) {
    const float squared = Product(folded, folded);
    const float fourth = Product(squared, squared);
    const float eighth = Product(fourth, fourth);
    const float seventh = Product(Product(folded, Float(0xc2992661)), squared);
    float sum = Product(Product(folded, Float(0xc2255de0)), squared);
    sum = MultiplyAdd(seventh, fourth, sum);
    sum = MultiplyAdd(Product(folded, Float(0x42a33457)), fourth, sum);
    sum = MultiplyAdd(folded, Float(0x40c90fda), sum);
    return MultiplyAdd(Product(folded, Float(0x421ed7b7)), eighth, sum);
}

float JointMath::Arc(float cosine) {
    const float half_pi = Float(0x3fc90fdb);
    const float delta = Difference(Product(cosine, half_pi), half_pi);
    const float correction = Polynomial(Fold(delta));
    const float base = Difference(Product(cosine, -half_pi), -half_pi);
    const float adjusted = Difference(Product(correction, -half_pi), -half_pi);
    return Difference(Sum(base, base), adjusted);
}

TE_JM_INLINE float JointMath::Sine(float angle) {
    return Polynomial(Fold(Difference(angle, Float(0x3fc90fd8))));
}

JointMath::Interpolation JointMath::Interpolate(const Vector& from, const Vector& to, float share) {
    Rounding rounding;
    float cosine = Product(from[0], to[0]);
    for (size_t lane = 1; lane < from.size(); ++lane)
        cosine = Sum(cosine, Product(from[lane], to[lane]));
    auto target = to;
    if (std::signbit(cosine)) {
        cosine = Product(cosine, -1);
        for (auto& lane : target) lane = Product(lane, -1);
    }
    Interpolation result{};
    result.curved = std::signbit(Difference(cosine, Float(0x3f7fbe77)));
    if (result.curved) {
        const float angle = Arc(cosine);
        const float inverse = Reciprocal(Sine(angle));
        result.to_weight = Product(Sine(Product(share, angle)), inverse);
        result.from_weight = Product(Sine(Product(Difference(1, share), angle)), inverse);
    } else {
        result.to_weight = share;
        result.from_weight = Difference(1, share);
    }
    for (size_t lane = 0; lane < from.size(); ++lane)
        result.rotation[lane] = MultiplyAdd(target[lane], result.to_weight,
            Product(from[lane], result.from_weight));
    return result;
}

JointMath::Basis JointMath::RotationBasis(const Vector& rotation, const Vector& parent_scale,
                                        const Vector* own_scale) {
    Rounding rounding;
    Vector scaled{}, doubled_square{}, positive_cross{}, negative_cross{};
    for (size_t lane = 0; lane < 4; ++lane)
        scaled[lane] = Product(rotation[lane], Float(0x3fb504f3));
    for (size_t lane = 0; lane < 3; ++lane) {
        doubled_square[lane] = MultiplyAdd(rotation[lane], rotation[lane],
            Product(rotation[lane], rotation[lane]));
        const float cross = Product(scaled[(lane + 1) % 3], scaled[(lane + 2) % 3]);
        positive_cross[lane] = MultiplyAdd(scaled[lane], scaled[3], cross);
        negative_cross[lane] = MultiplyAdd(-scaled[lane], scaled[3], cross);
    }
    const float diagonal_x = MultiplyAdd(-1, doubled_square[2], Difference(1, doubled_square[1]));
    const float diagonal_y = MultiplyAdd(-1, doubled_square[2], Difference(1, doubled_square[0]));
    const float diagonal_z = MultiplyAdd(-1, doubled_square[1], Difference(1, doubled_square[0]));
    Basis coefficients{{
        {diagonal_x, positive_cross[2], Sum(0, negative_cross[1]), 0},
        {negative_cross[2], diagonal_y, positive_cross[0], 0},
        {positive_cross[1], Sum(0, negative_cross[0]), diagonal_z, 0}}};
    if (own_scale)
        for (size_t row = 0; row < 3; ++row)
            for (auto& value : coefficients[row]) value = Product(value, (*own_scale)[row]);
    Vector inverse{};
    for (size_t lane = 0; lane < 3; ++lane)
        inverse[lane] = Sum(0, Reciprocal(parent_scale[lane]));
    Basis result{};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t lane = 0; lane < 4; ++lane) {
            float sum = Product(lane == 0 ? inverse[0] : 0, coefficients[row][0]);
            sum = MultiplyAdd(lane == 1 ? inverse[1] : 0, coefficients[row][1], sum);
            sum = MultiplyAdd(lane == 2 ? inverse[2] : 0, coefficients[row][2], sum);
            result[row][lane] = MultiplyAdd(0, coefficients[row][3], sum);
        }
    }
    return result;
}

JointMath::Matrix JointMath::MultiplyByParent(const Matrix& matrix, const Matrix& parent) {
    Rounding rounding;
    Matrix result{};
    for (size_t row = 0; row < 4; ++row) {
        const size_t lanes = row == 3 ? 3 : 4;
        for (size_t lane = 0; lane < lanes; ++lane) {
            float sum = Product(parent[0][lane], matrix[row][0]);
            sum = MultiplyAdd(parent[1][lane], matrix[row][1], sum);
            sum = MultiplyAdd(parent[2][lane], matrix[row][2], sum);
            result[row][lane] = MultiplyAdd(parent[3][lane], row == 3 ? 1 : matrix[row][3], sum);
        }
    }
    result[3][3] = 1;
    return result;
}

JointMath::Vector JointMath::Turn(const Vector& by, const Vector& rotation) {
    // Source quaternion product by * rotation in the standard program's order:
    // lane products, outer-product accumulation with both w broadcasts, then
    // the w lane as the difference of products. No normalization.
    Rounding rounding;
    Vector products, accumulator, result;
    for (size_t lane = 0; lane < 4; ++lane) products[lane] = Product(by[lane], rotation[lane]);
    accumulator[0] = Product(by[1], rotation[2]);
    accumulator[1] = Product(by[2], rotation[0]);
    accumulator[2] = Product(by[0], rotation[1]);
    for (size_t lane = 0; lane < 3; ++lane) {
        accumulator[lane] = MultiplyAdd(by[lane], rotation[3], accumulator[lane]);
        accumulator[lane] = MultiplyAdd(rotation[lane], by[3], accumulator[lane]);
    }
    accumulator[3] = Difference(products[3], products[2]);
    accumulator[3] = MultiplyAdd(-1.0f, products[1], accumulator[3]);
    result[0] = MultiplyAdd(-rotation[1], by[2], accumulator[0]);
    result[1] = MultiplyAdd(-rotation[2], by[0], accumulator[1]);
    result[2] = MultiplyAdd(-rotation[0], by[1], accumulator[2]);
    result[3] = MultiplyAdd(-1.0f, products[0], accumulator[3]);
    return result;
}

JointMath::EulerResult JointMath::EulerRotation(const Vector& angles_now, const Vector& angles_next,
                                                const Vector& move_now, const Vector& move_next) {
    Rounding rounding;
    const float share = angles_next[3];
    // Frame blend of the three angles and the translation, now + (next - now) * share,
    // in the program's accumulate order; each lane is clamped by the zero addition.
    Vector angles{}, move{};
    for (size_t lane = 0; lane < 3; ++lane) {
        float accumulated = Sum(angles_now[lane], 0.0f);
        accumulated = MultiplyAdd(-angles_now[lane], share, accumulated);
        angles[lane] = MultiplyAdd(angles_next[lane], share, accumulated);
        accumulated = Sum(move_now[lane], 0.0f);
        accumulated = MultiplyAdd(-move_now[lane], share, accumulated);
        move[lane] = MultiplyAdd(move_next[lane], share, accumulated);
    }
    move[3] = 1;
    // Half angles. Each axis yields a lane pair through the shared fold and
    // polynomial: the shifted lane (angle - pi/2) and the plain lane, which
    // pass through the program's lane moves (zero addition or unit product).
    const float half_x = Product(angles[0], 0.5f), half_y = Product(angles[1], 0.5f),
                half_z = Product(angles[2], 0.5f);
    const float shifted_x = Sine(half_x), plain_x = Polynomial(Fold(Sum(0.0f, half_x)));
    const float shifted_z = Sine(half_z), plain_z = Polynomial(Fold(Product(1.0f, half_z)));
    const float shifted_y = Sine(half_y), plain_y = Polynomial(Fold(Sum(0.0f, half_y)));
    // Pairwise x/z products, then the three-factor lanes, in the program's order.
    const float ss = Product(shifted_x, shifted_z), ps = Product(plain_x, shifted_z);
    const float sp = Product(shifted_x, plain_z), pp = Product(plain_x, plain_z);
    const float ss_p = Product(ss, plain_y), ps_p = Product(ps, plain_y);
    const float sp_s = Product(sp, shifted_y), pp_s = Product(pp, shifted_y);
    const float ss_s = Product(ss, shifted_y), ps_s = Product(ps, shifted_y);
    const float sp_p = Product(sp, plain_y), pp_p = Product(pp, plain_y);
    const float zero = Difference(0.0f, 0.0f);
    EulerResult result{};
    result.move = move;
    result.rotation[0] = Difference(sp_p, ps_s);
    result.rotation[1] = Sum(zero, Sum(ss_p, pp_s));
    result.rotation[2] = Sum(zero, Difference(ps_p, sp_s));
    result.rotation[3] = Sum(zero, Sum(pp_p, ss_s));
    return result;
}

namespace {
// Macro-mode arithmetic: both operands and the result pass the EE macro clamp
// (NaN/Inf to signed maximum, no denormal flush) in the current rounding mode.
float MacroClamp(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if (((bits >> 23) & 0xffu) == 0xffu) bits = (bits & 0x80000000u) | 0x7f7fffffu;
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
}  // namespace

#if defined(__vita__)
// The same clamps and operations without the volatile store and reload of every result (a float operation on the Vita's
// VFP is IEEE single precision in the thread's own rounding either way; the volatile only cost a store, a load and a
// forwarding stall per operation -- Lerp makes nine)
float JointMath::ScalarSum(float a, float b) { return MacroClamp(MacroClamp(a) + MacroClamp(b)); }
float JointMath::ScalarDifference(float a, float b) { return MacroClamp(MacroClamp(a) - MacroClamp(b)); }
float JointMath::ScalarProduct(float a, float b) { return MacroClamp(MacroClamp(a) * MacroClamp(b)); }
#else
float JointMath::ScalarSum(float a, float b) { volatile float r = MacroClamp(a) + MacroClamp(b); return MacroClamp(r); }
float JointMath::ScalarDifference(float a, float b) {
    volatile float r = MacroClamp(a) - MacroClamp(b);
    return MacroClamp(r);
}
float JointMath::ScalarProduct(float a, float b) { volatile float r = MacroClamp(a) * MacroClamp(b); return MacroClamp(r); }
#endif

JointMath::Vector JointMath::Lerp(const Vector& from, const Vector& to, float share) {
    Vector result{};
    for (size_t lane = 0; lane < 3; ++lane) {
        float accumulated = ScalarSum(from[lane], 0.0f);
        accumulated = ScalarDifference(accumulated, ScalarProduct(from[lane], share));
        result[lane] = ScalarSum(accumulated, ScalarProduct(to[lane], share));
    }
    result[3] = 1;
    return result;
}

}  // namespace title
