// platform/nativeee (host side) -- System::FastForwardCutscene: the in-game cutscene the player skipped with the cross
// button (GameController::Watching) run ahead until the game stops watching. The game's own skips come first (a cutscene
// file's Skip, the scripts' "cutscene skipped" condition); a scripted cutscene without either plays on -- here at three
// fields a frame (the Vita's 20 Hz frame: a step the game's clocks are used to), unpaced, one picture in eight drawn and the
// speakers silent, until the game's state is no longer Watching (the cutscene's end, a pause) or two minutes of game time.
// Play only: the fields are the pacer's kind (a deterministic run's would be absorbed into its frames).
#include <cstdint>

#include "audio_service.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_harness.h"
#include "te_services.h"

namespace {

constexpr uint32_t kFields = 3;
constexpr unsigned kWatching = 13;           // GameController::StateWatching
constexpr uint64_t kMostBoundaries = 2400;   // x 3 fields: two minutes of game time

bool g_on = false;
uint64_t g_since = 0;

void Set(bool on) {
    if (on == g_on) return;
    g_on = on;
    te::Clock& clock = te::Services::Get().clock();
    clock.SetFastForward(on ? kFields : 0);
    te::gfx::SetFastForward(on);
    te::audio::Service().SetSilenced(on);
    if (!on) return;
    g_since = clock.BoundaryCount();
    static bool hooked = false;
    if (hooked) return;
    hooked = true;
    clock.AddBoundaryHook("cutscene fast-forward", [](uint64_t boundary) {
        if (!g_on) return;
        unsigned state = 0, last = 0, next = 0;
        if (!te::harness::ReadGameState(&state, &last, &next) || state != kWatching || boundary - g_since > kMostBoundaries)
            Set(false);
    });
}

}  // namespace

// (a deterministic run keeps its pace: its pictures are the judges' and the frames its pad log was recorded against)
void ps2e_system_fast_forward_cutscene(void) {
    if (!te::GetSettings().deterministic) Set(true);
}
