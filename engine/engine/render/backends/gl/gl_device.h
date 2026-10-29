// render/backends/gl/gl_device.h -- GlDevice: the OpenGL 4.1 core Backend.
//
// Owns the GL objects of the backend (programs, uniform / state shadows, the vertex
// stream, the texture pool, the 1x1 white dummy texture every unused sampler unit
// binds) and the surface table: a renderable RGBA8 surface is a texture plus a
// framebuffer object, a depth surface a GL_DEPTH_COMPONENT32F texture (sampleable, so
// depth views can be served), a plain RGBA8 surface a texture drawn from the pool.
// Depth clears use 0.0 (the vertex shader maps GS z to window depth z / 2^32 -- z / 2^24
// for a Z24 producer, GlDeviceSettings::gs_z24 -- so the GS far plane z = 0 is exactly
// window 0.0 and GEQUAL orders correctly). Readback is
// synchronous glReadPixels (rows bottom-up as the attachment stores them) and the
// staging API completes immediately. A DepthStencil24_8 surface is a
// GL_DEPTH24_STENCIL8 renderbuffer; the stencil-composite passes draw into it through
// two device-owned framebuffers -- a stencil-only one (no colour attachment, draw
// buffer NONE) and a resolve one (a colour texture + the same stencil) -- rebuilt
// when the attachments change (OLD GsS9EnsureStencil). Present draws the scanout quad into the default
// framebuffer and calls the swap callback the platform supplied. The GL context must
// be current on the calling thread for the device's whole life.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "engine/render/backend.h"
#include "engine/render/backends/gl/gl_shader_library.h"
#include "engine/render/backends/gl/gl_state_cache.h"
#include "engine/render/backends/gl/gl_texture_pool.h"
#include "engine/render/backends/gl/gl_uniform_cache.h"
#include "engine/render/backends/gl/gl_vertex_stream.h"
#include "engine/render/texture_filter_selection.h"

namespace ps2r {
namespace render {

class GlDrawExecutor;

struct GlDeviceSettings {
    uint8_t scale = 1;             // default surface scale
    // The producer's GS Z stays in [0, 2^24] (a ps2e_draw title whose screen matrix maps the clip volume onto 0..2^24):
    // window depth z / 2^24, every integer Z exact (GlShaderLibrary::SetGsZ24Depth); false = z / 2^32 (a Z32 producer)
    bool gs_z24 = false;
};

// EXT/ARB_texture_filter_anisotropic tokens (the core headers do not all carry them).
constexpr uint32_t kGlTextureMaxAnisotropy = 0x84FE;
constexpr uint32_t kGlMaxTextureMaxAnisotropy = 0x84FF;

// One surface's GL objects.
struct GlSurface {
    SurfaceDesc desc;
    uint32_t tex = 0;
    uint32_t rbo = 0;              // DepthStencil24_8 renderbuffer
    uint32_t fbo = 0;              // renderable colour surfaces only
    uint32_t attached_depth = 0;   // depth texture (or depth-stencil renderbuffer) currently attached to fbo
    bool attached_depth_rbo = false;   // attached_depth names a renderbuffer (GL_DEPTH_STENCIL_ATTACHMENT)
    bool pooled = false;           // texture came from / returns to the pool
    bool respecify = false;        // pooled storage was busy: the first full upload re-specifies
    uint8_t levels = 0;            // authored mip levels
    uint8_t synth_levels = 0;      // synthesized tail levels
};

class GlDevice : public Backend {
public:
    explicit GlDevice(const GlDeviceSettings& settings = GlDeviceSettings{});
    ~GlDevice() override;

