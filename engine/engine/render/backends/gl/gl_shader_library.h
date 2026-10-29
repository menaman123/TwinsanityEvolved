// render/backends/gl/gl_shader_library.h -- GlShaderLibrary: compiles and links the
// programs (PS2 draw, scanout quad, the stencil-composite strip-count / invert /
// constant-quad specialisations, the opaque-alpha stencil mark) and holds the PS2
// program's uniform-location table plus the specialisations' few locations.
//
// The sources are the baseline text of GlShaderSources; compile / link failures are
// reported on stderr with the driver log and leave the program id 0. After a
// successful link the sampler uniforms are bound to fixed units (u_texture 0,
// u_rt_texture 1, u_clut 2, u_idx_tex2 3, u_zsrc_tex 4) and the arms that a draw
// may leave untouched are primed to their off values so the first draws are
// deterministic.
#pragma once

#include <cstdint>
#include <string>

#include "engine/platform/host/platform.h"   // PS2R_HOST_VITA

namespace ps2r {
namespace render {

class GlUniformCache;
class GlStateCache;
struct MeshDrawParams;   // engine/render/backend.h
struct MeshInstance;     // engine/render/backend.h

// Uniform locations of the PS2 draw program (-1 when the driver optimised the
// uniform away; glUniform* on -1 is a no-op).
struct GlUniformLocations {
    int screen_size = -1, use_texture = -1, tex_sampler = -1;
    int blend_a = -1, blend_b = -1, blend_c = -1, blend_d = -1, fix_alpha = -1, blend_enabled = -1;
    int tfx = -1, tcc = -1, fba = -1, colclamp = -1, scanmsk = -1;
    int zte = -1, ztst = -1;
    int ate = -1, atst = -1, aref = -1, afail = -1, date = -1, datm = -1;
    int fog_enabled = -1, fogcol = -1;
    // the native mesh draws' (NATIVE_MESH builds: the Vita); -1 elsewhere
    int native = -1, mesh_lit = -1, mesh = -1, mesh_light = -1;
    int tex_size = -1, uv_scale = -1, uv_mode = -1;
    int sw_blend = -1, rt_sampler = -1, flip_v = -1;
    int fbmsk16_en = -1, fbmsk16 = -1;
    int depth_fmt = -1, depth_zoff = -1;
    int idx_fmt = -1, idx_self = -1, draw_scale = -1, uv_native = -1;
    int clut_sampler = -1, idx_tex2_sampler = -1;
    int fb16 = -1, ct16q = -1, dthe = -1, dimx_lo = -1, dimx_hi = -1;
    int zsrc_en = -1, zsrc_sampler = -1, zsrc_num = -1, zsrc_den = -1, clut_raw = -1;
    int mip_ctl = -1, mip_lodp = -1, tex_lod_hw = -1;
};

// The stencil-composite programs' uniform locations (samplers are bound once at
// link: u_rt_texture 1, u_zsrc_tex 4 -- the PS2 program's units).
struct GlStencilLocations {
    int strip_screen = -1, strip_zsrc_en = -1, strip_datm = -1, strip_zsrc_num = -1, strip_zsrc_den = -1;
    int invert_screen = -1, invert_fix = -1;
    int const_color = -1;
};

class GlShaderLibrary {
public:
    // Compiles + links both programs; primes the PS2 program through `uniforms`
    // (which is cleared first). Returns false on any failure.
    bool Build(GlUniformCache& uniforms);
    // The context's clip control is GL_ZERO_TO_ONE: the PS2 vertex shader maps Z straight to window depth
    void SetZeroToOneDepth(bool on) { zero_to_one_depth_ = on; }
    // The producer's GS Z stays in [0, 2^24] (GlDeviceSettings::gs_z24): every vertex program maps it to window depth
    // z / 2^24 -- each integer Z exact (the default z / 2^32 resolves 128 / 256-unit steps: gl_shader_sources.cpp [Z24])
    void SetGsZ24Depth(bool on) { gs_z24_depth_ = on; }
    bool GsZ24Depth() const { return gs_z24_depth_; }
    // The depth mapping's #defines every vertex program takes (ZERO_TO_ONE_DEPTH, GS_Z24_DEPTH), each line-terminated
    std::string DepthDefines() const;
    // The PS2 program carries the native mesh transform (GlDevice::DrawMesh): built so on the Vita
    bool NativeMesh() const { return native_mesh_; }
    void Destroy();

