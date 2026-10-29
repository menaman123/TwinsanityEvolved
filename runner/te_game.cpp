// te_game.cpp -- see te_game.h.
#include "te_game.h"

#include <cstdio>
#include <cstdlib>

#include "engine/platform/host/platform.h"
#include "te_prof.h"
#include "te_run.h"
#if defined(TE_NATIVE)
#include <psp2/kernel/threadmgr.h>
#include "te_host.h"
#endif

namespace te::game {

namespace {
void FiberMain(void*) {
#if defined(__vita__)
    // THE VITA: this body runs on the game's own kernel thread (te_fiber.c: "te-game", created at the process's default
    // priority 160), not on the thread main() pinned -- main.cpp's PinCurrentThreadInteractive("te-game", Game) runs on
    // the MAIN thread, which only waits on its semaphore while the game runs (device-run3.psp2dmp: TWEV00001 at 96,
    // te-game at 160 = below the render thread and both builders at 128). The ranking the host layer means (the pacer
    // 72 above the game 96 above the renderer 128 and the builders 144) is applied here, on the thread that runs the game.
    if (!std::getenv("TE_NO_QOS")) ps2r::platform::host::PinCurrentThreadInteractive("te-game", ps2r::platform::host::ThreadRole::Game);
    // ... and the profiler's game slot (TE_PROF's host scopes on the game thread -- the park, the hand-over wait -- were
    // bound on the main thread by te::prof::StartFromEnv, so on this thread they counted nowhere) + the CPU census
    te::prof::BindThread(te::prof::kGame);
#if defined(TE_NATIVE)
    // the native game's stack is this kernel thread's: the host's reads of game pointers into it are valid (te_host.h)
    {
        SceKernelThreadInfo info{};
        info.size = sizeof info;
        if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) == 0 && info.stack && info.stackSize > 0) {
            te::g_guest_ranges.stack_base = uint32_t(reinterpret_cast<uintptr_t>(info.stack));
            te::g_guest_ranges.stack_span = uint32_t(info.stackSize);
        } else {
            std::fprintf(stderr, "[twinsanityevolved] native: the game thread's stack bounds are unknown (sceKernelGetThreadInfo): "
                                 "the host reads no game pointer into it\n");
        }
    }
#endif
#endif
    Fiber& f = Get();
    const int trap = te_run_entry(f.instance, f.test_trap ? 1 : 0);
    if (trap < 0) Yield(Reason::Returned, 0);
    else Yield(Reason::Trap, trap);
    // never resumed
    std::fprintf(stderr, "[twinsanityevolved] fatal: a finished game fiber was resumed\n");
    std::abort();
}
}  // namespace

Fiber& Get() {
    static Fiber fiber;
    return fiber;
}

bool Prepare(w2c_game* instance, bool test_trap, std::string* err) {
    Fiber& f = Get();
    if (!f.stack) {
        int at_hint = 0;
        f.stack = static_cast<uint8_t*>(te_fiber_map_stack(kStackHint, kStackSize, &at_hint));
        if (!f.stack) { if (err) *err = "cannot map the game stack"; return false; }
        f.stack_size = kStackSize;
        f.at_hint = at_hint != 0;
    }
    f.instance = instance;
    f.test_trap = test_trap;
    te_fiber_prepare(&f.game, f.stack, f.stack_size, FiberMain, nullptr);
    return true;
}

Reason Resume() {
    Fiber& f = Get();
    f.reason = Reason::Running;
    f.on_game = true;
    f.started = true;
    te_fiber_switch(&f.host, &f.game);
    f.on_game = false;
    return f.reason;
}

void Yield(Reason reason, int code, uint64_t boundary) {
    Fiber& f = Get();
    f.reason = reason;
    f.code = code;
    f.boundary = boundary;
    te_fiber_switch(&f.game, &f.host);
}

void ExitFromGame(int status) {
    Fiber& f = Get();
    if (!f.on_game) return;
    Yield(Reason::Exit, status);
}

}  // namespace te::game
