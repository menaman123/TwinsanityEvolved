#pragma once
// vs_zero_kick: the register-setup stubs the family's E-bit-terminated
// materials leave behind (no XGKICK at entry 0) and their MSCNT
// continuations. Every program here reproduces the FULL register file
// (has_regs) -- the game's next MSCNT consumes it.
//
// Entry 0 (the dispatcher prologue: vi14 = QW3.x, vi12 = 1, QW9.x = 1 as
// the only data write, vi13 = QW[node].x, vi15 = the JALR link; an empty
// list exits at the family TPC): class A loads vf01..05 = QW79..83 and exits
// at TPC 0x1750; class B seeds vi13 = node+1, vi12 = 0, vf01/vf02 =
// QW[node+3..4] and exits at 0x1748; class C zero-fills twelve registers,
// loads its constants and exits at 0x1820 with the MAC / STATUS chain of
// every FMAC applied in program order.
// 0x1820 / 0x1ac8 (the class-C continuation): a particle quad emitter --
// packets from QW[vi13].x down to QW[vi14+1].x, 8 particles per packet, 9
// QWs each; the mid-loop FSAND tests read the S bit of a pinned producer
// (the SUB.w four bundles earlier) and the final flags are the program-order
// chain; TPC 0x1ac8.
// 0x1750 (the class-A continuation): a billboard emitter -- 8 particles per
// packet, 12 QWs each (4 corners x {UV, RGBA, XYZ2}), corner offsets
// ITOF12'd, each corner projected by its own quotient in the FDIV pipeline's
// consumption order; degenerate particles store {+0,+0,+0,0x8000}; TPC
// 0x19e0.
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class ZeroKick {
public:
    static bool RunA(const Vu1HleInput& in, Vu1HleOutput& out);
    static bool RunB(const Vu1HleInput& in, Vu1HleOutput& out);
    static bool RunC(const Vu1HleInput& in, Vu1HleOutput& out);
    static bool RunCCont(const Vu1HleInput& in, Vu1HleOutput& out);
    // RunCCont's kicks alone, for a caller that reads nothing else and discards the image afterwards: the program runs in
    // place over `memory` (== in.data, mutable: the caller's own image), with no working copy, no MAC / STATUS chain and no
    // data writes (out.has_regs false). The kicks are RunCCont's byte for byte.
    static bool RunCContKicks(const Vu1HleInput& in, uint8_t* memory, Vu1HleOutput& out);
    static bool RunACont(const Vu1HleInput& in, Vu1HleOutput& out);
    // RunACont's kicks alone, in place over `memory` (== in.data), as RunCContKicks
    static bool RunAContKicks(const Vu1HleInput& in, uint8_t* memory, Vu1HleOutput& out);

private:
    // cls: 0 = A (five LQs), 1 = B (node regs + two LQs), 2 = C.
    static bool RunStub(const Vu1HleInput& in, Vu1HleOutput& out, int cls);
};
