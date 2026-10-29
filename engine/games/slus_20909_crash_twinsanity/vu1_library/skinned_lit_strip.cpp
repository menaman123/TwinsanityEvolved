// SkinnedLitStrip -- vs_skinned_lit_strip (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/skinned_lit_strip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_loaded_programs.h"

#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_float.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/displace_prefix.h"

using namespace vu1lib;

namespace {

constexpr u32 kEmitterPc = 0x1720;   // the material sub (= the variant base)
constexpr u32 kExitPc = 0x90;        // the E-bit exit node / stub entry
constexpr u32 kExitTpc = 0xA0;       // the E-bit stub's exit TPC
constexpr u32 kScaleEntryPc = 0x12a0;
constexpr u32 kSkinCb0Pc = 0xcb0;
constexpr u32 kClipMarkPc = 0xb30;
constexpr u32 kSkin1040Pc = 0x1040;
constexpr u32 kEnvSubPc = 0x1aa0;
constexpr u64 kEnvSubImage = 0x98164f645013a94dull;  // the only image with live code at kEnvSubPc

// One palette bone: three rotation columns + translation (LQ order QW[idx+13..16]).
struct Bone {
    V4 c0, c1, c2, tr;
};

Bone LoadBone(QwPtr m, u32 idx) {
    Bone b;
    for (int k = 0; k < 4; ++k) {
        b.c0.b[k] = m.W(idx + 13, k);
        b.c1.b[k] = m.W(idx + 14, k);
        b.c2.b[k] = m.W(idx + 15, k);
        b.tr.b[k] = m.W(idx + 16, k);
    }
    return b;
}

V4 Load(QwPtr m, u32 qw) {
    V4 v;
    for (int k = 0; k < 4; ++k) v.b[k] = m.W(qw, k);
    return v;
}

// A loaded bone's xyz lanes clamped once: every FMAC that reads a lane reads CF of its raw bits, the same value for each
// read (the loaded copy does not change), so the bodies below take it once per load. Likewise a value that is itself an
// FMAC result (stored clamped) is its own CF, and x * 1.0f is x for a clamped x: those clamps are left out. Same operands,
// same operations, same order -> the same bits.
struct BoneC {
    float c0[3], c1[3], c2[3], tr[3];
};
BoneC ClampBone(const Bone& b) {
    BoneC c;
    for (int k = 0; k < 3; ++k) {
        c.c0[k] = CF(bits2f(b.c0.b[k]));
        c.c1[k] = CF(bits2f(b.c1.b[k]));
        c.c2[k] = CF(bits2f(b.c2.b[k]));
        c.tr[k] = CF(bits2f(b.tr.b[k]));
    }
    return c;
}

}  // namespace

bool SkinnedLitStrip::RunScalePhase(QwPtr m, u32 top, u32* stale_w_out) {
    const u32 count = m.W(top + 1, 0) & 0xFFFF;  // ILW.x vi10, 1(viT)
    if (count == 0 || (count & 3) != 0) return false;
    const u32 base = top + 3;
    const float fsx = CF(m.F(top + 2, 0));  // LQ.xy vf11, 2(viT)
    const float fsy = CF(m.F(top + 2, 1));
    for (u32 v = 0; v < count; v += 4) {
        for (int p = 0; p < 2; ++p) {
            const u32 qw = base + v + p;
            const float s = p ? fsy : fsx;
            for (int k = 0; k < 4; ++k) {
                const float it = (float)(int32_t)m.W(qw, k);  // ITOF0 lane
                m.SetW(qw, k, f2bits(CF(CF(it) * s)));
            }
        }
    }
    if (stale_w_out) {
        // vf19's final value: the last tail load QW[g+13]. On the mod-4 exit
        // the ITOF0 is skipped (branch + delay slot) and on the mod-0 exit
        // the raw reload lands after the convert -- raw bits; the mod-8 /
        // mod-12 exits leave the converted value.
        const u32 mod = count & 15;
        u32 koff;
        bool conv;
        if (mod == 4) {
            koff = count + 9;
            conv = false;
        } else if (mod == 8) {
            koff = count + 5;
            conv = true;
        } else if (mod == 12) {
            koff = count + 1;
            conv = true;
        } else {  // mod == 0
            koff = count + 13;
            conv = false;
        }
        const u32 raw = m.W(base + koff, 3);
        *stale_w_out = conv ? f2bits((float)(int32_t)raw) : raw;
    }
    return true;
}

