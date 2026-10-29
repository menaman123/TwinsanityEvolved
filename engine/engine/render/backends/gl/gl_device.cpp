// render/backends/gl/gl_device.cpp -- GlDevice implementation.
#include "engine/render/backends/gl/gl_device.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "engine/render/backends/gl/gl_census.h"
#include "engine/render/backends/gl/gl_draw_executor.h"
#include "engine/render/backends/gl/gl_fill_census.h"
#include "engine/render/mesh_index.h"

#if PS2R_HOST_VITA
// vitaGL r1488 (source/gxm.c): `framebuffer *in_use_framebuffer`, the framebuffer of the GXM scene in progress (NULL =
// the display). Read only, by the screen copies' depth keep (GlDevice::VitaCheckFramebufferNames, KeepDepthAcrossCopies,
// CopyRect). Weak: a vitaGL without it still links (its address is then null) and the depth keep stays off.
extern "C" {
extern void* in_use_framebuffer __attribute__((weak));
// vitaGL r1488 (gxm.c, tests.c): `framebuffer *old_framebuffer`, the framebuffer of the last scene begun -- scene_reset
// draws its scene-start mask-update quad (update_scissor_test) when the next scene's framebuffer differs --, and the GL
// scissor test with its pending update. Read, and old_framebuffer written, by the screen copy's return to the frame
// (GlDevice::VitaCopyRect: the frame's scene continued without that quad). Weak: a vitaGL without them still links (their
// addresses are then null) and every copy restores the frame as before.
extern void* old_framebuffer __attribute__((weak));
extern GLboolean scissor_test_state __attribute__((weak));
extern GLboolean dirty_scissor_state __attribute__((weak));
}
#endif

