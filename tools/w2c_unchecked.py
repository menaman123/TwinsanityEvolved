#!/usr/bin/env python3
"""w2c_unchecked.py -- let a build compile the module's loads and stores without their bounds checks (the Vita lane).

wasm2c's runtime refuses guard-page memory on a 32-bit target, so a 32-bit build checks every load and store against
the memory's size: a 64-bit add and compare with the size read from memory, on the hottest path the game has. This
rewrites the ONE macro the default-page memory accesses go through in the generated `game-impl.h`, so that a build
defining TE_W2C_UNCHECKED_MEMORY skips the check (the bulk copies and fills keep their range checks). A build that does
not define it compiles exactly what wasm2c wrote.

The same build also addresses the loads and stores through the function's own copy of the memory's base
(`wasm_rt_local_memory_base`, which every generated function already reads at its entry): without mmap wasm2c's
fallback reads `instance->memory->data` again at every access, and as a byte store may alias any object the compiler
reloads both pointers after every store and call -- two dependent loads in front of most of the game's memory
accesses. The memory here is the engine's fixed 32 MB (imported, never grown: the module has no memory.grow), so its
base cannot change under a running function.

Three more rewrites, each behind its own switch (CMake TE_VITA_W2C_LEAN defines all three on the Vita lane):
- TE_W2C_VFP_ABS: wasm's f32.abs / f64.abs as the plain VABS. wasm2c's wasm_fabsf tests for a NaN first (a VCMP and a
  VMRS -- a wait for the VFP on the Cortex-A9 -- plus an IT block of three conditional moves) because a C fabs may
  quiet a NaN on some hosts; ARM's VABS is FPAbs, the sign bit cleared and nothing else, in every FPSCR mode (no NaN
  processing, no flush-to-zero: ARMv7-A ARM, A8.8.280 and the FPAbs pseudocode), which IS wasm's abs for all 2^32 inputs.
- TE_W2C_UNCHECKED_TRUNC: i32.trunc_f32_s / _u as the bare VCVT, whose result for every input that does not trap
  (|x| < 2^31 for _s, -1 < x < 2^32 for _u, not NaN) is the trapping form's. As the unchecked memory: a run on which the
  checked form never traps computes the same values; an input the checked form traps on (the game would stop with
  "wasm trap: integer overflow") saturates instead.
- TE_W2C_FIXED_TABLE: the module's one funcref table never changes size (allocated with initial == max at
  instantiation, no table.grow anywhere in the module), so call_indirect's bounds test compares the index with that
  constant instead of loading `table.size` from the instance at every call (reloaded after every call and store).

Idempotent; fails loudly if the header no longer has the expected shape (a wabt upgrade).

    w2c_unchecked.py build/w2c/game-impl.h [build/w2c/game_*.c]   (the parts: the table's allocation, no table.grow)
"""
import re
import sys
from pathlib import Path

MARK = "// te: the per-access bounds check can be compiled out (tools/w2c_unchecked.py)"
OLD = """#if WASM_RT_MEMCHECK_GUARD_PAGES
#define MEMCHECK_DEFAULT32(mem, local_memory_size, a, t) \\
  WASM_RT_CHECK_BASE(mem);
#else"""
NEW = """%s
#if WASM_RT_MEMCHECK_GUARD_PAGES || defined(TE_W2C_UNCHECKED_MEMORY)
#define MEMCHECK_DEFAULT32(mem, local_memory_size, a, t) \\
  WASM_RT_CHECK_BASE(mem);                               \\
  TE_W2C_COUNT_ACCESS(t)
#else""" % MARK