    // The context is already current; `swap` presents the default framebuffer.
    bool Init(std::function<void()> swap);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    Caps QueryCaps() const override;
    SurfaceId CreateSurface(const SurfaceDesc& desc) override;
    void DestroySurface(SurfaceId id) override;
    SurfaceDesc DescribeSurface(SurfaceId id) const override;
    void Clear(SurfaceId color, SurfaceId depth, uint32_t rgba, float z, uint32_t flags) override;
    void Upload(SurfaceId id, const Rect& rect, const void* bytes, size_t stride) override;
    SurfaceId CreateTextureWithData(const SurfaceDesc& desc, const void* bytes) override;
    void UploadLevel(SurfaceId id, int level, int w, int h, const void* rgba) override;
    void FinishMipChain(SurfaceId id, int authored_levels, bool synthesize_tail) override;
    DrawResult Draw(const gs::DrawIr& ir, const gs::TextureSource& tex,
                    const DrawParams& params) override;
    uint32_t CreateMesh(const MeshVertex* verts, size_t count) override;
    uint32_t CreateMeshIndexed(const MeshVertex* distinct, size_t distinct_count, const uint16_t* indices, size_t count) override;
    void DestroyMesh(uint32_t mesh) override;
    DrawResult DrawMesh(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params, uint32_t mesh,
                        const MeshDrawParams& mp) override;
    DrawResult DrawMeshAgain(uint32_t mesh, const MeshDrawParams& mp) override;
    DrawResult DrawMeshTexture(const gs::DrawIr& ir, const gs::TextureSource& tex, uint32_t mesh, const MeshDrawParams& mp) override;
#if PS2R_HOST_VITA
    // The mesh draw packets (Backend::TakeMeshPacket / DrawMeshPacket): GlShaderLibrary's raw draws record them, the
    // executor vouches for its run's GL context, GlShaderLibrary::DrawRawPacket replays one
    uint32_t TakeMeshPacket() override;
    bool DrawMeshPacket(uint32_t packet, uint32_t mesh, const MeshDrawParams& mp) override;
    // the instanced packet draws (Backend::DrawMeshPacketInstances)
    bool DrawMeshPacketInstances(uint32_t packet, uint32_t mesh, uint32_t first, uint32_t count, const MeshInstance* inst,
                                 int n) override;
    bool InstancedPacketDraws() const override { return shaders_.InstancingOn(); }
    // the GPU-skin draw packets (Backend::TakeSkinPacket / DrawSkinPacket): GlShaderLibrary's raw skin draws record them
    uint32_t TakeSkinPacket() override;
    bool DrawSkinPacket(uint32_t packet, uint32_t mesh, const MeshDrawParams& mp, const float* bones, int joints,
                        const float* disp, int disp_count) override;
#endif
    uint32_t CreateSkinMesh(const SkinVertex* verts, size_t count, const uint16_t* indices, size_t index_count) override;
    DrawResult DrawSkinMesh(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params, uint32_t mesh,
                            const MeshDrawParams& mp, const float* bones, int joints, const float* disp, int disp_count) override;
    bool DrawsRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params) const override;
    DrawResult DrawRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params, const RawVertex* verts,
                       size_t n, const float* box) override;
#if PS2R_HOST_VITA
    // the vertex ring's room for `n` raw vertices written in place (GlVertexStream::ReserveRaw); a DrawRaw of
    // them draws them where they lie (the executor finds them in the ring)
    RawVertex* ReserveRaw(size_t n) override;
    void ApplyStencilState(const StencilState& st) override { ApplyStencil(st); }
    void ResetStencilState() override { ResetStencil(); }
#endif
    void CopyRect(SurfaceId src, const Rect& src_rect, SurfaceId dst, const Rect& dst_rect,
                  uint32_t transform) override;
#if PS2R_HOST_VITA
    // vitaGL: the frame target's own depth/stencil surface force-stored for the frame (keep) and force-loaded again after
    // each copy (CopyRect) -- see Backend::KeepDepthAcrossCopies; TE_GFX_VITA_KEEP_DEPTH=0 leaves it transient
    void KeepDepthAcrossCopies(SurfaceId color, bool keep) override;
    // The movie pictures (gl_movie.cpp): a cpu_writable surface's vitaGL storage (vglGetTexDataPointer; the row stride is
    // vitaGL's: the width aligned to 8 texels), and a picture drawn from such surfaces by the movie program
    void* SurfaceMemory(SurfaceId id, size_t* stride) override;
    bool DrawMoviePicture(const MoviePicture& m) override;
#endif
    void Readback(SurfaceId id, const Rect& rect, void* out, size_t stride) override;
    FenceId BeginReadback(SurfaceId id, const Rect& rect) override;
    bool PollReadback(FenceId fence, void* out, size_t stride) override;
    void Present(const ScanoutDesc& desc) override;
    void ArmPresentCapture(bool armed) override;
    bool ReadPresented(std::vector<uint8_t>* rgba, int* w, int* h) override;
    void ClearStencil(SurfaceId stencil, uint8_t value) override;
    void DrawConstQuad(SurfaceId color, SurfaceId stencil, const Rect& scissor, uint32_t rgba,
                       bool write_alpha, const StencilState& st) override;
    // The opaque-alpha program over the NDC triangle into the stencil-only framebuffer: the
    // colour surface sampled on unit 0, colour writes off, depth ALWAYS without writes, the
    // stencil REPLACEs `bits` under a write mask of `bits` where the fragment survives.
    bool MarkStencilWhereOpaque(SurfaceId color, SurfaceId stencil, uint8_t bits) override;
    // glReadPixels GL_STENCIL_INDEX from the stencil-only framebuffer.
    bool ReadbackStencil(SurfaceId stencil, const Rect& rect, uint8_t* out) override;
    void BeginFrame() override;
    void EndFrame() override;
    // Run brackets (a run = consecutive draws; any other GL work ends it).
    void BeginDrawRun() override;
    void EndDrawRun() override;
    // Default-framebuffer size Present falls back to when ScanoutDesc.dst is empty.
    void SetWindowSize(int w, int h) override;
    // "draw_legacy" (Backend::SetOption): the executor's and the uniform cache's per-draw sequence as before the drawing thread's rework
    void SetOption(const char* name, int value) override;