namespace ps2r {
namespace render {

GlDevice::GlDevice(const GlDeviceSettings& settings) : settings_(settings) {}

GlDevice::~GlDevice() { Shutdown(); }

bool GlDevice::Init(std::function<void()> swap) {
    Shutdown();
    swap_ = std::move(swap);
    state_.InvalidateAll();
    // Window depth is z / 2^32 either way; with clip control (GL 4.5 or ARB_clip_control, which 4.1 contexts carry
    // on the desktop drivers) it needs no -1..1 detour, which costs the float depth the low 7 bits of Z
    bool zero_to_one = false;
#if defined(_WIN32)
    if (ps2r_glwin_glClipControl) {
        GLint extensions = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
        for (GLint i = 0; i < extensions && !zero_to_one; ++i) {
            const GLubyte* name = glGetStringi(GL_EXTENSIONS, GLuint(i));
            zero_to_one = name && std::strcmp(reinterpret_cast<const char*>(name), "GL_ARB_clip_control") == 0;
        }
        if (zero_to_one) {
            glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
            zero_to_one = glGetError() == GL_NO_ERROR;
        }
    }
#endif
    shaders_.SetZeroToOneDepth(zero_to_one);
    shaders_.SetGsZ24Depth(settings_.gs_z24);
    if (!shaders_.Build(uniforms_)) return false;
    if (!vertices_.Init(state_)) return false;

    // Scanout quad: NDC positions + texcoords with V flipped (PS2 top at GL bottom).
    static const float kQuad[] = {
        -1.f, -1.f, 0.f, 1.f,  1.f, -1.f, 1.f, 1.f,  -1.f, 1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 1.f,  1.f,  1.f, 1.f, 0.f,  -1.f, 1.f, 0.f, 0.f,
    };
    glGenVertexArrays(1, &scanout_vao_);
    glGenBuffers(1, &scanout_vbo_);
    state_.BindVertexArray(scanout_vao_);
    state_.BindArrayBuffer(scanout_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_DYNAMIC_DRAW);
#if PS2R_HOST_VITA
    scanout_quad_valid_ = false;   // the buffer holds kQuad, not a present's quad
#endif
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    state_.BindVertexArray(0);

    // A window filter belongs to presentation, not the shared color image.
    // Sampler objects keep mixed draws and later retained presents independent
    // of the explicit owned filter; no texture parameter repair is needed.
    const auto sampler = [](uint32_t& object,GLint filter) {
        glGenSamplers(1,&object);
        glSamplerParameteri(object,GL_TEXTURE_MIN_FILTER,filter);
        glSamplerParameteri(object,GL_TEXTURE_MAG_FILTER,filter);
        glSamplerParameteri(object,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glSamplerParameteri(object,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    };
    sampler(present_nearest_,GL_NEAREST);
    sampler(present_linear_,GL_LINEAR);

    // NDC fullscreen triangle for the stencil-masked constant quads.
    static const float kTri[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    glGenVertexArrays(1, &const_vao_);
    glGenBuffers(1, &const_vbo_);
    state_.BindVertexArray(const_vao_);
    state_.BindArrayBuffer(const_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kTri), kTri, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    state_.BindVertexArray(0);

    // GL state defaults: GEQUAL depth (the GS default), far plane cleared to 0.0.
    state_.Enable(GL_DEPTH_TEST);
    state_.DepthFunc(GL_GEQUAL);
    glClearDepth(0.0);
    state_.Enable(GL_BLEND);
    state_.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 1x1 white dummy for every sampler unit a draw leaves unused.
    const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &dummy_tex_);
    state_.ActiveTexture(GL_TEXTURE0);
    state_.BindTex2D(dummy_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    state_.TexParami(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    state_.TexParami(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    state_.BindTex2D(0);
    for (GLenum unit : {GL_TEXTURE1, GL_TEXTURE2, GL_TEXTURE3, GL_TEXTURE4}) {
        state_.ActiveTexture(unit);
        state_.BindTex2D(dummy_tex_);
    }
    state_.ActiveTexture(GL_TEXTURE0);

    GLint mts = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mts);
    max_texture_size_ = mts;
    // Anisotropic filtering (EXT/ARB_texture_filter_anisotropic; a GL 4.1 core
    // context exposes it as an extension): the limit is 1 without it, and the
    // executor never asks the driver for more than the limit.
    max_anisotropy_ = 1;
    GLint extensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    for (GLint i = 0; i < extensions; ++i) {
        const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (!name) continue;
        if (std::strcmp(name, "GL_EXT_texture_filter_anisotropic") == 0 ||
            std::strcmp(name, "GL_ARB_texture_filter_anisotropic") == 0) {
            GLfloat limit = 1.0f;
            glGetFloatv(kGlMaxTextureMaxAnisotropy, &limit);
            max_anisotropy_ = std::max(1, static_cast<int>(limit));
            break;
        }
    }
    executor_ = std::make_unique<GlDrawExecutor>(*this);
#if PS2R_HOST_VITA
    {
        // the per-draw call sequence (TE_GFX_DRAW_LEGACY=1: as before the drawing thread's rework, for an A/B; TE_AB=draws flips it in-run)
        const char* l = std::getenv("TE_GFX_DRAW_LEGACY");
        if (l && l[0] == '1') SetOption("draw_legacy", 1);
        std::fprintf(stderr, "[gl] per-draw sequence: %s\n", executor_->Legacy()
                     ? "legacy (TE_GFX_DRAW_LEGACY=1: both PS2 programs bound per draw, uniforms sent to both at once, masks reset per draw)"
                     : "lean (the draw's program bound once, uniforms synced to it alone, units 2-4 left bound, masks set per draw)");
    }
#endif
#if PS2R_HOST_VITA
    {
        const char* e = std::getenv("TE_GFX_VITA_STENCIL");
        vita_scene_stencil_ = !(e && e[0] == '0');
        std::fprintf(stderr, "[gl] shadow counter: %s\n", vita_scene_stencil_
                     ? "cleared in the frame's framebuffer, in its scene (vitaGL: one depth/stencil per framebuffer and scene)"
                     : "the stencil-only framebuffer (TE_GFX_VITA_STENCIL=0)");
        const char* c = std::getenv("TE_GFX_VITA_COPY");
        vita_gpu_copy_ = !(c && c[0] == '0') && shaders_.CopyProgram() && shaders_.CopySourceLocation() >= 0;
        // diagnosis: TE_GFX_VITA_COPY=none takes no copy at all (the copy texture keeps what it holds; no scene break):
        // the screen copy's own cost against a run that draws everything else alike
        vita_copy_none_ = c && std::strcmp(c, "none") == 0;
        if (vita_copy_none_) std::fprintf(stderr, "[gl] TE_GFX_VITA_COPY=none: NO screen copies are taken (diagnosis)\n");
        // diagnosis: TE_GFX_VITA_COPY=ab[N] flips between the copies (on) and none (off) every N presents (default 120),
        // starting on, logged as `[gl] copy A/B: present P: on|off` -- an in-run A/B of the copy's cost (both arms share
        // the host's load, which separate emulator runs do not)
        if (c && std::strncmp(c, "ab", 2) == 0) {
            vita_copy_ab_ = c[2] ? std::max(1, std::atoi(c + 2)) : 120;
            std::fprintf(stderr, "[gl] TE_GFX_VITA_COPY=ab: the screen copy on / off every %d presents (diagnosis)\n",
                         vita_copy_ab_);
        }
        // the fast PS2 program's in-run A/B (TE_GFX_VITA_PS2_FAST=abN: off / on every N presents, starting on)
        const char* f = std::getenv("TE_GFX_VITA_PS2_FAST");
        if (f && std::strncmp(f, "ab", 2) == 0 && shaders_.Ps2FastProgram()) {
            vita_ps2_fast_ab_ = f[2] ? std::max(1, std::atoi(f + 2)) : 600;
            std::fprintf(stderr, "[gl] TE_GFX_VITA_PS2_FAST=ab: the PS2 fast program on / off every %d presents (diagnosis)\n",
                         vita_ps2_fast_ab_);
        }
        std::fprintf(stderr, "[gl] PS2 fast program: %s\n", !shaders_.Ps2FastProgram() ? "not built"
                     : vita_ps2_fast_ab_ ? "on, switched off / on in turns (A/B)" : "on");
        // the lean PS2 programs' in-run A/B (TE_GFX_VITA_PS2_LEAN=abN: on / off every N presents, starting on; the off arm
        // draws as before them: the fast program, else the full one); it takes the census arms over from the fast A/B
        const char* lean = std::getenv("TE_GFX_VITA_PS2_LEAN");
        if (lean && std::strncmp(lean, "ab", 2) == 0 && shaders_.Ps2LeanBuilt()) {
            vita_ps2_lean_ab_ = lean[2] ? std::max(1, std::atoi(lean + 2)) : 600;
            vita_ps2_fast_ab_ = 0;
            std::fprintf(stderr, "[gl] TE_GFX_VITA_PS2_LEAN=ab: the lean PS2 programs on / off every %d presents (diagnosis)\n",
                         vita_ps2_lean_ab_);
        }
        // the raw mesh draws' in-run A/B (TE_GFX_VITA_RAW_MESH=abN: on / off every N presents, starting on; the off arm draws
        // every mesh through vitaGL's glDrawArrays, as before them)
        const char* rawm = std::getenv("TE_GFX_VITA_RAW_MESH");
        if (rawm && std::strncmp(rawm, "ab", 2) == 0 && shaders_.RawMeshBuilt()) {
            vita_raw_ab_ = rawm[2] ? std::max(1, std::atoi(rawm + 2)) : 600;
            std::fprintf(stderr, "[gl] TE_GFX_VITA_RAW_MESH=ab: the raw mesh draws on / off every %d presents (diagnosis)\n",
                         vita_raw_ab_);
        }
        std::fprintf(stderr, "[gl] raw mesh draws: %s\n", !shaders_.RawMeshBuilt() ? "not built (TE_GFX_VITA_RAW_MESH=0, or no twin)"
                     : vita_raw_ab_ ? "on, switched off / on in turns (A/B)" : "on");
        // the mesh draw packets' in-run A/B (TE_GFX_VITA_PACKETS=abN: on / off every N presents, starting on; the off arm
        // draws every again / texture-swap mesh draw through the executor's paths, as before them)
        const char* pk = std::getenv("TE_GFX_VITA_PACKETS");
        if (pk && std::strncmp(pk, "ab", 2) == 0 && shaders_.RawMeshBuilt()) {
            vita_packets_ab_ = pk[2] ? std::max(1, std::atoi(pk + 2)) : 600;
            std::fprintf(stderr, "[gl] TE_GFX_VITA_PACKETS=ab: the mesh draw packets on / off every %d presents (diagnosis)\n",
                         vita_packets_ab_);
        }
        Census().ab_name = vita_ps2_lean_ab_ ? "lean PS2 programs" : "PS2 fast program";
        std::fprintf(stderr, "[gl] lean PS2 programs: %s\n", !shaders_.Ps2LeanBuilt() ? "not built"
                     : vita_ps2_lean_ab_ ? "on, switched off / on in turns (A/B)" : "on");
        const char* k = std::getenv("TE_GFX_VITA_KEEP_DEPTH");
        vita_keep_depth_ = !(k && k[0] == '0');
        const char* rs = std::getenv("TE_GFX_VITA_COPY_RESTORE");
        vita_copy_restore_ = !(rs && rs[0] == '0');
        vita_copy_continue_ = !rs || !rs[0] || std::strcmp(rs, "auto") == 0;
        vita_copy_continue_ = vita_copy_continue_ && &old_framebuffer && &scissor_test_state && &dirty_scissor_state;
        const char* smp = std::getenv("TE_GFX_VITA_COPY_SAMPLE");
        vita_copy_sample_base_ = !(smp && std::strcmp(smp, "game") == 0);
        if (vita_copy_continue_)
            std::fprintf(stderr, "[gl] screen copy texture after a quiet copy (TE_GFX_VITA_COPY_SAMPLE): %s\n",
                         vita_copy_sample_base_
                             ? "sampled as after a restore (its vitaGL record re-stated through the copy's nearest sampler: "
                               "the pictures of the copy with its restore)"
                             : "game -- sampled as the material asks (not re-stated: the refraction filtered as on the "
                               "desktop; a different picture from the copy with its restore)");
        std::fprintf(stderr, "[gl] screen copy: %s%s; the frame's depth/stencil across a copy's scene break: %s\n",
                     vita_gpu_copy_ ? "drawn on the GPU into the copy's framebuffer"
                                    : "glCopyTexSubImage2D (vitaGL: glReadPixels = a CPU read-back after sceGxmFinish)",
                     vita_gpu_copy_ ? (vita_copy_continue_
                                           ? ", the frame's scene continued after it (no scene-start mask update, nothing to "
                                             "restore; else restored from the copy) -- TE_GFX_VITA_COPY_RESTORE=1: always restored"
                                       : vita_copy_restore_ ? ", the frame restored from it after the switch back"
                                                            : ", no restore (TE_GFX_VITA_COPY_RESTORE=0)") : "",
                     vita_keep_depth_ ? "kept (force store, then force load, in frames with copies)"
                                      : "lost (TE_GFX_VITA_KEEP_DEPTH=0: background depth 1.0 / stencil 0 after a copy)");
    }
#endif
    initialized_ = true;
    return true;
}

void GlDevice::Shutdown() {
    if (!initialized_) return;
    executor_.reset();
    for (auto& kv : meshes_) {
        state_.DeleteBuffers(1, &kv.second.vbo);
        state_.DeleteVertexArrays(1, &kv.second.vao);
    }
    meshes_.clear();
    for (auto& kv : surfaces_) ReleaseSurface(kv.second);
    surfaces_.clear();
    for (uint32_t i = 0; i < kLookupSlots; ++i) surface_ids_[i] = 0;
    for (uint32_t i = 0; i < kMeshSlots; ++i) mesh_slots_[i] = MeshSlot{};
    pool_.DropAll(state_);
    readbacks_.clear();
    if (dummy_tex_) { state_.DeleteTextures(1, &dummy_tex_); dummy_tex_ = 0; }
    vertices_.Shutdown(state_);
    if (scanout_vbo_) { state_.DeleteBuffers(1, &scanout_vbo_); scanout_vbo_ = 0; }
    if (scanout_vao_) { state_.DeleteVertexArrays(1, &scanout_vao_); scanout_vao_ = 0; }
    if (present_nearest_) { glDeleteSamplers(1,&present_nearest_); present_nearest_ = 0; }
    if (present_linear_) { glDeleteSamplers(1,&present_linear_); present_linear_ = 0; }
    if (const_vbo_) { state_.DeleteBuffers(1, &const_vbo_); const_vbo_ = 0; }
    if (const_vao_) { state_.DeleteVertexArrays(1, &const_vao_); const_vao_ = 0; }
    if (sten_fbo_) { state_.DeleteFramebuffers(1, &sten_fbo_); sten_fbo_ = 0; }
    if (res_fbo_) { state_.DeleteFramebuffers(1, &res_fbo_); res_fbo_ = 0; }
#if PS2R_HOST_VITA
    if (null_vbo_) { state_.DeleteBuffers(1, &null_vbo_); null_vbo_ = 0; }
    if (null_vao_) { state_.DeleteVertexArrays(1, &null_vao_); null_vao_ = 0; }
    if (copy_fbo_) { state_.DeleteFramebuffers(1, &copy_fbo_); copy_fbo_ = 0; }
    copy_fbo_tex_ = 0;
    vita_keep_fbo_ = 0;
    if (movie_quad_vbo_) { state_.DeleteBuffers(1, &movie_quad_vbo_); movie_quad_vbo_ = 0; }
    if (movie_quad_vao_) { state_.DeleteVertexArrays(1, &movie_quad_vao_); movie_quad_vao_ = 0; }
#endif
    if (capture_fbo_) { state_.DeleteFramebuffers(1, &capture_fbo_); capture_fbo_ = 0; }
    if (capture_tex_) { state_.DeleteTextures(1, &capture_tex_); capture_tex_ = 0; }
    capture_w_ = capture_h_ = 0;
    sten_fbo_rbo_ = res_fbo_rbo_ = res_fbo_tex_ = 0;
    shaders_.Destroy();
    state_.InvalidateAll();
    initialized_ = false;
}

Caps GlDevice::QueryCaps() const {
    Caps c;
    c.max_texture_size = max_texture_size_;
    c.staging_readback = false;
    c.reverse_subtract = true;
    c.depth_sample = true;
    c.texture_barrier = false;
    c.compute = false;
    return c;
}

const GlSurface* GlDevice::Surface(SurfaceId id) const {
    if (!id.id) return nullptr;   // no surface has id 0 (CreateSurface counts from 1)
    const uint32_t slot = id.id & (kLookupSlots - 1);
    if (surface_ids_[slot] == id.id) return surface_ptrs_[slot];
    auto it = surfaces_.find(id.id);
    if (it == surfaces_.end()) return nullptr;
    surface_ids_[slot] = id.id;
    surface_ptrs_[slot] = &it->second;
    return &it->second;
}

const GlDevice::GlMesh* GlDevice::FindMesh(uint32_t mesh) {
    if (!mesh) return nullptr;
    MeshSlot& slot = mesh_slots_[mesh & (kMeshSlots - 1)];
    Census().mesh_lookups++;
    if (slot.id == mesh) return slot.ptr;
    Census().mesh_lookup_misses++;
    auto it = meshes_.find(mesh);
    if (it == meshes_.end()) return nullptr;
    slot.id = mesh;
    slot.ptr = &it->second;
    return &it->second;
}

SurfaceDesc GlDevice::DescribeSurface(SurfaceId id) const {
    const GlSurface* s = Surface(id);
    return s ? s->desc : SurfaceDesc{};
}

// Colour attachment RGBA8 + framebuffer; NEAREST, cleared to transparent black and
// depth 0.0 (the GS far plane).
bool GlDevice::BuildRenderable(GlSurface& s) {
    glGenFramebuffers(1, &s.fbo);
    state_.BindFb(GL_FRAMEBUFFER, s.fbo);
    glGenTextures(1, &s.tex);
    state_.ActiveTexture(GL_TEXTURE0);
    state_.BindTex2D(s.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(s.desc.width),
                 static_cast<GLsizei>(s.desc.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    state_.TexParami(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    state_.TexParami(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[gl] framebuffer incomplete: 0x%x\n", status);
        state_.BindFb(GL_FRAMEBUFFER, 0);
        return false;
    }
    state_.Disable(GL_SCISSOR_TEST);
    state_.ColorMask(true, true, true, true);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    state_.BindTex2D(0);
    state_.BindFb(GL_FRAMEBUFFER, 0);
    return true;
}

SurfaceId GlDevice::CreateSurface(const SurfaceDesc& desc) {
    if (desc.width == 0 || desc.height == 0) return SurfaceId{};
    GlSurface s;
    s.desc = desc;
    if (s.desc.scale == 0) s.desc.scale = settings_.scale;
    state_.ActiveTexture(GL_TEXTURE0);
    if (desc.format == SurfaceFormat::DepthStencil24_8) {
        glGenRenderbuffers(1, &s.rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, s.rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, static_cast<GLsizei>(desc.width),
                              static_cast<GLsizei>(desc.height));
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    } else if (desc.format == SurfaceFormat::Depth32f) {
        glGenTextures(1, &s.tex);
        state_.BindTex2D(s.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, static_cast<GLsizei>(desc.width),
                     static_cast<GLsizei>(desc.height), 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        state_.TexParami(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        state_.TexParami(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        state_.TexParami(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        state_.TexParami(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        state_.BindTex2D(0);
    } else if (desc.renderable) {
        if (!BuildRenderable(s)) { ReleaseSurface(s); return SurfaceId{}; }
    } else {
        // The pool holds level-0 RGBA8 storage only (OLD PoolAcquireTexture is
        // RGBA8-only); an R8 surface is created and destroyed directly so pooled
        // storage never changes internal format. A CPU-writable surface (SurfaceMemory's writer) gets storage of its own:
        // pooled storage may still be read by the GPU for its previous owner's draws.
        const bool poolable = desc.format == SurfaceFormat::Rgba8 && !desc.cpu_writable;
        bool idle = false;
        if (poolable)
            s.tex = pool_.Acquire(static_cast<int>(desc.width), static_cast<int>(desc.height),
                                  &idle);
        s.pooled = s.tex != 0;
        s.respecify = s.pooled && !idle;
        const GLenum fmt = desc.format == SurfaceFormat::R8 ? GL_R8 : GL_RGBA8;
        const GLenum ext = desc.format == SurfaceFormat::R8 ? GL_RED : GL_RGBA;
        if (!s.tex) {
            glGenTextures(1, &s.tex);
            state_.BindTex2D(s.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, fmt, static_cast<GLsizei>(desc.width),
                         static_cast<GLsizei>(desc.height), 0, ext, GL_UNSIGNED_BYTE, nullptr);
        } else {
            state_.BindTex2D(s.tex);
        }
        state_.TexParami(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        state_.TexParami(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        state_.TexParami(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        state_.TexParami(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        state_.BindTex2D(0);
    }
    const uint32_t id = next_surface_++;
    surfaces_[id] = s;
    return SurfaceId{id};
}

void GlDevice::ReleaseSurface(GlSurface& s) {
#if PS2R_HOST_VITA
    if (s.fbo && vita_keep_fbo_ == s.fbo) vita_keep_fbo_ = 0;
#endif
    if (s.fbo) { state_.DeleteFramebuffers(1, &s.fbo); s.fbo = 0; }
    if (s.rbo) {
        if (sten_fbo_rbo_ == s.rbo) sten_fbo_rbo_ = 0;
        if (res_fbo_rbo_ == s.rbo) res_fbo_rbo_ = 0;
        glDeleteRenderbuffers(1, &s.rbo);
        s.rbo = 0;
    }
    if (s.tex && res_fbo_tex_ == s.tex) res_fbo_tex_ = 0;
#if PS2R_HOST_VITA
    if (s.tex && copy_fbo_tex_ == s.tex) copy_fbo_tex_ = 0;   // re-attached at the next copy (the name may be reused)
#endif
    if (s.tex) {
        // Only RGBA8 storage is ever retired to the pool (see CreateSurface); never a CPU-writable surface's.
        const bool rgba8 = s.desc.format == SurfaceFormat::Rgba8 && !s.desc.cpu_writable;
        if (rgba8 && (s.pooled || (!s.desc.renderable && s.levels == 0))) {
            pool_.Retire(s.tex, static_cast<int>(s.desc.width), static_cast<int>(s.desc.height),
                         state_);
        } else {
            state_.DeleteTextures(1, &s.tex);
        }
        s.tex = 0;
    }
}

void GlDevice::DestroySurface(SurfaceId id) {
    auto it = surfaces_.find(id.id);
    if (it == surfaces_.end()) return;
    ReleaseSurface(it->second);
    if (surface_ids_[id.id & (kLookupSlots - 1)] == id.id) surface_ids_[id.id & (kLookupSlots - 1)] = 0;
    surfaces_.erase(it);
}

void GlDevice::Clear(SurfaceId color, SurfaceId depth, uint32_t rgba, float z, uint32_t flags) {
    const GlSurface* c = Surface(color);
    const GlSurface* d = Surface(depth);
    if (!c || !c->fbo) return;
    BindTarget(color, depth);
    state_.Disable(GL_SCISSOR_TEST);
    GLbitfield bits = 0;
    if (flags & kClearColor) {
        state_.ColorMask(true, true, true, true);
        glClearColor(((rgba >> 0) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
                     ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if ((flags & kClearDepth) && d) {
        state_.DepthMask(true);
        glClearDepth(static_cast<double>(z));
        bits |= GL_DEPTH_BUFFER_BIT;
    }
#if PS2R_HOST_VITA
    if (bits) FillCensusForeign();   // (vitaGL's clear is a full-screen quad of its own: the fill census counts it apart)
#endif
    if (bits) glClear(bits);
}

void GlDevice::Upload(SurfaceId id, const Rect& rect, const void* bytes, size_t stride) {
    auto it = surfaces_.find(id.id);
    if (it == surfaces_.end() || !bytes || rect.w <= 0 || rect.h <= 0) return;
    GlSurface& s = it->second;
    const bool depth = s.desc.format == SurfaceFormat::Depth32f;
    if (depth && (rect.x < 0 || rect.y < 0 ||
                  uint64_t(rect.x) + uint32_t(rect.w) > s.desc.width ||
                  uint64_t(rect.y) + uint32_t(rect.h) > s.desc.height ||
                  (stride && (stride % 4 || stride / 4 < uint32_t(rect.w) ||
                              stride / 4 > size_t(std::numeric_limits<GLint>::max())))))
        return;
    const GLenum ext = depth ? GL_DEPTH_COMPONENT :
        (s.desc.format == SurfaceFormat::R8 ? GL_RED : GL_RGBA);
    const GLenum type = depth ? GL_FLOAT : GL_UNSIGNED_BYTE;
    const size_t bpp = s.desc.format == SurfaceFormat::R8 ? 1 : 4;
    state_.ActiveTexture(GL_TEXTURE0);
    state_.BindTex2D(s.tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride ? static_cast<GLint>(stride / bpp) : 0);
    const bool full = rect.x == 0 && rect.y == 0 && rect.w == static_cast<int32_t>(s.desc.width) &&
                      rect.h == static_cast<int32_t>(s.desc.height);
    Census().tex_uploads++;
    Census().tex_upload_bytes += uint64_t(rect.w) * uint64_t(rect.h) * bpp;
#if PS2R_HOST_VITA
    // vitaGL's glTexSubImage2D on a texture drawn in the last FRAME_PURGE_FREQ (4) frames copies the WHOLE old image
    // into a new allocation first (textures.c _glTexSubImage2D: gpu_alloc_mapped_for_gpu + vgl_fast_memcpy from the old
    // data -- a CPU read of uncached GPU memory, 1.2 MB per movie picture) and then writes the update over it. A whole
    // image re-specified with glTexImage2D frees the old storage (deferred while the GPU may read it: gpu_free_texture_data
    // -> mark_as_dirty) and writes the new texels once, no read; its sampler state is set again from the texture object
    // (vitaGL re-applies min / mag / wrap / lod after every glTexImage2D). Only for a level-0-only RGBA8 image: a texture
    // with levels keeps glTexSubImage2D (a level-0 glTexImage2D would drop vitaGL's chain).
    if (full && !depth && !s.desc.renderable && s.desc.format == SurfaceFormat::Rgba8 && s.levels == 0 &&
        s.synth_levels == 0 && (stride == 0 || stride == size_t(rect.w) * 4)) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rect.w, rect.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
        s.respecify = false;
        Census().tex_respecified++;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        state_.BindTex2D(0);
        return;
    }
#endif
    if (s.respecify && full && s.desc.format == SurfaceFormat::Rgba8) {
        // busy pooled storage (always RGBA8): re-specifying with data orphans the old
        // allocation
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rect.w, rect.h, 0, ext, GL_UNSIGNED_BYTE, bytes);
        s.respecify = false;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.w, rect.h, ext, type,
                        bytes);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    state_.BindTex2D(0);
}

// A sampled RGBA8 texture made with its first level. The Vita (TE_GFX_POOL, on by default): a texture not taken from the
// pool is specified ONCE, with its texels -- CreateSurface's glTexImage2D(NULL) allocated the storage and zero-filled it
// (vitaGL gpu_alloc_texture), and Upload's glTexImage2D(texels) then freed that storage (to the garbage collector) and
// allocated it again: two allocations, a memset and a free a texture, during a load hundreds of times. The texture's
// storage holds the same texels and its sampler the same state (CreateSurface's filters and wraps set after; vitaGL keeps
// them on the texture object and re-applies them at every glTexImage2D). Elsewhere (and TE_GFX_POOL=0) the two steps.
SurfaceId GlDevice::CreateTextureWithData(const SurfaceDesc& desc, const void* bytes) {
#if PS2R_HOST_VITA
    static const bool one_step = [] {
        const char* v = std::getenv("TE_GFX_POOL");
        return !(v && v[0] == '0');
    }();
    const bool plain = desc.format == SurfaceFormat::Rgba8 && !desc.renderable && !desc.cpu_writable && desc.width && desc.height;
    if (one_step && bytes && plain) {
        GlSurface s;
        s.desc = desc;
        if (s.desc.scale == 0) s.desc.scale = settings_.scale;
        state_.ActiveTexture(GL_TEXTURE0);
        bool idle = false;
        s.tex = pool_.Acquire(static_cast<int>(desc.width), static_cast<int>(desc.height), &idle);
        s.pooled = s.tex != 0;
        s.respecify = s.pooled && !idle;
        if (!s.tex) glGenTextures(1, &s.tex);
        state_.BindTex2D(s.tex);
        // (as Upload's whole-image path: the storage specified with the texels, one allocation)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, bytes);
        s.respecify = false;
        state_.TexParami(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        state_.TexParami(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        state_.TexParami(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        state_.TexParami(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        state_.BindTex2D(0);
        Census().tex_uploads++;
        Census().tex_upload_bytes += uint64_t(desc.width) * uint64_t(desc.height) * 4;
        Census().tex_respecified++;
        const uint32_t id = next_surface_++;
        surfaces_[id] = s;
        return SurfaceId{id};
    }
#endif
    return Backend::CreateTextureWithData(desc, bytes);
}

void GlDevice::UploadLevel(SurfaceId id, int level, int w, int h, const void* rgba) {
    auto it = surfaces_.find(id.id);
    if (it == surfaces_.end() || level <= 0 || w <= 0 || h <= 0) return;
    state_.ActiveTexture(GL_TEXTURE0);
    state_.BindTex2D(it->second.tex);
    glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    state_.BindTex2D(0);
}

// Sets GL_TEXTURE_MAX_LEVEL to the authored count, optionally extending the chain
// by box-averaging the last authored level down to 1x1 so the shader's LOD floor
// always finds a level (the retail LOD term is clamped at the authored count).
void GlDevice::FinishMipChain(SurfaceId id, int authored_levels, bool synthesize_tail) {
    auto it = surfaces_.find(id.id);
    if (it == surfaces_.end() || authored_levels <= 0) return;
    GlSurface& s = it->second;
#if PS2R_HOST_VITA
    (void)synthesize_tail;
    s.levels = static_cast<uint8_t>(authored_levels);
    s.synth_levels = 0;
    state_.BindTex2D(0);
    return;
#else
    state_.ActiveTexture(GL_TEXTURE0);
    state_.BindTex2D(s.tex);
    int syn = 0;
    if (synthesize_tail) {
        GLint cw = 0, ch = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, authored_levels, GL_TEXTURE_WIDTH, &cw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, authored_levels, GL_TEXTURE_HEIGHT, &ch);
        if (cw > 0 && ch > 0) {
            std::vector<uint8_t> cur(static_cast<size_t>(cw) * ch * 4);
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            state_.BindFb(GL_READ_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex,
                                   authored_levels);
            glReadPixels(0, 0, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, cur.data());
            state_.BindFb(GL_READ_FRAMEBUFFER, 0);
            state_.DeleteFramebuffers(1, &fbo);
            std::vector<uint8_t> nxt;
            while (cw > 1 || ch > 1) {
                int nw = 0, nh = 0;
                GlTexturePool::BoxHalve(cur, cw, ch, nxt, &nw, &nh);
                glTexImage2D(GL_TEXTURE_2D, authored_levels + syn + 1, GL_RGBA8, nw, nh, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, nxt.data());
                syn++;
                cur.swap(nxt);
                cw = nw;
                ch = nh;
            }
        }
    }
    state_.TexParami(GL_TEXTURE_MAX_LEVEL, authored_levels + syn);
    state_.BindTex2D(0);
    s.levels = static_cast<uint8_t>(authored_levels);
    s.synth_levels = static_cast<uint8_t>(syn);
#endif
}

uint32_t GlDevice::BindTarget(SurfaceId color, SurfaceId depth) {
    GlSurface* found = const_cast<GlSurface*>(Surface(color));   // (an element of the non-const map)
    if (!found || !found->fbo) return 0;
    GlSurface& c = *found;
    const GlSurface* d = Surface(depth);
    state_.BindFb(GL_FRAMEBUFFER, c.fbo);
    if (d && d->rbo) {
        // A DepthStencil24_8 surface as the target's depth (ps2e_draw: the stencil is the shadow volumes' counter)
        if (c.attached_depth != d->rbo || !c.attached_depth_rbo) {
            if (c.attached_depth && !c.attached_depth_rbo)
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, d->rbo);
            c.attached_depth = d->rbo;
            c.attached_depth_rbo = true;
        }
#if PS2R_HOST_VITA
        // the framebuffer whose own depth/stencil this renderbuffer stands for on vitaGL (ClearStencil)
        vita_ds_fbo_ = c.fbo;
        vita_ds_rbo_ = d->rbo;
#endif
        return c.fbo;
    }
    const uint32_t dtex = d ? d->tex : 0;
    if (c.attached_depth != dtex || c.attached_depth_rbo) {
        if (c.attached_depth_rbo) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dtex, 0);
        c.attached_depth = dtex;
        c.attached_depth_rbo = false;
    }
    return c.fbo;
}

uint32_t GlDevice::BindStencilTarget(SurfaceId color, SurfaceId stencil) {
    const GlSurface* st = Surface(stencil);
    if (!st || !st->rbo) return 0;
    const GlSurface* c = Surface(color);
    if (color.id != 0 && (!c || !c->tex)) return 0;
    if (color.id == 0) {
        if (!sten_fbo_) glGenFramebuffers(1, &sten_fbo_);
        state_.BindFb(GL_FRAMEBUFFER, sten_fbo_);
        if (sten_fbo_rbo_ != st->rbo) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, st->rbo);
#if !PS2R_HOST_VITA
            glDrawBuffer(GL_NONE);
#endif
            glReadBuffer(GL_NONE);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                static bool said = false;
                if (!said) {
                    said = true;
                    std::fprintf(stderr, "[gl] the stencil-only framebuffer is incomplete (status 0x%x): nothing is drawn into it\n",
                                 unsigned(status));
                }
                return 0;
            }
            sten_fbo_rbo_ = st->rbo;
        }
        return sten_fbo_;
    }
    if (!res_fbo_) glGenFramebuffers(1, &res_fbo_);
    state_.BindFb(GL_FRAMEBUFFER, res_fbo_);
    if (res_fbo_tex_ != c->tex || res_fbo_rbo_ != st->rbo) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, st->rbo);
        const GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
#if !PS2R_HOST_VITA
        glDrawBuffers(1, bufs);
#else
        (void)bufs;
#endif
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 0;
        res_fbo_tex_ = c->tex;
        res_fbo_rbo_ = st->rbo;
    }
    return res_fbo_;
}

void GlDevice::ApplyStencil(const StencilState& st) {
    if (!st.enabled) { glDisable(GL_STENCIL_TEST); return; }
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(st.func == StencilState::kEqual ? GL_EQUAL : GL_ALWAYS, st.ref, st.cmp_mask);
    glStencilMask(st.write_mask);
    const auto op = [](StencilState::Op o) -> GLenum {
        switch (o) {
            case StencilState::kInvert: return GL_INVERT;
            case StencilState::kIncrWrap: return GL_INCR_WRAP;
            case StencilState::kDecrWrap: return GL_DECR_WRAP;
            case StencilState::kReplace: return GL_REPLACE;
            default: return GL_KEEP;
        }
    };
    glStencilOp(GL_KEEP, op(st.depth_fail_op), op(st.pass_op));
}

void GlDevice::ResetStencil() {
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
}

void GlDevice::ClearStencil(SurfaceId stencil, uint8_t value) {
#if PS2R_HOST_VITA
    // vitaGL (r1488) keeps no storage in a renderbuffer: glRenderbufferStorage only notes that a stencil is wanted, and
    // every framebuffer it is attached to gets a depth/stencil surface of its OWN, made at that framebuffer's first scene
    // (gxm.c scene_reset -> init_depth_stencil_buffer) and -- the library is built without STORE_DEPTH_STENCIL -- never
    // force-loaded or stored: it lives for one GXM scene, which starts at GXM's background values (depth 1.0, stencil 0).
    // A framebuffer without a colour attachment is incomplete there (GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT), so the
    // stencil-only framebuffer below is refused, and a complete one would still hold another stencil than the frame's.
    // The counter is cleared in the frame target's own framebuffer instead, inside its current scene: a glClear of the
    // stencil alone draws nothing into colour or depth (vitaGL disables the fragment program and the depth writes).
    // TE_GFX_VITA_STENCIL=0 keeps the stencil-only path (the counter never cleared: no shadow is ever composited).
    if (vita_scene_stencil_) {
        const GlSurface* st = Surface(stencil);
        if (!st || !st->rbo || st->rbo != vita_ds_rbo_ || !vita_ds_fbo_) return;
        state_.BindFb(GL_FRAMEBUFFER, vita_ds_fbo_);
        state_.Disable(GL_SCISSOR_TEST);
        glStencilMask(0xFF);
        glClearStencil(value);
        FillCensusForeign();   // (vitaGL's clear is a full-screen quad of its own: the fill census counts it apart)
        glClear(GL_STENCIL_BUFFER_BIT);
        return;
    }
#endif
    if (!BindStencilTarget(SurfaceId{}, stencil)) return;
    state_.Disable(GL_SCISSOR_TEST);
    glStencilMask(0xFF);
    glClearStencil(value);
    glClear(GL_STENCIL_BUFFER_BIT);
}

// Constant colour over `scissor` under the stencil test: viewport = the stencil
// surface, blend off, depth ALWAYS without writes, colour writes only with a colour
// attachment (alpha per `write_alpha`); the const program over the NDC triangle.
void GlDevice::DrawConstQuad(SurfaceId color, SurfaceId stencil, const Rect& scissor, uint32_t rgba,
                             bool write_alpha, const StencilState& st) {
    const GlSurface* ss = Surface(stencil);
    if (!ss || scissor.w <= 0 || scissor.h <= 0) return;
    if (!BindStencilTarget(color, stencil)) return;
    EndDrawRun();
    state_.Viewport(0, 0, static_cast<int>(ss->desc.width), static_cast<int>(ss->desc.height));
    state_.Enable(GL_SCISSOR_TEST);
    state_.Scissor(scissor.x, scissor.y, scissor.w, scissor.h);
    state_.Disable(GL_BLEND);
    state_.Enable(GL_DEPTH_TEST);
    state_.DepthFunc(GL_ALWAYS);
    state_.DepthMask(false);
    const bool cw = color.id != 0;
    state_.ColorMask(cw, cw, cw, cw && write_alpha);
    state_.UseProgram(shaders_.ConstProgram());
    glUniform4f(shaders_.StencilLocations().const_color, ((rgba >> 0) & 0xFF) / 255.0f,
                ((rgba >> 8) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f,
                ((rgba >> 24) & 0xFF) / 255.0f);
    state_.BindVertexArray(const_vao_);
    ApplyStencil(st);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ResetStencil();
    state_.BindVertexArray(0);
    state_.UseProgram(0);
    state_.ColorMask(true, true, true, true);
    state_.DepthMask(true);
    state_.Disable(GL_SCISSOR_TEST);
}

bool GlDevice::MarkStencilWhereOpaque(SurfaceId color, SurfaceId stencil, uint8_t bits) {
#if PS2R_HOST_VITA
    // In the frame's scene its colour is still in the GPU's tiles: sampling the frame target's texture here reads the
    // memory the last scene stored (the previous picture, with everything drawn after its shadows), and the stencil-only
    // framebuffer is refused anyway (ClearStencil). The producer marks its casters by drawing them again (ps2e_draw).
    if (vita_scene_stencil_) return false;
#endif
    const GlSurface* frame = Surface(color);
    const GlSurface* ss = Surface(stencil);
    if (!frame || !frame->tex || !ss) return false;
    if (!BindStencilTarget(SurfaceId{}, stencil)) return false;
    EndDrawRun();
    state_.Viewport(0, 0, static_cast<int>(ss->desc.width), static_cast<int>(ss->desc.height));
    state_.Disable(GL_SCISSOR_TEST);
    state_.Disable(GL_BLEND);
    state_.Enable(GL_DEPTH_TEST);
    state_.DepthFunc(GL_ALWAYS);
    state_.DepthMask(false);
    state_.ColorMask(false, false, false, false);
    state_.UseProgram(shaders_.OpaqueMaskProgram());
    state_.ActiveTexture(GL_TEXTURE0);
    state_.BindTex2D(frame->tex);
    state_.BindVertexArray(const_vao_);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, static_cast<GLint>(bits), 0xFF);
    glStencilMask(bits);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
#if PS2R_HOST_VITA
    state_.ApplyVitaSamplerSizes();
#endif
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ResetStencil();
    state_.BindVertexArray(0);
    state_.UseProgram(0);
    state_.ColorMask(true, true, true, true);
    state_.DepthMask(true);
    return true;
}

bool GlDevice::ReadbackStencil(SurfaceId stencil, const Rect& rect, uint8_t* out) {
    if (!out || rect.w <= 0 || rect.h <= 0) return false;
    if (!BindStencilTarget(SurfaceId{}, stencil)) return false;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(rect.x, rect.y, rect.w, rect.h,
#if PS2R_HOST_VITA
                 GL_STENCIL_INDEX8,
#else
                 GL_STENCIL_INDEX,
#endif
                 GL_UNSIGNED_BYTE, out);
    return glGetError() == GL_NO_ERROR;
}

DrawResult GlDevice::Draw(const gs::DrawIr& ir, const gs::TextureSource& tex,
                          const DrawParams& params) {
    if (!executor_) return DrawResult{};
    return executor_->Draw(ir, tex, params);
}

// A native mesh: its vertices in a buffer of their own with the attribute layout bound once (GpuVertex's four, then the
// normal and the lit colour); 0 when the PS2 program has no native transform (the caller builds on the CPU)
namespace {
// the mesh vertex layout over the bound array buffer, into the bound vertex array
void MeshVertexAttributes() {
    const GLsizei stride = sizeof(MeshVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(MeshVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(MeshVertex, s)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(MeshVertex, r)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(MeshVertex, fog)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(MeshVertex, nx)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(MeshVertex, lr)));
}

// (MeshIndices, the deduplication of the Vita's indexed meshes: engine/render/mesh_index.h -- the producer may run it
// ahead on a worker, ps2e_draw_mesh_prepare)
}  // namespace

#if PS2R_HOST_VITA
// TE_GFX_VITA_MESH_INDEX, read once (the first mesh made, or the first prepared one handed over)
void GlDevice::EnsureMeshIndexMode() {
    if (vita_mesh_index_ >= 0) return;
    {
        const char* e = std::getenv("TE_GFX_VITA_MESH_INDEX");
        vita_mesh_index_ = 1;
        if (e && std::strncmp(e, "ab", 2) == 0) {
            vita_mesh_index_ = 3;
            vita_mesh_ab_ = e[2] ? std::max(1, std::atoi(e + 2)) : 600;
        } else if (e && (e[0] == '0' || e[0] == '1' || e[0] == '2')) {
            vita_mesh_index_ = e[0] - '0';
        }
        static const char* const kNames[4] = {"the triangle lists as they are (TE_GFX_VITA_MESH_INDEX=0; unset / 1: indexed)",
                                              "indexed over their distinct vertices (the default; TE_GFX_VITA_MESH_INDEX=0: the lists)",
                                              "indexed 0, 1, 2, ... over the whole list (TE_GFX_VITA_MESH_INDEX=2: the index path alone)",
                                              "both forms made, the indexed one drawn in every other window (TE_GFX_VITA_MESH_INDEX=ab)"};
        std::fprintf(stderr, "[gl] native meshes: %s\n", kNames[vita_mesh_index_]);
        if (vita_mesh_index_ == 3)
            std::fprintf(stderr, "[gl] native meshes A/B: indexed for presents 1-%d, the lists for %d-%d, ...\n", vita_mesh_ab_,
                         vita_mesh_ab_ + 1, 2 * vita_mesh_ab_);
    }
}
#endif

uint32_t GlDevice::CreateMesh(const MeshVertex* verts, size_t count) { return MakeMesh(verts, count, nullptr, 0, nullptr); }

// The Vita's indexed form computed ahead (ps2e_draw_mesh_prepare: the same MeshIndices over the same list): the mesh made
// from it as CreateMesh makes it from the list -- the same GL calls in the same order with the same bytes -- without the
// deduplication here. 0 when the device does not draw meshes indexed (TE_GFX_VITA_MESH_INDEX 0 / 2 / ab: the caller makes
// it from the list) or has no native meshes.
uint32_t GlDevice::CreateMeshIndexed(const MeshVertex* distinct, size_t distinct_count, const uint16_t* indices, size_t count) {
#if PS2R_HOST_VITA
    if (!distinct || !indices || !distinct_count || distinct_count >= count) return 0;
    EnsureMeshIndexMode();
    if (vita_mesh_index_ != 1) return 0;
    return MakeMesh(nullptr, count, distinct, distinct_count, indices);
#else
    (void)distinct; (void)distinct_count; (void)indices; (void)count;
    return 0;
#endif
}

uint32_t GlDevice::MakeMesh(const MeshVertex* verts, size_t count, const MeshVertex* pre_distinct, size_t pre_count,
                            const uint16_t* pre_indices) {
    if (!shaders_.NativeMesh() || (!verts && !pre_distinct) || count < 3) return 0;
#if !PS2R_HOST_VITA
    (void)pre_distinct; (void)pre_count; (void)pre_indices;
#endif
#if PS2R_HOST_VITA
    const auto t_create = std::chrono::steady_clock::now();   // (the census: the drawing thread's time making meshes)
#endif
    GlMesh m;
    m.count = count;
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    if (!m.vao || !m.vbo) return 0;
    state_.BindVertexArray(m.vao);
    state_.BindArrayBuffer(m.vbo);
#if PS2R_HOST_VITA
    // INDEXED MESHES (TE_GFX_VITA_MESH_INDEX: unset / 1 on -- the default --, 0 off, abN both forms made
    // and the indexed one drawn in every other window of N presents (ab1: every other present, the GPU's work measured per
    // form: vita_gpu_census's per-present A/B), 2 = indices 0, 1, 2, ... over the whole list -- the index path alone, a
    // diagnosis): the triangle list's vertices deduplicated by value (all 48 bytes) and the list drawn through 16-bit
    // indices in its own order -- the same triangles from the same vertex values, but a strip's shared vertices shaded
    // once instead of up to three times (the GPU runs the vertex program per distinct index of a primitive block, and the
    // parameter buffer holds a block's distinct vertices once: the hub's ~91k shaded mesh vertices a frame become ~30k).
    // A sub-range (first, count: list vertices) is the same range of indices. Exact by construction on the device's GPU
    // (each vertex's program output is a function of its 48 bytes; the triangles, their order and their vertices' values
    // are the list's). In Vita3K (x86 4194: the Mac's GPU through MoltenVK) pictures differ at isolated triangle-edge
    // pixels where two triangles share an index (the identity diagnosis 2 -- the very same index path without the sharing
    // -- draws pixel-identically to the lists): the host GPU's handling of shared vertices (measured:
    // the fill census and the GPU timings), not the triangles drawn.
    EnsureMeshIndexMode();
    thread_local std::vector<MeshVertex> distinct;
    thread_local std::vector<uint16_t> indices;
    // the distinct vertices and the indices: given (prepared ahead: the device's mode is indexed), or made here
    const MeshVertex* dv = pre_distinct;
    size_t dn = pre_count;
    const uint16_t* iv = pre_indices;
    bool indexed = pre_distinct != nullptr;
    if (!pre_distinct) {
        const auto t_index = std::chrono::steady_clock::now();
        indexed = vita_mesh_index_ && MeshIndices(verts, count, vita_mesh_index_ == 2, distinct, indices);
        if (vita_mesh_index_)
            Census().mesh_index_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                                                    t_index).count());
        dv = distinct.data();
        dn = distinct.size();
        iv = indices.data();
    }
    const bool both = indexed && vita_mesh_index_ == 3;   // the list form here, the indexed one in vao_idx / vbo_idx
    size_t stored = count;   // the vertices m.vbo holds
    if (indexed && !both) {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(dn * sizeof(MeshVertex)), dv, GL_STATIC_DRAW);
        glGenBuffers(1, &m.list_ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.list_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(uint16_t)), iv, GL_STATIC_DRAW);
        stored = dn;
    } else {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(MeshVertex)), verts, GL_STATIC_DRAW);
    }
    MeshVertexAttributes();
    if (both) {
        glGenVertexArrays(1, &m.vao_idx);
        glGenBuffers(1, &m.vbo_idx);
        glGenBuffers(1, &m.list_ibo);
        state_.BindVertexArray(m.vao_idx);
        state_.BindArrayBuffer(m.vbo_idx);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(dn * sizeof(MeshVertex)), dv, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.list_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(uint16_t)), iv, GL_STATIC_DRAW);
        MeshVertexAttributes();
    }
    if (indexed) m.shaded = float(double(dn) / double(count));
    Census().mesh_list_vertices += count;
    Census().mesh_stored_vertices += both ? count + dn : indexed ? dn : count;   // (the A/B keeps both)
    {
        // the raw mesh draws read the buffer's GPU address from vitaGL's record of it (the GL name: buffers.c glGenBuffers;
        // shared.h struct vbo: ptr, size, alloc_func, last_frame): the record checked once, at the first mesh
        static int checked = 0;
        if (!checked) {
            const int32_t* rec = reinterpret_cast<const int32_t*>(uintptr_t(m.vbo));
            bool ok = rec[0] != 0 && rec[1] == int32_t(stored * sizeof(MeshVertex));
            if (m.list_ibo) {
                const int32_t* irec = reinterpret_cast<const int32_t*>(uintptr_t(m.list_ibo));
                ok = ok && irec[0] != 0 && irec[1] == int32_t(count * sizeof(uint16_t));
            }
            checked = ok ? 1 : -1;
            if (executor_) executor_->SetRawMesh(ok);
            std::fprintf(stderr, "[gl] raw mesh draws: vitaGL's buffer record %s\n", ok ? "as expected" : "NOT as expected: the raw mesh draws are off");
        }
    }
#else
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(MeshVertex)), verts, GL_STATIC_DRAW);
    MeshVertexAttributes();
#endif
    state_.BindVertexArray(0);
    const uint32_t id = next_mesh_++;
    meshes_[id] = m;
    Census().meshes_created++;
    Census().mesh_bytes += uint64_t(count) * sizeof(MeshVertex);
    Census().meshes_alive = meshes_.size();
#if PS2R_HOST_VITA
    Census().mesh_create_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                                             t_create).count());
#endif
    return id;
}

void GlDevice::DestroyMesh(uint32_t mesh) {
    auto it = meshes_.find(mesh);
    if (it == meshes_.end()) return;
    if (mesh_slots_[mesh & (kMeshSlots - 1)].id == mesh) mesh_slots_[mesh & (kMeshSlots - 1)] = MeshSlot{};
    state_.DeleteBuffers(1, &it->second.vbo);
    if (it->second.ibo) {
        // (the element binding is the vertex array's state, deleted with it below; the state cache never binds it)
        glDeleteBuffers(1, &it->second.ibo);
    }
    if (it->second.list_ibo) glDeleteBuffers(1, &it->second.list_ibo);   // (an indexed native mesh's: likewise)
    if (it->second.vbo_idx) state_.DeleteBuffers(1, &it->second.vbo_idx);   // (the A/B's indexed form)
    if (it->second.vao_idx) state_.DeleteVertexArrays(1, &it->second.vao_idx);
    state_.DeleteVertexArrays(1, &it->second.vao);
    meshes_.erase(it);
    Census().meshes_alive = meshes_.size();
}

// A GPU-skinned mesh (the Vita): the skin part's static vertices in a buffer of their own with the skin vertex program's
// attribute layout bound once (position, STQ, the colour lanes, the normal, the weights, the joints), and its triangles as
// 16-bit indices in an element buffer the vertex array holds; 0 when the library has no skinning programs
uint32_t GlDevice::CreateSkinMesh(const SkinVertex* verts, size_t count, const uint16_t* indices, size_t index_count) {
#if PS2R_HOST_VITA
    if (!shaders_.SkinBuilt() || !verts || count < 3 || count > 65536 || !indices || index_count < 3) return 0;
    GlMesh m;
    m.count = index_count / 3 * 3;
    m.shaded = m.count ? float(double(count) / double(m.count)) : 1.f;   // (the fill census: distinct vertices per index)
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glGenBuffers(1, &m.ibo);
    if (!m.vao || !m.vbo || !m.ibo) return 0;
    state_.BindVertexArray(m.vao);
    state_.BindArrayBuffer(m.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(SkinVertex)), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(m.count * sizeof(uint16_t)), indices, GL_STATIC_DRAW);
    const GLsizei stride = sizeof(SkinVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(SkinVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(SkinVertex, s)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(SkinVertex, r)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(SkinVertex, nx)));
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(SkinVertex, w0)));
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(8, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(SkinVertex, j0)));
    glEnableVertexAttribArray(9);
    glVertexAttribPointer(9, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(SkinVertex, index)));
    state_.BindVertexArray(0);
    {
        // the skin meshes' census: the vertices by the weights they use (a mesh's most: 1, 2 or 3 bones), the
        // meshes with a negative colour lane (the program's int8 wrap matters) and the blend parts (a displacement index) --
        // what a cheaper skin program per mesh kind would cover
        int most = 1;
        bool negative = false, blend = false;
        for (size_t i = 0; i < count; ++i) {
            const SkinVertex& v = verts[i];
            if (v.w2 != 0.f) most = 3;
            else if (v.w1 != 0.f && most < 2) most = 2;
            negative = negative || v.r < 0.f || v.g < 0.f || v.b < 0.f;
            blend = blend || v.index != 0.f;
        }
        GlCensus& c = Census();
        c.skin_mesh_vertices[most - 1] += count;
        if (negative) c.skin_mesh_negative_vertices += count;
        if (blend) c.skin_mesh_blend_vertices += count;
    }
    {
        // the raw skin draws read the buffers' GPU addresses from vitaGL's records of them (as the raw mesh draws: checked
        // once, at the first skin mesh; not as expected -> the raw draws off)
        static int checked = 0;
        if (!checked) {
            const int32_t* rec = reinterpret_cast<const int32_t*>(uintptr_t(m.vbo));
            const int32_t* irec = reinterpret_cast<const int32_t*>(uintptr_t(m.ibo));
            const bool ok = rec[0] != 0 && rec[1] == int32_t(count * sizeof(SkinVertex)) && irec[0] != 0 &&
                            irec[1] == int32_t(m.count * sizeof(uint16_t));
            checked = ok ? 1 : -1;
            if (!ok && executor_) executor_->SetRawMesh(false);
            std::fprintf(stderr, "[gl] raw skin draws: vitaGL's buffer records %s\n", ok ? "as expected" : "NOT as expected: the raw draws are off");
        }
    }
    const uint32_t id = next_mesh_++;
    meshes_[id] = m;
    Census().meshes_created++;
    Census().mesh_bytes += uint64_t(count) * sizeof(SkinVertex) + uint64_t(m.count) * sizeof(uint16_t);
    Census().meshes_alive = meshes_.size();
    return id;
