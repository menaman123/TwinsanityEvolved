#pragma once
// The PAL executable's functions (SLES_525.68, the decomp's symbol table): a code address -> the function that holds it,
// named "FUN_001ac8d0 Font::ConstructBase" (the symbol, then the decomp's C++ name when it has one). The table is
// generated at build time by tools/mods/gen_function_names.py from game/symbol_addrs.txt and the decomp's RETAIL() labels
// (addresses and names only); a build without the decomp's sources links an empty table (function_names_none.cpp) and
// names a changed address by itself.
#include <cstddef>
#include <cstdint>
#include <string>

namespace te::modimport {

struct FunctionName {
    uint32_t address;
    const char* symbol;   // "FUN_001ac8d0", "ReadFont"
    const char* cxx;      // "Font::ConstructBase", "" when the decomp has no name for it
};
extern const FunctionName kFunctionNames[];   // sorted by address
extern const size_t kFunctionNameCount;
extern const uint32_t kCodeStart, kCodeEnd;  // the .text range the table covers (0, 0 for the empty table)

// The function holding `address` ("FUN_001ac8d0 Font::ConstructBase"; "code at 0x001AC8D4" when the table has none),
// *start = its first address (the address itself when unknown)
std::string FunctionAt(uint32_t address, uint32_t* start = nullptr);
// Whether `address` is the first address of a function the table knows
bool IsFunctionStart(uint32_t address);

}  // namespace te::modimport
