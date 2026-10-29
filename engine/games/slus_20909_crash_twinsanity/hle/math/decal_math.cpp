#include "games/slus_20909_crash_twinsanity/hle/math/decal_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"
#include "engine/vu/vu_float.h"

// The decal set's programs 0x1C0 and 0x360 mirrored bundle by bundle over a register file, with the VU's rules as
// title::RayMath keeps them: every FMAC reads clamped operands (NaN/Inf -> +/-MAX, denormal -> +/-0) and writes clamped
// lanes, rounding toward zero (RoundingGuard); MADD/MSUB are single-rounding fused multiply-adds; the MAC sign flags are
// judged on the raw lanes written (a write to vf00 is dropped, its flags kept); ITOF/FTOI convert the raw lanes (FTOI
// saturating); MAX compares raw bits; within a bundle each word reads the register file as the bundle found it.
//
// Both programs are straight-line, so their timing is fixed: each FMAND follows an ABS vf00 that stalls on the FMAC before
// it (or, in 0x1C0's first, the four cycles 0x308's own stall leaves), so it reads that FMAC's flags -- the near plane's
// test and the four side planes' -- and 0x1C0's ADDq issues on the SQRT's seventh cycle and reads the new Q (0x360 waits
// for it). Pinned to the interpreter by test_vu0_decal_age.

