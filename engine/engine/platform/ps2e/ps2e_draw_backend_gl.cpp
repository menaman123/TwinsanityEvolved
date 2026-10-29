// engine/platform/ps2e/ps2e_draw_backend_gl.cpp -- the GL arm of ps2e_draw_backend.h: a GL 4.1 core context on the
// window (current on the calling thread, swap interval 0 -- the producer paces itself) and a GlDevice presenting
// through the window's swap. Selected by PS2R_PS2E_DRAW_BACKEND=gl (the default).
#include "engine/platform/ps2e/ps2e_draw_backend.h"

#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backends/gl/gl_device.h"

namespace ps2r {
namespace ps2e {

std::unique_ptr<render::Backend> CreateDrawBackend(platform::Window& window, platform::GlContext& context, bool gs_z24) {
    if (!context.Create(window)) return nullptr;
    context.MakeCurrent(window);
    context.SetSwapInterval(0);
    render::GlDeviceSettings settings;
    settings.gs_z24 = gs_z24;
    auto device = std::make_unique<render::GlDevice>(settings);
    platform::Window* win = &window;
    if (!device->Init([win] { win->Swap(); })) return nullptr;
    return device;
}

}  // namespace ps2e
}  // namespace ps2r
