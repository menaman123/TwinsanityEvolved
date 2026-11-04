// platform/nativeee (host side) -- the PURE half of include/platform/math.h and the VU0 bounds functions (PS2 bodies
// src/platform/ps2/{bounds,math}.cpp): the ones whose semantics the decomp states exactly, without a VU0 microprogram.
// Added with the vtable thunks: with their weak stubs (return 0, outputs untouched)
// a zero-height hull box made CastHullDown's step loop never end on the first chunk, so the title was never reached.
//   PointsBox / TriangleBounds / GetBbox  -- VU0 vmini/vmax over the points (GetBbox after a row-vector transform, in
//                                            the asm's vmulax/vmadday/vmaddaz/vmaddw accumulation order)
//   SinCos                                 -- the game's own SinCosPolynomial (include/platform/math.h: the microprogram's
//                                            degree-9 polynomial, bit for bit what the game's CPU path computes)
//   DivideBySquareRoot                     -- a / sqrt(b) (the EE's RSQRT.S; its last-bit rounding is the oracle's to pin)
// The microprogram functions (JointMatrix 0x548/0x668/0x790, ViewDistance 0xCB0, SetRay/StartRayTriangle/FinishRayTriangle
// 0xAB8, ParticleBlockView, Slerp/Euler/Turn) stay with the VU0-math bodies: they need the microcode semantics.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__vita__)
#include <arm_neon.h>
#endif

#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_host.h"
#include "te_prof.h"

