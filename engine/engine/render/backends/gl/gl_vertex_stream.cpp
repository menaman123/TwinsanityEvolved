// render/backends/gl/gl_vertex_stream.cpp -- GlVertexStream implementation.
#include "engine/render/backends/gl/gl_vertex_stream.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/render/backends/gl/gl_census.h"
#if PS2R_HOST_VITA
#include <psp2/kernel/processmgr.h>   // (the sampled copy timing)
#endif
#include "engine/render/backends/gl/gl_state_cache.h"
#include "engine/render/backend.h"

namespace ps2r {
namespace render {

bool GlVertexStream::Init(GlStateCache& st) {
#if PS2R_HOST_VITA
    {
        const char* e = std::getenv("TE_VITA_RING_ORPHAN");
        vita_orphan_ring_ = e && e[0] == '1';
        std::fprintf(stderr, "[gl] vertex ring: %s\n", vita_orphan_ring_
                     ? "16 MB, re-specified at every wrap (TE_VITA_RING_ORPHAN=1)"
                     : "16 MB allocated once, 4 slices of 4 MB: vitaGL frame F writes slice F % 4");
    }
#endif
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    st.BindVertexArray(vao_);
    st.BindArrayBuffer(vbo_);
    // attribute 0 position xyz @0, 1 texcoord uvq @12, 2 colour rgba8n @24, 3 fog u8n @28
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, r)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, fog)));
    st.BindVertexArray(0);
#if PS2R_HOST_VITA
    // the raw layout over the same buffer: S / T on attribute 1 (its Q read from attribute 6), no fog
    glGenVertexArrays(1, &raw_vao_);
    st.BindVertexArray(raw_vao_);
    st.BindArrayBuffer(vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RawVertex), reinterpret_cast<void*>(offsetof(RawVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(RawVertex), reinterpret_cast<void*>(offsetof(RawVertex, s)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RawVertex), reinterpret_cast<void*>(offsetof(RawVertex, r)));
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(RawVertex), reinterpret_cast<void*>(offsetof(RawVertex, q)));
    st.BindVertexArray(0);
#endif
    ready_ = false;
    offset_ = 0;
    return vao_ != 0 && vbo_ != 0;
}

