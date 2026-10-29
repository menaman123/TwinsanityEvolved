#include "games/slus_20909_crash_twinsanity/hle/math/particle_view_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine/vu/vu_float.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

// The culling VU0 set's programs 0xA08 (ParticleBlockView) and 0xB90 (ParticleBlockView, keepsTranslation) as value
// arithmetic -- the VU's rules as title::RayMath keeps them: every FMAC reads clamped operands (NaN/Inf -> +/-MAX, denormal
// -> +/-0) and writes clamped lanes, rounding toward zero (RoundingGuard); MADD/MSUB are single-rounding fused multiply-adds;
// SQRT is sqrt(|x|), DIV num / den with the by-zero rule; the MAC sign flags are judged on the raw lanes written. Both
// programs are straight-line: their FMANDs read the flags of fixed producers (centre + radius, centre - radius per plane
// group), their Q consumers the bound DIV/SQRT results (0xA08's MULq falls on the DIV's seventh cycle and reads the new Q:
// pinned by tests/vu/test_vu0_particle_view.cpp on the interpreter).

namespace title {
namespace {

// The Vita: the clamps inline in the multiply-adds (GCC kept Clamp4 as two calls in every MaddBc -- 24 a block view, each
// returning its four lanes through memory); the same operations in the same order. MaddBc / MulBc / GroupFlags stay
// calls: all of them inline made BlockView 4700 straight-line instructions (~14 KB of a 32 KB instruction cache)
#if defined(__vita__)
#define TE_PV_INLINE __attribute__((always_inline)) inline
#else
#define TE_PV_INLINE
#endif

using Vector = ParticleViewMath::Vector;
using Matrix = ParticleViewMath::Matrix;

constexpr uint32_t kSign = 0x80000000u;
constexpr uint32_t kMaxFloat = 0x7F7FFFFFu;

TE_PV_INLINE uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
TE_PV_INLINE float Float(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

TE_PV_INLINE float Clamp(float v) {
    const uint32_t b = Bits(v), e = b & 0x7F800000u;
    if (e == 0x7F800000u) return Float((b & kSign) | kMaxFloat);
    if (e == 0) return Float(b & kSign);
    return v;
}
TE_PV_INLINE Vector Clamp4(const Vector& v) { return {Clamp(v[0]), Clamp(v[1]), Clamp(v[2]), Clamp(v[3])}; }

// The MAC S flags of a raw result (x 0x80, y 0x40, z 0x20, w 0x10)
TE_PV_INLINE uint16_t SignFlags(const Vector& raw) {
    uint16_t s = 0;
    for (int k = 0; k < 4; ++k) if (Bits(raw[k]) & kSign) s |= uint16_t(0x80 >> k);
    return s;
}

// a * s (broadcast) + acc, every lane, fused
Vector MaddBc(const Vector& acc, const Vector& a, float s) {
    const Vector ca = Clamp4(a), cacc = Clamp4(acc);
    const float cs = Clamp(s);
    Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = Clamp(VuFloat::Fma(ca[k], cs, cacc[k]));
    return r;
}
Vector MulBc(const Vector& a, float s) {
    const Vector ca = Clamp4(a);
    const float cs = Clamp(s);
    Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = Clamp(ca[k] * cs);
    return r;
}
// m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3] * v.w in the programs' order (MULAx, MADDAy, MADDAz, MADDw)
Vector RowTimes(const Vector& m0, const Vector& m1, const Vector& m2, const Vector& m3, const Vector& v) {
    Vector acc = MulBc(m0, v[0]);
    acc = MaddBc(acc, m1, v[1]);
    acc = MaddBc(acc, m2, v[2]);
    return MaddBc(acc, m3, v[3]);
}
// The plane group's centre (n . place + d, four planes in the lanes) and radius (|n| . extents); the flags the group's two
// FMANDs AND together: the signs of centre + radius and centre - radius
uint32_t GroupFlags(const Vector* qw, const Vector& extents, const Vector& place) {
    Vector radius = MulBc(qw[0], extents[0]);
    radius = MaddBc(radius, qw[1], extents[1]);
    radius = MaddBc(radius, qw[2], extents[2]);
    Vector centre = MulBc(qw[3], place[0]);
    centre = MaddBc(centre, qw[4], place[1]);
    centre = MaddBc(centre, qw[5], place[2]);
    centre = MaddBc(centre, qw[6], 1.f);
    Vector plus{}, minus{};
    for (int k = 0; k < 4; ++k) {
        plus[k] = VuFloat::Fma(radius[k], 1.f, centre[k]);
        minus[k] = VuFloat::Fma(-radius[k], 1.f, centre[k]);
    }
    return uint32_t(SignFlags(plus) & 0xF0) & uint32_t(SignFlags(minus) & 0xF0);
}
float Sqrt(float x) { return Clamp(std::sqrt(std::fabs(Clamp(x)))); }
float Div(float num, float den) {
    num = Clamp(num); den = Clamp(den);
    if (den == 0.f) return num == 0.f ? Float((Bits(num) ^ Bits(den)) & kSign) : Float(((Bits(num) ^ Bits(den)) & kSign) | kMaxFloat);
    return Clamp(num / den);
}

}  // namespace

ParticleViewMath::Result ParticleViewMath::BlockView(const View& view, const Vector& extents, const Matrix& gravity, const Vector& place,
                                                     float scale, bool keepsTranslation) {
    RoundingGuard rounding;
    const Vector* q = view.qw.data();
    const Vector camera = Clamp4(q[14]);
    const Vector p = Clamp4(place);
    Result out{};
    // vf27 = place - camera (0xA08) / vf29 = camera - place (0xB90); |d|^2 summed x + y then + z
    Vector d{};
    for (int k = 0; k < 4; ++k) d[k] = Clamp(keepsTranslation ? camera[k] - p[k] : p[k] - camera[k]);
    Vector sq{};
    for (int k = 0; k < 4; ++k) sq[k] = Clamp(d[k] * d[k]);
    const float len2 = Clamp(Clamp(sq[0] + sq[1]) + sq[2]);
    const float len = Sqrt(len2);
    out.distance = Clamp(0.f + len);
    const uint32_t flags1 = GroupFlags(q + 0, extents, p);
    const uint32_t flags2 = GroupFlags(q + 7, extents, p);
    out.flags = flags1 | flags2;
    const float inverse = Div(1.f, out.distance);
    // the pull: unit(d) * scale over xyz (w stays 0: d.w is place.w - camera.w, both 1)
    Vector pull{};
    for (int k = 0; k < 3; ++k) pull[k] = Clamp(Clamp(d[k] * inverse) * Clamp(scale));
    pull[3] = Clamp(d[3] * Clamp(scale));
    Vector at{};
    if (keepsTranslation) {
        at = {pull[0], pull[1], pull[2], p[3]};   // vf6.xyz = unit(camera - place) * scale, its w the place's
    } else {
        for (int k = 0; k < 4; ++k) at[k] = Clamp(p[k] - pull[k]);   // vf6 = place - unit(place - camera) * scale, w 1
    }
    for (int r = 0; r < 3; ++r)
        out.matrix[r] = keepsTranslation ? q[15 + r] : RowTimes(q[15], q[16], q[17], q[18], gravity[r]);
    out.matrix[3] = RowTimes(q[15], q[16], q[17], q[18], at);
    return out;
}

}  // namespace title