    // Binds GL_FRAMEBUFFER to the colour surface's fbo with `depth` attached
    // (attachment changed only when it differs). Returns the fbo (0 if none).
    uint32_t BindTarget(SurfaceId color, SurfaceId depth);
    // Binds GL_FRAMEBUFFER to the stencil-only framebuffer (color none) or the
    // resolve framebuffer (colour texture + stencil renderbuffer). Returns the fbo
    // (0 when the stencil surface is missing or the framebuffer is incomplete).
    uint32_t BindStencilTarget(SurfaceId color, SurfaceId stencil);
    // Raw stencil state (not cached: the passes set it per draw and reset it after).
    static void ApplyStencil(const StencilState& st);
    static void ResetStencil();
    uint32_t ConstVao() const { return const_vao_; }

    // Component access for the executor and tests.
    GlStateCache& State() { return state_; }
    GlUniformCache& Uniforms() { return uniforms_; }
    const GlShaderLibrary& Shaders() const { return shaders_; }
    GlVertexStream& Vertices() { return vertices_; }
    // A 1x1 white RGBA8 texture kept bound on every sampler unit a draw does not use.
    uint32_t DummyTexture() const { return dummy_tex_; }
    // The player's filtering enhancement over the owned textures:
    // read per draw by the executor; null = the title's choice everywhere. Any
    // thread may change the selection's fields; this pointer is set once by the
    // context owner. MaxAnisotropy is the driver's limit (1 = no extension).
    void SetTextureFilter(std::shared_ptr<const TextureFilterSelection> selection) {
        texture_filter_ = std::move(selection);
    }
    const TextureFilterSelection* TextureFilter() const { return texture_filter_.get(); }
    int MaxAnisotropy() const { return max_anisotropy_; }
    // Census of the enhancement: owned-texture draws seen and the draws whose
    // sampler the selection changed (the title's choice would have differed).
    struct TextureFilterCensus {
        uint64_t owned_draws = 0;
        uint64_t enhanced_draws = 0;
        uint64_t hw_lod_draws = 0;   // draws sampled with the hardware LOD (aniso / trilinear over a chain)
    };
    const TextureFilterCensus& FilterCensus() const { return filter_census_; }
    TextureFilterCensus& FilterCensus() { return filter_census_; }
    const GlSurface* Surface(SurfaceId id) const;

private:
    bool BuildRenderable(GlSurface& s);
    void ReleaseSurface(GlSurface& s);

