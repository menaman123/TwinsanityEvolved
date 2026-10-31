// render/backends/gl/gl_state_cache.h -- GlStateCache: the GL binding / fixed-function
// state shadow.
//
// Every GL bind and fixed-function state call of the backend goes through this
// object; a call is skipped only when the shadow -- fed by every site -- already
// records the requested value, so the GL server state always equals what a raw
// call would have produced. Deletion follows the GL 4.1 rule that deleting a bound
// framebuffer / texture / buffer / vertex array reverts that binding to 0; a deleted
// program folds to "unknown" (a program in use is only flagged for deletion and its
// name may be reused). Texture parameters are shadowed per texture object so a name
// re-used after glGenTextures starts with a clean record. Single GL thread.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/platform/host/platform.h"   // PS2R_HOST_VITA

namespace ps2r {
namespace render {

class GlStateCache {
public:
    static constexpr uint32_t kUnknown = 0xFFFFFFFFu;
    static constexpr int kMaxUnits = 8;


    void InvalidateAll();

    // framebuffer
    void BindFb(uint32_t target, uint32_t fbo);
    // A function told right before the draw framebuffer binding changes (null: none) -- where a tile-based GPU's render
    // pass ends (the GPU phase timer cuts its queries there: gl_gpu_timers.h)
    void SetDrawFramebufferHook(void (*hook)(void*), void* user) {
        fb_hook_ = hook;
        fb_hook_user_ = user;
    }
    void DeleteFramebuffers(int n, const uint32_t* ids);
    // texture units / objects
    void ActiveTexture(uint32_t unit_enum);
    void BindTex2D(uint32_t tex);
    // `tex` bound on unit `unit_enum` (GL_TEXTUREn) without switching the active unit when that unit holds it already;
    // when a bind is made the active unit is `unit_enum` afterwards (a caller that sets texture parameters, or relies on
    // the active unit, selects it itself)
    void BindTex2DUnit(uint32_t unit_enum, uint32_t tex);
#if PS2R_HOST_VITA
    // The current program's te_size_<sampler> uniforms from the textures bound on the samplers' units (0-4: the PS2
    // program's samplers, u_frame on 0): the Vita's fragment profile has no textureSize, and its texelFetch is a sample at the texel centre
    // (gl_shader_library.cpp VitaGlsl). Call right before a draw, after the textures are bound.
    void ApplyVitaSamplerSizes();
#endif
    void TexParami(uint32_t pname, int param);
    void TexParamf(uint32_t pname, float param);
    void DeleteTextures(int n, const uint32_t* ids);
    // program / array buffer / viewport
    void UseProgram(uint32_t prog);
    void DeleteProgram(uint32_t prog);
    void BindArrayBuffer(uint32_t buf);
    void DeleteBuffers(int n, const uint32_t* ids);
    void Viewport(int x, int y, int w, int h);
    // fixed-function draw state
    void Enable(uint32_t cap);
    void Disable(uint32_t cap);
    void BlendEquationSeparate(uint32_t rgb, uint32_t a);
    void BlendFuncSeparate(uint32_t sr, uint32_t dr, uint32_t sa, uint32_t da);
    void BlendFunc(uint32_t s, uint32_t d);
    void BlendColor(float r, float g, float b, float a);
    void DepthFunc(uint32_t f);
    void DepthMask(bool m);
    void ColorMask(bool r, bool g, bool b, bool a);
    void Scissor(int x, int y, int w, int h);
    void BindVertexArray(uint32_t vao);
    void DeleteVertexArrays(int n, const uint32_t* ids);

