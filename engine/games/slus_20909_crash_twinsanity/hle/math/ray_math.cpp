#include "games/slus_20909_crash_twinsanity/hle/math/ray_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine/vu/vu_float.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

// The standard VU0 set's program 0xAB8 (the fast ray/triangle test of TR collision.cpp's CheckCollisionRayCastFast), its
// 88 bundles mirrored one by one as value arithmetic over a register file -- the VU's rules kept: every FMAC reads
// clamped operands (NaN/Inf -> +/-MAX, denormal -> +/-0) and writes clamped lanes, rounding toward zero (RoundingGuard);
// MADD/MSUB/OPMSUB are single-rounding fused multiply-adds; the MAC sign flags are judged on the RAW result of the lanes
// written; RSQRT is num / sqrt(|den|) and DIV num / den on clamped operands with the by-zero rule; R is RINIT/RGET's
// constant 1.0 trick; the lower word of a bundle reads the register file BEFORE that bundle's upper writes it.
//
// What the program computes (the names below follow the registers): n = (v1-v0) x (v2-v0); the three edge normals
// e_i = n x edge_i normalised (world units); ds / de = the ray's ends' signed distances to the plane; the hit at
// share = |ds| / (|ds| + |de|); the flag word VI2 = ((sign(ds) + sign(de)) * 0x80 + (share < nearest.w) * 0x10) << 4
// | (the edge planes' distances of the hit + 0.1 negative) * {0x80, 0x40, 0x20} -- kNearer (0x900) exactly when the ends
// are on opposite sides of the plane, the hit is nearer than the nearest so far and lies within 0.1 units of the
// triangle's edges (the margin the provisional port body did not have). The MAC flags an FMAND reads are those of the
// FMAC that retired four VU cycles earlier; the register hazards of this straight-line program stall it at fixed places,
// so each FMAND / Q read is bound to a fixed producer below -- pinned to the interpreter by test_vu0_ray_triangle.