namespace title {
namespace {

using Vector = DecalMath::Vector;

constexpr uint32_t kSign = 0x80000000u;
constexpr uint32_t kMaxFloat = 0x7F7FFFFFu;
constexpr uint8_t X = 8, Y = 4, Z = 2, W = 1, XZ = X | Z, XYZ = X | Y | Z, XYZW = 15;

uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
float Float(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

float Clamp(float v) {
    const uint32_t b = Bits(v), e = b & 0x7F800000u;
    if (e == 0x7F800000u) return Float((b & kSign) | kMaxFloat);
    if (e == 0) return Float(b & kSign);
    return v;
}
Vector Clamp4(const Vector& v) { return {Clamp(v[0]), Clamp(v[1]), Clamp(v[2]), Clamp(v[3])}; }

uint32_t FtoiSat(float f) {
    const uint32_t b = Bits(f);
    if ((b & 0x7F800000u) >= 0x4F000000u) return (b & kSign) ? 0x80000000u : 0x7FFFFFFFu;
    return uint32_t(int32_t(f));
}

// The register file's clamps as a compile-time switch: C = true is the VU's rule (every FMAC operand and result clamped);
// C = false leaves them out -- DecalMath::Age takes that form only where no clamp can act (see there), the same values
template <bool C> inline float Cl(float v) { if constexpr (C) return Clamp(v); else return v; }
template <bool C> inline Vector Cl4(const Vector& v) { if constexpr (C) return Clamp4(v); else return v; }

template <bool C>
struct VuT {
    const Vector* data;
    Vector vf[32]{};
    Vector acc{};
    float i = 0.f, q = 0.f;
    uint16_t vi1 = 0, vi2 = 0;
};

// ---- the upper forms (each re-clamps ACC first, as the FMAC prologue does) ---------------------------------------------
template <bool C>
__attribute__((noinline)) Vector SrcClamped(VuT<C>& u, int reg) { u.acc = Clamp4(u.acc); return Clamp4(u.vf[reg]); }
// (the unclamped form a plain read, inlined at every instruction; the clamped one a call, as before)
template <bool C>
inline __attribute__((always_inline)) Vector Src(VuT<C>& u, int reg) {
    if constexpr (C) return SrcClamped(u, reg);
    else return u.vf[reg];
}
template <bool C>
float Bc(const VuT<C>& u, int reg, int lane) { return Cl<C>(u.vf[reg][lane]); }

template <bool C>
uint16_t Put(VuT<C>& u, int fd, const Vector& raw, uint8_t mask) {
    uint16_t s = 0;
    for (int k = 0; k < 4; ++k) {
        if (!(mask & (8 >> k))) continue;
        if (Bits(raw[k]) & kSign) s |= uint16_t(0x80 >> k);
        if (fd != 0) u.vf[fd][k] = Cl<C>(raw[k]);
    }
    return s;
}
template <bool C>
void PutAcc(VuT<C>& u, const Vector& raw, uint8_t mask) {
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.acc[k] = Cl<C>(raw[k]);
}

template <bool C>
void AddS(VuT<C>& u, int fd, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] + s;
    Put(u, fd, r, mask);
}
template <bool C>
void SubS(VuT<C>& u, int fd, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] - s;
    Put(u, fd, r, mask);
}
template <bool C>
void MulS(VuT<C>& u, int fd, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] * s;
    Put(u, fd, r, mask);
}
template <bool C>
void SubV(VuT<C>& u, int fd, int fs, int ft, uint8_t mask) {
    const Vector a = Src(u, fs), b = Cl4<C>(u.vf[ft]); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] - b[k];
    Put(u, fd, r, mask);
}
template <bool C>
void MulV(VuT<C>& u, int fd, int fs, int ft, uint8_t mask) {
    const Vector a = Src(u, fs), b = Cl4<C>(u.vf[ft]); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] * b[k];
    Put(u, fd, r, mask);
}
template <bool C>
void MaddS(VuT<C>& u, int fd, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(a[k], s, u.acc[k]);
    Put(u, fd, r, mask);
}
template <bool C>
uint16_t MsubS(VuT<C>& u, int fd, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(-a[k], s, u.acc[k]);
    return Put(u, fd, r, mask);
}
template <bool C>
void AccAddS(VuT<C>& u, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] + s;
    PutAcc(u, r, mask);
}
template <bool C>
void AccMulS(VuT<C>& u, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = a[k] * s;
    PutAcc(u, r, mask);
}
template <bool C>
void AccMaddS(VuT<C>& u, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(a[k], s, u.acc[k]);
    PutAcc(u, r, mask);
}
template <bool C>
void AccMsubS(VuT<C>& u, int fs, float s, uint8_t mask) {
    const Vector a = Src(u, fs); Vector r{};
    for (int k = 0; k < 4; ++k) r[k] = VuFloat::Fma(-a[k], s, u.acc[k]);
    PutAcc(u, r, mask);
}
// ITOFn / FTOIn of the raw lanes (scale 1, 4096, 32768)
template <bool C>
void Itof(VuT<C>& u, int ft, int fs, float scale, uint8_t mask) {
    if constexpr (C) u.acc = Clamp4(u.acc);
    const Vector a = u.vf[fs];
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.vf[ft][k] = float(int32_t(Bits(a[k]))) / scale;
}
template <bool C>
void Ftoi(VuT<C>& u, int ft, int fs, float scale, uint8_t mask) {
    if constexpr (C) u.acc = Clamp4(u.acc);
    const Vector a = u.vf[fs];
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.vf[ft][k] = Float(FtoiSat(a[k] * scale));
}
// MAX of raw bits as sign-magnitude integers (no flags)
uint32_t MaxBits(uint32_t a, uint32_t b) {
    const int32_t ka = (a & kSign) ? -int32_t(a & 0x7FFFFFFFu) - 1 : int32_t(a);
    const int32_t kb = (b & kSign) ? -int32_t(b & 0x7FFFFFFFu) - 1 : int32_t(b);
    return ka >= kb ? a : b;
}
template <bool C>
void MaxV(VuT<C>& u, int fd, int fs, int ft, uint8_t mask) {
    if constexpr (C) u.acc = Clamp4(u.acc);
    const Vector a = u.vf[fs], b = u.vf[ft];
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.vf[fd][k] = Float(MaxBits(Bits(a[k]), Bits(b[k])));
}
template <bool C>
void MaxBc(VuT<C>& u, int fd, int fs, int ft, int bc, uint8_t mask) {
    if constexpr (C) u.acc = Clamp4(u.acc);
    const Vector a = u.vf[fs];
    const uint32_t b = Bits(u.vf[ft][bc]);
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.vf[fd][k] = Float(MaxBits(Bits(a[k]), b));
}

