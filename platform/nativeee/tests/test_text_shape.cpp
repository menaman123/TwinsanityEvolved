// test_text_shape.cpp -- te_text_shape_test: the text layer's Arabic shaping (platform/nativeee/wasm/text_shape.cpp, compiled
// natively here exactly as it is into the game module).
//
//   * the contextual forms of words by the joining rules (dual-joining letters take four forms, right-joining two; a letter
//     after a right-joining one starts anew), lam + alef as one ligature, tatweel;
//   * display order: a right-to-left line reversed, a number and a Latin word kept left to right inside it, ( ) mirrored,
//     the game's button glyphs ([ ...) left where the words put them, digits drawn as Arabic-Indic digits;
//   * a two-line string: its lines in order, each shaped alone;
//   * the layout: the game's line rules (widths, heights, the empty line), the alignment mirrored;
//   * the reveal: the shown prefix of the logical string lands at the line's RIGHT end and grows leftwards;
//   * a Latin string is not touched (no Arabic: the game's own path draws it);
//   * when TE_SOURCE_DIR names the tree: every line of the shipped bank (mods/arabic/data/arabic.bank) shapes with no glyph
//     missing from the shipped font (mods/arabic/data/arabic.font's codepoint table) or the game font's range.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "te_text_shape.h"

namespace {
int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failures;                                                                 \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                 \
    } while (0)

using te::text::ShapedText;

std::vector<uint32_t> Display(const ShapedText& t, int line) {
    std::vector<uint32_t> out;
    const auto& l = t.lines[line];
    for (int i = l.first; i < l.first + l.count; ++i) out.push_back(t.glyphs[i].codepoint);
    return out;
}

std::string Hex(const std::vector<uint32_t>& v) {
    std::string s;
    char b[16];
    for (uint32_t c : v) {
        std::snprintf(b, sizeof b, "%s%04X", s.empty() ? "" : " ", c);
        s += b;
    }
    return s;
}

bool Expect(const char* what, const char* text, std::vector<uint32_t> want, int line = 0) {
    static ShapedText t;
    te::text::Shape(text, &t);
    const auto got = Display(t, line);
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL %s: display %s, expected %s\n", what, Hex(got).c_str(), Hex(want).c_str());
        return false;
    }
    std::printf("  ok %-28s %s\n", what, Hex(got).c_str());
    return true;
}

// Metrics for the layout tests: every glyph 10 wide, Arabic glyphs 52 high, the game's 39
float Width(void*, uint8_t, uint8_t) { return 10.0f; }
float Height(void*, uint8_t font, uint8_t) { return font == te::text::kFontArabic ? 52.0f : 39.0f; }

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::vector<uint8_t> out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
        std::fclose(f);
    }
    return out;
}

