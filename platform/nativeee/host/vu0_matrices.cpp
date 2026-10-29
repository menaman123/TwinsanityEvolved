// platform/nativeee (host side) -- the VU0 matrix half of the game's maths library (include/game/math.h, PS2 bodies
// src/platform/ps2/matrices.cpp: macro-mode asm), as the second seam's imports (tools/second_seam.tsv). PROVISIONAL: plain IEEE arithmetic in the PS2 sequences' order, so the UI's placed sprites and matrices work; the VU0 rounding
// (multiply-adds without guard bits, no NaN/Inf) and the retail-exact conventions are not pinned (the
// second seam). Matrices are four rows of four floats; vectors transform as ROW vectors (p' = p.x·row0 +
// p.y·row1 + p.z·row2 + row3), as the asm's vmulax/vmadday/vmaddaz/vmaddw sequences do.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "ps2e_seam.h"
#include "graphics_internal.h"
#include "te_host.h"
#include "te_math_diag.h"

namespace {

struct V4 {
    float x, y, z, w;
};
struct M4 {
    V4 r[4];
};

const M4* Mat(uint32_t a) { return reinterpret_cast<const M4*>(te::GuestPointer(a, sizeof(M4))); }
M4* MatW(uint32_t a) { return reinterpret_cast<M4*>(te::GuestPointer(a, sizeof(M4))); }
const V4* Vec(uint32_t a) { return reinterpret_cast<const V4*>(te::GuestPointer(a, sizeof(V4))); }
V4* VecW(uint32_t a) { return reinterpret_cast<V4*>(te::GuestPointer(a, sizeof(V4))); }

V4 Scale(const V4& v, float s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
V4 Add(const V4& a, const V4& b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }

// p.x·m0 + p.y·m1 + p.z·m2 + w·m3 in the asm's accumulation order
V4 RowCombine(const M4& m, const V4& p, float w4) {
    V4 acc = Scale(m.r[0], p.x);
    acc = Add(acc, Scale(m.r[1], p.y));
    acc = Add(acc, Scale(m.r[2], p.z));
    acc = Add(acc, Scale(m.r[3], w4));
    return acc;
}

// ---- diagnosis (te_math_diag.h): the retail VuTransformPoint / VuRotateVector on the engine's VU interpreter --------------
// A leaf function of the PAL executable: lqc2 loads from a0 (the matrix) / a1 (the vector), COP2 FMAC macro instructions, one
// sqc2 to a2. A COP2 FMAC word is the VU's upper instruction behind the COP2 prefix (bits 24..0: dest, ft, fs, fd, function),
// so each one runs alone on the interpreter -- bundle 2k the word with the E bit, bundle 2k+1 its delay slot -- in program
// order, the loads and the store done on the unit's register file between them.
constexpr uint32_t kUpperNop = 0x000002FFu, kLowerNop = 0x8000033Cu, kEndBit = 0x40000000u;

struct RetailMacro {
    struct Op {
        enum Kind { Load, Fmac, Store } kind;
        uint32_t reg = 0, base = 0, offset = 0, pc = 0;
    };
    const char* name = "";
    std::vector<Op> ops;
    unsigned loads = 0, fmacs = 0, stores = 0;
    bool ok = false;
    std::string why;

    // the words from the function's first instruction; its FMAC words written to the unit's micro memory from `pc`
    bool Decode(const uint32_t* w, size_t n, VuUnit& vu, uint32_t& pc) {
        ops.clear();
        size_t end = n;
        for (size_t i = 0; i < end; ++i) {
            const uint32_t word = w[i], opcode = word >> 26;
            char buf[96];
            if (word == 0x03E00008u) { end = i + 2 < n ? i + 2 : n; continue; }   // jr ra: its delay slot is the last word
            if (word == 0) continue;                                                 // nop
            if (opcode == 0x36 || opcode == 0x3E) {                                  // lqc2 / sqc2 vf, offset(base)
                Op o;
                o.kind = opcode == 0x36 ? Op::Load : Op::Store;
                o.base = (word >> 21) & 31u;
                o.reg = (word >> 16) & 31u;
                o.offset = word & 0xFFFFu;
                const bool from_matrix = o.kind == Op::Load && o.base == 4 && o.offset <= 0x30 && (o.offset & 15u) == 0;
                const bool from_vector = o.kind == Op::Load && o.base == 5 && o.offset == 0;
                const bool to_out = o.kind == Op::Store && o.base == 6 && o.offset == 0;
                if (!from_matrix && !from_vector && !to_out) {
                    std::snprintf(buf, sizeof buf, "+%zu: 0x%08X (a quadword access the decoder does not take)", i * 4, word);
                    why = buf;
                    return false;
                }
                (o.kind == Op::Load ? loads : stores) += 1;
                ops.push_back(o);
            } else if (opcode == 0x12 && (word & (1u << 25))) {                      // COP2 CO: an FMAC macro instruction
                const uint32_t upper = word & 0x01FFFFFFu;
                if (upper == kUpperNop) continue;                                    // vnop
                Op o;
                o.kind = Op::Fmac;
                o.pc = pc;
                uint8_t bundles[16];
                const uint32_t words[4] = {kLowerNop, upper | kEndBit, kLowerNop, kUpperNop};
                std::memcpy(bundles, words, sizeof bundles);
                vu.WriteMicroMem(pc, bundles, sizeof bundles);
                pc += 16;
                ++fmacs;
                ops.push_back(o);
            } else {
                std::snprintf(buf, sizeof buf, "+%zu: 0x%08X (not a load, an FMAC word or a store)", i * 4, word);
                why = buf;
                return false;
            }
        }
        ok = stores == 1 && fmacs > 0;
        if (!ok) why = "no store or no FMAC word before the return";
        return ok;
    }

    void Run(VuUnit& vu, VuInterpreterTier& tier, const float* matrix, const float* vector, float out[4]) const {
        VUregs& r = vu.regs();
        for (const Op& o : ops) {
            if (o.kind == Op::Load) std::memcpy(r.VF[o.reg].F, (o.base == 4 ? matrix : vector) + o.offset / 4, 16);
            else if (o.kind == Op::Fmac) tier.Run(vu, o.pc);
            else std::memcpy(out, r.VF[o.reg].F, 16);
        }
    }
};

// one function's comparison: the port's answer (P), the engine's HLE replacement (H: what the recompiled reference runs) and
// the retail instructions on the interpreter (V), bit for bit per lane
struct Tally {
    uint64_t calls = 0, port_hle = 0, hle_micro = 0, port_micro = 0;   // calls whose answers differ in any lane
    uint64_t lane[4] = {};                                               // H vs V: the lanes that differ
    uint64_t ulp1 = 0, ulp_more = 0;                                     // H vs V differing lanes: by 1 ulp / more
    uint32_t max_ulp = 0;                                                // in the result's own ulps (huge across a sign change)
    double max_abs = 0;
    double max_term_ulps = 0;                                            // |H - V| in ulps of the lane's largest term (the scale
                                                                         // the arithmetic works at: products and the translation)
    uint64_t watched = 0, watched_differ = 0;                            // calls on a watched matrix (TE_MATH_TRANSFORM_WATCH)
    double watched_max_abs_y = 0;
    uint32_t watched_max_ulp_y = 0;
    unsigned shown = 0;
};

uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
uint32_t UlpDistance(float a, float b) {
    // the floats in order as integers (sign-magnitude to two's complement); +0 and -0 one apart
    auto key = [](float f) { const uint32_t b = Bits(f); return (b & 0x80000000u) ? -int64_t(b & 0x7FFFFFFFu) - 1 : int64_t(b); };
    const int64_t d = key(a) - key(b);
    return uint32_t(d < 0 ? -d : d);
}

struct MacroDiag {
    bool read = false, compare = false, micro = false, ready = false;
    std::unique_ptr<VuUnit> vu;
    std::unique_ptr<VuInterpreterTier> tier;
    RetailMacro point, rotate;
    Tally point_tally, rotate_tally;
    std::vector<uint32_t> watch;
    float bias_y = 0.0f;          // TE_MATH_TRANSFORM_BIAS_Y: added to the watched matrices' VuTransformPoint y (a perturbation)
    uint64_t biased = 0;

    void Read() {
        if (read) return;
        read = true;
        micro = std::getenv("TE_MATH_TRANSFORM_MICRO") != nullptr;
        compare = micro || std::getenv("TE_MATH_TRANSFORM_COMPARE") != nullptr;
        if (const char* b = std::getenv("TE_MATH_TRANSFORM_BIAS_Y")) bias_y = std::strtof(b, nullptr);
        if (const char* w = std::getenv("TE_MATH_TRANSFORM_WATCH")) {
            for (const char* p = w; *p;) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 0);
                if (end == p) break;
                watch.push_back(uint32_t(v));
                p = *end == ',' ? end + 1 : end;
            }
        }
        if (compare) std::atexit([] { Report(); });
        if (bias_y != 0.0f) {
            std::fprintf(stderr, "[te.math] TE_MATH_TRANSFORM_BIAS_Y=%g: added to the y of every VuTransformPoint on %zu watched matri%s (a "
                                 "perturbation of the seam, for a judge's sensitivity check)\n",
                         double(bias_y), watch.size(), watch.size() == 1 ? "x" : "ces");
            std::atexit([] { ReportBias(); });
        }
    }
    bool Watched(uint32_t matrix) const {
        for (uint32_t w : watch)
            if (w == matrix) return true;
        return false;
    }
    void Count(Tally& t, const char* what, uint32_t matrix, const float port[4], const float hle[4], const float micro_out[4], const float scale[4],
               int lanes) {
        ++t.calls;
        bool ph = false, hm = false, pm = false;
        for (int k = 0; k < lanes; ++k) {
            ph |= Bits(port[k]) != Bits(hle[k]);
            pm |= Bits(port[k]) != Bits(micro_out[k]);
            if (Bits(hle[k]) != Bits(micro_out[k])) {
                hm = true;
                ++t.lane[k];
                const uint32_t u = UlpDistance(hle[k], micro_out[k]);
                (u == 1 ? t.ulp1 : t.ulp_more) += 1;
                if (u > t.max_ulp) t.max_ulp = u;
                const double a = std::fabs(double(hle[k]) - double(micro_out[k]));
                if (a > t.max_abs) t.max_abs = a;
                if (scale[k] > 0.f && std::isfinite(scale[k])) {
                    const double units = a / std::ldexp(1.0, std::ilogb(scale[k]) - 23);
                    if (units > t.max_term_ulps) t.max_term_ulps = units;
                }
            }
        }
        t.port_hle += ph;
        t.hle_micro += hm;
        t.port_micro += pm;
        if (Watched(matrix)) {
            ++t.watched;
            t.watched_differ += hm;
            const double a = std::fabs(double(hle[1]) - double(micro_out[1]));
            if (a > t.watched_max_abs_y) t.watched_max_abs_y = a;
            const uint32_t u = UlpDistance(hle[1], micro_out[1]);
            if (u > t.watched_max_ulp_y) t.watched_max_ulp_y = u;
        }
        if (hm && t.shown < 6) {
            ++t.shown;
            std::fprintf(stderr, "[te.math] %s differs (call %llu, matrix 0x%08X%s): hle %08X %08X %08X %08X  micro %08X %08X %08X %08X\n", what,
                         (unsigned long long)t.calls, matrix, Watched(matrix) ? ", watched" : "", Bits(hle[0]), Bits(hle[1]), Bits(hle[2]),
                         Bits(hle[3]), Bits(micro_out[0]), Bits(micro_out[1]), Bits(micro_out[2]), Bits(micro_out[3]));
        }
    }
    static void Print(const char* what, const RetailMacro& m, const Tally& t) {
        if (!m.ok) {
            std::fprintf(stderr, "[te.math] %s compared: none (the retail words were not taken: %s)\n", what, m.why.empty() ? "not read" : m.why.c_str());
            return;
        }
        std::fprintf(stderr,
                     "[te.math] %s compared: %llu calls; port vs HLE differ %llu; HLE vs retail-on-interpreter agree %llu, differ %llu "
                     "(lanes x %llu y %llu z %llu w %llu; by 1 ulp %llu, by more %llu; max %u ulp of the result, %.2f ulp of the largest term; max |d| %.3g); "
                     "port vs interpreter differ %llu; "
                     "watched %llu (differ %llu, max |dy| %.3g = %u ulp)\n",
                     what, (unsigned long long)t.calls, (unsigned long long)t.port_hle, (unsigned long long)(t.calls - t.hle_micro),
                     (unsigned long long)t.hle_micro, (unsigned long long)t.lane[0], (unsigned long long)t.lane[1], (unsigned long long)t.lane[2],
                     (unsigned long long)t.lane[3], (unsigned long long)t.ulp1, (unsigned long long)t.ulp_more, t.max_ulp, t.max_term_ulps, t.max_abs,
                     (unsigned long long)t.port_micro, (unsigned long long)t.watched, (unsigned long long)t.watched_differ, t.watched_max_abs_y,
                     t.watched_max_ulp_y);
    }
    static void Report();
    static void ReportBias();
};
MacroDiag g_macro;
void MacroDiag::ReportBias() {
    std::fprintf(stderr, "[te.math] TE_MATH_TRANSFORM_BIAS_Y: %llu answers biased by %g\n", (unsigned long long)g_macro.biased, double(g_macro.bias_y));
}
// -1 until the first call reads the knobs, then 0 (the plain path: one predictable branch a call) or 1
int g_macroMode = -1;

