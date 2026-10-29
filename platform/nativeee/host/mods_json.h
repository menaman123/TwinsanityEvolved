// mods_json.h -- a strict, dependency-free JSON reader for the mods' manifests (<mods>/<id>/mod.json).
// RFC 8259: objects, arrays, strings (every escape, \uXXXX with surrogate pairs -> UTF-8), numbers, true / false / null;
// nothing else (no comments, no trailing commas, no NaN). Duplicate keys in one object are refused (a manifest that names
// a field twice is ambiguous). Depth at most 64; one value per document, whitespace around it. Errors carry line:column.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace te::json {

struct Value {
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;                                      // a String's UTF-8; a Number's literal as written
    std::vector<Value> items;                              // an Array's
    std::vector<std::pair<std::string, Value>> members;    // an Object's, in document order

    bool IsNull() const { return type == Type::Null; }
    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool IsString() const { return type == Type::String; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsBool() const { return type == Type::Bool; }
    // An object's member, or nullptr (also when this is not an object)
    const Value* Get(const std::string& key) const;
    // An object's string member, `fallback` when absent or not a string
    std::string String(const std::string& key, const std::string& fallback = "") const;
    // An object's number member as an unsigned integer (0 .. 2^53), `fallback` when absent, not a number, negative or
    // fractional
    uint64_t Unsigned(const std::string& key, uint64_t fallback = 0) const;
};

// Parses `text` (UTF-8; a byte-order mark is skipped). False with *error ("line L col C: why") on any deviation.
bool Parse(const std::string& text, Value& out, std::string* error);

}  // namespace te::json
