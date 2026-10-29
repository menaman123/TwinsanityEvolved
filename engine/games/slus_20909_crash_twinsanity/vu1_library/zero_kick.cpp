// ZeroKick -- vs_zero_kick_{a,b,c} + the 0x1820/0x1ac8 and 0x1750 continuations
// (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/zero_kick.h"

#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_float.h"

using namespace vu1lib;

namespace {

constexpr u32 kEmitterPc = 0x1720;     // the material sub (= the variant base)
constexpr u32 kExitPc = 0x90;          // the E-bit exit node
constexpr u32 kExitTpc = 0xA0;         // the family exit TPC
constexpr u32 kClassATpc = 0x1750;     // E-bit 0x1740 + delay
constexpr u32 kClassBTpc = 0x1748;     // E-bit 0x1738 + delay
constexpr u32 kClassCTpc = 0x1820;     // E-bit 0x1810 + delay
constexpr u32 kCContEntryPc = 0x1820;  // the class-C MSCNT continuation
constexpr u32 kCContReentryPc = 0x1ac8;  // its branch-back entry (vi13 = vi14 + 4; B 0x1820)
constexpr u32 kCContTpc = 0x1ac8;      // E-bit 0x1ab8 + delay
constexpr u32 kACContEntryPc = 0x1750;  // the class-A MSCNT continuation
constexpr u32 kACContTpc = 0x19e0;     // E-bit 0x19d0 + delay
constexpr u16 kDispatchLink = 0x0A;    // the dispatcher's JALR link (0x40 + 16) / 8

// Program-order MAC push from RAW result lanes (mask bit3=X .. bit0=W).
void Push(uint16_t& mac, uint16_t& status, const u32 b[4], u8 mask) { PushMac(mac, status, b, mask); }

}  // namespace

bool ZeroKick::RunStub(const Vu1HleInput& in, Vu1HleOutput& out, int cls) {
    if (in.start_pc != 0) return false;

    VuChopRoundGuard chop_guard;

    // Read-only use of data memory: the caller's image is read in place (QwPtr's reads only; nothing is written to it).
    QwPtr m{const_cast<u8*>(in.data)};
    out.regs = *in.regs;
    VUregs& r = out.regs;

    // Dispatcher prologue.
    const u32 node = m.W(3, 0) & 0xFFFF;  // ILW.x vi14, 3(vi00)
    r.VI[14] = (uint16_t)node;
    r.VI[12] = 1;  // IADDIU vi12, vi00, 1
    {              // ISW.x vi12, 9(vi00): full 32-bit lane = zero-extended VI.
        Vu1HleOutput::DataWrite w;
        w.addr = 9 * 16;
        w.bytes = {1, 0, 0, 0};
        out.data_writes.push_back(std::move(w));
    }
    const u32 call_vi = m.W(node, 0) & 0xFFFF;  // ILW.x vi13, 0(vi14)
    r.VI[13] = (uint16_t)call_vi;
    r.VI[15] = kDispatchLink;  // JALR link
    const u32 call_pc = call_vi * 8;
    if (call_pc == kExitPc) {  // empty list: family exit
        r.TPC = kExitTpc;
        out.has_regs = true;
        return true;
    }
    if (call_pc != kEmitterPc) return false;

    auto loadq = [&](u32 qw, int vf) {
        for (int k = 0; k < 4; ++k) r.VF[vf].I[k] = m.W(qw, k);
    };

    if (cls == 0) {  // class A
        loadq(82, 4);
        loadq(81, 3);
        loadq(80, 2);
        loadq(79, 1);
        loadq(83, 5);
        r.TPC = kClassATpc;
    } else if (cls == 1) {                // class B
        r.VI[13] = (uint16_t)(node + 1);  // IADDIU vi13, vi14, 1
        r.VI[12] = 0;                     // IADDIU vi12, vi00, 0
        loadq(node + 3, 1);
        loadq(node + 4, 2);
        r.TPC = kClassBTpc;
    } else {  // class C
        // SUB vfN,vfN,vfN zero-fills: +0 every lane under round-toward-zero
        // for every input class (normals cancel exactly; the clamp folds
        // NaN/Inf to +/-MAX and denormals to +/-0 first). Each pushes MAC 0x000F.
        uint16_t mac = r.mac_flag, status = r.status_flag;
        static const int kSubRegs[12] = {6, 10, 12, 18, 20, 21, 22, 23, 24, 26, 27, 28};
        for (int i = 0; i < 12; ++i) {
            for (int k = 0; k < 4; ++k) r.VF[kSubRegs[i]].I[k] = 0;
            ApplyMac(mac, status, 0x000Fu);
        }
        loadq(79, 1);
        loadq(80, 2);
        loadq(81, 3);
        loadq(82, 4);                     // lowers
        r.VI[13] = (uint16_t)(node + 4);  // IADDIU vi13, vi14, 4
        r.VI[15] = (uint16_t)node;        // IADD vi15, vi00, vi14
        // ADDi.xyzw vf08 = +0 + 32768.0.
        for (int k = 0; k < 4; ++k) r.VF[8].I[k] = 0x47000000u;
        ApplyMac(mac, status, 0);
        r.VF[12].I[3] = 0x47000000u;  // ADDi.w vf12
        ApplyMac(mac, status, 0);
        r.VF[21].I[3] = 0x47000000u;  // ADDi.w vf21
        ApplyMac(mac, status, 0);
        r.VF[12].I[3] = 0x00008000u;  // FTOI0.w
        loadq(83, 5);
        loadq(85, 14);                // lowers
        r.VF[21].I[3] = 0x00008000u;  // FTOI0.w
        r.VF[22].I[3] = 0x00000000u;  // FTOI0.w(+0)
        loadq(86, 25);
        // ADDi.xyzw vf23 = +0 + 62.9.
        for (int k = 0; k < 4; ++k) r.VF[23].I[k] = 0x427b999au;
        ApplyMac(mac, status, 0);
        // ADDx.xyzw vf07 = +0 + CF(vf05.x) broadcast -- input-dependent.
        {
            const float bc = CF(bits2f(r.VF[5].I[0]));
            const u32 res = f2bits(CF(0.0f + bc));
            u32 b4[4] = {res, res, res, res};
            for (int k = 0; k < 4; ++k) r.VF[7].I[k] = res;
            ApplyMac(mac, status, VuFloat::PackMacBits(b4, 0xF));
        }
        loadq(87, 26);
        // ADDi.y vf06 = +0 + 65535.0.
        r.VF[6].I[0] = 0;
        r.VF[6].I[1] = 0x477fff00u;
        r.VF[6].I[2] = 0;
        r.VF[6].I[3] = 0;
        ApplyMac(mac, status, 0);
        r.VI[11] = 1023;  // IADDIU vi11
        // MR32.yz vf27, vf14; MR32.yz vf28, vf25 (rotate right one lane).
        r.VF[27].I[1] = r.VF[14].I[2];
        r.VF[27].I[2] = r.VF[14].I[3];
        r.VF[28].I[1] = r.VF[25].I[2];
        r.VF[28].I[2] = r.VF[25].I[3];
        // ADDi.xyzw vf10 = +0 + 6720.0 -- the LAST FMAC push.
        for (int k = 0; k < 4; ++k) r.VF[10].I[k] = 0x45d20000u;
        ApplyMac(mac, status, 0);
        // MR32.xy vf27, vf27; MR32.xy vf28, vf28 (self-rotate, mask xy).
        {
            const u32 ty = r.VF[27].I[1], tz = r.VF[27].I[2];
            r.VF[27].I[0] = ty;
            r.VF[27].I[1] = tz;
            const u32 uy = r.VF[28].I[1], uz = r.VF[28].I[2];
            r.VF[28].I[0] = uy;
            r.VF[28].I[1] = uz;
        }
        r.VI[4] = (uint16_t)in.top;  // XTOP vi04
        r.I = 0x45d20000u;           // final I immediate
        r.mac_flag = mac;
        r.status_flag = status;
        r.TPC = kClassCTpc;
    }
    out.has_regs = true;
    return true;
}