int MacroMode() {
    if (g_macroMode < 0) {
        g_macro.Read();
        g_macroMode = g_macro.compare || g_macro.bias_y != 0.0f ? 1 : 0;
    }
    return g_macroMode;
}

void MacroDiag::Report() {
    std::fprintf(stderr, "[te.math] the retail VU0 functions on the engine's VU interpreter (%s): MultiplyMatrixByVector %s (%u loads, %u FMAC, "
                         "%u store), TransformVector_ %s (%u loads, %u FMAC, %u store)\n",
                 g_macro.micro ? "they answer the game: TE_MATH_TRANSFORM_MICRO" : "compared only: TE_MATH_TRANSFORM_COMPARE",
                 g_macro.point.ok ? "decoded" : "NOT decoded", g_macro.point.loads, g_macro.point.fmacs, g_macro.point.stores,
                 g_macro.rotate.ok ? "decoded" : "NOT decoded", g_macro.rotate.loads, g_macro.rotate.fmacs, g_macro.rotate.stores);
    Print("transform (VuTransformPoint)", g_macro.point, g_macro.point_tally);
    Print("rotate (VuRotateVector)", g_macro.rotate, g_macro.rotate_tally);
}

using HleVector = title::MathLibrary::Vector;
using HleMatrix = title::MathLibrary::Matrix;
HleMatrix ToHle(const M4& m) {
    HleMatrix out;
    for (int r = 0; r < 4; ++r) out[r] = {m.r[r].x, m.r[r].y, m.r[r].z, m.r[r].w};
    return out;
}