bool SkinnedLitStrip::RunSkinCb0(QwPtr m, u32 top) {
    u32 stale_w = 0;
    if (!RunScalePhase(m, top, &stale_w)) return false;  // phase 1

    const u32 count = m.W(top + 1, 0) & 0xFFFF;
    const u32 base = top + 3;
    for (u32 v = 0; v < count; v += 4) {
        const u32 a = base + v;
        V4 pos, st, wq;
        for (int k = 0; k < 4; ++k) {
            pos.b[k] = m.W(a, k);
            st.b[k] = m.W(a + 1, k);
            wq.b[k] = m.W(a + 2, k);
        }
        const u32 flags16 = wq.b[3] & 0xFFFF;  // MTIR vi07 (unmasked copy)
        const u32 cnt = flags16 & 3;           // IAND vi06, 3
        const float wx = CF(bits2f(wq.b[0]));  // weight lanes (clamped reads)
        const float wy = CF(bits2f(wq.b[1]));
        const float wz = CF(bits2f(wq.b[2]));
        // pw_i = pos * w_i (MULx/MULy/MULz) -- clamped lanes.
        float pw0[3], pw1[3], pw2[3];
        for (int k = 0; k < 3; ++k) {
            const float p = CF(bits2f(pos.b[k]));
            pw0[k] = CF(p * wx);
            pw1[k] = CF(p * wy);
            pw2[k] = CF(p * wz);
        }
        const Bone b0 = LoadBone(m, wq.b[0] & 0x1FF);

        // Normal: n = (pos.w, st.z, st.w) rotated by bone0 (MULAw / MADDAz / MADDw).
        const float npw = CF(bits2f(pos.b[3]));
        const float nsz = CF(bits2f(st.b[2]));
        const float nsw = CF(bits2f(st.b[3]));
        float nrm[3];
        for (int k = 0; k < 3; ++k) {
            float acc = CF(CF(bits2f(b0.c0.b[k])) * npw);
            acc = CF(VuFloat::Fma(CF(bits2f(b0.c1.b[k])), nsz, acc));
            nrm[k] = CF(VuFloat::Fma(CF(bits2f(b0.c2.b[k])), nsw, acc));
        }

        // Position blend -- the exact per-path accumulation order.
        float po[3];
        if (cnt == 1) {
            // MULAx ACC = tr0*w0 (delay slot), then the bone0 rows.
            for (int k = 0; k < 3; ++k) {
                float acc = CF(CF(bits2f(b0.tr.b[k])) * wx);
                acc = CF(VuFloat::Fma(CF(bits2f(b0.c0.b[k])), CF(pw0[0]), acc));
                acc = CF(VuFloat::Fma(CF(bits2f(b0.c1.b[k])), CF(pw0[1]), acc));
                po[k] = CF(VuFloat::Fma(CF(bits2f(b0.c2.b[k])), CF(pw0[2]), acc));
            }
        } else {
            // Bone0 full result vf31.
            const Bone b1 = LoadBone(m, wq.b[1] & 0x1FF);
            float v31[3];
            for (int k = 0; k < 3; ++k) {
                float acc = CF(CF(bits2f(b0.tr.b[k])) * wx);
                acc = CF(VuFloat::Fma(CF(bits2f(b0.c0.b[k])), CF(pw0[0]), acc));
                acc = CF(VuFloat::Fma(CF(bits2f(b0.c1.b[k])), CF(pw0[1]), acc));
                v31[k] = CF(VuFloat::Fma(CF(bits2f(b0.c2.b[k])), CF(pw0[2]), acc));
            }
            if (cnt == 2) {
                // ACC = tr1*w1; += c0*(pw1).x; += vf31 (delay slot); += c1*(pw1).y; res = +c2*(pw1).z.
                for (int k = 0; k < 3; ++k) {
                    float acc = CF(CF(bits2f(b1.tr.b[k])) * wy);
                    acc = CF(VuFloat::Fma(CF(bits2f(b1.c0.b[k])), CF(pw1[0]), acc));
                    acc = CF(VuFloat::Fma(CF(v31[k]), 1.0f, acc));
                    acc = CF(VuFloat::Fma(CF(bits2f(b1.c1.b[k])), CF(pw1[1]), acc));
                    po[k] = CF(VuFloat::Fma(CF(bits2f(b1.c2.b[k])), CF(pw1[2]), acc));
                }
            } else {  // cnt == 3 (the degenerate cnt == 0 falls through too)
                const Bone b2 = LoadBone(m, wq.b[2] & 0x1FF);
                for (int k = 0; k < 3; ++k) {
                    // Two-bone accumulate vf19.
                    float acc = CF(CF(bits2f(b1.tr.b[k])) * wy);
                    acc = CF(VuFloat::Fma(CF(bits2f(b1.c0.b[k])), CF(pw1[0]), acc));
                    acc = CF(VuFloat::Fma(CF(v31[k]), 1.0f, acc));
                    acc = CF(VuFloat::Fma(CF(bits2f(b1.c1.b[k])), CF(pw1[1]), acc));
                    const float two = CF(VuFloat::Fma(CF(bits2f(b1.c2.b[k])), CF(pw1[2]), acc));
                    // ACC = two*1.0; += tr2*w2; then the bone2 rows.
                    acc = CF(CF(two) * 1.0f);
                    acc = CF(VuFloat::Fma(CF(bits2f(b2.tr.b[k])), wz, acc));
                    acc = CF(VuFloat::Fma(CF(bits2f(b2.c0.b[k])), CF(pw2[0]), acc));
                    acc = CF(VuFloat::Fma(CF(bits2f(b2.c1.b[k])), CF(pw2[1]), acc));
                    po[k] = CF(VuFloat::Fma(CF(bits2f(b2.c2.b[k])), CF(pw2[2]), acc));
                }
            }
        }

        // Stores.
        for (int k = 0; k < 3; ++k) m.SetW(a, k, f2bits(po[k]));
        m.SetW(a, 3, stale_w);  // SQ vf19 (.w stale)
        // QW[v+1]: MULw copy (arithmetic -- denormals flush, sign kept) with
        // ADDw.z forcing .z = 1.0.
        m.SetW(a + 1, 0, f2bits(CF(CF(bits2f(st.b[0])) * 1.0f)));
        m.SetW(a + 1, 1, f2bits(CF(CF(bits2f(st.b[1])) * 1.0f)));
        m.SetW(a + 1, 2, f2bits(CF(CF(0.0f) + CF(1.0f))));
        m.SetW(a + 1, 3, f2bits(CF(CF(bits2f(st.b[3])) * 1.0f)));
        // QW[v+2]: ADD.xyz vf27, vf21, vf00 (+0-normalizes -0) + MFIR.w
        // (sext16 of the unmasked flags copy).
        for (int k = 0; k < 3; ++k) m.SetW(a + 2, k, f2bits(CF(CF(nrm[k]) + CF(0.0f))));
        const u32 adcw = (flags16 & 0x8000) ? (0xFFFF0000u | flags16) : flags16;
        m.SetW(a + 2, 3, adcw);
    }
    return true;
}

