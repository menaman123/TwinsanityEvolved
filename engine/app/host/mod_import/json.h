#pragma once
// A small JSON value with a strict reader (RFC 8259: no comments, no trailing commas, duplicate keys refused) and a
// writer (two-space indent, members in insertion order, the way tools/mods/make_fixture_overlay.py writes mod.json).
// The importer's own: the game's reader (platform/nativeee/host/mods_json.h) is outside the engine tree the launcher
// builds from.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace te::modimport::json {

struct Value {
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;                                     // a String's UTF-8; a Number's literal
    std::vector<Value> items;                             // an Array's
    std::vector<std::pair<std::string, Value>> members;   // an Object's, in order

    static Value Str(const std::string& s);
    static Value Num(uint64_t v);
    static Value Bool(bool b);
    static Value Arr();
    static Value Obj();

    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool IsString() const { return type == Type::String; }
    bool IsNumber() const { return type == Type::Number; }
    const Value* Get(const std::string& key) const;
    std::string String(const std::string& key, const std::string& fallback = "") const;
    uint64_t Unsigned(const std::string& key, uint64_t fallback = 0) const;
    // An object's member, added (at the end) when absent
    Value& Set(const std::string& key, Value v);
    Value& Push(Value v);
};

bool Parse(const std::string& text, Value* out, std::string* error);
std::string Write(const Value& v);   // pretty, ends with a newline

}  // namespace te::modimport::json
