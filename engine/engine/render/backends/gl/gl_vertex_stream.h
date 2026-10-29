// render/backends/gl/gl_vertex_stream.h -- GlVertexStream: the vertex array object, the
// 16 MiB streaming vertex buffer ring, and the run brackets.
//
// Per draw the converted vertices are bump-allocated (32-byte aligned) inside one
// buffer through glMapBufferRange(WRITE | UNSYNCHRONIZED | FLUSH_EXPLICIT) and the
// draw addresses them as a first-vertex index against the attribute layout
// configured once at offset 0 with stride 32. On wrap the fence of the previous wrap
// is waited on (bounding in-flight generations to one buffer), the storage is
// orphaned and a new fence issued. A run of consecutive draws keeps the VAO and
// program bound; EndDrawRun unbinds once at the end of the run (VAO 0, program
// 0) so any non-draw GL work starts from the same state.
//
// The Vita (vitaGL) has no fences, and an orphan there is a NEW 16 MiB allocation from its GPU pools while the old
// storage waits for vitaGL's garbage collector (buffers.c glNamedBufferData: mark_as_dirty, freed FRAME_PURGE_FREQ
// swaps later) -- per wrap, 32 MiB of pool held at once, the second copy spilling into whichever pool has room. There
// the storage is allocated ONCE and split into kVitaSlices frame slices: the vertices of vitaGL frame F (the frames
// vglSwapBuffers counts, vglGetFrameNumber) go into slice F % kVitaSlices, so a slice is written again only
// kVitaSlices swaps later. vitaGL's display queue holds at most 2 pending frames (displayQueueMaxPendingCount =
// buffers - 1), so when swap F+3 has returned the GPU has finished frame F+1 and everything before it: the slice frame F
// used is idle when frame F+4 rewrites it -- the same bound vitaGL's own garbage collector relies on (FRAME_PURGE_FREQ
// 4). A frame that outgrows its slice re-specifies the storage (the old way, counted and logged) and continues in the
// new storage. TE_VITA_RING_ORPHAN=1: the old ring (wrap + orphan) for an A/B.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/platform/host/platform.h"   // PS2R_HOST_VITA (the class layout below depends on it)
#include "engine/render/backends/gl/gpu_vertex.h"

namespace ps2r {
namespace render {

class GlStateCache;

class GlVertexStream {
public:
    static constexpr size_t kRingSize = 16u * 1024u * 1024u;
    static constexpr int kVitaSlices = 4;
    static constexpr size_t kVitaSliceSize = kRingSize / kVitaSlices;

    bool Init(GlStateCache& st);
    void Shutdown(GlStateCache& st);

    // Binds the VAO + array buffer for a draw of the PS2 program.
    void BindForDraw(GlStateCache& st);
    // The same ring read as RawVertex (position, S / T, colour, Q on attribute 6; no fog): a raw draw's arrays
    void BindRawForDraw(GlStateCache& st);
    // Uploads `bytes` of `stride`-byte vertices; returns the first-vertex index in that stride
    int UploadRaw(const void* verts, size_t bytes, size_t stride);
    // Uploads n vertices into the ring; returns the first-vertex index for glDrawArrays.
    // GL_ARRAY_BUFFER must be bound to the ring (BindForDraw).
    int Upload(const GpuVertex* verts, size_t n);
#if PS2R_HOST_VITA
    // THE RING WRITTEN IN PLACE (the Vita): `bytes` of `stride`-byte vertices reserved in this vitaGL frame's slice
    // exactly where UploadRaw would put them (its offset rule, its census), WITHOUT a GL call -- the caller writes the
    // vertices through the returned pointer (the ring's storage itself: vitaGL's mapping of a buffer is its storage) and
    // draws them from vertex `*first`. Null, nothing reserved, where UploadRaw would re-specify the storage (its first
    // allocation, a frame past its slice, a draw larger than a slice, the A/B's orphan ring): the caller uploads then.
    void* ReserveRaw(size_t bytes, size_t stride, int* first);
    // The copy into a reservation (UploadRaw's: one in 32 timed for the census's memory line)
    static void CopyIn(void* dst, const void* src, size_t bytes);
    // whether `bytes` at `p` lie in the ring's current storage on a `stride` boundary (vertices a caller wrote in
    // place: ReserveRaw) -- then `*first` is their first vertex in that stride
    bool InRing(const void* p, size_t bytes, size_t stride, int* first) const;
    // the census: bytes copied into the ring from a producer's storage (CopyIn / UploadRaw), bytes of vertices
    // drawn where a caller wrote them (InRing), since the start
    static uint64_t CopiedBytes();
    static uint64_t InPlaceBytes();
    static void NoteInPlace(size_t bytes);
    static void NoteCopied(size_t bytes);
    // the ring's GL name (vitaGL's: its buffer record -- the storage's address at 0, its size at 4, its last use at 12)
    uint32_t Buffer() const { return vbo_; }
#endif

    void BeginDrawRun();
    void EndDrawRun(GlStateCache& st);
    bool InDrawRun() const { return in_run_; }
    uint64_t Generation() const { return generation_; }

private:
    uint32_t vao_ = 0;
    uint32_t raw_vao_ = 0;
    uint32_t vbo_ = 0;
    size_t offset_ = 0;
    bool ready_ = false;
    void* fence_ = nullptr;      // GLsync of the previous wrap
    uint64_t generation_ = 1;    // bumped on every re-specification
    bool in_run_ = false;
#if PS2R_HOST_VITA
    bool vita_orphan_ring_ = false;        // TE_VITA_RING_ORPHAN=1: the desktop ring (wrap + orphan)
    uint32_t slice_frame_ = 0xFFFFFFFFu;   // the vitaGL frame the current slice belongs to
    size_t slice_end_ = 0;
    uint64_t frame_bytes_ = 0;             // bytes this vitaGL frame uploaded (the census' per-frame peak)
    uint64_t overflows_ = 0;
#endif
};

}  // namespace render
}  // namespace ps2r