// The codepoint table of our font file (psf.py: the trailer "TEGM", version, count, the codepoints after the glyphs)
std::vector<uint32_t> FontCodepoints(const std::vector<uint8_t>& font) {
    std::vector<uint32_t> out;
    for (size_t at = 0; at + 12 <= font.size(); ++at) {
        if (std::memcmp(&font[at], "TEGM", 4) != 0) continue;
        uint32_t count;
        std::memcpy(&count, &font[at + 8], 4);
        if (at + 12 + size_t(count) * 4 != font.size()) continue;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t cp;
            std::memcpy(&cp, &font[at + 12 + i * 4], 4);
            out.push_back(cp);
        }
        return out;
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace te::text;
    // --dump BANK: every text of a language bank shaped by this code, for tools/judges/arabic_length_judge.py to compare with
    // tools/text/shape.py: one row per text, "<section> <n>\t" then each line's display codepoints (hex), lines joined by
    // " | ", an empty line as "~"
    if (argc == 3 && std::strcmp(argv[1], "--dump") == 0) {
        const std::vector<uint8_t> bank = ReadFile(argv[2]);
        if (bank.empty()) return 2;
        std::string section, line;
        int n = 0;
        static ShapedText t;
        auto dump = [&](const std::string& s) {
            if (s.empty()) return;
            if (s.rfind("@@ ", 0) == 0) {
                section = s.substr(3, s.find(' ', 3) - 3);
                n = 0;
                return;
            }
            Shape(s.c_str(), &t);
            std::string row;
            for (int l = 0; l < t.lineCount; ++l) {
                if (l) row += " | ";
                if (t.lines[l].empty) { row += "~"; continue; }
                row += Hex(Display(t, l));
            }
            std::printf("%s %d\t%s\n", section.c_str(), n++, row.c_str());
        };
        for (uint8_t c : bank) {
            if (c == '\r' || c == '\n') { dump(line); line.clear(); }
            else line.push_back(char(c));
        }
        dump(line);
        return 0;
    }
    (void)argc;
    (void)argv;
    std::printf("te_text_shape_test: forms and display order\n");
    // kaf joins reh; reh never joins the letter after it, so alef and sheen stand alone
    Expect("kaf reh alef sheen", "\xDF\xD1\xC7\xD4", {0xFEB5, 0xFE8D, 0xFEAE, 0xFEDB});
    Expect("cortex", "\xDF\xE6\xD1\xCA\xDF\xD3", {0xFEB2, 0xFEDC, 0xFE97, 0xFEAD, 0xFEEE, 0xFEDB});
    Expect("lam-alef alone", "\xE1\xC7", {0xFEFB});
    Expect("lam-alef joined (salam)", "\xD3\xE1\xC7\xE3", {0xFEE1, 0xFEFC, 0xFEB3});
    Expect("feh + lam-alef", "\xDD\xE1\xC7", {0xFEFC, 0xFED3});
    Expect("tatweel after kaf", "\xDF\xDC", {0x0640, 0xFEDB});
    Expect("digits in an RTL sentence", "\xE3\xD3\xCA\xE6\xEC 12", {0x0661, 0x0662, 0x20, 0xFEEF, 0xFEEE, 0xFE98, 0xFEB4, 0xFEE3});
    Expect("a Latin name inside", "\xE3\xD1\xCD\xC8\xC7" " crash!", {'!', 'c', 'r', 'a', 's', 'h', ' ', 0xFE8E, 0xFE92, 0xFEA3, 0xFEAE, 0xFEE3});
    Expect("a Latin word with a digit", "\xCC\xE5\xC7\xD2 ps2", {'p', 's', '2', ' ', 0xFEAF, 0xFE8E, 0xFEEC, 0xFE9F});
    Expect("a button glyph between words", "\xC7\xD6\xDB\xD8 [ \xE1\xE1\xDE\xDD\xD2",
           {0xFEB0, 0xFED4, 0xFED8, 0xFEE0, 0xFEDF, ' ', '[', ' ', 0xFEC2, 0xFED0, 0xFEBF, 0xFE8D});
    Expect("parentheses mirrored", "(\xE4\xD5)", {'(', 0xFEBA, 0xFEE7, ')'});
    // the language row: "<language> : arabic (text)" -- the bracket pair holds Latin after Latin: one left-to-right run
    Expect("a Latin run with brackets", "\xC7\xE1\xE1\xDB\xC9 : arabic (text)",
           {'a', 'r', 'a', 'b', 'i', 'c', ' ', '(', 't', 'e', 'x', 't', ')', ' ', ':', ' ', 0xFE94, 0xFED0, 0xFEE0, 0xFEDF, 0xFE8D});
    // a bracketed Latin name after Arabic is right to left: its brackets mirrored around the Latin run
    Expect("a bracketed Latin name", "\xED\xCF (dualshock@2)", {'(', 'd', 'u', 'a', 'l', 's', 'h', 'o', 'c', 'k', '@', '2', ')', ' ', 0xFEAA, 0xFEF3});
    Expect("a thousands separator", "\xDA\xCF\xCF 1,000", {0x0661, ',', 0x0660, 0x0660, 0x0660, ' ', 0xFEA9, 0xFEAA, 0xFECB});
    {
        // the parenthesis glyph drawn is its partner's byte (the game font draws the byte)
        static ShapedText t;
        Shape("(\xE4\xD5)", &t);
        CHECK(t.glyphs[0].byte == '(' && t.glyphs[0].source == 3);
        CHECK(t.glyphs[3].byte == ')' && t.glyphs[3].source == 0);
    }

    std::printf("te_text_shape_test: two lines\n");
    {
        static ShapedText t;
        Shape("\xD3\xD8\xD1~\xCB\xC7\xE4\xED", &t);
        CHECK(t.rtl);
        CHECK(t.lineCount == 2);
        Expect("line 0 (top) is the first", "\xD3\xD8\xD1~\xCB\xC7\xE4\xED", {0xFEAE, 0xFEC4, 0xFEB3}, 0);
        Expect("line 1 shaped alone", "\xD3\xD8\xD1~\xCB\xC7\xE4\xED", {0xFEF2, 0xFEE7, 0xFE8E, 0xFE9B}, 1);
        CHECK(t.lines[0].sourceStart == 0 && t.lines[0].sourceEnd == 3);
        CHECK(t.lines[1].sourceStart == 4 && t.lines[1].sourceEnd == 8);
    }

    std::printf("te_text_shape_test: resolve and layout\n");
    {
        // a font table holding the forms of the words below; the game font from 0x20, 221 glyphs (the disc font's range)
        const uint32_t table[] = {0xFEB5, 0xFE8D, 0xFEAE, 0xFEDB, 0xFEB3, 0xFEC4, 0xFEF2, 0xFEE7, 0xFE8E, 0xFE9B};
        FontTables fonts{table, int(sizeof table / sizeof *table), 0x20, 221};
        static ShapedText t;
        Shape("\xD3\xD8\xD1~~\xCB\xC7\xE4\xED [", &t);
        Resolve(&t, fonts);
        CHECK(t.missing == 0);
        CHECK(t.lineCount == 3);   // a line, the empty one, a line
        CHECK(t.lines[1].empty);
        CHECK(t.glyphs[0].font == kFontArabic && t.glyphs[0].index == 2);    // reh final
        Metrics m{Width, Height, nullptr, 39.0f};
        static LaidText laid;
        Layout(t, m, &laid);
        CHECK(laid.lineCount == 2);
        CHECK(laid.lines[0].width == 30.0f);
        CHECK(laid.lines[0].height == 52.0f + 39.0f);   // the empty line adds the space's height to the line before
        CHECK(laid.lines[1].width == 60.0f);            // "[ " + four letters: the button glyph and the space are the game's
        CHECK(laid.height == 52.0f + 39.0f + 52.0f);
        CHECK(laid.widest == 60.0f);
        // the alignment mirrored: start-aligned (Left) lines end at the right of the box [anchor, anchor + widest]
        CHECK(LineStart(0x11, true, 30.0f, 60.0f) == 30.0f);
        CHECK(LineStart(0x11, true, 60.0f, 60.0f) == 0.0f);
        CHECK(LineStart(0x41, true, 30.0f, 60.0f) == -60.0f);   // end-aligned (Right): lines start at the box's left
        CHECK(LineStart(0x21, true, 30.0f, 60.0f) == -15.0f);   // centred stays centred
        CHECK(LineStart(0x11, false, 30.0f, 60.0f) == 0.0f);    // the game's own rule for left-to-right text
        CHECK(LineStart(0x41, false, 30.0f, 60.0f) == -30.0f);
        // a codepoint the Arabic font lacks is missing (never drawn with the game font's glyph of its byte)
        static ShapedText u;
        Shape("\xC8", &u);   // beh: not in this table
        Resolve(&u, fonts);
        CHECK(u.missing == 1);
        CHECK(u.glyphs[0].index == kNoGlyph);
    }

    std::printf("te_text_shape_test: the reveal\n");
    {
        // kaf reh alef sheen: display [sheen alef reh kaf], each 10 wide: the logical prefix of k letters is the k glyphs at
        // the RIGHT end, so the shown text grows leftwards from the line's right edge
        static ShapedText t;
        Shape("\xDF\xD1\xC7\xD4", &t);
        for (int shown = 0; shown <= 4; ++shown) {
            float left = 1e9f, right = -1e9f;
            int drawn = 0;
            for (int i = 0; i < t.count; ++i) {
                if (!Shown(t.glyphs[i], shown)) continue;
                ++drawn;
                const float x = 10.0f * float(i);
                if (x < left) left = x;
                if (x + 10.0f > right) right = x + 10.0f;
            }
            CHECK(drawn == shown);
            if (shown > 0) {
                CHECK(right == 40.0f);                       // anchored at the right end
                CHECK(left == 40.0f - 10.0f * float(shown));  // one glyph further left per letter
                std::printf("  ok reveal %d: x %.0f..%.0f\n", shown, left, right);
            }
        }
        // the forms are the whole word's: with one letter shown, the kaf is already its initial form
        CHECK(t.glyphs[3].codepoint == 0xFEDB);
    }

    std::printf("te_text_shape_test: Latin untouched\n");
    {
        CHECK(!HasArabic("new game"));
        CHECK(!HasArabic("caf\xE9 \xA6 \xAC"));   // Latin-1 accents and the game's button symbols are no Arabic letters
        CHECK(HasArabic("\xC7"));
        static ShapedText t;
        Shape("new game~(x)", &t);
        CHECK(!t.rtl);
        CHECK(Hex(Display(t, 0)) == "006E 0065 0077 0020 0067 0061 006D 0065");
        CHECK(Hex(Display(t, 1)) == "0028 0078 0029");
    }

    // The shipped bank against the shipped font
    if (const char* root = std::getenv("TE_SOURCE_DIR")) {
        const std::vector<uint8_t> bank = ReadFile(std::string(root) + "/mods/arabic/data/arabic.bank");
        const std::vector<uint8_t> font = ReadFile(std::string(root) + "/mods/arabic/data/arabic.font");
        const std::vector<uint32_t> cps = FontCodepoints(font);
        if (bank.empty() || cps.empty()) {
            std::printf("te_text_shape_test: no shipped bank/font under %s (skipped)\n", root);
        } else {
            FontTables fonts{cps.data(), int(cps.size()), 0x20, 221};
            int lines = 0, arabic = 0, missing = 0;
            std::string line;
            static ShapedText t;
            auto check = [&](const std::string& s) {
                if (s.empty() || s.rfind("@@ ", 0) == 0) return;   // a section header (te_language_mod.h) is no text
                ++lines;
                if (!HasArabic(s.c_str())) return;
                ++arabic;
                Shape(s.c_str(), &t);
                Resolve(&t, fonts);
                if (t.missing) {
                    ++missing;
                    std::fprintf(stderr, "  FAIL bank line %d: %d glyph(s) missing\n", lines - 1, t.missing);
                }
            };
            for (uint8_t c : bank) {
                if (c == '\r' || c == '\n') { check(line); line.clear(); }
                else line.push_back(char(c));
            }
            check(line);
            ++g_checks;
            if (missing) ++g_failures;
            std::printf("te_text_shape_test: shipped bank: %d lines, %d Arabic, %d with a missing glyph (font: %zu glyphs)\n", lines,
                        arabic, missing, cps.size());
        }
    }

    std::printf("te_text_shape_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
