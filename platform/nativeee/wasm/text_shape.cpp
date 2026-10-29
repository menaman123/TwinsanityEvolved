// platform/nativeee/wasm/text_shape.cpp -- see te_text_shape.h. Plain C++ (compiled into the game module and natively for
// te_text_shape_test): no allocation, no state, the tables below.
#include "te_text_shape.h"

namespace te::text {

namespace {

// Windows-1256's upper half (0x80..0xFF), as Unicode
constexpr uint16_t kCp1256[128] = {
    0x20AC, 0x067E, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0679, 0x2039, 0x0152, 0x0686, 0x0698, 0x0688,
    0x06AF, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x06A9, 0x2122, 0x0691, 0x203A, 0x0153, 0x200C, 0x200D, 0x06BA,
    0x00A0, 0x060C, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x06BE, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x061B, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x061F,
    0x06C1, 0x0621, 0x0622, 0x0623, 0x0624, 0x0625, 0x0626, 0x0627, 0x0628, 0x0629, 0x062A, 0x062B, 0x062C, 0x062D, 0x062E, 0x062F,
    0x0630, 0x0631, 0x0632, 0x0633, 0x0634, 0x0635, 0x0636, 0x00D7, 0x0637, 0x0638, 0x0639, 0x063A, 0x0640, 0x0641, 0x0642, 0x0643,
    0x00E0, 0x0644, 0x00E2, 0x0645, 0x0646, 0x0647, 0x0648, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x0649, 0x064A, 0x00EE, 0x00EF,
    0x064B, 0x064C, 0x064D, 0x064E, 0x00F4, 0x064F, 0x0650, 0x00F7, 0x0651, 0x00F9, 0x0652, 0x00FB, 0x00FC, 0x200E, 0x200F, 0x06D2,
};

// U+0621..U+064A: the joining class and the isolated presentation form (Unicode's Arabic Presentation Forms-B). A dual-joining
// letter's forms are iso, fin, ini, med = base .. base + 3; a right-joining letter's iso, fin = base, base + 1. ALEF MAKSURA is
// right-joining here (in Arabic it only ends a word). 0 = not a letter this layer shapes.
enum Joining : uint8_t { None = 0, NonJoining = 1, Right = 2, Dual = 3 };
struct Letter {
    uint8_t joining;
    uint16_t base;
};
constexpr Letter kLetters[0x2A] = {
    {NonJoining, 0xFE80},  // 0621 hamza
    {Right, 0xFE81},       // 0622 alef with madda above
    {Right, 0xFE83},       // 0623 alef with hamza above
    {Right, 0xFE85},       // 0624 waw with hamza above
    {Right, 0xFE87},       // 0625 alef with hamza below
    {Dual, 0xFE89},        // 0626 yeh with hamza above
    {Right, 0xFE8D},       // 0627 alef
    {Dual, 0xFE8F},        // 0628 beh
    {Right, 0xFE93},       // 0629 teh marbuta
    {Dual, 0xFE95},        // 062A teh
    {Dual, 0xFE99},        // 062B theh
    {Dual, 0xFE9D},        // 062C jeem
    {Dual, 0xFEA1},        // 062D hah
    {Dual, 0xFEA5},        // 062E khah
    {Right, 0xFEA9},       // 062F dal
    {Right, 0xFEAB},       // 0630 thal
    {Right, 0xFEAD},       // 0631 reh
    {Right, 0xFEAF},       // 0632 zain
    {Dual, 0xFEB1},        // 0633 seen
    {Dual, 0xFEB5},        // 0634 sheen
    {Dual, 0xFEB9},        // 0635 sad
    {Dual, 0xFEBD},        // 0636 dad
    {Dual, 0xFEC1},        // 0637 tah
    {Dual, 0xFEC5},        // 0638 zah
    {Dual, 0xFEC9},        // 0639 ain
    {Dual, 0xFECD},        // 063A ghain
    {None, 0},             // 063B
    {None, 0},             // 063C
    {None, 0},             // 063D
    {None, 0},             // 063E
    {None, 0},             // 063F
    {Dual, 0x0640},        // 0640 tatweel (join-causing: drawn as itself)
    {Dual, 0xFED1},        // 0641 feh
    {Dual, 0xFED5},        // 0642 qaf
    {Dual, 0xFED9},        // 0643 kaf
    {Dual, 0xFEDD},        // 0644 lam
    {Dual, 0xFEE1},        // 0645 meem
    {Dual, 0xFEE5},        // 0646 noon
    {Dual, 0xFEE9},        // 0647 heh
    {Right, 0xFEED},       // 0648 waw
    {Right, 0xFEEF},       // 0649 alef maksura
    {Dual, 0xFEF1},        // 064A yeh
};
constexpr uint32_t kTatweel = 0x0640;
constexpr uint32_t kLam = 0x0644;

const Letter* LetterOf(uint32_t cp) {
    if (cp < 0x0621 || cp > 0x064A) return nullptr;
    const Letter* l = &kLetters[cp - 0x0621];
    return l->joining == None ? nullptr : l;
}

bool JoinsNext(uint32_t cp) {   // connects to the letter after it (on its left)
    const Letter* l = LetterOf(cp);
    return l && l->joining == Dual;
}

bool JoinsPrevious(uint32_t cp) {   // connects to the letter before it (on its right)
    const Letter* l = LetterOf(cp);
    return l && (l->joining == Dual || l->joining == Right);
}

// lam + this alef -> (isolated, final) ligature; 0 when it is no alef
void LamAlef(uint32_t alef, uint32_t* iso, uint32_t* fin) {
    switch (alef) {
        case 0x0622: *iso = 0xFEF5; *fin = 0xFEF6; return;
        case 0x0623: *iso = 0xFEF7; *fin = 0xFEF8; return;
        case 0x0625: *iso = 0xFEF9; *fin = 0xFEFA; return;
        case 0x0627: *iso = 0xFEFB; *fin = 0xFEFC; return;
        default: *iso = *fin = 0; return;
    }
}

// The bidi classes this layer knows (a subset of Unicode's): strong right to left, strong left to right, a digit, and the
// rest neutral -- the game's button glyphs ([ ] \ ^ { } < > and the two Latin-1 symbols) included, so an icon sits where the
// words around it put it
enum Class : uint8_t { ClassN = 0, ClassR = 1, ClassL = 2, ClassEN = 3, ClassCS = 4, ClassET = 5 };

Class ClassOf(uint32_t cp) {
    if (cp >= 0x0621 && cp <= 0x064A) return ClassR;
    if (cp >= 0xFE70 && cp <= 0xFEFC) return ClassR;
    if (cp == 0x060C || cp == 0x061B || cp == 0x061F) return ClassR;
    if ((cp >= '0' && cp <= '9') || (cp >= 0x0660 && cp <= 0x0669)) return ClassEN;
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return ClassL;
    if (cp >= 0x00C0 && cp <= 0x024F && cp != 0x00D7 && cp != 0x00F7) return ClassL;
    if (cp == '.' || cp == ',' || cp == ':' || cp == '/') return ClassCS;
    if (cp == '%') return ClassET;
    return ClassN;
}

struct Unit {
    uint32_t cp;
    uint16_t source;
    uint8_t byte;
    uint8_t cls;
    uint8_t level;
};

constexpr int kMaxLine = 512;

void ShapeLine(const char* text, int start, int end, bool rtl, ShapedText* out) {
    if (out->lineCount >= kMaxLines) return;
    ShapedLine line{};
    line.first = out->count;
    line.sourceStart = start;
    line.sourceEnd = end;
    Unit units[kMaxLine];
    int n = 0;
    // 1 the logical codepoints given their forms (the joining looks at the letters of this line only)
    for (int i = start; i < end && n < kMaxLine; ++i) {
        const uint32_t cp = Cp1256(uint8_t(text[i]));
        const Letter* l = LetterOf(cp);
        Unit u{cp, uint16_t(i), uint8_t(text[i]), 0, 0};
        if (l) {
            const uint32_t prev = i > start ? Cp1256(uint8_t(text[i - 1])) : 0;
            const uint32_t next = i + 1 < end ? Cp1256(uint8_t(text[i + 1])) : 0;
            const bool joinPrev = prev && JoinsNext(prev) && JoinsPrevious(cp);
            uint32_t iso = 0, fin = 0;
            if (cp == kLam && next) LamAlef(next, &iso, &fin);
            if (iso) {
                u.cp = joinPrev ? fin : iso;   // the alef is the ligature's: it ends the joining (an alef joins no letter after it)
                ++i;
            } else if (cp == kTatweel) {
                u.cp = kTatweel;
            } else {
                const bool joinNext = next && JoinsNext(cp) && JoinsPrevious(next);
                uint32_t form = 0;
                if (l->joining == Dual) form = joinPrev && joinNext ? 3 : joinNext ? 2 : joinPrev ? 1 : 0;
                else if (l->joining == Right) form = joinPrev ? 1 : 0;
                u.cp = l->base + form;
            }
        }
        u.cls = ClassOf(u.cp);
        units[n++] = u;
    }
    if (rtl) {
        // 2 weak types: a separator between two digits and a percent sign next to one are the number's (W4, W5); what is left
        // of them is neutral (W6); a digit after Latin with no Arabic between is the Latin word's ("ps2", W7)
        for (int i = 0; i < n; ++i) {
            const uint8_t c = units[i].cls;
            if (c == ClassCS && i > 0 && i + 1 < n && units[i - 1].cls == ClassEN && units[i + 1].cls == ClassEN)
                units[i].cls = ClassEN;
            else if (c == ClassET && ((i > 0 && units[i - 1].cls == ClassEN) || (i + 1 < n && units[i + 1].cls == ClassEN)))
                units[i].cls = ClassEN;
        }
        for (int i = 0; i < n; ++i)
            if (units[i].cls == ClassCS || units[i].cls == ClassET) units[i].cls = ClassN;
        uint8_t strong = ClassR;
        for (int i = 0; i < n; ++i) {
            if (units[i].cls == ClassL || units[i].cls == ClassR) strong = units[i].cls;
            else if (units[i].cls == ClassEN && strong == ClassL) units[i].cls = ClassL;
        }
        // the digits still a number's draw as Arabic-Indic digits
        for (int i = 0; i < n; ++i)
            if (units[i].cls == ClassEN && units[i].cp >= '0' && units[i].cp <= '9') units[i].cp = 0x0660 + (units[i].cp - '0');
        // 3a paired brackets (N0, for ( ) -- the only brackets the game's font has that are not button glyphs): a pair holding
        // right-to-left text (a number counts) is right to left; one holding only left-to-right text takes the direction
        // before its opening bracket ("arabic (text)" stays one Latin run, "(dualshock@2)" after Arabic is right to left)
        {
            int open[64];
            int depth = 0;
            for (int i = 0; i < n; ++i) {
                if (units[i].cp == '(' && depth < 64) {
                    open[depth++] = i;
                } else if (units[i].cp == ')' && depth > 0) {
                    const int o = open[--depth];
                    bool hasR = false, hasL = false;
                    for (int k = o + 1; k < i; ++k) {
                        if (units[k].cls == ClassR || units[k].cls == ClassEN) hasR = true;
                        else if (units[k].cls == ClassL) hasL = true;
                    }
                    uint8_t dir = ClassN;
                    if (hasR) {
                        dir = ClassR;
                    } else if (hasL) {
                        dir = ClassR;
                        for (int k = o - 1; k >= 0; --k) {
                            if (units[k].cls == ClassL) { dir = ClassL; break; }
                            if (units[k].cls == ClassR || units[k].cls == ClassEN) break;
                        }
                    }
                    if (dir != ClassN) units[o].cls = units[i].cls = dir;
                }
            }
        }
        // 3 neutrals: a run between two strong types of one direction takes it (a number counts as right to left, N1), else
        // the paragraph's; the line's start and end are right to left
        for (int i = 0; i < n;) {
            if (units[i].cls != ClassN) { ++i; continue; }
            int j = i;
            while (j < n && units[j].cls == ClassN) ++j;
            const uint8_t before = i > 0 && units[i - 1].cls == ClassL ? ClassL : ClassR;
            const uint8_t after = j < n && units[j].cls == ClassL ? ClassL : ClassR;
            const uint8_t resolved = before == after ? before : uint8_t(ClassR);
            for (int k = i; k < j; ++k) units[k].cls = resolved;
            i = j;
        }
        // 4 levels: right to left 1, left to right and numbers 2; the line's trailing spaces the paragraph's (L1)
        for (int i = 0; i < n; ++i) units[i].level = units[i].cls == ClassR ? 1 : 2;
        for (int i = n - 1; i >= 0 && units[i].cp == ' '; --i) units[i].level = 1;
        // 5 mirrored in right-to-left runs: ( ) only (the game's [ ] { } < > are button glyphs, not brackets)
        for (int i = 0; i < n; ++i) {
            if (units[i].level != 1) continue;
            if (units[i].cp == '(') { units[i].cp = ')'; units[i].byte = ')'; }
            else if (units[i].cp == ')') { units[i].cp = '('; units[i].byte = '('; }
        }
    }
    // 6 display order, left to right: a right-to-left line from its end, each level-2 run in its own order (L2)
    auto emit = [&](const Unit& u) {
        if (out->count >= kMaxGlyphs) return;
        ShapedGlyph& g = out->glyphs[out->count++];
        g.codepoint = u.cp;
        g.source = u.source;
        g.byte = u.byte;
        g.font = kFontGame;
        g.index = kNoGlyph;
    };
    if (!rtl) {
        for (int i = 0; i < n; ++i) emit(units[i]);
    } else {
        for (int i = n - 1; i >= 0;) {
            if (units[i].level == 2) {
                int s = i;
                while (s > 0 && units[s - 1].level == 2) --s;
                for (int m = s; m <= i; ++m) emit(units[m]);
                i = s - 1;
            } else {
                emit(units[i]);
                --i;
            }
        }
    }
    line.count = out->count - line.first;
    out->lines[out->lineCount++] = line;
}

}  // namespace

uint32_t Cp1256(uint8_t byte) { return byte < 0x80 ? byte : kCp1256[byte - 0x80]; }

bool IsArabicLetter(uint32_t cp) { return LetterOf(cp) != nullptr; }

bool HasArabic(const char* text) {
    if (!text) return false;
    for (const char* p = text; *p; ++p)
        if (uint8_t(*p) >= 0x80 && IsArabicLetter(Cp1256(uint8_t(*p)))) return true;
    return false;
}

void Shape(const char* text, ShapedText* out) {
    out->count = 0;
    out->lineCount = 0;
    out->missing = 0;
    out->rtl = HasArabic(text);
    if (!text) return;
    int pos = 0;
    // RenderText's walk: a '~' where a line would start is an empty line; a line runs to the next '~' (eaten) or the end
    while (text[pos] != '\0') {
        if (text[pos] == '~') {
            if (out->lineCount < kMaxLines) {
                ShapedLine empty{};
                empty.first = out->count;
                empty.count = 0;
                empty.sourceStart = empty.sourceEnd = pos;
                empty.empty = true;
                out->lines[out->lineCount++] = empty;
            }
            ++pos;
            continue;
        }
        const int start = pos;
        while (text[pos] != '\0' && text[pos] != '~') ++pos;
        ShapeLine(text, start, pos, out->rtl, out);
        if (text[pos] == '~') ++pos;
    }
}

void Resolve(ShapedText* text, const FontTables& fonts) {
    text->missing = 0;
    for (int i = 0; i < text->count; ++i) {
        ShapedGlyph& g = text->glyphs[i];
        g.font = kFontGame;
        g.index = kNoGlyph;
        const uint32_t cp = g.codepoint;
        for (int k = 0; k < fonts.arabicCount && k < 256; ++k) {
            if (fonts.arabicCodepoints[k] == cp) {
                g.font = kFontArabic;
                g.index = uint8_t(k);
                break;
            }
        }
        if (g.font == kFontArabic) continue;
        // an Arabic codepoint the Arabic font does not carry draws nothing (never the game font's glyph of its byte)
        if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0xFE70 && cp <= 0xFEFF)) {
            ++text->missing;
            continue;
        }
        // the game font's glyph of the byte (its GlyphIndex)
        const int index = int(g.byte) - fonts.gameFirst;
        if (index < 0 || index >= fonts.gameCount) {
            ++text->missing;
            continue;
        }
        g.index = uint8_t(index);
    }
}

