// platform/nativeee/te_text_shape.h -- the text layer's ARABIC: a string of the game's 8-bit text (a language mod's bank is
// Windows-1256) shaped and ordered for drawing, then laid out by the game's own line rules.
//
// Compiled into the game module with the text layer (platform/nativeee/wasm/text_shape.cpp: plain C++, no allocation, no
// state) and natively for its unit test (platform/nativeee/tests/test_text_shape.cpp), so every platform draws the same.
//
//   Shape    the string's lines (split at '~' as the game's RenderText does), each line's logical codepoints given their
//            contextual forms (isolated / initial / medial / final by joining class; lam + alef as one ligature), then put
//            in display order: a right-to-left line reversed with its Latin and digit runs kept left to right, ( ) mirrored
//            in right-to-left runs, ASCII digits drawn as Arabic-Indic digits in a right-to-left paragraph.
//   Resolve  each shaped codepoint's glyph: the Arabic font's (its codepoint table: our font file's trailer) or the game font's
//            (the byte's own glyph, the game's GlyphIndex); what neither has is counted as missing.
//   Layout   the game's RenderText over the shaped glyphs: a line's width the sum of its glyphs' widths, its height the tallest
//            glyph's (at least the space's), an empty line ("~~") adding the space's height to the line before; and each
//            line's start by the item's alignment MIRRORED for a right-to-left text (start-aligned = right-aligned within the
//            text's box, centred stays centred).
//   Reveal   `shown` logical bytes of the string drawn (the rest laid out but hidden): the shown prefix of a right-to-left
//            line sits at its RIGHT end and grows leftwards; the forms are the whole string's, so a letter never changes
//            shape as more appear.
#pragma once
#include <stdint.h>

namespace te::text {

constexpr int kMaxGlyphs = 0x800;   // TextLayout::glyphs' capacity
constexpr int kMaxLines = 64;       // TextLayout::lines'
enum : uint8_t { kFontGame = 0, kFontArabic = 1 };
constexpr uint8_t kNoGlyph = 0xFF;

// The byte's codepoint in Windows-1256 (ASCII below 0x80)
uint32_t Cp1256(uint8_t byte);
// Is the codepoint an Arabic letter (U+0621..U+064A) or tatweel
bool IsArabicLetter(uint32_t cp);
// Does the string hold an Arabic letter (a byte that is one in Windows-1256): the strings the shaped path draws
bool HasArabic(const char* text);

struct ShapedGlyph {
    uint32_t codepoint;   // what is drawn: a presentation form, an Arabic-Indic digit or Arabic punctuation (Arabic font), or the
                          // codepoint of the string's byte (game font)
    uint16_t source;      // the logical byte (index into the string) it shows; a lam-alef ligature: its lam's
    uint8_t byte;         // the byte the game font draws it with (the string's own; a mirrored bracket: its partner's)
    uint8_t font;         // kFontGame / kFontArabic
    uint8_t index;        // its glyph in that font (Resolve), kNoGlyph when neither font has it
};

struct ShapedLine {
    int first, count;     // its glyphs in DISPLAY order (left to right) in ShapedText::glyphs
    int sourceStart, sourceEnd;   // its logical bytes [start, end) in the string
    bool empty;           // a '~' with nothing on its line: no glyphs, the space's height added to the line before
};

struct ShapedText {
    ShapedGlyph glyphs[kMaxGlyphs];
    int count;
    ShapedLine lines[kMaxLines];
    int lineCount;        // including the empty ones, in order
    bool rtl;             // the paragraph's direction: right to left when the string holds an Arabic letter
    int missing;          // glyphs neither font has (Resolve)
};

void Shape(const char* text, ShapedText* out);

struct FontTables {
    const uint32_t* arabicCodepoints;   // the Arabic font's glyph i draws arabicCodepoints[i]
    int arabicCount;
    int gameFirst;                      // the game font's firstCharacter and glyphCount (GlyphIndex)
    int gameCount;
};
void Resolve(ShapedText* text, const FontTables& fonts);

// A glyph's width and height in the game's glyph units (sixteenths of a texel: the glyph quadword's z and w)
struct Metrics {
    float (*width)(void* context, uint8_t font, uint8_t index);
    float (*height)(void* context, uint8_t font, uint8_t index);
    void* context;
    float spaceHeight;   // the game font's space's height (the least a line is)
};

struct LaidLine {
    float width, height;   // unscaled (glyph units): RenderText's line extent before it multiplies by the scale
    int first, count;      // its glyphs (display order) in ShapedText::glyphs
};

struct LaidText {
    LaidLine lines[kMaxLines];   // the lines with glyphs (RenderText's lineCount)
    int lineCount;
    float height;                // the text's height, unscaled
    float widest;                // the widest line, unscaled
};
void Layout(const ShapedText& text, const Metrics& metrics, LaidText* out);

// Where a line starts (its left edge) relative to the item's anchor, scaled widths in: the game's rule (left: at the anchor;
// right: ending at it; neither: centred on it), mirrored for a right-to-left text inside the box the widest line spans.
// alignment = TextAlignment bits (Left 0x10, Right 0x40).
float LineStart(uint32_t alignment, bool rtl, float lineWidth, float widest);

// Is a glyph drawn when only the first `shown` logical bytes of the string are (negative: all)
inline bool Shown(const ShapedGlyph& glyph, int shown) { return shown < 0 || glyph.source < shown; }

// The census of one shaped text (TE_GFX_DIAG_TEXT): the text layer fills it in guest memory and the host prints it
// (ps2e_text_shaped). Positions in the GS's sixteenths, like the glyph quads. Fixed-width fields only: one layout on every side.
constexpr int kDiagGlyphs = 512;
struct DiagGlyph {
    uint32_t codepoint;
    uint16_t source;
    uint8_t font, index;
    float x0, x1;
};
struct DiagLine {
    float x0, y0, x1, y1;
    int32_t first, count;
};
struct Diag {
    uint32_t text;                  // the item's string (guest address)
    float anchorX, anchorY;         // the item's place
    uint32_t alignment;
    int32_t rtl, missing, lineCount, glyphCount;
    float boxX0, boxY0, boxX1, boxY1;   // the text's box: the widest line's span at the anchor, the lines' heights
    DiagLine lines[kMaxLines];
    DiagGlyph glyphs[kDiagGlyphs];
};
static_assert(sizeof(DiagGlyph) == 16 && sizeof(DiagLine) == 24, "the census record has one layout on every side");

}  // namespace te::text