// VuTransformPoint's three answers compared; `r` (the port's) replaced by the interpreter's under TE_MATH_TRANSFORM_MICRO
void ComparePoint(uint32_t matrix, const M4& m, const V4& p, V4& r) {
    const HleVector hp{p.x, p.y, p.z, p.w};
    const HleVector h = title::MathLibrary::EvaluateTransformPoint(ToHle(m), hp, 1.0f).output;
    float v[4];
    g_macro.point.Run(*g_macro.vu, *g_macro.tier, &m.r[0].x, &p.x, v);
    const float port[4] = {r.x, r.y, r.z, r.w};
    float scale[4];
    for (int k = 0; k < 4; ++k) {
        const float* c0 = &m.r[0].x + k;   // column k of the rows
        scale[k] = std::fmax(std::fmax(std::fabs(c0[0] * p.x), std::fabs(c0[4] * p.y)), std::fmax(std::fabs(c0[8] * p.z), std::fabs(c0[12])));
    }
    g_macro.Count(g_macro.point_tally, "transform", matrix, port, h.data(), v, scale, 4);
    if (g_macro.micro) r = {v[0], v[1], v[2], v[3]};
}

// VuTransformPoint diagnosed: compared (TE_MATH_TRANSFORM_COMPARE / _MICRO), then, on a watched matrix, biased
// (TE_MATH_TRANSFORM_BIAS_Y: the y every caller of that body's matrix gets -- the level springs' corner heights among them)
void DiagnosePoint(uint32_t matrix, const M4& m, const V4& p, V4& r) {
    if (g_macro.compare && g_macro.point.ok) ComparePoint(matrix, m, p, r);
    if (g_macro.bias_y != 0.0f && g_macro.Watched(matrix)) {
        r.y += g_macro.bias_y;
        ++g_macro.biased;
    }
}

