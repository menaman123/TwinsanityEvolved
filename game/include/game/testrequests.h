#pragma once

#include "common.h"

struct GameProgress;

// TwinsanityEvolved: what a TEST asks of the game while it plays (Platform::System::NextTestRequest), carried out through the
// game's own code at the start of a frame of play, before the scripts run: an instance warped as PositionWarp warps one, a user
// message sent as SendUserMessage sends one, a game counter set as SetCounter sets one. The platform asks nothing unless a test
// does: the frames of play are the game's own
void ApplyTestRequests(GameProgress* progress);
