// platform/nativeee (host side) -- TE_MACHINE_BENCH=1 (the Vita): the processor's and the memory's ceilings measured on
// the console once at start-up, before the game runs (~0.2 s on the main thread and two helper threads: each case ~1-2 ms
// at 444 MHz, three times; the 4 MB chase and streams the longest), so that a device
// log carries what an instruction, a stall and a byte cost here: the cost of the same work at the machine's limits is what
// the game's and the drawing thread's measured cycles are compared with. Every case is a fixed instruction sequence in
// inline assembly (the compiler cannot reorder or remove it), timed on the kernel clock over ~1-2 ms, the best of three,
// in cycles of the ARM clock read at the time. Three lines:
//   "[te.machine] core: ..."    integer issue (4 independent chains = the dual issue's limit), an add's latency; VFP single
//                               and double precision latencies (dependent chains) and throughputs (8 independent chains);
//                               NEON quadword latency / throughput; VDIV / VSQRT; a compare whose flags go to the core
//                               (VCMP + VMRS + a branch); a value moved VFP -> core -> VFP
//   "[te.machine] memory: ..."  a load's latency by working set (a random chase of 32-byte lines: L1, L2, DRAM -- the knees
//                               are the caches' sizes); streaming read / write / copy bandwidth (L2-resident and DRAM); two
//                               cores streaming at once (the shared bus: the game and the drawing threads share it)
//   "[te.machine] calls: ..."   an indirect call with four arguments, the EABI's unsigned division, a thread_local read
//                               (the Vita's emulated TLS), a sequentially consistent atomic increment, the kernel clock
#include "te_prof.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(__vita__)
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#endif

