// render/backends/gl/gl_draw_executor.cpp -- GlDrawExecutor implementation.
#include "engine/render/backends/gl/gl_draw_executor.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/texsrc/texture_source.h"
#include "engine/render/backends/gl/blend_ff_map.h"
#include "engine/render/backends/gl/cd_copy_policy.h"
#include "engine/render/backends/gl/gl_census.h"
#include "engine/render/backends/gl/gl_device.h"
#include "engine/render/backends/gl/gl_fill_census.h"
#include "engine/render/backends/gl/vertex_convert.h"
#if PS2R_HOST_VITA
#include <psp2/kernel/processmgr.h>   // (the full paths' sampled CPU split)
#endif


namespace ps2r {
namespace render {

GlCensus g_gl_census;   // (gl_census.h: constant-initialised, no guard)

namespace {

#if PS2R_HOST_VITA
// vitaGL draws every glDrawArrays through ONE fixed buffer of 0xC000 progressive 16-bit indices (vgl.c default_idx_ptr,
// MAX_IDX_NUMBER; draw.c: sceGxmDraw(..., default_idx_ptr, count), checked only in a debug vitaGL -- the VitaSDK package
// is built NO_DEBUG). A longer draw would read its indices past that buffer: the next allocations' patterns (the quads'
// and the line strips' index tables), i.e. triangles made of the wrong vertices of the draw's head, and its own tail
// never drawn. A longer triangle list is therefore drawn as runs of at most kVitaDrawVerts vertices (a multiple of 3:
// whole triangles, in order), each its own glDrawArrays from its own first vertex (vitaGL moves the vertex streams by
// `first`, so every run's indices start at 0).
constexpr size_t kVitaDrawVerts = 0xC000;
static_assert(kVitaDrawVerts % 3 == 0, "a run is whole triangles");
#endif

// glDrawArrays(GL_TRIANGLES, first, count) as the host's GL can take it (above: the Vita's runs)
void DrawTriangles(GLint first, size_t count) {
#if PS2R_HOST_VITA
    if (count > kVitaDrawVerts) {
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr, "[gl] a draw of %zu vertices: drawn in runs of %zu (vitaGL's glDrawArrays limit)\n", count, kVitaDrawVerts);
        }
        for (size_t at = 0; at < count; at += kVitaDrawVerts) {
            glDrawArrays(GL_TRIANGLES, first + static_cast<GLint>(at), static_cast<GLsizei>(std::min(count - at, kVitaDrawVerts)));
            Census().gl_draws++;
        }
        return;
    }
#endif
    glDrawArrays(GL_TRIANGLES, first, static_cast<GLsizei>(count));
    Census().gl_draws++;
}

bool Is16BitFrame(uint8_t psm) { return psm == 0x02 || psm == 0x0A; }

float SnapColumn8(float u) { return std::nearbyint(u / 8.0f) * 8.0f; }
float SnapRow(float v) {
    const float s = std::nearbyint(v / 8.0f) * 8.0f;
    return s + std::nearbyint(v - s);
}

}  // namespace

void GlDrawExecutor::DrawMeshTriangles(uint32_t first, size_t count) {
#if PS2R_HOST_VITA
    if (mesh_ibo_) {
        // the indices of the list's range (the vertex array holds the index buffer: GlDevice::CreateMesh)
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_SHORT,
                       reinterpret_cast<const void*>(uintptr_t(first) * sizeof(uint16_t)));
        Census().gl_draws++;
        return;
    }
#endif
    DrawTriangles(static_cast<GLint>(first), count);
}

GlDrawExecutor::GlDrawExecutor(GlDevice& device) : dev_(device) {}

bool GlDrawExecutor::BindPs2Program(const Step& s, bool again, bool mesh, uint32_t tex_w, uint32_t tex_h, uint32_t first) {
#if PS2R_HOST_VITA
    // The program the draw takes, bound once right before it (the uniforms were recorded without binding anything:
    // vitaGL's glUniform* writes the program that owns the location, and every glUseProgram makes the next draw upload
    // both of the bound program's default uniform buffers again): the fast program while every uniform it specializes
    // holds its constant (the choice re-made only when one of those values changed), else the full one; then that
    // program's texture-size uniforms (the full program's te_size_*; the fast program has none) and the values it does
    // not hold yet (GlUniformCache::Sync).
    const GlShaderLibrary& sh = dev_.Shaders();
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    // (the fill census: this draw's vertex work -- an indexed mesh shades about its distinct vertices)
    FillCensusVerts(unsigned(double(std::min<size_t>(s.tri_verts, 0xFFFFFFFFu)) * (mesh && mesh_ibo_ ? double(mesh_shaded_) : 1.0)));
    if (legacy_) {
        // the sequence before the drawing thread's rework: the full program's sampler sizes every draw but a run's again-draw, the variant
        // chosen every draw
        if (!again) {
            st.UseProgram(sh.Ps2Program());
            st.ApplyVitaSamplerSizes();
        }
        bool fast = sh.Ps2FastEnabled();
        if (fast) {
            int n = 0;
            const GlShaderLibrary::FastConstant* fc = GlShaderLibrary::FastConstants(&n);
            const GlUniformLocations& L = sh.Locations();
            for (int i = 0; i < n && fast; ++i) fast = u.Holds1i(L.*(fc[i].field), fc[i].value);
        }
        st.UseProgram(fast ? sh.Ps2FastProgram() : sh.Ps2Program());
        u.Sync(fast ? 1 : 0);   // (a no-op: the legacy sequence sends at once)
        if (fast) Census().draws_ps2_fast++;
        else Census().draws_ps2_full++;
        return false;
    }
    // the lean programs (GlShaderLibrary::SelectPs2: a lean program for the draw's state, else the fast or the full one --
    // chosen from the uniform cache's values and bound; a lean program's uniforms are sent by SelectPs2), then this
    // cache's values for the fast / full program it may have fallen back to. A native mesh draw may be made raw: SelectPs2
    // submits it itself when its program is a mesh-path one with a GXM twin (nothing bound, nothing more to draw).
    if (mesh && raw_mesh_ && mesh_vbo_ && sh.RawMeshEnabled()) {
        GlShaderLibrary::RawMesh raw;
        raw.vbo = mesh_vbo_;
        raw.ibo = mesh_ibo_;   // an indexed mesh: its indices (first / count then index the list)
        const uint32_t t0 = st.Tex2DOnUnit(0);
        raw.texture = t0 == GlStateCache::kUnknown ? 0 : t0;
        raw.tex_w = tex_w;
        raw.tex_h = tex_h;
        raw.first = first;   // a mesh sub-range (MeshDrawParams::first: a chunked instance's part): its stream starts there
        raw.count = uint32_t(std::min<size_t>(s.tri_verts, 0xFFFFFFFFu));
        bool drew = false;
        const uint32_t prog = sh.SelectPs2(st, u, s.bb_all, s.render_w, s.render_h, &raw, &drew);
        if (drew) {
            Census().gl_draws++;   // (one draw of the frame, made without vitaGL)
            return true;
        }
        u.Sync(prog == sh.Ps2FastProgram() ? 1 : prog == sh.Ps2Program() ? 0 : -1);
        return false;
    }
    const uint32_t prog = sh.SelectPs2(st, u, s.bb_all, s.render_w, s.render_h);
    u.Sync(prog == sh.Ps2FastProgram() ? 1 : prog == sh.Ps2Program() ? 0 : -1);
    return false;
#else
    (void)s;
    (void)again;
    (void)mesh;
    (void)tex_w;
    (void)tex_h;
    (void)first;
    return false;
#endif
}

#if PS2R_HOST_VITA
bool GlDrawExecutor::RawVertexDraws() const { return !legacy_ && raw_mesh_ && dev_.Shaders().RawVertsEnabled(); }

bool GlDrawExecutor::BindPs2RawVertices(const Step& s, uint32_t tex_w, uint32_t tex_h) {
    const GlShaderLibrary& sh = dev_.Shaders();
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    // (the fill census: this draw's vertex work, as BindPs2Program gives a vertex draw's)
    FillCensusVerts(unsigned(std::min<size_t>(s.tri_verts, 0xFFFFFFFFu)));
    GlShaderLibrary::RawMesh raw;
    raw.vertices = true;
    raw.vbo = dev_.Vertices().Buffer();
    const uint32_t t0 = st.Tex2DOnUnit(0);
    raw.texture = t0 == GlStateCache::kUnknown ? 0 : t0;
    raw.tex_w = tex_w;
    raw.tex_h = tex_h;
    raw.first = uint32_t(std::max(0, s.first));
    raw.count = uint32_t(std::min<size_t>(s.tri_verts, 0xFFFFFFFFu));
    bool drew = false;
    const uint32_t prog = sh.SelectPs2(st, u, s.bb_all, s.render_w, s.render_h, &raw, &drew);
    if (drew) {
        // (one draw of the frame per run of vitaGL's index buffer, made without vitaGL: DrawTriangles' count)
        Census().gl_draws += (s.tri_verts + kVitaDrawVerts - 1) / kVitaDrawVerts;
        return true;
    }
    u.Sync(prog == sh.Ps2FastProgram() ? 1 : prog == sh.Ps2Program() ? 0 : -1);
    return false;
}
#endif

// FRAME.FBMSK is an RGBA8-layout mask for every frame format; 16-bit frames use the
// top 5 bits of each channel and bit 31 for A, folded to R[0:4] G[5:9] B[10:14] A[15].
uint32_t GlDrawExecutor::Fbmsk16From32(uint32_t m) {
    return ((m >> 3) & 0x1F) | ((m >> 6) & 0x3E0) | ((m >> 9) & 0x7C00) | ((m >> 16) & 0x8000);
}

// A 5551 mask whose R/G/B fields are each all-0 or all-1 is expressible as a
// per-channel colour mask; anything else must merge bit-exactly in the shader.
bool GlDrawExecutor::Fbmsk16Exact(uint32_t fbmsk16) {
    auto aligned = [](uint32_t v, uint32_t field) {
        const uint32_t f = v & field;
        return f == 0 || f == field;
    };
    const bool channel_aligned =
        aligned(fbmsk16, 0x001F) && aligned(fbmsk16, 0x03E0) && aligned(fbmsk16, 0x7C00);
    return !channel_aligned && fbmsk16 != 0;
}