#else
    (void)verts; (void)count; (void)indices; (void)index_count;
    return 0;
#endif
}

DrawResult GlDevice::DrawSkinMesh(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params, uint32_t mesh,
                                  const MeshDrawParams& mp, const float* bones, int joints, const float* disp, int disp_count) {
    const GlMesh* m = FindMesh(mesh);
#if PS2R_HOST_VITA
    shaders_.DropSkinPacket();   // (TakeSkinPacket hands over only the packet THIS draw records)
#endif
    if (!executor_ || !m || !m->ibo) return DrawResult{};
#if PS2R_HOST_VITA
    executor_->SetMeshIndices(0, m->shaded);   // (the fill census's vertex estimate; a skin draws its own indices)
    executor_->SetSkinBuffers(m->vbo, m->ibo);  // (a raw skin draw streams the skin's buffer through its indices)
#endif
    return executor_->DrawSkinMesh(ir, tex, params, m->vao, m->count, mp, bones, joints, disp, disp_count);
}

#if PS2R_HOST_VITA
uint32_t GlDevice::TakeSkinPacket() { return shaders_.TakeSkinPacket(); }

bool GlDevice::DrawSkinPacket(uint32_t packet, uint32_t mesh, const MeshDrawParams& mp, const float* bones, int joints,
                              const float* disp, int disp_count) {
    const GlMesh* m = FindMesh(mesh);
    if (!executor_ || !m || !m->ibo) return false;
    return executor_->DrawSkinPacket(packet, m->vbo, m->ibo, m->shaded, m->count, mp, bones, joints, disp, disp_count);
}
#endif