    uint32_t Ps2Program() const { return ps2_program_; }
    uint32_t ScanoutProgram() const { return scanout_program_; }
    int ScanoutTextureLocation() const { return scanout_tex_loc_; }
    // The CT16 dithered scanout (ScanoutDesc::dither16): u_texture, u_source_size (the source's texel size), u_dimx[16]
    uint32_t ScanoutDitherProgram() const { return scanout_dither_program_; }
    int ScanoutDitherTextureLocation() const { return scanout_dither_tex_loc_; }
    int ScanoutDitherSourceSizeLocation() const { return scanout_dither_size_loc_; }
    int ScanoutDitherMatrixLocation() const { return scanout_dither_dimx_loc_; }
    const GlUniformLocations& Locations() const { return loc_; }
    uint32_t StencilStripProgram() const { return strip_program_; }
    uint32_t StencilInvertProgram() const { return invert_program_; }
    uint32_t ConstProgram() const { return const_program_; }
    // The opaque-alpha stencil mark (MarkStencilWhereOpaque): u_frame bound to unit 0 at link
    uint32_t OpaqueMaskProgram() const { return opaque_mask_program_; }
    // The screen copy on the Vita (GlDevice::CopyRect): u_texture (unit 0) drawn unchanged over the constant program's
    // full-screen triangle, u_src = the source rectangle in texture coordinates (u0, v0, du, dv). Built on the Vita only
    // (0 elsewhere: desktop GL copies with glCopyTexSubImage2D).
    uint32_t CopyProgram() const { return copy_program_; }
    int CopySourceLocation() const { return copy_src_loc_; }
    // The movie pictures' YUV program on the Vita (GlDevice::DrawMoviePicture): the copy program's vertex shader, the Y U V
    // planes on units 0-2 converted (BT.601, limited range), u_src as the copy program's. 0 elsewhere / when it failed.
    uint32_t MovieYuvProgram() const { return movie_yuv_program_; }
    int MovieYuvSourceLocation() const { return movie_yuv_src_loc_; }
    int MovieYuvSizeLocation() const { return movie_yuv_size_loc_; }   // u_luma_size: the luma plane's size, the chroma's
    const GlStencilLocations& StencilLocations() const { return sloc_; }

    // The Vita's FAST PS2 program (0 elsewhere, or when TE_GFX_VITA_PS2_FAST=0): the same vertex shader and the same
    // fragment source with the uniforms of the arms no Vita draw takes replaced by their values (kVitaFastConstants in
    // gl_shader_library.cpp) -- Sony's compiler drops those arms. A draw may use it only while every one of those
    // uniforms holds exactly that value (GlDrawExecutor::SelectPs2Variant checks the uniform cache's shadows): then the
    // two programs compute the same expressions. Its uniforms mirror the full program's (FastMirror: full location ->
    // fast location; the uniform cache writes both).
    uint32_t Ps2FastProgram() const { return ps2_fast_program_; }
    bool Ps2FastEnabled() const { return ps2_fast_program_ != 0 && ps2_fast_enabled_; }
    void SetPs2FastEnabled(bool on) { ps2_fast_enabled_ = on; }
    struct FastConstant { int GlUniformLocations::*field; int value; };
    static const FastConstant* FastConstants(int* count);