bool SkinnedLitStrip::RunSkin1040(QwPtr m, u32 top, u32 entry_vf19_w) {
    const u32 count = m.W(top + 1, 0) & 0xFFFF;  // ILW.x vi10, 1(vi08)
    if (count == 0 || (count & 3) != 0) return false;
    const u32 base = top + 3;
    for (u32 v = 0; v < count; v += 4) {
        const u32 a = base + v;
        V4 pos, st, wq;
        for (int k = 0; k < 4; ++k) {
            pos.b[k] = m.W(a, k);
            st.b[k] = m.W(a + 1, k);
            wq.b[k] = m.W(a + 2, k);
        }
        const u32 cnt = (wq.b[3] & 0xFFFF) & 3;
        const float wx = CF(bits2f(wq.b[0]));
        const float wy = CF(bits2f(wq.b[1]));
        const float wz = CF(bits2f(wq.b[2]));
        const BoneC b0 = ClampBone(LoadBone(m, wq.b[0] & 0x1FF));

        // Bone0 position: ACC = tr0*w0 FIRST, then the rotation rows; pw0 = pos * w0.
        float pw[3];
        for (int k = 0; k < 3; ++k) pw[k] = CF(CF(bits2f(pos.b[k])) * wx);
        float po[3];
        for (int k = 0; k < 3; ++k) {
            float acc = CF(b0.tr[k] * wx);
            acc = CF(VuFloat::Fma(b0.c0[k], pw[0], acc));
            acc = CF(VuFloat::Fma(b0.c1[k], pw[1], acc));
            po[k] = CF(VuFloat::Fma(b0.c2[k], pw[2], acc));
        }
        // Normal by bone0: n = (pos.w, st.z raw, st.w raw), clamped on read.
        float nrm[3];
        {
            const float npw = CF(bits2f(pos.b[3]));
            const float nsz = CF(bits2f(st.b[2]));
            const float nsw = CF(bits2f(st.b[3]));
            for (int k = 0; k < 3; ++k) {
                float acc = CF(b0.c0[k] * npw);
                acc = CF(VuFloat::Fma(b0.c1[k], nsz, acc));
                nrm[k] = CF(VuFloat::Fma(b0.c2[k], nsw, acc));
            }
        }
        if (cnt != 1) {
            // Bone1 chained AFTER bone0 completes: ACC = prev*1.0 (= prev, a clamped result); += tr1*w1; += rows.
            const BoneC b1 = ClampBone(LoadBone(m, wq.b[1] & 0x1FF));
            for (int k = 0; k < 3; ++k) pw[k] = CF(CF(bits2f(pos.b[k])) * wy);
            for (int k = 0; k < 3; ++k) {
                float acc = po[k];
                acc = CF(VuFloat::Fma(b1.tr[k], wy, acc));
                acc = CF(VuFloat::Fma(b1.c0[k], pw[0], acc));
                acc = CF(VuFloat::Fma(b1.c1[k], pw[1], acc));
                po[k] = CF(VuFloat::Fma(b1.c2[k], pw[2], acc));
            }
            if (cnt != 2) {  // cnt == 3 (or 0)
                const BoneC b2 = ClampBone(LoadBone(m, wq.b[2] & 0x1FF));
                for (int k = 0; k < 3; ++k) pw[k] = CF(CF(bits2f(pos.b[k])) * wz);
                for (int k = 0; k < 3; ++k) {
                    float acc = po[k];
                    acc = CF(VuFloat::Fma(b2.tr[k], wz, acc));
                    acc = CF(VuFloat::Fma(b2.c0[k], pw[0], acc));
                    acc = CF(VuFloat::Fma(b2.c1[k], pw[1], acc));
                    po[k] = CF(VuFloat::Fma(b2.c2[k], pw[2], acc));
                }
            }
        }
        // Stores: pos'.w = entry vf19.w; st raw bits with MR32.z from vf00
        // (.z = 1.0, raw move -- .xyw denormals SURVIVE); normal via
        // MOVE.xyz (raw copy, -0 survives), .w = the ORIGINAL wq.w.
        for (int k = 0; k < 3; ++k) m.SetW(a, k, f2bits(po[k]));
        m.SetW(a, 3, entry_vf19_w);
        m.SetW(a + 1, 0, st.b[0]);
        m.SetW(a + 1, 1, st.b[1]);
        m.SetW(a + 1, 2, 0x3F800000u);
        m.SetW(a + 1, 3, st.b[3]);
        for (int k = 0; k < 3; ++k) m.SetW(a + 2, k, f2bits(nrm[k]));
        m.SetW(a + 2, 3, wq.b[3]);
    }
    return true;
}

