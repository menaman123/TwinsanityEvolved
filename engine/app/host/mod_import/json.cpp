// json.h implementation.
#include "app/host/mod_import/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace te::modimport::json {

namespace {

struct Cursor {
    const char* p;
    const char* end;
    const char* begin;
    std::string error;
};

bool Fail(Cursor& c, const char* why) {
    if (c.error.empty()) {
        int line = 1, col = 1;
        for (const char* q = c.begin; q < c.p && q < c.end; ++q) {
            if (*q == '\n') { ++line; col = 1; } else { ++col; }
        }
        c.error = "line " + std::to_string(line) + " col " + std::to_string(col) + ": " + why;
    }
    return false;
}

void Space(Cursor& c) {
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r')) ++c.p;
}

void Utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

bool Hex4(Cursor& c, uint32_t* v) {
    if (c.end - c.p < 4) return Fail(c, "a short \\u escape");
    *v = 0;
    for (int i = 0; i < 4; ++i) {
        const char h = *c.p++;
        *v <<= 4;
        if (h >= '0' && h <= '9') *v |= uint32_t(h - '0');
        else if (h >= 'a' && h <= 'f') *v |= uint32_t(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') *v |= uint32_t(h - 'A' + 10);
        else return Fail(c, "a bad \\u escape");
    }
    return true;
}

bool ParseString(Cursor& c, std::string* out) {
    if (c.p >= c.end || *c.p != '"') return Fail(c, "a string was expected");
    ++c.p;
    out->clear();
    for (;;) {
        if (c.p >= c.end) return Fail(c, "an unfinished string");
        const char ch = *c.p++;
        if (ch == '"') return true;
        if (static_cast<unsigned char>(ch) < 0x20) return Fail(c, "a control character in a string");
        if (ch != '\\') { out->push_back(ch); continue; }
        if (c.p >= c.end) return Fail(c, "an unfinished escape");
        const char e = *c.p++;
        switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!Hex4(c, &cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    uint32_t lo = 0;
                    if (c.end - c.p < 6 || c.p[0] != '\\' || c.p[1] != 'u') return Fail(c, "a lone surrogate");
                    c.p += 2;
                    if (!Hex4(c, &lo) || lo < 0xDC00 || lo > 0xDFFF) return Fail(c, "a bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return Fail(c, "a lone surrogate");
                }
                Utf8(*out, cp);
                break;
            }
            default: return Fail(c, "an unknown escape");
        }
    }
}

bool ParseValue(Cursor& c, Value* out, int depth);

bool ParseNumber(Cursor& c, Value* out) {
    const char* start = c.p;
    if (c.p < c.end && *c.p == '-') ++c.p;
    if (c.p >= c.end || !(*c.p >= '0' && *c.p <= '9')) return Fail(c, "a bad number");
    if (*c.p == '0') ++c.p;
    else while (c.p < c.end && *c.p >= '0' && *c.p <= '9') ++c.p;
    if (c.p < c.end && *c.p == '.') {
        ++c.p;
        if (c.p >= c.end || !(*c.p >= '0' && *c.p <= '9')) return Fail(c, "a bad number");
        while (c.p < c.end && *c.p >= '0' && *c.p <= '9') ++c.p;
    }
    if (c.p < c.end && (*c.p == 'e' || *c.p == 'E')) {
        ++c.p;
        if (c.p < c.end && (*c.p == '+' || *c.p == '-')) ++c.p;
        if (c.p >= c.end || !(*c.p >= '0' && *c.p <= '9')) return Fail(c, "a bad number");
        while (c.p < c.end && *c.p >= '0' && *c.p <= '9') ++c.p;
    }
    out->type = Value::Type::Number;
    out->text.assign(start, c.p);
    out->number = std::strtod(out->text.c_str(), nullptr);
    return true;
}

bool Word(Cursor& c, const char* w, Value* out, Value::Type t, bool b) {
    size_t n = 0;
    while (w[n]) ++n;
    if (size_t(c.end - c.p) < n) return Fail(c, "an unknown word");
    for (size_t i = 0; i < n; ++i)
        if (c.p[i] != w[i]) return Fail(c, "an unknown word");
    c.p += n;
    out->type = t;
    out->boolean = b;
    return true;
}

bool ParseValue(Cursor& c, Value* out, int depth) {
    if (depth > 64) return Fail(c, "nested too deeply");
    Space(c);
    if (c.p >= c.end) return Fail(c, "a value was expected");
    switch (*c.p) {
        case '{': {
            out->type = Value::Type::Object;
            ++c.p;
            Space(c);
            if (c.p < c.end && *c.p == '}') { ++c.p; return true; }
            for (;;) {
                Space(c);
                std::string key;
                if (!ParseString(c, &key)) return false;
                for (const auto& m : out->members)
                    if (m.first == key) return Fail(c, "a key twice in one object");
                Space(c);
                if (c.p >= c.end || *c.p != ':') return Fail(c, "':' was expected");
                ++c.p;
                Value v;
                if (!ParseValue(c, &v, depth + 1)) return false;
                out->members.emplace_back(std::move(key), std::move(v));
                Space(c);
                if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
                if (c.p < c.end && *c.p == '}') { ++c.p; return true; }
                return Fail(c, "',' or '}' was expected");
            }
        }
        case '[': {
            out->type = Value::Type::Array;
            ++c.p;
            Space(c);
            if (c.p < c.end && *c.p == ']') { ++c.p; return true; }
            for (;;) {
                Value v;
                if (!ParseValue(c, &v, depth + 1)) return false;
                out->items.push_back(std::move(v));
                Space(c);
                if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
                if (c.p < c.end && *c.p == ']') { ++c.p; return true; }
                return Fail(c, "',' or ']' was expected");
            }
        }
        case '"': out->type = Value::Type::String; return ParseString(c, &out->text);
        case 't': return Word(c, "true", out, Value::Type::Bool, true);
        case 'f': return Word(c, "false", out, Value::Type::Bool, false);
        case 'n': return Word(c, "null", out, Value::Type::Null, false);
        default: return ParseNumber(c, out);
    }
}

void Escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char ch : s) {
        const unsigned char u = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (u < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", unsigned(u));
                    out += b;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void WriteValue(std::string& out, const Value& v, int indent) {
    const std::string pad(size_t(indent) * 2, ' '), inner(size_t(indent + 1) * 2, ' ');
    switch (v.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
        case Value::Type::Number: out += v.text.empty() ? "0" : v.text; break;
        case Value::Type::String: Escape(out, v.text); break;
        case Value::Type::Array:
            if (v.items.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.items.size(); ++i) {
                out += inner;
                WriteValue(out, v.items[i], indent + 1);
                out += i + 1 < v.items.size() ? ",\n" : "\n";
            }
            out += pad + "]";
            break;
        case Value::Type::Object:
            if (v.members.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t i = 0; i < v.members.size(); ++i) {
                out += inner;
                Escape(out, v.members[i].first);
                out += ": ";
                WriteValue(out, v.members[i].second, indent + 1);
                out += i + 1 < v.members.size() ? ",\n" : "\n";
            }
            out += pad + "}";
            break;
    }
}

}  // namespace

Value Value::Str(const std::string& s) {
    Value v;
    v.type = Type::String;
    v.text = s;
    return v;
}
Value Value::Num(uint64_t n) {
    Value v;
    v.type = Type::Number;
    v.number = double(n);
    v.text = std::to_string(n);
    return v;
}
Value Value::Bool(bool b) {
    Value v;
    v.type = Type::Bool;
    v.boolean = b;
    return v;
}
Value Value::Arr() {
    Value v;
    v.type = Type::Array;
    return v;
}
Value Value::Obj() {
    Value v;
    v.type = Type::Object;
    return v;
}

const Value* Value::Get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& m : members)
        if (m.first == key) return &m.second;
    return nullptr;
}