    // the colour mask (bits 0..3 = r g b a; -1 unknown) and depth mask (1/0; -1 unknown) the cache records as set
    int ColorMaskBits() const {
        if (color_mask_[0] < 0 || color_mask_[1] < 0 || color_mask_[2] < 0 || color_mask_[3] < 0) return -1;
        return color_mask_[0] | color_mask_[1] << 1 | color_mask_[2] << 2 | color_mask_[3] << 3;
    }
    int DepthMaskBit() const { return depth_mask_; }
    // the texture the cache records as bound on unit `unit` (GL_TEXTURE_2D; kUnknown when not known)
    uint32_t Tex2DOnUnit(int unit) const { return unit >= 0 && unit < kMaxUnits ? tex2d_[unit] : kUnknown; }
    uint64_t Skips() const { return skips_; }
    // every GL call made through the cache, counted: equal counts bracket a stretch in which the cached state did not move
    uint64_t Mutations() const { return mutations_; }
    // The fixed-function state a draw rasterises under, as the cache records it (the bound draw framebuffer, the viewport,
    // blending, depth test / function / mask, the colour mask, the scissor): two equal snapshots = the same GL context for
    // a draw whatever was bound or set in between (programs, textures, arrays, uniforms aside). The Vita's mesh draw
    // packets compare them (GlDrawExecutor::DrawMeshPacket after a vertex draw of the run's own state).
    struct Context {
        uint32_t fbo, blend_eq_rgb, blend_eq_a, blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, depth_func;
        int32_t viewport[4], scissor[4];
        float blend_color[4];
        int8_t cap_blend, cap_depth, cap_scissor, depth_mask, color_mask[4];
        uint8_t viewport_known, scissor_known, blend_color_known, pad;
    };
    Context Snapshot() const {
        Context c;
        std::memset(&c, 0, sizeof c);   // (compared as bytes: the padding too)
        c.fbo = fbo_draw_;
        c.blend_eq_rgb = blend_eq_rgb_;
        c.blend_eq_a = blend_eq_a_;
        c.blend_src_rgb = blend_src_rgb_;
        c.blend_dst_rgb = blend_dst_rgb_;
        c.blend_src_a = blend_src_a_;
        c.blend_dst_a = blend_dst_a_;
        c.depth_func = depth_func_;
        for (int k = 0; k < 4; ++k) {
            c.viewport[k] = viewport_[k];
            c.scissor[k] = scissor_box_[k];
            c.blend_color[k] = blend_color_[k];
            c.color_mask[k] = color_mask_[k];
        }
        c.cap_blend = cap_blend_;
        c.cap_depth = cap_depth_;
        c.cap_scissor = cap_scissor_;
        c.depth_mask = depth_mask_;
        c.viewport_known = viewport_known_ ? 1 : 0;
        c.scissor_known = scissor_known_ ? 1 : 0;
        c.blend_color_known = blend_color_known_ ? 1 : 0;
        return c;
    }
    // every field a draw rasterises under is known (a field the cache does not know -- after InvalidateAll, before its first
    // set -- may hold anything: no snapshot with one stands for a context)
    static bool Known(const Context& c) {
        if (c.fbo == kUnknown || c.depth_func == kUnknown || !c.viewport_known || c.cap_blend < 0 || c.cap_depth < 0 ||
            c.cap_scissor < 0 || c.depth_mask < 0)
            return false;
        for (int k = 0; k < 4; ++k)
            if (c.color_mask[k] < 0) return false;
        if (c.cap_blend && (c.blend_eq_rgb == kUnknown || c.blend_eq_a == kUnknown || c.blend_src_rgb == kUnknown ||
                            c.blend_dst_rgb == kUnknown || c.blend_src_a == kUnknown || c.blend_dst_a == kUnknown))
            return false;
        return !c.cap_scissor || c.scissor_known;
    }

private:
    struct TexParamBucket {
        std::vector<std::pair<uint32_t, int32_t>> params;
    };
    bool TexParamCheckSet(uint32_t tex, uint32_t pname, int32_t bits);
    void SetCap(uint32_t cap, bool on);

    uint32_t fbo_draw_ = kUnknown;
    uint32_t fbo_read_ = kUnknown;
    void (*fb_hook_)(void*) = nullptr;
    void* fb_hook_user_ = nullptr;
    int active_unit_ = -1;
    uint32_t tex2d_[kMaxUnits] = {kUnknown, kUnknown, kUnknown, kUnknown,
                                  kUnknown, kUnknown, kUnknown, kUnknown};
    std::unordered_map<uint32_t, TexParamBucket> tex_params_;
    // the last texture's bucket (a draw sets 4-5 parameters of one texture in a row; an element's address holds until
    // it is erased: DeleteTextures / InvalidateAll drop it)
    uint32_t last_param_tex_ = kUnknown;
    TexParamBucket* last_param_bucket_ = nullptr;
#if PS2R_HOST_VITA
    // program -> its te_size_* locations and what each was last given (the texture, its size's generation): a draw
    // that samples the same textures sets none of them again
    struct VitaSizes {
        std::array<int32_t, 6> locs{};
        std::array<uint32_t, 6> tex{};
        std::array<uint32_t, 6> generation{};
        // the size each uniform was last given (-1: never): another texture of the same size sends nothing (a glUniform*
        // on the Vita dirties the program's whole default uniform buffer, copied again at the next draw)
        std::array<int32_t, 6> sent_w{{-1, -1, -1, -1, -1, -1}};
        std::array<int32_t, 6> sent_h{{-1, -1, -1, -1, -1, -1}};
    };
    // the program the last ApplyVitaSamplerSizes looked up (its entry: a run of draws of one program looks up nothing)
    uint32_t vita_sizes_program_ = kUnknown;
    VitaSizes* vita_sizes_entry_ = nullptr;
    std::unordered_map<uint32_t, VitaSizes> vita_sizes_;
#endif

    uint32_t program_ = kUnknown;
    uint32_t array_buffer_ = kUnknown;
    bool viewport_known_ = false;
    int viewport_[4] = {0, 0, 0, 0};

    int8_t cap_blend_ = -1, cap_depth_ = -1, cap_scissor_ = -1;
    uint32_t blend_eq_rgb_ = kUnknown, blend_eq_a_ = kUnknown;
    uint32_t blend_src_rgb_ = kUnknown, blend_dst_rgb_ = kUnknown;
    uint32_t blend_src_a_ = kUnknown, blend_dst_a_ = kUnknown;
    bool blend_color_known_ = false;
    float blend_color_[4] = {0, 0, 0, 0};
    uint32_t depth_func_ = kUnknown;
    int8_t depth_mask_ = -1;
    int8_t color_mask_[4] = {-1, -1, -1, -1};
    bool scissor_known_ = false;
    int scissor_box_[4] = {0, 0, 0, 0};
    uint32_t vao_ = kUnknown;
    uint64_t skips_ = 0;
    uint64_t mutations_ = 0;
};

}  // namespace render
}  // namespace ps2r
