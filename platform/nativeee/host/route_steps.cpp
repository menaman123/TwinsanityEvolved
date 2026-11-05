// platform/nativeee/host -- route_steps.cpp: see te_route_steps.h. Platform::System::RouteStepsReached answered from the run's
// setting; each time the game asks (a route's step missed straight ahead) the first few are named in the log.
#include "te_route_steps.h"

#include <cstdio>

#include "ps2e_seam.h"
#include "te_services.h"

namespace te::route_steps {

namespace {
bool g_reached = true;
}  // namespace

void Configure(bool reached) { g_reached = reached; }
bool Reached() { return g_reached; }

}  // namespace te::route_steps

extern "C" ps2e_s32 ps2e_system_route_steps_reached(void) {
    static unsigned asked = 0;
    const bool reached = te::route_steps::Reached();
    if (++asked <= 8)
        std::fprintf(stderr, "[te.route] boundary %llu: a route's step missed straight ahead -> %s (game.route_steps = %s)\n",
                     (unsigned long long)te::Services::Get().clock().BoundaryCount(),
                     reached ? "the step's own position is the target" : "the straight-ahead point kept", reached ? "reached" : "retail");
    return reached ? 1 : 0;
}