void GlVertexStream::Shutdown(GlStateCache& st) {
#if !PS2R_HOST_VITA
    if (fence_) { glDeleteSync(static_cast<GLsync>(fence_)); fence_ = nullptr; }
#else
    fence_ = nullptr;
#endif
    ready_ = false;
    offset_ = 0;
    if (vbo_) { st.DeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { st.DeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (raw_vao_) { st.DeleteVertexArrays(1, &raw_vao_); raw_vao_ = 0; }
}

void GlVertexStream::BindForDraw(GlStateCache& st) {
    st.BindVertexArray(vao_);
    st.BindArrayBuffer(vbo_);
}

void GlVertexStream::BindRawForDraw(GlStateCache& st) {
    st.BindVertexArray(raw_vao_);
    st.BindArrayBuffer(vbo_);
}

int GlVertexStream::Upload(const GpuVertex* verts, size_t n) {
    return UploadRaw(verts, n * sizeof(GpuVertex), sizeof(GpuVertex));
}

int GlVertexStream::UploadRaw(const void* verts, size_t bytes, size_t stride) {
    if (bytes == 0) return 0;
    Census().ring_bytes += bytes;
#if PS2R_HOST_VITA
    NoteCopied(bytes);   // (every path below copies the producer's vertices)
    if (!vita_orphan_ring_ && bytes <= kVitaSliceSize) {
        if (!ready_) {
            // the storage, once (vitaGL: gpu_alloc_mapped_for_cpu, the RAM pool first)
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kRingSize), nullptr, GL_STREAM_DRAW);
            ready_ = true;
            slice_frame_ = 0xFFFFFFFFu;
            generation_++;
            Census().ring_respecs++;
            Census().ring_respec_bytes += kRingSize;
        }
        // this vitaGL frame's slice (see the header: a slice is rewritten kVitaSlices swaps after it was last written)
        const uint32_t frame = vglGetFrameNumber();
        if (frame != slice_frame_) {
            slice_frame_ = frame;
            offset_ = size_t(frame % uint32_t(kVitaSlices)) * kVitaSliceSize;
            slice_end_ = offset_ + kVitaSliceSize;
            frame_bytes_ = 0;
        }
        size_t off = (offset_ + stride - 1) / stride * stride;
        if (off + bytes > slice_end_) {
            // more than a slice this frame: a new storage (vitaGL keeps the old one until the GPU is past its last draw:
            // the draw before this upload set its last_frame), the frame continues from its slice's base in it
            ++overflows_;
            if (overflows_ <= 8 || overflows_ % 256 == 0)
                std::fprintf(stderr, "[gl] vertex ring: frame %u uploaded more than its %u KB slice (%llu bytes so far, "
                             "%llu more): the storage re-specified (%llu so far)\n", frame, unsigned(kVitaSliceSize >> 10),
                             (unsigned long long)frame_bytes_, (unsigned long long)bytes, (unsigned long long)overflows_);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kRingSize), nullptr, GL_STREAM_DRAW);
            generation_++;
            Census().ring_respecs++;
            Census().ring_respec_bytes += kRingSize;
            offset_ = slice_end_ - kVitaSliceSize;
            off = (offset_ + stride - 1) / stride * stride;
        }
        void* p = glMapBufferRange(GL_ARRAY_BUFFER, static_cast<GLintptr>(off), static_cast<GLsizeiptr>(bytes),
                                   GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
        if (p) {
            // one copy in 32 timed: the uncached ring's write rate on the device (the census's memory line)
            CopyIn(p, verts, bytes);
            glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes));
            glUnmapBuffer(GL_ARRAY_BUFFER);
        } else {
            glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(off), static_cast<GLsizeiptr>(bytes), verts);
        }
        offset_ = off + bytes;
        frame_bytes_ += bytes;
        if (frame_bytes_ > Census().ring_frame_peak) Census().ring_frame_peak = frame_bytes_;
        return static_cast<int>(off / stride);
    }
#endif
    // A single draw larger than the ring (the Vita's sliced ring: larger than a slice): plain re-specification -- a
    // storage of its own (fresh memory: no frame in flight reads it); the ring re-initialises on the next upload.
#if PS2R_HOST_VITA
    const size_t one_off = vita_orphan_ring_ ? kRingSize : kVitaSliceSize;
#else
    const size_t one_off = kRingSize;
#endif
    if (bytes > one_off) {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), verts, GL_STREAM_DRAW);
        ready_ = false;
        offset_ = 0;
        generation_++;
        Census().ring_respecs++;
        Census().ring_respec_bytes += bytes;
        return 0;
    }
    if (!ready_) {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kRingSize), nullptr, GL_STREAM_DRAW);
        ready_ = true;
        offset_ = 0;
        generation_++;
        Census().ring_respecs++;
        Census().ring_respec_bytes += kRingSize;
    }
    // the next multiple of the stride (32-byte records: the next 32-byte boundary, as before)
    size_t off = (offset_ + stride - 1) / stride * stride;
    if (off + bytes > kRingSize) {
#if !PS2R_HOST_VITA
        if (fence_) {
            glClientWaitSync(static_cast<GLsync>(fence_), GL_SYNC_FLUSH_COMMANDS_BIT,
                             1000000000ull);
            glDeleteSync(static_cast<GLsync>(fence_));
            fence_ = nullptr;
        }
#endif
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kRingSize), nullptr, GL_STREAM_DRAW);
        generation_++;
        Census().ring_respecs++;
        Census().ring_respec_bytes += kRingSize;
        fence_ = nullptr;
#if !PS2R_HOST_VITA
        fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