bool SkinnedLitStrip::RunClipMark0B30(QwPtr m, u32 top) {
    // Per vertex (stride 4): judge the guard-scaled clip vector of rows
    // QW[head+89..93]; QW[vq+2].w = sext16(((window OR of the last three
    // judgements) + 0x7FFF) | low16(old w)) -- bit15 set marks a guard-band
    // hit for the family clipper's early-out. Each judgement is its own
    // iteration's CLIP push (0 / 1, not bits). Memory-only.
    const u32 head = m.W(3, 0) & 0xFFFF;         // ILW.x vi11, 3(vi00)
    const u32 count = m.W(top + 1, 0) & 0xFFFF;  // ILW.x vi10, 1(vi08)
    if (count == 0 || (count & 3) != 0) return false;
    V4 rows[5];
    for (int r = 0; r < 5; ++r)
        for (int k = 0; k < 4; ++k) rows[r].b[k] = m.W(head + 89 + r, k);
    const u32 vbase = top + 3;  // IADDIU vi09, vi08, 3
    u32 h1 = 0, h2 = 0;         // vi02, vi03
    for (u32 j = 0; j < count / 4; ++j) {
        const u32 vq = vbase + j * 4;
        const float vx = CF(m.F(vq, 0)), vy = CF(m.F(vq, 1)), vz = CF(m.F(vq, 2));
        const V4 t = XformClip(rows[0], rows[1], rows[2], rows[3], vx, vy, vz);
        const V4 g = GuardScale(rows[4], t);  // MUL.xyzw vf14, vf09, vf14
        const u32 cur = (ClipNf(g.b[0], g.b[1], g.b[2], g.b[3]) != 0) ? 1u : 0u;
        const u32 orig = m.W(vq + 2, 3) & 0xFFFF;  // MTIR vi04, vf13w
        const u32 vi05 = (((cur | h1 | h2) + 0x7FFF) | orig) & 0xFFFF;
        const u32 wbits = (vi05 & 0x8000) ? (0xFFFF0000u | vi05) : vi05;  // MFIR.w
        m.SetW(vq + 2, 3, wbits);                                          // SQ vf13, -2(vi09)
        h2 = h1;
        h1 = cur;  // window shift
    }
    return true;
}

bool SkinnedLitStrip::RunSkinnedLitCall(QwPtr m, u32 node, u32 top, Vu1HleOutput& out) {
    if (!RunSkinnedLitBody(m, node, top)) return false;
    const u32 out_base = m.W(1, 0) & 0xFFFF;   // read before the rotation, as the body read it
    // QW1 MR32 rotation BEFORE the kicks here; node+3 then main, BOTH unconditional.
    RotateQw1(m);
    if (!KickAt(m, node + 3, out)) return false;
    if (!KickAt(m, out_base, out)) return false;
    return true;
}

