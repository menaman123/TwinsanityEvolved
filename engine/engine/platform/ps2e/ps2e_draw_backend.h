// engine/platform/ps2e/ps2e_draw_backend.h -- the arm behind ps2e_draw: the render::Backend the typed-draw surface
// (ps2e_draw.cpp) draws through, made by the ONE translation unit CMake selects with PS2R_PS2E_DRAW_BACKEND
// (ps2e_draw_backend_gl.cpp: a GL 4.1 context on the window + GlDevice; ps2e_draw_backend_null.cpp: NullBackend,
// no context, nothing rasterised). ps2e_draw.cpp itself names no API: everything it does goes through
// engine/render/backend.h, so a new graphics API is a new backend plus a new arm file, not a change to the surface.
#pragma once

#include <memory>

#include "engine/render/backend.h"

namespace ps2r {
namespace platform {
class Window;
class GlContext;
}  // namespace platform

namespace ps2e {

// The backend over `window`, initialised and ready to draw. An arm that needs a GL context creates it on the window
// through `context` (left current on the calling thread, swap interval 0) and presents through the window's swap; an
// arm that needs none leaves `context` uncreated (ps2e_draw then skips the swap-interval and thread-binding calls).
// nullptr when the arm cannot be made (no GL on the host, the device's init failed). `gs_z24`: the producer's Z stays in
// [0, 2^24] (ps2e_draw_config::depth_bits 24) -- the arm maps it onto window depth exactly.
std::unique_ptr<render::Backend> CreateDrawBackend(platform::Window& window, platform::GlContext& context, bool gs_z24);

}  // namespace ps2e
}  // namespace ps2r
