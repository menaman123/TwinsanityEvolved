// render/backends/gl/gl_draw_executor.h -- GlDrawExecutor: one DrawIr -> one
// glDrawArrays(GL_TRIANGLES) of the PS2 program.
//
// Draw runs the fixed step order the pixel result depends on: guards, target bind +
// viewport + u_screen_size, per-game-frame clear, texture bind + texture uniforms,
// vertex conversion into the ring, the register uniforms, the 5551 write-mask
// analysis, the blend route (exact fixed-function map for the eight expressible
// RT-read combos, otherwise the shader-side blend over a lazily refreshed copy of
// the destination), depth state, scissor, colour mask, the draw, the colour / depth
// mask reset to their open defaults, and the result record for the coherence tail
// above the backend. Every input comes from DrawIr, TextureSource and DrawParams;
// no VRAM address is consulted here. DrawParams.program selects the stencil-composite
// specialisations instead: StripCount draws the geometry into the stencil-only
// framebuffer under the caller's StencilState (the routed colour image on unit 1 as
// the DATE source, the colour-authored Z on unit 4, colour writes off); Invert runs
// the folded byte-NOT program over a full Cd copy of the target.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/platform/host/platform.h"   // PS2R_HOST_VITA (the inline members below test it)
#include "engine/render/backend.h"
#include "engine/render/backends/gl/gl_state_cache.h"   // GlStateCache::Context (the run's context snapshot)
#include "engine/render/backends/gl/gpu_vertex.h"
#include "engine/render/texture_filter_selection.h"