// Blended reads of another target whose ALPHA maps EXACTLY to fixed-function blending
// (the eight combos below) skip the destination copy; every other Cd-reading form,
// C = Ad, PABE, COLCLAMP=0, exact 5551 masks, DATE and FBA stay on the shader blend.
bool GlDrawExecutor::RouteRtBlend(const gs::DrawIr& ir, bool using_rt, bool fbmsk16_exact) {
    const auto& r = ir.raster;
    if (!(ir.alpha_blend && using_rt && !r.fba && !fbmsk16_exact && !r.test.date && !r.pabe &&
          r.colclamp))
        return false;
    const uint8_t a = r.alpha.a, b = r.alpha.b, c = r.alpha.c, d = r.alpha.d;
    auto is = [&](uint8_t A, uint8_t B, uint8_t C, uint8_t D) {
        return a == A && b == B && c == C && d == D;
    };
    return is(0, 1, 0, 1) || is(0, 2, 0, 1) || is(0, 2, 0, 2) || is(0, 1, 2, 1) ||
           is(0, 2, 2, 1) || is(1, 0, 0, 0) || is(0, 2, 2, 2) || is(2, 1, 0, 1);
}

// TEX1 MMAG/MMIN -> GL filters. With mip levels uploaded the trilinear forms are
// used for MMIN 2/4 as well (the shader selects the LOD); without levels the mip
// modes fall back to their base filter.
void GlDrawExecutor::SelectFilters(bool mag_linear, uint8_t mmin, uint8_t levels, uint32_t* mag,
                                   uint32_t* min) {
    *mag = mag_linear ? GL_LINEAR : GL_NEAREST;
    if (levels > 0) {
        switch (mmin) {
            case 0: *min = GL_NEAREST; break;
            case 1: *min = GL_LINEAR; break;
            case 2: *min = GL_NEAREST_MIPMAP_LINEAR; break;
            case 3: *min = GL_NEAREST_MIPMAP_LINEAR; break;
            case 4: *min = GL_LINEAR_MIPMAP_LINEAR; break;
            case 5: *min = GL_LINEAR_MIPMAP_LINEAR; break;
            default: *min = GL_NEAREST; break;
        }
    } else {
        switch (mmin) {
            case 1: case 4: case 5: *min = GL_LINEAR; break;
            default: *min = GL_NEAREST; break;
        }
    }
}

bool GlDrawExecutor::EnhanceFilters(TextureFilter filter, int anisotropy, int max_anisotropy,
                                    bool title_mips, uint8_t levels, uint32_t* mag, uint32_t* min,
                                    int* applied_anisotropy) {
    const int aniso = std::min(TextureFilterSelection::ClampAnisotropy(anisotropy), std::max(1, max_anisotropy));
    *applied_anisotropy = aniso;
    const bool chain = levels > 0;
    const bool hw_lod = chain && (aniso > 1 || filter == TextureFilter::Trilinear);
    const bool mips = chain && (title_mips || hw_lod);
    switch (filter) {
        case TextureFilter::Title:
            break;
        case TextureFilter::Point:
            *mag = GL_NEAREST;
            *min = mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
            break;
        case TextureFilter::Bilinear:
            *mag = GL_LINEAR;
            *min = mips ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
            break;
        case TextureFilter::Trilinear:
            *mag = GL_LINEAR;
            *min = chain ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
            break;
    }
    return hw_lod;
}

DrawResult GlDrawExecutor::Draw(const gs::DrawIr& ir, const gs::TextureSource& tex,
                                const DrawParams& p) {
    if (p.program == StencilProgram::StripCount) return DrawStripCount(ir, tex, p);
    if (p.program == StencilProgram::Invert) return DrawInvert(ir, tex, p);
    Step s;
    DrawResult none;
    const size_t n = ir.verts.size();
    if (n < 3) return none;
    if (!BindTarget(ir, p, s)) return none;
    FrameClear(p);
    BindTextures(tex, p, s);
    if (!ConvertVertices(ir, tex, s)) return none;
    Census().draws_converted++;
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    if (BindFullFirst()) st.UseProgram(dev_.Shaders().Ps2Program());   // (the Vita sets uniforms unbound, binds the draw's program last)
    // the vertices as they are (a native mesh draw leaves its transform on: cached, so a run of these sets nothing)
    u.Set1i(L.native, 0);
    u.Set1i(L.uv_mode, 0);
    u.Set2f(L.screen_size, static_cast<float>(s.render_w), static_cast<float>(s.render_h));
    TextureUniforms(ir, tex, p, s);
    RegisterUniforms(ir, p);
    ClassifyRoutes(ir, tex, p, s);
    BlendState(ir, p, s);
    DepthState(ir, p, s);
    ScissorState(ir, s);
    dev_.Vertices().BindForDraw(st);
    s.first = dev_.Vertices().Upload(scratch_.data(), s.tri_verts);
    ColorMaskState(ir, s);
    // The caller's stencil test over the depth attachment (a DepthStencil24_8 surface: the
    // typed-draw surface's shadow-volume counter); the register program itself never touches
    // the stencil, so the state is set around this one draw and reset after it.
    if (p.stencil_state.enabled) GlDevice::ApplyStencil(p.stencil_state);
    BindPs2Program(s);
    DrawTriangles(s.first, s.tri_verts);
    if (p.stencil_state.enabled) GlDevice::ResetStencil();
    // Masks return to their open defaults after every draw, so any raw GL pass
    // that follows (clear, refresh, present) inherits writable colour and depth.
    // (The Vita leaves them: every draw and pass sets the masks it needs -- each glDepthMask there is two GXM calls and
    // each glColorMask a fragment program re-patch at the next draw.)
    if (ResetMasksAfter()) {
        st.ColorMask(true, true, true, true);
        st.DepthMask(true);
    }
    return MakeResult(ir, p, s);
}

// A native mesh: Draw's state sequence over the mesh's buffer, the vertices transformed by the shader (u_native) rather
// than converted and uploaded; no screen box is known, so the copy and write rectangles are the whole target
#if PS2R_HOST_VITA
namespace {
// THE FULL PATHS' CPU SPLIT, one draw in 16 of each path timed (the device's census line "full draws' CPU
// split"): [path][0] the state set-up (target, textures, uniforms, blend / depth / scissor), [1] the upload (vertex draws),
// [2] the program (BindPs2Program: the lean selection + the raw GXM draw when it takes one), [3] the GL draw and the rest
struct SplitTimer {
    bool on = false;
    uint64_t t = 0;
    int path = 0, k = 0;
    SplitTimer(int path_, uint32_t& tick) : path(path_) {
        on = (++tick & 15u) == 0;
        if (on) t = sceKernelGetProcessTimeWide();
    }
    void Mark() {
        if (!on) return;
        const uint64_t now = sceKernelGetProcessTimeWide();
        Census().split_us[path][k < 3 ? k : 3] += now - t;
        t = now;
        ++k;
    }
    void Skip() { ++k; }
    ~SplitTimer() {
        if (!on) return;
        Census().split_us[path][3] += sceKernelGetProcessTimeWide() - t;
        Census().split_n[path]++;
    }
};
uint32_t g_split_tick[2] = {0, 0};
}  // namespace
#define TE_SPLIT(path) SplitTimer split_(path, g_split_tick[path])
#define TE_SPLIT_MARK() split_.Mark()
#define TE_SPLIT_SKIP() split_.Skip()
#else
#define TE_SPLIT(path) do {} while (0)
#define TE_SPLIT_MARK() do {} while (0)
#define TE_SPLIT_SKIP() do {} while (0)
#endif

DrawResult GlDrawExecutor::DrawMesh(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, uint32_t vao,
                                    size_t count, const MeshDrawParams& mp) {
    Step s;
    DrawResult none;
    if (count < 3 || !dev_.Shaders().NativeMesh()) return none;
    TE_SPLIT(0);
    if (!BindTarget(ir, p, s)) return none;
    Census().mesh_full++;
    const uint64_t state0 = dev_.State().Mutations();
    FrameClear(p);
    BindTextures(tex, p, s);
    const auto& t = tex.transform;
    const bool textured = tex.enabled && t.uv_tex_w > 0 && t.uv_tex_h > 0;
    for (int k = 0; k < 4; ++k) s.bb_all[k] = s.bb_tri[k] = mp.box[k];
    s.bb_valid = true;
    s.tri_verts = count / 3 * 3;
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    if (BindFullFirst()) st.UseProgram(dev_.Shaders().Ps2Program());
    u.Set2f(L.screen_size, static_cast<float>(s.render_w), static_cast<float>(s.render_h));
    TextureUniforms(ir, tex, p, s);
    RegisterUniforms(ir, p);
    ClassifyRoutes(ir, tex, p, s);
    {
        // the destination copy's rectangle (ClassifyRoutes takes it from the vertices, which a mesh draw has none of here):
        // the box, inside the scissor and the target
        const auto& sc = ir.raster.scissor;
        int rx0 = std::max(static_cast<int>(std::floor(mp.box[0])), static_cast<int>(sc.x0));
        int ry0 = std::max(static_cast<int>(std::floor(mp.box[1])), static_cast<int>(sc.y0));
        int rx1 = std::min(static_cast<int>(std::ceil(mp.box[2])), static_cast<int>(sc.x1) + 1);
        int ry1 = std::min(static_cast<int>(std::ceil(mp.box[3])), static_cast<int>(sc.y1) + 1);
        rx0 = std::max(0, std::min(rx0, p.target_w));
        ry0 = std::max(0, std::min(ry0, p.target_h));
        rx1 = std::max(0, std::min(rx1, p.target_w));
        ry1 = std::max(0, std::min(ry1, p.target_h));
        if (rx1 > rx0 && ry1 > ry0 && !s.idx_self_serve) {
            s.copy_x = rx0; s.copy_y = ry0; s.copy_w = rx1 - rx0; s.copy_h = ry1 - ry0;
        }
    }
    BlendState(ir, p, s);
    DepthState(ir, p, s);
    ScissorState(ir, s);
    // the destination copy and the shader blend's set-up above may have used other programs: the PS2 program again,
    // with the mesh's transform, before its draw
    if (BindFullFirst()) st.UseProgram(dev_.Shaders().Ps2Program());
    u.Set1i(L.native, 1);
    u.Set1i(L.uv_mode, textured ? 2 : 3);
    if (textured) u.Set2f(L.uv_scale, static_cast<float>(t.uv_pre_w), static_cast<float>(t.uv_pre_h));
    MeshUniforms(mp);
    st.BindVertexArray(vao);
    ColorMaskState(ir, s);
    if (p.stencil_state.enabled) GlDevice::ApplyStencil(p.stencil_state);
    run_color_mask_ = st.ColorMaskBits();
    run_depth_mask_ = st.DepthMaskBit();
    TE_SPLIT_MARK();   // (the state set-up)
    TE_SPLIT_SKIP();   // (no upload)
    // (the Vita: or drawn raw -- from the range's first vertex, as the GL draw)
    const bool drew_raw = BindPs2Program(s, false, true, 0, 0, mp.first);
    TE_SPLIT_MARK();   // (the program, the raw draw)
    if (!drew_raw) DrawMeshTriangles(mp.first, s.tri_verts);
    if (p.stencil_state.enabled) GlDevice::ResetStencil();
    if (ResetMasksAfter()) {
        st.ColorMask(true, true, true, true);
        st.DepthMask(true);
    }
    // the transform and the mesh's vertex array stay bound for the next mesh draw: a plain draw turns the transform off
    // and binds the stream's own array
    // another mesh may take this state as it stands (no destination copy, shader blend, stencil or clear of its own)
    run_valid_ = !s.needs_rt_copy && !s.sw_blend_active && !s.cd_copied && !p.stencil_state.enabled && !p.do_frame_clear &&
                 run_color_mask_ >= 0 && run_depth_mask_ >= 0;
#if PS2R_HOST_VITA
    pkt_valid_ = run_valid_;
#endif
    run_textured_ = textured && s.has_texture && !s.using_rt && tex.owner.kind == gs::TextureSource::OwnerKind::Decoded &&
                    tex.transform.idx_fmt == 0 && tex.transform.depth_fmt == 0 && !tex.transform.region.applied;
    if (run_valid_) {
        run_step_ = s;
        run_ir_ = &ir;
        run_params_ = p;
        run_state_ = st.Mutations();
        run_uniforms_ = u.Mutations();
#if PS2R_HOST_VITA
        run_ctx_ = st.Snapshot();
        run_ctx_known_ = GlStateCache::Known(run_ctx_);
        pkt_ctx_mut_ = ~0ull;
#endif
    }
    Census().state_by_path[0] += dev_.State().Mutations() - state0;
    return MakeResult(ir, p, s);
}

