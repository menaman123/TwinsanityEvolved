// te_run.h -- entering the game module (see te_run.c for why this is C).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct w2c_game;
// Calls the module's `entry` inside the runtime's trap handler. Returns the wasm trap code (>0) if the module
// trapped, -1 if entry returned. Platform::System::Exit leaves through te::RequestExit instead.
int te_run_entry(struct w2c_game* instance, int test_trap);
// Called by te_run_entry once the trap handler is armed, before `entry`, when set: a state loaded before the game first
// ran holds the fiber here (te_game.cpp), so the trap handler's jump buffer is this process's when the state replaces the
// fiber's stack -- the buffer's frame is te_run_entry's, at the same address in every process.
extern void (*te_run_hold)(void);
const char* te_trap_name(int trap);
// The wasm2c runtime's thread-local state a save state carries (trap jump buffer + call-depth counters): opaque bytes.
// A restore keeps the jump buffer this process armed (the C library mixes a per-process secret into its frame, stack and
// return words: another process's buffer would unwind a trap to stray addresses); it takes the saved one only when no
// handler was armed here yet.
size_t te_run_tls_size(void);
void te_run_tls_save(void* out);
void te_run_tls_restore(const void* in);
uint32_t te_run_call_depth(void);
#if defined(TE_W2C_COUNT)
// a diagnosis build's module operation counters (te_run.c; CMake TE_VITA_W2C_COUNT)
void te_w2c_count_read(uint32_t out[8]);
#endif
#ifdef __cplusplus
}
#endif