namespace ps2r {
namespace gs {
struct DrawIr;
struct TextureSource;
}  // namespace gs

namespace render {

class GlDevice;

class GlDrawExecutor {
public:
    explicit GlDrawExecutor(GlDevice& device);
    // The Vita's per-draw call sequence as before the drawing thread's rework (Backend::SetOption "draw_legacy"; TE_GFX_DRAW_LEGACY=1 at
    // start): the full PS2 program bound while the uniforms are set and the variant bound after it, the sampler sizes
    // set on the full program every draw, the units 2-4 selected every draw, the masks reset after every draw
    void SetLegacy(bool on) { legacy_ = on; variant_known_ = false; }
    bool Legacy() const { return legacy_; }
#if PS2R_HOST_VITA
    // The native mesh about to be drawn (GlDevice::DrawMesh*): its GL buffer, for a raw mesh draw (GlShaderLibrary::RawMesh)
    void SetMeshBuffer(uint32_t vbo) { mesh_vbo_ = vbo; }
    // ... and its index buffer when it is indexed (GlDevice::GlMesh::list_ibo; 0: the vbo is its triangle list), with its
    // distinct vertices per list vertex (the fill census's vertex-work estimate)
    void SetMeshIndices(uint32_t ibo, float shaded) { mesh_ibo_ = ibo; mesh_shaded_ = shaded; }
    // the raw mesh draws allowed (GlDevice: TE_GFX_VITA_RAW_MESH, vitaGL's buffer record checked at the first mesh)
    void SetRawMesh(bool on) { raw_mesh_ = on; }
    // the raw vertex draws taken (DrawRaw through GXM; GlDevice::ReserveRaw hands out ring room only then)
    bool RawVertexDraws() const;
    // The skin mesh about to be drawn (GlDevice::DrawSkinMesh): its GL vertex and index buffers, for a raw skin draw
    void SetSkinBuffers(uint32_t vbo, uint32_t ibo) { skin_vbo_ = vbo; skin_ibo_ = ibo; }
#endif
    DrawResult Draw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p);
    // A native mesh (GlDevice::DrawMesh): `ir`'s state over the mesh's `count` vertices in `vao`, transformed by the shader
    DrawResult DrawMesh(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, uint32_t vao, size_t count,
                        const MeshDrawParams& mp);
    // A mesh under the last DrawMesh's state, when the caches show nothing moved since (GlDevice::DrawMeshAgain)
    DrawResult DrawMeshAgain(uint32_t vao, size_t count, const MeshDrawParams& mp);
    // A mesh under the last DrawMesh's state with another texture (an owned, decoded one in both): BindTextures and the
    // texture uniforms for `tex`, then DrawMeshAgain's transform and draw (GlDevice::DrawMeshTexture)
    DrawResult DrawMeshTexture(const gs::DrawIr& ir, const gs::TextureSource& tex, uint32_t vao, size_t count,
                               const MeshDrawParams& mp);
#if PS2R_HOST_VITA
    // A packet's replay (GlDevice::DrawMeshPacket): `count` vertices of the mesh in `vbo` from mp.first, through `mp`, with
    // the packet's programs, texture and uniforms -- only while this run's GL context stands as its last draw left it (the
    // caches' counts: nothing set since; the caller vouches that the packet's state takes this context). False: not drawn
    // (the census counts why). `ibo` / `shaded`: an indexed mesh's index buffer (`count` indices from mp.first) and its
    // distinct vertices per list vertex (the fill census's vertex estimate), as SetMeshIndices takes them; 0 / 1 a list.
    bool DrawMeshPacket(uint32_t packet, uint32_t vbo, uint32_t ibo, float shaded, size_t count, const MeshDrawParams& mp);
    // `n` replays of one packet over one range (`count` vertices / indices from `first`), each with its own
    // transform (inst[i]), as ONE instanced draw -- under DrawMeshPacket's conditions; false: none drawn
    bool DrawMeshPacketInstances(uint32_t packet, uint32_t vbo, uint32_t ibo, size_t count, uint32_t first, const MeshInstance* inst,
                                 int n);
    // A skin packet's replay (GlDevice::DrawSkinPacket): the skin mesh in `vbo` through its `count` indices in `ibo`, `mp`
    // and the palette, with the packet's programs, texture and uniforms -- under DrawMeshPacket's conditions (this run's GL
    // context as its last draw left it; the caller vouches that the packet's state takes it)
    bool DrawSkinPacket(uint32_t packet, uint32_t vbo, uint32_t ibo, float shaded, size_t count, const MeshDrawParams& mp,
                        const float* bones, int joints, const float* disp, int disp_count);
#endif
    // A GPU-skinned mesh (GlDevice::DrawSkinMesh; the Vita): DrawMesh's state sequence, the skin program for the draw's
    // state (GlShaderLibrary::SelectSkin) with the palette (`joints` x 3 rows), the mesh's `count` indices drawn
    DrawResult DrawSkinMesh(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, uint32_t vao, size_t count,
                            const MeshDrawParams& mp, const float* bones, int joints, const float* disp, int disp_count);
    // A raw draw (GlDevice::DrawRaw): the producer's vertices uploaded as they are, read by the raw arrays
    bool DrawsRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p) const;
    DrawResult DrawRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, const RawVertex* verts,
                       size_t n, const float* box);

    // Pure helpers (unit-testable without a context).
    static uint32_t Fbmsk16From32(uint32_t m);
    static bool Fbmsk16Exact(uint32_t fbmsk16);
    static bool RouteRtBlend(const gs::DrawIr& ir, bool using_rt, bool fbmsk16_exact);
    static void SelectFilters(bool mag_linear, uint8_t mmin, uint8_t levels, uint32_t* mag,
                              uint32_t* min);
    // The player's enhancement over an OWNED texture's sampler.
    // `mag`/`min` arrive as SelectFilters chose them from the title's TEX1 and
    // leave as the mode demands: Title keeps them; Point / Bilinear force
    // nearest / linear, mip-aware exactly when the title sampled the chain
    // (`title_mips`) or the hardware LOD is in force; Trilinear forces linear
    // with linear level blending whenever the texture carries a chain
    // (`levels` > 0). Returns whether the draw samples with the HARDWARE LOD
    // (u_tex_lod_hw): anisotropy > 1 or Trilinear over a chain -- the implicit
    // footprint LOD the driver's anisotropic / level-blended fetch needs, in
    // place of the retail per-pixel formula. `anisotropy` is clamped to the
    // driver limit in `*applied_anisotropy` (1 = none).
    static bool EnhanceFilters(TextureFilter filter, int anisotropy, int max_anisotropy, bool title_mips,
                               uint8_t levels, uint32_t* mag, uint32_t* min, int* applied_anisotropy);