    // The Vita's LEAN PS2 programs (TE_GFX_VITA_PS2_LEAN: unset / 1 on, 0 off, abN on / off every N presents): one small
    // fragment program per draw state the Vita's draws take (GlShaderSources::LeanPs2Fragment specialised by #defines:
    // no uniform branches; a discard only in the alpha-tested variants -- the GPU's hidden-surface removal then shades an
    // opaque pixel once), over the PS2 vertex program. SelectPs2 picks the program for the draw about to be made from the
    // full program's uniform values (the uniform cache's shadows: whatever the full program would compute with, the lean
    // program computes the same arithmetic for), binds it, brings its uniforms to those values (only what changed since
    // that program last drew) and counts it (`box`: the draw's screen box, frame pixels, for the census); a state no lean
    // program serves takes the fast program, else the full one. Elsewhere SelectPs2 is not built.
#if PS2R_HOST_VITA
    // THE RAW MESH DRAW (the Vita; TE_GFX_VITA_RAW_MESH=0: never): a native mesh draw whose lean program SelectPs2 chooses
    // is a mesh-path one (LeanPs2Vertex 1 / 2) is submitted to GXM directly by SelectPs2 -- the same two GXPs (the program's
    // own, registered once more with vitaGL's shader patcher), the vertex program patched for the mesh's one 48-byte
    // stream, the fragment program patched for vitaGL's current blend state, the same uniform values written into fresh
    // default uniform buffers, vitaGL's bound texture on unit 0, sceGxmDraw -- instead of vitaGL's glDrawArrays (its
    // per-draw gathering of every attribute, one vertex stream per attribute, its uniform copies). vitaGL's state stays
    // its own: every GL state call the draw needs has been made (vitaGL applies them to the GXM context as they are made),
    // its scene is begun as its own draws begin it (scene_reset), and its next draw re-reserves its uniform buffers (it
    // re-sets its programs, textures and streams at every draw anyway). `drew_raw` true: drawn, the caller draws nothing.
    struct RawMesh {
        uint32_t vbo = 0;           // the mesh's GL buffer (vitaGL's name for a buffer is its vbo record)
        uint32_t texture = 0;       // the GL texture on unit 0 (0: none)
        uint32_t tex_w = 0, tex_h = 0;   // its size when the caller knows it (the texture record's layout check), else 0
        uint32_t first = 0;         // the first vertex drawn (a sub-range of the mesh: its stream starts there)
        uint32_t count = 0;         // vertices
        uint32_t ibo = 0;           // an indexed mesh's GL index buffer (GlDevice::GlMesh::list_ibo): `first` / `count` are
                                    // then indices into it and the stream starts at the mesh's first distinct vertex
        // THE RAW VERTEX DRAW: `vbo` is the vertex ring (GlVertexStream: RawVertex records, 28 bytes) holding the
        // draw's `count` vertices from vertex `first` -- a vertex draw (GlDrawExecutor::DrawRaw) whose lean program SelectPs2
        // chooses is the raw vertex path's (LeanPs2Vertex 4) is submitted to GXM directly, as a raw mesh draw is: the
        // program's own GXPs (its GXM twin, the vertex program patched for the ring's one 28-byte stream), the same uniform
        // values in default uniform buffers, vitaGL's bound texture on unit 0, sceGxmDraw over vitaGL's progressive indices
        // from the draw's first vertex -- instead of vitaGL's glDrawArrays (its attribute gathering, one stream per attribute,
        // its program's uniform copies)
        bool vertices = false;
    };
    uint32_t SelectPs2(GlStateCache& st, GlUniformCache& u, const float* box, int frame_w, int frame_h,
                       const RawMesh* raw = nullptr, bool* drew_raw = nullptr) const;
    bool RawMeshEnabled() const { return raw_mesh_ && raw_on_; }
    // the raw vertex draws (TE_GFX_VITA_RAW_VERTS=0: never): twins built for the raw vertex path's programs
    bool RawVertsEnabled() const { return raw_verts_ && raw_on_; }
    bool RawMeshBuilt() const { return raw_mesh_; }
    void SetRawMeshOn(bool on) { raw_on_ = on; }   // the in-run A/B (GlDevice: TE_GFX_VITA_RAW_MESH=abN)
    // THE MESH DRAW PACKETS (the Vita; TE_GFX_VITA_PACKETS=0: none, =abN: on / off every N presents): every raw mesh draw
    // records a packet -- its twin, its fragment program (the blend state's patch), its texture record and that record's
    // SceGxmTexture as handed over, its composed fragment uniforms, its vertex constants -- TakeRawPacket hands it over
    // once (0: the last mesh draw recorded none; DropRawPacket forgets it at a mesh draw's start) and DrawRawPacket draws a
    // mesh with one: RawSubmit's GXM calls with the packet's values and the draw's own transform rows, after checking that
    // the packet is current (its slot, the twins' build), the blend state, the target's format and size, and the texture
    // record are as recorded. The caller (GlDrawExecutor::DrawMeshPacket) vouches for the GL context. -1 drawn, else the
    // census's packet_declined reason (nothing handed to GXM). `ibo`: the mesh's index buffer when it is drawn indexed
    // (GlDevice::GlMesh::list_ibo: `first` / `count` index it, the stream starts at the buffer's first vertex), else 0.
    uint32_t TakeRawPacket() const;
    void DropRawPacket() const;
    int DrawRawPacket(uint32_t packet, uint32_t vbo, uint32_t ibo, uint32_t first, uint32_t count, const MeshDrawParams& mp,
                      int frame_w, int frame_h) const;
    // `n` (2..64) replays of one packet (the unlit mesh path's) over one range, each with its own transform
    // (inst[i]: MeshDrawParams' screen / offset / scroll / box), as ONE instanced draw through the instanced twin of the
    // packet's variant -- under DrawRawPacket's conditions, not on the fill census's sampled frames. -1 drawn; else
    // DrawRawPacket's reasons (0: instancing off) -- nothing drawn (the caller replays them one by one)
    int DrawRawPacketInstances(uint32_t packet, uint32_t vbo, uint32_t ibo, uint32_t first, uint32_t count,
                               const MeshInstance* inst, int n, int frame_w, int frame_h) const;
    bool InstancingOn() const;   // the instanced twins built (TE_GFX_VITA_INSTANCING), the packets and the raw draws on
    bool PacketsEnabled() const;
    void SetPacketsOn(bool on) const;   // the in-run A/B (GlDevice: TE_GFX_VITA_PACKETS=abN)
#endif
    bool Ps2LeanBuilt() const { return lean_count_ > 0; }
    bool Ps2LeanEnabled() const { return lean_count_ > 0 && lean_enabled_; }
    void SetPs2LeanEnabled(bool on) { lean_enabled_ = on; }
    struct LeanState;   // gl_shader_library.cpp (the Vita only)