// VuRotateVector's (xyz; w is the vector's own in all three)
void DiagnoseRotate(uint32_t matrix, const M4& m, const V4& vec, V4& r) {
    if (!g_macro.compare || !g_macro.rotate.ok) return;
    const HleVector hv{vec.x, vec.y, vec.z, vec.w};
    const HleVector h = title::MathLibrary::EvaluateRotation(ToHle(m), hv).output;
    float v[4];
    g_macro.rotate.Run(*g_macro.vu, *g_macro.tier, &m.r[0].x, &vec.x, v);
    const float port[4] = {r.x, r.y, r.z, r.w};
    float scale[4];
    for (int k = 0; k < 4; ++k) {
        const float* c0 = &m.r[0].x + k;
        scale[k] = k == 3 ? std::fabs(vec.w) : std::fmax(std::fmax(std::fabs(c0[0] * vec.x), std::fabs(c0[4] * vec.y)), std::fabs(c0[8] * vec.z));
    }
    g_macro.Count(g_macro.rotate_tally, "rotate", matrix, port, h.data(), v, scale, 4);
    if (g_macro.micro) r = {v[0], v[1], v[2], v[3]};
}

}  // namespace

namespace te::math {

bool MacroDiagRequested() { return std::getenv("TE_MATH_TRANSFORM_COMPARE") != nullptr || std::getenv("TE_MATH_TRANSFORM_MICRO") != nullptr; }

void SetRetailMacroWords(const uint32_t* transform_point, size_t transform_point_words, const uint32_t* rotate_vector,
                         size_t rotate_vector_words) {
    g_macro.Read();
    if (!g_macro.vu) {
        g_macro.vu = std::make_unique<VuUnit>(0);
        g_macro.tier = std::make_unique<VuInterpreterTier>();
        g_macro.vu->regs().Reset();
    }
    uint32_t pc = 0;
    g_macro.point.name = "MultiplyMatrixByVector";
    g_macro.rotate.name = "TransformVector_";
    const bool point = g_macro.point.Decode(transform_point, transform_point_words, *g_macro.vu, pc);
    const bool rotate = g_macro.rotate.Decode(rotate_vector, rotate_vector_words, *g_macro.vu, pc);
    std::fprintf(stderr, "[te.math] VuTransformPoint / VuRotateVector: the retail MultiplyMatrixByVector %s, TransformVector_ %s, run as VU "
                         "upper instructions on the engine's interpreter (%s)\n",
                 point ? "decoded" : ("NOT decoded: " + g_macro.point.why).c_str(), rotate ? "decoded" : ("NOT decoded: " + g_macro.rotate.why).c_str(),
                 g_macro.micro ? "its answers are the game's: TE_MATH_TRANSFORM_MICRO" : "compared with the port's and the HLE's: TE_MATH_TRANSFORM_COMPARE");
}

}  // namespace te::math