private:
    // the variant choice, remembered while none of the watched uniforms (the fast program's constants) changed
    bool variant_known_ = false;
#if PS2R_HOST_VITA
    uint32_t variant_gen_ = 0;
    uint64_t variant_watch_ = 0;
    bool variant_fast_ = false;
    uint32_t mesh_vbo_ = 0;    // SetMeshBuffer
    uint32_t mesh_ibo_ = 0;    // SetMeshIndices
    float mesh_shaded_ = 1.f;
    bool raw_mesh_ = true;     // SetRawMesh
    uint32_t skin_vbo_ = 0, skin_ibo_ = 0;   // SetSkinBuffers
#endif
    // A native mesh's draw through vitaGL (no raw draw made): its list's range [first, first + count) -- an indexed mesh's
    // through its index buffer (the Vita), else as the triangle list it is
    void DrawMeshTriangles(uint32_t first, size_t count);
    struct Step {
        bool has_texture = false;
        bool using_rt = false;
        bool hw_lod = false;     // the enhancement samples the chain with the hardware LOD
        uint32_t fbo = 0;
        int render_w = 0, render_h = 0, scale = 1;
        size_t tri_verts = 0;
        int first = 0;
        bool alpha_blend = false;
        uint32_t fbmsk16 = 0;
        bool fbmsk16_exact = false;
        bool route_rt_blend = false;
        bool idx_self_serve = false;
        bool wrap_route = false;
        bool needs_rt_copy = false;
        bool sw_blend_active = false;
        int copy_x = 0, copy_y = 0, copy_w = 0, copy_h = 0;
        bool cd_copied = false;
        Rect cd_rect;
        float bb_tri[4] = {0, 0, 0, 0};
        float bb_all[4] = {0, 0, 0, 0};
        bool bb_valid = false;
    };
    // The Vita: binds the PS2 program variant the draw about to be made takes (the fast program while every uniform it
    // specializes holds its constant: GlShaderLibrary::Ps2FastProgram; else the full one) -- once, right before the draw
    // -- with its sampler sizes and the uniform values it does not hold yet (GlUniformCache::Sync). `s`: the draw's step
    // (its screen box); `again`: a run's again-draw (the legacy sequence binds no full program there). Elsewhere a no-op
    // (the caller binds the PS2 program before setting its uniforms). `raw` (the Vita's native mesh draws): the draw may
    // be made raw (GlShaderLibrary::RawMesh) -- true when it was (nothing else to draw); `first`: the mesh range's first
    // vertex (MeshDrawParams::first, 0 = the mesh's start), where the raw draw's stream starts as the GL draw does.
    bool BindPs2Program(const Step& s, bool again = false, bool mesh = false, uint32_t tex_w = 0, uint32_t tex_h = 0,
                        uint32_t first = 0);
#if PS2R_HOST_VITA
    // BindPs2Program for a raw draw whose RawVertex records lie in the vertex ring from `s.first`: the program
    // chosen as for the GL draw, the draw made raw through GXM when it is the raw vertex path's (true: drawn, nothing more
    // to do), else that program bound with its uniforms for the GL draw (false). `tex_w` / `tex_h`: the texture's size
    // when known (vitaGL's texture record check), else 0.
    bool BindPs2RawVertices(const Step& s, uint32_t tex_w, uint32_t tex_h);