void GlDrawExecutor::MeshUniforms(const MeshDrawParams& mp) {
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    float mesh[20];
    std::memcpy(mesh, mp.screen, sizeof mp.screen);
    mesh[16] = mp.offset[0];
    mesh[17] = mp.offset[1];
    mesh[18] = mp.scroll[0];
    mesh[19] = mp.scroll[1];
    u.Set4fv(L.mesh, 5, mesh);
    u.Set1i(L.mesh_lit, mp.lit ? 1 : 0);
    if (mp.lit) {
        float light[28] = {};
        for (int l = 0; l < 3; ++l)
            for (int k = 0; k < 3; ++k) {
                light[l * 4 + k] = mp.light_dir[l][k];
                light[(3 + l) * 4 + k] = mp.light_colour[l][k];
            }
        for (int k = 0; k < 3; ++k) light[24 + k] = mp.ambient[k];
        u.Set4fv(L.mesh_light, 7, light);
    }
}

DrawResult GlDrawExecutor::DrawMeshAgain(uint32_t vao, size_t count, const MeshDrawParams& mp) {
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    if (!run_valid_ || count < 3 || st.Mutations() != run_state_ || u.Mutations() != run_uniforms_) return DrawResult{};
    Census().mesh_again++;
    const uint64_t state0 = st.Mutations();
    Step s = run_step_;
    for (int k = 0; k < 4; ++k) s.bb_all[k] = s.bb_tri[k] = mp.box[k];
    s.tri_verts = count / 3 * 3;
    MeshUniforms(mp);
    st.BindVertexArray(vao);
    // Nothing moved in the caches since the run's draw (the counts above): its program, textures, sampler sizes, blend,
    // depth test, scissor and their uniforms are as it left them; only the two masks it reset to the open defaults after
    // drawing are put back (ColorMaskState / DepthState would set the same values and find the rest cached)
    st.ColorMask((run_color_mask_ & 1) != 0, (run_color_mask_ & 2) != 0, (run_color_mask_ & 4) != 0, (run_color_mask_ & 8) != 0);
    st.DepthMask(run_depth_mask_ != 0);
    // (the Vita: or drawn raw -- from the range's first vertex, as the GL draw)
    if (!BindPs2Program(s, true, true, 0, 0, mp.first)) DrawMeshTriangles(mp.first, s.tri_verts);
    if (ResetMasksAfter()) {
        st.ColorMask(true, true, true, true);
        st.DepthMask(true);
    }
    run_state_ = st.Mutations();
    run_uniforms_ = u.Mutations();
#if PS2R_HOST_VITA
    run_ctx_ = st.Snapshot();
    run_ctx_known_ = GlStateCache::Known(run_ctx_);
    pkt_ctx_mut_ = ~0ull;
#endif
    Census().state_by_path[1] += st.Mutations() - state0;
    return MakeResult(*run_ir_, run_params_, s);
}

// The run's state with another texture. Everything DrawMesh sets apart from the texture's part is a function of the
// record's raster state and the target, which a texture change leaves as they were: the target, the register uniforms,
// the routes (an owned decoded texture is never a render-target read, so the blend, the destination copy and the colour
// mask do not depend on which one it is), the blend, depth and scissor state, the program. Nothing moved in the caches
// since the run's draw (the counts), so those stand as it left them; the texture's binding and parameters, its uniforms,
// the UV prescale and the sampler sizes are set as DrawMesh sets them, then the transform and the draw as DrawMeshAgain.
DrawResult GlDrawExecutor::DrawMeshTexture(const gs::DrawIr& ir, const gs::TextureSource& tex, uint32_t vao, size_t count,
                                           const MeshDrawParams& mp) {
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    if (!run_valid_ || !run_textured_ || count < 3 || st.Mutations() != run_state_ || u.Mutations() != run_uniforms_)
        return DrawResult{};
    const auto& t = tex.transform;
    const GlSurface* owner = dev_.Surface(tex.owner.surface);
    if (!tex.enabled || tex.owner.kind != gs::TextureSource::OwnerKind::Decoded || !owner || !owner->tex || t.uv_tex_w <= 0 ||
        t.uv_tex_h <= 0 || t.idx_fmt != 0 || t.depth_fmt != 0 || t.region.applied || t.is_shuffle)
        return DrawResult{};
    Census().mesh_texture++;
    const uint64_t state0 = st.Mutations();
    Step s = run_step_;
    BindTextures(tex, run_params_, s);
    TextureUniforms(ir, tex, run_params_, s);
    // the record's alpha test may differ from the run's (ps2e_draw's swap level 2): its register uniforms (memoised: a
    // record whose values are the run's sends nothing)
    RegisterUniforms(ir, run_params_);
    const GlUniformLocations& L = dev_.Shaders().Locations();
    u.Set2f(L.uv_scale, static_cast<float>(t.uv_pre_w), static_cast<float>(t.uv_pre_h));
    for (int k = 0; k < 4; ++k) s.bb_all[k] = s.bb_tri[k] = mp.box[k];
    s.tri_verts = count / 3 * 3;
    MeshUniforms(mp);
    st.BindVertexArray(vao);
    st.ColorMask((run_color_mask_ & 1) != 0, (run_color_mask_ & 2) != 0, (run_color_mask_ & 4) != 0, (run_color_mask_ & 8) != 0);
    st.DepthMask(run_depth_mask_ != 0);
    // (the Vita: BindPs2Program binds the variant and, for the full program, its sampler sizes -- or draws it raw, from
    // the range's first vertex as the GL draw; the texture's size checks vitaGL's texture record before the first raw draw
    // that samples one)
    if (!BindPs2Program(s, false, true, owner->desc.width, owner->desc.height, mp.first))
        DrawMeshTriangles(mp.first, s.tri_verts);
    if (ResetMasksAfter()) {
        st.ColorMask(true, true, true, true);
        st.DepthMask(true);
    }
    run_step_ = s;
    run_ir_ = &ir;
    run_state_ = st.Mutations();
    run_uniforms_ = u.Mutations();
#if PS2R_HOST_VITA
    run_ctx_ = st.Snapshot();
    run_ctx_known_ = GlStateCache::Known(run_ctx_);
    pkt_ctx_mut_ = ~0ull;
#endif
    Census().state_by_path[2] += st.Mutations() - state0;
    return MakeResult(ir, run_params_, s);
}

#if PS2R_HOST_VITA
// A packet's replay. The run's draws left every GL state and uniform the packet's state takes as they are (the caches'
// counts unmoved since the run's last draw: no GL call was made since -- a replay makes none either), so vitaGL's GXM
// context (depth, stencil, cull, viewport, the scene, the scissor applied at scene_reset) is the context the slow path
// would draw this state in; the packet supplies the rest exactly as the raw draw that recorded it supplied it.
bool GlDrawExecutor::DrawMeshPacket(uint32_t packet, uint32_t vbo, uint32_t ibo, float shaded, size_t count, const MeshDrawParams& mp) {
    if (legacy_ || !raw_mesh_) {
        Census().packet_declined[0]++;
        return false;
    }
    const GlStateCache& st = dev_.State();
    if (!pkt_valid_) {
        Census().packet_declined[2]++;
        return false;
    }
    // (the uniform cache does not matter: a packet's programs read their default uniform buffers alone, which the replay
    // writes whole; the GL program bound, the arrays and the units other than 0 are vitaGL's draws' own, re-set by them)
    const uint64_t mut = st.Mutations();
    bool across = mut != run_state_;
    if (mut != run_state_ && mut != pkt_ctx_mut_) {
        // GL calls since the run's last slow draw -- a vertex draw of the run's own state, by the caller's account: the
        // fixed-function context as the cache records it must be the run's, field for field
        const GlStateCache::Context now = st.Snapshot();
        if (!run_ctx_known_ || !GlStateCache::Known(now) || std::memcmp(&now, &run_ctx_, sizeof now) != 0) {
            Census().packet_declined[2]++;
            return false;
        }
        pkt_ctx_mut_ = mut;
    }
    if (count < 3) {
        Census().packet_declined[7]++;
        return false;
    }
    // (the fill census: the replay's vertex work, as BindPs2Program gives the slow path's -- an indexed mesh shades about its
    // distinct vertices; a declined replay's slow path sets it again)
    FillCensusVerts(unsigned(double(std::min<size_t>(count / 3 * 3, 0xFFFFFFFFu)) * (ibo ? double(shaded) : 1.0)));
    const int why = dev_.Shaders().DrawRawPacket(packet, vbo, ibo, mp.first, static_cast<uint32_t>(count / 3 * 3), mp,
                                                 run_step_.render_w, run_step_.render_h);
    if (why >= 0) {
        Census().packet_declined[why < 8 ? why : 7]++;
        return false;
    }
    Census().mesh_packet++;
    if (across) Census().packet_across++;
    Census().gl_draws++;   // (one draw of the frame, made without vitaGL)
    return true;
}