bool ZeroKick::RunA(const Vu1HleInput& in, Vu1HleOutput& out) { return RunStub(in, out, 0); }
bool ZeroKick::RunB(const Vu1HleInput& in, Vu1HleOutput& out) { return RunStub(in, out, 1); }
bool ZeroKick::RunC(const Vu1HleInput& in, Vu1HleOutput& out) { return RunStub(in, out, 2); }

namespace {
// The class-C continuation's body. kFull: the whole contract (the working copy of data memory, the MAC / STATUS chain,
// the register file, the data writes). !kFull (ZeroKick::RunCContKicks): the kicks only -- the program runs over the
// caller's image in place (the caller discards it) and the MAC / STATUS chain, which nothing but the final registers
// observes (the FSAND tests read their producers' raw results directly), is not kept: the same arithmetic, the same
// packets.
template <bool kFull>
bool RunCContBody(const Vu1HleInput& in, u8* memory, Vu1HleOutput& out) {
    // FMAC rule for every op below: the RAW result feeds the MAC push, the
    // CLAMPED result is stored. A-form MULA/MADDA broadcast operands read
    // the RAW lane; dest-form broadcast ops read the CLAMPED lane. MOVE /
    // ITOF0 / FTOI0 / MTIR / MAX / MINI push nothing. Each FSAND observes
    // the push exactly four bundles earlier (the interpreter's MAC delay).
    if ((in.start_pc != kCContEntryPc && in.start_pc != kCContReentryPc) || in.regs == nullptr) return false;

    VuChopRoundGuard chop_guard;

    u8* const s_mem = kFull ? WorkingCopy(in).data() : memory;
    QwPtr m{s_mem};

    out.regs = *in.regs;
    VUregs& r = out.regs;
    uint16_t mac = r.mac_flag, status = r.status_flag;
    auto W = [&](int vf, int k) -> u32& { return r.VF[vf].I[k]; };
    auto vi = [&](int n) -> u16& { return r.VI[n]; };

    auto push = [&](const u32 b[4], u8 mask) {
        if constexpr (kFull) Push(mac, status, b, mask);
        else (void)b, (void)mask;
    };
    // Single-lane FMACs: raw = op(CF(a), CF(b)); push; store CF(raw).
    // Returns the RAW result bits (FSAND producers test its sign).
    auto sub_lane = [&](int vfd, int k, u32 a, u32 b) -> u32 {
        const float f = CF(bits2f(a)) - CF(bits2f(b));
        const u32 rb = f2bits(f);
        u32 raw[4] = {0, 0, 0, 0};
        raw[k] = rb;
        push(raw, (u8)(8u >> k));
        W(vfd, k) = f2bits(CF(f));
        return rb;
    };
    auto add_lane = [&](int vfd, int k, u32 a, u32 b) {
        const float f = CF(bits2f(a)) + CF(bits2f(b));
        u32 raw[4] = {0, 0, 0, 0};
        raw[k] = f2bits(f);
        push(raw, (u8)(8u >> k));
        W(vfd, k) = f2bits(CF(f));
    };
    auto mul_lane = [&](int vfd, int k, u32 a, u32 b) {
        const float f = CF(bits2f(a)) * CF(bits2f(b));
        u32 raw[4] = {0, 0, 0, 0};
        raw[k] = f2bits(f);
        push(raw, (u8)(8u >> k));
        W(vfd, k) = f2bits(CF(f));
    };
    // Masked vector ops (per-lane, both operands clamped).
    auto vec_add = [&](int vfd, int vfa, int vfb, u8 mask) {
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfa, k))) + CF(bits2f(W(vfb, k)));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto vec_sub = [&](int vfd, int vfa, int vfb, u8 mask) {
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfa, k))) - CF(bits2f(W(vfb, k)));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto vec_mul = [&](int vfd, int vfa, int vfb, u8 mask) {
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfa, k))) * CF(bits2f(W(vfb, k)));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    // Dest-form broadcast MUL/MULq (bc operand CLAMPED).
    auto mul_bc = [&](int vfd, int vfs, u32 bc_bits, u8 mask) {
        const float bc = CF(bits2f(bc_bits));
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfs, k))) * bc;
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    // A-form MULA/MADDA (bc operand RAW; ACC results clamp on store; the
    // MADD accumulation single-rounded).
    auto mula_bc = [&](int vfs, u32 bc_bits, u8 mask) {
        const float bc = bits2f(bc_bits);
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfs, k))) * bc;
                raw[k] = f2bits(f);
                r.ACC.I[k] = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto madda_bc = [&](int vfs, u32 bc_bits, u8 mask) {
        const float bc = bits2f(bc_bits);
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = VuFloat::Fma(CF(bits2f(W(vfs, k))), bc, CF(bits2f(r.ACC.I[k])));
                raw[k] = f2bits(f);
                r.ACC.I[k] = f2bits(CF(f));
            }
        push(raw, mask);
    };
    // Dest-form broadcast MADD (bc CLAMPED, reads architectural ACC).
    auto madd_bc = [&](int vfd, int vfs, u32 bc_bits, u8 mask) {
        const float bc = CF(bits2f(bc_bits));
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = VuFloat::Fma(CF(bits2f(W(vfs, k))), bc, CF(bits2f(r.ACC.I[k])));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    // DIV Q with the FDIV status commit (bit sets disjoint from the MAC push's).
    auto div_q = [&](u32 num_bits, u32 den_bits) -> float { return DivQ(status, num_bits, den_bits); };
    auto sq = [&](int vfs, u16 qw) {
        for (int k = 0; k < 4; ++k) m.SetW(qw, k, W(vfs, k));
    };
    auto lq = [&](int vfd, u16 qw) {
        for (int k = 0; k < 4; ++k) W(vfd, k) = m.W(qw, k);
    };

    // ---- 0x1ac8 branch-back entry: IADDIU vi13, vi14, 4 (no flags), then
    // ---- fall into the 0x1820 body below.
    if (in.start_pc == kCContReentryPc) vi(13) = (u16)(vi(14) + 4);

    // ---- preamble [0x1820, 0x18a0) ----
    {  // SUB.xyzw vf24,vf24,vf24 | XTOP vi04
        u32 raw[4];
        for (int k = 0; k < 4; ++k) {
            const float f = CF(bits2f(W(24, k))) - CF(bits2f(W(24, k)));
            raw[k] = f2bits(f);
            W(24, k) = f2bits(CF(f));
        }
        push(raw, 0xF);
        vi(4) = (u16)in.top;
    }
    lq(26, (u16)(vi(4) + 1));  // LQ vf26, 1(vi04)
    lq(18, (u16)(vi(4) + 2));  // LQ vf18, 2(vi04)
    vi(3) = (u16)(vi(4) + 1);  // IADDIU vi03
    // SUB.w vf26, vf05, vf26 | ILW.x vi02, 0(vi13)
    const u32 f1870_raw = sub_lane(26, 3, W(5, 3), W(26, 3));
    vi(2) = (u16)m.W(vi(13), 0);
    {  // MAX.xyzw vf31, vf00, vf00w (raw-bit, no flags) | vi08 = 8
        const u32 bc = W(0, 3);
        for (int k = 0; k < 4; ++k) W(31, k) = Fmax_raw(W(0, k), bc);
        vi(8) = 8;
    }
    // FSAND vi07 <- the SUB.w four bundles earlier (MAX pushes nothing) | MUL.w vf20 = vf26.w^2
    vi(7) = (f1870_raw >> 31) ? (u16)2 : (u16)0;
    mul_lane(20, 3, W(26, 3), W(26, 3));
    // MUL.xyzw vf18 *= vf26.w | vi05 = vi02 + 1
    mul_bc(18, 18, W(26, 3), 0xF);
    vi(5) = (u16)(vi(2) + 1);
    lq(10, (u16)vi(4));          // LQ vf10, 0(vi04)
    vi(12) = 63;
    W(17, 3) = Ftoi0(W(18, 3));  // FTOI0.w vf17
    sq(10, vi(2));               // SQ vf10 -> 0(vi02)

    // ---- packet loop ----
    float q_reg = r.Q;
    int kicks = 0;
    for (int packet_guard = 0;; ++packet_guard) {
        if (packet_guard > 256) return false;  // pathological node list
        for (int iter_guard = 0;; ++iter_guard) {
            if (iter_guard > 16) return false;  // vi08 walks 8 -> 0
            // ---- particle body [0x18a0, 0x1970] (common) ----
            vi(9) = (u16)W(17, 3);                          // MTIR vi09
            vec_add(26, 26, 18, 0xE);                       // ADD.xyz vf26 += vf18
            W(17, 3) = f2bits((float)(int32_t)W(17, 3));    // ITOF0.w (raw)
            vi(5) = (u16)(vi(5) + 9);
            mul_lane(20, 1, W(5, 1), W(20, 3));             // MUL.y vf20
            vi(3) = (u16)(vi(3) + 2);
            mula_bc(4, W(0, 3), 0xF);                       // MULA ACC = vf04*vf00w
            madda_bc(1, W(26, 0), 0xF);                     // MADDA += vf01*vf26x
            vi(9) = (u16)(vi(9) & vi(12));                  // IAND vi09 &= 63
            sub_lane(17, 3, W(18, 3), W(17, 3));            // SUB.w vf17
            vi(9) = (u16)(vi(9) + vi(9));                   // IADD vi09 *= 2
            add_lane(26, 1, W(26, 1), W(20, 1));            // ADD.y vf26
            lq(29, (u16)(90 + vi(9)));                      // LQ vf29
            madda_bc(3, W(26, 2), 0xF);                     // MADDA += vf03*vf26z
            vi(8) = (u16)(vi(8) - 1);                       // ISUBIU vi08
            // SUB.w vf18 = vf23.w - vf18.w  [FSAND producer]
            const u32 f1900_raw = sub_lane(18, 3, W(23, 3), W(18, 3));
            lq(30, (u16)(91 + vi(9)));                      // LQ vf30
            sub_lane(20, 3, W(0, 3), W(17, 3));             // SUB.w vf20 = 1 - vf17.w
            lq(15, (u16)(88 + vi(9)));                      // LQ vf15
            mul_bc(29, 29, W(17, 3), 0xF);                  // MUL vf29 *= vf17.w
            lq(19, (u16)(89 + vi(9)));                      // LQ vf19
            madd_bc(16, 2, W(26, 1), 0xF);                  // MADD vf16 (position)
            // FSAND vi12 <- the vf18 push | MUL.xy vf30 *= vf17.w
            vi(12) = (f1900_raw >> 31) ? (u16)2 : (u16)0;
            mul_bc(30, 30, W(17, 3), 0xC);
            mula_bc(15, W(20, 3), 0xF);                     // MULA ACC = vf15*vf20w
            sq(15, (u16)(vi(5) - 8));                       // SQ vf15 (RGBAQ row)
            madd_bc(15, 29, W(0, 3), 0xF);                  // MADD vf15 = ACC+vf29
            lq(26, vi(3));                                  // LQ vf26 (next pair A)
            // SUB.w vf24 = vf16.w - vf07.w  [FSAND producer]
            const u32 f1938_raw = sub_lane(24, 3, W(16, 3), W(7, 3));
            q_reg = div_q(W(0, 3), W(16, 3));               // DIV Q, vf00w, vf16w
            mula_bc(19, W(20, 3), 0xC);                     // MULA.xy ACC
            vi(12) = (u16)(vi(12) + vi(7));                 // vi12 += vi07 (prev)
            madd_bc(19, 30, W(0, 3), 0xC);                  // MADD.xy vf19
            lq(18, (u16)(vi(3) + 1));                       // LQ vf18 (next pair B)
            // SUB.w vf26 = vf05.w - vf26.w  [FSAND producer]
            const u32 f1950_raw = sub_lane(26, 3, W(5, 3), W(26, 3));
            W(12, 2) = W(16, 2);                            // MOVE.z vf12 (raw)
            vi(7) = (f1938_raw >> 31) ? (u16)2 : (u16)0;    // FSAND vi07
            vec_mul(15, 15, 31, 0xC);                       // MUL.xy vf15 *= vf31
            mul_lane(22, 0, W(31, 0), W(15, 2));            // MUL.x vf22 = vf31x*vf15z
            W(21, 2) = W(16, 2);                            // MOVE.z vf21 (raw)
            vec_mul(19, 19, 31, 0xC);                       // MUL.xy vf19 *= vf31
            vi(12) = (u16)(vi(12) + vi(7));                 // vi12 += vi07
            vi(7) = (f1950_raw >> 31) ? (u16)2 : (u16)0;    // FSAND vi07
            mul_lane(22, 1, W(31, 1), W(15, 3));            // MUL.y vf22 = vf31y*vf15w
            vec_add(12, 16, 15, 0xC);                       // ADD.xy vf12
            vec_add(16, 16, 19, 0xC);                       // ADD.xy vf16
            W(17, 2) = W(16, 2);                            // MOVE.z vf17 (z unwritten)
            mul_bc(18, 18, W(26, 3), 0xF);                  // MUL vf18 *= vf26.w
            const bool deg = ((int16_t)vi(12) > 0);         // IBGTZ vi12
            vec_add(17, 12, 22, 0xC);                       // ADD.xy vf17 [delay]
            vi(12) = 63;                                    // vi12 = 63 [delay]

            if (!deg) {
                // ---- full-quad path [0x1978, 0x1a20] ----
                vec_sub(21, 16, 22, 0xC);           // SUB.xy vf21
                const u32 qb = f2bits(CF(q_reg));   // the visible Q clamps
                mul_bc(12, 12, qb, 0xE);            // MUL.xyz vf12 *= Q
                mul_bc(16, 16, qb, 0xE);            // MUL.xyz vf16 *= Q
                mul_bc(22, 17, qb, 0xE);            // MUL.xyz vf22 = vf17*Q
                mul_bc(21, 21, qb, 0xE);            // MUL.xyz vf21 *= Q
                sq(25, (u16)(vi(5) - 6));           // SQ vf25 (UV const)
                // MAX.xy vs vf06.x; MINI.xy vs vf06.y (raw-bit compares, no
                // flags) + interleaved SQs.
                for (int v : {12, 16, 22, 21}) {
                    W(v, 0) = Fmax_raw(W(v, 0), W(6, 0));
                    W(v, 1) = Fmax_raw(W(v, 1), W(6, 0));
                }
                sq(27, (u16)(vi(5) - 4));  // SQ vf27 (UV const)
                sq(28, (u16)(vi(5) - 2));  // SQ vf28 (UV const)
                sq(14, (u16)(vi(5) - 9));  // SQ vf14 (UV const)
                for (int v : {12, 16, 22, 21}) {
                    W(v, 0) = Fmin_raw(W(v, 0), W(6, 1));
                    W(v, 1) = Fmin_raw(W(v, 1), W(6, 1));
                }
                for (int k = 0; k < 3; ++k) W(12, k) = Ftoi0(W(12, k));
                W(16, 3) = W(22, 3);  // MOVE.w vf16 = vf22.w
                for (int k = 0; k < 3; ++k) W(16, k) = Ftoi0(W(16, k));
                for (int k = 0; k < 3; ++k) W(22, k) = Ftoi0(W(22, k));
                for (int k = 0; k < 3; ++k) W(21, k) = Ftoi0(W(21, k));
                sq(12, (u16)(vi(5) - 7));             // SQ vf12 (XYZ2 A)
                mul_lane(20, 3, W(26, 3), W(26, 3));  // MUL.w vf20 (next pair)
                sq(16, (u16)(vi(5) - 1));             // SQ vf16 (XYZ2 D)
                W(17, 3) = Ftoi0(W(18, 3));           // FTOI0.w vf17
                sq(22, (u16)(vi(5) - 3));             // SQ vf22 (XYZ2 C)
                sq(21, (u16)(vi(5) - 5));             // SQ vf21 [delay]
            } else {
                // ---- degenerate path [0x1a40, 0x1a60]: 4x XYZ2 = vf12 ----
                mul_lane(20, 3, W(26, 3), W(26, 3));  // MUL.w vf20
                sq(12, (u16)(vi(5) - 7));
                W(17, 3) = Ftoi0(W(18, 3));  // FTOI0.w vf17
                sq(12, (u16)(vi(5) - 5));
                {  // SUB.xyz vf24, vf24, vf24 (zero-fill, flags xyz)
                    u32 raw[4] = {0, 0, 0, 0};
                    for (int k = 0; k < 3; ++k) {
                        const float f = CF(bits2f(W(24, k))) - CF(bits2f(W(24, k)));
                        raw[k] = f2bits(f);
                        W(24, k) = f2bits(CF(f));
                    }
                    push(raw, 0xE);
                }
                sq(12, (u16)(vi(5) - 3));
                sq(12, (u16)(vi(5) - 1));  // SQ vf12 [delay]
            }
            if (vi(8) == 0) break;  // IBNE fall-through
        }
        if (++kicks > 64) return false;
        if (!KickAt(m, vi(2), out)) return false;  // XGKICK
        // ---- epilogue [0x1a78, 0x1ab0] ----
        vi(13) = (u16)(vi(13) - 1);            // ISUBIU vi13
        vi(2) = (u16)m.W(vi(13), 0);           // ILW.x vi02
        const bool done = (vi(14) == vi(13));  // IBEQ
        vi(8) = 8;                             // [delay, both paths]
        if (done) break;                       // E-bit
        vi(5) = (u16)(vi(2) + 1);              // IADDIU vi05
        sq(10, vi(2));                         // SQ vf10 [delay]
    }

    r.Q = q_reg;
    r.TPC = kCContTpc;
    if constexpr (kFull) {
        r.mac_flag = mac;
        r.status_flag = status;
        out.has_regs = true;
        if (in.data_writes) EmitDataWrites(s_mem, in, out);
    } else {
        out.has_regs = false;   // MAC / STATUS not reproduced
    }
    return true;
}
}  // namespace