// ---- the lower forms -------------------------------------------------------------------------------------------------
template <bool C>
void Lq(VuT<C>& u, int ft, uint16_t vi, int imm, uint8_t mask) {
    const Vector& m = u.data[(uint32_t(vi) + uint32_t(imm)) & (DecalMath::kDataQuadwords - 1)];
    for (int k = 0; k < 4; ++k)
        if (mask & (8 >> k)) u.vf[ft][k] = m[k];
}
template <bool C>
uint16_t Mtir(const VuT<C>& u, int fs, int lane) { return uint16_t(Bits(u.vf[fs][lane])); }
template <bool C>
void Sqrt(VuT<C>& u, float x) { u.q = Cl<C>(std::sqrt(std::fabs(Cl<C>(x)))); }

template <bool C>
DecalMath::Look Out(const VuT<C>& u) {
    DecalMath::Look look;
    for (int k = 0; k < 4; ++k) {
        look.place[k] = Bits(u.vf[28][k]);
        look.colour[k] = Bits(u.vf[29][k]);
        look.offsets[k] = Bits(u.vf[31][k]);
        look.more[k] = Bits(u.vf[30][k]);
    }
    return look;
}

// 0x1C0: a decal in the camera's chunk
template <bool C>
DecalMath::Look SameChunk(VuT<C>& u) {
    Lq(u, 28, u.vi1, 5, W);                                   // 01c0
    MulS(u, 1, 1, Bc(u, 28, 3), W); u.i = 0.5f;               // 01c8: the life spent
    AddS(u, 29, 0, Cl<C>(u.i), XZ);                           // 01d0
    SubS(u, 1, 0, Bc(u, 1, 3), W); u.i = 0.5f;                // 01d8: SUBw.w vf1, vf0, vf1w
    AddS(u, 30, 0, Cl<C>(u.i), XZ);                           // 01e0
    SubS(u, 31, 0, Cl<C>(u.i), X);                            // 01e8
    AddS(u, 31, 0, Cl<C>(u.i), Z); u.i = 3.0f;                // 01f0
    MulS(u, 1, 1, Cl<C>(u.i), W);                             // 01f8: in thirds
    Ftoi(u, 29, 1, 1.f, W);                                   // 0200
    u.vi2 = Mtir(u, 29, 3); Itof(u, 29, 29, 1.f, W);          // 0208: the key
    u.vi1 = uint16_t(u.vi2 + u.vi1);                          // 0210
    Lq(u, 4, u.vi1, 4, XZ);                                   // 0218
    SubV(u, 1, 1, 29, W); u.vi2 = 240;                        // 0220: the share between the keys
    Itof(u, 3, 3, 32768.f, XYZW); Lq(u, 5, u.vi1, 5, XZ);     // 0228
    Itof(u, 2, 2, 32768.f, XYZW); Lq(u, 6, u.vi1, 0, XYZW);   // 0230
    AccAddS(u, 4, Bc(u, 0, 0), XZ);                           // 0238
    AccMsubS(u, 4, Bc(u, 1, 3), XZ); Lq(u, 4, u.vi1, 1, XYZW);// 0240
    MaddS(u, 5, 5, Bc(u, 1, 3), XZ);                          // 0248: the size
    AccAddS(u, 6, Bc(u, 0, 0), XYZW);                         // 0250
    AccMsubS(u, 6, Bc(u, 1, 3), XYZW);                        // 0258
    MaddS(u, 7, 4, Bc(u, 1, 3), XYZW);                        // 0260: the colour
    MulV(u, 6, 5, 29, XZ);                                    // 0268
    MulS(u, 4, 2, Bc(u, 5, 0), XYZW);                         // 0270
    MulS(u, 3, 3, Bc(u, 5, 2), XYZW);                         // 0278
    Ftoi(u, 29, 7, 1.f, XYZW);                                // 0280
    MulV(u, 7, 6, 6, XZ);                                     // 0288
    AccMulS(u, 4, Bc(u, 31, 0), XYZ);                         // 0290
    MaddS(u, 2, 3, Bc(u, 31, 2), XYZ); Lq(u, 31, 0, 5, XYZW); // 0298
    AccMulS(u, 4, Bc(u, 30, 0), XYZW); Lq(u, 5, 0, 7, XYZW);  // 02a0
    AddS(u, 4, 7, Bc(u, 7, 2), X); Lq(u, 6, 0, 4, XYZW);      // 02a8: the half extents' square
    MaddS(u, 3, 3, Bc(u, 30, 2), XYZW); Lq(u, 30, 0, 6, XYZW);// 02b8
    AccMulS(u, 5, Bc(u, 0, 3), XYZW);                         // 02c0: the near plane's distance
    AccMaddS(u, 6, Bc(u, 1, 0), XYZW); Sqrt(u, u.vf[4][0]);   // 02c8
    AccMaddS(u, 31, Bc(u, 1, 1), XYZW); Lq(u, 6, 0, 0, XYZW); // 02d0
    AccMaddS(u, 30, Bc(u, 1, 2), XYZW);                       // 02d8
    Ftoi(u, 30, 3, 4096.f, XYZW); u.vf[3][3] = u.vf[0][0];    // 02e0 (MR32.w vf3, vf0)
    Ftoi(u, 31, 2, 4096.f, XYZ);                              // 02f0
    MaxBc(u, 2, 0, 0, 3, XYZW);                               // 02f8
    AddS(u, 1, 3, u.q, W); Lq(u, 3, 0, 3, XYZW);              // 0300: the radius
    const uint16_t nearFlags = MsubS(u, 0, 2, Bc(u, 1, 3), XYZW); Lq(u, 4, 0, 1, XYZW);   // 0308
    AccMulS(u, 3, Bc(u, 0, 3), XYZW); Lq(u, 3, 0, 2, XYZW);   // 0310: the side planes' distances
    AccMaddS(u, 6, Bc(u, 1, 0), XYZW); u.vi1 = 240;           // 0320
    AccMaddS(u, 4, Bc(u, 1, 1), XYZW); u.vi1 = uint16_t(nearFlags & u.vi1);   // 0328
    AccMaddS(u, 3, Bc(u, 1, 2), XYZW);                        // 0330
    const uint16_t sideFlags = MsubS(u, 8, 2, Bc(u, 1, 3), XYZW);             // 0338
    u.vi2 = uint16_t(sideFlags & u.vi2);                      // 0340
    MaxV(u, 28, 1, 1, XYZ); u.vi1 = uint16_t(u.vi2 | u.vi1);  // 0350
    MaxV(u, 28, 0, 0, W); u.vf[31][3] = Float(uint32_t(int32_t(int16_t(u.vi1))));   // 0358
    return Out(u);
}

