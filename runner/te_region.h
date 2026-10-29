// runner/te_region.h -- the region-aware loader on the runner's side (engine/app/host/disc_regions.h: the table, the maps).
// The row is chosen by the disc's IDENTITY -- the program SYSTEM.CNF boots and its serial, never the image's file name:
//   the PAL program (the decompilation's own layout)  loaded exactly as before (te::LoadRetailImage), 60 Hz by default,
//                                                     50 offered (game.hz / --hz)
//   another release's program with its address map   its data image laid out at the PAL addresses through the map
//                                                     (the map's executable sha256 must be this one's), 60 Hz
//   another release's program without a map          refused with the readable sentence (RegionRefusal)
//   a program the table does not know                loaded as the PAL layout (the loader refuses one that is not)
// The maps: built in (launcher/discs/region_map_*.json), --region-map FILE, or <game folder>/region_map_<serial>.json.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "app/host/disc_identity.h"
#include "app/host/disc_regions.h"
#include "te_env.h"

namespace te::region {

struct Selection {
    const ps2r::app::RegionEntry* entry = nullptr;   // the release's row; nullptr for a program the table does not know
    std::string executable;     // the program to load (SYSTEM.CNF's); "" = the runner's default name
    std::string serial;
    bool native = true;         // the PAL layout: te::LoadRetailImage
    bool refused = false;       // a release that needs an address map and has none
    std::string message;        // the readable refusal
    uint32_t default_hz = 60;   // the field rate without --hz / game.hz
    bool offers_50 = true;      // game.hz = 50 applies
    std::vector<std::string> maps;   // the maps registered for the serial (where they came from)
    bool identified = false;    // the identity accepted the disc (a refused or absent disc: the PAL layout, as before)
};

// The row for the identity (`identified` = the identity accepted the disc); registers the maps it may use first.
Selection Select(const ps2r::app::DiscIdentity& id, bool identified, const std::string& map_arg, const std::string& game_folder);
// "region: PAL SLES-52568, the decompilation's own layout, 60 Hz (50 offered)"
std::string Describe(const Selection& s);
// Before the disc opens: the file names this row's map says the two programs spell differently become the PAL names in
// the disc's archive index (one computed CRASH6/CRASH.BH layer over the image and the mods: te::mods::SetRegionFiles).
// Nothing for the PAL layout or a map without names. Returns the line to log ("" for none).
std::string PrepareDisc(const Selection& s);
// The data image into the module's memory: the PAL layout -> te::LoadRetailImage (byte for byte the earlier path); a map ->
// ps2r::app::LoadImageThroughMap (logged), then the same code-pointer patch as for PAL.
bool LoadImage(w2c_env& env, const std::vector<uint8_t>& elf, const Selection& s, const RetailImage& image, std::string* error);

}  // namespace te::region
