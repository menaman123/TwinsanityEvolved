// render/backends/gl/gl_gpu_lifetime.h -- the GPU-lifetime audit (TE_GFX_GPU_LIFETIME=1; off by default).
//
// Every GPU-visible storage the backend frees, re-specifies or writes from the CPU is checked, at that moment, against
// what the GPU may still be reading:
//   - the Vita: the storage's last use is vitaGL's own record of it (a texture record's last_frame, a buffer record's:
//     every vitaGL draw and every raw GXM draw of the backend stamps them), the GPU's progress the scene fence vitaGL
//     passes to every sceGxmEndScene (query_fence: value = the scenes ended, *address = the scenes the GPU finished),
//     marked at every swap. A storage last used in a frame the GPU has not finished is IN FLIGHT; vitaGL then defers the
//     free to its garbage collector when its record says the use is at most FRAME_PURGE_FREQ (4) frames old (the
//     protection), else frees it at once (UNPROTECTED: logged). The uniform ring (2 MB, vitaGL's only ring without a
//     fence) is checked by the platform's wrapper of its reservation (UniformsReserved), the vertex ring per slice, the
//     garbage collector's own frees per swap.
//   - elsewhere: the GL driver keeps every storage the GPU may still read; the same events are counted, with the presents
//     the GPU had not finished at each (per-present fences), so a deterministic run gives the same event counts.
// Off, every hook is one test of a global flag; nothing is recorded, read or allocated.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ps2r {
namespace render {
namespace gpu_lifetime {

enum class Event : uint8_t { Free, Respecify, CpuWrite };

extern bool g_on;
inline bool On() { return g_on; }

// GlDevice::Init: TE_GFX_GPU_LIFETIME read once (its start-up line)
void Init();
// GlDevice::Present, right after the swap returned: this frame's scenes marked (the Vita), a fence (elsewhere)
void OnSwap();
// a texture's storage (GL name `tex`, `w` x `h`, `bytes`) about to be freed / re-specified / written by the CPU through
// `what` (the path, a string literal). `copy_on_write`: vitaGL copies an in-flight storage before the write (its
// glTexSubImage2D / glBufferSubData) -- a CPU write the GPU never sees. `at_once`: the path frees the old storage
// itself whatever vitaGL's record says (gpu_alloc_mipmaps' realloc)
void Texture(uint32_t tex, uint32_t w, uint32_t h, Event e, const char* what, bool copy_on_write = false, bool at_once = false);
// a buffer's storage (GL name `buf`) likewise
void Buffer(uint32_t buf, uint64_t bytes, Event e, const char* what, bool copy_on_write = false);
// the vertex ring: vitaGL frame `frame` starts writing slice `slice`, last written by frame `prev` (0xFFFFFFFF: never)
void RingSlice(uint32_t frame, uint32_t prev, int slice, uint64_t slice_bytes);
// the uniform ring (the platform's wrapper of vglReserveUniformCircularPoolBuffer): `size` bytes reserved at `p`
void UniformsReserved(const void* p, uint32_t size);
// THE SHADER PROGRAMS (the platform's wrappers of the shader patcher and of the program sets): a fragment / vertex
// program's USSE code is GPU-visible memory too -- the patcher hands a released program's code space to the next one
// patched. `r`: the creation's result (< 0: refused, e.g. the patcher's fixed code heap full)
void ProgramCreated(const void* program, const void* id, bool fragment, int r);
void ProgramUsed(const void* program, bool fragment);      // a set on vitaGL's context (a draw follows)
void ProgramReleased(const void* program, bool fragment);  // one reference released (the code freed at the last)
void ProgramIdUnregistered(const void* id, bool force);    // a registered GXP unregistered (force: its programs freed)
// the window's line (graphics.cpp's census; empty when off) -- its counters then start again
std::string WindowLine();

}  // namespace gpu_lifetime
}  // namespace render
}  // namespace ps2r
