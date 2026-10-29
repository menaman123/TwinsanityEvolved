// platform/nativeee (host side, the Vita) -- what the drawing thread waits for, measured on the device.
//
// The device's run4: the game thread waits 88 ms a frame for the drawing thread at the title (26 draws). Where those
// milliseconds go is not visible in the emulator (Vita3K's GXM is not the GPU; its CPU is 10-20x the Cortex-A9's), so
// the Vita build counts it itself:
//   - every GXM call vitaGL can block in, timed (the link wraps them: CMakeLists.txt TE_VITA, -Wl,--wrap=<name>):
//     sceGxmBeginScene (a render target whose previous scene the GPU has not finished), sceGxmEndScene,
//     sceGxmDisplayQueueAddEntry (vitaGL's display queue full: 2 frames pending = the GPU or the vertical blank is the
//     limit), sceGxmFinish / sceGxmNotificationWait (a full drain: glFinish, glReadPixels, vitaGL's out-of-memory path),
//     the shader patcher's program creations, sceGxmDraw;
//   - vitaGL's pool allocations by pool (vgl_memalign: RAM, PHYCONT, BUDGET, VRAM, and the newlib-heap fallback --
//     CACHED memory the GPU may read stale), the failed attempts, the frees, glFinish calls, and the 1-second sleeps of
//     its unsafe allocator (gpu_utils.c gpu_alloc_mapped_aligned_unsafe_*: sceKernelDelayThread(1000000) per retry);
//   - the GPU's lag at each present: scenes ended (vitaGL's query_fence.value, incremented at every sceGxmEndScene)
//     minus scenes the GPU has finished (the fragment notification vitaGL passes to every sceGxmEndScene);
//   - a probe every TE_GFX_VITA_GPU_PROBE (or TE_VITA_GPU_PROBE_EVERY) presents -- OFF by default: each
//     probe drains the GPU twice (38 ms each in the device's island pan): the GPU drained after a present (sceGxmFinish),
//     then after the next present's swap the time until the GPU has finished that one frame -- the GPU's own time for a
//     frame beyond what the CPU's submission overlapped;
//   - without a drain: the GPU's BUSY share, sampled by the profiler's thread every TE_PROF_US (SampleGpuBusy:
//     scenes ended but not finished = the GPU has work), as milliseconds of GPU work per present in the window line.
// Off the Vita these are no-ops.
#pragma once

#include <cstdint>
#include <string>

namespace te::vita_gpu {

// The drawing thread, right after a present's swap returned (graphics.cpp RenderFrame / RenderBetween)
void OnPresented();
// The presents whose GPU work is not finished yet: each present's last scene (vitaGL's query_fence value after its swap)
// against the scenes the GPU has finished (the fence's notification). -1 when it cannot be known (no fence). The drawing
// thread's low-latency hand-over (graphics.cpp) takes the next picture only once this is down to TE_VITA_GPU_AHEAD.
int FramesInFlight();
// vitaGL's scene fence (query_fence), readable from any thread: `issued` = the scenes ended so far (sceGxmEndScene
// calls), `done` = the scenes the GPU has finished (the fence's notification). Storage sampled only by scenes ended up to
// a moment is free for the CPU to write once `done` has reached the `issued` read at that moment (int32_t(done - mark)
// >= 0) -- the movie pictures' slots (movie_player.cpp). false when the fence is not known (no such vitaGL symbol).
bool SceneFence(uint32_t* issued, uint32_t* done);
// The pace window's lines (graphics.cpp NotePace, the game thread): per frame over `frames` presents; resets the window
std::string WindowLines(double frames);
// One sample of the GPU's state (the profiler's sampler thread, TE_PROF=1): busy when scenes ended are not all finished
void SampleGpuBusy();
// vitaGL's pool allocations (vgl_memalign: every pool tried counts) and frees since the start, from any thread (zero off the
// Vita): the load census reads them at every tick's end
void AllocsSoFar(uint64_t* allocs, uint64_t* bytes, uint64_t* frees, uint64_t* us = nullptr);

}  // namespace te::vita_gpu
