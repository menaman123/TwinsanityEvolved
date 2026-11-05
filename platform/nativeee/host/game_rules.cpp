// platform/nativeee (host side) -- the game's rules the player chooses, where retail's own is a fault of its frame timing or a
// design the classic games had otherwise (te_host.h Settings, ps2r.ini):
//   game.slide_steering = fixed | retail (fixed by default): Platform::System::SlideSteeringFixed. The knee slide steers toward
//     its direction at the angle left over the steering's time left, the frame turning that rate for its whole length; in the
//     steering's last frame retail takes as the time left only the part of the frame before the steering ends, and the frame turns
//     its length over that part times the angle (game/src/game/charactermoves.cpp CrouchController::Slide): at 60 Hz 149 clock
//     units over 9, the slide swinging past its direction by up to some tens of degrees -- the 60 Hz slide glitch (the 59.94 Hz
//     NTSC console has a different remainder every slide). Fixed, the time left is never less than half the frame: the last frame
//     turns at most twice its share, as at the PAL disc's 50 Hz (97 of 179 units left), where nothing changes.
//   game.invincible_to_explosions = on | off (off by default): Platform::System::InvincibleToExplosions. Retail lets a hit of 50 or
//     more through the invincibility (CharacterAgent::Contact), and an explosion's is 100: the third mask's invincibility, and the
//     blink after a hurt, do not stop a nitro or a TNT crate. On, they do, as the classic games' Aku Aku did.
// The speedrun mod's runs keep retail's rules for both.
#include <cstdio>

#include "ps2e_seam.h"
#include "te_host.h"
#include "te_mods.h"
#include "te_services.h"

namespace {
bool RetailRules() { return te::mods::Enabled("speedrun"); }
}  // namespace

// Platform::System::SlideSteeringFixed (include/platform/system.h): asked every frame of a slide's steering
extern "C" ps2e_s32 ps2e_system_slide_steering_fixed(void) {
    return te::GetSettings().slide_steering_fixed && !RetailRules() ? 1 : 0;
}

// Platform::System::InvincibleToExplosions: asked when an explosion reaches an invincible character
extern "C" ps2e_s32 ps2e_system_invincible_to_explosions(void) {
    const bool on = te::GetSettings().invincible_to_explosions && !RetailRules();
    if (on) {
        static unsigned spared = 0;
        std::fprintf(stderr, "[twinsanityevolved] invincible to explosions: an explosion spared @ boundary %llu (%u so far)\n",
                     (unsigned long long)te::Services::Get().clock().BoundaryCount(), ++spared);
    }
    return on ? 1 : 0;
}