// An instanced run of a packet's replays: DrawMeshPacket's conditions (the run's packets drawable, its
// fixed-function context as its last draw left it), then the library's instanced replay. The census counts each instance
// as the packet draw it stands for.
bool GlDrawExecutor::DrawMeshPacketInstances(uint32_t packet, uint32_t vbo, uint32_t ibo, size_t count, uint32_t first,
                                             const MeshInstance* inst, int n) {
    if (legacy_ || !raw_mesh_ || !pkt_valid_ || n < 2) {
        Census().instanced_declined++;
        return false;
    }
    const GlStateCache& st = dev_.State();
    const uint64_t mut = st.Mutations();
    const bool across = mut != run_state_;
    if (mut != run_state_ && mut != pkt_ctx_mut_) {
        const GlStateCache::Context now = st.Snapshot();
        if (!run_ctx_known_ || !GlStateCache::Known(now) || std::memcmp(&now, &run_ctx_, sizeof now) != 0) {
            Census().instanced_declined++;
            return false;
        }
        pkt_ctx_mut_ = mut;
    }
    if (count < 3) {
        Census().instanced_declined++;
        return false;
    }
    const int why = dev_.Shaders().DrawRawPacketInstances(packet, vbo, ibo, first, static_cast<uint32_t>(count / 3 * 3), inst, n,
                                                          run_step_.render_w, run_step_.render_h);
    if (why >= 0) {
        Census().instanced_declined++;
        return false;
    }
    Census().mesh_packet += uint64_t(n);
    if (across) Census().packet_across += uint64_t(n);
    Census().gl_draws++;   // (one draw of the frame, made without vitaGL)
    Census().instanced_draws++;
    Census().instanced_instances += uint64_t(n);
    return true;
}

// A skin packet's replay: DrawMeshPacket's conditions (the run's packets drawable, its fixed-function context as its last
// draw left it -- a skin of the run's context sets that very context on its full path), then the library's replay
bool GlDrawExecutor::DrawSkinPacket(uint32_t packet, uint32_t vbo, uint32_t ibo, float shaded, size_t count, const MeshDrawParams& mp,
                                    const float* bones, int joints, const float* disp, int disp_count) {
    if (legacy_ || !raw_mesh_ || !pkt_valid_) {
        Census().skin_packet_declined++;
        return false;
    }
    const GlStateCache& st = dev_.State();
    const uint64_t mut = st.Mutations();
    if (mut != run_state_ && mut != pkt_ctx_mut_) {
        const GlStateCache::Context now = st.Snapshot();
        if (!run_ctx_known_ || !GlStateCache::Known(now) || std::memcmp(&now, &run_ctx_, sizeof now) != 0) {
            Census().skin_packet_declined++;
            return false;
        }
        pkt_ctx_mut_ = mut;
    }
    if (count < 3) {
        Census().skin_packet_declined++;
        return false;
    }
    FillCensusVerts(unsigned(double(std::min<size_t>(count / 3 * 3, 0xFFFFFFFFu)) * double(shaded)));
    const int why = dev_.Shaders().DrawSkinPacket(packet, vbo, ibo, static_cast<uint32_t>(count / 3 * 3), mp, bones, joints, disp,
                                                  disp_count, run_step_.render_w, run_step_.render_h);
    if (why >= 0) {
        Census().skin_packet_declined++;
        return false;
    }
    Census().skin_packet++;
    Census().skin_draws++;
    Census().skin_indices += count / 3 * 3;
    Census().gl_draws++;   // (one draw of the frame, made without vitaGL)
    return true;
}
#endif

// A GPU-skinned mesh (the Vita): DrawMesh's state sequence over the skin mesh's vertex array -- the target, the clear,
// the textures and their uniforms, the register uniforms, the routes, blend / depth / scissor, the mesh transform's
// uniforms (u_mesh / u_mesh_light as a lit native mesh has them) -- then the skin program for the draw's state
// (GlShaderLibrary::SelectSkin: the lean variant the full program's values select, with the skin vertex program), the
// palette (u_bones) and the indexed draw. No other mesh draw takes this state as its own afterwards (the vertex program
// and array differ): the run ends here.
DrawResult GlDrawExecutor::DrawSkinMesh(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p, uint32_t vao,
                                        size_t count, const MeshDrawParams& mp, const float* bones, int joints, const float* disp,
                                        int disp_count) {
    DrawResult none;
#if PS2R_HOST_VITA
    Step s;
    run_valid_ = false;
    if (count < 3 || !bones || joints <= 0 || joints > 64 || disp_count < 0 || disp_count > 64 || (disp_count && !disp) ||
        !dev_.Shaders().SkinBuilt())
        return none;
    if (!BindTarget(ir, p, s)) return none;
    const uint64_t state0 = dev_.State().Mutations();
    FrameClear(p);
    BindTextures(tex, p, s);
    const auto& t = tex.transform;
    const bool textured = tex.enabled && t.uv_tex_w > 0 && t.uv_tex_h > 0;
    for (int k = 0; k < 4; ++k) s.bb_all[k] = s.bb_tri[k] = mp.box[k];
    s.bb_valid = true;
    s.tri_verts = count / 3 * 3;
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    u.Set2f(L.screen_size, static_cast<float>(s.render_w), static_cast<float>(s.render_h));
    TextureUniforms(ir, tex, p, s);
    RegisterUniforms(ir, p);
    ClassifyRoutes(ir, tex, p, s);
    {
        // the destination copy's rectangle (as DrawMesh: the box, inside the scissor and the target)
        const auto& sc = ir.raster.scissor;
        int rx0 = std::max(static_cast<int>(std::floor(mp.box[0])), static_cast<int>(sc.x0));
        int ry0 = std::max(static_cast<int>(std::floor(mp.box[1])), static_cast<int>(sc.y0));
        int rx1 = std::min(static_cast<int>(std::ceil(mp.box[2])), static_cast<int>(sc.x1) + 1);
        int ry1 = std::min(static_cast<int>(std::ceil(mp.box[3])), static_cast<int>(sc.y1) + 1);
        rx0 = std::max(0, std::min(rx0, p.target_w));
        ry0 = std::max(0, std::min(ry0, p.target_h));
        rx1 = std::max(0, std::min(rx1, p.target_w));
        ry1 = std::max(0, std::min(ry1, p.target_h));
        if (rx1 > rx0 && ry1 > ry0 && !s.idx_self_serve) {
            s.copy_x = rx0; s.copy_y = ry0; s.copy_w = rx1 - rx0; s.copy_h = ry1 - ry0;
        }
    }
    BlendState(ir, p, s);
    DepthState(ir, p, s);
    ScissorState(ir, s);
    u.Set1i(L.native, 1);
    u.Set1i(L.uv_mode, textured ? 2 : 3);
    if (textured) u.Set2f(L.uv_scale, static_cast<float>(t.uv_pre_w), static_cast<float>(t.uv_pre_h));
    MeshUniforms(mp);
    st.BindVertexArray(vao);
    ColorMaskState(ir, s);
    if (p.stencil_state.enabled) GlDevice::ApplyStencil(p.stencil_state);
    FillCensusVerts(unsigned(double(std::min<size_t>(s.tri_verts, 0xFFFFFFFFu)) * double(mesh_shaded_)));   // (the fill census: ~ its distinct vertices)
    // the raw skin draw (GlShaderLibrary::RawSkin): the skin mesh's buffers, the texture on unit 0, the palette
    GlShaderLibrary::RawSkin raw;
    raw.vbo = skin_vbo_;
    raw.ibo = skin_ibo_;
    {
        const uint32_t t0 = st.Tex2DOnUnit(0);
        raw.texture = t0 == GlStateCache::kUnknown ? 0 : t0;
        const GlSurface* owner = textured ? dev_.Surface(tex.owner.surface) : nullptr;
        raw.tex_w = owner ? owner->desc.width : 0;
        raw.tex_h = owner ? owner->desc.height : 0;
    }
    raw.count = uint32_t(std::min<size_t>(s.tri_verts, 0xFFFFFFFFu));
    raw.bones = bones;
    raw.joints = joints;
    raw.disp = disp;
    raw.disp_count = disp_count;
    bool drew_raw = false;
    const bool may_raw = raw_mesh_ && !legacy_ && skin_vbo_ && skin_ibo_;
    const GlShaderLibrary::SkinProgram* prog =
        dev_.Shaders().SelectSkin(st, u, s.bb_all, s.render_w, s.render_h, may_raw ? &raw : nullptr, &drew_raw);
    if (!prog) {
        if (p.stencil_state.enabled) GlDevice::ResetStencil();
        Census().skin_refused++;
        return none;
    }
    if (!drew_raw) {
        glUniform4fv(prog->bones, joints * 3, bones);
        if (disp_count) {
            glUniform4fv(prog->disp, disp_count, disp);   // a blend skin's displacements
            prog->disp_dirty = true;
        } else if (prog->disp_dirty) {
            static const float zero[4] = {0.f, 0.f, 0.f, 0.f};
            glUniform4fv(prog->disp, 1, zero);            // a plain skin's vertices all read u_disp[0]
            prog->disp_dirty = false;
        }
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(s.tri_verts), GL_UNSIGNED_SHORT, nullptr);
    }
    Census().gl_draws++;
    Census().skin_draws++;
    Census().skin_indices += s.tri_verts;
    if (p.stencil_state.enabled) GlDevice::ResetStencil();
    Census().state_by_path[0] += dev_.State().Mutations() - state0;
    return MakeResult(ir, p, s);
#else
    (void)ir; (void)tex; (void)p; (void)vao; (void)count; (void)mp; (void)bones; (void)joints; (void)disp; (void)disp_count;
    return none;
#endif
}

// A raw draw takes what Draw's conversion would have done to the vertices to the shader: the S / T prescale (u_uv_mode 2,
// the same single multiply), Q 0 read as 1 (u_native 2); a texture that rebases or rescales its coordinates per vertex,
// a fogged or a stencil-program draw takes Draw
bool GlDrawExecutor::DrawsRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p) const {
    if (!dev_.Shaders().NativeMesh() || p.program != StencilProgram::None || ir.fog || ir.tex.fst) return false;
    const auto& t = tex.transform;
    return !t.region.applied && t.shuffle_uv_scale_u == 1.0f && !t.shuffle_snap;
}

