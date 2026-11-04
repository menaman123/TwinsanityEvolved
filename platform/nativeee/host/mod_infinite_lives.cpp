// platform/nativeee (host side) -- the INFINITE LIVES mod: one off / on switch on Options > Mods ("infinite lives"). While it
// is on, a death costs no life: the game asks Platform::System::InfiniteLives() where a script takes a life
// (AddLivesCommand::Execute, game/src/game/commandsgame.cpp -- the played character's death script giving -1) and gives 0
// instead, so the game's own count stays where it is, the lives panel shows that number, and the checkpoint restart after the
// death never finds the lives gone (GameController::RestartFromCheckpoint: no game over from the lives). Lives GIVEN still
// count (a hundred wumpa fruit, an extra life), up to the game's most (100). Switched off, the count goes on from where it is.
// Nothing is drawn: the game's font has digits only, so the lives simply stop counting down.
#include <cstdio>

#include "ps2e_seam.h"
#include "te_mods.h"
#include "te_services.h"

namespace te::mods {

namespace {
constexpr const char* kId = "infinite_lives";
}  // namespace

void RegisterInfiniteLives() {
    Mod m;
    m.id = kId;
    m.name = "infinite lives";
    m.version = "1.0.0";
    m.on_enable = [](bool on) {
        if (on) std::fprintf(stderr, "[twinsanityevolved] infinite lives: on -- a death costs no life, the lives shown stay as they are\n");
    };
    Register(m);
}

}  // namespace te::mods

// Platform::System::InfiniteLives (include/platform/system.h): the mod's switch, asked when a script takes a life
extern "C" ps2e_s32 ps2e_system_infinite_lives(void) {
    const bool on = te::mods::Enabled(te::mods::kId);
    if (on) {
        static unsigned kept = 0;
        std::fprintf(stderr, "[twinsanityevolved] infinite lives: a life kept @ boundary %llu (%u so far)\n",
                     (unsigned long long)te::Services::Get().clock().BoundaryCount(), ++kept);
    }
    return on ? 1 : 0;
}
