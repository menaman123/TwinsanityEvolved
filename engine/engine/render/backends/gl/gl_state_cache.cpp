// render/backends/gl/gl_state_cache.cpp -- GlStateCache implementation.
#include "engine/render/backends/gl/gl_state_cache.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <cstring>

#include "engine/render/backends/gl/gl_census.h"

namespace ps2r {
namespace render {

void GlStateCache::InvalidateAll() {
    fbo_draw_ = fbo_read_ = kUnknown;
    active_unit_ = -1;
    for (int i = 0; i < kMaxUnits; i++) tex2d_[i] = kUnknown;
    tex_params_.clear();
    last_param_tex_ = kUnknown;
    last_param_bucket_ = nullptr;
    program_ = kUnknown;
    array_buffer_ = kUnknown;
    viewport_known_ = false;
    cap_blend_ = cap_depth_ = cap_scissor_ = -1;
    blend_eq_rgb_ = blend_eq_a_ = kUnknown;
    blend_src_rgb_ = blend_dst_rgb_ = blend_src_a_ = blend_dst_a_ = kUnknown;
    blend_color_known_ = false;
    depth_func_ = kUnknown;
    depth_mask_ = -1;
    color_mask_[0] = color_mask_[1] = color_mask_[2] = color_mask_[3] = -1;
    scissor_known_ = false;
    vao_ = kUnknown;
}

// --- framebuffer -----------------------------------------------------------------

void GlStateCache::BindFb(uint32_t target, uint32_t fbo) {
    if (target == GL_FRAMEBUFFER) {
        if (fbo_draw_ == fbo && fbo_read_ == fbo) return;
        if (fb_hook_ && fbo_draw_ != fbo) fb_hook_(fb_hook_user_);
        fbo_draw_ = fbo_read_ = fbo;
    } else if (target == GL_DRAW_FRAMEBUFFER) {
        if (fbo_draw_ == fbo) return;
        if (fb_hook_) fb_hook_(fb_hook_user_);
        fbo_draw_ = fbo;
    } else if (target == GL_READ_FRAMEBUFFER) {
        if (fbo_read_ == fbo) return;
        fbo_read_ = fbo;
    }
    ++mutations_;
    glBindFramebuffer(target, fbo);
}

void GlStateCache::DeleteFramebuffers(int n, const uint32_t* ids) {
    for (int i = 0; i < n; i++) {
        if (fbo_draw_ == ids[i]) fbo_draw_ = 0;
        if (fbo_read_ == ids[i]) fbo_read_ = 0;
    }
    ++mutations_;
    glDeleteFramebuffers(n, ids);
}

// --- texture units / objects -----------------------------------------------------

void GlStateCache::ActiveTexture(uint32_t unit_enum) {
    const int idx = static_cast<int>(unit_enum - GL_TEXTURE0);
    if (idx >= 0 && idx < kMaxUnits && active_unit_ == idx) return;
    active_unit_ = (idx >= 0 && idx < kMaxUnits) ? idx : -1;
    ++mutations_;
    glActiveTexture(unit_enum);
}

void GlStateCache::BindTex2D(uint32_t tex) {
    if (active_unit_ < 0) {
        ++mutations_;
        glBindTexture(GL_TEXTURE_2D, tex);
        return;
    }
    if (tex2d_[active_unit_] == tex) return;
    tex2d_[active_unit_] = tex;
    ++mutations_;
    glBindTexture(GL_TEXTURE_2D, tex);
}

void GlStateCache::BindTex2DUnit(uint32_t unit_enum, uint32_t tex) {
    const int idx = static_cast<int>(unit_enum - GL_TEXTURE0);
    if (idx >= 0 && idx < kMaxUnits && tex2d_[idx] == tex) {
        skips_++;
        return;
    }
    ActiveTexture(unit_enum);
    BindTex2D(tex);
}

bool GlStateCache::TexParamCheckSet(uint32_t tex, uint32_t pname, int32_t bits) {
    if (tex != last_param_tex_ || !last_param_bucket_) {
        last_param_bucket_ = &tex_params_[tex];
        last_param_tex_ = tex;
    }
    TexParamBucket& b = *last_param_bucket_;
    for (auto& pv : b.params) {
        if (pv.first == pname) {
            if (pv.second == bits) return true;
            pv.second = bits;
            return false;
        }
    }
    b.params.emplace_back(pname, bits);
    return false;
}

void GlStateCache::TexParami(uint32_t pname, int param) {
    if (active_unit_ < 0 || tex2d_[active_unit_] == kUnknown || tex2d_[active_unit_] == 0) {
        ++mutations_;
        glTexParameteri(GL_TEXTURE_2D, pname, param);
        return;
    }
    if (TexParamCheckSet(tex2d_[active_unit_], pname, param)) { skips_++; return; }
    ++mutations_;
    glTexParameteri(GL_TEXTURE_2D, pname, param);
}

void GlStateCache::TexParamf(uint32_t pname, float param) {
    if (active_unit_ < 0 || tex2d_[active_unit_] == kUnknown || tex2d_[active_unit_] == 0) {
        ++mutations_;
        glTexParameterf(GL_TEXTURE_2D, pname, param);
        return;
    }
    int32_t bits = 0;
    std::memcpy(&bits, &param, sizeof(bits));
    if (TexParamCheckSet(tex2d_[active_unit_], pname, bits)) { skips_++; return; }
    ++mutations_;
    glTexParameterf(GL_TEXTURE_2D, pname, param);
}

void GlStateCache::DeleteTextures(int n, const uint32_t* ids) {
    for (int i = 0; i < n; i++) {
        for (int u = 0; u < kMaxUnits; u++)
            if (tex2d_[u] == ids[i]) tex2d_[u] = 0;
        if (last_param_tex_ == ids[i]) {
            last_param_tex_ = kUnknown;
            last_param_bucket_ = nullptr;
        }
        tex_params_.erase(ids[i]);
    }
    ++mutations_;
    glDeleteTextures(n, ids);
}

// --- program / array buffer / viewport ------------------------------------------

#if PS2R_HOST_VITA
void GlStateCache::ApplyVitaSamplerSizes() {
    if (program_ == kUnknown || program_ == 0) return;
    static const struct { const char* name; int unit; } kSizes[6] = {
        {"te_size_u_texture", 0}, {"te_size_u_frame", 0}, {"te_size_u_rt_texture", 1},
        {"te_size_u_clut", 2},    {"te_size_u_idx_tex2", 3}, {"te_size_u_zsrc_tex", 4},
    };
    if (program_ != vita_sizes_program_ || !vita_sizes_entry_) {
        auto it = vita_sizes_.find(program_);
        if (it == vita_sizes_.end()) {
            VitaSizes sizes;
            for (int i = 0; i < 6; ++i) sizes.locs[i] = glGetUniformLocation(program_, kSizes[i].name);
            it = vita_sizes_.emplace(program_, sizes).first;
        }
        vita_sizes_program_ = program_;
        vita_sizes_entry_ = &it->second;   // (std::unordered_map: the element's address holds until it is erased)
    }
    VitaSizes& sizes = *vita_sizes_entry_;
    const uint32_t generation = ps2r_vita_TexGeneration();
    for (int i = 0; i < 6; ++i) {
        const int32_t loc = sizes.locs[i];
        const uint32_t tex = tex2d_[kSizes[i].unit];
        if (loc < 0 || tex == kUnknown || tex == 0) continue;
        if (sizes.tex[i] == tex && sizes.generation[i] == generation) continue;
        int w = 0, h = 0;
        if (!ps2r_vita_TexSize(tex, &w, &h)) continue;
        sizes.tex[i] = tex;
        sizes.generation[i] = generation;
        if (sizes.sent_w[i] == w && sizes.sent_h[i] == h) {
            Census().size_skipped++;   // the uniform holds this size: nothing to send
            continue;
        }
        ++mutations_;
        Census().size_uniforms++;
        glUniform2f(loc, float(w), float(h));
        sizes.sent_w[i] = w;
        sizes.sent_h[i] = h;
    }
}
#endif

void GlStateCache::UseProgram(uint32_t prog) {
    if (program_ == prog) { skips_++; return; }
    program_ = prog;
    ++mutations_;
    glUseProgram(prog);
}

void GlStateCache::DeleteProgram(uint32_t prog) {
    if (program_ == prog) program_ = kUnknown;
#if PS2R_HOST_VITA
    vita_sizes_.erase(prog);
    vita_sizes_program_ = kUnknown;
    vita_sizes_entry_ = nullptr;
#endif
    ++mutations_;
    glDeleteProgram(prog);
}

void GlStateCache::BindArrayBuffer(uint32_t buf) {
    if (array_buffer_ == buf) { skips_++; return; }
    array_buffer_ = buf;
    ++mutations_;
    glBindBuffer(GL_ARRAY_BUFFER, buf);
}

void GlStateCache::DeleteBuffers(int n, const uint32_t* ids) {
    for (int i = 0; i < n; i++)
        if (array_buffer_ == ids[i]) array_buffer_ = 0;
    ++mutations_;
    glDeleteBuffers(n, ids);
}

void GlStateCache::Viewport(int x, int y, int w, int h) {
    if (viewport_known_ && viewport_[0] == x && viewport_[1] == y && viewport_[2] == w &&
        viewport_[3] == h) {
        skips_++;
        return;
    }
    viewport_known_ = true;
    viewport_[0] = x; viewport_[1] = y; viewport_[2] = w; viewport_[3] = h;
    ++mutations_;
    glViewport(x, y, w, h);
}

// --- fixed-function draw state ---------------------------------------------------

void GlStateCache::SetCap(uint32_t cap, bool on) {
    int8_t* s = nullptr;
    if (cap == GL_BLEND) s = &cap_blend_;
    else if (cap == GL_DEPTH_TEST) s = &cap_depth_;
    else if (cap == GL_SCISSOR_TEST) s = &cap_scissor_;
    if (s) {
        if (*s == static_cast<int8_t>(on ? 1 : 0)) { skips_++; return; }
        *s = static_cast<int8_t>(on ? 1 : 0);
    }
    ++mutations_;
    if (on) glEnable(cap); else glDisable(cap);
}

void GlStateCache::Enable(uint32_t cap) { SetCap(cap, true); }
void GlStateCache::Disable(uint32_t cap) { SetCap(cap, false); }

void GlStateCache::BlendEquationSeparate(uint32_t rgb, uint32_t a) {
    if (blend_eq_rgb_ == rgb && blend_eq_a_ == a) { skips_++; return; }
    blend_eq_rgb_ = rgb;
    blend_eq_a_ = a;
    ++mutations_;
    glBlendEquationSeparate(rgb, a);
}

void GlStateCache::BlendFuncSeparate(uint32_t sr, uint32_t dr, uint32_t sa, uint32_t da) {
    if (blend_src_rgb_ == sr && blend_dst_rgb_ == dr && blend_src_a_ == sa &&
        blend_dst_a_ == da) {
        skips_++;
        return;
    }
    blend_src_rgb_ = sr; blend_dst_rgb_ = dr;
    blend_src_a_ = sa;   blend_dst_a_ = da;
    ++mutations_;
    glBlendFuncSeparate(sr, dr, sa, da);
}

void GlStateCache::BlendFunc(uint32_t s, uint32_t d) { BlendFuncSeparate(s, d, s, d); }

void GlStateCache::BlendColor(float r, float g, float b, float a) {
    if (blend_color_known_ && blend_color_[0] == r && blend_color_[1] == g &&
        blend_color_[2] == b && blend_color_[3] == a) {
        skips_++;
        return;
    }
    blend_color_known_ = true;
    blend_color_[0] = r; blend_color_[1] = g; blend_color_[2] = b; blend_color_[3] = a;
#if PS2R_HOST_VITA
    (void)r; (void)g; (void)b; (void)a;   // vitaGL has no constant-colour blend factor
#else
    ++mutations_;
    glBlendColor(r, g, b, a);
#endif
}

void GlStateCache::DepthFunc(uint32_t f) {
    if (depth_func_ == f) { skips_++; return; }
    depth_func_ = f;
    ++mutations_;
    glDepthFunc(f);
}

void GlStateCache::DepthMask(bool m) {
    const int8_t v = m ? 1 : 0;
    if (depth_mask_ == v) { skips_++; return; }
    depth_mask_ = v;
    ++mutations_;
    glDepthMask(m ? GL_TRUE : GL_FALSE);
}

void GlStateCache::ColorMask(bool r, bool g, bool b, bool a) {
    const int8_t vr = r, vg = g, vb = b, va = a;
    if (color_mask_[0] == vr && color_mask_[1] == vg && color_mask_[2] == vb &&
        color_mask_[3] == va) {
        skips_++;
        return;
    }
    color_mask_[0] = vr; color_mask_[1] = vg; color_mask_[2] = vb; color_mask_[3] = va;
    ++mutations_;
    glColorMask(r, g, b, a);
}

void GlStateCache::Scissor(int x, int y, int w, int h) {
    if (scissor_known_ && scissor_box_[0] == x && scissor_box_[1] == y && scissor_box_[2] == w &&
        scissor_box_[3] == h) {
        skips_++;
        return;
    }
    scissor_known_ = true;
    scissor_box_[0] = x; scissor_box_[1] = y; scissor_box_[2] = w; scissor_box_[3] = h;
    ++mutations_;
    glScissor(x, y, w, h);
}

void GlStateCache::BindVertexArray(uint32_t vao) {
    if (vao_ == vao) { skips_++; return; }
    vao_ = vao;
    // GL_ARRAY_BUFFER binding is not vertex-array state (GL 4.1), but a real VAO
    // bind drops the array-buffer shadow so no spec corner can be argued.
    array_buffer_ = kUnknown;
    ++mutations_;
    glBindVertexArray(vao);
}

void GlStateCache::DeleteVertexArrays(int n, const uint32_t* ids) {
    for (int i = 0; i < n; i++)
        if (vao_ == ids[i]) vao_ = 0;
    ++mutations_;
    glDeleteVertexArrays(n, ids);
}

}  // namespace render
}  // namespace ps2r
