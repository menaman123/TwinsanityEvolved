// mods_json.cpp -- see mods_json.h.
#include "mods_json.h"

#include <cmath>
#include <cstdlib>

namespace te::json {

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool Document(Value& out, std::string* error) {
        if (s_.size() >= 3 && uint8_t(s_[0]) == 0xEF && uint8_t(s_[1]) == 0xBB && uint8_t(s_[2]) == 0xBF) pos_ = 3;
        Space();
        if (!ParseValue(out, 0)) return Fail(error);
        Space();
        if (pos_ != s_.size()) { why_ = "text after the value"; return Fail(error); }
        return true;
    }

private:
    bool Fail(std::string* error) {
        if (error) {
            size_t line = 1, col = 1;
            for (size_t i = 0; i < pos_ && i < s_.size(); ++i) {
                if (s_[i] == '\n') { ++line; col = 1; } else { ++col; }
            }
            *error = "line " + std::to_string(line) + " col " + std::to_string(col) + ": " + (why_.empty() ? "malformed" : why_);
        }
        return false;
    }
    bool Bad(const char* why) { if (why_.empty()) why_ = why; return false; }

    void Space() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool Literal(const char* word) {
        size_t i = 0;
        for (; word[i]; ++i)
            if (pos_ + i >= s_.size() || s_[pos_ + i] != word[i]) return Bad("unknown literal");
        pos_ += i;
        return true;
    }

    bool ParseValue(Value& v, int depth) {
        if (depth > kMaxDepth) return Bad("nested too deeply");
        if (pos_ >= s_.size()) return Bad("a value expected");
        const char c = s_[pos_];
        if (c == '{') return ParseObject(v, depth);
        if (c == '[') return ParseArray(v, depth);
        if (c == '"') { v.type = Value::Type::String; return ParseString(v.text); }
        if (c == 't') { v.type = Value::Type::Bool; v.boolean = true; return Literal("true"); }
        if (c == 'f') { v.type = Value::Type::Bool; v.boolean = false; return Literal("false"); }
        if (c == 'n') { v.type = Value::Type::Null; return Literal("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(v);
        return Bad("a value expected");
    }

    bool ParseObject(Value& v, int depth) {
        v.type = Value::Type::Object;
        ++pos_;
        Space();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            Space();
            if (pos_ >= s_.size() || s_[pos_] != '"') return Bad("a member name (a string) expected");
            std::string key;
            if (!ParseString(key)) return false;
            for (const auto& m : v.members)
                if (m.first == key) return Bad("a member named twice");
            Space();
            if (pos_ >= s_.size() || s_[pos_] != ':') return Bad("':' expected");
            ++pos_;
            Space();
            v.members.emplace_back(key, Value{});
            if (!ParseValue(v.members.back().second, depth + 1)) return false;
            Space();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            return Bad("',' or '}' expected");
        }
    }

    bool ParseArray(Value& v, int depth) {
        v.type = Value::Type::Array;
        ++pos_;
        Space();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            Space();
            v.items.emplace_back();
            if (!ParseValue(v.items.back(), depth + 1)) return false;
            Space();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            return Bad("',' or ']' expected");
        }
    }

    bool Hex4(uint32_t& out) {
        if (pos_ + 4 > s_.size()) return Bad("\\u needs four hex digits");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_ + size_t(i)];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') out |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= uint32_t(c - 'A' + 10);
            else return Bad("\\u needs four hex digits");
        }
        pos_ += 4;
        return true;
    }

    static void Utf8(uint32_t cp, std::string& out) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseString(std::string& out) {
        out.clear();
        ++pos_;   // the opening quote
        while (pos_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') { ++pos_; return true; }
            if (c < 0x20) return Bad("a control character inside a string");
            if (c != '\\') { out.push_back(char(c)); ++pos_; continue; }
            ++pos_;
            if (pos_ >= s_.size()) break;
            const char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // a high surrogate: its low half must follow as \uDC00..\uDFFF
                        uint32_t lo = 0;
                        if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') return Bad("a lone surrogate");
                        pos_ += 2;
                        if (!Hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return Bad("a lone surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return Bad("a lone surrogate");
                    }
                    Utf8(cp, out);
                    break;
                }
                default: return Bad("an unknown escape");
            }
        }
        return Bad("an unterminated string");
    }

    bool ParseNumber(Value& v) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size()) return Bad("a digit expected");
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            return Bad("a digit expected");
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return Bad("a digit expected after '.'");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return Bad("a digit expected in the exponent");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        v.type = Value::Type::Number;
        v.text = s_.substr(start, pos_ - start);
        v.number = std::strtod(v.text.c_str(), nullptr);   // the grammar above is a subset of strtod's ("C" digits only)
        if (!std::isfinite(v.number)) return Bad("a number out of range");
        return true;
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::string why_;
};

}  // namespace

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
    if (!v || v->type != Type::Number || v->number < 0 || v->number > 9007199254740992.0) return fallback;
    const double whole = std::floor(v->number);
    return whole == v->number ? uint64_t(whole) : fallback;
}

bool Parse(const std::string& text, Value& out, std::string* error) {
    out = Value{};
    Parser p(text);
    return p.Document(out, error);
}

}  // namespace te::json