// 0x360: a decal placed in another chunk (its place through that chunk's matrix first)
template <bool C>
DecalMath::Look OtherChunk(VuT<C>& u) {
    Itof(u, 2, 2, 32768.f, XYZW); Lq(u, 31, 0, 8, XYZ);       // 0360
    Itof(u, 3, 3, 32768.f, XYZW); Lq(u, 29, 0, 9, XYZ);       // 0368
    Lq(u, 29, u.vi1, 5, W);                                   // 0370
    AccMulS(u, 31, Bc(u, 1, 0), XYZ); Lq(u, 30, 0, 10, XYZ);  // 0378
    AccMaddS(u, 29, Bc(u, 1, 1), XYZ); Lq(u, 29, 0, 11, XYZ); // 0380
    MulS(u, 1, 1, Bc(u, 29, 3), W);                           // 0388: the life spent
    AccMaddS(u, 30, Bc(u, 1, 2), XYZ);                        // 0390
    MaddS(u, 1, 29, Bc(u, 0, 3), XYZ);                        // 0398: the place in this chunk
    SubS(u, 1, 0, Bc(u, 1, 3), W); u.i = 0.5f;               // 03a0
    AddS(u, 29, 0, Cl<C>(u.i), XZ); u.i = 0.5f;               // 03a8
    AddS(u, 30, 0, Cl<C>(u.i), XZ);                           // 03b0
    SubS(u, 31, 0, Cl<C>(u.i), X);                            // 03b8
    AddS(u, 31, 0, Cl<C>(u.i), Z); u.i = 3.0f;                // 03c0
    MulS(u, 1, 1, Cl<C>(u.i), W);                             // 03c8
    Ftoi(u, 29, 1, 1.f, W);                                   // 03d0
    u.vi2 = Mtir(u, 29, 3); Itof(u, 29, 29, 1.f, W);          // 03d8
    u.vi1 = uint16_t(u.vi2 + u.vi1);                          // 03e0
    Lq(u, 28, u.vi1, 4, XZ);                                  // 03e8
    SubV(u, 1, 1, 29, W); Lq(u, 4, u.vi1, 5, XZ);             // 03f0
    AccAddS(u, 28, Bc(u, 0, 0), XZ); Lq(u, 5, u.vi1, 0, XYZW);// 0408
    AccMsubS(u, 28, Bc(u, 1, 3), XZ); u.vi2 = 240;            // 0410
    MaddS(u, 4, 4, Bc(u, 1, 3), XZ);                          // 0418: the size
    AccAddS(u, 5, Bc(u, 0, 0), XYZW); Lq(u, 28, u.vi1, 1, XYZW);   // 0420
    AccMsubS(u, 5, Bc(u, 1, 3), XYZW); u.vi1 = 240;           // 0428
    MulV(u, 5, 4, 29, XZ);                                    // 0430
    MaddS(u, 6, 28, Bc(u, 1, 3), XYZW);                       // 0438: the colour
    MulS(u, 28, 2, Bc(u, 4, 0), XYZW);                        // 0440
    MulV(u, 5, 5, 5, XZ);                                     // 0448
    MulS(u, 2, 3, Bc(u, 4, 2), XYZW);                         // 0450
    AccMulS(u, 28, Bc(u, 31, 0), XYZ);                        // 0458
    AddS(u, 4, 5, Bc(u, 5, 2), X); Lq(u, 5, 0, 0, XYZW);      // 0460
    MaddS(u, 3, 2, Bc(u, 31, 2), XYZ); Lq(u, 31, 0, 3, XYZW); // 0468
    AccMulS(u, 28, Bc(u, 30, 0), XYZW); Lq(u, 28, 0, 1, XYZW);// 0470
    Sqrt(u, u.vf[4][0]);                                      // 0478
    MaddS(u, 30, 2, Bc(u, 30, 2), XYZW); Lq(u, 2, 0, 2, XYZW);// 0480
    AccMulS(u, 31, Bc(u, 0, 3), XYZW); Lq(u, 4, 0, 7, XYZW);  // 0488: the side planes' distances
    AccMaddS(u, 5, Bc(u, 1, 0), XYZW); u.vf[1][3] = u.vf[0][0];   // 0490 (MR32.w vf1, vf0)
    AccMaddS(u, 28, Bc(u, 1, 1), XYZW);                       // 0498
    AccMaddS(u, 2, Bc(u, 1, 2), XYZW);                        // 04a0
    MaxBc(u, 2, 0, 0, 3, XYZW);                               // 04a8
    AddS(u, 1, 1, u.q, W);                                    // 04b0: the radius (WAITQ)
    Ftoi(u, 31, 3, 4096.f, XYZ); Lq(u, 3, 0, 4, XYZW);        // 04c0
    const Vector moved = u.vf[1];
    const uint16_t sideFlags = MsubS(u, 7, 2, Bc(u, 1, 3), XYZW); u.vf[28] = moved;   // 04c8 (MOVE vf28, vf1)
    AccMulS(u, 4, Bc(u, 0, 3), XYZW); Lq(u, 5, 0, 5, XYZW);   // 04d0: the near plane's distance
    AccMaddS(u, 3, Bc(u, 1, 0), XYZW); Lq(u, 3, 0, 6, XYZW);  // 04d8
    u.vi1 = uint16_t(sideFlags & u.vi1);                      // 04e0
    AccMaddS(u, 5, Bc(u, 1, 1), XYZW);                        // 04e8
    AccMaddS(u, 3, Bc(u, 1, 2), XYZW);                        // 04f0
    const uint16_t nearFlags = MsubS(u, 8, 2, Bc(u, 1, 3), XYZW);   // 04f8
    u.vi2 = uint16_t(nearFlags & u.vi2);                      // 0500
    Ftoi(u, 29, 6, 1.f, XYZW); u.vi1 = uint16_t(u.vi1 | u.vi2);   // 0508
    Ftoi(u, 30, 30, 4096.f, XYZW); u.vf[31][3] = Float(uint32_t(int32_t(int16_t(u.vi1))));   // 0510
    return Out(u);
}

}  // namespace

