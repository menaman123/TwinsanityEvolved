// engine/platform/ps2e/ps2e_draw_backend_null.cpp -- the null arm of ps2e_draw_backend.h: no GL context, a
// NullBackend (engine/render/backends/null) -- every call of the typed-draw surface validated and counted, nothing
// rasterised, so a producer runs unchanged on a host whose graphics API has no adapter yet (a console target's first
// boot, a CI box without a display). Selected by PS2R_PS2E_DRAW_BACKEND=null.
#include "engine/platform/ps2e/ps2e_draw_backend.h"

#include <cstdio>

#include "engine/render/backends/null/null_backend.h"

namespace ps2r {
namespace ps2e {

std::unique_ptr<render::Backend> CreateDrawBackend(platform::Window&, platform::GlContext&, bool) {
    std::fprintf(stderr, "[ps2e_draw] null backend: no GL context, nothing rasterised (draws are counted only)\n");
    return std::make_unique<render::NullBackend>();
}

}  // namespace ps2e
}  // namespace ps2r