DrawResult GlDrawExecutor::DrawRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& p,
                                   const RawVertex* verts, size_t n, const float* box) {
    Step s;
    DrawResult none;
    if (n < 3 || !DrawsRaw(ir, tex, p)) return none;
    TE_SPLIT(1);
    if (!BindTarget(ir, p, s)) return none;
    Census().draws_raw++;
    const uint64_t state0 = dev_.State().Mutations();
    FrameClear(p);
    BindTextures(tex, p, s);
    const auto& t = tex.transform;
    const bool textured = tex.enabled && t.uv_tex_w > 0 && t.uv_tex_h > 0;
    for (int k = 0; k < 4; ++k) s.bb_all[k] = s.bb_tri[k] = box[k];
    s.bb_valid = true;
    s.tri_verts = n / 3 * 3;
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    if (BindFullFirst()) st.UseProgram(dev_.Shaders().Ps2Program());
    u.Set1i(L.native, 2);
    u.Set1i(L.uv_mode, textured ? 2 : 3);
    if (textured) u.Set2f(L.uv_scale, static_cast<float>(t.uv_pre_w), static_cast<float>(t.uv_pre_h));
    u.Set2f(L.screen_size, static_cast<float>(s.render_w), static_cast<float>(s.render_h));
    TextureUniforms(ir, tex, p, s);
    RegisterUniforms(ir, p);
    ClassifyRoutes(ir, tex, p, s);
    BlendState(ir, p, s);
    DepthState(ir, p, s);
    ScissorState(ir, s);
    // the destination copy and the shader blend's set-up above may have used other programs
    if (BindFullFirst()) st.UseProgram(dev_.Shaders().Ps2Program());
#if PS2R_HOST_VITA
    if (RawVertexDraws()) {
        // THE RAW VERTEX DRAW: the vertices written straight into the ring's slice (GlVertexStream::ReserveRaw:
        // no GL call -- UploadRaw where it would re-specify the storage), then the lean program chosen as for the GL draw and,
        // when it is the raw vertex path's, the draw made through GXM from the ring (GlShaderLibrary::RawMesh::vertices) --
        // else the GL draw as before over the same ring range. The state set-up above and the stencil around the draw are the
        // GL draw's own; the arrays are bound only for a GL draw.
        TE_SPLIT_MARK();   // (the state set-up)
        const size_t bytes = s.tri_verts * sizeof(RawVertex);
        int first = 0;
        bool arrays = false;
        if (dev_.Vertices().InRing(verts, bytes, sizeof(RawVertex), &first)) {
            // the caller wrote them in place (Backend::ReserveRaw): drawn where they lie
            GlVertexStream::NoteInPlace(bytes);
        } else if (void* dst = dev_.Vertices().ReserveRaw(bytes, sizeof(RawVertex), &first)) {
            GlVertexStream::CopyIn(dst, verts, bytes);
            GlVertexStream::NoteCopied(bytes);
        } else {
            dev_.Vertices().BindRawForDraw(st);
            arrays = true;
            first = dev_.Vertices().UploadRaw(verts, bytes, sizeof(RawVertex));
        }
        s.first = first;
        TE_SPLIT_MARK();   // (the upload into the ring)
        ColorMaskState(ir, s);
        if (p.stencil_state.enabled) GlDevice::ApplyStencil(p.stencil_state);
        uint32_t tex_w = 0, tex_h = 0;
        if (textured && tex.owner.kind == gs::TextureSource::OwnerKind::Decoded) {
            if (const GlSurface* owner = dev_.Surface(tex.owner.surface)) {
                tex_w = owner->desc.width;
                tex_h = owner->desc.height;
            }
        }
        const bool drew = BindPs2RawVertices(s, tex_w, tex_h);
        TE_SPLIT_MARK();   // (the program, the raw draw)
        if (!drew) {
            if (!arrays) dev_.Vertices().BindRawForDraw(st);
            DrawTriangles(s.first, s.tri_verts);
        }
        if (p.stencil_state.enabled) GlDevice::ResetStencil();
        Census().state_by_path[3] += dev_.State().Mutations() - state0;
        return MakeResult(ir, p, s);
    }
#endif
    dev_.Vertices().BindRawForDraw(st);
    TE_SPLIT_MARK();   // (the state set-up)
    s.first = dev_.Vertices().UploadRaw(verts, s.tri_verts * sizeof(RawVertex), sizeof(RawVertex));
    TE_SPLIT_MARK();   // (the upload into the ring)
    ColorMaskState(ir, s);
    if (p.stencil_state.enabled) GlDevice::ApplyStencil(p.stencil_state);
    BindPs2Program(s);
    TE_SPLIT_MARK();   // (the program)
    DrawTriangles(s.first, s.tri_verts);
    if (p.stencil_state.enabled) GlDevice::ResetStencil();
    if (ResetMasksAfter()) {
        st.ColorMask(true, true, true, true);
        st.DepthMask(true);
    }
    Census().state_by_path[3] += dev_.State().Mutations() - state0;
    return MakeResult(ir, p, s);
}

// The strip-count pass: geometry into the stencil-only framebuffer; the shader
// discards on DATE (unit 1 = the ROUTED accumulator image, not attached here) and
// on the colour-authored Z compare (unit 4); the caller's stencil state counts the
// survivors; no colour output. Viewport = the logical render size (the accumulator
// is native-scale by the recipe's prime rule). The per-frame clear the target
// layer consumed for this draw still lands on the COLOUR target (bound first, as
// the reference strip ladder does) before the stencil-only framebuffer is bound;
// the scissor is issued at scale 1 (the prime rule pins the accumulator native).
DrawResult GlDrawExecutor::DrawStripCount(const gs::DrawIr& ir, const gs::TextureSource& tex,
                                          const DrawParams& p) {
    Step s;
    DrawResult none;
    if (ir.verts.size() < 3) return none;
    GlStateCache& st = dev_.State();
    if (p.do_frame_clear) {
        if (!BindTarget(ir, p, s)) return none;
        FrameClear(p);
    }
    st.Disable(GL_SCISSOR_TEST);
    s.render_w = p.render_w;
    s.render_h = p.render_h;
    s.scale = 1;
    s.fbo = dev_.BindStencilTarget(SurfaceId{}, p.stencil);
    if (!s.fbo) return none;
    st.Viewport(0, 0, s.render_w, s.render_h);
    st.UseProgram(dev_.Shaders().StencilStripProgram());
    const GlStencilLocations& SL = dev_.Shaders().StencilLocations();
    glUniform2f(SL.strip_screen, static_cast<float>(s.render_w), static_cast<float>(s.render_h));
    glUniform1i(SL.strip_zsrc_en, static_cast<int>(ir.raster.test.ztst));
    glUniform1i(SL.strip_zsrc_num, std::max<int>(1, p.zsrc_scale));
    glUniform1i(SL.strip_zsrc_den, std::max<int>(1, p.scale));
    glUniform1i(SL.strip_datm, ir.raster.test.datm ? 1 : 0);
    const GlSurface* routed = dev_.Surface(p.alt16_route && p.alt16.id ? p.alt16 : p.color);
    const GlSurface* zsrc = dev_.Surface(p.zsrc);
    st.Disable(GL_BLEND);
    st.ActiveTexture(GL_TEXTURE1);
    st.BindTex2D(routed && routed->tex ? routed->tex : dev_.DummyTexture());
    st.ActiveTexture(GL_TEXTURE4);
    st.BindTex2D(zsrc && zsrc->tex ? zsrc->tex : dev_.DummyTexture());
    st.ActiveTexture(GL_TEXTURE0);
    st.Enable(GL_DEPTH_TEST);
    st.DepthFunc(GL_ALWAYS);
    st.DepthMask(false);
    ScissorState(ir, s);
    if (!ConvertVertices(ir, tex, s)) return none;
    // The strip's FBMSK still classifies the draw for the coherence tail (a masked
    // 16-bit write keeps the uploaded assets under it and records its keep mask)
    // even though no colour is written here.
    if (Is16BitFrame(ir.raster.frame.psm) && ir.raster.frame.fbmsk != 0) {
        s.fbmsk16 = Fbmsk16From32(ir.raster.frame.fbmsk);
        s.fbmsk16_exact = Fbmsk16Exact(s.fbmsk16);
    }
    dev_.Vertices().BindForDraw(st);
    s.first = dev_.Vertices().Upload(scratch_.data(), s.tri_verts);
    st.ColorMask(false, false, false, false);
    GlDevice::ApplyStencil(p.stencil_state);
#if PS2R_HOST_VITA
    st.ApplyVitaSamplerSizes();
#endif
    DrawTriangles(s.first, s.tri_verts);
    GlDevice::ResetStencil();
    st.ColorMask(true, true, true, true);
    st.DepthMask(true);
    return MakeResult(ir, p, s);
}

// The folded invert: the target's Cd copy is refreshed over the WHOLE target (the
// self-read rule), bound on unit 1, and the program writes rgb = ~Cd, a = 255 -
// CSM1(idx) with FIX/128; blend off, depth ALWAYS without writes, all channels.
DrawResult GlDrawExecutor::DrawInvert(const gs::DrawIr& ir, const gs::TextureSource& tex,
                                      const DrawParams& p) {
    Step s;
    DrawResult none;
    if (ir.verts.size() < 3) return none;
    if (!BindTarget(ir, p, s)) return none;
    FrameClear(p);
    GlStateCache& st = dev_.State();
    st.UseProgram(dev_.Shaders().StencilInvertProgram());
    const GlStencilLocations& SL = dev_.Shaders().StencilLocations();
    glUniform2f(SL.invert_screen, static_cast<float>(s.render_w), static_cast<float>(s.render_h));
    glUniform1f(SL.invert_fix, ir.raster.alpha.fix / 128.0f);
    if (!ConvertVertices(ir, tex, s)) return none;
    s.idx_self_serve = true;
    s.copy_x = s.copy_y = s.copy_w = s.copy_h = 0;
    s.needs_rt_copy = true;
    CdCopy(p, s);
    if (!s.sw_blend_active) return none;   // no Cd copy on the target: the caller's Accept declines this
    st.Disable(GL_BLEND);
    st.ActiveTexture(GL_TEXTURE4);
    st.BindTex2D(dev_.DummyTexture());
    st.ActiveTexture(GL_TEXTURE0);
    st.Enable(GL_DEPTH_TEST);
    st.DepthFunc(GL_ALWAYS);
    st.DepthMask(false);
    ScissorState(ir, s);
    dev_.Vertices().BindForDraw(st);
    s.first = dev_.Vertices().Upload(scratch_.data(), s.tri_verts);
    st.ColorMask(true, true, true, true);
#if PS2R_HOST_VITA
    st.ApplyVitaSamplerSizes();
#endif
    DrawTriangles(s.first, s.tri_verts);
    st.DepthMask(true);
    return MakeResult(ir, p, s);
}