#endif
        off = 0;
    }
    void* p = glMapBufferRange(GL_ARRAY_BUFFER, static_cast<GLintptr>(off),
                               static_cast<GLsizeiptr>(bytes),
                               GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT |
                                   GL_MAP_FLUSH_EXPLICIT_BIT);
    if (p) {
        std::memcpy(p, verts, bytes);
        glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes));
        glUnmapBuffer(GL_ARRAY_BUFFER);
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(off), static_cast<GLsizeiptr>(bytes),
                        verts);
    }
    offset_ = off + bytes;
    return static_cast<int>(off / stride);
}

#if PS2R_HOST_VITA
namespace {
uint64_t g_copied_bytes = 0, g_inplace_bytes = 0;   // the drawing thread's alone
}  // namespace
uint64_t GlVertexStream::CopiedBytes() { return g_copied_bytes; }
uint64_t GlVertexStream::InPlaceBytes() { return g_inplace_bytes; }
void GlVertexStream::NoteInPlace(size_t bytes) { g_inplace_bytes += bytes; }
void GlVertexStream::NoteCopied(size_t bytes) { g_copied_bytes += bytes; }

bool GlVertexStream::InRing(const void* p, size_t bytes, size_t stride, int* first) const {
    if (!p || !first || !ready_ || !vbo_ || !stride) return false;
    const uint8_t* const base = *reinterpret_cast<const uint8_t* const*>(uintptr_t(vbo_));   // vitaGL's record: storage, size
    int32_t size;
    std::memcpy(&size, reinterpret_cast<const uint8_t*>(uintptr_t(vbo_)) + 4, sizeof size);
    const uint8_t* const q = static_cast<const uint8_t*>(p);
    if (!base || size <= 0 || q < base || q + bytes > base + size || size_t(q - base) % stride != 0) return false;
    *first = static_cast<int>(size_t(q - base) / stride);
    return true;
}

void GlVertexStream::CopyIn(void* dst, const void* src, size_t bytes) {
    static uint32_t sample = 0;
    if ((++sample & 31u) == 0) {
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        std::memcpy(dst, src, bytes);
        Census().ring_copy_us += sceKernelGetProcessTimeWide() - t0;
        Census().ring_copy_bytes += bytes;
    } else {
        std::memcpy(dst, src, bytes);
    }
}

void* GlVertexStream::ReserveRaw(size_t bytes, size_t stride, int* first) {
    if (bytes == 0 || !first || vita_orphan_ring_ || !ready_ || !vbo_ || bytes > kVitaSliceSize) return nullptr;
    // UploadRaw's steps without its GL calls: this vitaGL frame's slice, the next multiple of the stride in it
    const uint32_t frame = vglGetFrameNumber();
    if (frame != slice_frame_) {
        slice_frame_ = frame;
        offset_ = size_t(frame % uint32_t(kVitaSlices)) * kVitaSliceSize;
        slice_end_ = offset_ + kVitaSliceSize;
        frame_bytes_ = 0;
    }
    const size_t off = (offset_ + stride - 1) / stride * stride;
    if (off + bytes > slice_end_) return nullptr;   // (UploadRaw re-specifies the storage: the frame continues there)
    uint8_t* const base = *reinterpret_cast<uint8_t* const*>(uintptr_t(vbo_));   // vitaGL's buffer record: the storage at 0
    if (!base) return nullptr;
    Census().ring_bytes += bytes;
    offset_ = off + bytes;
    frame_bytes_ += bytes;
    if (frame_bytes_ > Census().ring_frame_peak) Census().ring_frame_peak = frame_bytes_;
    *first = static_cast<int>(off / stride);
    return base + off;
}
#endif

void GlVertexStream::BeginDrawRun() { in_run_ = true; }

void GlVertexStream::EndDrawRun(GlStateCache& st) {
    if (!in_run_) return;
    st.BindVertexArray(0);
    st.UseProgram(0);
    in_run_ = false;
}

}  // namespace render
}  // namespace ps2r
