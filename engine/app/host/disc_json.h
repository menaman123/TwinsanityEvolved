#pragma once
// disc_json -- the strict, minimal JSON reader the disc tables are read with (launcher/discs/*.json: the file lists, the
// table of known retail dumps, the region maps). Plain C++17, no allocation tricks: objects keep their keys in order,
// numbers are doubles (sizes up to 2^53 are exact; addresses are written as "0x..." strings). Shared by the launcher and
// the game (engine/app/host/disc_identity.cpp, disc_regions.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace ps2r::app::json {

struct Value {
    enum Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> items;          // Array elements; Object values (beside `keys`)
    std::vector<std::string> keys;     // Object keys, in order

    const Value* Field(const char* key) const;              // nullptr when absent or not an object
    std::string Text(const char* key) const;                // the string field, else ""
    bool Flag(const char* key, bool otherwise = false) const;
    double Num(const char* key, double otherwise = 0) const;
    // A number, or a string holding a number ("0x002D9D90", "1234"): addresses and sizes in the tables.
    uint64_t U64(const char* key, uint64_t otherwise = 0) const;
};

// Parses one JSON document. False with *error ("... at byte N") when it does not read.
bool Parse(const std::string& text, Value* out, std::string* error);
// A value that is a number or a numeric string, as an unsigned 64-bit integer (0x hex accepted).
bool ToU64(const Value& v, uint64_t* out);

}  // namespace ps2r::app::json