namespace te::prof {

#if defined(__vita__)
namespace {

int64_t Us() { return int64_t(sceKernelGetProcessTimeWide()); }

double g_mhz = 444.0;

// cycles an operation: the best of three runs of fn(iterations), each `ops` operations an iteration
template <typename F>
double Cycles(uint32_t iterations, double ops, F&& fn) {
    double best = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        const int64_t t0 = Us();
        fn(iterations);
        const int64_t t1 = Us();
        best = std::min(best, double(t1 - t0));
    }
    return best * g_mhz / (double(iterations) * ops);
}

// ---- the core ------------------------------------------------------------------------------------------------------
#define TE_X4(s) s s s s
#define TE_X16(s) TE_X4(s) TE_X4(s) TE_X4(s) TE_X4(s)

void IntIndependent(uint32_t n) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    asm volatile("1:\n" TE_X4("add %[a], %[a], #1\n add %[b], %[b], #1\n add %[c], %[c], #1\n add %[d], %[d], #1\n")
                 "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n), [a] "+r"(a), [b] "+r"(b), [c] "+r"(c), [d] "+r"(d)
                 :
                 : "cc");
}
void IntDependent(uint32_t n) {
    uint32_t a = 0;
    asm volatile("1:\n" TE_X16("add %[a], %[a], #1\n") "subs %[n], %[n], #1\n bne 1b\n" : [n] "+r"(n), [a] "+r"(a) : : "cc");
}
// VFP single precision: dependent chains (latency) and 8 independent chains (throughput)
void VaddDep(uint32_t n) {
    asm volatile("vmov.f32 s0, #1.0\n vmov.f32 s1, #0.5\n 1:\n" TE_X16("vadd.f32 s0, s0, s1\n") "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "s0", "s1");
}
void VmulDep(uint32_t n) {
    asm volatile("vmov.f32 s0, #1.0\n vmov.f32 s1, #1.0\n 1:\n" TE_X16("vmul.f32 s0, s0, s1\n") "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "s0", "s1");
}
void VmlaDep(uint32_t n) {
    asm volatile("vmov.f32 s0, #1.0\n vmov.f32 s1, #1.0\n vmov.f32 s2, #0.5\n 1:\n" TE_X16("vmla.f32 s0, s1, s2\n")
                 "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "s0", "s1", "s2");
}
void VaddInd(uint32_t n) {
    asm volatile("vmov.f32 s8, #0.5\n 1:\n"
                 TE_X4("vadd.f32 s0, s0, s8\n vadd.f32 s1, s1, s8\n vadd.f32 s2, s2, s8\n vadd.f32 s3, s3, s8\n"
                       "vadd.f32 s4, s4, s8\n vadd.f32 s5, s5, s8\n vadd.f32 s6, s6, s8\n vadd.f32 s7, s7, s8\n")
                 "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8");
}
void VmlaInd(uint32_t n) {
    asm volatile("vmov.f32 s8, #0.5\n vmov.f32 s9, #1.0\n 1:\n"
                 TE_X4("vmla.f32 s0, s8, s9\n vmla.f32 s1, s8, s9\n vmla.f32 s2, s8, s9\n vmla.f32 s3, s8, s9\n"
                       "vmla.f32 s4, s8, s9\n vmla.f32 s5, s8, s9\n vmla.f32 s6, s8, s9\n vmla.f32 s7, s8, s9\n")
                 "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9");
}
// double precision
void DaddDep(uint32_t n) {
    asm volatile("vmov.f64 d0, #1.0\n vmov.f64 d1, #0.5\n 1:\n" TE_X16("vadd.f64 d0, d0, d1\n") "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "d0", "d1");
}
void DmulDep(uint32_t n) {
    asm volatile("vmov.f64 d0, #1.0\n vmov.f64 d1, #1.0\n 1:\n" TE_X16("vmul.f64 d0, d0, d1\n") "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "d0", "d1");
}
// division and square root (dependent: a value that stays 1)
void VdivDep(uint32_t n) {
    asm volatile("vmov.f32 s0, #1.0\n vmov.f32 s1, #1.0\n 1:\n" TE_X4("vdiv.f32 s0, s1, s0\n") "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "s0", "s1");
}
void VsqrtDep(uint32_t n) {
    asm volatile("vmov.f32 s0, #1.0\n 1:\n" TE_X4("vsqrt.f32 s0, s0\n") "subs %[n], %[n], #1\n bne 1b\n" : [n] "+r"(n) : : "cc", "s0");
}
// NEON quadwords
void QaddDep(uint32_t n) {
    asm volatile("vmov.f32 q0, #1.0\n vmov.f32 q1, #0.5\n 1:\n" TE_X16("vadd.f32 q0, q0, q1\n") "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "d0", "d1", "d2", "d3");
}
void QmlaDep(uint32_t n) {
    asm volatile("vmov.f32 q0, #1.0\n vmov.f32 q1, #1.0\n vmov.f32 q2, #0.5\n 1:\n" TE_X16("vmla.f32 q0, q1, q2\n")
                 "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "d0", "d1", "d2", "d3", "d4", "d5");
}
void QmlaInd(uint32_t n) {
    asm volatile("vmov.f32 q8, #0.5\n vmov.f32 q9, #1.0\n 1:\n"
                 TE_X4("vmla.f32 q0, q8, q9\n vmla.f32 q1, q8, q9\n vmla.f32 q2, q8, q9\n vmla.f32 q3, q8, q9\n"
                       "vmla.f32 q4, q8, q9\n vmla.f32 q5, q8, q9\n vmla.f32 q6, q8, q9\n vmla.f32 q7, q8, q9\n")
                 "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15", "d16", "d17", "d18", "d19");
}
// a compare whose flags the core branches on: VCMP, VMRS APSR_nzcv, a branch (the VFP -> core flag transfer)
void VcmpVmrs(uint32_t n) {
    asm volatile("vmov.f32 s0, #1.0\n vmov.f32 s1, #0.5\n 1:\n"
                 TE_X4("vcmp.f32 s0, s1\n vmrs APSR_nzcv, fpscr\n bgt 2f\n nop\n 2:\n")
                 "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : : "cc", "s0", "s1");
}
// the same compare done in the core (the bits of two positive floats compare as integers): the baseline
void CoreCmp(uint32_t n) {
    uint32_t a = 0x3f800000u, b = 0x3f000000u;
    asm volatile("1:\n" TE_X4("cmp %[a], %[b]\n bgt 2f\n nop\n 2:\n") "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n) : [a] "r"(a), [b] "r"(b) : "cc");
}
// a value VFP -> core -> VFP, dependent (VMOV r, s; an add; VMOV s, r)
void VfpCoreTrip(uint32_t n) {
    uint32_t r = 0;
    asm volatile("vmov.f32 s0, #1.0\n 1:\n" TE_X4("vmov %[r], s0\n add %[r], %[r], #0\n vmov s0, %[r]\n")
                 "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n), [r] "+r"(r) : : "cc", "s0");
}

// ---- the memory ----------------------------------------------------------------------------------------------------
// a load's latency: a chase through `bytes` of 32-byte lines in a random cycle (every line once a lap)
double ChaseCycles(uint32_t* buf, uint32_t bytes, uint32_t loads) {
    const uint32_t lines = bytes / 32;
    std::vector<uint32_t> order(lines);
    for (uint32_t i = 0; i < lines; ++i) order[i] = i;
    uint32_t seed = 0x9e3779b9u ^ bytes;
    for (uint32_t i = lines - 1; i > 0; --i) {
        seed = seed * 1664525u + 1013904223u;
        std::swap(order[i], order[seed % (i + 1)]);
    }
    for (uint32_t i = 0; i < lines; ++i) {
        uint32_t* from = buf + order[i] * 8;
        *from = reinterpret_cast<uint32_t>(buf + order[(i + 1) % lines] * 8);
    }
    uint32_t* p = buf + order[0] * 8;
    for (uint32_t i = 0; i < lines; ++i) p = reinterpret_cast<uint32_t*>(*p);   // warm (the TLB, the caches that fit)
    return Cycles(loads / 16, 16.0, [&](uint32_t n) {
        asm volatile("1:\n" TE_X16("ldr %[p], [%[p]]\n") "subs %[n], %[n], #1\n bne 1b\n" : [n] "+r"(n), [p] "+r"(p) : : "cc", "memory");
    });
}
// streaming: read / write `bytes` with 32-byte NEON loads / stores, `laps` times; bytes a second (MB/s)
void ReadLap(const uint8_t* p, uint32_t bytes) {
    uint32_t n = bytes / 128;
    asm volatile("1:\n" TE_X4("vld1.64 {d0-d3}, [%[p]:128]!\n") "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n), [p] "+r"(p) : : "cc", "memory", "d0", "d1", "d2", "d3");
}
void WriteLap(uint8_t* p, uint32_t bytes) {
    uint32_t n = bytes / 128;
    asm volatile("vmov.i32 q0, #0\n vmov.i32 q1, #0\n 1:\n" TE_X4("vst1.64 {d0-d3}, [%[p]:128]!\n") "subs %[n], %[n], #1\n bne 1b\n"
                 : [n] "+r"(n), [p] "+r"(p) : : "cc", "memory", "d0", "d1", "d2", "d3");
}
template <typename F>
double MBs(uint32_t bytes, int laps, F&& lap) {
    double best = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        const int64_t t0 = Us();
        for (int l = 0; l < laps; ++l) lap();
        best = std::min(best, double(Us() - t0));
    }
    return double(bytes) * laps / best;   // bytes a microsecond = MB/s
}

// a thread on one core (the user cores 0-2 of the application)
template <typename F>
void OnCore(int core, F&& fn) {
    std::thread t([&] {
        sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0 << core);
        sceKernelDelayThread(100);
        fn();
    });
    t.join();
}

// ---- the calls -----------------------------------------------------------------------------------------------------
__attribute__((noinline)) uint32_t Callee(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a + b + c + d; }
uint32_t (*volatile g_callee)(uint32_t, uint32_t, uint32_t, uint32_t) = Callee;
thread_local uint32_t t_local = 1;
std::atomic<uint32_t> g_atomic{0};

}  // namespace

void MachineBench() {
    const char* v = std::getenv("TE_MACHINE_BENCH");
    if (!(v && *v == '1')) return;
    const int64_t start = Us();
    if (const int mhz = scePowerGetArmClockFrequency(); mhz > 0) g_mhz = double(mhz);
    char core[900];
    // ~1-2 ms a case at 444 MHz (the loops are 16 or 4 operations an iteration)
    const double int_ind = Cycles(20000, 16.0, IntIndependent), int_dep = Cycles(40000, 16.0, IntDependent);
    const double vadd_d = Cycles(10000, 16.0, VaddDep), vmul_d = Cycles(10000, 16.0, VmulDep), vmla_d = Cycles(6000, 16.0, VmlaDep);
    const double vadd_i = Cycles(10000, 32.0, VaddInd), vmla_i = Cycles(6000, 32.0, VmlaInd);
    const double dadd_d = Cycles(10000, 16.0, DaddDep), dmul_d = Cycles(8000, 16.0, DmulDep);
    const double vdiv_d = Cycles(10000, 4.0, VdivDep), vsqrt_d = Cycles(10000, 4.0, VsqrtDep);
    const double qadd_d = Cycles(10000, 16.0, QaddDep), qmla_d = Cycles(6000, 16.0, QmlaDep), qmla_i = Cycles(6000, 32.0, QmlaInd);
    const double vmrs = Cycles(20000, 4.0, VcmpVmrs), corecmp = Cycles(40000, 4.0, CoreCmp), trip = Cycles(10000, 4.0, VfpCoreTrip);
    std::snprintf(core, sizeof core,
                  "[te.machine] core at %.0f MHz (cycles; TE_MACHINE_BENCH): integer adds 4 independent chains %.2f a cycle (the issue "
                  "limit), dependent %.2f cycles; VFP f32 dependent vadd %.1f vmul %.1f vmla %.1f, 8 independent vadd %.2f vmla %.2f a cycle; "
                  "f64 dependent vadd %.1f vmul %.1f; vdiv.f32 %.1f vsqrt.f32 %.1f; NEON q dependent vadd %.1f vmla %.1f, 8 independent vmla.q "
                  "%.2f a cycle (= %.1f flops); VCMP + VMRS + branch %.1f (the same compare in the core %.1f); VFP -> core -> VFP %.1f",
                  g_mhz, 1.0 / int_ind, int_dep, vadd_d, vmul_d, vmla_d, 1.0 / vadd_i, 1.0 / vmla_i, dadd_d, dmul_d, vdiv_d, vsqrt_d, qadd_d,
                  qmla_d, 1.0 / qmla_i, 8.0 / qmla_i, vmrs, corecmp, trip);
    std::fprintf(stderr, "%s\n", core);
    // the memory: one buffer of 4 MB (released after)
    constexpr uint32_t kBig = 4u << 20;
    uint8_t* raw = static_cast<uint8_t*>(std::malloc(kBig + 2 * 128 + (1u << 20)));
    if (raw) {
        uint8_t* buf = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(raw) + 127) & ~uintptr_t(127));
        std::string mem = "[te.machine] memory (cycles a load by working set, a random chase of 32-byte lines):";
        static const uint32_t kSets[] = {8u << 10, 16u << 10, 28u << 10, 48u << 10, 96u << 10, 192u << 10, 320u << 10, 448u << 10,
                                         640u << 10, 1u << 20, 2u << 20, 4u << 20};
        OnCore(0, [&] {
            for (uint32_t set : kSets) {
                // ~1.5 ms a size: ~150 000 loads in L1, ~6000 from DRAM (the count shrinks with the set)
                const uint32_t loads = set <= (32u << 10) ? 160000u : set <= (256u << 10) ? 32000u : 8000u;
                const double c = ChaseCycles(reinterpret_cast<uint32_t*>(buf), set, loads);
                char b[48];
                std::snprintf(b, sizeof b, " %u%s %.1f", set >= (1u << 20) ? set >> 20 : set >> 10, set >= (1u << 20) ? "M" : "K", c);
                mem += b;
            }
            const double rd_l2 = MBs(128u << 10, 24, [&] { ReadLap(buf, 128u << 10); });
            const double rd = MBs(kBig, 2, [&] { ReadLap(buf, kBig); });
            const double wr = MBs(kBig, 2, [&] { WriteLap(buf, kBig); });
            const double cp = MBs(kBig / 2, 2, [&] { std::memcpy(buf + kBig / 2, buf, kBig / 2); });
            char b[260];
            std::snprintf(b, sizeof b, "; streaming MB/s (32-byte NEON loads / stores): read 128 KB %.0f, read 4 MB %.0f, write 4 MB %.0f, memcpy 2 MB %.0f",
                          rd_l2, rd, wr, cp);
            mem += b;
        });
        // two cores reading at once (2 MB each, their own halves): the shared bus
        {
            std::atomic<int> ready{0};
            double each[2] = {0, 0};
            auto reader = [&](int k) {
                sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0 << k);
                ready.fetch_add(1);
                while (ready.load() < 2) {}
                each[k] = MBs(kBig / 2, 3, [&] { ReadLap(buf + k * (kBig / 2), kBig / 2); });
            };
            std::thread t0(reader, 0), t1(reader, 1);
            t0.join();
            t1.join();
            char b[120];
            std::snprintf(b, sizeof b, "; two cores reading at once %.0f + %.0f MB/s", each[0], each[1]);
            mem += b;
        }
        std::free(raw);
        std::fprintf(stderr, "%s\n", mem.c_str());
    }
    // the calls
    {
        volatile uint32_t sink = 0;
        const double call = Cycles(20000, 1.0, [&](uint32_t n) { for (uint32_t i = 0; i < n; ++i) sink = sink + g_callee(i, 1, 2, 3); });
        volatile uint32_t divisor = 7;
        const double div = Cycles(20000, 1.0, [&](uint32_t n) { for (uint32_t i = 0; i < n; ++i) sink = sink + (i + 1000u) / divisor; });
        const double tls = Cycles(20000, 1.0, [&](uint32_t n) {
            for (uint32_t i = 0; i < n; ++i) {
                asm volatile("" ::: "memory");
                sink = sink + t_local;
            }
        });
        const double atom = Cycles(20000, 1.0, [&](uint32_t n) { for (uint32_t i = 0; i < n; ++i) g_atomic.fetch_add(1); });
        const double clock = Cycles(4000, 1.0, [&](uint32_t n) { for (uint32_t i = 0; i < n; ++i) sink = sink + uint32_t(Us()); });
        const double loop = Cycles(40000, 1.0, [&](uint32_t n) { for (uint32_t i = 0; i < n; ++i) sink = sink + i; });
        std::fprintf(stderr,
                     "[te.machine] calls (cycles, the loop's own %.1f included): an indirect call with 4 arguments %.1f, the EABI's unsigned "
                     "division %.1f, a thread_local read %.1f, a sequentially consistent atomic increment %.1f, the kernel clock %.1f; "
                     "the bench took %.1f ms\n",
                     loop, call, div, tls, atom, clock, double(Us() - start) / 1000.0);
    }
}
#else
void MachineBench() {}
#endif

}  // namespace te::prof
