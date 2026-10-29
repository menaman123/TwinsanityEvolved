// platform/sdl/window.cpp -- Window implementation.
#include "engine/platform/sdl/window.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>

#include "engine/platform/host/platform.h"
#include "engine/platform/sdl/gl_context.h"
#if PS2R_HOST_VITA
#include PS2R_GL_HEADER
#endif

namespace ps2r {
namespace platform {

namespace {

// The part of the window's display (the first display before there is a window) a window may use: the desktop less the
// taskbar / dock / menu bar. Empty when the video driver cannot tell.
SDL_Rect UsableBounds(SDL_Window* window) {
    int display = window ? SDL_GetWindowDisplayIndex(window) : 0;
    if (display < 0) display = 0;
    SDL_Rect r{0, 0, 0, 0};
    if (SDL_GetDisplayUsableBounds(display, &r) != 0 || r.w <= 0 || r.h <= 0) r = SDL_Rect{0, 0, 0, 0};
    return r;
}

// The client size w x h, its shape kept, made small enough that the window with its frame (border_w across, border_h
// down: the title bar and the edges) takes at most 9/10 of `area` -- the title bar and the buttons always in reach
void FitClient(const SDL_Rect& area, int border_w, int border_h, int& w, int& h) {
    if (area.w <= 0 || area.h <= 0 || w <= 0 || h <= 0) return;
    const int max_w = area.w * 9 / 10 - border_w;
    const int max_h = area.h * 9 / 10 - border_h;
    if (max_w <= 0 || max_h <= 0 || (w <= max_w && h <= max_h)) return;
    const double k = std::min(double(max_w) / w, double(max_h) / h);
    w = std::max(1, int(w * k));
    h = std::max(1, int(h * k));
}

// The window, frame included, in the middle of `area`
void CenterIn(SDL_Window* window, const SDL_Rect& area) {
    if (area.w <= 0 || area.h <= 0) return;
    int top = 0, left = 0, bottom = 0, right = 0;
    if (SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right) != 0) top = left = bottom = right = 0;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    const int x = area.x + std::max(0, (area.w - (w + left + right)) / 2) + left;
    const int y = area.y + std::max(0, (area.h - (h + top + bottom)) / 2) + top;
    SDL_SetWindowPosition(window, x, y);
}

// The frame's size around the client (0 when the window system does not say)
void BorderSize(SDL_Window* window, int& across, int& down) {
    int top = 0, left = 0, bottom = 0, right = 0;
    if (SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right) != 0) top = left = bottom = right = 0;
    across = left + right;
    down = top + bottom;
}

}  // namespace

Window::~Window() { Destroy(); }

bool Window::Create(bool headless, int w, int h, const char* title) {
    Destroy();
#if PS2R_HOST_VITA
    std::fprintf(stderr, "[sdl] start-up: the video subsystem + the window\n"); std::fflush(stderr);
#endif
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "[sdl] SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
            return false;
        }
        video_inited_ = true;
    }
    // SDL_VideoInit resets the GL attributes: request the 4.1 core profile AFTER the
    // video subsystem exists and BEFORE the window is created.
    GlContext::RequestAttributes();
    headless_ = headless;
#if PS2R_HOST_VITA
    // SDL's Vita video driver has no OpenGL: a plain window (the screen) for the Vita's own draw arm. It is the whole
    // 960x544 screen vitaGL draws to -- the present takes its size as the drawable size.
    (void)w; (void)h;
    const int cw = 960;
    const int ch = 544;
    const Uint32 flags = headless ? SDL_WINDOW_HIDDEN : (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#else
    int cw = headless ? 64 : (w > 0 ? w : 640);
    int ch = headless ? 64 : (h > 0 ? h : 448);
    // a window that fits the screen it opens on (the frame not known yet: a title bar and edges allowed for), shown
    // once it is sized and placed
    if (!headless) FitClient(UsableBounds(nullptr), 16, 48, cw, ch);
    const Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | (headless ? 0u : Uint32(SDL_WINDOW_RESIZABLE));
#endif
    window_ = SDL_CreateWindow(title ? title : "NativeEE", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, cw, ch, flags);
    if (!window_) {
        // The requested 32-bit window depth has no GLX visual on Mesa / Xvfb
        // (24+8 is the deepest); the window's own depth buffer is unused -- every
        // GS target is an FBO with its own depth attachment -- so retry at 24.
        // macOS grants 32 on the first attempt and never reaches this branch.
        std::fprintf(stderr, "[sdl] SDL_CreateWindow failed: %s -- retrying with a 24-bit depth visual\n", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        window_ = SDL_CreateWindow(title ? title : "NativeEE", SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED, cw, ch, flags);
    }
    if (!window_) {
        std::fprintf(stderr, "[sdl] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
#if !PS2R_HOST_VITA
    if (!headless) {
        // the real frame measured: the client refitted to it, the whole window centred on the usable area
        const SDL_Rect area = UsableBounds(window_);
        int across = 0, down = 0, fw = cw, fh = ch;
        BorderSize(window_, across, down);
        FitClient(area, across, down, fw, fh);
        if (fw != cw || fh != ch) SDL_SetWindowSize(window_, fw, fh);
        SDL_SetWindowMinimumSize(window_, 320, 240);
        CenterIn(window_, area);
        SDL_ShowWindow(window_);
    }
#endif
    SDL_GetWindowSize(window_, &width_, &height_);
    return true;
}

void Window::Destroy() {
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (video_inited_) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        video_inited_ = false;
    }
    width_ = height_ = 0;
}

void Window::Resize(int w, int h) {
    if (!window_ || w <= 0 || h <= 0) return;
    // never larger than the screen it is on allows; kept centred there
    const SDL_Rect area = UsableBounds(window_);
    int across = 0, down = 0;
    BorderSize(window_, across, down);
    FitClient(area, across, down, w, h);
    SDL_SetWindowSize(window_, w, h);
    CenterIn(window_, area);
    SDL_GetWindowSize(window_, &width_, &height_);
}

void Window::Swap() {
#if PS2R_HOST_VITA
    vglSwapBuffers(GL_FALSE);
#else
    if (window_) SDL_GL_SwapWindow(window_);
#endif
}

}  // namespace platform
}  // namespace ps2r