// The vertices a mesh draw takes: the whole mesh, or the params' sub-range of it (clamped to the mesh; 0 = none)
static size_t MeshRange(size_t vertices, const MeshDrawParams& mp) {
    if (!mp.count) return mp.first ? 0 : vertices;
    if (mp.first >= vertices) return 0;
    return std::min<size_t>(mp.count, vertices - mp.first);
}

DrawResult GlDevice::DrawMesh(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params, uint32_t mesh,
                              const MeshDrawParams& mp) {
    const GlMesh* m = FindMesh(mesh);
#if PS2R_HOST_VITA
    shaders_.DropRawPacket();   // (TakeMeshPacket hands over only the packet THIS draw records)
#endif
    if (!executor_ || !m || m->ibo) return DrawResult{};
#if PS2R_HOST_VITA
    // (an indexed mesh: drawn through its indices; the A/B's mesh: the form of the window's arm)
    const bool use_idx = m->vao_idx ? vita_mesh_ab_on_ : m->list_ibo != 0;
    const uint32_t mesh_vao = m->vao_idx && use_idx ? m->vao_idx : m->vao;
    executor_->SetMeshBuffer(m->vao_idx && use_idx ? m->vbo_idx : m->vbo);   // (a raw mesh draw streams it from mp.first)
    executor_->SetMeshIndices(use_idx ? m->list_ibo : 0, use_idx ? m->shaded : 1.f);
#else
    const uint32_t mesh_vao = m->vao;
#endif
    return executor_->DrawMesh(ir, tex, params, mesh_vao, MeshRange(m->count, mp), mp);
}