#endif

    DrawResult DrawStripCount(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p);
    DrawResult DrawInvert(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p);
    bool BindTarget(const gs::DrawIr& ir, const DrawParams& p, Step& s);
    void FrameClear(const DrawParams& p);
    void BindTextures(const gs::TextureSource& tex, const DrawParams& p, Step& s);
    void TextureUniforms(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p,
                         const Step& s);
    bool ConvertVertices(const gs::DrawIr& ir, const gs::TextureSource& tex, Step& s);
    void RegisterUniforms(const gs::DrawIr& ir, const DrawParams& p);
    void ClassifyRoutes(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p,
                        Step& s);
    void CdCopy(const DrawParams& p, Step& s);
    void BlendAbcdUniforms(const gs::DrawIr& ir, const DrawParams& p);
    void BlendState(const gs::DrawIr& ir, const DrawParams& p, Step& s);
    void DepthState(const gs::DrawIr& ir, const DrawParams& p, const Step& s);
    void ScissorState(const gs::DrawIr& ir, const Step& s);
    void ColorMaskState(const gs::DrawIr& ir, const Step& s);
    DrawResult MakeResult(const gs::DrawIr& ir, const DrawParams& p, const Step& s) const;

    void MeshUniforms(const MeshDrawParams& mp);

    GlDevice& dev_;
    std::vector<GpuVertex> scratch_;
    // the last mesh draw, while another may take its state: its step, record and parameters, and the caches' counts
    // after it
    bool run_valid_ = false;
    Step run_step_;
    const gs::DrawIr* run_ir_ = nullptr;
    DrawParams run_params_;
    uint64_t run_state_ = 0, run_uniforms_ = 0;
    int run_color_mask_ = -1, run_depth_mask_ = -1;   // the masks the run's draw drew under (the open defaults follow it)
    bool run_textured_ = false;   // the run's draw sampled an owned decoded texture (u_uv_mode 2): DrawMeshTexture may follow
#if PS2R_HOST_VITA
    // the run's fixed-function context as its last draw left it (GlStateCache::Snapshot; DrawMeshPacket after a vertex draw
    // of the run's own state), and the state cache's count at which a later context was last found equal to it
    GlStateCache::Context run_ctx_{};
    bool run_ctx_known_ = false;
    uint64_t pkt_ctx_mut_ = ~0ull;
    // the run's packets may be drawn: set with run_valid_ by DrawMesh, NOT ended by a skin draw (whose program, arrays and
    // uniforms end the slow paths' run but leave the fixed-function context, which DrawMeshPacket checks)
    bool pkt_valid_ = false;
#endif
    // What TextureUniforms / RegisterUniforms last sent (they are the only writers of their uniforms): a draw whose
    // values are the same sends nothing and skips the ~35 cache lookups (valid while the uniform cache's generation holds)
    struct TextureUniformValues {
        int32_t use_texture, depth_fmt, depth_zoff, idx_fmt, uv_native, draw_scale, idx_self, clut_raw, tfx, mip_levels,
            mip_lcm, lod_hw, tcc;
        float tex_w, tex_h, lod_k, lod_l;
    };
    struct RegisterUniformValues {
        int32_t blend_enabled, fba, colclamp, fb16, ct16q, dthe, dimx_lo, dimx_hi, scanmsk, ate, atst, aref, afail, date,
            datm, fog;
        float fog_r, fog_g, fog_b;
    };
    TextureUniformValues texture_sent_{};
    RegisterUniformValues register_sent_{};
    uint32_t texture_sent_gen_ = 0, register_sent_gen_ = 0;   // 0: nothing remembered
    bool register_inherited_dither_ = false;
    bool legacy_ = false;   // SetLegacy
    // the full PS2 program bound before the uniforms are set (always off the Vita; on the Vita only in the legacy sequence)
    bool BindFullFirst() const {
#if PS2R_HOST_VITA
        return legacy_;
#else
        return true;
#endif
    }
    // the colour / depth masks put back to open after each draw (likewise)
    bool ResetMasksAfter() const { return BindFullFirst(); }
};

}  // namespace render
}  // namespace ps2r
