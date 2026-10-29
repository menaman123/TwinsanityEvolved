// te_mods_page.h -- the title's Options > MODS page as the platform answers it (Platform::System::Mod*
// "Mods"). The registry (te_mods.h) holds every mod; the page lists the mods that APPLY WHILE THE GAME
// RUNS -- the built-ins (level select, speedrun mode), in registration order -- and nothing else:
//
//   ROWS     the built-ins only (te::mods::AppliesLive). A mod of the mods folder (kind "disc": a disc overlay) applies when
//            the game starts: it is switched in the launcher (ps2r.ini mods.<id>.enabled, read at start-up) and the game starts
//            with it in place -- the page has no row, switch, greyed line or note for it, and nothing in the game writes its
//            key. A language pack is not a mod at all. Row -> registry index; a row past the page is no mod.
//   SWITCH   a built-in's switch applies at once and is persisted (te::mods::SetEnabled), as it always did.
//   LABELS   the built-in's registered name (lower case, in the game's letters already).
#pragma once
#include <cstdint>
#include <string>

namespace te::mods::page {

// The registry index of a row past the page: no mod (te::mods::At answers null, Enabled false, SetEnabled does nothing)
constexpr uint32_t kNoMod = 0xFFFFFFFFu;

uint32_t Count();
// The registry index (te::mods::At) of the page's row `row`; kNoMod when `row` is past the page
uint32_t Entry(uint32_t row);
// The switch the row shows: the state in force
bool Shown(uint32_t row);
// The player's switch (the seam's SetModEnabled): applied at once and persisted; a row past the page changes nothing
void Choose(uint32_t row, bool on);
std::string Label(uint32_t row);

}  // namespace te::mods::page
