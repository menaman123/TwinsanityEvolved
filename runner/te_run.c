// te_run.c -- the ONE place that enters the wasm2c module, in C on purpose.
//
// wabt's wasm-rt.h defines WASM_RT_THREAD_LOCAL as `_Thread_local` for C11 but EMPTY for C++ on Apple ("sporadic test
// failures"), so a C++ translation unit and the C runtime (wasm-rt-impl.c) see g_wasm_rt_jmp_buf as two different
// kinds of variable and `wasm_rt_impl_try()` from C++ sets up a jump buffer `wasm_rt_trap` never longjmps to (SIGBUS
// in wasm_rt_trap). Every access to the runtime's thread-local state therefore lives in this C file; the C++ runner
// only calls te_run_entry, and the save state (te_savestate.cpp) reads/writes that state through te_run_tls_*.
#include "te_run.h"

#include <string.h>

#if defined(TE_NATIVE)
/* The Vita's native build (te_native.h): the game's entry is a native function -- no module instance, no runtime state,
   no traps (a fault is the device's, as with the unchecked wasm2c module). */
void entry(void);

int te_run_entry(struct w2c_game* instance, int test_trap) {
    (void)instance;
    if (test_trap) return 1;   /* --test-trap: there is no wasm trap to unwind on this lane */
    entry();
    return -1;
}

const char* te_trap_name(int trap) {
    (void)trap;
    return "(no traps: the native build)";
}

size_t te_run_tls_size(void) { return 0; }
void te_run_tls_save(void* out) { (void)out; }
void te_run_tls_restore(const void* in) { (void)in; }
uint32_t te_run_call_depth(void) { return 0; }
#else

#include "game.h"
#include "wasm-rt.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"

#if defined(_WIN32)
// on the Windows lane the runtime state below MUST be plain globals (the save state carries it; a TLS block's
// address differs between the saving and the loading process). CMakeLists.txt compiles the module and this file against
// a copy of wasm-rt.h with WASM_RT_THREAD_LOCAL defined EMPTY; this check fails the build if the storage class is back
// (`_Thread_local1` / `__declspec1` is not an identifier).
#define TE_CAT_(a, b) a##b
#define TE_CAT(a, b) TE_CAT_(a, b)
enum { te_wasm_rt_state_is_plain = TE_CAT(WASM_RT_THREAD_LOCAL, 1) };
#endif

int te_run_entry(struct w2c_game* instance, int test_trap) {
    wasm_rt_trap_t trap = (wasm_rt_trap_t)wasm_rt_impl_try();
    if (trap != 0) {
        return (int)trap;
    }
    if (test_trap) {
        wasm_rt_trap(WASM_RT_TRAP_OOB);   // self-test: must come back through the branch above
    }
    w2c_game_entry(instance);
    return -1;   // entry returned (it should end in Platform::System::Exit -> te::RequestExit)
}

const char* te_trap_name(int trap) {
    return wasm_rt_strerror((wasm_rt_trap_t)trap);
}

// The runtime's per-thread state a save state at the frame boundary must carry: the trap jump buffer (its registers
// point into the game fiber's te_run_entry frame, which the state carries at the same address) and the call-depth
// counters (WASM_RT_STACK_DEPTH_COUNT). Opaque bytes to the caller.
size_t te_run_tls_size(void) {
    return sizeof(g_wasm_rt_jmp_buf) + sizeof(uint32_t) * 2;
}

void te_run_tls_save(void* out) {
    unsigned char* p = (unsigned char*)out;
    memcpy(p, &g_wasm_rt_jmp_buf, sizeof(g_wasm_rt_jmp_buf));
    p += sizeof(g_wasm_rt_jmp_buf);
#if WASM_RT_STACK_DEPTH_COUNT
    memcpy(p, &wasm_rt_call_stack_depth, sizeof(uint32_t));
    p += sizeof(uint32_t);
    memcpy(p, &wasm_rt_saved_call_stack_depth, sizeof(uint32_t));
#else
    memset(p, 0, sizeof(uint32_t) * 2);   // no counters (the Vita: the stack's depth unchecked)
#endif
}

void te_run_tls_restore(const void* in) {
    const unsigned char* p = (const unsigned char*)in;
    memcpy(&g_wasm_rt_jmp_buf, p, sizeof(g_wasm_rt_jmp_buf));
    p += sizeof(g_wasm_rt_jmp_buf);
#if WASM_RT_STACK_DEPTH_COUNT
    memcpy(&wasm_rt_call_stack_depth, p, sizeof(uint32_t));
    p += sizeof(uint32_t);
    memcpy(&wasm_rt_saved_call_stack_depth, p, sizeof(uint32_t));
#endif
    wasm_rt_set_unwind_target(&g_wasm_rt_jmp_buf);   // the unwind target is a pointer into THIS process's TLS
}

uint32_t te_run_call_depth(void) {
#if WASM_RT_STACK_DEPTH_COUNT
    return wasm_rt_call_stack_depth;
#else
    return 0;
#endif
}

#if defined(TE_W2C_COUNT)
// The module's operation counters of a diagnosis build (CMake TE_VITA_W2C_COUNT; the generated header's hooks,
// tools/w2c_unchecked.py): [0] function entries, [1] loads + stores, [2] their f32 part, [3] abs, [4] float->int,
// [5] call_indirect, [6] sqrt. main.cpp reports them per boundary.
uint32_t te_w2c_counts[8];
void te_w2c_count_read(uint32_t out[8]) { memcpy(out, te_w2c_counts, sizeof te_w2c_counts); }
#endif
#endif   /* !TE_NATIVE */