DrawResult GlDevice::DrawMeshAgain(uint32_t mesh, const MeshDrawParams& mp) {
    const GlMesh* m = FindMesh(mesh);
#if PS2R_HOST_VITA
    shaders_.DropRawPacket();
#endif
    if (!executor_ || !m || m->ibo) return DrawResult{};
#if PS2R_HOST_VITA
    // (an indexed mesh: drawn through its indices; the A/B's mesh: the form of the window's arm)
    const bool use_idx = m->vao_idx ? vita_mesh_ab_on_ : m->list_ibo != 0;
    const uint32_t mesh_vao = m->vao_idx && use_idx ? m->vao_idx : m->vao;
    executor_->SetMeshBuffer(m->vao_idx && use_idx ? m->vbo_idx : m->vbo);   // (a raw mesh draw streams it from mp.first)
    executor_->SetMeshIndices(use_idx ? m->list_ibo : 0, use_idx ? m->shaded : 1.f);
#else
    const uint32_t mesh_vao = m->vao;
#endif
    return executor_->DrawMeshAgain(mesh_vao, MeshRange(m->count, mp), mp);
}

DrawResult GlDevice::DrawMeshTexture(const gs::DrawIr& ir, const gs::TextureSource& tex, uint32_t mesh, const MeshDrawParams& mp) {
    const GlMesh* m = FindMesh(mesh);
#if PS2R_HOST_VITA
    shaders_.DropRawPacket();
#endif
    if (!executor_ || !m || m->ibo) return DrawResult{};
#if PS2R_HOST_VITA
    // (an indexed mesh: drawn through its indices; the A/B's mesh: the form of the window's arm)
    const bool use_idx = m->vao_idx ? vita_mesh_ab_on_ : m->list_ibo != 0;
    const uint32_t mesh_vao = m->vao_idx && use_idx ? m->vao_idx : m->vao;
    executor_->SetMeshBuffer(m->vao_idx && use_idx ? m->vbo_idx : m->vbo);   // (a raw mesh draw streams it from mp.first)
    executor_->SetMeshIndices(use_idx ? m->list_ibo : 0, use_idx ? m->shaded : 1.f);
#else
    const uint32_t mesh_vao = m->vao;
#endif
    return executor_->DrawMeshTexture(ir, tex, mesh_vao, MeshRange(m->count, mp), mp);
}

#if PS2R_HOST_VITA
uint32_t GlDevice::TakeMeshPacket() { return shaders_.TakeRawPacket(); }

bool GlDevice::DrawMeshPacket(uint32_t packet, uint32_t mesh, const MeshDrawParams& mp) {
    const GlMesh* m = FindMesh(mesh);
    if (!executor_ || !m || m->ibo) return false;
    // the form DrawMesh / DrawMeshAgain / DrawMeshTexture draw it in (TE_GFX_VITA_MESH_INDEX: an indexed mesh through its
    // indices; the A/B's mesh in the window's arm) -- the replay addresses that form's buffers as RawSubmit does
    const bool use_idx = m->vao_idx ? vita_mesh_ab_on_ : m->list_ibo != 0;
    const uint32_t vbo = m->vao_idx && use_idx ? m->vbo_idx : m->vbo;
    return executor_->DrawMeshPacket(packet, vbo, use_idx ? m->list_ibo : 0, use_idx ? m->shaded : 1.f, MeshRange(m->count, mp), mp);
}

bool GlDevice::DrawMeshPacketInstances(uint32_t packet, uint32_t mesh, uint32_t first, uint32_t count, const MeshInstance* inst,
                                       int n) {
    const GlMesh* m = FindMesh(mesh);
    if (!executor_ || !m || m->ibo || n < 2 || !inst) return false;
    // (DrawMeshPacket's form of the mesh and its range)
    const bool use_idx = m->vao_idx ? vita_mesh_ab_on_ : m->list_ibo != 0;
    const uint32_t vbo = m->vao_idx && use_idx ? m->vbo_idx : m->vbo;
    MeshDrawParams range;
    range.first = first;
    range.count = count;
    return executor_->DrawMeshPacketInstances(packet, vbo, use_idx ? m->list_ibo : 0, MeshRange(m->count, range), first, inst, n);
}
#endif

bool GlDevice::DrawsRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params) const {
    return executor_ && executor_->DrawsRaw(ir, tex, params);
}

DrawResult GlDevice::DrawRaw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params,
                             const RawVertex* verts, size_t n, const float* box) {
    if (!executor_) return DrawResult{};
    return executor_->DrawRaw(ir, tex, params, verts, n, box);
}

#if PS2R_HOST_VITA
RawVertex* GlDevice::ReserveRaw(size_t n) {
    // (only the executor's raw vertex draws take vertices where they lie: without them a reservation would be copied again)
    if (!executor_ || !n || !executor_->RawVertexDraws()) return nullptr;
    int first = 0;
    return static_cast<RawVertex*>(vertices_.ReserveRaw(n * sizeof(RawVertex), sizeof(RawVertex), &first));
}
#endif