extern "C" {

void ps2e_vu0_init_identity_matrix(ps2e_u32 matrix) {
    M4* m = MatW(matrix);
    if (!m) return;
    *m = M4{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}

// VuTransformPoint (MultiplyMatrixByVector): the translation row added once (vf0.w = 1), whatever the point's w
void ps2e_vu0_transform_point(ps2e_u32 matrix, ps2e_u32 point, ps2e_u32 out) {
    const M4* m = Mat(matrix);
    const V4* p = Vec(point);
    V4* o = VecW(out);
    if (!m || !p || !o) return;
    V4 r = RowCombine(*m, *p, 1.0f);
    if (MacroMode()) DiagnosePoint(matrix, *m, *p, r);   // te_math_diag.h (off: one branch)
    *o = r;
}

// VuTransformByRows: the point's own w scales the fourth row
void ps2e_vu0_transform_by_rows(ps2e_u32 rows, ps2e_u32 point, ps2e_u32 out) {
    const M4* m = Mat(rows);
    const V4* p = Vec(point);
    V4* o = VecW(out);
    if (!m || !p || !o) return;
    const V4 r = RowCombine(*m, *p, p->w);
    *o = r;
}

// VuRotateVector (TransformVector_): xyz through the three rotation rows; w stays the vector's
void ps2e_vu0_rotate_vector(ps2e_u32 matrix, ps2e_u32 vector, ps2e_u32 out) {
    const M4* m = Mat(matrix);
    const V4* v = Vec(vector);
    V4* o = VecW(out);
    if (!m || !v || !o) return;
    V4 acc = Scale(m->r[0], v->x);
    acc = Add(acc, Scale(m->r[1], v->y));
    acc = Add(acc, Scale(m->r[2], v->z));
    V4 r = {acc.x, acc.y, acc.z, v->w};
    if (MacroMode()) DiagnoseRotate(matrix, *m, *v, r);   // te_math_diag.h (off: one branch)
    *o = r;
}

// VuMultiplyMatrices(a, b, out): out = a × b (each row of a through b)
void ps2e_vu0_multiply_matrices(ps2e_u32 a, ps2e_u32 b, ps2e_u32 out) {
    te::gfx::CountCall(te::gfx::kCallMultiplyMatrices);
    const M4* ma = Mat(a);
    const M4* mb = Mat(b);
    M4* o = MatW(out);
    if (!ma || !mb || !o) return;
    if (te::gfx::g_repeatCensus) te::gfx::NoteRepeat(te::gfx::kCallMultiplyMatrices, ma, sizeof *ma, mb, sizeof *mb);
    M4 r;
    for (int i = 0; i < 4; ++i) r.r[i] = RowCombine(*mb, ma->r[i], ma->r[i].w);
    *o = r;
}

// MultiplyRotations (RotateVecByVec): the quaternion product -- a.w·b + b.w·a + a × b; w = a.w·b.w − a·b
void ps2e_vu0_multiply_rotations(ps2e_u32 out, ps2e_u32 a, ps2e_u32 b) {
    const V4* qa = Vec(a);
    const V4* qb = Vec(b);
    V4* o = VecW(out);
    if (!qa || !qb || !o) return;
    const V4 A = *qa, B = *qb;
    V4 r;
    r.x = (A.y * B.z - A.z * B.y) + A.x * B.w + B.x * A.w;
    r.y = (A.z * B.x - A.x * B.z) + A.y * B.w + B.y * A.w;
    r.z = (A.x * B.y - A.y * B.x) + A.z * B.w + B.z * A.w;
    r.w = A.w * B.w - A.z * B.z - A.y * B.y - A.x * B.x;
    *o = r;
}

// MatrixFromRotation (Rotate_): a unit quaternion's three rotation rows (w 0); the fourth row is left as it was
void ps2e_vu0_matrix_from_rotation(ps2e_u32 matrix, ps2e_u32 rotation) {
    M4* m = MatW(matrix);
    const V4* q = Vec(rotation);
    if (!m || !q) return;
    const float x = q->x, y = q->y, z = q->z, w = q->w;
    const float xx = 2 * x * x, yy = 2 * y * y, zz = 2 * z * z;
    const float xy = 2 * x * y, xz = 2 * x * z, yz = 2 * y * z, wx = 2 * w * x, wy = 2 * w * y, wz = 2 * w * z;
    m->r[0] = {1 - yy - zz, xy + wz, xz - wy, 0};
    m->r[1] = {xy - wz, 1 - xx - zz, yz + wx, 0};
    m->r[2] = {xz + wy, yz - wx, 1 - xx - yy, 0};
}

// VuInvertRigid: the rotation transposed, the translation turned by it and negated, w 1
void ps2e_vu0_invert_rigid(ps2e_u32 out, ps2e_u32 matrix) {
    const M4* m = Mat(matrix);
    M4* o = MatW(out);
    if (!m || !o) return;
    const M4 s = *m;
    M4 r;
    r.r[0] = {s.r[0].x, s.r[1].x, s.r[2].x, 0};
    r.r[1] = {s.r[0].y, s.r[1].y, s.r[2].y, 0};
    r.r[2] = {s.r[0].z, s.r[1].z, s.r[2].z, 0};
    const V4 t = {-s.r[3].x, -s.r[3].y, -s.r[3].z, 1};
    V4 acc = Scale(r.r[0], t.x);
    acc = Add(acc, Scale(r.r[1], t.y));
    acc = Add(acc, Scale(r.r[2], t.z));
    r.r[3] = {acc.x, acc.y, acc.z, 1};
    *o = r;
}

void ps2e_vu0_invert_rigid_in_place(ps2e_u32 matrix) { ps2e_vu0_invert_rigid(matrix, matrix); }

// PlaneThroughTriangle: the unit normal of the edges' cross product, w minus the first vertex's distance along it;
// whether the cross product was longer than 5e-05
ps2e_u32 ps2e_vu0_plane_through_triangle(ps2e_u32 plane, ps2e_u32 first, ps2e_u32 second, ps2e_u32 third) {
    V4* o = VecW(plane);
    const V4* a = Vec(first);
    const V4* b = Vec(second);
    const V4* c = Vec(third);
    if (!o || !a || !b || !c) return 0;
    const V4 e1 = {b->x - a->x, b->y - a->y, b->z - a->z, 0};
    const V4 e2 = {c->x - a->x, c->y - a->y, c->z - a->z, 0};
    V4 n = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x, 0};
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    const float inv = length != 0.f ? 1.0f / length : 0.f;
    n = {n.x * inv, n.y * inv, n.z * inv, 0};
    const float d = n.x * a->x + n.y * a->y + n.z * a->z;
    *o = {n.x, n.y, n.z, -d};
    return length > 5e-05f ? 1u : 0u;
}

}  // extern "C"
