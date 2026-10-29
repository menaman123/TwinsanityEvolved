// platform/sdl/gl_context.cpp -- GlContext implementation.
#include "engine/platform/sdl/gl_context.h"

#include <SDL.h>
#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "engine/platform/sdl/window.h"

#if PS2R_HOST_VITA
#include <psp2/kernel/sysmem.h>   // sceKernelGetFreeMemorySize (the memory left after the GPU's start)
#endif

namespace ps2r {
namespace platform {

GlContext::~GlContext() { Destroy(); }

void GlContext::RequestAttributes() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#ifdef _WIN32
    // 24, not 32, on Windows. The accelerated ICDs (nvoglv64 / igxelpicd64)
    // advertise 24-bit depth with 8 stencil and no 32-bit depth format at all,
    // while the Microsoft GDI Generic software renderer DOES offer 32 -- so
    // asking for 32 does not fail, it silently selects the software format and
    // the context comes back as "GDI Generic" OpenGL 1.1, where none of the 4.1
    // core entry points exist. (The 24-bit retry in platform/sdl/window.cpp only
    // fires when SDL_CreateWindow fails outright, which this does not.)
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#else
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 32);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

bool GlContext::Create(Window& window) {
    Destroy();
    if (!window.Handle()) return false;
#if PS2R_HOST_VITA
    // vitaGL's memory, configured before its init (vgl.c / buffers.c / gxm.c r1488). Each setting gives back RAM-pool
    // memory the device's run4 showed exhausted (vitaGL free: ram 0.1 MB, phycont 0.1 MB; the device's RAM pool is the
    // user RAM left at vglInitExtended minus its 32 MB threshold, ~55 MB) without changing what any draw reads:
    //  - the temporary-data circular pool (CIRCULAR_POOL_SIZE_DEF 32 MB, split over the 3 display buffers and taken from
    //    the RAM pool at init): its users are client-side vertex arrays, glDrawElements index copies, GL_QUADS / fan /
    //    loop index conversions, transposed glUniformMatrix and read-back conversions -- none of which the GL backend
    //    issues per frame (it draws GL_TRIANGLES from VBOs, uniforms through glUniform*v). A frame that outgrows its share
    //    is still served (vgl_reserve_data_pool: gpu_alloc_mapped_for_cpu + the garbage collector), only slower. 3 MB.
    //  - each vertex array object's constant-attribute pool (buffers.c reset_vao: 256 KB for the default VAO, 64 KB for
    //    every other one, from the RAM pool): the backend makes a VAO per native mesh (hundreds), and a pool only ever
    //    holds the 16 attributes' reset values (16 x 4 floats = 256 bytes; reserve_attrib_pool) since nothing calls
    //    glVertexAttrib*. The default VAO's stays 256 KB; the others 1 KB (64 x 16-byte values).
    //  - the garbage collector thread (gxm.c init_gxm: priority 0x10000100 = the process default 160, the lowest of the
    //    busy threads): vglSwapBuffers waits on it (gc_mutex) when it is FRAME_PURGE_FREQ frames behind, so it runs just
    //    below the game thread (96): it only frees one frame's list per swap.
    vglSetCircularPoolSize(3u * 1024u * 1024u);
    vglSetVertexAttribPoolSize(256u * 1024u, 1024u);
    vglSetupGarbageCollector(100, 0);
    //  - the framebuffers' render targets (gxm.c setup_render_target, gxm_fbo_rt_size 1): a render target made for one
    //    scene per frame makes sceGxmBeginScene wait for the GPU to finish its previous scene; the frame target takes a
    //    scene per screen copy (the crystal caves: 3 a frame), so 4 (the display's stays 1: one scene a frame). The order
    //    the GPU draws the scenes in is the order they were ended either way.
    int fbo_scenes = 4;
    if (const char* e = std::getenv("TE_VITA_FBO_SCENES")) fbo_scenes = std::max(1, std::min(8, std::atoi(e)));
    vglSetupRenderTargetScenesNum(1, uint8_t(fbo_scenes));
    std::fprintf(stderr, "[gl] vitaGL pools: circular pool 3 MB (was 32 MB), VAO constant pools 1 KB (was 64 KB; default "
                         "VAO 256 KB), garbage collector at priority 100 (was 160), framebuffer render targets %d scenes a "
                         "frame (was 1; TE_VITA_FBO_SCENES)\n", fbo_scenes);
    //  - GXM's PARAMETER BUFFER (the binned triangles and their vertices' outputs of every scene in flight;
    //    vitaGL hands sceGxmInitialize SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE, 16 MB): TE_VITA_PARAM_BUFFER_MB=N (8..64)
    //    sizes it for an experiment -- unset, the default stands (a size the system cannot give fails the GPU's start:
    //    no fallback). The memory left after the start is logged below.
    if (const char* e = std::getenv("TE_VITA_PARAM_BUFFER_MB")) {
        const int mb = std::max(8, std::min(64, std::atoi(e)));
        vglSetParamBufferSize(uint32_t(mb) * 1024u * 1024u);
        std::fprintf(stderr, "[gl] GXM parameter buffer: %d MB (TE_VITA_PARAM_BUFFER_MB; the default is 16)\n", mb);
    }
    // The result is only whether the requested size fell back to the display's maximum (vitaGL vgl.c), not success.
    // No MSAA: the frame target and every PS2 target are plain single-sampled framebuffers.
    std::fprintf(stderr, "[gl] start-up: vglInitExtended (GXM, the shader compiler module, the GPU memory pools)\n"); std::fflush(stderr);
    const GLboolean resolution_fallback = vglInitExtended(0, 960, 544, 32 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    std::fprintf(stderr, "[gl] vitaGL ready (resolution fallback: %s); pools: vram %.1f MB, ram %.1f MB, phycont %.1f MB "
                 "(free %.1f / %.1f / %.1f MB)\n", resolution_fallback ? "yes" : "no",
                 vglMemTotal(VGL_MEM_VRAM) / 1048576.0, vglMemTotal(VGL_MEM_RAM) / 1048576.0,
                 vglMemTotal(VGL_MEM_PHYCONT) / 1048576.0, vglMemFree(VGL_MEM_VRAM) / 1048576.0,
                 vglMemFree(VGL_MEM_RAM) / 1048576.0, vglMemFree(VGL_MEM_PHYCONT) / 1048576.0);
    std::fflush(stderr);
    {
        // what the system has left once GXM and vitaGL took theirs (the room a bigger parameter buffer would need)
        SceKernelFreeMemorySizeInfo mem{};
        mem.size = sizeof mem;
        if (sceKernelGetFreeMemorySize(&mem) >= 0)
            std::fprintf(stderr, "[gl] free memory after the GPU's start: user %d KB, cdram %d KB, phycont %d KB\n",
                         mem.size_user / 1024, mem.size_cdram / 1024, mem.size_phycont / 1024);
    }
    // compile each shader in glCompileShader (a vertex shader then its fragment shader, as GlShaderLibrary builds them),
    // so a shader Sony's compiler refuses is reported there instead of failing inside glLinkProgram
    vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);
    // the runtime shader compiler's messages (the GLSL translation and Sony's Cg compiler) into the log
    shark_install_log_cb([](const char* msg, shark_log_level level, int line) {
        std::fprintf(stderr, "[gl] shader compiler %s (line %d): %s\n",
                     level == SHARK_LOG_ERROR ? "error" : level == SHARK_LOG_WARNING ? "warning" : "info", line, msg ? msg : "");
        std::fflush(stderr);
    });
    vita_initialized_ = true;
    return true;
#endif
    context_ = SDL_GL_CreateContext(window.Handle());
    if (!context_) {
        std::fprintf(stderr, "[gl] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    if (!MakeCurrent(window)) return false;
#if defined(_WIN32)
    // The GL entry points are function pointers on Windows (engine/platform/windows/gl_loader.h): fill them from this
    // context's driver before the first GL call below. A driver without one of the 4.1-core names the engine uses
    // cannot run the backend: refuse here with the names rather than crash on the first null call.
    const char* missing[8];
    const int n_missing = ps2r::glwin::Load(&SDL_GL_GetProcAddress, missing, 8);
    if (n_missing) {
        std::fprintf(stderr, "[gl] %d of %d GL entry points are missing from this driver (first:", n_missing, ps2r::glwin::Count());
        for (int i = 0; i < n_missing && i < 8; ++i) std::fprintf(stderr, " %s", missing[i]);
        std::fprintf(stderr, ") -- an OpenGL 4.1 core driver is required\n");
        Destroy();
        return false;
    }
#endif
    SetSwapInterval(0);
#if !PS2R_HOST_VITA
    const GLubyte* v = glGetString(GL_VERSION);
    const GLubyte* r = glGetString(GL_RENDERER);
    version_ = v ? reinterpret_cast<const char*>(v) : "";
    renderer_ = r ? reinterpret_cast<const char*>(r) : "";
#endif
    return true;
}

void GlContext::Destroy() {
#if PS2R_HOST_VITA
    vita_initialized_ = false;
#else
    if (context_) {
        SDL_GL_DeleteContext(static_cast<SDL_GLContext>(context_));
        context_ = nullptr;
    }
#endif
}

bool GlContext::MakeCurrent(Window& window) {
#if PS2R_HOST_VITA
    (void)window;
    return vita_initialized_;
#else
    if (!context_ || !window.Handle()) return false;
    if (SDL_GL_MakeCurrent(window.Handle(), static_cast<SDL_GLContext>(context_)) != 0) {
        std::fprintf(stderr, "[gl] SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
#endif
}

void GlContext::SetSwapInterval(int interval) {
#if PS2R_HOST_VITA
    (void)interval;
#else
    SDL_GL_SetSwapInterval(interval);
#endif
}

}  // namespace platform
}  // namespace ps2r