// glCopyTexSubImage2D from the source attachment into the destination texture
// (transform 0 = plain copy; other values are reserved).
void GlDevice::CopyRect(SurfaceId src, const Rect& src_rect, SurfaceId dst, const Rect& dst_rect,
                        uint32_t transform) {
    (void)transform;
    const GlSurface* s = Surface(src);
    const GlSurface* d = Surface(dst);
    if (!s || !d || !s->fbo || src_rect.w <= 0 || src_rect.h <= 0) return;
#if PS2R_HOST_VITA
    if (vita_copy_none_) return;   // diagnosis (TE_GFX_VITA_COPY=none): no copy, no scene break
    // the source's scene ends at this copy -- stored, when this frame keeps its depth (KeepDepthAcrossCopies) -- and every
    // later scene of it loads what the scene before stored (the modes are read at each sceGxmBeginScene: set before the
    // break, they apply from the scene after it; the glCopyTexSubImage2D arm's glReadPixels begins that scene itself).
    // LOAD is switched on only when the scene this copy ends is the target's own (vitaGL's scene in progress) and began
    // after the store was set: the surface still the one KeepDepthAcrossCopies set (its memory, its store mode) -- so a
    // load always reads what a store of this frame wrote. A copy before the frame's first draw into the target ends no
    // scene of it: the scene its restore begins stores, and the next copy loads.
    if (vita_keep_fbo_ == s->fbo && !vita_keep_loading_ && &in_use_framebuffer &&
        in_use_framebuffer == reinterpret_cast<void*>(uintptr_t(s->fbo))) {
        auto* ds = static_cast<SceGxmDepthStencilSurface*>(VitaDepthSurface(s->fbo, vita_keep_w_, vita_keep_h_));
        if (ds && ds->depthData == vita_keep_data_ &&
            sceGxmDepthStencilSurfaceGetForceStoreMode(ds) == SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED) {
            sceGxmDepthStencilSurfaceSetForceLoadMode(ds, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
            vita_keep_loading_ = true;
        } else {
            VitaDropKeep("its depth/stencil surface was re-made or its store mode reset during the frame");
        }
    }
    if (vita_gpu_copy_ && VitaCopyRect(*s, src_rect, *d, dst_rect)) return;
#endif
    state_.BindFb(GL_READ_FRAMEBUFFER, s->fbo);
    state_.ActiveTexture(GL_TEXTURE0);
    state_.BindTex2D(d->tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, dst_rect.x, dst_rect.y, src_rect.x, src_rect.y, src_rect.w,
                        src_rect.h);
    state_.BindTex2D(0);
}

#if PS2R_HOST_VITA
namespace {
// vitaGL r1488's framebuffer object (source/shared.h `framebuffer`): the GLuint glGenFramebuffers hands out IS its
// address (framebuffers.c: ids[j] = (GLuint)&framebuffers[i]) -- proven at run time before any name is read through
// (VitaCheckFramebufferNames). The installed libvitaGL.a agrees with this layout: its `framebuffers` array is 256 x 0x68
// bytes, and scene_reset makes the surface with init_depth_stencil_buffer(fb + 0x54 width, fb + 0x58 height, fb + 0x38),
// then stores fb + 0x38 into fb + 0x4C (depthbuffer_ptr) and ORs DEPTHBUFFER_READY (2) into the byte at fb + 2. Only the
// depth/stencil surface's force load / store modes are touched here, which vitaGL itself never sets (it is built without
// STORE_DEPTH_STENCIL: every framebuffer's depth/stencil lives for one GXM scene); vitaGL re-making the surface
// (vglDepthStencilSurfaceInit) resets them, which hands it back to its transient use.
struct VglFramebuffer {
    uint8_t active;
    uint8_t is_float;
    uint8_t depthbuffer_state;
    SceGxmRenderTarget* target;
    SceGxmColorSurface colorbuffer;
    SceGxmDepthStencilSurface depthbuffer;
    SceGxmDepthStencilSurface* depthbuffer_ptr;
    SceGxmTextureFormat format;
    int width;
    int height;
    int stride;
    void* data;
    void* tex;
};
static_assert(offsetof(VglFramebuffer, depthbuffer) == 0x38, "vitaGL r1488 framebuffer layout");
static_assert(offsetof(VglFramebuffer, depthbuffer_ptr) == 0x4C, "vitaGL r1488 framebuffer layout");
static_assert(offsetof(VglFramebuffer, width) == 0x54 && offsetof(VglFramebuffer, height) == 0x58,
              "vitaGL r1488 framebuffer layout");
static_assert(sizeof(VglFramebuffer) == 0x68, "vitaGL r1488 framebuffer layout (its framebuffers array: 256 x 0x68)");
constexpr uint8_t kVglDepthbufferReady = 2;   // shared.h: DEPTHBUFFER_MISSING, DEPTHBUFFER_WANTS_STENCIL, DEPTHBUFFER_READY

// The depth/stencil surface vitaGL made for framebuffer `fbo` at its first scene, checked against the layout above
// (active, READY, depthbuffer_ptr pointing at its own depthbuffer, the colour attachment's size, real depth memory);
// null when it has none yet or anything disagrees.
SceGxmDepthStencilSurface* VglDepthSurface(uint32_t fbo, uint32_t width, uint32_t height) {
    auto* fb = reinterpret_cast<VglFramebuffer*>(uintptr_t(fbo));
    if (!fb || fb->active != 1 || !(fb->depthbuffer_state & kVglDepthbufferReady) || fb->depthbuffer_ptr != &fb->depthbuffer ||
        uint32_t(fb->width) != width || uint32_t(fb->height) != height || !fb->depthbuffer.depthData)
        return nullptr;
    return &fb->depthbuffer;
}
}  // namespace

// At each present until it holds (Present calls it while the frame target's scene is still the one in progress): when
// the frame was drawn, vitaGL's in_use_framebuffer -- the framebuffer of the GXM scene in progress -- must hold the
// target's GL name, which proves the name is vitaGL's framebuffer struct before anything is read through it (vita_vgl_
// 2); its depth/stencil surface must then match the layout (vita_vgl_ 1). A frame that drew nothing into the target
// proves nothing, and a just-made target's surface is not ready before its second scene (MakeFrameTarget attaches the
// depth renderbuffer inside the scene its colour clear began: vitaGL re-makes the surface at the next one). A vitaGL
// whose names are not addresses never matches: the keep then stays off, said once at the first frame with copies.
void GlDevice::VitaCheckFramebufferNames(const GlSurface& frame) {
    if (vita_vgl_ == 1 || vita_vgl_ < 0 || !frame.fbo || !vita_keep_depth_) return;
    if (!&in_use_framebuffer) {
        vita_vgl_ = -1;
        std::fprintf(stderr, "[gl] vitaGL's in_use_framebuffer is not linked: the depth is not kept across screen copies\n");
        return;
    }
    if (in_use_framebuffer != reinterpret_cast<void*>(uintptr_t(frame.fbo))) return;
    vita_vgl_ = 2;   // the name is vitaGL's framebuffer struct
    if (!VglDepthSurface(frame.fbo, frame.desc.width, frame.desc.height)) {
        // the first presents after a target is made see its surface not ready (above); said once if it stays so
        static int fails = 0;
        if (++fails == 30) {
            const auto* fb = reinterpret_cast<const VglFramebuffer*>(uintptr_t(frame.fbo));
            std::fprintf(stderr, "[gl] vitaGL framebuffer %#x is its scene's framebuffer, but its depth/stencil surface "
                         "disagrees with the r1488 layout after 30 presents (active %u state %u ptr %s size %dx%d, target "
                         "%ux%u, depth %p): the depth is not kept across screen copies while it does\n", frame.fbo,
                         unsigned(fb->active), unsigned(fb->depthbuffer_state),
                         fb->depthbuffer_ptr == &fb->depthbuffer ? "own" : fb->depthbuffer_ptr ? "other" : "null", fb->width,
                         fb->height, frame.desc.width, frame.desc.height, fb->depthbuffer.depthData);
        }
        return;
    }
    vita_vgl_ = 1;
    std::fprintf(stderr, "[gl] vitaGL framebuffer names verified (%#x = its scene's framebuffer, a %ux%u depth/stencil "
                 "surface at +0x38): the depth can be kept across screen copies\n", frame.fbo, frame.desc.width,
                 frame.desc.height);
}

// The depth/stencil surface vitaGL keeps for framebuffer `fbo` -- only once the names are verified
void* GlDevice::VitaDepthSurface(uint32_t fbo, uint32_t width, uint32_t height) {
    if (vita_vgl_ != 1) return nullptr;
    return VglDepthSurface(fbo, width, height);
}

void GlDevice::VitaDepthModes(uint32_t fbo, bool store, bool load) {
    auto* ds = static_cast<SceGxmDepthStencilSurface*>(VitaDepthSurface(fbo, vita_keep_w_, vita_keep_h_));
    if (!ds) return;
    sceGxmDepthStencilSurfaceSetForceStoreMode(ds, store ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
                                                         : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
    sceGxmDepthStencilSurfaceSetForceLoadMode(ds, load ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED
                                                       : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
}

// The old behaviour for the rest of this frame (the depth after a copy at GXM's background values), its surface handed
// back to vitaGL's transient use; each reason is said once.
void GlDevice::VitaDropKeep(const char* why) {
    if (vita_keep_fbo_) VitaDepthModes(vita_keep_fbo_, false, false);
    vita_keep_fbo_ = 0;
    vita_keep_loading_ = false;
    static const char* said[8] = {};
    for (const char*& s : said) {
        if (s == why) return;
        if (!s) {
            s = why;
            std::fprintf(stderr, "[gl] screen copies: the frame's depth/stencil is not kept (%s) -- lost after the copy, "
                         "as before\n", why);
            return;
        }
    }
}

// A frame with screen copies: GXM reads a depth/stencil surface's modes at each sceGxmBeginScene. The frame's first
// scene starts at the background values as always (the frame clears its depth; the shadow bracket clears its stencil)
// and STORES its depth/stencil at its end -- the copy's scene break; the copy turns LOAD on (CopyRect), so each later
// scene of the frame starts with what the scene before it stored. A load therefore always reads what a store of
// the same frame wrote (the DF32M depth's mask bits included). Kept only when every assumption holds: the names verified,
// the target's scene not yet begun (it must begin after the store is set: a target whose scene is still open from before
// -- a frame not presented, a target just re-made and cleared -- keeps nothing this frame), its surface found. A frame
// without copies hands the surface back to vitaGL's transient use (no memory traffic).
void GlDevice::KeepDepthAcrossCopies(SurfaceId color, bool keep) {
    const GlSurface* c = Surface(color);
    const uint32_t fbo = c ? c->fbo : 0;
    keep = keep && vita_keep_depth_ && !vita_copy_none_ && fbo;
    if (vita_keep_fbo_ && (vita_keep_fbo_ != fbo || !keep)) {
        VitaDepthModes(vita_keep_fbo_, false, false);
        vita_keep_fbo_ = 0;
    }
    vita_keep_loading_ = false;
    if (!keep) return;
    if (vita_vgl_ != 1) {
        VitaDropKeep(vita_vgl_ < 0 ? "vitaGL's framebuffers are not the r1488 ones: see the line above"
                                   : "vitaGL's framebuffer names are not verified yet");
        return;
    }
    if (in_use_framebuffer == reinterpret_cast<void*>(uintptr_t(fbo))) {
        VitaDropKeep("the target's GXM scene was already in progress when the frame began");
        return;
    }
    auto* ds = static_cast<SceGxmDepthStencilSurface*>(VglDepthSurface(fbo, c->desc.width, c->desc.height));
    if (!ds) {
        VitaDropKeep("the frame target's vitaGL depth/stencil surface was not found");
        return;
    }
    vita_keep_fbo_ = fbo;
    vita_keep_w_ = c->desc.width;
    vita_keep_h_ = c->desc.height;
    vita_keep_data_ = ds->depthData;
    VitaDepthModes(fbo, true, false);
    vita_kept_frames_++;
}

// The source texture drawn into the destination texture's framebuffer: a scene of its own (vitaGL ends the source's
// scene at the first draw into another framebuffer -- the colour reaches memory -- and Vita3K serves the sampled
// texture from its rendered surface), no read-back, no CPU copy, no sceGxmFinish. False = not possible here (the
// caller copies with glCopyTexSubImage2D).
bool GlDevice::VitaCopyRect(const GlSurface& s, const Rect& sr, const GlSurface& d, const Rect& dr) {
    if (!s.tex || !d.tex || !s.desc.width || !s.desc.height || dr.w <= 0 || dr.h <= 0) return false;
    // surfaces of one size, the same rectangle in both (ps2e_draw_copy_frame: the whole frame; ps2e_draw_copy_frame_region:
    // a region of it)
    if (sr.x != dr.x || sr.y != dr.y || sr.w != dr.w || sr.h != dr.h || d.desc.width != s.desc.width ||
        d.desc.height != s.desc.height || sr.x < 0 || sr.y < 0 || uint32_t(sr.x + sr.w) > s.desc.width ||
        uint32_t(sr.y + sr.h) > s.desc.height)
        return false;
    const int W = int(s.desc.width), H = int(s.desc.height);
    const bool asked_whole = sr.x == 0 && sr.y == 0 && sr.w == W && sr.h == H;
    EndDrawRun();
    FillCensusForeign();   // (the census: the copy's passes and vitaGL's mask quads are not the last draw's samples)
    // THE QUIET COPY (TE_GFX_VITA_COPY_RESTORE unset / auto): neither scene this copy begins -- the copy's, the frame's next
    // -- is begun with vitaGL's scene-start mask-update quad. scene_reset draws that quad (update_scissor_test) when a
    // scene's framebuffer is not the one of the scene before (`old_framebuffer`); told beforehand that it is, it begins the
    // scene as a continuation (its viewport set again, nothing drawn). Allowed when the quad would draw the all-1 mask --
    // vitaGL's scissor test off and no scissor update pending (read here, before this copy changes any state) -- and every
    // scene it skips it in already holds that mask: the copy's framebuffer begins with its depth surface's background
    // mask, 1 (vitaGL's surface init: 0x300 in backgroundControl, bit 8 = the mask), on the device as in the Vita3K that
    // emulates the mask (it clears the mask to the background mask at every scene start); the frame's next scene force-
    // loads the depth / stencil this frame keeps (vita_keep_loading_), the DF32M depth's mask bits with it -- stored at the
    // end of every scene of the frame, all 1 since its first scene's quad (and its background mask is 1 too). The older
    // Vita3K (x86 4194, before the mask emulation ea14eb2) has no mask and painted that quad WHITE into the colour: quiet,
    // nothing is painted, so the frame needs no restore and the copy texture keeps its texels outside a region -- what a
    // region copy needs. Only when the names are verified and the frame keeps its depth; else the noisy copy as before.
    const bool quiet = vita_copy_continue_ && !scissor_test_state && !dirty_scissor_state && vita_vgl_ == 1 &&
                       vita_keep_fbo_ == s.fbo && vita_keep_loading_;
    // the rectangle copied: the region asked when quiet; else the whole frame (the restore below reads the whole copy,
    // and the noisy copy scene's quad has painted the older Vita3K's copy white outside any region)
    Rect r = (quiet && !asked_whole) ? sr : Rect{0, 0, W, H};
    // EVEN SIDES: vitaGL's glViewport halves the size with an integer shift (misc.c: x_scale = width >> 1, y_scale =
    // -(height >> 1)), so over an odd side the viewport is half a pixel short and the copy's texture coordinates (u_src,
    // made for the exact rectangle) land on the neighbouring texel at some pixels -- not the identity. A rectangle grown
    // to even sides (by one pixel, inside the target: W and H are even) maps exactly; the extra texels are the frame's
    // own, so the texture still equals the frame wherever it was copied.
    if (r.w & 1) { if (r.x + r.w < W) ++r.w; else { --r.x; ++r.w; } }
    if (r.h & 1) { if (r.y + r.h < H) ++r.h; else { --r.y; ++r.h; } }
    if (r.x < 0 || r.y < 0 || (r.w & 1) || (r.h & 1)) r = Rect{0, 0, W, H};   // (an odd target: the whole frame)
    if (!copy_fbo_) glGenFramebuffers(1, &copy_fbo_);
    state_.BindFb(GL_FRAMEBUFFER, copy_fbo_);
    if (copy_fbo_tex_ != d.tex) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.tex, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr, "[gl] the screen copy's framebuffer is incomplete (status 0x%x): this copy goes through "
                             "glCopyTexSubImage2D\n", unsigned(status));
            }
            copy_fbo_tex_ = 0;
            BeginDrawRun();
            return false;
        }
        copy_fbo_tex_ = d.tex;
    }
    if (quiet) old_framebuffer = reinterpret_cast<void*>(uintptr_t(copy_fbo_));   // the copy's scene: no mask quad
    state_.Disable(GL_SCISSOR_TEST);
    state_.Disable(GL_BLEND);
    state_.Disable(GL_DEPTH_TEST);   // vitaGL: no depth test = no depth write either (the kept depth is untouched)
    glDisable(GL_STENCIL_TEST);      // (each stencil draw resets it after itself; the copy and the restore are exact anyway)
    state_.ColorMask(true, true, true, true);
    // the copy program's triangle over the viewport `r`, u_src mapping it to the same texels of the source: texel p of the
    // destination = texel p of the source wherever the triangle covers (GXM does not clip at the viewport: the triangle
    // reaches past `r` to the right and up, and copies those texels alike -- nearest, the identity)
    state_.Viewport(r.x, r.y, r.w, r.h);
    state_.UseProgram(shaders_.CopyProgram());
    glUniform4f(shaders_.CopySourceLocation(), float(r.x) / float(W), float(r.y) / float(H), float(r.w) / float(W),
                float(r.h) / float(H));
    state_.ActiveTexture(GL_TEXTURE0);
    state_.BindTex2D(s.tex);
    glBindSampler(0, present_nearest_);
    state_.BindVertexArray(const_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    state_.Viewport(0, 0, W, H);   // (the frame's: vitaGL begins the frame's next scene with the viewport it holds)
    // Back to the source: vitaGL begins a scene that follows one on another framebuffer with its scissor mask update
    // (scene_reset -> update_scissor_test: a full-screen mask-update draw while the scissor test is off). Vita3K before
    // its GXM mask emulation (commit ea14eb2, 2026-10-18 -- the x86 build 4194 tested with is older) draws a mask-update
    // program into the COLOUR attachment as vec4(writing_mask) = white: the frame drawn so far would turn white. The
    // copy is drawn back as that scene's first draw -- an exact restore of every pixel and its alpha (on hardware, and
    // on a newer Vita3K, the mask update writes no colour and this repeats what is already there). Quiet, the frame's next
    // scene is begun as the continuation of its last one (no quad, nothing to restore).
    if (quiet) {
        old_framebuffer = reinterpret_cast<void*>(uintptr_t(s.fbo));
        state_.BindFb(GL_FRAMEBUFFER, s.fbo);
        vita_copies_continued_++;
        Census().copies_continued++;
        // THE COPY TEXTURE'S SAMPLING (gl_device.h vita_copy_sample_base_): the restore below samples the copy texture
        // through present_nearest_, and vitaGL applies a sampler object by rewriting the texture's own GXM record (its
        // filters, address modes, mip count, LOD bias; `overridden` set) -- which the raw draws then hand GXM as it
        // stands. A quiet copy has no restore: one vitaGL draw of a zero-area triangle with the copy texture and that
        // sampler on unit 0 makes the same rewrite (the copy program, no fragment, nothing drawn), as the frame's scene's
        // first draw where the restore was -- so the copy-sampling draws sample the copy as they did after a restore.
        if (vita_copy_sample_base_) {
            if (!null_vao_) {
                static const float kPoint[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};   // three equal vertices: no area
                glGenVertexArrays(1, &null_vao_);
                glGenBuffers(1, &null_vbo_);
                state_.BindVertexArray(null_vao_);
                state_.BindArrayBuffer(null_vbo_);
                glBufferData(GL_ARRAY_BUFFER, sizeof(kPoint), kPoint, GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            }
            state_.BindVertexArray(null_vao_);
            state_.BindTex2D(d.tex);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            Census().copies_restated++;
        }
    } else if (vita_copy_restore_) {
        state_.BindFb(GL_FRAMEBUFFER, s.fbo);
        state_.BindTex2D(d.tex);
        glUniform4f(shaders_.CopySourceLocation(), 0.f, 0.f, 1.f, 1.f);   // uploaded again in the new scene
        glDrawArrays(GL_TRIANGLES, 0, 3);
        vita_copies_restored_++;
        Census().copies_restored++;
    }
    Census().copies++;
    if (r.w == W && r.h == H) Census().copy_whole++;
    Census().copy_pixels += uint64_t(r.w) * uint64_t(r.h);
    glBindSampler(0, 0);
    state_.BindVertexArray(0);
    state_.UseProgram(0);
    state_.BindTex2D(0);
    state_.Enable(GL_DEPTH_TEST);   // the device's default (Present leaves it so too)
    vita_copies_++;
    if (vita_copies_ <= 2 || vita_copies_ % 2000 == 0)
        std::fprintf(stderr, "[gl] screen copy %llu (GPU, %dx%d at %d,%d of %dx%d): the frame's depth/stencil %s (%llu frames "
                     "kept); quiet (no mask quads, nothing restored) %llu copies, restored %llu\n",
                     (unsigned long long)vita_copies_, r.w, r.h, r.x, r.y, W, H, vita_keep_fbo_ == s.fbo ? "kept" : "NOT kept",
                     (unsigned long long)vita_kept_frames_, (unsigned long long)vita_copies_continued_,
                     (unsigned long long)vita_copies_restored_);
    BeginDrawRun();
    return true;
}
#endif

// Rows bottom-up (attachment order); colour RGBA8, depth float.
void GlDevice::Readback(SurfaceId id, const Rect& rect, void* out, size_t stride) {
    const GlSurface* s = Surface(id);
    if (!s || !out || rect.w <= 0 || rect.h <= 0) return;
    GLuint fbo = s->fbo;
    GLuint tmp = 0;
    const bool depth = s->desc.format == SurfaceFormat::Depth32f;
    if (!fbo) {
        glGenFramebuffers(1, &tmp);
        fbo = tmp;
        state_.BindFb(GL_READ_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s->tex, 0);
    } else {
        state_.BindFb(GL_READ_FRAMEBUFFER, fbo);
    }
    const size_t bpp = depth ? 4 : 4;
    const size_t row_bytes = static_cast<size_t>(rect.w) * bpp;
    const bool packed_rows = stride == 0 || stride == row_bytes;
#if !PS2R_HOST_VITA
    glPixelStorei(GL_PACK_ROW_LENGTH, packed_rows ? 0 : static_cast<GLint>(stride / bpp));
#else
    if (!packed_rows) {
        auto* rows = static_cast<uint8_t*>(out);
        for (int y = 0; y < rect.h; ++y) {
            auto* row = rows + static_cast<size_t>(y) * stride;
            if (depth)
                glReadPixels(rect.x, rect.y + y, rect.w, 1, GL_DEPTH_COMPONENT, GL_FLOAT, row);
            else
                glReadPixels(rect.x, rect.y + y, rect.w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
        }
    }
#endif
    if (packed_rows) {
        if (depth)
            glReadPixels(rect.x, rect.y, rect.w, rect.h, GL_DEPTH_COMPONENT, GL_FLOAT, out);
        else
            glReadPixels(rect.x, rect.y, rect.w, rect.h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    }
#if !PS2R_HOST_VITA
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
#endif
    state_.BindFb(GL_READ_FRAMEBUFFER, 0);
    if (tmp) state_.DeleteFramebuffers(1, &tmp);
}

FenceId GlDevice::BeginReadback(SurfaceId id, const Rect& rect) {
    const GlSurface* s = Surface(id);
    if (!s || rect.w <= 0 || rect.h <= 0) return FenceId{};
    PendingReadback pr;
    pr.stride = static_cast<size_t>(rect.w) * 4;
    pr.rows = rect.h;
    pr.bytes.resize(pr.stride * static_cast<size_t>(rect.h));
    Readback(id, rect, pr.bytes.data(), pr.stride);
    const uint64_t f = next_fence_++;
    readbacks_[f] = std::move(pr);
    return FenceId{f};
}

bool GlDevice::PollReadback(FenceId fence, void* out, size_t stride) {
    auto it = readbacks_.find(fence.id);
    if (it == readbacks_.end()) return false;
    const PendingReadback& pr = it->second;
    const size_t dst_stride = stride ? stride : pr.stride;
    for (int r = 0; r < pr.rows; r++)
        std::memcpy(static_cast<uint8_t*>(out) + dst_stride * r, pr.bytes.data() + pr.stride * r,
                    std::min(pr.stride, dst_stride));
    readbacks_.erase(it);
    return true;
}

// Textured quad of the source into the default framebuffer, then swap.
void GlDevice::Present(const ScanoutDesc& desc) {
    const auto present_t0 = std::chrono::steady_clock::now();
    if (desc.filter != PresentationFilter::Surface && desc.filter != PresentationFilter::Nearest &&
        desc.filter != PresentationFilter::Linear)
        throw std::invalid_argument("Graphics presentation filter is unsupported");
    const GlSurface* s = Surface(desc.source);
    Rect source = desc.src;
    float u0 = 0.f, u1 = 1.f, v0 = 0.f, v1 = 1.f;
    if (desc.source.id) {
        if (!s || !s->tex || !s->desc.width || !s->desc.height ||
            s->desc.width > uint32_t(INT32_MAX) || s->desc.height > uint32_t(INT32_MAX))
            throw std::invalid_argument("Graphics presentation source is unavailable");
        // An entirely unspecified source retains the existing full-image API.
        // Every explicit rectangle must fit before ending work or clearing the
        // window. Subtraction avoids overflow in malformed origin + extent.
        if (!source.x && !source.y && !source.w && !source.h)
            source = {0,0,int32_t(s->desc.width),int32_t(s->desc.height)};
        if (source.x < 0 || source.y < 0 || source.w <= 0 || source.h <= 0 ||
            uint32_t(source.x) >= s->desc.width || uint32_t(source.y) >= s->desc.height ||
            uint32_t(source.w) > s->desc.width-uint32_t(source.x) ||
            uint32_t(source.h) > s->desc.height-uint32_t(source.y))
            throw std::invalid_argument("Graphics presentation crop exceeds its source");
        u0 = float(source.x)/float(s->desc.width);
        u1 = float(source.x+source.w)/float(s->desc.width);
        v0 = float(source.y)/float(s->desc.height);
        v1 = float(source.y+source.h)/float(s->desc.height);
        if (!desc.flip_y) std::swap(v0,v1);
    }
    EndDrawRun();
#if PS2R_HOST_VITA
    {
        // the arms this frame was drawn in (counted before the flips below and after the swap)
        GlCensus& c = Census();
        c.presents_counted++;
        if (shaders_.PacketsEnabled()) c.presents_packets_on++;
        if (vita_mesh_index_ == 1 || vita_mesh_index_ == 2 || (vita_mesh_index_ == 3 && vita_mesh_ab_on_)) c.presents_meshes_indexed++;
    }
    FillCensusEnd();   // the fill census's counters off before the scanout (a sampled frame's last draw was made)
    // TE_GFX_VITA_MESH_INDEX=abN: the next frame's form of the native meshes (this present ends the arm's N-th frame); a
    // short arm (N < 60: ab1 alternates every present, the per-present GPU A/B of vita_gpu_census) flips silently -- the
    // census lines name the arms
    if (vita_mesh_ab_ > 0 && ++vita_mesh_ab_presents_ % uint64_t(vita_mesh_ab_) == 0) {
        vita_mesh_ab_on_ = !vita_mesh_ab_on_;
        if (vita_mesh_ab_ >= 60)
            std::fprintf(stderr, "[gl] native meshes A/B: present %llu: %s\n", (unsigned long long)vita_mesh_ab_presents_ + 1,
                         vita_mesh_ab_on_ ? "indexed" : "the lists");
    }
    // the frame target's scene is still the one in progress here (the switch below ends it): vitaGL's names checked
    // until verified (0 = not yet seen, 2 = the name proven, its surface not ready yet)
    if ((vita_vgl_ == 0 || vita_vgl_ == 2) && s && s->fbo) VitaCheckFramebufferNames(*s);
    // TE_GFX_VITA_COPY=ab[N]: the next frame's arm (this present ends the arm's N-th frame)
    if (vita_copy_ab_ > 0 && ++vita_ab_presents_ % uint64_t(vita_copy_ab_) == 0) {
        vita_copy_none_ = !vita_copy_none_;
        std::fprintf(stderr, "[gl] copy A/B: present %llu: %s\n", (unsigned long long)vita_ab_presents_ + 1,
                     vita_copy_none_ ? "off" : "on");
    }
#endif
    // An armed capture draws the window image into an owned framebuffer of the
    // window's size (the default framebuffer of a hidden window is not the
    // window's size, and its pixels are not readable), reads it back, and
    // mirrors it onto the window. Unarmed, the window is drawn directly.
    const int capture_w = window_w_ > 0 ? window_w_ : desc.dst.w;
    const int capture_h = window_h_ > 0 ? window_h_ : desc.dst.h;
    const bool capture = present_capture_ && capture_w > 0 && capture_h > 0 && CaptureTarget(capture_w, capture_h);
    if (!desc.to_window && !capture) return;   // a resolve nobody reads: nothing to do
    state_.BindFb(GL_FRAMEBUFFER, capture ? capture_fbo_ : 0);
    state_.Disable(GL_SCISSOR_TEST);
    state_.ColorMask(true, true, true, true);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (s && s->tex) {
        const int vw = desc.dst.w > 0 ? desc.dst.w : window_w_;
        const int vh = desc.dst.h > 0 ? desc.dst.h : window_h_;
        state_.Viewport(desc.dst.x, desc.dst.y, vw, vh);
        state_.UseProgram(desc.dither16 ? shaders_.ScanoutDitherProgram() : shaders_.ScanoutProgram());
        state_.ActiveTexture(GL_TEXTURE0);
        state_.BindTex2D(s->tex);
        const uint32_t sampler = desc.filter == PresentationFilter::Linear ? present_linear_ :
            desc.filter == PresentationFilter::Nearest ? present_nearest_ : 0;
        glBindSampler(0,sampler);
        if (desc.dither16) {
            glUniform1i(shaders_.ScanoutDitherTextureLocation(), 0);
            glUniform2f(shaders_.ScanoutDitherSourceSizeLocation(), float(s->desc.width), float(s->desc.height));
            GLint dimx[16];
            for (int i = 0; i < 16; ++i) {
                const int cell = int((desc.dimx >> (4 * i)) & 7u);
                dimx[i] = cell >= 4 ? cell - 8 : cell;   // 3-bit two's complement
            }
            glUniform1iv(shaders_.ScanoutDitherMatrixLocation(), 16, dimx);
        } else {
            glUniform1i(shaders_.ScanoutTextureLocation(), 0);
        }
        state_.Disable(GL_BLEND);
        state_.Disable(GL_DEPTH_TEST);
        const float quad[]{
            -1.f,-1.f,u0,v1, 1.f,-1.f,u1,v1, -1.f,1.f,u0,v0,
             1.f,-1.f,u1,v1, 1.f, 1.f,u1,v0, -1.f,1.f,u0,v0,
        };
        state_.BindArrayBuffer(scanout_vbo_);
#if PS2R_HOST_VITA
        // vitaGL's glBufferSubData on a buffer drawn in the last 4 frames is a copy-on-write: a new allocation, the old
        // contents copied, the old block to the garbage collector (buffers.c glNamedBufferSubData) -- every present. The
        // quad only changes with the crop: uploaded when it differs from what the buffer holds.
        if (!scanout_quad_valid_ || std::memcmp(scanout_quad_, quad, sizeof quad) != 0) {
            glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(quad),quad);
            std::memcpy(scanout_quad_, quad, sizeof quad);
            scanout_quad_valid_ = true;
            Census().buffer_subdata++;
        } else {
            Census().scanout_same++;
        }
#else
        glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(quad),quad);
#endif
        state_.BindVertexArray(scanout_vao_);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindSampler(0,0); // the next draw observes only its own texture parameters
        state_.BindVertexArray(0);
        state_.UseProgram(0);
        state_.Enable(GL_DEPTH_TEST);
    }
    presented_w_ = presented_h_ = 0;
    if (capture) {
        // The frame dump's presented image: the whole window as drawn above
        // (the display quad at its offset, black around it), bottom-up.
        presented_.assign(static_cast<size_t>(capture_w) * capture_h * 4, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, capture_w, capture_h, GL_RGBA, GL_UNSIGNED_BYTE, presented_.data());
        presented_w_ = capture_w;
        presented_h_ = capture_h;
        if (!desc.to_window) {   // resolved for the capture only: the window keeps what it shows
            state_.BindFb(GL_FRAMEBUFFER, 0);
            return;
        }
        state_.BindFb(GL_READ_FRAMEBUFFER, capture_fbo_);
        state_.BindFb(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, capture_w, capture_h, 0, 0, capture_w, capture_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        state_.BindFb(GL_FRAMEBUFFER, 0);
    }
    if (swap_) {
        // the present's split (the census): the draw part (this function so far: the frame target's scene ended by the
        // switch to the display, the display's scene begun by its clear) and the swap (the Vita: vglSwapBuffers -- the
        // display scene's end, the display queue entry, vitaGL's garbage-collector hand-off)
        const auto t_swap = std::chrono::steady_clock::now();
        swap_();
        const auto t_done = std::chrono::steady_clock::now();
#if PS2R_HOST_VITA
        FillCensusAfterSwap();   // (outside a scene here: the fill census's visibility buffer is set at the first swap after it is wanted)
#endif
        GlCensus& c = Census();
        c.present_draw_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t_swap - present_t0).count());
        const uint64_t swap_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t_done - t_swap).count());
        c.swap_us += swap_us;
        if (swap_us > c.swap_max_us) c.swap_max_us = swap_us;
        c.presents++;