std::string Value::String(const std::string& key, const std::string& fallback) const {
    const Value* v = Get(key);
    return v && v->type == Type::String ? v->text : fallback;
}

uint64_t Value::Unsigned(const std::string& key, uint64_t fallback) const {
    const Value* v = Get(key);
    if (!v || v->type != Type::Number || v->number < 0 || v->number > 9007199254740992.0 || std::floor(v->number) != v->number)
        return fallback;
    // the literal as written (exact past 2^53 would need it; sizes here are below)
    return uint64_t(v->number);
}

Value& Value::Set(const std::string& key, Value v) {
    if (type != Type::Object) {
        type = Type::Object;
        members.clear();
    }
    for (auto& m : members)
        if (m.first == key) { m.second = std::move(v); return m.second; }
    members.emplace_back(key, std::move(v));
    return members.back().second;
}

Value& Value::Push(Value v) {
    if (type != Type::Array) {
        type = Type::Array;
        items.clear();
    }
    items.push_back(std::move(v));
    return items.back();
}

bool Parse(const std::string& text, Value* out, std::string* error) {
    Cursor c{text.data(), text.data() + text.size(), text.data(), {}};
    if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF) c.p += 3;
    Value v;
    if (!ParseValue(c, &v, 0)) {
        if (error) *error = c.error;
        return false;
    }
    Space(c);
    if (c.p != c.end) {
        Fail(c, "text after the value");
        if (error) *error = c.error;
        return false;
    }
    *out = std::move(v);
    return true;
}

std::string Write(const Value& v) {
    std::string out;
    WriteValue(out, v, 0);
    out.push_back('\n');
    return out;
}

}  // namespace te::modimport::json