namespace {

struct V4 {
    float x, y, z, w;
};
struct Box {
    V4 min, max;
};
struct M4 {
    V4 r[4];
};

const V4* Vecs(uint32_t a, uint32_t count) { return reinterpret_cast<const V4*>(te::GuestPointer(a, sizeof(V4) * count)); }
Box* BoxW(uint32_t a) { return reinterpret_cast<Box*>(te::GuestPointer(a, sizeof(Box))); }
const M4* Mat(uint32_t a) { return reinterpret_cast<const M4*>(te::GuestPointer(a, sizeof(M4))); }

// The Vita: the bounds' per-point helpers inline in the loops (GCC kept Transform, TakeMinBits and TakeMaxBits as calls in
// get_bbox's loop -- three calls a point and the transformed point copied through the stack between them); the same
// operations in the same order
#if defined(__vita__)
#define TE_BOUNDS_INLINE __attribute__((always_inline)) inline
#else
#define TE_BOUNDS_INLINE
#endif

// vmini.xyz / vmax.xyz: w keeps the first point's (StartBox loads it into both registers)
#if defined(__vita__)
// newlib's fminf / fmaxf (as linked in the eboot: a NaN operand gives the other operand, else x < y ? x : y /
// x > y ? x : y), inline -- the same operand picked by the same comparison, bit for bit, without the two
// __fpclassifyf calls (each a VFP-to-core register transfer on the Cortex-A9) and the libcall per component
inline float FminNewlib(float x, float y) {
    if (std::isnan(x)) return y;
    if (std::isnan(y)) return x;
    return x < y ? x : y;
}
inline float FmaxNewlib(float x, float y) {
    if (std::isnan(x)) return y;
    if (std::isnan(y)) return x;
    return x > y ? x : y;
}
void TakeMin(V4& m, const V4& p) { m.x = FminNewlib(m.x, p.x); m.y = FminNewlib(m.y, p.y); m.z = FminNewlib(m.z, p.z); }
void TakeMax(V4& m, const V4& p) { m.x = FmaxNewlib(m.x, p.x); m.y = FmaxNewlib(m.y, p.y); m.z = FmaxNewlib(m.z, p.z); }
// The same picks on the stored bits, for a thread whose FPSCR flushes denormals to zero (FZ: the game thread's -- the
// compares above then read a denormal operand as a zero of its sign): a NaN operand gives the other operand, else the
// operands compare as signed magnitudes with every zero and denormal equal to zero (VCMP under FZ), and x < y ? x : y /
// x > y ? x : y return the operand's own bits. Each FminNewlib / FmaxNewlib was two VCMP + VMRS for the NaN tests and one
// for the compare (VFP-to-core flag transfers on the Cortex-A9: ~100 in a triangle's bounds). Host check:
// cpuwork/gamecpu2/minmaxtest (every special class x 10^8 random pairs under RZ|FZ|DN: the same bits).
struct FzKey {
    TE_BOUNDS_INLINE static bool Nan(uint32_t b) { return (b & 0x7FFFFFFFu) > 0x7F800000u; }
    TE_BOUNDS_INLINE static int32_t Key(uint32_t b) {
        const int32_t magnitude = (b & 0x7F800000u) ? int32_t(b & 0x7FFFFFFFu) : 0;
        return (b & 0x80000000u) ? -magnitude : magnitude;
    }
    TE_BOUNDS_INLINE static uint32_t Min(uint32_t x, uint32_t y) {
        if (Nan(x)) return y;
        if (Nan(y)) return x;
        return Key(x) < Key(y) ? x : y;
    }
    TE_BOUNDS_INLINE static uint32_t Max(uint32_t x, uint32_t y) {
        if (Nan(x)) return y;
        if (Nan(y)) return x;
        return Key(x) > Key(y) ? x : y;
    }
};
// The calling thread flushes denormals to zero (FPSCR.FZ); the integer picks are exact only then
bool FlushesToZero() {
    uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    return (fpscr >> 24) & 1u;
}
struct U4 {
    uint32_t x, y, z, w;
};
TE_BOUNDS_INLINE void TakeMinBits(U4& m, const U4& p) { m.x = FzKey::Min(m.x, p.x); m.y = FzKey::Min(m.y, p.y); m.z = FzKey::Min(m.z, p.z); }
TE_BOUNDS_INLINE void TakeMaxBits(U4& m, const U4& p) { m.x = FzKey::Max(m.x, p.x); m.y = FzKey::Max(m.y, p.y); m.z = FzKey::Max(m.z, p.z); }
// triangle_bounds' picks stay calls: inline there (two points, min and max, three lanes, all live together) GCC spilled
// ~100 registers to the stack
__attribute__((noinline)) void TakeMinBitsCall(U4& m, const U4& p) { TakeMinBits(m, p); }
__attribute__((noinline)) void TakeMaxBitsCall(U4& m, const U4& p) { TakeMaxBits(m, p); }
TE_BOUNDS_INLINE U4 BitsOf(const V4& v) { U4 u; std::memcpy(&u, &v, 16); return u; }
TE_BOUNDS_INLINE V4 FloatsOf(const U4& u) { V4 v; std::memcpy(&v, &u, 16); return v; }
// The integer picks apply: the fast game-thread paths on (TE_GAMECPU2) and the thread flushes to zero
bool BitsPicks() { return te::gfx::GameCpuFast() && FlushesToZero(); }
// FzKey::Min / Max on NEON integer lanes, the four lanes at once (bit tests, shifts, signed compares and selects -- no
// floating-point operation): per lane x NaN -> y, else y NaN -> x, else the smaller (larger) signed-magnitude key's
// operand, the second one on a tie -- FzKey's choices exactly; the callers keep the w lane as the scalar picks leave it.
// TakeMinBits / TakeMaxBits were ~12 core instructions a lane and a pick (a triangle's bounds: ~450 instructions)
TE_BOUNDS_INLINE int32x4_t FzKeys(uint32x4_t b) {
    const uint32x4_t magnitude = vandq_u32(b, vdupq_n_u32(0x7FFFFFFFu));
    // (a zero exponent -- a zero or a denormal -- keys as 0; the sign negates: (m ^ s) - s with s = 0 or -1)
    const uint32x4_t kept = vandq_u32(magnitude, vtstq_u32(b, vdupq_n_u32(0x7F800000u)));
    const int32x4_t sign = vshrq_n_s32(vreinterpretq_s32_u32(b), 31);
    return vsubq_s32(veorq_s32(vreinterpretq_s32_u32(kept), sign), sign);
}
TE_BOUNDS_INLINE uint32x4_t FzNans(uint32x4_t b) { return vcgtq_u32(vandq_u32(b, vdupq_n_u32(0x7FFFFFFFu)), vdupq_n_u32(0x7F800000u)); }
TE_BOUNDS_INLINE uint32x4_t FzMin(uint32x4_t x, uint32x4_t y) {
    uint32x4_t pick = vbslq_u32(vcltq_s32(FzKeys(x), FzKeys(y)), x, y);
    pick = vbslq_u32(FzNans(y), x, pick);
    return vbslq_u32(FzNans(x), y, pick);
}
TE_BOUNDS_INLINE uint32x4_t FzMax(uint32x4_t x, uint32x4_t y) {
    uint32x4_t pick = vbslq_u32(vcgtq_s32(FzKeys(x), FzKeys(y)), x, y);
    pick = vbslq_u32(FzNans(y), x, pick);
    return vbslq_u32(FzNans(x), y, pick);
}
// the lanes x, y, z of `picked` with the w lane of `keep`
TE_BOUNDS_INLINE uint32x4_t KeepW(uint32x4_t picked, uint32x4_t keep) {
    static const uint32_t kXyz[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u};
    return vbslq_u32(vld1q_u32(kXyz), picked, keep);
}
TE_BOUNDS_INLINE uint32x4_t LoadBits(const V4* v) { return vld1q_u32(reinterpret_cast<const uint32_t*>(v)); }
TE_BOUNDS_INLINE void StoreBits(V4* v, uint32x4_t b) { vst1q_u32(reinterpret_cast<uint32_t*>(v), b); }
#else
void TakeMin(V4& m, const V4& p) { m.x = std::fmin(m.x, p.x); m.y = std::fmin(m.y, p.y); m.z = std::fmin(m.z, p.z); }
void TakeMax(V4& m, const V4& p) { m.x = std::fmax(m.x, p.x); m.y = std::fmax(m.y, p.y); m.z = std::fmax(m.z, p.z); }
#endif

TE_BOUNDS_INLINE V4 Scale(const V4& v, float s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
TE_BOUNDS_INLINE V4 Add(const V4& a, const V4& b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
TE_BOUNDS_INLINE V4 Transform(const M4& m, const V4& p) {
    V4 acc = Scale(m.r[0], p.x);
    acc = Add(acc, Scale(m.r[1], p.y));
    acc = Add(acc, Scale(m.r[2], p.z));
    return Add(acc, Scale(m.r[3], p.w));
}

// include/platform/math.h CosinePolynomial, verbatim (the microprogram's steps)
float CosinePolynomial(float radians) {
    constexpr float MinusInverseTurn = -0x1.45F30Ep-3f;
    constexpr float Term1 = 0x1.921FB4p+2f;
    constexpr float Term3 = -0x1.4ABBC0p+5f;
    constexpr float Term5 = 0x1.4668AEp+6f;
    constexpr float Term7 = -0x1.324CC2p+6f;
    constexpr float Term9 = 0x1.3DAF6Ep+5f;
    float angle = std::fabs(radians);
    float turns = angle * MinusInverseTurn;
    float whole = -static_cast<float>(static_cast<int32_t>(-turns));
    float fraction = whole - turns;
    float t = std::fabs(fraction - 0.5f) - 0.25f;
    float t2 = t * t;
    float t4 = t2 * t2;
    float t8 = t4 * t4;
    float sum = Term3 * t * t2;
    sum += Term7 * t * t2 * t4;
    sum += Term5 * t * t4;
    sum += t * Term1;
    return sum + Term9 * t * t8;
}

}  // namespace

extern "C" {

void ps2e_vu0_points_box(ps2e_u32 points, ps2e_s32 count, ps2e_u32 box) {
    TE_SEAM(kSeamBounds);
    Box* out = BoxW(box);
    if (!out || count <= 0) return;
    const V4* p = Vecs(points, uint32_t(count));
    if (!p) return;
#if defined(__vita__)
    if (BitsPicks()) {   // the same picks on the stored bits (FzKey)
        if (te::gfx::SeamLean()) {   // (on NEON lanes; w: the first point's, read before any store)
            const uint32x4_t first = LoadBits(&p[0]);
            uint32x4_t lo = first, hi = first;
            for (int32_t i = 1; i < count; ++i) {
                const uint32x4_t q = LoadBits(&p[i]);
                lo = FzMin(lo, q);
                hi = FzMax(hi, q);
            }
            StoreBits(&out->min, KeepW(lo, first));
            StoreBits(&out->max, KeepW(hi, first));
            return;
        }
        U4 lo = BitsOf(p[0]), hi = lo;
        for (int32_t i = 1; i < count; ++i) { const U4 q = BitsOf(p[i]); TakeMinBits(lo, q); TakeMaxBits(hi, q); }
        out->min = FloatsOf(lo);
        out->max = FloatsOf(hi);
        return;
    }
#endif
    V4 lo = p[0], hi = p[0];
    for (int32_t i = 1; i < count; ++i) { TakeMin(lo, p[i]); TakeMax(hi, p[i]); }
    out->min = lo;
    out->max = hi;
}

void ps2e_vu0_triangle_bounds(ps2e_u32 box, ps2e_u32 first, ps2e_u32 second, ps2e_u32 third) {
    TE_SEAM(kSeamBounds);
    te::gfx::CountCall(te::gfx::kCallTriangleBounds);
    Box* out = BoxW(box);
    const V4* a = Vecs(first, 1);
    const V4* b = Vecs(second, 1);
    const V4* c = Vecs(third, 1);
    if (!out || !a || !b || !c) return;
    // "The corners' w stay what vf11 and vf12 had": the box's w components are left as they were
#if defined(__vita__)
    if (BitsPicks()) {   // the same picks on the stored bits (FzKey)
        if (te::gfx::SeamLean()) {   // (on NEON lanes: the three points read first, then the box's w lanes, then the stores)
            const uint32x4_t ua = LoadBits(a), ub = LoadBits(b), uc = LoadBits(c);
            const uint32x4_t lo = FzMin(FzMin(ua, ub), uc), hi = FzMax(FzMax(ua, ub), uc);
            const uint32x4_t keepMin = LoadBits(&out->min), keepMax = LoadBits(&out->max);
            StoreBits(&out->min, KeepW(lo, keepMin));
            StoreBits(&out->max, KeepW(hi, keepMax));
            return;
        }
        const U4 ua = BitsOf(*a), ub = BitsOf(*b), uc = BitsOf(*c);
        U4 lo = ua, hi = ua;
        TakeMinBitsCall(lo, ub); TakeMaxBitsCall(hi, ub);
        TakeMinBitsCall(lo, uc); TakeMaxBitsCall(hi, uc);
        V4 l = FloatsOf(lo), h = FloatsOf(hi);
        l.w = out->min.w;
        h.w = out->max.w;
        out->min = l;
        out->max = h;
        return;
    }
#endif
    V4 lo = *a, hi = *a;
    TakeMin(lo, *b); TakeMax(hi, *b);
    TakeMin(lo, *c); TakeMax(hi, *c);
    lo.w = out->min.w;
    hi.w = out->max.w;
    out->min = lo;
    out->max = hi;
}

void ps2e_vu0_get_bbox(ps2e_u32 points, ps2e_s32 count, ps2e_u32 box, ps2e_u32 matrix) {
    TE_SEAM(kSeamBounds);
    te::gfx::CountCall(te::gfx::kCallGetBbox);
    te::gfx::CountCall(te::gfx::kCallBboxPoints, count > 0 ? uint32_t(count) : 0u);
    Box* out = BoxW(box);
    const M4* m = Mat(matrix);
    if (!out || !m || count <= 0) return;
    const V4* p = Vecs(points, uint32_t(count));
    if (!p) return;
    if (te::gfx::g_repeatCensus) te::gfx::NoteRepeat(te::gfx::kCallGetBbox, m, sizeof *m, p, sizeof(V4) * size_t(count), &count, 4);
    V4 first = Transform(*m, p[0]);
    V4 lo = first, hi = first;
#if defined(__vita__)
    if (BitsPicks() && te::gfx::SeamLean()) {   // the same transforms, the same picks on NEON lanes (w: the first point's)
        const uint32x4_t firstBits = vld1q_u32(reinterpret_cast<const uint32_t*>(&first));
        uint32x4_t ulo = firstBits, uhi = firstBits;
        for (int32_t i = 1; i < count; ++i) {
            const V4 q = Transform(*m, p[i]);
            const uint32x4_t qb = vld1q_u32(reinterpret_cast<const uint32_t*>(&q));
            ulo = FzMin(ulo, qb);
            uhi = FzMax(uhi, qb);
        }
        ulo = KeepW(ulo, firstBits);
        uhi = KeepW(uhi, firstBits);
        vst1q_u32(reinterpret_cast<uint32_t*>(&lo), ulo);
        vst1q_u32(reinterpret_cast<uint32_t*>(&hi), uhi);
    } else if (BitsPicks()) {   // the same transforms, the same picks on the stored bits (FzKey)
        U4 ulo = BitsOf(first), uhi = ulo;
        for (int32_t i = 1; i < count; ++i) {
            const U4 q = BitsOf(Transform(*m, p[i]));
            TakeMinBits(ulo, q);
            TakeMaxBits(uhi, q);
        }
        lo = FloatsOf(ulo);
        hi = FloatsOf(uhi);
    } else
#endif
    for (int32_t i = 1; i < count; ++i) {
        V4 q = Transform(*m, p[i]);
        TakeMin(lo, q);
        TakeMax(hi, q);
    }
    out->min = lo;
    out->max = hi;
    // Diagnosis (TE_DIAG_BBOX=0xADDR[,0xADDR]): every bounds query over a hull blob at one of these guest addresses is printed
    // with its matrix row 3 and the box (the sphere-vs-instance-hull path: GetHullBoundingBox before BoxesOverlap). (Read once
    // into a plain value: a function-local static's guard was a DMB per call on the Vita.)
    static int diagRead = 0;
    static const char* diag = nullptr;
    if (!diagRead) { diagRead = 1; diag = std::getenv("TE_DIAG_BBOX"); }
    if (diag && std::strstr(diag, "0x")) {
        char want[16];
        std::snprintf(want, sizeof want, "0x%08X", points);
        char want2[16];
        std::snprintf(want2, sizeof want2, "0x%08x", points);
        if (std::strstr(diag, want) || std::strstr(diag, want2))
            std::fprintf(stderr, "[te.math] get_bbox blob 0x%08X n %d matrix row3 (%.3f %.3f %.3f) -> box (%.3f %.3f %.3f)-(%.3f %.3f %.3f)\n", points, count, m->r[3].x, m->r[3].y, m->r[3].z, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
    }
}

void ps2e_math_sin_cos(ps2e_f32 first, ps2e_f32 second, ps2e_u32 out) {
    float* o = reinterpret_cast<float*>(te::GuestPointer(out, 4 * sizeof(float)));
    if (!o) return;
    constexpr float ProgramHalfPi = 0x1.921FB0p+0f;
    o[0] = CosinePolynomial(first - ProgramHalfPi);
    o[1] = CosinePolynomial(first);
    o[2] = CosinePolynomial(second - ProgramHalfPi);
    o[3] = CosinePolynomial(second);
}

ps2e_f32 ps2e_math_divide_by_square_root(ps2e_f32 value, ps2e_f32 square) {
    // RSQRT.S: value / sqrt(|square|); the EE's FPU has no NaN/Inf -- a zero divisor gives the largest magnitude
    const float s = std::sqrt(std::fabs(square));
    if (s == 0.0f) return std::copysign(0x1.FFFFFEp+127f, value);
    return value / s;
}

}  // extern "C"