namespace title {
namespace {

using Vector = RayMath::Vector;

constexpr uint32_t kSign = 0x80000000u;
constexpr uint32_t kMaxFloat = 0x7F7FFFFFu;

uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
float Float(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

// The VU clamp of one lane: NaN/Inf -> +/-MAX_FLOAT, denormal -> +/-0
float Clamp(float v) {
    const uint32_t b = Bits(v), e = b & 0x7F800000u;
    if (e == 0x7F800000u) return Float((b & kSign) | kMaxFloat);
    if (e == 0) return Float(b & kSign);
    return v;
}
Vector Clamp4(const Vector& v) { return {Clamp(v[0]), Clamp(v[1]), Clamp(v[2]), Clamp(v[3])}; }

// The register file of one run (a record; the program's operations are the free functions below)
struct Vu {
    Vector vf[32]{};
    Vector acc{};
    float q = 0.f;        // Q as the program's consumers see it (set at the bound producer, see Test)
    float r = 0.f;        // R
    float i = 0.f;        // I
    uint32_t vi1 = 0, vi2 = 0;
};
constexpr uint8_t X = 8, Y = 4, Z = 2, W = 1;

// An FMAC result `raw` (every lane computed) written to vf[fd] over `mask`; the lanes' S flags (x 0x80, y 0x40, z 0x20,
// w 0x10) kept as `flags` when asked
void Write(Vu& u, int fd, const Vector& raw, uint8_t mask, uint16_t* flags) {
    uint16_t s = 0;
    for (int k = 0; k < 4; ++k) {
        const uint8_t bit = uint8_t(8 >> k);
        if (!(mask & bit)) continue;
        if (Bits(raw[k]) & kSign) s |= uint16_t(0x80 >> k);
        if (fd != 0) u.vf[fd][k] = Clamp(raw[k]);
    }
    if (flags) *flags = s;
}
void WriteAcc(Vu& u, const Vector& raw, uint8_t mask, uint16_t* flags) {
    uint16_t s = 0;
    for (int k = 0; k < 4; ++k) {
        const uint8_t bit = uint8_t(8 >> k);
        if (!(mask & bit)) continue;
        if (Bits(raw[k]) & kSign) s |= uint16_t(0x80 >> k);
        u.acc[k] = Clamp(raw[k]);
    }
    if (flags) *flags = s;
}
Vector S(const Vu& u, int reg) { return Clamp4(u.vf[reg]); }
void ClampAcc(Vu& u) { u.acc = Clamp4(u.acc); }

// ---- the upper forms this program uses (fd, fs, ft; bc = the broadcast lane of ft) -----------------------------------
void Add(Vu& u, int fd, int fs, int ft, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs), b = S(u, ft); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] + b[k];
    Write(u, fd, r, mask, f);
}
void Sub(Vu& u, int fd, int fs, int ft, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs), b = S(u, ft); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] - b[k];
    Write(u, fd, r, mask, f);
}
void Mul(Vu& u, int fd, int fs, int ft, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs), b = S(u, ft); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] * b[k];
    Write(u, fd, r, mask, f);
}
void AddBc(Vu& u, int fd, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs); const float s = S(u, ft)[bc]; Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] + s;
    Write(u, fd, r, mask, f);
}
void SubBc(Vu& u, int fd, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs); const float s = S(u, ft)[bc]; Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] - s;
    Write(u, fd, r, mask, f);
}
void MulQ(Vu& u, int fd, int fs, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] * u.q;
    Write(u, fd, r, mask, f);
}
void MaddBc(Vu& u, int fd, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs); const float s = S(u, ft)[bc]; Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(a[k], s, u.acc[k]);
    Write(u, fd, r, mask, f);
}
void MaddQ(Vu& u, int fd, int fs, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(a[k], u.q, u.acc[k]);
    Write(u, fd, r, mask, f);
}
void MaddI(Vu& u, int fd, int fs, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs); const float s = Clamp(u.i); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(a[k], s, u.acc[k]);
    Write(u, fd, r, mask, f);
}
void Abs(Vu& u, int fd, int fs, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = Float(Bits(u.vf[fs][k]) & ~kSign);   // raw source lanes
    Write(u, fd, r, mask, f);
}
// MAX fd, fs, ft: raw integer compare, no flags, no clamp
void Max(Vu& u, int fd, int fs, int ft, uint8_t mask) {
    ClampAcc(u);
    for (int k = 0; k < 4; ++k) {
        if (!(mask & (8 >> k))) continue;
        const uint32_t a = Bits(u.vf[fs][k]), b = Bits(u.vf[ft][k]);
        const int32_t ka = (a & kSign) ? -int32_t(a & 0x7FFFFFFFu) - 1 : int32_t(a), kb = (b & kSign) ? -int32_t(b & 0x7FFFFFFFu) - 1 : int32_t(b);
        if (fd != 0) u.vf[fd][k] = Float(ka >= kb ? a : b);
    }
}
void AddABc(Vu& u, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs); const float s = S(u, ft)[bc]; Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] + s;
    WriteAcc(u, r, mask, f);
}
void MulABc(Vu& u, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs); const float s = S(u, ft)[bc]; Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] * s;
    WriteAcc(u, r, mask, f);
}
void MaddABc(Vu& u, int fs, int ft, int bc, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs); const float s = S(u, ft)[bc]; Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(a[k], s, u.acc[k]);
    WriteAcc(u, r, mask, f);
}
void MsubAQ(Vu& u, int fs, uint8_t mask, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(-a[k], u.q, u.acc[k]);
    WriteAcc(u, r, mask, f);
}
void Opmula(Vu& u, int fs, int ft, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs), b = S(u, ft);
    Vector r{a[1] * b[2], a[2] * b[0], a[0] * b[1], u.acc[3]};
    WriteAcc(u, r, X | Y | Z, f);
}
void Opmsub(Vu& u, int fd, int fs, int ft, uint16_t* f = nullptr) {
    ClampAcc(u); const Vector a = S(u, fs), b = S(u, ft);
    Vector r{VuFloat::Fma(-a[1], b[2], u.acc[0]), VuFloat::Fma(-a[2], b[0], u.acc[1]), VuFloat::Fma(-a[0], b[1], u.acc[2]), u.acc[3]};
    Write(u, fd, r, X | Y | Z, f);
}
// ---- the FDIV pipe (the interpreter's bodies) ----------------------------------------------------------------------
float Rsqrt(float num, float den) {
    num = Clamp(num); den = Clamp(den);
    const uint32_t sign = (Bits(num) ^ Bits(den)) & kSign;
    if (den == 0.f) return num != 0.f ? Float(sign | kMaxFloat) : Float(sign);
    return Clamp(num / std::sqrt(std::fabs(den)));
}
float Div(float num, float den) {
    num = Clamp(num); den = Clamp(den);
    if (den == 0.f) return num == 0.f ? Float((Bits(num) ^ Bits(den)) & kSign) : Float(((Bits(num) ^ Bits(den)) & kSign) | kMaxFloat);
    return Clamp(num / den);
}
// RINIT R, vf0w: the constant 1.0 the program fetches with RGET
void Rinit(Vu& u) { u.r = Float(0x3F800000u | (Bits(u.vf[0][3]) & 0x007FFFFFu)); }