void Layout(const ShapedText& text, const Metrics& metrics, LaidText* out) {
    out->lineCount = 0;
    out->height = 0.0f;
    out->widest = 0.0f;
    for (int l = 0; l < text.lineCount; ++l) {
        const ShapedLine& line = text.lines[l];
        if (line.empty) {
            const int last = out->lineCount != 0 ? out->lineCount - 1 : 0;
            out->lines[last].height = out->lines[last].height + metrics.spaceHeight;
            out->height = out->height + metrics.spaceHeight;
            continue;
        }
        float width = 0.0f;
        float height = metrics.spaceHeight;
        for (int i = line.first; i < line.first + line.count; ++i) {
            const ShapedGlyph& g = text.glyphs[i];
            if (g.index == kNoGlyph) continue;
            const float h = metrics.height(metrics.context, g.font, g.index);
            if (height < h) height = h;
            width = width + metrics.width(metrics.context, g.font, g.index);
        }
        out->height = out->height + height;
        if (line.count > 0 && out->lineCount < kMaxLines) {
            LaidLine& laid = out->lines[out->lineCount++];
            laid.width = width;
            laid.height = height;
            laid.first = line.first;
            laid.count = line.count;
            if (out->widest < width) out->widest = width;
        }
    }
}

float LineStart(uint32_t alignment, bool rtl, float lineWidth, float widest) {
    const bool left = (alignment & 0x10) != 0;
    const bool right = (alignment & 0x40) != 0;
    if (!rtl) return left ? 0.0f : right ? -lineWidth : -lineWidth * 0.5f;
    // mirrored: a start-aligned (left) text's lines end at the right of the box it spans from the anchor; an end-aligned
    // (right) text's start at the box's left; centred stays centred
    if (left) return widest - lineWidth;
    if (right) return -widest;
    return -lineWidth * 0.5f;
}

}  // namespace te::text
