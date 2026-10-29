// te_savestate_keys.h -- F5 / F8, the player's save state: F5 saves the running game to its slot, F8 loads the slot back in
// place (the same process: the game continues from the saved moment). The slot is <root>/savestates/slot<N>.ps2st, the root
// the GAME FOLDER (the folder holding ps2r.ini: the launcher's game folder; the PS Vita's ux0:data/twinsanity), else
// paths.save_dir when ps2r.ini names one; N is save.slot (0 by default). Every press is logged ("[twinsanityevolved]
// savestate: ...") and answered on screen for two seconds through the game's own text system (te_texts.h): "state saved to
// slot 0", "state loaded from slot 0", "no saved state in slot 0", or why not. The keys are polled at the frame boundary
// (te::input::KeyHeld: the keyboard, or --inject-key F5@N for a run without one); the save / load itself is the runner's
// (te::savestate::RequestSave / RequestLoad, taken at the boundary's checkpoint), which reports back with Saved / Loaded.
#pragma once
#include <cstdint>
#include <string>

namespace te::savestate_keys {

// <root>/savestates and its slot file
std::string Directory(const std::string& root);
std::string SlotPath(const std::string& root, int slot);

// The keys' boundary hook and the notice's text source (once; a second call only moves the root / slot)
void Install(const std::string& root, int slot);
bool Installed();
const std::string& Root();
int Slot();

// The runner's answer to a save / load (every one it performed: only the keys' own are answered on screen)
void Saved(const std::string& path, bool ok, const std::string& error);
void Loaded(const std::string& path, bool ok, const std::string& error);
// Whether `path` is the slot F5 asked for and the runner has not answered yet (the runner writes its sidecar then)
bool IsPendingSave(const std::string& path);

// The notice on screen now ("" = none) and the frames it has left (the judge reads the log; tests read these)
std::string Notice();
uint32_t NoticeFramesLeft();

}  // namespace te::savestate_keys