bool SkinnedLitStrip::RunSkinnedLitBody(QwPtr m, u32 node, u32 top) {
    auto load = [&](u32 qw) { return Load(m, qw); };

    const V4 payload = load(node + 2);     // LQ vf12, 2(vi14)
    const u32 first = m.W(3, 0) & 0xFFFF;  // ILW.x vi09, 3(vi00)
    V4 obj[4], nrmR[3], litR[4];
    for (int r = 0; r < 4; ++r) obj[r] = load(first + 78 + r);   // vf01..04
    for (int r = 0; r < 3; ++r) nrmR[r] = load(first + 82 + r);  // vf05..07
    for (int r = 0; r < 4; ++r) litR[r] = load(first + 85 + r);  // vf08..11
    // The rows and the payload as every FMAC reads them (CF of the raw lanes), once (see ClampBone)
    float objC[4][4], nrmC[3][4], litC[4][4], payC[4];
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) {
            objC[r][k] = CF(bits2f(obj[r].b[k]));
            litC[r][k] = CF(bits2f(litR[r].b[k]));
        }
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) nrmC[r][k] = CF(bits2f(nrmR[r].b[k]));
    for (int k = 0; k < 4; ++k) payC[k] = CF(bits2f(payload.b[k]));

    const u32 vqw_count = m.W(top + 1, 0) & 0xFFFF;  // MR32 + MTIR vf14.w
    if ((vqw_count & 3) != 0) return false;
    const u32 nverts = vqw_count / 4;
    if (nverts < 3) return false;  // micro pipeline needs >= 3
    const u32 vbase = top + 3;

    const u32 out_base = m.W(1, 0) & 0xFFFF;                       // ILW.x vi11, 1(vi00)
    for (int k = 0; k < 4; ++k) m.SetW(out_base, k, m.W(top, k));  // GIFtag

    for (u32 j = 0; j < nverts; ++j) {
        const u32 vq = vbase + j * 4;
        const V4 pos = load(vq);         // LQ.xyz (only .xyz used)
        const V4 stuv = load(vq + 1);    // LQ.xyz (payload target)
        const V4 nrm = load(vq + 2);     // LQ.xyzw (.w = ADC)
        const V4 st_col = load(vq + 3);  // static RGBA ints

        // XformEmit (ACC = row4*1; += row2*y; += row3*z; res = ACC + row1*x), the rows clamped above
        const float vx = CF(bits2f(pos.b[0])), vy = CF(bits2f(pos.b[1])), vz = CF(bits2f(pos.b[2]));
        float T[4];
        for (int k = 0; k < 4; ++k) {
            float acc = objC[3][k];
            acc = CF(VuFloat::Fma(objC[1][k], vy, acc));
            acc = CF(VuFloat::Fma(objC[2][k], vz, acc));
            T[k] = CF(VuFloat::Fma(objC[0][k], vx, acc));
        }
        const float Q = VuDiv(1.0f, T[3]);

        // wn = NrmMtx * n (MULAx vf05 / MADDAy vf06 / MADDz vf07).
        const float nx = CF(bits2f(nrm.b[0])), ny = CF(bits2f(nrm.b[1])), nz = CF(bits2f(nrm.b[2]));
        float wn[3];
        for (int k = 0; k < 3; ++k) {
            float acc = CF(nrmC[0][k] * nx);
            acc = CF(VuFloat::Fma(nrmC[1][k], ny, acc));
            wn[k] = CF(VuFloat::Fma(nrmC[2][k], nz, acc));
        }

        // Lighting -> RGBA (raw-bit MAX vs +0 -- of a clamped value: clamped; MULAw vf11 first).
        u32 rgba[4];
        {
            const float wcx = bits2f(Fmax_raw(f2bits(wn[0]), 0u));
            const float wcy = bits2f(Fmax_raw(f2bits(wn[1]), 0u));
            const float wcz = bits2f(Fmax_raw(f2bits(wn[2]), 0u));
            float lit[3];
            for (int k = 0; k < 3; ++k) {
                float acc = litC[3][k];
                acc = CF(VuFloat::Fma(litC[0][k], wcx, acc));
                acc = CF(VuFloat::Fma(litC[1][k], wcy, acc));
                lit[k] = CF(VuFloat::Fma(litC[2][k], wcz, acc));
            }
            for (int k = 0; k < 3; ++k) {
                const float s = (float)(int32_t)st_col.b[k];  // ITOF0 (an integer's float: its own CF)
                const float mod = CF(lit[k] * s);             // MUL.xyz
                rgba[k] = Ftoi0(Fmin_raw(f2bits(mod), f2bits(255.0f)));
            }
            const float sa = (float)(int32_t)st_col.b[3];  // alpha unmodulated
            rgba[3] = Ftoi0(Fmin_raw(f2bits(sa), f2bits(255.0f)));
        }

        // Stores: ST_j (xyz only -- .w VIF-prefilled), RGBA_j, XYZ2_j with
        // ADC = QW[v+2].w raw bits.
        const u32 st_qw = out_base + 1 + 3 * j;
        for (int k = 0; k < 3; ++k) {
            const float stp = CF(CF(bits2f(stuv.b[k])) + payC[k]);
            m.SetW(st_qw, k, f2bits(CF(stp * Q)));
        }
        for (int k = 0; k < 4; ++k) m.SetW(st_qw + 1, k, rgba[k]);
        m.SetW(st_qw + 2, 0, Ftoi0(f2bits(CF(T[0] * Q))));
        m.SetW(st_qw + 2, 1, Ftoi0(f2bits(CF(T[1] * Q))));
        m.SetW(st_qw + 2, 2, Ftoi0(f2bits(CF(T[2] * Q))));
        m.SetW(st_qw + 2, 3, nrm.b[3]);
    }
    return true;
}

