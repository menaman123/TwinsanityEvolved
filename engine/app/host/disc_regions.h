#pragma once
// disc_regions -- the region-aware loader's table and its address maps. Plain C++17 (no Qt):
// the game selects how it loads a disc's program from it, the launcher says why a disc does not play yet.
//
// The port runs the decompilation's C++, compiled for the European executable's addresses (SLES_525.68, PAL 1.01). Each
// release's program is one row, keyed by its serial: the region, the console's field rate and the port's, the entry
// point, the data image the C++ reads (in that executable's own addresses) and whether it loads as it is (the
// decompilation's own layout: PAL) or through an ADDRESS MAP (region_map_<serial>.json, written by
// tools/discs/elf_region_map.py from the two executables): the map lays the other executable's data image out at the PAL
// addresses -- its segments copied, its pointer words translated -- and the game then continues exactly as for PAL.
// Without a map the row's program is refused with a sentence a player can read.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "app/host/disc_identity.h"

namespace ps2r::app {

struct RegionEntry {
    std::string serial;          // "SLES-52568"
    std::string executable;      // "SLES_525.68" (the PlayStation 2 naming convention)
    DiscIdentity::Region region = DiscIdentity::Region::Unknown;
    std::string disc_words;      // "the European disc", "the North American disc", "the Japanese disc"
    uint32_t native_hz = 60;     // the console's field rate: 50 PAL, 60 NTSC
    uint32_t default_hz = 60;    // the port's rate without --hz / game.hz
    bool offers_50 = false;      // game.hz = 50 (the PAL console's speed) applies
    uint32_t entry = 0;          // the ELF entry point (0 = not known)
    // the data image the C++ reads, in THIS executable's addresses (0 = not known without the executable)
    uint32_t image_start = 0, bss_start = 0, image_end = 0, code_start = 0, code_end = 0;
    std::string elf_sha256;      // the executable these facts are for ("" = not known)
    uint64_t elf_bytes = 0;
    bool native = false;         // the decompilation's own layout: loaded as it is
    std::string map_file;        // the map a non-native program needs: "region_map_SLUS-20909.json"
    std::string source;          // where the facts come from
};

// The table: PAL (native, the constants of runner/te_env.h), NTSC-U and NTSC-J (a map needed; none made yet).
const std::vector<RegionEntry>& RegionTable();
const RegionEntry* RegionEntryFor(const std::string& serial);   // "SLUS-20909" or "SLUS_209.09"; nullptr = not a release

// A region map (launcher/discs/region_map_<serial>.json, kind "region-map").
struct RegionMap {
    struct Range { uint32_t pal = 0, other = 0, bytes = 0; };
    std::string serial, executable;
    std::string elf_sha256;           // the executable the map was made from (the one it applies to)
    uint64_t elf_bytes = 0;
    uint32_t entry = 0;
    std::string pal_elf_sha256;       // the PAL executable it was made against
    uint32_t image_start = 0, bss_start = 0, image_end = 0;   // the PAL data image
    std::vector<Range> segments;      // PAL [pal, pal + bytes) <- other [other, other + bytes)
    std::vector<Range> bss;           // address ranges only (no bytes): pointer translation
    std::vector<uint32_t> pointers;   // PAL addresses whose word is translated
    std::vector<std::pair<uint32_t, uint32_t>> code, data;   // other -> PAL, sorted by other
    // schema 2: PAL words written last, as they are (PAL function addresses: a code pointer of a PAL-only object, a
    // slot whose other function the PAL code does not have) -- addresses only
    std::vector<std::pair<uint32_t, uint32_t>> fills;        // PAL address -> value, sorted by address
    // schema 2: the file names the two programs spell differently (a matched string object of the data image that
    // differs and names a path: "StartUp\Fonts\Crash_Euro" on the European disc, "StartUp\Fonts\Crash" on this one).
    // The PAL C++ names its files the PAL way -- some by its own literals -- so the loader gives this disc's archive
    // entries the PAL names too (RenameArchiveEntries over CRASH6/CRASH.BH): names, no bytes
    struct Name { std::string pal, other; uint32_t at = 0; };
    std::vector<Name> names;
    uint32_t schema = 1;
    std::string origin;               // where it was read from (a file, "built in")
};

bool ParseRegionMap(const std::string& json, RegionMap* out, std::string* error);
bool LoadRegionMap(const std::string& path, RegionMap* out, std::string* error);
// The maps a run may use (the built-in ones; the one named on the command line or found beside the game).
void AddRegionMap(const RegionMap& map);
// A map's TEXT, read only when a disc of its serial needs it (the built-in tables: a map is half a megabyte, and a PAL
// disc never reads one). The pointer form keeps the caller's storage (static: the built-in tables); the string form a copy.
// False (nothing registered) for a text with no "serial" among its first keys: a map is keyed by its release.
bool AddRegionMapText(const char* text);
bool AddRegionMapText(const std::string& text);
// The texts of `serial`'s maps read now (a map that does not read is logged and dropped); RegionMaps() lists them after.
void ParsePendingRegionMaps(const std::string& serial);
void ClearRegionMaps();
const RegionMap* FindRegionMap(const std::string& serial, const std::string& elf_sha256);   // reads the serial's texts first
const std::vector<RegionMap>& RegionMaps();     // the maps read so far
bool HasRegionMap(const std::string& serial);   // any map for this serial (whatever executable), read or not

// The sentence a player reads when a release's program has no map: "This is the North American disc (SLUS-20909). The
// port runs the European game's code and needs this disc's address map, which has not been made yet: ..."
std::string RegionRefusal(const RegionEntry& entry);
// The launcher's reason a disc does not play: the refusal for a release without a map, "" for one that plays.
std::string RegionNotPlayable(const std::string& serial);

// The data image laid out in guest memory `mem` (guest address = offset, `mem_size` bytes) from `elf` through `map`:
// [image_start, image_end) zeroed, the segments copied from the ELF's PT_LOAD file bytes, every listed pointer word
// translated (code pairs, data pairs, then the segments and bss ranges), the fills written. False with *error when the
// ELF is not the one the map was made from or does not cover a segment.
struct MapLoadReport {
    uint32_t segments = 0;
    uint64_t bytes_copied = 0;
    uint32_t pointers = 0, by_code = 0, by_data = 0, by_range = 0, unmapped = 0;
    uint32_t fills = 0;
};
bool LoadImageThroughMap(const std::vector<uint8_t>& elf, const RegionMap& map, uint8_t* mem, size_t mem_size, MapLoadReport* report,
                         std::string* error);
// The native load as a plain function (what runner/te_env.cpp LoadRetailImage does to the module's memory): the PT_LOAD
// file bytes covering [image_start, bss_start) copied, [bss_start, image_end) zeroed. The tests' reference.
bool LoadImageNative(const std::vector<uint8_t>& elf, uint32_t image_start, uint32_t bss_start, uint32_t image_end, uint8_t* mem,
                     size_t mem_size, std::string* error);
std::string Sha256OfBytes(const std::vector<uint8_t>& bytes);

// The game's archive index (CRASH6/CRASH.BH: s32 header word, then {s32 name length, the name, u32 offset, u32 size} per
// entry) with every entry this disc names `other[.ext]` (any case) given the PAL name `pal[.ext]` -- unless
// an entry of that PAL name is already there. Offsets and sizes stay: only the index's names change. `renamed` gets one
// "old -> new" line per entry. False with *why when the index does not read.
bool RenameArchiveEntries(const std::vector<uint8_t>& index, const std::vector<RegionMap::Name>& names, std::vector<uint8_t>* out,
                          std::vector<std::string>* renamed, std::string* why);

}  // namespace ps2r::app