    GlDeviceSettings settings_;
    bool initialized_ = false;
    std::function<void()> swap_;
    GlStateCache state_;
    GlUniformCache uniforms_;
    GlShaderLibrary shaders_;
    GlVertexStream vertices_;
    GlTexturePool pool_;
    std::unique_ptr<GlDrawExecutor> executor_;
    uint32_t dummy_tex_ = 0;
    uint32_t scanout_vao_ = 0;
    uint32_t scanout_vbo_ = 0;
    // CreateMesh / CreateMeshIndexed: a mesh from its list (deduplicated here on the Vita) or from its indexed form given
    uint32_t MakeMesh(const MeshVertex* verts, size_t count, const MeshVertex* pre_distinct, size_t pre_count, const uint16_t* pre_indices);
    void EnsureMeshIndexMode();        // (the Vita: TE_GFX_VITA_MESH_INDEX read once)
#if PS2R_HOST_VITA
    float scanout_quad_[24] = {};      // what scanout_vbo_ holds (the Vita uploads the quad only when it changes)
    bool scanout_quad_valid_ = false;
    int vita_ps2_fast_ab_ = 0;         // TE_GFX_VITA_PS2_FAST=abN: the fast PS2 program flipped every N presents
    int vita_ps2_lean_ab_ = 0;         // TE_GFX_VITA_PS2_LEAN=abN: the lean PS2 programs flipped every N presents
    int vita_raw_ab_ = 0;              // TE_GFX_VITA_RAW_MESH=abN: the raw mesh draws flipped every N presents
    int vita_mesh_index_ = -1;         // TE_GFX_VITA_MESH_INDEX: native meshes indexed over their distinct vertices (-1 unread)
    int vita_mesh_ab_ = 0;             // TE_GFX_VITA_MESH_INDEX=abN: the indexed form drawn in every other window of N presents
    bool vita_mesh_ab_on_ = true;
    uint64_t vita_mesh_ab_presents_ = 0;
    uint64_t vita_raw_ab_presents_ = 0;
    int vita_packets_ab_ = 0;          // TE_GFX_VITA_PACKETS=abN: the mesh draw packets flipped every N presents
    uint64_t vita_packets_ab_presents_ = 0;
    uint64_t vita_ps2_ab_presents_ = 0;
    std::chrono::steady_clock::time_point vita_last_present_{};
    bool vita_last_present_valid_ = false;
#endif
    uint32_t present_nearest_ = 0; // window-only sampler objects; never alter shared textures
    uint32_t present_linear_ = 0;
    uint32_t const_vao_ = 0;       // NDC fullscreen triangle for the stencil-masked quads
    uint32_t const_vbo_ = 0;
    uint32_t sten_fbo_ = 0;        // stencil-only framebuffer
    uint32_t res_fbo_ = 0;         // colour + stencil resolve framebuffer
    uint32_t sten_fbo_rbo_ = 0;    // attachments the two framebuffers currently hold
    uint32_t res_fbo_rbo_ = 0;
    uint32_t res_fbo_tex_ = 0;
#if PS2R_HOST_VITA
    // vitaGL: the framebuffer BindTarget last attached a depth-stencil renderbuffer to (the frame target's) -- the only
    // framebuffer whose depth/stencil the shadow counter is (ClearStencil); vita_scene_stencil_ false with
    // TE_GFX_VITA_STENCIL=0 (the stencil-only framebuffer path)
    uint32_t vita_ds_fbo_ = 0, vita_ds_rbo_ = 0;
    bool vita_scene_stencil_ = true;
    // The screen copy (CopyRect) on vitaGL: glCopyTexSubImage2D there is glReadPixels (the scene ended + sceGxmFinish +
    // a CPU copy of the colour memory, which Vita3K's disable-surface-sync leaves stale) + glTexSubImage2D. Instead the
    // source texture is drawn into the destination texture's own framebuffer (copy_fbo_, re-attached when the destination
    // changes) by the copy program: GPU work only, in a scene of its own. TE_GFX_VITA_COPY=0 = glCopyTexSubImage2D.
    bool VitaCopyRect(const GlSurface& s, const Rect& src_rect, const GlSurface& d, const Rect& dst_rect);
    // the SceGxmDepthStencilSurface vitaGL keeps for framebuffer `fbo` (null until the names are verified, when it has
    // none yet, or when the layout check fails)
    void* VitaDepthSurface(uint32_t fbo, uint32_t width, uint32_t height);
    void VitaDepthModes(uint32_t fbo, bool store, bool load);
    // Present, while the frame target's scene is the one in progress: vitaGL's own record of that scene's framebuffer
    // must be the target's GL name (the proof that a name IS vitaGL's framebuffer struct) before any name is read through
    void VitaCheckFramebufferNames(const GlSurface& frame);
    void VitaDropKeep(const char* why);   // this frame keeps no depth from here on (one log line per reason)
    uint32_t copy_fbo_ = 0, copy_fbo_tex_ = 0;
    bool vita_gpu_copy_ = true, vita_keep_depth_ = true;
    bool vita_copy_restore_ = true;   // the frame drawn back from the copy after the switch (TE_GFX_VITA_COPY_RESTORE=0: not)
    // TE_GFX_VITA_COPY_RESTORE unset / auto: the frame's scene CONTINUED after the copy when it can be -- vitaGL told the
    // frame's framebuffer was its last scene's (old_framebuffer), so it begins the next scene without its scene-start mask
    // update; nothing is painted over the frame, so nothing is restored. =1: always the mask update + the restore (as before)
    bool vita_copy_continue_ = true;
    uint64_t vita_copies_continued_ = 0, vita_copies_restored_ = 0;
    // THE COPY TEXTURE'S SAMPLING AFTER A QUIET COPY (TE_GFX_VITA_COPY_SAMPLE). vitaGL applies a bound sampler object by
    // rewriting the bound texture's own GXM record in place (custom_shaders.c: filters, address modes, mip count, LOD
    // bias; `overridden` set) and puts the texture's own values back only at a later vitaGL draw of it with no sampler
    // bound; the raw draws hand GXM the record as it stands, and the executor's TexParami cache re-states only values it
    // did not set itself. The noisy copy's restore samples the copy texture through present_nearest_, so since a run's
    // first restore the raw draws that sample the copy (the icicles' refraction) have sampled it NEAREST / CLAMP.
    // unset / `base`: a quiet copy makes that same rewrite (a zero-area draw, VitaCopyRect) -- the pictures stay the noisy
    // copy's; `game`: it does not -- the copy is sampled as the material asks (as the desktop does): a different picture.
    bool vita_copy_sample_base_ = true;
    uint32_t null_vao_ = 0, null_vbo_ = 0;   // the zero-area triangle (three equal vertices) of that rewrite
    bool vita_copy_none_ = false;     // diagnosis: TE_GFX_VITA_COPY=none, no copy at all
    int vita_copy_ab_ = 0;            // diagnosis: TE_GFX_VITA_COPY=ab[N], copies on / off every N presents
    uint64_t vita_ab_presents_ = 0;
    int vita_vgl_ = 0;   // vitaGL's framebuffer names: 0 not yet seen, 2 the name proven (its surface not ready yet),
                         // 1 verified (the keep can run), -1 unusable (the keep stays off)
    uint32_t vita_keep_fbo_ = 0;      // the framebuffer whose depth/stencil this frame stores (copies announced)
    uint32_t vita_keep_w_ = 0, vita_keep_h_ = 0;
    const void* vita_keep_data_ = nullptr;   // its depth memory when the store was set (a re-made surface is not loaded)
    bool vita_keep_loading_ = false;  // its later scenes load them again (set by the frame's first copy)
    uint64_t vita_copies_ = 0, vita_kept_frames_ = 0;
    // The movie picture's geometry (gl_movie.cpp DrawMoviePicture): an exact quad over the picture's viewport
    // (two triangles, NDC -1..1), made at the first movie draw. The GPU of the Vita does not clip at the viewport (GXM's
    // viewport is a transform; only the region clip and vitaGL's scissor mask bound a draw), so the copy program's
    // full-screen triangle (const_vao_, NDC up to 3) drawn over a letterboxed picture's viewport painted the target below
    // and right of the picture; Vita3K's host viewport clips it. TE_MOVIE_QUAD=0: the full-screen triangle (the device A/B).
    uint32_t movie_quad_vao_ = 0, movie_quad_vbo_ = 0;
    int movie_quad_mode_ = -1;   // -1 not read yet, 1 the exact quad (default), 0 the full-screen triangle
    float movie_last_box_[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};   // the last logged picture box + target (one line per change)
#endif
    int window_w_ = 0;
    int window_h_ = 0;
    bool present_capture_ = false;      // ArmPresentCapture: Present reads its window image back
    std::vector<uint8_t> presented_;    // the last captured window image (RGBA8, bottom-up)
    int presented_w_ = 0, presented_h_ = 0;
    uint32_t capture_fbo_ = 0;          // the armed present's window-sized target (then blitted to the window)
    uint32_t capture_tex_ = 0;
    int capture_w_ = 0, capture_h_ = 0;
    bool CaptureTarget(int w, int h);
    std::unordered_map<uint32_t, GlSurface> surfaces_;
    uint32_t next_surface_ = 1;
    struct GlMesh {
        uint32_t vao = 0, vbo = 0;
        size_t count = 0;            // vertices (a native mesh: its triangle list's) or indices (a skin mesh)
        uint32_t ibo = 0;            // a skin mesh's index buffer (CreateSkinMesh; 0: a native mesh's triangle list)
        uint32_t list_ibo = 0;       // the Vita: a native mesh's list as 16-bit indices into its distinct vertices (the vbo
                                     // then holds each distinct vertex once; 0: the vbo is the list itself)
        float shaded = 1.f;          // distinct vertices / list vertices (the fill census's vertex-work estimate)
        uint32_t vao_idx = 0, vbo_idx = 0;   // TE_GFX_VITA_MESH_INDEX=abN: the indexed form beside the list (vao / vbo)
    };
    std::unordered_map<uint32_t, GlMesh> meshes_;
    uint32_t next_mesh_ = 1;
    // The last lookups, direct-mapped by id (an element's address holds until it is erased; the erase drops its slot): a
    // draw finds its target, depth, texture and mesh without hashing -- the map's bucket index is a division, which the
    // Vita's CPU does in software
    static constexpr uint32_t kLookupSlots = 64;
    mutable uint32_t surface_ids_[kLookupSlots] = {};
    mutable const GlSurface* surface_ptrs_[kLookupSlots] = {};
    // the meshes' slots: 2048, an id and its record side by side (one cache line per lookup) -- with 64 a hub
    // frame's ~700 mesh draws over several hundred meshes found most of them in the map (a software division per lookup)
    static constexpr uint32_t kMeshSlots = 2048;
    struct MeshSlot {
        uint32_t id = 0;
        const GlMesh* ptr = nullptr;
    };
    MeshSlot mesh_slots_[kMeshSlots] = {};
    const GlMesh* FindMesh(uint32_t mesh);
    struct PendingReadback {
        std::vector<uint8_t> bytes;
        size_t stride = 0;
        int rows = 0;
    };
    std::unordered_map<uint64_t, PendingReadback> readbacks_;
    uint64_t next_fence_ = 1;
    int max_texture_size_ = 0;
    int max_anisotropy_ = 1;
    std::shared_ptr<const TextureFilterSelection> texture_filter_;
    TextureFilterCensus filter_census_;
};

}  // namespace render
}  // namespace ps2r