bool SkinnedLitStrip::RunSkinEnv1aa0(QwPtr m, u32 node, u32 top, Vu1HleOutput& out) {
    // A specular / env-mapped pass over the SAME skinned vertex stream the
    // 0x1720 body consumes: lit RGBA as the main body (MINI.xyzw: alpha
    // clamped too); reflection = 2*dot(d, n)*n - d with d = normalize(eye -
    // pos) through the ITOF0/FTOI0 fast-rsqrt bit trick against constants
    // built once per call; env ST = clamp01(EnvMtx . refl * 0.5 + 0.5) +
    // payload.xy, .z = 1.0, all * Q; XYZ2 = FTOI0(ObjMtx . pos * Q), .w =
    // nrm.w raw. Kicks node+3 then the packet, THEN rotates QW1.
    auto load = [&](u32 qw) { return Load(m, qw); };

    // Header.
    const u32 out_list = m.W(1, 0) & 0xFFFF;  // ILW.x vi11, 1(vi00)
    const u32 first = m.W(3, 0) & 0xFFFF;     // ILW.x vi09, 3(vi00)
    const V4 eye = load(first + 77);          // vf16
    V4 obj[4], nrmR[3], litR[4], envR[3];
    for (int r = 0; r < 4; ++r) obj[r] = load(first + 78 + r);   // vf01..04
    for (int r = 0; r < 3; ++r) nrmR[r] = load(first + 82 + r);  // vf05..07
    for (int r = 0; r < 4; ++r) litR[r] = load(first + 85 + r);  // vf08..11
    for (int r = 0; r < 3; ++r) envR[r] = load(first + 89 + r);  // vf12..14
    const V4 payload = load(node + 2);                           // vf17 = LQ 2(vi14)

    // GIFtag copy: vf18 = QW[top], SQ -> 0(out_list).
    for (int k = 0; k < 4; ++k) m.SetW(out_list, k, m.W(top, k));

    // vf20 magic constants: .xyz = 0 + 1065353216.0 (= bits-as-float of
    // 1.0f); .z *= -0.5; .x = .x - .z; .w = 1.0 * -0.5.
    const float big = CF(0.0f + CF(bits2f(0x4e7e0000u)));
    const float halfneg = CF(CF(big) * CF(bits2f(0xbf000000u)));
    const float magic = CF(CF(big) - CF(halfneg));               // vf20.x = 1598029824.0
    const float negp5 = CF(CF(1.0f) * CF(bits2f(0xbf000000u)));  // vf20.w

    // Count = low16(QW[top+1].x) via MR32+MTIR vf19.w; vertex stride 4 QWs.
    // The loop is do-while shaped; IBNE vi06,vi07 exits only on exact
    // equality -- count == 0 or count % 4 != 0 never terminates.
    const u32 count = m.W(top + 1, 0) & 0xFFFF;
    if (count == 0 || (count & 3) != 0) return false;
    const u32 vbase = top + 3;  // IADDIU vi06, vi08, 3
    const u32 vend = vbase + count;

    u32 dst = out_list + 1;  // IADDI vi11, vi11, 1
    for (u32 vq = vbase; vq != vend; vq += 4, dst += 3) {
        const V4 pos = load(vq);      // vf21
        const V4 nrm = load(vq + 2);  // vf23 (.w = ADC)
        const V4 col = load(vq + 3);  // vf24 static RGBA ints

        // wn = NrmMtx * n (MULAx vf05 / MADDAy vf06 / MADDz vf07).
        const float nx = CF(bits2f(nrm.b[0])), ny = CF(bits2f(nrm.b[1])), nz = CF(bits2f(nrm.b[2]));
        float wn[3];
        for (int k = 0; k < 3; ++k) {
            float acc = CF(CF(bits2f(nrmR[0].b[k])) * nx);
            acc = CF(VuFloat::Fma(CF(bits2f(nrmR[1].b[k])), ny, acc));
            wn[k] = CF(VuFloat::Fma(CF(bits2f(nrmR[2].b[k])), nz, acc));
        }
        // MAX.xyzw vs +0 (raw-bit), lit accumulate (MULAw vf11 / MADDAx vf08 /
        // MADDAy vf09 / MADDz vf10), colour modulate + MINI.xyzw 255.0
        // (raw-bit) + FTOI0.xyzw.
        u32 rgba[4];
        {
            const float wcx = bits2f(Fmax_raw(f2bits(wn[0]), 0u));
            const float wcy = bits2f(Fmax_raw(f2bits(wn[1]), 0u));
            const float wcz = bits2f(Fmax_raw(f2bits(wn[2]), 0u));
            float c4[4];
            for (int k = 0; k < 4; ++k) c4[k] = (float)(int32_t)col.b[k];  // ITOF0
            for (int k = 0; k < 3; ++k) {
                float acc = CF(CF(bits2f(litR[3].b[k])) * 1.0f);
                acc = CF(VuFloat::Fma(CF(bits2f(litR[0].b[k])), CF(wcx), acc));
                acc = CF(VuFloat::Fma(CF(bits2f(litR[1].b[k])), CF(wcy), acc));
                const float lit = CF(VuFloat::Fma(CF(bits2f(litR[2].b[k])), CF(wcz), acc));
                c4[k] = CF(CF(c4[k]) * lit);  // MUL.xyz vf24, vf24, vf25
            }
            for (int k = 0; k < 4; ++k) rgba[k] = Ftoi0(Fmin_raw(f2bits(c4[k]), f2bits(255.0f)));
        }

        // d = eye - pos (SUB.xyz), normalized by the fast-rsqrt bit trick:
        // len2 = x^2+z^2+y^2 (that ADD order); ITOF0.x reinterprets len2's
        // BITS as int32; rs = magic - 0.5*that; FTOI0.x truncates back to
        // int; the MUL below reads those INT bits as float = ~1/len.
        float dir[3];
        for (int k = 0; k < 3; ++k) dir[k] = CF(CF(bits2f(eye.b[k])) - CF(bits2f(pos.b[k])));
        {
            const float sx = CF(CF(dir[0]) * CF(dir[0]));
            const float sy = CF(CF(dir[1]) * CF(dir[1]));
            const float sz = CF(CF(dir[2]) * CF(dir[2]));
            const float len2 = CF(CF(CF(CF(sx) + CF(sz))) + CF(sy));
            const float fl = (float)(int32_t)f2bits(len2);        // ITOF0.x
            const float acc = CF(CF(fl) * CF(negp5));             // MULA.x
            const float rs = CF(VuFloat::Fma(CF(magic), 1.0f, acc));  // MADD.x
            const u32 rsbits = Ftoi0(f2bits(rs));                 // FTOI0.x
            const float inv = CF(bits2f(rsbits));
            for (int k = 0; k < 3; ++k) dir[k] = CF(CF(dir[k]) * inv);
        }
        // refl = 2*dot(d, n)*n - d (dot adds z then y).
        float refl[3];
        {
            float t[3];
            for (int k = 0; k < 3; ++k) t[k] = CF(CF(dir[k]) * CF(bits2f(nrm.b[k])));
            const float dot = CF(CF(CF(CF(t[0]) + CF(t[2]))) + CF(t[1]));
            const float twod = CF(CF(dot) * CF(2.0f));  // MUL.x I=2
            for (int k = 0; k < 3; ++k) {
                const float rp = CF(CF(bits2f(nrm.b[k])) * CF(twod));
                refl[k] = CF(CF(rp) - CF(dir[k]));  // SUB.xyz
            }
        }

        // T = ObjMtx . pos (XformEmit order exactly); Q = 1/T.w.
        const V4 T = XformEmit(obj[0], obj[1], obj[2], obj[3], CF(bits2f(pos.b[0])), CF(bits2f(pos.b[1])),
                               CF(bits2f(pos.b[2])));
        const float Q = VuDiv(1.0f, bits2f(T.b[3]));

        // env = EnvMtx . refl (MULAz vf14 / MADDAy vf13 / MADDx vf12 -- NO
        // translation row; only .xy consumed).
        float env[2];
        for (int k = 0; k < 2; ++k) {
            float acc = CF(CF(bits2f(envR[2].b[k])) * CF(refl[2]));
            acc = CF(VuFloat::Fma(CF(bits2f(envR[1].b[k])), CF(refl[1]), acc));
            env[k] = CF(VuFloat::Fma(CF(bits2f(envR[0].b[k])), CF(refl[0]), acc));
        }
        // ST build: .xy = env*0.5 + 0.5, .z = 0+1.0; MINI.xyzw vs 1.0 then
        // MAX.xyzw vs +0 (raw-bit); .xy += payload; *= Q.
        const float half = CF(0.0f + CF(bits2f(0x3f000000u)));  // ADD.z vf26,I
        float st[3];
        st[0] = CF(CF(env[0]) * half);
        st[1] = CF(CF(env[1]) * half);
        st[2] = CF(0.0f + 1.0f);       // ADD.z vf19, vf00, vf00w
        st[0] = CF(CF(st[0]) + half);  // ADD.xy vf19 += vf26z
        st[1] = CF(CF(st[1]) + half);
        for (int k = 0; k < 3; ++k) {
            u32 b = Fmin_raw(f2bits(st[k]), 0x3F800000u);
            b = Fmax_raw(b, 0u);
            st[k] = bits2f(b);
        }
        st[0] = CF(CF(st[0]) + CF(bits2f(payload.b[0])));  // ADD.xy += vf17
        st[1] = CF(CF(st[1]) + CF(bits2f(payload.b[1])));

        // Stores: SQ.xyz ST (.w VIF-prefilled), SQ.xyzw RGBA, SQ.xyzw XYZ2
        // (FTOI0.xyz, .w = nrm.w raw).
        for (int k = 0; k < 3; ++k) m.SetW(dst, k, f2bits(CF(CF(st[k]) * Q)));
        for (int k = 0; k < 4; ++k) m.SetW(dst + 1, k, rgba[k]);
        for (int k = 0; k < 3; ++k) m.SetW(dst + 2, k, Ftoi0(f2bits(CF(CF(bits2f(T.b[k])) * Q))));
        m.SetW(dst + 2, 3, nrm.b[3]);
    }

    // Kicks FIRST (node+3 prelude then the packet), THEN the QW1 rotation.
    if (!KickAt(m, node + 3, out)) return false;
    if (!KickAt(m, out_list, out)) return false;
    RotateQw1(m);
    return true;
}