# A diagnosis build (CMake TE_VITA_W2C_COUNT, never shipped) counts what the module executes, for the dynamic side of the
# static census: the hooks below are empty unless TE_W2C_COUNT is defined
COUNT_MARK = "// te: the module's operations counted in a diagnosis build (TE_W2C_COUNT; tools/w2c_unchecked.py)"
COUNT_OLD = """#include "game.h"
#define IS_SINGLE_UNSHARED_DEFAULT32_MEMORY 1"""
COUNT_NEW = COUNT_OLD + """
%s
// [0] function entries, [1] loads + stores, [2] their f32 part, [3] abs, [4] float->int, [5] call_indirect, [6] sqrt --
// runner/te_run.c holds the counters, runner/main.cpp reports them ("[te.w2c.count]"). Counted where the unchecked and
// TE_VITA_W2C_LEAN forms are (the Vita's shipped shape).
#if defined(TE_W2C_COUNT)
extern u32 te_w2c_counts[8];
#define TE_W2C_COUNT_ACCESS(t) te_w2c_counts[1]++; te_w2c_counts[2] += __builtin_types_compatible_p(t, f32);
#define TE_W2C_COUNT_ONE(i) (te_w2c_counts[i]++),
#else
#define TE_W2C_COUNT_ACCESS(t)
#define TE_W2C_COUNT_ONE(i)
#endif""" % COUNT_MARK
COUNT_TAIL_MARK = "// te: the counted function entries and sqrt (TE_W2C_COUNT; tools/w2c_unchecked.py)"
COUNT_TAIL = """
%s
#if defined(TE_W2C_COUNT)
#undef FUNC_PROLOGUE
#define FUNC_PROLOGUE te_w2c_counts[0]++;
static inline float te_count_sqrtf(float x) { te_w2c_counts[6]++; return wasm_sqrtf(x); }
#define wasm_sqrtf(x) te_count_sqrtf(x)
#endif
""" % COUNT_TAIL_MARK

BASE_MARK = "// te: the unchecked build addresses memory through the function's local base (tools/w2c_unchecked.py)"
BASE_OLD = """#elif WASM_RT_USE_LOCAL_MEMORY_BASE_SIZE_FOR_THIS_MODULE
#define MEM_ADDR_MEMOP(mem, addr, n) (&wasm_rt_local_memory_base[(addr)])
#else"""
BASE_NEW = """%s
#elif WASM_RT_USE_LOCAL_MEMORY_BASE_SIZE_FOR_THIS_MODULE || defined(TE_W2C_UNCHECKED_MEMORY)
#define MEM_ADDR_MEMOP(mem, addr, n) (&wasm_rt_local_memory_base[(addr)])
#else""" % BASE_MARK


TRUNC_MARK = "// te: float -> i32 without the three VFP compares on the common inputs (tools/w2c_unchecked.py)"
TRUNC_OLD = """#define I32_TRUNC_U_F32(x) TRUNC_U(u32, f32, 4294967296.f, x)"""
TRUNC_NEW = """#define I32_TRUNC_U_F32(x) TRUNC_U(u32, f32, 4294967296.f, x)
%s
// An unchecked build (the Vita): wasm's trapping i32.trunc_f32_s / _u test NaN and both ends of the range with three VFP
// compares, each a VMRS that waits for the FPU on the Cortex-A9, before the VCVT. The range is tested on the float's bits
// instead (|x| < 2^31: (bits & 0x7fffffff) < 0x4f000000, which no NaN or infinity passes; [+0, 2^32): bits < 0x4f800000)
// and those inputs go to the same VCVT; every other input takes the checked form, traps included. Same result for every
// one of the 2^32 inputs (cpuwork/gamecpu/trunc/exhaustive.c).
#if defined(TE_W2C_UNCHECKED_MEMORY)
static inline u32 te_f32_bits(f32 x) {
  u32 b;
  memcpy(&b, &x, sizeof b);
  return b;
}
static inline u32 te_i32_trunc_s_f32(f32 x) {
  if (LIKELY((te_f32_bits(x) & 0x7fffffffu) < 0x4f000000u)) return (u32)(s32)x;
  return TRUNC_S(u32, s32, f32, (f32)INT32_MIN, >=, 2147483648.f, x);
}
static inline u32 te_i32_trunc_u_f32(f32 x) {
  if (LIKELY(te_f32_bits(x) < 0x4f800000u)) return (u32)x;
  return TRUNC_U(u32, f32, 4294967296.f, x);
}
#undef I32_TRUNC_S_F32
#define I32_TRUNC_S_F32(x) te_i32_trunc_s_f32(x)
#undef I32_TRUNC_U_F32
#define I32_TRUNC_U_F32(x) te_i32_trunc_u_f32(x)
#endif""" % TRUNC_MARK

