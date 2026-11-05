// platform/nativeee/host -- te_route_steps.h: the answer to Platform::System::RouteStepsReached (include/platform/system.h).
// A script's SetFocusPosition that takes a route's step "straight ahead" (the step's distance along the agent's facing) misses
// the step whenever the agent does not face it closely; a chase without a tolerance that waits to be inside the step's circle
// then stands at the missed point for good -- Evil Crash in Bandicoot Pursuit, whose command's option word holds what the
// authoring tool left in memory (bit 1, the straight-ahead option, among it). The game asks only in that case.
//
//   game.route_steps = reached   (the default) the step's own position is the target: the route goes on
//   game.route_steps = retail    the straight-ahead point, as the console has it
#pragma once

namespace te::route_steps {

// The run's choice (ps2r.ini's game.route_steps; true = reached)
void Configure(bool reached);
bool Reached();

}  // namespace te::route_steps