    // The Vita's GPU-SKINNING programs: GlShaderSources::Ps2SkinVertex linked with the lean fragment variants (the
    // textured MODULATE ones at Build, the others the first time a skinned draw takes their state). SelectSkin picks the
    // variant for the draw about to be made exactly as SelectPs2 does (the full program's uniform values in the uniform
    // cache), binds it, brings its uniforms to those values (the mirrored ones, u_mesh / u_mesh_light included) and returns
    // its own locations (the caller sends the palette, u_bones, itself); null when no lean variant serves the state (a
    // skinned draw is then not made: the caller counts it refused). Elsewhere not built.
    struct SkinProgram {
        uint32_t id = 0;
        int bones = -1;   // u_bones (3 rows per joint)
        int disp = -1;    // u_disp (a blend skin's displacement per vertex; u_disp[0] = 0 for a plain skin's draw)
        mutable bool disp_dirty = true;   // u_disp[0] may not be 0 (the last draw with it was a blend skin's)
    };
    struct SkinState;   // gl_shader_library.cpp (the Vita only)
#if PS2R_HOST_VITA
    // THE RAW SKIN DRAW (the Vita; with the raw mesh draws: TE_GFX_VITA_RAW_MESH=0 never): as the raw mesh draw, a skinned
    // draw whose lean skin program has a GXM twin (its GXPs, the vertex program patched for the 72-byte SkinVertex stream)
    // is submitted by SelectSkin itself -- the same uniform values written into fresh default uniform buffers, the palette
    // (u_bones: the joints' rows) and the displacements (u_disp: a blend skin's, u_disp[0] = 0 for a plain skin) straight
    // from the caller's arrays, vitaGL's bound texture on unit 0, the indexed sceGxmDraw -- instead of glUniform4fv of the
    // palette and vitaGL's glDrawElements (its copy of the program's whole 4.3 KB vertex uniform image at every draw).
    struct RawSkin {
        uint32_t vbo = 0, ibo = 0;      // the skin mesh's GL buffers (vitaGL's names are its vbo records)
        uint32_t texture = 0;           // the GL texture on unit 0 (0: none)
        uint32_t tex_w = 0, tex_h = 0;  // its size when known (the texture record's layout check), else 0
        uint32_t count = 0;             // indices
        const float* bones = nullptr;   // joints x 3 rows of 4 floats
        int joints = 0;
        const float* disp = nullptr;    // disp_count x 4 floats (a blend skin's part), or none
        int disp_count = 0;
    };
    const SkinProgram* SelectSkin(GlStateCache& st, GlUniformCache& u, const float* box, int frame_w, int frame_h,
                                  const RawSkin* raw = nullptr, bool* drew_raw = nullptr) const;
    bool SkinBuilt() const { return skin_ != nullptr; }
    // THE SKIN DRAW PACKETS (with the mesh draw packets: TE_GFX_VITA_PACKETS): every raw skin draw records one (its twin,
    // fragment program, texture record and SceGxmTexture, composed fragment uniforms, vertex constants); DrawSkinPacket
    // replays one with the draw's own transform, lighting, palette and displacements (-1 drawn, else a decline reason;
    // nothing handed to GXM then). TakeSkinPacket / DropSkinPacket as TakeRawPacket / DropRawPacket.
    uint32_t TakeSkinPacket() const;
    void DropSkinPacket() const;
    int DrawSkinPacket(uint32_t packet, uint32_t vbo, uint32_t ibo, uint32_t count, const MeshDrawParams& mp, const float* bones,
                       int joints, const float* disp, int disp_count, int frame_w, int frame_h) const;
#endif