ABS_MARK = "// te: f32.abs / f64.abs as the plain VABS (tools/w2c_unchecked.py)"
ABS_OLD = """static double wasm_fabs(double x) {
  if (UNLIKELY(isnan(x))) {
    uint64_t tmp;
    wasm_rt_memcpy(&tmp, &x, 8);
    tmp = tmp & ~(1ULL << 63);
    wasm_rt_memcpy(&x, &tmp, 8);
    return x;
  }
  return fabs(x);
}"""
ABS_NEW = ABS_OLD + """
%s
// wasm's abs clears the sign bit of every input, NaNs included. wasm_fabsf / wasm_fabs above test for a NaN first (on
// the Cortex-A9 a VCMP, a VMRS that waits for the VFP pipeline and an IT block of three conditional moves around the
// VABS) because a host's C fabs may quiet a NaN. ARM's VABS is FPAbs: the sign bit cleared and nothing else -- no NaN
// processing, no flush-to-zero, no exception, under every FPSCR mode (ARMv7-A ARM A8.8.280; the game thread runs RZ |
// FZ | DN) -- so the plain VABS is wasm's abs for all 2^32 (2^64) inputs. The Vita runner checks it at start-up
// (runner/te_vita.cpp: "w2c: VABS ...").
#if defined(TE_W2C_VFP_ABS)
#if !defined(__ARM_FP)
#error "TE_W2C_VFP_ABS: VABS is ARM's (FPAbs); another target's fabs may quiet a NaN"
#endif
static inline float te_vfp_absf(float x) { return TE_W2C_COUNT_ONE(3) __builtin_fabsf(x); }
static inline double te_vfp_abs(double x) { return TE_W2C_COUNT_ONE(3) __builtin_fabs(x); }
#define wasm_fabsf(x) te_vfp_absf(x)
#define wasm_fabs(x) te_vfp_abs(x)
#endif""" % ABS_MARK

UTRUNC_MARK = "// te: i32.trunc_f32_s / _u as the bare VCVT (tools/w2c_unchecked.py)"
UTRUNC_OLD = """#define I64_TRUNC_U_F32(x) TRUNC_U(u64, f32, (f32)UINT64_MAX, x)"""
UTRUNC_NEW = """%s
// The checked forms (wasm2c's, or the bits-tested ones above) trap on a NaN or an out-of-range input and otherwise
// return the VCVT's result (C's conversion: toward zero). The unchecked build returns the VCVT's result for every input:
// identical wherever the checked form does not trap; where it would trap (the game would stop) the VCVT saturates
// (NaN -> 0), as the R5900's CVT.W.S does. Saves the bits' transfer from the VFP, the test and its branch.
#if defined(TE_W2C_UNCHECKED_TRUNC)
static inline u32 te_i32_trunc_s_f32_vcvt(f32 x) { return TE_W2C_COUNT_ONE(4) (u32)(s32)x; }
static inline u32 te_i32_trunc_u_f32_vcvt(f32 x) { return TE_W2C_COUNT_ONE(4) (u32)x; }
#undef I32_TRUNC_S_F32
#define I32_TRUNC_S_F32(x) te_i32_trunc_s_f32_vcvt(x)
#undef I32_TRUNC_U_F32
#define I32_TRUNC_U_F32(x) te_i32_trunc_u_f32_vcvt(x)
#endif
#define I64_TRUNC_U_F32(x) TRUNC_U(u64, f32, (f32)UINT64_MAX, x)""" % UTRUNC_MARK

