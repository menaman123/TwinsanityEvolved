// disc_json -- see disc_json.h. The reader the retail lists were first read with (disc_identity.cpp), made shared.
#include "app/host/disc_json.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace ps2r::app::json {
namespace {

struct Cursor {
    const char* p = nullptr;
    const char* end = nullptr;
    std::string error;
};

void SkipSpace(Cursor& c) {
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r')) ++c.p;
}

bool Fail(Cursor& c, const char* what) {
    if (c.error.empty()) c.error = what;
    return false;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool ParseHex4(Cursor& c, uint32_t* out) {
    if (c.end - c.p < 4) return Fail(c, "a short \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        const char h = *c.p++;
        v <<= 4;
        if (h >= '0' && h <= '9') v |= uint32_t(h - '0');
        else if (h >= 'a' && h <= 'f') v |= uint32_t(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') v |= uint32_t(h - 'A' + 10);
        else return Fail(c, "a bad \\u escape");
    }
    *out = v;
    return true;
}

bool ParseString(Cursor& c, std::string* out) {
    if (c.p >= c.end || *c.p != '"') return Fail(c, "a string was expected");
    ++c.p;
    out->clear();
    while (c.p < c.end && *c.p != '"') {
        const char ch = *c.p++;
        if (static_cast<unsigned char>(ch) < 0x20) return Fail(c, "a control character inside a string");
        if (ch != '\\') { *out += ch; continue; }
        if (c.p >= c.end) return Fail(c, "an unfinished escape");
        const char e = *c.p++;
        switch (e) {
            case '"': *out += '"'; break;
            case '\\': *out += '\\'; break;
            case '/': *out += '/'; break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case 'n': *out += '\n'; break;
            case 'r': *out += '\r'; break;
            case 't': *out += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!ParseHex4(c, &cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00) {   // a surrogate pair
                    uint32_t lo = 0;
                    if (c.end - c.p < 6 || c.p[0] != '\\' || c.p[1] != 'u') return Fail(c, "a lone surrogate");
                    c.p += 2;
                    if (!ParseHex4(c, &lo) || lo < 0xDC00 || lo > 0xDFFF) return Fail(c, "a bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(*out, cp);
                break;
            }
            default: return Fail(c, "an unknown escape");
        }
    }
    if (c.p >= c.end) return Fail(c, "an unfinished string");
    ++c.p;
    return true;
}

bool ParseValue(Cursor& c, Value* out, int depth);

bool ParseNumber(Cursor& c, Value* out) {
    const char* start = c.p;
    if (c.p < c.end && *c.p == '-') ++c.p;
    while (c.p < c.end && (std::isdigit(static_cast<unsigned char>(*c.p)) || *c.p == '.' || *c.p == 'e' || *c.p == 'E' ||
                           *c.p == '+' || *c.p == '-'))
        ++c.p;
    const std::string digits(start, c.p);
    char* stop = nullptr;
    out->kind = Value::Number;
    out->number = std::strtod(digits.c_str(), &stop);
    if (digits.empty() || stop != digits.c_str() + digits.size()) return Fail(c, "a bad number");
    return true;
}

bool ParseWord(Cursor& c, const char* word, Value::Kind kind, bool value, Value* out) {
    const size_t n = std::strlen(word);
    if (size_t(c.end - c.p) < n || std::strncmp(c.p, word, n) != 0) return Fail(c, "an unknown word");
    c.p += n;
    out->kind = kind;
    out->boolean = value;
    return true;
}

bool ParseArray(Cursor& c, Value* out, int depth) {
    out->kind = Value::Array;
    ++c.p;
    SkipSpace(c);
    if (c.p < c.end && *c.p == ']') { ++c.p; return true; }
    for (;;) {
        Value item;
        if (!ParseValue(c, &item, depth + 1)) return false;
        out->items.push_back(std::move(item));
        SkipSpace(c);
        if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
        if (c.p < c.end && *c.p == ']') { ++c.p; return true; }
        return Fail(c, "',' or ']' was expected");
    }
}

bool ParseObject(Cursor& c, Value* out, int depth) {
    out->kind = Value::Object;
    ++c.p;
    SkipSpace(c);
    if (c.p < c.end && *c.p == '}') { ++c.p; return true; }
    for (;;) {
        SkipSpace(c);
        std::string key;
        if (!ParseString(c, &key)) return false;
        SkipSpace(c);
        if (c.p >= c.end || *c.p != ':') return Fail(c, "':' was expected");
        ++c.p;
        Value value;
        if (!ParseValue(c, &value, depth + 1)) return false;
        out->keys.push_back(std::move(key));
        out->items.push_back(std::move(value));
        SkipSpace(c);
        if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
        if (c.p < c.end && *c.p == '}') { ++c.p; return true; }
        return Fail(c, "',' or '}' was expected");
    }
}

bool ParseValue(Cursor& c, Value* out, int depth) {
    if (depth > 32) return Fail(c, "nested too deeply");
    SkipSpace(c);
    if (c.p >= c.end) return Fail(c, "a value was expected");
    switch (*c.p) {
        case '{': return ParseObject(c, out, depth);
        case '[': return ParseArray(c, out, depth);
        case '"': out->kind = Value::String; return ParseString(c, &out->text);
        case 't': return ParseWord(c, "true", Value::Bool, true, out);
        case 'f': return ParseWord(c, "false", Value::Bool, false, out);
        case 'n': return ParseWord(c, "null", Value::Null, false, out);
        default: return ParseNumber(c, out);
    }
}

}  // namespace

const Value* Value::Field(const char* key) const {
    if (kind != Object) return nullptr;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return &items[i];
    return nullptr;
}

std::string Value::Text(const char* key) const {
    const Value* v = Field(key);
    return v && v->kind == String ? v->text : std::string();
}

bool Value::Flag(const char* key, bool otherwise) const {
    const Value* v = Field(key);
    return v && v->kind == Bool ? v->boolean : otherwise;
}

double Value::Num(const char* key, double otherwise) const {
    const Value* v = Field(key);
    return v && v->kind == Number ? v->number : otherwise;
}

uint64_t Value::U64(const char* key, uint64_t otherwise) const {
    const Value* v = Field(key);
    uint64_t out = 0;
    return v && ToU64(*v, &out) ? out : otherwise;
}

bool ToU64(const Value& v, uint64_t* out) {
    if (v.kind == Value::Number) {
        if (v.number < 0) return false;
        *out = static_cast<uint64_t>(v.number);
        return true;
    }
    if (v.kind != Value::String || v.text.empty()) return false;
    char* stop = nullptr;
    const unsigned long long n = std::strtoull(v.text.c_str(), &stop, 0);
    if (stop != v.text.c_str() + v.text.size()) return false;
    *out = n;
    return true;
}

bool Parse(const std::string& text, Value* out, std::string* error) {
    Cursor c;
    c.p = text.data();
    c.end = text.data() + text.size();
    Value root;
    if (!ParseValue(c, &root, 0)) {
        if (error) *error = c.error + " at byte " + std::to_string(c.p - text.data());
        return false;
    }
    SkipSpace(c);
    if (c.p != c.end) {
        if (error) *error = "text after the value at byte " + std::to_string(c.p - text.data());
        return false;
    }
    *out = std::move(root);
    return true;
}

}  // namespace ps2r::app::json
