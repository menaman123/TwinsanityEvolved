// render/backends/gl/gl_movie.cpp -- the Vita's movie pictures in GlDevice (Backend::SurfaceMemory / DrawMoviePicture;
// ps2e_draw_movie_* drives them). Every other host builds nothing here: the backend's defaults refuse, and the movie
// player keeps its RGBA uploads.
//
// A movie picture used to be one RGBA8 texture re-specified with glTexImage2D per picture: on the device a 1.2 MB vitaGL
// allocation, the picture copied into uncached GPU memory and the old storage handed to vitaGL's garbage collector -- on
// the DRAWING thread, ~28 ms a picture at 640x480 (device run7: 63 ms of the drawing thread per present during the boot
// movies, 54 % of the pictures replaced before they were drawn) -- drawn through the PS2 program (no lean variant takes
// TFX DECAL: the 229-instruction fast program). Here a picture's storage is made once per movie (a ring of slots, each
// slot's planes a CPU-writable surface: SurfaceDesc::cpu_writable, never pooled), the decoder writes each picture into a
// free slot's storage itself (SurfaceMemory: vitaGL's own texture memory), and a draw samples the slot shown with the
// movie program: the copy program's full-screen triangle over the picture's viewport -- RGBA: the copy program as it is;
// YUV: the planes on units 0-2 converted (GlShaderLibrary::MovieYuvProgram) -- no blending, no depth or stencil test, the
// device's state put back as the screen copy (VitaCopyRect) puts it back. Nothing is allocated or copied per picture on
// the drawing thread.
#include "engine/render/backends/gl/gl_device.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ps2r {
namespace render {

#if PS2R_HOST_VITA
void* GlDevice::SurfaceMemory(SurfaceId id, size_t* stride) {
    if (stride) *stride = 0;
    const GlSurface* s = Surface(id);
    if (!s || !s->tex || !s->desc.cpu_writable || s->desc.renderable || s->levels || s->synth_levels) return nullptr;
    const size_t bpp = s->desc.format == SurfaceFormat::R8 ? 1 : s->desc.format == SurfaceFormat::Rgba8 ? 4 : 0;
    if (!bpp) return nullptr;
    state_.ActiveTexture(GL_TEXTURE0);
    state_.BindTex2D(s->tex);
    void* data = vglGetTexDataPointer(GL_TEXTURE_2D);
    state_.BindTex2D(0);
    if (!data) return nullptr;
    // vitaGL's storage of a level-0 linear texture (gpu_alloc_texture): rows of the width aligned up to 8 texels
    if (stride) *stride = ((size_t(s->desc.width) + 7) & ~size_t(7)) * bpp;
    return data;
}

bool GlDevice::DrawMoviePicture(const MoviePicture& m) {
    const uint32_t prog = m.yuv ? shaders_.MovieYuvProgram() : shaders_.CopyProgram();
    const int src_loc = m.yuv ? shaders_.MovieYuvSourceLocation() : shaders_.CopySourceLocation();
    if (!prog || src_loc < 0) return false;
    const int n = m.yuv ? 3 : 1;
    uint32_t tex[3] = {0, 0, 0};
    float plane_w[3] = {0.f, 0.f, 0.f}, plane_h[3] = {0.f, 0.f, 0.f};
    for (int i = 0; i < n; ++i) {
        const GlSurface* s = Surface(m.planes[i]);
        if (!s || !s->tex) return false;
        tex[i] = s->tex;
        plane_w[i] = float(s->desc.width);
        plane_h[i] = float(s->desc.height);
    }
    if (m.yuv && shaders_.MovieYuvSizeLocation() < 0) return false;
    const GlSurface* c = Surface(m.color);
    if (!c || !c->fbo || m.target_w <= 0 || m.target_h <= 0 || !(m.x1 > m.x0) || !(m.y1 > m.y0)) return false;
    // The quad clipped to the target (its texture extent with it), in the target's physical pixels: GL's row 0 is the
    // frame's top row (the PS2 vertex program maps frame y 0 to NDC -1; the scanout flips), and v0 is the picture's top
    const float tw = float(m.target_w), th = float(m.target_h);
    const float x0 = std::clamp(m.x0, 0.f, tw), x1 = std::clamp(m.x1, 0.f, tw);
    const float y0 = std::clamp(m.y0, 0.f, th), y1 = std::clamp(m.y1, 0.f, th);
    if (!(x1 > x0) || !(y1 > y0)) return true;   // nothing of it on the target
    const float du = (m.u1 - m.u0) / (m.x1 - m.x0), dv = (m.v1 - m.v0) / (m.y1 - m.y0);
    const float u0 = m.u0 + (x0 - m.x0) * du, u1 = m.u0 + (x1 - m.x0) * du;
    const float v0 = m.v0 + (y0 - m.y0) * dv, v1 = m.v0 + (y1 - m.y0) * dv;
    const int scale = std::max(1, m.scale);
    const int vx = int(std::lround(x0 * float(scale))), vy = int(std::lround(y0 * float(scale)));
    const int vw = int(std::lround(x1 * float(scale))) - vx, vh = int(std::lround(y1 * float(scale))) - vy;
    if (vw <= 0 || vh <= 0) return true;
    // The geometry: the picture's viewport exactly. GXM's viewport is only a transform -- the Vita's GPU clips x / y at its
    // guard band, not at the viewport, and vitaGL bounds a draw only by its region clip (the whole framebuffer while the
    // scissor test is off) and its scissor mask -- so the copy program's full-screen triangle (NDC (-1,-1) (3,-1) (-1,3))
    // also paints up to a viewport's width right of the picture and its height below it wherever the target extends
    // there: nothing for a picture filling the target (the boot movies' {0 0 512 512}), the bottom letterbox bar for the
    // NEW GAME intro (H01_A's {0 112 512 288}: rows 400-511, the picture's last row smeared down by the clamped sampler).
    // Vita3K maps the viewport onto a host viewport, whose clip hides it: the emulator showed a clean intro. The quad
    // covers the same pixels the clipped triangle covers, with the same texture coordinates (the copy vertex shader's
    // affine map of the position). TE_MOVIE_QUAD=0: the full-screen triangle again (the device A/B).
    if (movie_quad_mode_ < 0) {
        const char* e = std::getenv("TE_MOVIE_QUAD");
        movie_quad_mode_ = e && *e && std::strcmp(e, "0") == 0 ? 0 : 1;
    }
    {
        // one line per picture box (a movie keeps its box): where the full-screen triangle would also have drawn
        const float box[6] = {x0, y0, x1, y1, tw, th};
        if (std::memcmp(box, movie_last_box_, sizeof(box)) != 0) {
            std::memcpy(movie_last_box_, box, sizeof(box));
            const float below = std::max(0.f, std::min(th, y1 + (y1 - y0)) - y1);
            const float right = std::max(0.f, std::min(tw, x1 + (x1 - x0)) - x1);
            const std::string extra = below > 0.f || right > 0.f
                ? "paints up to " + std::to_string(int(below)) + " rows below the picture and " + std::to_string(int(right)) +
                      " columns right of it as well"
                : std::string("paints nothing outside the picture inside the target");
            std::fprintf(stderr, "[gl] movie draw: the picture's box {%.0f %.0f %.0f %.0f} in the %.0fx%.0f target (scale %d, %s), "
                         "drawn as %s; on the Vita's GPU (it clips at its guard band, not at the viewport; Vita3K clips at the "
                         "viewport) the full-screen triangle %s\n",
                         x0, y0, x1 - x0, y1 - y0, tw, th, scale, m.yuv ? "YUV" : "RGBA",
                         movie_quad_mode_ == 1 ? "an exact quad over its viewport (TE_MOVIE_QUAD=1, the default)"
                                               : "the full-screen triangle (TE_MOVIE_QUAD=0)",
                         extra.c_str());
        }
    }
    EndDrawRun();
    if (movie_quad_mode_ == 1 && !movie_quad_vao_) {
        // (made between draw runs, at the first movie draw: two triangles over NDC -1..1, the position on attribute 0 as the
        // copy vertex shader reads it from const_vao_)
        static const float kMovieQuad[12] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f};
        glGenVertexArrays(1, &movie_quad_vao_);
        glGenBuffers(1, &movie_quad_vbo_);
        state_.BindVertexArray(movie_quad_vao_);
        state_.BindArrayBuffer(movie_quad_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kMovieQuad), kMovieQuad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        state_.BindVertexArray(0);
        if (!movie_quad_vao_ || !movie_quad_vbo_) movie_quad_mode_ = 0;   // (no GL objects: the triangle, as before)
    }
    const bool quad = movie_quad_mode_ == 1 && movie_quad_vao_;
    if (!BindTarget(m.color, m.depth)) {
        BeginDrawRun();
        return false;
    }
    state_.Disable(GL_SCISSOR_TEST);
    state_.Disable(GL_BLEND);
    state_.Disable(GL_DEPTH_TEST);   // vitaGL: no depth test = no depth write either (the movie quad is the frame's last draw)
    glDisable(GL_STENCIL_TEST);      // (each stencil draw resets it after itself; a movie frame has none)
    state_.ColorMask(true, true, true, true);
    state_.Viewport(vx, vy, vw, vh);
    state_.UseProgram(prog);
    glUniform4f(src_loc, u0, v0, u1 - u0, v1 - v0);
    if (m.yuv) glUniform4f(shaders_.MovieYuvSizeLocation(), plane_w[0], plane_h[0], plane_w[1], plane_h[1]);
    const uint32_t sampler = m.linear ? present_linear_ : present_nearest_;
    uint32_t kept[3] = {GlStateCache::kUnknown, GlStateCache::kUnknown, GlStateCache::kUnknown};
    for (int i = 0; i < n; ++i) {
        kept[i] = state_.Tex2DOnUnit(i);
        state_.ActiveTexture(GL_TEXTURE0 + uint32_t(i));
        state_.BindTex2D(tex[i]);
        glBindSampler(GLuint(i), sampler);
    }
    state_.BindVertexArray(quad ? movie_quad_vao_ : const_vao_);
    glDrawArrays(GL_TRIANGLES, 0, quad ? 6 : 3);
    state_.BindVertexArray(0);
    // units 1-2 back to what they held (the PS2 draws leave units 2-4 bound across draws), unit 0 empty as the copy leaves it
    for (int i = n - 1; i >= 0; --i) {
        glBindSampler(GLuint(i), 0);
        state_.ActiveTexture(GL_TEXTURE0 + uint32_t(i));
        state_.BindTex2D(i == 0 ? 0 : kept[i] != GlStateCache::kUnknown ? kept[i] : dummy_tex_);
    }
    state_.ActiveTexture(GL_TEXTURE0);
    state_.UseProgram(0);
    state_.Viewport(0, 0, m.target_w * scale, m.target_h * scale);
    state_.Enable(GL_DEPTH_TEST);   // the device's default (Present leaves it so too)
    BeginDrawRun();
    return true;
}
#endif

}  // namespace render
}  // namespace ps2r