bool ZeroKick::RunCCont(const Vu1HleInput& in, Vu1HleOutput& out) { return RunCContBody<true>(in, nullptr, out); }

bool ZeroKick::RunCContKicks(const Vu1HleInput& in, uint8_t* memory, Vu1HleOutput& out) {
    if (!memory || memory != in.data) return false;
    return RunCContBody<false>(in, memory, out);
}


namespace {
// The class-A continuation's body; kFull as RunCContBody's (!kFull: ZeroKick::RunAContKicks -- the kicks only, in place,
// no MAC / STATUS chain: nothing in this program reads the flags back).
template <bool kFull>
bool RunAContBody(const Vu1HleInput& in, u8* memory, Vu1HleOutput& out) {
    // Q consumption follows the FDIV pipeline: each MULq reads the quotient
    // of the DIV issued seven bundles earlier (q1..q4 below); the final Q is
    // the last DIV. Same-bundle hazards: a lower-pipe MR32 / SQ reads its
    // source BEFORE the upper op of the same bundle writes it.
    if (in.start_pc != kACContEntryPc || in.regs == nullptr) return false;

    VuChopRoundGuard chop_guard;

    u8* const s_mem = kFull ? WorkingCopy(in).data() : memory;
    QwPtr m{s_mem};

    out.regs = *in.regs;
    VUregs& r = out.regs;
    uint16_t mac = r.mac_flag, status = r.status_flag;
    auto W = [&](int vf, int k) -> u32& { return r.VF[vf].I[k]; };
    auto vi = [&](int n) -> u16& { return r.VI[n]; };

    auto push = [&](const u32 b[4], u8 mask) {
        if constexpr (kFull) Push(mac, status, b, mask);
        else (void)b, (void)mask;
    };
    auto sub_lane = [&](int vfd, int k, u32 a, u32 b) {
        const float f = CF(bits2f(a)) - CF(bits2f(b));
        u32 raw[4] = {0, 0, 0, 0};
        raw[k] = f2bits(f);
        push(raw, (u8)(8u >> k));
        W(vfd, k) = f2bits(CF(f));
    };
    auto add_lane = [&](int vfd, int k, u32 a, u32 b) {
        const float f = CF(bits2f(a)) + CF(bits2f(b));
        u32 raw[4] = {0, 0, 0, 0};
        raw[k] = f2bits(f);
        push(raw, (u8)(8u >> k));
        W(vfd, k) = f2bits(CF(f));
    };
    auto vec_add = [&](int vfd, int vfa, int vfb, u8 mask) {
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfa, k))) + CF(bits2f(W(vfb, k)));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto vec_sub = [&](int vfd, int vfa, int vfb, u8 mask) {
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfa, k))) - CF(bits2f(W(vfb, k)));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto mul_bc = [&](int vfd, int vfs, u32 bc_bits, u8 mask) {
        const float bc = CF(bits2f(bc_bits));
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfs, k))) * bc;
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto mula_bc = [&](int vfs, u32 bc_bits, u8 mask) {
        const float bc = bits2f(bc_bits);
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = CF(bits2f(W(vfs, k))) * bc;
                raw[k] = f2bits(f);
                r.ACC.I[k] = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto madda_bc = [&](int vfs, u32 bc_bits, u8 mask) {
        const float bc = bits2f(bc_bits);
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = VuFloat::Fma(CF(bits2f(W(vfs, k))), bc, CF(bits2f(r.ACC.I[k])));
                raw[k] = f2bits(f);
                r.ACC.I[k] = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto madd_bc = [&](int vfd, int vfs, u32 bc_bits, u8 mask) {
        const float bc = CF(bits2f(bc_bits));
        u32 raw[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) {
                const float f = VuFloat::Fma(CF(bits2f(W(vfs, k))), bc, CF(bits2f(r.ACC.I[k])));
                raw[k] = f2bits(f);
                W(vfd, k) = f2bits(CF(f));
            }
        push(raw, mask);
    };
    auto div_q = [&](u32 num_bits, u32 den_bits) -> float { return DivQ(status, num_bits, den_bits); };
    auto sq = [&](int vfs, u16 qw) {
        for (int k = 0; k < 4; ++k) m.SetW(qw, k, W(vfs, k));
    };
    auto lq = [&](int vfd, u16 qw) {
        for (int k = 0; k < 4; ++k) W(vfd, k) = m.W(qw, k);
    };
    // ITOF12 (raw op, no flags): float(int32)/4096.0f per masked lane.
    auto itof12 = [&](int vfd, u8 mask) {
        for (int k = 0; k < 4; ++k)
            if (mask & (8u >> k)) W(vfd, k) = f2bits((float)(int32_t)W(vfd, k) / 4096.0f);
    };

    // ---- preamble [0x1750, 0x1790] ----
    vec_sub(6, 0, 0, 0xF);                 // SUB vf06 = vf00-vf00
    vi(2) = (u16)(vi(14) + 4);             // IADDIU vi02 = vi14+4
    vi(3) = (u16)m.W(vi(2), 0);            // ILW.x vi03 = QW[vi02].x
    r.I = 0x47000000u;                     // LOI 32768.0
    add_lane(6, 3, W(6, 3), 0x47000000u);  // ADDi.w vf06 += I
    vi(4) = 8;                             // IADDIU vi04 = 8
    vi(1) = (u16)in.top;                   // XTOP vi01
    sub_lane(7, 3, W(0, 3), W(0, 3));      // SUB.w vf07 = +0
    vi(5) = vi(3);                         // IADDIU vi05 = vi03
    W(6, 3) = Ftoi0(W(6, 3));              // FTOI0.w vf06 (0x8000)
    sq(5, vi(3));                          // SQI vf05 -> (vi03++)
    vi(3) = (u16)(vi(3) + 1);

    float q_reg = r.Q;
    int kicks = 0;
    for (int packet_guard = 0;; ++packet_guard) {
        if (packet_guard > 256) return false;      // pathological node list
        for (int iter_guard = 0;; ++iter_guard) {  // vi04 walks 8 -> 0
            if (iter_guard > 16) return false;
            lq(8, (u16)(vi(1) + 32));         // LQ vf08 = QW[vi01+32]
            vi(6) = (u16)(W(8, 3) & 0xFFFF);  // MTIR vi06 = low16(vf08.w)
            if (vi(6) == 0) {                 // IBNE fall-through: FULL
                itof12(8, 0xE);               // ITOF12.xyz vf08
                lq(11, (u16)vi(1));           // LQ vf11 = QW[vi01]
                vec_add(10, 11, 8, 0xE);      // ADD.xyz vf10 = vf11+vf08
                mula_bc(4, W(0, 3), 0xF);     // MULAw ACC = vf04*vf00w
                vec_sub(12, 11, 8, 0xE);      // SUB.xyz vf12 = vf11-vf08
                madda_bc(1, W(10, 0), 0xF);   // MADDAx ACC += vf01*vf10x
                for (int k = 0; k < 3; ++k)   // LQ.xyz vf07 (w preserved)
                    W(7, k) = m.W((u16)(vi(1) + 64), k);
                madda_bc(2, W(10, 1), 0xF);   // MADDAy
                vi(7) = 63;                   // IADDIU vi07 = 63
                madd_bc(15, 3, W(10, 2), 0xF);           // MADDz vf15 = ACC+vf03*vf10z
                vi(6) = (u16)(W(11, 3) & 0xFFFF);        // MTIR vi06 = low16(vf11.w)
                W(9, 3) = Fmax_raw(W(6, 3), W(6, 3));    // MAX.w vf09 (raw)
                vi(6) = (u16)(vi(6) & vi(7));            // IAND vi06 &= 63
                itof12(7, 0xE);                          // ITOF12.xyz vf07
                vi(6) = (u16)(vi(6) + vi(6));            // IADD vi06 *= 2
                mula_bc(4, W(0, 3), 0xF);                // MULAw ACC = vf04
                lq(8, (u16)(85 + vi(6)));                // LQ vf08 = QW[85+vi06]
                madda_bc(1, W(12, 0), 0xF);              // MADDAx ACC += vf01*vf12x
                const float q1 = div_q(W(0, 3), W(15, 3));  // DIV Q1 = 1/vf15.w
                madda_bc(2, W(12, 1), 0xF);              // MADDAy
                lq(10, (u16)(84 + vi(6)));               // LQ vf10 = QW[84+vi06]
                madd_bc(14, 3, W(12, 2), 0xF);           // MADDz vf14 = ACC+vf03*vf12z
                vec_sub(13, 11, 7, 0xE);                 // SUB.xyz vf13 = vf11-vf07
                r.R = (W(0, 3) & 0x7FFFFFu) | 0x3F800000u;  // RINIT R, vf00.w
                vec_add(9, 11, 7, 0xE);                  // ADD.xyz vf09 = vf11+vf07
                {                                        // MR32.xyz vf11 = rot(vf08)
                    const u32 sy = W(8, 1), sz = W(8, 2), sw = W(8, 3);
                    W(11, 0) = sy;
                    W(11, 1) = sz;
                    W(11, 2) = sw;
                }
                {  // MR32.xyz vf07 = rot(vf10)
                    const u32 sy = W(10, 1), sz = W(10, 2), sw = W(10, 3);
                    W(7, 0) = sy;
                    W(7, 1) = sz;
                    W(7, 2) = sw;
                }
                W(10, 2) = r.R;                          // RGET.z vf10
                mula_bc(4, W(0, 3), 0xF);                // MULAw ACC = vf04
                const float q2 = div_q(W(0, 3), W(14, 3));  // DIV Q2 = 1/vf14.w
                madda_bc(1, W(9, 0), 0xF);               // MADDAx ACC += vf01*vf09x
                madda_bc(2, W(9, 1), 0xF);               // MADDAy
                {                                        // MR32.xyw vf12 = rot(vf11)
                    // SAME-BUNDLE: the lower MR32 reads vf11 BEFORE the MADDz.
                    const u32 sx = W(11, 0), sy = W(11, 1), sz = W(11, 2);
                    madd_bc(11, 3, W(9, 2), 0xF);  // MADDz vf11 = ACC+vf03*vf09z
                    W(12, 0) = sy;
                    W(12, 1) = sz;
                    W(12, 3) = sx;
                }
                mul_bc(10, 10, f2bits(q1), 0xE);         // MULq.xyz vf10 *= Q1
                mul_bc(9, 15, f2bits(q1), 0xE);          // MULq.xyz vf09 = vf15*Q1
                mula_bc(4, W(0, 3), 0xF);                // MULAw ACC = vf04
                lq(16, (u16)(vi(1) + 96));               // LQ vf16 = QW[vi01+96]
                madda_bc(1, W(13, 0), 0xF);              // MADDAx ACC += vf01*vf13x
                const float q3 = div_q(W(0, 3), W(11, 3));  // DIV Q3 = 1/vf11.w
                madda_bc(2, W(13, 1), 0xF);              // MADDAy
                sq(10, (u16)(vi(3) + 3));                // SQ vf10 -> 3(vi03)
                madd_bc(10, 3, W(13, 2), 0xF);           // MADDz vf10 = ACC+vf03*vf13z
                W(12, 2) = r.R;                          // RGET.z vf12
                mul_bc(13, 14, f2bits(q2), 0xE);         // MULq.xyz vf13 = vf14*Q2
                {                                        // MR32.xyw vf14 = rot(vf07)
                    const u32 sx = W(7, 0), sy = W(7, 1), sz = W(7, 2);
                    W(14, 0) = sy;
                    W(14, 1) = sz;
                    W(14, 3) = sx;
                }
                for (int k = 0; k < 3; ++k)  // FTOI0.xyz vf09
                    W(9, k) = Ftoi0(W(9, k));
                mul_bc(12, 12, f2bits(q2), 0xE);         // MULq.xyz vf12 *= Q2
                sq(16, (u16)(vi(3) + 1));                // SQ vf16 -> 1(vi03)
                const float q4 = div_q(W(0, 3), W(10, 3));  // DIV Q4 = 1/vf10.w
                W(11, 3) = Fmax_raw(W(6, 3), W(6, 3));   // MAX.w vf11
                W(14, 2) = r.R;                          // RGET.z vf14
                sq(9, (u16)(vi(3) + 5));                 // SQ vf09 -> 5 (PRE-upper)
                W(9, 3) = Fmax_raw(W(7, 3), W(7, 3));    // MAX.w vf09 = vf07.w
                mul_bc(11, 11, f2bits(q3), 0xE);         // MULq.xyz vf11 *= Q3
                sq(16, (u16)(vi(3) + 4));                // SQ vf16 -> 4(vi03)
                for (int k = 0; k < 3; ++k)  // FTOI0.xyz vf09 = ftoi0(vf13)
                    W(9, k) = Ftoi0(W(13, k));
                W(8, 2) = r.R;                           // RGET.z vf08
                mul_bc(14, 14, f2bits(q3), 0xE);         // MULq.xyz vf14 *= Q3
                sq(16, (u16)(vi(3) + 7));                // SQ vf16 -> 7(vi03)
                mul_bc(10, 10, f2bits(q4), 0xE);         // MULq.xyz vf10 *= Q4
                sq(16, (u16)(vi(3) + 10));               // SQ vf16 -> 10(vi03)
                for (int k = 0; k < 3; ++k)  // FTOI0.xyz vf11
                    W(11, k) = Ftoi0(W(11, k));
                sq(12, (u16)(vi(3) + 6));                // SQ vf12 -> 6(vi03)
                mul_bc(8, 8, f2bits(q4), 0xE);           // MULq.xyz vf08 *= Q4
                W(10, 3) = W(7, 3);                      // MOVE.w vf10 = vf07.w
                sq(9, (u16)(vi(3) + 8));                 // SQ vf09 -> 8(vi03)
                for (int k = 0; k < 3; ++k)  // FTOI0.xyz vf10
                    W(10, k) = Ftoi0(W(10, k));
                sq(14, (u16)vi(3));                      // SQ vf14 -> 0(vi03)
                sq(11, (u16)(vi(3) + 2));                // SQ vf11 -> 2(vi03)
                sq(8, (u16)(vi(3) + 9));                 // SQ vf08 -> 9(vi03)
                sq(10, (u16)(vi(3) + 11));               // SQ vf10 -> 11 [delay]
                q_reg = q4;
            } else {  // degenerate path [0x1940, 0x1958]
                sq(6, (u16)(vi(3) + 8));
                sq(6, (u16)(vi(3) + 5));
                sq(6, (u16)(vi(3) + 2));
                sq(6, (u16)(vi(3) + 11));
            }
            vi(4) = (u16)(vi(4) - 1);        // ISUBIU vi04
            vi(3) = (u16)(vi(3) + 12);       // IADDIU vi03 += 12
            const bool more = (vi(4) != 0);  // IBNE
            vi(1) = (u16)(vi(1) + 1);        // IADDIU vi01 [delay]
            if (!more) break;
        }
        vi(2) = (u16)(vi(2) - 1);  // ISUBIU vi02
        if (++kicks > 64) return false;
        if (!KickAt(m, vi(5), out)) return false;  // XGKICK vi05
        const bool done = (vi(2) == vi(14));       // IBEQ
        vi(3) = (u16)m.W(vi(2), 0);                // ILW.x vi03 [delay]
        if (done) break;                           // E-bit
        vi(4) = 8;                                 // IADDIU vi04 = 8
        vi(5) = vi(3);                             // IADDIU vi05 = vi03
        sq(5, vi(3));                              // SQI vf05 [delay]
        vi(3) = (u16)(vi(3) + 1);
    }

    r.Q = q_reg;
    r.TPC = kACContTpc;
    if constexpr (kFull) {
        r.mac_flag = mac;
        r.status_flag = status;
        out.has_regs = true;
        if (in.data_writes) EmitDataWrites(s_mem, in, out);
    } else {
        out.has_regs = false;   // MAC / STATUS not reproduced
    }
    return true;
}
}  // namespace

bool ZeroKick::RunACont(const Vu1HleInput& in, Vu1HleOutput& out) { return RunAContBody<true>(in, nullptr, out); }

bool ZeroKick::RunAContKicks(const Vu1HleInput& in, uint8_t* memory, Vu1HleOutput& out) {
    if (!memory || memory != in.data) return false;
    return RunAContBody<false>(in, memory, out);
}