#if defined(__vita__)
namespace {
// The calling thread rounds toward zero with flush-to-zero on (FPSCR / FPCR: FZ bit 24, RMode bits 23:22 = 0b11)
bool FlushToZeroTowardZero() {
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
#else
    uint32_t fpcr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpcr));
#endif
    return ((fpcr >> 24) & 1u) && ((fpcr >> 22) & 3u) == 3u;
}
}  // namespace
#endif

DecalMath::Look DecalMath::Age(const Vector* data, uint32_t program, const Decal& decal, bool dataFinite, bool* unclamped) {
    RoundingGuard rounding;
    if (unclamped) *unclamped = false;
#if defined(__vita__)
    // The clamps left out where none can act: under round-toward-zero (the guard's) with flush-to-zero (the game thread's
    // FPSCR: RZ | FZ | DN, device dump) and finite inputs -- the data image (the caller vouches: dataFinite) and the
    // decal's place; the frame's half words only reach ITOF -- every FMAC result is finite (an overflow rounds to the
    // largest finite number, never to an infinity; no NaN can arise), never a denormal (the hardware flushes it to a zero
    // of its sign, which is what the clamp would make of it), and every FMAC operand is such a result or a finite input
    // (a denormal input reads as a zero of its sign in the hardware too). So each clamp is the identity: the same
    // operations on the same values, bit for bit (host check: decaltest, both forms over random images and decals).
    // On the Cortex-A9 every clamp is a VFP-to-core transfer and a branch: ~13 of them per VU instruction.
    if (dataFinite) {
        uint32_t special = 0;
        for (int k = 0; k < 4; ++k) special |= uint32_t((Bits(decal.place[k]) & 0x7F800000u) == 0x7F800000u);
        if (FlushToZeroTowardZero() && !special) {
            VuT<false> u;
            u.data = data;
            u.vf[0] = {0.f, 0.f, 0.f, 1.f};
            u.vf[1] = decal.place;
            for (int k = 0; k < 4; ++k) {
                u.vf[2][k] = Float(uint32_t(decal.frame[k]));
                u.vf[3][k] = Float(uint32_t(decal.more[k]));
            }
            u.vi1 = decal.variant;
            if (unclamped) *unclamped = true;
            return program == kOtherChunk ? OtherChunk(u) : SameChunk(u);
        }
    }
#else
    (void)dataFinite;
#endif
    VuT<true> u;
    u.data = data;
    u.vf[0] = {0.f, 0.f, 0.f, 1.f};
    u.vf[1] = decal.place;
    for (int k = 0; k < 4; ++k) {
        u.vf[2][k] = Float(uint32_t(decal.frame[k]));
        u.vf[3][k] = Float(uint32_t(decal.more[k]));
    }
    u.vi1 = decal.variant;
    return program == kOtherChunk ? OtherChunk(u) : SameChunk(u);
}

#if defined(DECAL_MATH_TEST_UNCLAMPED)
// decaltest (a host check): the unclamped form itself, whatever the platform, for comparing the two forms bit for bit
DecalMath::Look DecalMath::AgeUnclampedForTest(const Vector* data, uint32_t program, const Decal& decal) {
    RoundingGuard rounding;
    VuT<false> u;
    u.data = data;
    u.vf[0] = {0.f, 0.f, 0.f, 1.f};
    u.vf[1] = decal.place;
    for (int k = 0; k < 4; ++k) {
        u.vf[2][k] = Float(uint32_t(decal.frame[k]));
        u.vf[3][k] = Float(uint32_t(decal.more[k]));
    }
    u.vi1 = decal.variant;
    return program == kOtherChunk ? OtherChunk(u) : SameChunk(u);
}
#endif

}  // namespace title
