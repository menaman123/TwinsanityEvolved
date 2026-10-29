// render/backends/gl/gl_shader_sources.h -- GlShaderSources: accessors for the ONE copy
// of the GLSL text (PS2 draw program vertex + fragment, scanout quad vertex +
// fragment, the stencil-composite specialisations: strip-count / invert fragments
// over the PS2 vertex program, the constant-colour quad pair, the opaque-alpha stencil
// mark, and the folded CLEAR arms kept for the text gate). The text lives in
// gl_shader_sources.cpp as raw string literals.
#pragma once

namespace ps2r {
namespace render {

class GlShaderSources {
public:
    static const char* Ps2Vertex();
    static const char* Ps2Fragment();
    static const char* ScanoutVertex();
    static const char* ScanoutFragment();
    // The CT16 dithered scanout (the vertex is the plain scanout's)
    static const char* ScanoutDitherFragment();
    static const char* StencilClearFragment();
    static const char* StencilStripFragment();
    static const char* StencilInvertFragment();
    static const char* ConstVertex();
    static const char* ConstFragment();
    // The opaque-alpha stencil mark over the constant vertex program (MarkStencilWhereOpaque)
    static const char* OpaqueMaskFragment();
    // The Vita's screen copy (GlDevice::CopyRect): a texture drawn unchanged over the constant program's triangle
    static const char* CopyVertex();
    static const char* CopyFragment();
    // The Vita's lean PS2 fragment programs: one source, a variant's arms chosen by #defines put after its #version line
    // (LEAN_TEX, LEAN_TFX, LEAN_TCC, LEAN_MIP, LEAN_ATEST, LEAN_NOCOLOR: GlShaderLibrary::SelectPs2)
    static const char* LeanPs2Fragment();
    // The Vita's lean PS2 VERTEX programs (the lean fragments' partners): the PS2 vertex program's arithmetic for ONE draw
    // path, chosen by `#define LEAN_VS_PATH n` after the #version line -- 1 a native mesh (u_native 1, u_mesh_lit 0,
    // u_uv_mode 2), 2 a lit native mesh (u_mesh_lit 1), 3 vertices as converted (u_native 0, u_uv_mode 0), 4 a raw draw
    // (u_native 2, u_uv_mode 2) -- with only the attributes that path reads and no branch on a uniform
    static const char* LeanPs2Vertex();
    // The Vita's GPU-skinning vertex program (GlShaderLibrary::SelectSkin): linked with the lean fragment programs. Its
    // draws never take SelectPs2's vertex paths or raw mesh draws (GlDrawExecutor::DrawSkinMesh selects through SelectSkin)
    static const char* Ps2SkinVertex();
    // The Vita's INSTANCED native-mesh vertex program (GlShaderLibrary::DrawRawPacketInstances): the lean mesh
    // path's text (LEAN_VS_PATH 1) with the transform's five rows read from per-instance attributes a_mesh0..a_mesh4 (an
    // instance stream) instead of the uniform u_mesh[0..4] -- the same expressions in the same order. A source of its own:
    // the other programs' texts (their shader-cache keys) unchanged. Never drawn through GL (its GXM twin only).
    static const char* LeanPs2InstancedVertex();
};

}  // namespace render
}  // namespace ps2r