bool SkinnedLitStrip::Run(const Vu1HleInput& in, Vu1HleOutput& out) {
    // Shared-prefix displace entries: full-regs contract, their own data-write diff.
    if (in.start_pc == DisplacePrefix::kIntEntryPc) return DisplacePrefix::RunDisplaceInt(in, out);
    if (in.start_pc == DisplacePrefix::kFloatEntryPc) return DisplacePrefix::RunDisplaceFloat(in, out);

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    if (in.start_pc == kScaleEntryPc) {
        // Direct scale entry (E-bit at 0x13e8): zero kicks, data writes only.
        if (!RunScalePhase(m, in.top, nullptr)) return false;
    } else if (in.start_pc == 0) {
        m.SetW(9, 0, 1);  // dispatcher prologue beacon (shared family prefix)
        u32 node = m.W(3, 0) & 0xFFFF;
        for (int guard = 0; guard < 16; ++guard) {
            const u32 call_pc = (m.W(node, 0) & 0xFFFF) * 8;
            if (call_pc == kExitPc) break;  // E-bit exit node
            if (call_pc == kSkinCb0Pc) {
                if (!RunSkinCb0(m, in.top)) return false;
            } else if (call_pc == kClipMarkPc) {
                if (!RunClipMark0B30(m, in.top)) return false;
            } else if (call_pc == kSkin1040Pc) {
                if (!RunSkin1040(m, in.top, in.regs->VF[19].I[3])) return false;
            } else if (call_pc == kEmitterPc) {
                if (!RunSkinnedLitCall(m, node, in.top, out)) return false;
            } else if (call_pc == kEnvSubPc && (Vu1LoadedPrograms::At(in, kEnvSubPc) == Vu1LoadedPrograms::kType0F ||
                                                in.image_hash == kEnvSubImage)) {
                // The env-mapped skin sub = the shader-type-0F program loaded right after type 04.
                if (!RunSkinEnv1aa0(m, node, in.top, out)) return false;
            } else {
                return false;  // other material subs
            }
            const u32 next = m.W(node, 2) & 0xFFFF;
            if (next == node) return false;
            node = next;
        }
    } else if (in.start_pc == kExitPc) {
        // Entry at the family E-bit stub ([E] NOP; NOP delay): exactly two
        // bundles, ZERO kicks, ZERO data writes, the register file passes
        // through byte-identical; TPC = the family exit.
        if (in.regs == nullptr) return false;
        out.regs = *in.regs;
        out.regs.TPC = kExitTpc;
        out.has_regs = true;
        return true;  // no memory diff needed
    } else {
        return false;  // unknown direct entry
    }

    EmitDataWrites(s_mem.data(), in, out);
    return true;
}