    static const char* VertexSource();
    static const char* FragmentSource();
    static const char* ScanoutVertexSource();
    static const char* ScanoutFragmentSource();
    static const char* ScanoutDitherFragmentSource();

private:
    static uint32_t CompileShader(uint32_t type, const char* source);
    static uint32_t LinkProgram(uint32_t vert, uint32_t frag);
    // A vertex + fragment pair compiled and linked (the shaders deleted after the link; 0 on any failure). On the Vita
    // the pair's compiled binaries are cached on the memory card (TE_GL_SHADER_CACHE, a directory; the runner points it
    // at ux0:data/twinsanity/shaders): a launch after the first loads them instead of running Sony's compiler.
    // keep_vs / keep_fs: the pair's shader objects handed back (the caller deletes them) instead of deleted -- the Vita's
    // lean path programs link them again; link_vs + link_fs: nothing compiled, these compiled objects linked (they stay
    // the caller's) and the pair cached under the key a compiled pair of `vertex` + `fragment` has.
    static uint32_t BuildProgram(const char* vertex, const char* fragment, const char* label, uint32_t* keep_vs = nullptr,
                                 uint32_t* keep_fs = nullptr, uint32_t link_vs = 0, uint32_t link_fs = 0);
    void LookupLocations();
    static void LookupLocationsOf(uint32_t program, GlUniformLocations& out);
    void PrimeUniforms(GlUniformCache& uniforms);

    bool BuildStencilPrograms();

    uint32_t ps2_program_ = 0;
    uint32_t scanout_program_ = 0;
    uint32_t scanout_dither_program_ = 0;
    uint32_t strip_program_ = 0;
    uint32_t invert_program_ = 0;
    uint32_t const_program_ = 0;
    uint32_t opaque_mask_program_ = 0;
    uint32_t copy_program_ = 0;
    int copy_src_loc_ = -1;
    uint32_t movie_yuv_program_ = 0;
    int movie_yuv_src_loc_ = -1;
    int movie_yuv_size_loc_ = -1;
    GlStencilLocations sloc_;
    int scanout_tex_loc_ = -1;
    int scanout_dither_tex_loc_ = -1, scanout_dither_size_loc_ = -1, scanout_dither_dimx_loc_ = -1;
    GlUniformLocations loc_;
    bool zero_to_one_depth_ = false;
    bool gs_z24_depth_ = false;
    bool native_mesh_ = false;
    uint32_t ps2_fast_program_ = 0;
    GlUniformLocations loc_fast_;
    bool ps2_fast_enabled_ = true;
    // the lean programs (gl_shader_library.cpp; the Vita only)
    LeanState* lean_ = nullptr;
    int lean_count_ = 0;
    bool lean_enabled_ = true;
#if PS2R_HOST_VITA
    bool raw_mesh_ = false;   // the raw mesh draws (TE_GFX_VITA_RAW_MESH, twins built)
    bool raw_on_ = true;      // ... and on (the A/B's arm)
    bool raw_verts_ = false;  // the raw vertex draws (TE_GFX_VITA_RAW_VERTS, the raw vertex path's twins built)
#endif
    bool BuildLeanPrograms(const char* vertex);
    // the GPU-skinning programs (gl_shader_library.cpp; the Vita only)
    SkinState* skin_ = nullptr;
    bool BuildSkinPrograms(const std::string& defines);
};

}  // namespace render
}  // namespace ps2r