#if defined(__vita__)
// The Vita's test: the program's arithmetic in plain floats, in its order (no clamped operands, the CPU's rounding, no
// fused multiply-adds) -- the faithful register file costs the Vita's game thread a tenth of a hub frame in ray casts.
// Only a degenerate triangle or ray keeps the program's by-zero rules (RSQRT and DIV give MAX_FLOAT and 0).
RayMath::Result FastTest(const RayMath::Ray& ray, const RayMath::Triangle& t, const Vector& nearest) {
    const auto cross = [](const float* a, const float* b, float* out) {
        out[0] = a[1] * b[2] - b[1] * a[2];
        out[1] = a[2] * b[0] - b[2] * a[0];
        out[2] = a[0] * b[1] - b[0] * a[1];
    };
    const auto dot = [](const float* a, const float* b) { return (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]; };
    const auto rsqrt = [](float den) { return den == 0.f ? Float(kMaxFloat) : 1.f / std::sqrt(std::fabs(den)); };
    const float* v0 = t[0].data();
    const float* v1 = t[1].data();
    const float* v2 = t[2].data();
    const float e01[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
    const float e02[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
    const float e12[3] = {v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2]};
    const float e20[3] = {v0[0] - v2[0], v0[1] - v2[1], v0[2] - v2[2]};
    float n[3], c12[3], c01[3], c20[3];
    cross(e01, e02, n);
    cross(n, e12, c12);
    cross(n, e01, c01);
    cross(n, e20, c20);
    const float q1 = rsqrt(dot(n, n)), q2 = rsqrt(dot(c12, c12)), q3 = rsqrt(dot(c01, c01)), q4 = rsqrt(dot(c20, c20));
    float nn[3], n12[3], n01[3], n20[3];
    for (int k = 0; k < 3; ++k) { nn[k] = n[k] * q1; n12[k] = c12[k] * q2; n01[k] = c01[k] * q3; n20[k] = c20[k] * q4; }
    const float plane = 0.f - dot(nn, v0);
    const float ds = dot(ray.start.data(), nn) + plane, de = dot(ray.end.data(), nn) + plane;
    const float sum = std::fabs(ds) + std::fabs(de);
    const float share = sum == 0.f ? 0.f : std::fabs(ds) / sum;
    RayMath::Result out;
    for (int k = 0; k < 3; ++k) out.hit[size_t(k)] = (ray.start[size_t(k)] - ray.start[size_t(k)] * share) + ray.end[size_t(k)] * share;
    out.hit[3] = share;
    const float offset[3] = {0.f - dot(n01, v0), 0.f - dot(n12, v2), 0.f - dot(n20, v0)};
    const float* normal[3] = {n01, n12, n20};
    uint32_t edges = 0;
    for (int k = 0; k < 3; ++k) {
        const float d = (((offset[k] + normal[k][0] * out.hit[0]) + normal[k][1] * out.hit[1]) + normal[k][2] * out.hit[2]) + 0.1f;
        if (Bits(d) & kSign) edges |= 0x80u >> k;
    }
    const uint32_t sides = ((Bits(ds) & kSign) ? 0x80u : 0u) + ((Bits(de) & kSign) ? 0x80u : 0u);
    const uint32_t nearer = (Bits(share - nearest[3]) & kSign) ? 0x10u : 0u;
    out.flags = ((((sides + nearer) << 4) & 0xFFFFu) | edges);
    out.nearer = out.flags == RayMath::kNearer;
    return out;
}

// FastTest's answer to the seam's question alone (RayMath::Nearer). kNearer is exactly one end's distance negative (the
// sides 0x80), the share's difference from the nearest negative (0x10) and no edge distance negative: the plane's normal,
// the plane and the ends' distances first; the share when the ends are on the two sides; the edge normals and the hit
// when the share is the nearer. The Academy hub's walk (316114 tests from its start view on): 67.4 % end at the sides,
// 1.7 % at the share, 30.8 % reach the edges, 0.2 % are nearer.
bool NearerFast(const RayMath::Ray& ray, const RayMath::Triangle& t, const Vector& nearest, Vector& hit) {
    const auto cross = [](const float* a, const float* b, float* out) {
        out[0] = a[1] * b[2] - b[1] * a[2];
        out[1] = a[2] * b[0] - b[2] * a[0];
        out[2] = a[0] * b[1] - b[0] * a[1];
    };
    const auto dot = [](const float* a, const float* b) { return (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]; };
    const auto rsqrt = [](float den) { return den == 0.f ? Float(kMaxFloat) : 1.f / std::sqrt(std::fabs(den)); };
    const float* v0 = t[0].data();
    const float* v1 = t[1].data();
    const float* v2 = t[2].data();
    const float e01[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
    const float e02[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
    float n[3];
    cross(e01, e02, n);
    const float q1 = rsqrt(dot(n, n));
    float nn[3];
    for (int k = 0; k < 3; ++k) nn[k] = n[k] * q1;
    const float plane = 0.f - dot(nn, v0);
    const float ds = dot(ray.start.data(), nn) + plane, de = dot(ray.end.data(), nn) + plane;
    if (((Bits(ds) ^ Bits(de)) & kSign) == 0) return false;   // the ends on one side (sides 0 or 0x100)
    const float sum = std::fabs(ds) + std::fabs(de);
    const float share = sum == 0.f ? 0.f : std::fabs(ds) / sum;
    if (!(Bits(share - nearest[3]) & kSign)) return false;   // not below the nearest so far
    const float e12[3] = {v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2]};
    const float e20[3] = {v0[0] - v2[0], v0[1] - v2[1], v0[2] - v2[2]};
    float c12[3], c01[3], c20[3];
    cross(n, e12, c12);
    cross(n, e01, c01);
    cross(n, e20, c20);
    const float q2 = rsqrt(dot(c12, c12)), q3 = rsqrt(dot(c01, c01)), q4 = rsqrt(dot(c20, c20));
    float n12[3], n01[3], n20[3];
    for (int k = 0; k < 3; ++k) { n12[k] = c12[k] * q2; n01[k] = c01[k] * q3; n20[k] = c20[k] * q4; }
    Vector at;
    for (int k = 0; k < 3; ++k) at[size_t(k)] = (ray.start[size_t(k)] - ray.start[size_t(k)] * share) + ray.end[size_t(k)] * share;
    at[3] = share;
    const float offset[3] = {0.f - dot(n01, v0), 0.f - dot(n12, v2), 0.f - dot(n20, v0)};
    const float* normal[3] = {n01, n12, n20};
    for (int k = 0; k < 3; ++k) {
        const float d = (((offset[k] + normal[k][0] * at[0]) + normal[k][1] * at[1]) + normal[k][2] * at[2]) + 0.1f;
        if (Bits(d) & kSign) return false;   // past an edge by more than the margin
    }
    hit = at;
    return true;
}
#endif

}  // namespace

RayMath::Result RayMath::Test(const Ray& ray, const Triangle& t, const Vector& nearest) {
#if defined(__vita__)
    return FastTest(ray, t, nearest);
#endif
    RoundingGuard rounding;
    Vu u;
    u.vf[0] = {0.f, 0.f, 0.f, 1.f};
    u.vf[1] = t[0]; u.vf[2] = t[1]; u.vf[3] = t[2];
    u.vf[4] = ray.start; u.vf[5] = ray.end; u.vf[6] = nearest;
        constexpr int x = 0, y = 1, z = 2, w = 3;
    uint16_t fDs = 0, fDe = 0, fNearer = 0, fEdges = 0;   // the FMAC flags the four FMANDs read
    float q1, q2, q3, q4;                                   // the RSQRT results in issue order; q5 the DIV's
    // 1  SUB.xyz vf6,vf2,vf1            | IADDIU vi1,vi0,0x80
    Sub(u, 6, 2, 1, X | Y | Z); u.vi1 = 0x80;
    // 2  SUB.xyz vf7,vf3,vf1
    Sub(u, 7, 3, 1, X | Y | Z);
    // 3  OPMULA.xyz ACC,vf6,vf7 ; 4 OPMSUB.xyz vf8,vf7,vf6      (vf8 = n = (v1-v0) x (v2-v0))
    Opmula(u, 6, 7); Opmsub(u, 8, 7, 6);
    // 5  MUL.xyz vf7,vf8,vf8
    Mul(u, 7, 8, 8, X | Y | Z);
    // 8  SUB.xyz vf2,vf3,vf2            (vf2 = v2 - v1)
    Sub(u, 2, 3, 2, X | Y | Z);
    // 9  ADDy.x vf7,vf7,vf7y
    AddBc(u, 7, 7, 7, y, X);
    // 10 OPMULA.xyz ACC,vf8,vf2
    Opmula(u, 8, 2);
    // 11 ADDz.x vf11,vf7,vf7z           (vf11.x = |n|^2)
    AddBc(u, 11, 7, 7, z, X);
    // 12 OPMSUB.xyz vf2,vf2,vf8         (vf2 = n x (v2-v1))
    Opmsub(u, 2, 2, 8);
    // 13 SUB.xyz vf31,vf1,vf3           | RSQRT Q,vf0w,vf11x
    q1 = Rsqrt(u.vf[0][w], u.vf[11][x]);
    Sub(u, 31, 1, 3, X | Y | Z);
    // 14 MUL.xyz vf10,vf2,vf2 ; 15 ADDy.x vf10,vf10,vf10y
    Mul(u, 10, 2, 2, X | Y | Z); AddBc(u, 10, 10, 10, y, X);
    // 18 OPMULA.xyz ACC,vf8,vf6 ; 19 ADDz.x vf10,vf10,vf10z ; 20 OPMSUB.xyz vf6,vf6,vf8   (vf6 = n x (v1-v0))
    Opmula(u, 8, 6); AddBc(u, 10, 10, 10, z, X); Opmsub(u, 6, 6, 8);
    // 21 OPMULA.xyz ACC,vf8,vf31 ; 22 OPMSUB.xyz vf31,vf31,vf8   (vf31 = n x (v0-v2))
    Opmula(u, 8, 31); Opmsub(u, 31, 31, 8);
    // 23 MULq.xyz vf8,vf8,Q             | RSQRT Q,vf0w,vf10x      (Q = q1: vf8 = n normalised)
    q2 = Rsqrt(u.vf[0][w], u.vf[10][x]);
    u.q = q1; MulQ(u, 8, 8, X | Y | Z);
    // 24 MUL.xyz vf9,vf6,vf6 ; 25 MUL.xyz vf7,vf31,vf31 ; 26 MUL.xyz vf11,vf8,vf1
    Mul(u, 9, 6, 6, X | Y | Z); Mul(u, 7, 31, 31, X | Y | Z); Mul(u, 11, 8, 1, X | Y | Z);
    // 27 ADDy.x vf9,vf9,vf9y ; 28 MUL.xyz vf10,vf4,vf8 ; 29 MUL.xyz vf8,vf5,vf8
    AddBc(u, 9, 9, 9, y, X); Mul(u, 10, 4, 8, X | Y | Z); Mul(u, 8, 5, 8, X | Y | Z);
    // 30 ADDy.x vf12,vf11,vf11y ; 31 ADDz.x vf11,vf9,vf9z ; 32 ADDy.x vf10,vf10,vf10y
    AddBc(u, 12, 11, 11, y, X); AddBc(u, 11, 9, 9, z, X); AddBc(u, 10, 10, 10, y, X);
    // 33 ADDz.x vf9,vf12,vf11z          | MR32.w vf31,vf0        (vf9.x = n.v0 ; vf31.w = 0)
    { const Vector pre0 = u.vf[0]; AddBc(u, 9, 12, 11, z, X); u.vf[31][w] = pre0[x]; }
    // 34 ADDy.x vf8,vf8,vf8y            | RSQRT Q,vf0w,vf11x
    q3 = Rsqrt(u.vf[0][w], u.vf[11][x]);
    AddBc(u, 8, 8, 8, y, X);
    // 35 MULq.xyz vf2,vf2,Q             (Q = q2: the edge normal of v1->v2 normalised)
    u.q = q2; MulQ(u, 2, 2, X | Y | Z);
    // 36 ADDz.x vf10,vf10,vf10z         (vf10.x = n.start)
    AddBc(u, 10, 10, 10, z, X);
    // 37 SUBx.w vf31,vf31,vf9x          (vf31.w = -n.v0)
    SubBc(u, 31, 31, 9, x, W);
    // 38 ADDz.x vf11,vf8,vf8z           (vf11.x = n.end)
    AddBc(u, 11, 8, 8, z, X);
    // 39 ADDy.x vf9,vf7,vf7y
    AddBc(u, 9, 7, 7, y, X);
    // 40 MUL.xyz vf8,vf2,vf3
    Mul(u, 8, 2, 3, X | Y | Z);
    // 41 ADDw.x vf3,vf10,vf31w          (vf3.x = ds: the start's signed distance to the plane)  -> FMAND vi2 (bundle 45)
    AddBc(u, 3, 10, 31, w, X, &fDs);
    // 42 ADDw.x vf7,vf11,vf31w          | RINIT R,vf0w          (vf7.x = de)  -> FMAND vi1 (bundle 46)
    AddBc(u, 7, 11, 31, w, X, &fDe); Rinit(u);
    // 43 ADDz.x vf9,vf9,vf7z            | RGET.x vf10           (vf9.x = |n x (v0-v2)|^2 ; vf10.x = 1.0)
    { const float pre7z = u.vf[7][z]; (void)pre7z; AddBc(u, 9, 9, 7, z, X); u.vf[10][x] = u.r; }
    // 44 ADDAy.x ACC,vf8,vf8y           | MR32.w vf31,vf0        (vf31.w = 0)
    { const Vector pre0 = u.vf[0]; AddABc(u, 8, 8, y, X); u.vf[31][w] = pre0[x]; }
    // 45 ABS.x vf3,vf3                  | FMAND vi2,vi1          (vi2 = sign(ds) ? 0x80 : 0 ; vf3.x = |ds|)
    Abs(u, 3, 3, X); u.vi2 = u.vi1 & fDs;
    // 46 MAX.w vf5,vf0,vf0              | FMAND vi1,vi1          (vi1 = sign(de) ? 0x80 : 0 ; vf5.w = 1)
    Max(u, 5, 0, 0, W); u.vi1 = u.vi1 & fDe;
    // 47 MADDz.x vf8,vf10,vf8z          | RSQRT Q,vf0w,vf9x      (vf8.x = e2.v2)
    q4 = Rsqrt(u.vf[0][w], u.vf[9][x]);
    MaddBc(u, 8, 10, 8, z, X);
    // 51 ABS.x vf10,vf7 ; 52 ADD.x vf10,vf3,vf10               (vf10.x = |ds| + |de|)
    Abs(u, 10, 7, X); Add(u, 10, 3, 10, X);
    // 56 MULq.xyz vf6,vf6,Q             (Q = q3: the edge normal of v0->v1 normalised)
    u.q = q3; MulQ(u, 6, 6, X | Y | Z);
    // 57 MULq.xyz vf3,vf31,Q            | DIV Q,vf3x,vf10x       (Q = q4: the edge normal of v2->v0 normalised; the DIV reads the pre-upper vf3.x = |ds|)
    const float q5 = Div(u.vf[3][x], u.vf[10][x]);
    u.q = q4; MulQ(u, 3, 31, X | Y | Z);
    // 59 SUBx.w vf31,vf31,vf8x          | MR32.w vf4,vf0         (vf31.w = -e2.v2 ; vf4.w = 0)
    { const Vector pre0 = u.vf[0]; SubBc(u, 31, 31, 8, x, W); u.vf[4][w] = pre0[x]; }
    // 60 MUL.xyz vf10,vf3,vf1           | RINIT R,vf0w
    Mul(u, 10, 3, 1, X | Y | Z); Rinit(u);
    // 61 MUL.xyz vf7,vf6,vf1            | RGET.x vf8             (vf8.x = 1.0)
    Mul(u, 7, 6, 1, X | Y | Z); u.vf[8][x] = u.r;
    // 62 ADDAx.xyzw ACC,vf4,vf0x        | RINIT R,vf0w           (ACC = start, w 0)
    AddABc(u, 4, 0, x, X | Y | Z | W); Rinit(u);
    // 63 MSUBAq.xyzw ACC,vf4,Q          | RGET.x vf9             (Q = the DIV's share ; vf9.x = 1.0)
    u.q = q5; MsubAQ(u, 4, X | Y | Z | W); u.vf[9][x] = u.r;
    // 64 MADDq.xyzw vf31,vf5,Q          | MR32.z vf1,vf31        (vf31 = start + (end - start) * share, w = share ; vf1.z = the pre-upper vf31.w = -e2.v2)
    { const Vector pre31 = u.vf[31]; MaddQ(u, 31, 5, X | Y | Z | W); u.vf[1][z] = pre31[w]; }
    // 65 ADDAy.x ACC,vf7,vf7y
    AddABc(u, 7, 7, y, X);
    // 66 MADDz.x vf8,vf8,vf7z           | MR32.w vf2,vf0         (vf8.x = e0.v0 ; vf2.w = 0)
    { const Vector pre0 = u.vf[0]; MaddBc(u, 8, 8, 7, z, X); u.vf[2][w] = pre0[x]; }
    // 67 ADDAy.x ACC,vf10,vf10y         | MR32.w vf1,vf0         (vf1.w = 0)
    { const Vector pre0 = u.vf[0]; AddABc(u, 10, 10, y, X); u.vf[1][w] = pre0[x]; }
    // 68 MADDz.x vf7,vf9,vf10z          | IADD vi2,vi1,vi2       (vf7.x = e1.v2 ; vi2 = sign(ds) + sign(de))
    MaddBc(u, 7, 9, 10, z, X); u.vi2 = (u.vi1 + u.vi2) & 0xFFFF;
    // 69 SUB.w vf0,vf31,vf6             | MR32.y vf1,vf6         (the flags of share - nearest.w -> FMAND vi1 (bundle 73) ; vf1.y = the pre-upper vf6.z)
    { const Vector pre6 = u.vf[6]; Sub(u, 0, 31, 6, W, &fNearer); u.vf[1][y] = pre6[z]; }
    // 70 SUBx.w vf2,vf2,vf8x            | MR32.x vf1,vf6         (vf2.w = -e0.v0 ; vf1.x = vf6.y)
    { const Vector pre6 = u.vf[6]; SubBc(u, 2, 2, 8, x, W); u.vf[1][x] = pre6[y]; }
    // 71 NOP                            | MR32.y vf6,vf2         (vf6.y = vf2.z)
    u.vf[6][y] = u.vf[2][z];
    // 72 SUBx.w vf6,vf1,vf7x            | IADDIU vi1,vi0,0x10    (vf6.w = -e1.v2)
    SubBc(u, 6, 1, 7, x, W); u.vi1 = 0x10;
    // 73 ADDy.z vf2,vf0,vf3y            | FMAND vi1,vi1          (vf2.z = e2.y ; vi1 = (share < nearest.w) ? 0x10 : 0)
    AddBc(u, 2, 0, 3, y, Z); u.vi1 = u.vi1 & fNearer;
    // 74 ADDw.x vf7,vf0,vf2w            | MOVE.z vf6,vf3         (vf7.x = -e0.v0 ; vf6.z = the pre-upper vf3.z)
    { const Vector pre3 = u.vf[3]; AddBc(u, 7, 0, 2, w, X); u.vf[6][z] = pre3[z]; }
    // 75 ADDx.z vf3,vf0,vf3x            | MR32.y vf7,vf1         (vf3.z = e2.x ; vf7.y = the pre-upper vf1.z = -e2.v2)
    { const Vector pre1 = u.vf[1]; AddBc(u, 3, 0, 3, x, Z); u.vf[7][y] = pre1[z]; }
    // 76 MAX.x vf3,vf6,vf6              | MR32.z vf7,vf6         (vf3.x = vf6.x = e0.x ; vf7.z = the pre-upper vf6.w = -e1.v2)
    { const Vector pre6 = u.vf[6]; Max(u, 3, 6, 6, X); u.vf[7][z] = pre6[w]; }
    // 77 ADDx.y vf3,vf0,vf2x            (vf3.y = e1.x)
    AddBc(u, 3, 0, 2, x, Y);
    // 78 MAX.x vf2,vf1,vf1              (vf2.x = vf1.x = e0.y)
    Max(u, 2, 1, 1, X);
    // 79 ADDy.x vf6,vf0,vf1y            | RINIT R,vf0w           (vf6.x = e0.z)
    AddBc(u, 6, 0, 1, y, X); Rinit(u);
    // 80 MULAw.xyz ACC,vf7,vf0w         | RGET.xyz vf1           (ACC = the edge planes' offsets ; vf1.xyz = 1.0)
    MulABc(u, 7, 0, w, X | Y | Z); u.vf[1][x] = u.vf[1][y] = u.vf[1][z] = u.r;
    // 81 MADDAx.xyz ACC,vf3,vf31x       | LOI 0.1
    MaddABc(u, 3, 31, x, X | Y | Z); u.i = 0.1f;
    // 82 MADDAy.xyz ACC,vf2,vf31y       | IADD vi2,vi2,vi1
    MaddABc(u, 2, 31, y, X | Y | Z); u.vi2 = (u.vi2 + u.vi1) & 0xFFFF;
    // 83 MADDAz.xyz ACC,vf6,vf31z       | IADD vi2,vi2,vi2
    MaddABc(u, 6, 31, z, X | Y | Z); u.vi2 = (u.vi2 + u.vi2) & 0xFFFF;
    // 84 MADDi.xyz vf13,vf1,I           | IADD vi2,vi2,vi2       (vf13.xyz = the hit's distances to the edge planes + 0.1) -> FMAND vi1 (bundle 88)
    MaddI(u, 13, 1, X | Y | Z, &fEdges); u.vi2 = (u.vi2 + u.vi2) & 0xFFFF;
    // 85 NOP | IADD vi2,vi2,vi2 ; 86 NOP | IADD vi2,vi2,vi2
    u.vi2 = (u.vi2 + u.vi2) & 0xFFFF; u.vi2 = (u.vi2 + u.vi2) & 0xFFFF;
    // 87 NOP                            | IADDIU vi1,vi0,0xe0
    u.vi1 = 0xE0;
    // 88 ABS.xyz vf0,vf13 [E]           | FMAND vi1,vi1          (the write to vf0 dropped ; vi1 = the edge planes' negative distances' bits)
    Abs(u, 0, 13, X | Y | Z); u.vi1 = u.vi1 & fEdges;
    // 89 NOP                            | IOR vi2,vi1,vi2        (the E bit's delay slot)
    u.vi2 = (u.vi1 | u.vi2) & 0xFFFF;

    Result out;
    out.flags = u.vi2;
    out.nearer = u.vi2 == kNearer;
    out.hit = u.vf[31];
    return out;
}

bool RayMath::Nearer(const Ray& ray, const Triangle& t, const Vector& nearest, Vector& hit) {
#if defined(__vita__)
    return NearerFast(ray, t, nearest, hit);
#else
    const Result r = Test(ray, t, nearest);
    if (r.nearer) hit = r.hit;
    return r.nearer;
#endif
}

}  // namespace title