// Scissor off before any framebuffer operation; bind the main or companion
// attachment; viewport = physical size; u_screen_size stays logical so PS2 px -> NDC
// is unchanged and rasterisation scales through the viewport alone.
bool GlDrawExecutor::BindTarget(const gs::DrawIr& ir, const DrawParams& p, Step& s) {
    (void)ir;
    GlStateCache& st = dev_.State();
    st.Disable(GL_SCISSOR_TEST);
    s.render_w = p.render_w;
    s.render_h = p.render_h;
    s.scale = std::max<int>(1, p.scale);
    const SurfaceId color = p.alt16_route && p.alt16.id ? p.alt16 : p.color;
    s.fbo = dev_.BindTarget(color, p.depth);
    if (!s.fbo) return false;
    st.Viewport(0, 0, s.render_w * s.scale, s.render_h * s.scale);
    return true;
}

// Per-game-frame clear: depth always (to 0.0 = GS far plane), colour only on the
// scanout target (transparent black).
void GlDrawExecutor::FrameClear(const DrawParams& p) {
    if (!p.do_frame_clear) return;
    GlStateCache& st = dev_.State();
    st.DepthMask(true);
    glClearDepth(0.0);
    GLbitfield bits = GL_DEPTH_BUFFER_BIT;
    if (p.clear_color) {
        glClearColor(0.f, 0.f, 0.f, 0.f);
        st.ColorMask(true, true, true, true);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    glClear(bits);
}

void GlDrawExecutor::BindTextures(const gs::TextureSource& tex, const DrawParams& p, Step& s) {
    using Kind = gs::TextureSource::OwnerKind;
    GlStateCache& st = dev_.State();
    const GlSurface* owner = dev_.Surface(tex.owner.surface);
    s.using_rt = tex.enabled && tex.owner.kind == Kind::Surface && owner && owner->tex;
    s.has_texture = tex.enabled && owner && owner->tex &&
                    (tex.owner.kind == Kind::Surface || tex.owner.kind == Kind::Decoded);
    st.ActiveTexture(GL_TEXTURE0);
    if (s.has_texture) {
        st.BindTex2D(owner->tex);
        if (s.using_rt && tex.transform.is_shuffle) {
            st.TexParami(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            st.TexParami(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        // CLAMP register: 0 REPEAT, 1 CLAMP, 2 REGION_CLAMP (as CLAMP), 3 REGION_REPEAT (as REPEAT);
        // render-target content never tiles, every other texture wraps per wms/wmt.
        auto wrap = [](uint8_t mode) -> GLint {
            return (mode == 0 || mode == 3) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        };
        if (s.using_rt) {
            st.TexParami(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            st.TexParami(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            st.TexParami(GL_TEXTURE_WRAP_S, wrap(tex.sampler.wms));
            st.TexParami(GL_TEXTURE_WRAP_T, wrap(tex.sampler.wmt));
        }
        uint32_t mag = GL_NEAREST, min = GL_NEAREST;
        const uint8_t levels = s.using_rt ? 0 : owner->levels;
        SelectFilters(tex.sampler.mag_linear, tex.sampler.mmin, levels, &mag, &min);
        // The player's enhancement (video.texture_filter / video.anisotropy) over
        // the OWNED textures only: the title's TEX1 choice above is the baseline
        // (and the whole story under `title` / anisotropy 1); render-target reads
        // and the VRAM-decoded model textures keep the title's sampler.
        s.hw_lod = false;
        if (tex.owner.owned && !s.using_rt) {
            const TextureFilterSelection* sel = dev_.TextureFilter();
            const TextureFilter mode = sel ? sel->Filter() : TextureFilter::Title;
            const int requested = sel ? sel->Anisotropy() : 1;
            const uint32_t title_mag = mag, title_min = min;
            int aniso = 1;
            s.hw_lod = EnhanceFilters(mode, requested, dev_.MaxAnisotropy(), tex.transform.mip_levels > 0,
                                      levels, &mag, &min, &aniso);
            // The anisotropy parameter is per texture object: an owned texture
            // keeps the value of its LAST draw, so it is restated (state-cached)
            // on every owned draw -- 1.0 under a selection without anisotropy.
            if (dev_.MaxAnisotropy() > 1) st.TexParamf(kGlTextureMaxAnisotropy, static_cast<float>(aniso));
            GlDevice::TextureFilterCensus& census = dev_.FilterCensus();
            census.owned_draws++;
            if (mag != title_mag || min != title_min || aniso > 1) census.enhanced_draws++;
            if (s.hw_lod) census.hw_lod_draws++;
        }
        st.TexParami(GL_TEXTURE_MAG_FILTER, static_cast<GLint>(mag));
        st.TexParami(GL_TEXTURE_MIN_FILTER, static_cast<GLint>(min));
        // depth and indexed serves never filter
        if (s.using_rt && (tex.transform.depth_fmt != 0 || tex.transform.idx_fmt != 0)) {
            st.TexParami(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            st.TexParami(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        }
    }
    // Units 2 (u_clut) and 3 (u_idx_tex2) hold valid textures on every draw (bound without a unit switch when they hold
    // the texture already: the active unit is 0 again below either way)
    const bool idx_active = s.has_texture && s.using_rt && tex.transform.idx_fmt != 0;
    const GlSurface* clut = dev_.Surface(p.clut);
    const GlSurface* partner = dev_.Surface(p.idx_partner);
    const uint32_t dummy = dev_.DummyTexture();
    if (legacy_) st.ActiveTexture(GL_TEXTURE2);   // (the legacy sequence selects every unit)
    st.BindTex2DUnit(GL_TEXTURE2, idx_active && clut && clut->tex ? clut->tex : dummy);
    const bool partner_active = idx_active && tex.transform.idx_fmt == 5 && partner && partner->tex;
    if (partner_active || legacy_) {
        st.ActiveTexture(GL_TEXTURE3);
        st.BindTex2D(partner_active ? partner->tex : dummy);
        if (partner_active) {
            st.TexParami(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            st.TexParami(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            st.TexParami(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            st.TexParami(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    } else {
        st.BindTex2DUnit(GL_TEXTURE3, dummy);
    }
    st.ActiveTexture(GL_TEXTURE0);
}

void GlDrawExecutor::TextureUniforms(const gs::DrawIr& ir, const gs::TextureSource& tex,
                                     const DrawParams& p, const Step& s) {
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    const auto& t = tex.transform;
    const bool rt = s.has_texture && s.using_rt;
    {
        // the values this call sends (-1: a uniform it leaves as inherited); the same as the last call's: nothing to do
        TextureUniformValues v;
        std::memset(&v, 0, sizeof v);
        v.use_texture = s.has_texture ? 1 : 0;
        v.depth_fmt = rt ? t.depth_fmt : 0;
        v.depth_zoff = rt ? t.z_row_offset : 0;
        v.idx_fmt = rt ? t.idx_fmt : 0;
        v.uv_native = (p.inherited_uniforms & kInheritUvNative) ? -1 : ((s.has_texture && t.uv_native && v.idx_fmt == 0) ? 1 : 0);
        v.draw_scale = (p.inherited_uniforms & kInheritDrawScale) ? -1 : std::max<int>(1, p.scale);
        v.idx_self = v.idx_fmt != 0 ? static_cast<int>(t.idx_self) : 0;
        if (v.idx_fmt == 6 && ir.tex.fst) v.idx_self |= 0x1000;
        v.clut_raw = (v.idx_fmt == 4 && t.idx_self != 0 && tex.enabled && ir.tex.tfx == 3 && ir.tex.tcc == 1) ? 1 : 0;
        v.tex_w = t.region.applied ? static_cast<float>(t.region.w) : static_cast<float>(t.uv_tex_w);
        v.tex_h = t.region.applied ? static_cast<float>(t.region.h) : static_cast<float>(t.uv_tex_h);
        v.tfx = tex.enabled ? static_cast<int>(ir.tex.tfx) : 0;
        v.mip_levels = rt ? 0 : t.mip_levels;
        v.mip_lcm = t.mip_lcm ? 1 : 0;
        v.lod_hw = s.hw_lod ? 1 : 0;
        v.lod_k = static_cast<float>(t.mip_k) / 16.0f;
        v.lod_l = static_cast<float>(1u << (t.mip_l & 3));
        v.tcc = tex.enabled ? static_cast<int>(ir.tex.tcc) : 0;
        static_assert(sizeof(TextureUniformValues) == 17 * 4, "no padding in the key");
        if (texture_sent_gen_ == u.Generation() && std::memcmp(&v, &texture_sent_, sizeof v) == 0) return;
        texture_sent_ = v;
        texture_sent_gen_ = u.Generation();
    }
    u.Set1i(L.use_texture, s.has_texture ? 1 : 0);
    u.Set1i(L.tex_sampler, 0);
    u.Set1i(L.depth_fmt, rt ? t.depth_fmt : 0);
    u.Set1i(L.depth_zoff, rt ? t.z_row_offset : 0);
    const int idx_fmt = rt ? t.idx_fmt : 0;
    u.Set1i(L.idx_fmt, idx_fmt);
    if (!(p.inherited_uniforms & kInheritUvNative))
        u.Set1i(L.uv_native, (s.has_texture && t.uv_native && idx_fmt == 0) ? 1 : 0);
    if (!(p.inherited_uniforms & kInheritDrawScale)) u.Set1i(L.draw_scale, std::max<int>(1, p.scale));
    int idx_self = idx_fmt != 0 ? static_cast<int>(t.idx_self) : 0;
    if (idx_fmt == 6 && ir.tex.fst) idx_self |= 0x1000;   // fmt 6 rescales STQ to claimed texels
    u.Set1i(L.idx_self, idx_self);
    // CLUT row 1 (raw PS2 alpha) only for the PSMT8H self-read HIGHLIGHT2/TCC=1 class.
    const bool clut_raw = idx_fmt == 4 && t.idx_self != 0 && tex.enabled && ir.tex.tfx == 3 &&
                          ir.tex.tcc == 1;
    u.Set1i(L.clut_raw, clut_raw ? 1 : 0);
    // Render-target composites are never V-flipped; the single flip is at scanout.
    u.Set1i(L.flip_v, 0);
    if (t.region.applied)
        u.Set2f(L.tex_size, static_cast<float>(t.region.w), static_cast<float>(t.region.h));
    else
        u.Set2f(L.tex_size, static_cast<float>(t.uv_tex_w), static_cast<float>(t.uv_tex_h));
    u.Set1i(L.tfx, tex.enabled ? static_cast<int>(ir.tex.tfx) : 0);
    const int levels = rt ? 0 : t.mip_levels;
    u.Set2i(L.mip_ctl, levels, t.mip_lcm ? 1 : 0);
    u.Set1i(L.tex_lod_hw, s.hw_lod ? 1 : 0);
    u.Set2f(L.mip_lodp, static_cast<float>(t.mip_k) / 16.0f,
            static_cast<float>(1u << (t.mip_l & 3)));
    u.Set1i(L.tcc, tex.enabled ? static_cast<int>(ir.tex.tcc) : 0);
}

// Mode by FST / enable; STQ prescale by the resolver's operands; region rebase
// ((u - rx*q)/q = u/q - rx per pixel); shuffle U scale for STQ; optional column snap.
bool GlDrawExecutor::ConvertVertices(const gs::DrawIr& ir, const gs::TextureSource& tex, Step& s) {
    const auto& t = tex.transform;
    const size_t n = ir.verts.size();
    scratch_.resize(n);
    // Untextured iff no enabled texture with a positive u_tex_size (uv_tex_w/h);
    // the STQ prescale operands (uv_pre_w/h) are a separate input.
    const bool textured = tex.enabled && t.uv_tex_w > 0 && t.uv_tex_h > 0;
    const VertexConvert::UvMode mode =
        !textured ? VertexConvert::UvMode::Untextured
                  : (ir.tex.fst ? VertexConvert::UvMode::UvFixed : VertexConvert::UvMode::Stq);
    const VertexConvert::FusedBboxes bb =
        VertexConvert::Convert(ir.verts.data(), n, scratch_.data(), mode, t.uv_pre_w, t.uv_pre_h);
    s.bb_valid = bb.valid;
    s.bb_tri[0] = bb.tri.x0; s.bb_tri[1] = bb.tri.y0; s.bb_tri[2] = bb.tri.x1; s.bb_tri[3] = bb.tri.y1;
    s.bb_all[0] = bb.all.x0; s.bb_all[1] = bb.all.y0; s.bb_all[2] = bb.all.x1; s.bb_all[3] = bb.all.y1;
    if (t.region.applied) {
        const float rx = static_cast<float>(t.region.u0), ry = static_cast<float>(t.region.v0);
        for (GpuVertex& gv : scratch_) {
            gv.u -= rx * gv.q;
            gv.v -= ry * gv.q;
        }
    }
    if (t.shuffle_uv_scale_u != 1.0f && !ir.tex.fst)
        for (GpuVertex& gv : scratch_) gv.u *= t.shuffle_uv_scale_u;
    if (t.shuffle_snap)
        for (GpuVertex& gv : scratch_) {
            gv.u = SnapColumn8(gv.u);
            gv.v = SnapRow(gv.v);
        }
    s.tri_verts = (n / 3) * 3;
    return s.tri_verts >= 3;
}

void GlDrawExecutor::RegisterUniforms(const gs::DrawIr& ir, const DrawParams& p) {
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    const auto& r = ir.raster;
    {
        // the values this call sends (-1: left as inherited); the same as the last call's: nothing to do
        RegisterUniformValues v;
        std::memset(&v, 0, sizeof v);
        v.blend_enabled = ir.alpha_blend ? 1 : 0;
        v.fba = r.fba ? 1 : 0;
        v.colclamp = r.colclamp ? 1 : 0;
        v.fb16 = r.frame.psm == 0x0A ? 1 : 0;
        v.ct16q = (p.inherited_uniforms & kInheritCt16Quant) ? -1 : (Is16BitFrame(r.frame.psm) ? std::max<int>(1, p.scale) : 0);
        if (p.inherited_uniforms & kInheritDither) {
            v.dthe = v.dimx_lo = v.dimx_hi = -1;
        } else {
            v.dthe = r.dthe ? 1 : 0;
            v.dimx_lo = static_cast<int>(static_cast<uint32_t>(r.dimx & 0xFFFFFFFFull));
            v.dimx_hi = static_cast<int>(static_cast<uint32_t>(r.dimx >> 32));
        }
        v.scanmsk = ir.scanmsk;
        v.ate = r.test.ate ? 1 : 0;
        v.atst = r.test.atst;
        v.aref = r.test.aref;
        v.afail = r.test.afail;
        v.date = r.test.date ? 1 : 0;
        v.datm = r.test.datm ? 1 : 0;
        v.fog = ir.fog ? 1 : 0;
        v.fog_r = r.fogcol_r / 255.0f;
        v.fog_g = r.fogcol_g / 255.0f;
        v.fog_b = r.fogcol_b / 255.0f;
        // (the inherited dither's -1 lo/hi and a real 0xFFFFFFFF both read -1: the inheritance mask tells them apart)
        const bool inherited_dither = (p.inherited_uniforms & kInheritDither) != 0;
        static_assert(sizeof(RegisterUniformValues) == 19 * 4, "no padding in the key");
        if (register_sent_gen_ == u.Generation() && register_inherited_dither_ == inherited_dither &&
            std::memcmp(&v, &register_sent_, sizeof v) == 0)
            return;
        register_sent_ = v;
        register_sent_gen_ = u.Generation();
        register_inherited_dither_ = inherited_dither;
    }
    u.Set1i(L.blend_enabled, ir.alpha_blend ? 1 : 0);
    u.Set1i(L.fba, r.fba ? 1 : 0);
    u.Set1i(L.colclamp, r.colclamp ? 1 : 0);
    // 16-bit colour FRAME conventions: the A-bit alpha (PSMCT16S only) and the 5-bit
    // channel quantisation with DTHE/DIMX dither (both CT16 formats; value = scale).
    u.Set1i(L.fb16, r.frame.psm == 0x0A ? 1 : 0);
    // A class-native generic arm leaves these as the last full-set draw wrote them
    // (InheritedUniform, backend.h).
    if (!(p.inherited_uniforms & kInheritCt16Quant))
        u.Set1i(L.ct16q, Is16BitFrame(r.frame.psm) ? std::max<int>(1, p.scale) : 0);
    if (!(p.inherited_uniforms & kInheritDither)) {
        u.Set1i(L.dthe, r.dthe ? 1 : 0);
        u.Set1i(L.dimx_lo, static_cast<int>(static_cast<uint32_t>(r.dimx & 0xFFFFFFFFull)));
        u.Set1i(L.dimx_hi, static_cast<int>(static_cast<uint32_t>(r.dimx >> 32)));
    }
    u.Set1i(L.scanmsk, ir.scanmsk);
    u.Set1i(L.ate, r.test.ate ? 1 : 0);
    u.Set1i(L.atst, r.test.atst);
    u.Set1i(L.aref, r.test.aref);
    u.Set1i(L.afail, r.test.afail);
    u.Set1i(L.date, r.test.date ? 1 : 0);
    u.Set1i(L.datm, r.test.datm ? 1 : 0);
    u.Set1i(L.fog_enabled, ir.fog ? 1 : 0);
    u.Set3f(L.fogcol, r.fogcol_r / 255.0f, r.fogcol_g / 255.0f, r.fogcol_b / 255.0f);
}

// fbmsk16 analysis, the exact-GL route, the Cd read rect (bbox ∩ scissor ∩ target),
// and the reasons a destination copy is needed.
void GlDrawExecutor::ClassifyRoutes(const gs::DrawIr& ir, const gs::TextureSource& tex,
                                    const DrawParams& p, Step& s) {
    const auto& r = ir.raster;
    s.alpha_blend = ir.alpha_blend;
    s.fbmsk16 = 0;
    s.fbmsk16_exact = false;
    if (Is16BitFrame(r.frame.psm) && r.frame.fbmsk != 0) {
        s.fbmsk16 = Fbmsk16From32(r.frame.fbmsk);
        s.fbmsk16_exact = Fbmsk16Exact(s.fbmsk16);
    }
    s.route_rt_blend = RouteRtBlend(ir, s.using_rt, s.fbmsk16_exact);
    s.idx_self_serve = s.has_texture && s.using_rt && tex.transform.idx_fmt == 4 &&
                       tex.transform.idx_self != 0;
    if (s.idx_self_serve) s.route_rt_blend = false;
    // read rect: bbox_all clamped to ±1e9 (a NaN box collapses to the ±1e9 seeds) ∩ scissor ∩ target
    s.copy_x = s.copy_y = s.copy_w = s.copy_h = 0;
    if (!ir.verts.empty()) {
        const float cbx0 = std::min(s.bb_all[0], 1e9f), cby0 = std::min(s.bb_all[1], 1e9f);
        const float cbx1 = std::max(s.bb_all[2], -1e9f), cby1 = std::max(s.bb_all[3], -1e9f);
        int rx0 = std::max(static_cast<int>(std::floor(cbx0)), static_cast<int>(r.scissor.x0));
        int ry0 = std::max(static_cast<int>(std::floor(cby0)), static_cast<int>(r.scissor.y0));
        int rx1 = std::min(static_cast<int>(std::ceil(cbx1)), static_cast<int>(r.scissor.x1) + 1);
        int ry1 = std::min(static_cast<int>(std::ceil(cby1)), static_cast<int>(r.scissor.y1) + 1);
        rx0 = std::max(0, std::min(rx0, p.target_w));
        ry0 = std::max(0, std::min(ry0, p.target_h));
        rx1 = std::max(0, std::min(rx1, p.target_w));
        ry1 = std::max(0, std::min(ry1, p.target_h));
        if (rx1 > rx0 && ry1 > ry0) {
            s.copy_x = rx0; s.copy_y = ry0; s.copy_w = rx1 - rx0; s.copy_h = ry1 - ry0;
        }
    }
    if (s.idx_self_serve) { s.copy_w = 0; s.copy_h = 0; }   // UV-addressed read: whole copy
    s.needs_rt_copy = r.test.date || s.fbmsk16_exact || s.idx_self_serve;
    s.wrap_route = BlendFfMap::RouteWrapBlend(ir.alpha_blend, r.colclamp);
}

// Lazy destination copy into rt_copy (unit 1) from the bound attachment.
void GlDrawExecutor::CdCopy(const DrawParams& p, Step& s) {
    const GlSurface* copy = dev_.Surface(p.rt_copy);
    if (!p.rt_copy_available || !copy || !copy->tex) return;
    GlStateCache& st = dev_.State();
    bool skip = false;
    int cx = s.copy_x, cy = s.copy_y, cw = s.copy_w, ch = s.copy_h;
    if (p.cd_copy) {
        const int8_t route_now = p.alt16_route ? 1 : 0;
        if (p.cd_copy->last_route != route_now) {
            CdCopyPolicy::Invalidate(*p.cd_copy);
            p.cd_copy->last_route = route_now;
        }
        int oc[4];
        if (!CdCopyPolicy::Decide(*p.cd_copy, s.copy_x, s.copy_y, s.copy_w, s.copy_h,
                                  s.idx_self_serve, p.target_w, p.target_h, oc)) {
            skip = true;
        } else {
            cx = oc[0]; cy = oc[1]; cw = oc[2]; ch = oc[3];
        }
    }
    st.BindFb(GL_READ_FRAMEBUFFER, s.fbo);
    st.ActiveTexture(GL_TEXTURE1);
    st.BindTex2D(copy->tex);
    if (!skip) {
        const int sc = s.scale;
        if (cw > 0 && ch > 0)
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, cx * sc, cy * sc, cx * sc, cy * sc, cw * sc, ch * sc);
        else
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, p.target_w * sc, p.target_h * sc);
        s.cd_copied = true;
        s.cd_rect = Rect{cx, cy, cw, ch};
    }
    st.ActiveTexture(GL_TEXTURE0);
    s.sw_blend_active = true;
}

// The blend-equation uniforms of a blended draw (both the shader and the
// fixed-function arm write them); a class-native generic arm inherits them.
void GlDrawExecutor::BlendAbcdUniforms(const gs::DrawIr& ir, const DrawParams& p) {
    if (p.inherited_uniforms & kInheritBlendAbcd) return;
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    const auto& a = ir.raster.alpha;
    u.Set1i(L.blend_a, a.a);
    u.Set1i(L.blend_b, a.b);
    u.Set1i(L.blend_c, a.c);
    u.Set1i(L.blend_d, a.d);
    u.Set1f(L.fix_alpha, a.fix / 128.0f);
}

void GlDrawExecutor::BlendState(const gs::DrawIr& ir, const DrawParams& p, Step& s) {
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    const auto& a = ir.raster.alpha;
    s.sw_blend_active = false;
    if (ir.alpha_blend && (s.using_rt || ir.raster.fba || s.fbmsk16_exact || s.wrap_route) &&
        !s.route_rt_blend) {
        s.needs_rt_copy = true;
        CdCopy(p, s);
        if (s.sw_blend_active) u.Set1i(L.rt_sampler, 1);
        BlendAbcdUniforms(ir, p);
        st.Disable(GL_BLEND);
    } else if (s.needs_rt_copy) {
        // DATE / exact mask / idx self-read without a shader blend still reads the
        // destination copy; an unblended draw must not inherit blend state.
        CdCopy(p, s);
        if (s.sw_blend_active) u.Set1i(L.rt_sampler, 1);
        if (!ir.alpha_blend) st.Disable(GL_BLEND);
    } else if (ir.alpha_blend) {
        const FfBlendMap ffm = BlendFfMap::Map(a.a, a.b, a.c, a.d);
        if (ffm.uses_fix) st.BlendColor(0.f, 0.f, 0.f, a.fix / 128.0f);
        st.Enable(GL_BLEND);
        st.BlendEquationSeparate(ffm.eq_rgb, GL_FUNC_ADD);
        st.BlendFuncSeparate(ffm.src_rgb, ffm.dst_rgb, GL_ONE, GL_ZERO);   // alpha: source replace
        BlendAbcdUniforms(ir, p);
    } else {
        st.Disable(GL_BLEND);
    }
    u.Set1i(L.sw_blend, s.sw_blend_active ? 1 : 0);
}

// The colour-authored Z source (unit 4) moves the compare into the shader with the
// GL depth function forced ALWAYS; otherwise TEST.ZTE/ZTST and ZBUF.ZMSK map directly.
void GlDrawExecutor::DepthState(const gs::DrawIr& ir, const DrawParams& p, const Step& s) {
    (void)s;
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    const auto& t = ir.raster.test;
    const GlSurface* zsrc = dev_.Surface(p.zsrc);
    const bool armed = p.zsrc_armed && zsrc && zsrc->tex;
    if (legacy_) st.ActiveTexture(GL_TEXTURE4);   // (the legacy sequence selects every unit)
    st.BindTex2DUnit(GL_TEXTURE4, armed ? zsrc->tex : dev_.DummyTexture());
    st.ActiveTexture(GL_TEXTURE0);
    u.Set1i(L.zsrc_en, armed ? t.ztst : 0);
    if (!(p.inherited_uniforms & kInheritZsrcMap)) {
        u.Set1i(L.zsrc_num, armed ? std::max<int>(1, p.zsrc_scale) : 1);
        u.Set1i(L.zsrc_den, armed ? std::max<int>(1, p.scale) : 1);
    }
    if (armed) {
        st.Enable(GL_DEPTH_TEST);
        st.DepthFunc(GL_ALWAYS);
        st.DepthMask(false);
    } else if (t.zte) {
        st.Enable(GL_DEPTH_TEST);
        switch (t.ztst) {
            case 0: st.DepthFunc(GL_NEVER); break;
            case 1: st.DepthFunc(GL_ALWAYS); break;
            case 2: st.DepthFunc(GL_GEQUAL); break;
            case 3: st.DepthFunc(GL_GREATER); break;
            default: st.DepthFunc(GL_GEQUAL); break;
        }
        st.DepthMask(!ir.raster.zbuf.zmsk);
    } else {
        st.Enable(GL_DEPTH_TEST);
        st.DepthFunc(GL_ALWAYS);
        st.DepthMask(false);
    }
}

// The GS scissor is inclusive and always active; the targets keep PS2 top at GL
// bottom so y is not flipped. The half-open span scales exactly to physical pixels.
void GlDrawExecutor::ScissorState(const gs::DrawIr& ir, const Step& s) {
    GlStateCache& st = dev_.State();
    const auto& sc = ir.raster.scissor;
    const int sx0 = std::max(0, static_cast<int>(sc.x0));
    const int sy0 = std::max(0, static_cast<int>(sc.y0));
    const int sx1 = std::min(s.render_w - 1, static_cast<int>(sc.x1));
    const int sy1 = std::min(s.render_h - 1, static_cast<int>(sc.y1));
    if (sx1 >= sx0 && sy1 >= sy0 &&
        (sx0 > 0 || sy0 > 0 || sx1 < s.render_w - 1 || sy1 < s.render_h - 1)) {
        st.Enable(GL_SCISSOR_TEST);
        st.Scissor(sx0 * s.scale, sy0 * s.scale, (sx1 - sx0 + 1) * s.scale,
                   (sy1 - sy0 + 1) * s.scale);
    } else {
        st.Disable(GL_SCISSOR_TEST);
    }
}

// FBMSK bit = 1 protects. Exact 5551 merge engaged => the shader merges and GL writes
// all channels; 16-bit frames mask per 5551 field; 32-bit per byte; PSMCT24 never
// writes alpha.
void GlDrawExecutor::ColorMaskState(const gs::DrawIr& ir, const Step& s) {
    GlStateCache& st = dev_.State();
    GlUniformCache& u = dev_.Uniforms();
    const GlUniformLocations& L = dev_.Shaders().Locations();
    const uint32_t mask = ir.raster.frame.fbmsk;
    const uint8_t psm = ir.raster.frame.psm;
    const bool engaged = s.fbmsk16_exact && s.sw_blend_active;
    bool wr, wg, wb, wa;
    if (engaged) {
        wr = wg = wb = wa = true;
    } else if (Is16BitFrame(psm)) {
        wr = (s.fbmsk16 & 0x001F) != 0x001F;
        wg = (s.fbmsk16 & 0x03E0) != 0x03E0;
        wb = (s.fbmsk16 & 0x7C00) != 0x7C00;
        wa = (s.fbmsk16 & 0x8000) != 0x8000;
    } else {
        wr = (mask & 0x000000FF) != 0x000000FF;
        wg = (mask & 0x0000FF00) != 0x0000FF00;
        wb = (mask & 0x00FF0000) != 0x00FF0000;
        wa = (mask & 0xFF000000) != 0xFF000000;
    }
    if (psm == 0x01) wa = false;
    st.ColorMask(wr, wg, wb, wa);
    u.Set1i(L.fbmsk16_en, engaged ? 1 : 0);
    u.Set1i(L.fbmsk16, static_cast<int>(s.fbmsk16));
}

DrawResult GlDrawExecutor::MakeResult(const gs::DrawIr& ir, const DrawParams& p,
                                      const Step& s) const {
    Census().state_calls = dev_.State().Mutations();
    Census().uniform_calls = dev_.Uniforms().Mutations();
    Census().uniform_sends = dev_.Uniforms().Sent();
    Census().uniform_checked = dev_.Uniforms().CheckedSyncs();
    Census().uniform_mismatch = dev_.Uniforms().CheckMismatches();
    DrawResult r;
    r.drew = true;
    r.bbox_valid = s.bb_valid;
    for (int i = 0; i < 4; i++) { r.bbox_tri[i] = s.bb_tri[i]; r.bbox_all[i] = s.bb_all[i]; }
    r.alt16_route = p.alt16_route;
    r.sw_blend_active = s.sw_blend_active;
    r.needs_rt_copy = s.needs_rt_copy;
    r.route_rt_blend = s.route_rt_blend;
    r.wrap_route = s.wrap_route;
    r.fbmsk16_exact = s.fbmsk16_exact;
    r.fbmsk16_engaged = s.fbmsk16_exact && s.sw_blend_active;
    r.fbmsk16 = s.fbmsk16;
    r.cd_copy_performed = s.cd_copied;
    r.cd_copy_rect = s.cd_rect;
    // write rect: bbox_all ∩ scissor ∩ target (logical, half-open)
    const auto& sc = ir.raster.scissor;
    int x0 = std::max(static_cast<int>(std::floor(s.bb_all[0])), static_cast<int>(sc.x0));
    int y0 = std::max(static_cast<int>(std::floor(s.bb_all[1])), static_cast<int>(sc.y0));
    int x1 = std::min(static_cast<int>(std::ceil(s.bb_all[2])), static_cast<int>(sc.x1) + 1);
    int y1 = std::min(static_cast<int>(std::ceil(s.bb_all[3])), static_cast<int>(sc.y1) + 1);
    x0 = std::max(0, std::min(x0, p.target_w));
    y0 = std::max(0, std::min(y0, p.target_h));
    x1 = std::max(0, std::min(x1, p.target_w));
    y1 = std::max(0, std::min(y1, p.target_h));
    if (x1 > x0 && y1 > y0) r.write_rect = Rect{x0, y0, x1 - x0, y1 - y0};
    return r;
}

}  // namespace render
}  // namespace ps2r
