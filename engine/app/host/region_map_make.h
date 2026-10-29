#pragma once
// region_map_make -- the ADDRESS MAP of another release's executable, made in C++: the same
// algorithm as tools/discs/elf_region_map.py `map` (schema 2), and the same FILE -- the JSON is written exactly as the
// Python tool writes it (platform/nativeee/tests/test_region_map compares the two byte for byte), so the mod tool can
// offer it to a player with both discs:
//
//   twinsanityevolved-modtool region-map --pal <the European disc> --disc <this disc> --out region_map_<serial>.json
//
// The two .text sections are aligned instruction by instruction (relocated fields masked; difflib's matching blocks,
// ported: Ratcliff-Obershelp with the popular-element rule); the aligned address-forming instructions vote where each
// PAL data object went; each data section is aligned by content (a code pointer = the PAL function it is, a pointer to a
// string = the string); every PAL data object gets its counterpart by the strongest evidence (content, the code, a call's
// arguments, the pointers to it, a part of the content, the instructions of a gap, the same words elsewhere, its
// neighbours), one other object per PAL object; the layout is made object by object, the pointer words of the laid-out
// image paired, the fills and the file names recorded -- addresses and names only, no byte of either executable. Plain C++17.
#include <cstdint>
#include <string>
#include <vector>

namespace ps2r::app {

// The map's "made_by" (the same in both tools)
inline constexpr const char* kRegionMapMadeBy =
    "tools/discs/elf_region_map.py map = twinsanityevolved-modtool region-map (schema 2): addresses and names only, no byte of either executable";

struct RegionMapMakeOptions {
    std::string serial;          // the other executable's serial ("SLUS-20909")
    std::string executable;      // its file name ("SLUS_209.09")
    std::string symbols_path;    // game/symbol_addrs.txt (the PAL objects and functions); "" = symbols_text, else the built-in copy
    std::string symbols_text;    // the same, as text
};

struct RegionMapMakeReport {
    uint32_t segments = 0, bss = 0, pointer_words = 0, code_pairs = 0, code_conflicts = 0, data_pairs = 0, data_consistent = 0;
    uint32_t data_exceptions = 0, symbols = 0, symbols_mapped = 0, fills = 0, code_no_pal_function = 0, data_unmapped = 0, names = 0;
    uint64_t aligned_bytes = 0, data_image_bytes = 0, differing_bytes = 0;
};

// The map of `other` against `pal` as JSON (kind "region-map", schema 2: the file tools/discs/elf_region_map.py writes);
// false with *error when an executable does not read.
bool MakeRegionMap(const std::vector<uint8_t>& pal, const std::vector<uint8_t>& other, const RegionMapMakeOptions& options,
                   std::string* json, RegionMapMakeReport* report, std::string* error);

// The symbol list a build carries (the mod tool's: game/symbol_addrs.txt embedded at configure time, registered by a static
// initialiser of the generated file); none in the game itself.
void SetDefaultPalSymbols(const char* symbol_addrs_text);
const char* DefaultPalSymbols();

// difflib.SequenceMatcher(None, a, b, autojunk=True).get_matching_blocks(), ported: (i, j, n) with the terminal (len a,
// len b, 0). Exposed for the tests.
struct MatchBlock { uint32_t a, b, n; };
std::vector<MatchBlock> MatchingBlocks(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b);

// The mod tool's sub-command: region-map --pal <disc or ELF> --disc <disc or ELF> [--out FILE] [--serial S]
// [--symbols game/symbol_addrs.txt] [--json]. Exit 0 done, 1 failed, 2 a usage error (the mod tool's convention).
int RegionMapCommand(int argc, char** argv);

}  // namespace ps2r::app
