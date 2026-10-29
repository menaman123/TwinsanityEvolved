#pragma once
// (the Vita's builders) WHEN THE VU-FAITHFUL CLAMPS ARE THE IDENTITY -- so that the clamp-free form of a vertex's
// arithmetic computes the clamped form's bits.
//
// The geometry clamps every result (Finite: a denormal to a signed zero, an infinity or a NaN to +/-FLT_MAX). On the
// Cortex-A9 each clamp moves the result to a core register and back (the integer pipeline waits for the VFP's chained
// multiply-add every time): ~35 clamps a vertex, most of its cost. A clamp is the identity on every zero and every normal
// number, so a computation whose clamped values are all zeros or normal numbers gives the same bits without its clamps
// (the same operations on the same operands in the same rounding mode: by induction over the operations). Two ways to
// know that, without the per-value transfer:
//  * A BUDGET on the operands' exponents, checked once per build, for the multiply-add chains of a transform:
//    v0 = m3, v_k = fl(fl(m_k p_k) + v_{k-1}), k = 1..3 (the product rounded, then the sum: VuFloat::Fma on the Vita).
//    With every nonzero matrix entry |m| >= 2^Mlo and every nonzero position component |p| >= 2^Plo, b = Mlo + min(Plo, 0),
//    G = 2^(b - 23): every nonzero product is >= 2^(Mlo + Plo) (rounding is monotone and the power of two is a float), so a
//    normal number whose ulp -- and hence the product itself -- is a multiple of 2^(Mlo + Plo - 23); v0 = m3 is a multiple
//    of 2^(Mlo - 23); both multiples of G. A sum of two multiples of G is one, and so is its rounding (under 2^24 G it is
//    exact; above, the float's ulp is at least 2G), nonzero ones at least G in magnitude: every nonzero v_k is >= G. So
//    b >= -103 keeps every product and every v_k a zero or a normal number. With |m| < 2^(Mhi + 1), |p| < 2^(Phi + 1):
//    |v_k| < 2^(Mhi + max(Phi, 0) + 4) -- under 2^127 every value is finite. A product of a chain result by g (|g| in
//    [2^Glo, 2^(Ghi + 1))) and the reciprocal of a chain result are bounded likewise.
//  * An AUDIT of the stored results a budget cannot bound (a chain result times a reciprocal): an integer scan of their bits
//    after the pass (memory, not a register transfer per value) -- none denormal, infinite or NaN.
// Either failing, the caller computes the build with the clamped forms. Not for a fused multiply-add (one rounding: the
// chain argument is the Vita's two-rounding form); the callers use it on the Vita only.
#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace title {

// The exponents of a set of values: the smallest and largest unbiased exponent of the nonzero ones (a denormal counts as
// -127 and an infinity or a NaN as 128: below / above every budget); lo INT_MAX, hi INT_MIN while every value is zero
struct ExponentRange {
    int lo = INT_MAX, hi = INT_MIN;
    void AddBits(uint32_t b) {
        if (!(b & 0x7FFFFFFFu)) return;
        const int e = int((b >> 23) & 0xFFu) - 127;
        lo = std::min(lo, e);
        hi = std::max(hi, e);
    }
    void Add(const float* values, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            uint32_t b;
            std::memcpy(&b, values + i, 4);
            AddBits(b);
        }
    }
    void Add(const ExponentRange& o) {
        lo = std::min(lo, o.lo);
        hi = std::max(hi, o.hi);
    }
    bool AllZero() const { return lo == INT_MAX; }
};
// A transform's chain (matrix range m, position range p) and what it may be scaled by: the smallest exponent its nonzero
// results reach (INT_MAX: all zero) and the largest they stay below (exclusive)
struct ChainBudget {
    int lo = INT_MAX, hi = INT_MIN;
    bool ok = false;   // every intermediate of the chain normal or zero, every one finite
    ChainBudget(const ExponentRange& m, const ExponentRange& p) {
        if (m.AllZero()) { ok = true; return; }   // every value zero (v0 = 0, every product 0)
        const int b = m.lo + std::min(p.AllZero() ? 0 : p.lo, 0);
        lo = b - 23;
        hi = m.hi + std::max(p.AllZero() ? 0 : p.hi, 0) + 4;
        ok = lo >= -126 && hi <= 126;
    }
    // the chain's results times a factor of range g: every nonzero product normal and finite
    bool ScaledOk(const ExponentRange& g) const {
        if (!ok) return false;
        if (lo == INT_MAX || g.AllZero()) return true;
        return lo + g.lo >= -126 && hi + g.hi + 1 <= 126;
    }
    // the reciprocal of a nonzero chain result normal and finite (1 / |w| in (2^-hi, 2^-lo])
    bool ReciprocalOk() const { return ok && (lo == INT_MAX || (-lo <= 126 && hi <= 126)); }
};
// The audit: none of the stored floats is denormal, infinite or NaN (zeros and normal numbers: their clamp's identity)
inline bool AllClampFree(const float* values, size_t count, size_t stride) {
    uint32_t bad = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t b;
        std::memcpy(&b, values + i * stride, 4);
        const uint32_t e = b & 0x7F800000u;
        bad |= uint32_t(e == 0x7F800000u) | uint32_t(e == 0 && (b & 0x007FFFFFu) != 0);
    }
    return bad == 0;
}

}  // namespace title
