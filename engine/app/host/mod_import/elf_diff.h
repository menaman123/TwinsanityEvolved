#pragma once
// A mod's executable (SLES_525.68) against the disc's: what of it the port can use. The port runs the decomp's C++ for
// the game's code and loads only the executable's DATA IMAGE (runner/te_env.cpp LoadRetailImage: the PT_LOAD bytes of
// 0x2D9D90..0x30A480 -- .vutext / .vudata (the VU microcode), .data, .rodata, .sdata), so
//   * data changes apply by construction (the mod's file is served whole through the overlay as disc/SLES_525.68),
//   * .text changes cannot: they are listed by the function that holds them (function_names.h) and never run,
//   * bytes outside the loaded sections (the VU overlay copies .DVP.*, headers, padding) change nothing in the port.
// The sections come from the ELF's section headers; an executable without them is classified by the retail layout
// (tools/gen_retail_image.py SECTIONS).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace te::modimport {

struct ElfSection {
    std::string name;
    uint32_t type = 0, flags = 0, addr = 0, offset = 0, size = 0;
};
struct ElfImage {
    uint32_t entry = 0;
    std::vector<ElfSection> sections;   // from the section headers (empty when there are none)
    struct Load { uint32_t offset, vaddr, filesz, memsz; };
    std::vector<Load> loads;            // PT_LOAD program headers
};
bool ParseElf(const std::vector<uint8_t>& bytes, ElfImage* out, std::string* error);

struct ExecutableDiff {
    uint64_t text = 0, data = 0, rodata = 0, sdata = 0, vu = 0, other = 0;   // changed bytes per class (after `restored`)
    std::vector<std::string> functions;    // the functions whose code changed, by address: "FUN_001ac8d0 Font::ConstructBase"
    std::vector<std::pair<uint32_t, uint32_t>> code_ranges;   // changed .text ranges [begin, end) by address
    std::vector<std::string> warnings;     // sentences for the player
    uint32_t base_entry = 0, mod_entry = 0;
    bool same_size = false;
    // Data words of the mod that point into code the port cannot run -- the mod's own changed code, or the middle of a
    // function: such a word is part of the code change (the port would call an address it has no function for and stop),
    // so the importer gives it back its disc value. Not counted in the data changes above.
    struct Restore {
        uint32_t addr = 0;          // the word's address in the data image
        uint64_t file_offset = 0;   // in the mod's file
        uint32_t was = 0, now = 0;  // the disc's value, the mod's
        std::string target;         // what `now` points into ("FUN_002d6b8c")
    };
    std::vector<Restore> restored;
};
// Compares the disc's executable with the mod's. False (with *error) when the mod's is not a 32-bit MIPS ELF.
bool DiffExecutables(const std::vector<uint8_t>& base, const std::vector<uint8_t>& mod, ExecutableDiff* out, std::string* error);

}  // namespace te::modimport