#if PS2R_HOST_VITA
        // the A/B arm this frame was drawn with (the lean programs' when their A/B runs, else the fast program's), and its A/B
        const int arm = vita_ps2_lean_ab_ ? (shaders_.Ps2LeanEnabled() ? 1 : 0) : (shaders_.Ps2FastEnabled() ? 1 : 0);
        c.arm_presents[arm]++;
        c.arm_swap_us[arm] += swap_us;
        if (vita_last_present_valid_)
            c.arm_interval_us[arm] += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t_done - vita_last_present_).count());
        vita_last_present_ = t_done;
        vita_last_present_valid_ = true;
        if (vita_ps2_fast_ab_ > 0 && ++vita_ps2_ab_presents_ % uint64_t(vita_ps2_fast_ab_) == 0) {
            shaders_.SetPs2FastEnabled(!shaders_.Ps2FastEnabled());
            std::fprintf(stderr, "[gl] PS2 fast program A/B: present %llu: %s\n", (unsigned long long)vita_ps2_ab_presents_ + 1,
                         shaders_.Ps2FastEnabled() ? "on" : "off");
        }
        if (vita_ps2_lean_ab_ > 0 && ++vita_ps2_ab_presents_ % uint64_t(vita_ps2_lean_ab_) == 0) {
            shaders_.SetPs2LeanEnabled(!shaders_.Ps2LeanEnabled());
            std::fprintf(stderr, "[gl] lean PS2 programs A/B: present %llu: %s\n", (unsigned long long)vita_ps2_ab_presents_ + 1,
                         shaders_.Ps2LeanEnabled() ? "on" : "off");
        }
        c.ps2_fast_active = vita_ps2_lean_ab_ ? (shaders_.Ps2LeanEnabled() ? 1 : 0) : (shaders_.Ps2FastEnabled() ? 1 : 0);
        if (vita_raw_ab_ > 0 && ++vita_raw_ab_presents_ % uint64_t(vita_raw_ab_) == 0) {
            shaders_.SetRawMeshOn(!shaders_.RawMeshEnabled());
            std::fprintf(stderr, "[gl] raw mesh draws A/B: present %llu: %s\n", (unsigned long long)vita_raw_ab_presents_ + 1,
                         shaders_.RawMeshEnabled() ? "on" : "off");
        }
        if (vita_packets_ab_ > 0 && ++vita_packets_ab_presents_ % uint64_t(vita_packets_ab_) == 0) {
            shaders_.SetPacketsOn(!shaders_.PacketsEnabled());
            std::fprintf(stderr, "[gl] mesh draw packets A/B: present %llu: %s\n", (unsigned long long)vita_packets_ab_presents_ + 1,
                         shaders_.PacketsEnabled() ? "on" : "off");
        }