TABLE_MARK = "// te: call_indirect against the table's fixed size (tools/w2c_unchecked.py)"
TABLE_OLD = """#define CHECK_CALL_INDIRECT(table, ft, x)                       \\
  (LIKELY((x) < table.size && ft == table.data[x].func_type) || \\
   func_types_eq_slowpath(&(table), ft, x))"""
TABLE_NEW = """%s
// The module's one table is allocated with initial == max = TE_W2C_TABLE_SIZE elements at instantiation and the module
// has no table.grow: its size is that constant for the whole run, so the test needs no load of `table.size` from the
// instance (reloaded after every call and every store, which may alias it). An index past the end still goes to the
// slow path, which tests it against the table itself.
#if defined(TE_W2C_FIXED_TABLE)
#define TE_W2C_TABLE_SIZE %du
#define CHECK_CALL_INDIRECT(table, ft, x)                                \\
  (TE_W2C_COUNT_ONE(5)(LIKELY((x) < TE_W2C_TABLE_SIZE && ft == table.data[x].func_type) || \\
   func_types_eq_slowpath(&(table), ft, x)))
#else
""" + TABLE_OLD + """
#endif"""

RE_TABLE_ALLOC = re.compile(r"wasm_rt_allocate_funcref_table\(&instance->w2c_0x5F_indirect_function_table, (\d+), (\d+)\);")


def table_size(parts):
    """The table's element count from the instantiation code, asserted fixed (initial == max) and allocated once."""
    allocs = [a for text in parts for a in RE_TABLE_ALLOC.findall(text)]
    if len(allocs) != 1 or allocs[0][0] != allocs[0][1]:
        sys.stderr.write("w2c_unchecked: the parts do not allocate the one funcref table once with initial == max (%r)\n" % allocs)
        sys.exit(1)
    return int(allocs[0][0])


def rewrite(s, mark, old, new, what):
    if mark in s:
        return s
    if s.count(old) != 1:
        sys.stderr.write("w2c_unchecked: game-impl.h does not have the expected %s shape\n" % what)
        sys.exit(1)
    return s.replace(old, new, 1)


def main():
    p = Path(sys.argv[1])
    s = p.read_text()
    t = rewrite(s, COUNT_MARK, COUNT_OLD, COUNT_NEW, "#include \"game.h\"")
    t = rewrite(t, MARK, OLD, NEW, "MEMCHECK_DEFAULT32")
    t = rewrite(t, BASE_MARK, BASE_OLD, BASE_NEW, "MEM_ADDR_MEMOP")
    t = rewrite(t, TRUNC_MARK, TRUNC_OLD, TRUNC_NEW, "I32_TRUNC_U_F32")
    t = rewrite(t, ABS_MARK, ABS_OLD, ABS_NEW, "wasm_fabs")
    t = rewrite(t, UTRUNC_MARK, UTRUNC_OLD, UTRUNC_NEW, "I64_TRUNC_U_F32")
    if len(sys.argv) > 2:
        parts = [Path(a).read_text() for a in sys.argv[2:]]
        if any("wasm_rt_grow_funcref_table" in x or "wasm_rt_grow_externref_table" in x for x in parts):
            sys.stderr.write("w2c_unchecked: the module grows a table -- TE_W2C_FIXED_TABLE does not hold\n")
            sys.exit(1)
        n = table_size(parts)
        t = rewrite(t, TABLE_MARK, TABLE_OLD, TABLE_NEW % (TABLE_MARK, n), "CHECK_CALL_INDIRECT")
    if COUNT_TAIL_MARK not in t:
        t += COUNT_TAIL
    if t != s:
        p.write_text(t)
        print("w2c_unchecked: the bounds check made optional, the local memory base used, the float->int range tests on the bits,"
              " VABS, the bare VCVT and the fixed table available in", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