#endif
    }
}

bool GlDevice::CaptureTarget(int w, int h) {
    if (capture_fbo_ && capture_w_ == w && capture_h_ == h) return true;
    if (!capture_tex_) glGenTextures(1, &capture_tex_);
    state_.ActiveTexture(GL_TEXTURE0);
    state_.BindTex2D(capture_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
#if !PS2R_HOST_VITA
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
#endif
    if (!capture_fbo_) glGenFramebuffers(1, &capture_fbo_);
    state_.BindFb(GL_FRAMEBUFFER, capture_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, capture_tex_, 0);
    const GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
#if !PS2R_HOST_VITA
    glDrawBuffers(1, bufs);
#else
    (void)bufs;
#endif
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    state_.BindFb(GL_FRAMEBUFFER, 0);
    capture_w_ = complete ? w : 0;
    capture_h_ = complete ? h : 0;
    return complete;
}

void GlDevice::ArmPresentCapture(bool armed) {
    present_capture_ = armed;
    if (!armed) {
        presented_.clear();
        presented_w_ = presented_h_ = 0;
    }
}

bool GlDevice::ReadPresented(std::vector<uint8_t>* rgba, int* w, int* h) {
    if (!present_capture_ || presented_w_ <= 0 || presented_h_ <= 0 || !rgba || !w || !h) return false;
    *rgba = presented_;
    *w = presented_w_;
    *h = presented_h_;
    return true;
}

void GlDevice::SetWindowSize(int w, int h) {
    window_w_ = w;
    window_h_ = h;
}

void GlDevice::SetOption(const char* name, int value) {
    if (!name) return;
    if (std::strcmp(name, "draw_legacy") == 0) {
        if (executor_) executor_->SetLegacy(value != 0);
        uniforms_.SetDeferred(value == 0);
    }
}

void GlDevice::BeginFrame() {
#if PS2R_HOST_VITA
    // the per-present GPU A/B (GlCensus::gpu_ab_arm): the native meshes' form this frame is drawn in, when their A/B runs
    // (TE_GFX_VITA_MESH_INDEX=abN; ab1 alternates every present) -- every scene the frame ends carries it (vita_gpu_census)
    GlCensus& c = Census();
    if (vita_mesh_ab_ > 0) {
        c.gpu_ab_arm = vita_mesh_ab_on_ ? 1 : 0;
        c.gpu_ab_name = "the native meshes";
        c.gpu_ab_arms[0] = "the triangle lists";
        c.gpu_ab_arms[1] = "indexed";
    }
#endif
    BeginDrawRun();
}
void GlDevice::EndFrame() { EndDrawRun(); }
void GlDevice::BeginDrawRun() { vertices_.BeginDrawRun(); }
void GlDevice::EndDrawRun() { vertices_.EndDrawRun(state_); }

}  // namespace render
}  // namespace ps2r
