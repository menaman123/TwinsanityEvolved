// platform/nativeee/host -- language_mod.cpp: see te_language_mod.h. The host side of a language mod: its row in the language
// table, its text files for the game (Platform::System::LanguageTextSize / LanguageText), its font and direction for the text
// layer (ps2e_text_*: platform/nativeee/wasm/graphics_text.cpp), and the shaped-text census line (TE_GFX_DIAG_TEXT).
#include "te_language_mod.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/cdvd/iso_fs.h"
#include "engine/core/hash/sha256.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_host.h"
#include "te_mods.h"
#include "te_services.h"
#include "te_text_shape.h"

extern "C" uint64_t te_graphics_presents();   // host/graphics.cpp: frames presented so far

namespace te::language::mod {

namespace {

const char* const kFiles[2] = {"Code", "AgentLab"};

struct Loaded {
    std::string id;                 // the pack's id
    std::string language;           // its row's name
    bool rtl = false;
    std::string text[2];            // the bank's Code / AgentLab sections
    std::vector<std::string> port;  // the bank's Port section: its words for the port's rows (empty: English)
    std::vector<uint8_t> font;
};
std::vector<Loaded> g_loaded;       // one per row appended
std::string g_describe = "language packs: none";
const std::string kEmpty;
const std::vector<uint8_t> kNoFont;

std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// A whole file of the disc's archive (CRASH6/CRASH.BH -> CRASH.BD), as the game's own reader finds it, read through the
// services' disc (the image, or the mods' overlay over it: the volume the game reads)
bool ArchiveFile(const std::string& name, std::string& out, std::string* error) {
    Disc& disc = Services::Get().disc();
    disc.Initialise();
    if (!disc.IsOpen()) {
        if (error) *error = disc.Error();
        return false;
    }
    const IsoFs& iso = disc.Iso();
    IsoFs::Entry bh, bd;
    if (!iso.Find("CRASH6/CRASH.BH", bh) || !iso.Find("CRASH6/CRASH.BD", bd) || bh.size < 8 || bh.size > (1u << 20)) {
        if (error) *error = "no CRASH6/CRASH.BH + CRASH.BD";
        return false;
    }
    std::vector<uint8_t> index(bh.size);
    if (iso.ReadBytes(uint64_t(bh.lba) * IsoFs::kSectorSize, bh.size, index.data()) != bh.size) {
        if (error) *error = "CRASH.BH did not read whole";
        return false;
    }
    const std::string want = Lower(name);
    size_t at = 4;
    while (at + 4 <= index.size()) {
        uint32_t len = 0;
        std::memcpy(&len, &index[at], 4);
        at += 4;
        if (len > 512 || at + len + 8 > index.size()) break;
        const std::string entry(reinterpret_cast<const char*>(&index[at]), len);
        at += len;
        uint32_t offset = 0, size = 0;
        std::memcpy(&offset, &index[at], 4);
        std::memcpy(&size, &index[at + 4], 4);
        at += 8;
        if (Lower(entry) != want) continue;
        out.assign(size, '\0');
        if (size && iso.ReadBytes(uint64_t(bd.lba) * IsoFs::kSectorSize + offset, size, reinterpret_cast<uint8_t*>(&out[0])) != size) {
            if (error) *error = name + " did not read whole";
            return false;
        }
        return true;
    }
    if (error) *error = "no " + name + " in the archive";
    return false;
}

// The text language in force, when it is a language pack's (a raw slot reads the disc's text)
const TextLanguage* SelectedPack() {
    const Table& t = Resolve();
    if (t.raw_slot >= 0 || t.text < 0 || size_t(t.text) >= t.texts.size()) return nullptr;
    const TextLanguage& l = t.texts[size_t(t.text)];
    return l.pack.empty() ? nullptr : &l;
}

// The font file read the way the text layer reads it (Font::Read's PSF, then our codepoint table) without reading past its end:
// "" when it is whole, else what is wrong. The text layer trusts the bytes it is given; this is where they are checked.
std::string CheckFont(const std::vector<uint8_t>& f) {
    size_t at = 0;
    auto u32 = [&](uint32_t& v) {
        if (at + 4 > f.size()) return false;
        std::memcpy(&v, &f[at], 4);
        at += 4;
        return true;
    };
    auto skip = [&](size_t n) {
        if (at + n > f.size()) return false;
        at += n;
        return true;
    };
    uint32_t pages = 0, v = 0;
    if (!u32(pages) || pages == 0 || pages > 3) return "page count " + std::to_string(pages);
    for (uint32_t p = 0; p < pages; ++p) {
        uint32_t size = 0;
        if (!u32(v) || !u32(v) || !u32(size) || size < 0x80 || !skip(size)) return "page " + std::to_string(p) + "'s texture";
        uint32_t name = 0, shaders = 0;
        if (!skip(8) || !u32(v) || !u32(name) || name > 256 || !skip(name) || !u32(shaders) || shaders > 4)
            return "page " + std::to_string(p) + "'s material";
        for (uint32_t s = 0; s < shaders; ++s) {
            uint32_t type = 0;
            if (!u32(type)) return "a shader";
            const size_t extra = (type == 0x10 || type == 0x11 || type == 0x18) ? 4 : type == 0x17 ? 12 : type == 0x1A ? 20 : type == 0x1C ? 8 : 0;
            if (!skip(extra) || at + 30 > f.size() || f[at + 29] != 0) return "a shader (an animated one is not a font's)";
            if (!skip(30 + 4 + 16 + 16 + 16 + 4 + 4)) return "a shader";
        }
    }
    uint32_t glyphs = 0, first = 0, magic = 0, version = 0, count = 0;
    if (!u32(glyphs) || glyphs == 0 || glyphs > 256 || !u32(first) || !skip(size_t(glyphs) * 16)) return "the glyphs";
    if (!u32(magic) || magic != 0x4D474554u || !u32(version) || !u32(count) || count != glyphs || !skip(size_t(count) * 4))
        return "the codepoint table (TEGM) after the glyphs";
    return at == f.size() ? "" : "bytes after the codepoint table";
}

const Loaded* ActiveLoaded() {
    const TextLanguage* l = SelectedPack();
    if (!l) return nullptr;
    for (const Loaded& m : g_loaded) if (m.id == l->pack) return &m;
    return nullptr;
}

}  // namespace

std::vector<std::string> TextsOf(const std::string& file) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : file) {
        if (c == '\r' || c == '\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string EnglishKey(const std::string& file) {
    std::string joined;
    const std::vector<std::string> texts = TextsOf(file);
    for (size_t i = 0; i < texts.size(); ++i) {
        if (i) joined.push_back('\n');
        joined += texts[i];
    }
    return ps2r::hash::Sha256::HexOf(joined.data(), joined.size());
}

bool ParseBank(const std::string& data, std::vector<Section>& out, std::string* error) {
    out.clear();
    size_t at = 0;
    Section* cur = nullptr;
    uint32_t taken = 0;
    while (at < data.size()) {
        size_t end = data.find_first_of("\r\n", at);
        if (end == std::string::npos) end = data.size();
        const std::string line = data.substr(at, end - at);
        at = end;
        while (at < data.size() && (data[at] == '\r' || data[at] == '\n')) ++at;
        if (line.empty()) continue;
        if (line.rfind("@@ ", 0) == 0) {
            if (cur && taken != cur->texts) {
                if (error) *error = "section " + cur->file + " has " + std::to_string(taken) + " texts, its header says " + std::to_string(cur->texts);
                return false;
            }
            char file[32] = {0}, sha[80] = {0};
            unsigned texts = 0;
            if (std::sscanf(line.c_str() + 3, "%31s %u %79s", file, &texts, sha) != 3 || std::strlen(sha) != 64) {
                if (error) *error = "bad section header '" + line + "'";
                return false;
            }
            out.push_back(Section{file, texts, sha, ""});
            cur = &out.back();
            taken = 0;
            continue;
        }
        if (!cur) {
            if (error) *error = "text before the first section header";
            return false;
        }
        cur->text += line + "\r\n";
        ++taken;
    }
    if (cur && taken != cur->texts) {
        if (error) *error = "section " + cur->file + " has " + std::to_string(taken) + " texts, its header says " + std::to_string(cur->texts);
        return false;
    }
    if (out.empty() && error) *error = "no section";
    return !out.empty();
}

const std::vector<std::string>& PortEnglish() {
    static const std::vector<std::string> texts = {"text language", "speech language"};
    return texts;
}

void Reset() {
    g_loaded.clear();
    g_describe = "language packs: none";
}

void AppendRows(Table& table) {
    g_loaded.clear();
    std::string why;
    // the disc's English text: the slot whose Code text IS English (this PAL image keeps it in the `Spanish` slot)
    const Bank* english = nullptr;
    for (const Bank& b : table.banks) if (b.recognised && b.content == "english") { english = &b; break; }
    const TextLanguage* english_row = nullptr;
    for (const TextLanguage& l : table.texts) if (l.pack.empty() && l.name == "english") { english_row = &l; break; }
    std::string english_keys[2];
    bool keyed = false;
    int rows = 0;
    // the port's own English row titles: what a Port section must translate to be used
    std::string port_joined;
    for (size_t i = 0; i < PortEnglish().size(); ++i) port_joined += (i ? "\n" : "") + PortEnglish()[i];
    const std::string port_key = ps2r::hash::Sha256::HexOf(port_joined.data(), port_joined.size());
    for (const te::mods::Info& info : te::mods::LanguagePacks()) {
        const te::mods::LanguageInfo& li = info.language;
        auto skip = [&](const std::string& reason) { why += " [" + info.id + ": " + reason + "]"; };
        if (li.id.empty() || li.text_bank.empty()) { skip("mod.json has no language.id / language.text_bank"); continue; }
        const std::string name = Lower(li.id);
        if (std::any_of(table.texts.begin(), table.texts.end(), [&](const TextLanguage& l) { return l.name == name; })) {
            skip("the disc has a text language '" + name + "' already");
            continue;
        }
        if (!english || !english_row) { skip("this disc has no English text to line the translation up with"); continue; }
        if (!keyed) {
            for (int f = 0; f < 2; ++f) {
                std::string file, error;
                if (!ArchiveFile(std::string("Language\\") + kFiles[f] + "\\" + english->name + ".txt", file, &error)) {
                    english_keys[f].clear();
                    why += " [the disc's English " + std::string(kFiles[f]) + " text: " + error + "]";
                } else {
                    english_keys[f] = EnglishKey(file);
                }
            }
            keyed = true;
        }
        std::vector<uint8_t> bank;
        std::string error;
        if (!te::mods::ReadModFile(info.id, li.text_bank, bank, &error)) { skip(error); continue; }
        std::vector<Section> sections;
        if (!ParseBank(std::string(bank.begin(), bank.end()), sections, &error)) { skip(li.text_bank + ": " + error); continue; }
        Loaded m;
        m.id = info.id;
        m.language = name;
        m.rtl = Lower(li.direction) == "rtl";
        bool fits = true;
        for (int f = 0; f < 2 && fits; ++f) {
            const Section* sec = nullptr;
            for (const Section& x : sections) if (Lower(x.file) == Lower(kFiles[f])) sec = &x;
            if (!sec) { skip(li.text_bank + " has no " + kFiles[f] + " section"); fits = false; break; }
            if (english_keys[f].empty() || sec->sha256 != english_keys[f]) {
                skip(li.text_bank + "'s " + kFiles[f] + " translates another English text than this disc's (sha256 " + sec->sha256.substr(0, 12) +
                     ", the disc's " + (english_keys[f].empty() ? std::string("unread") : english_keys[f].substr(0, 12)) + ")");
                fits = false;
                break;
            }
            m.text[f] = sec->text;
        }
        if (!fits) continue;
        // the port's rows in the pack's words (optional; only for the port's English as it is now)
        for (const Section& x : sections)
            if (Lower(x.file) == "port" && x.sha256 == port_key && x.texts == PortEnglish().size()) m.port = TextsOf(x.text);
        if (!li.font.empty() && !te::mods::ReadModFile(info.id, li.font, m.font, &error)) { skip(error); continue; }
        if (!m.font.empty()) {
            const std::string bad = CheckFont(m.font);
            if (!bad.empty()) { skip(li.font + " does not read as a font: " + bad); continue; }
        }
        if (m.rtl && m.font.empty()) { skip("a right-to-left language needs its font (language.font)"); continue; }
        TextLanguage row;
        row.name = name;
        row.pack = info.id;
        row.slot = english_row->slot;   // its files stand in for the English text's (the game reads that slot's numbers)
        table.texts.push_back(row);
        why += " [" + info.id + ": row '" + name + "', " + std::to_string(TextsOf(m.text[0]).size()) + " + " +
               std::to_string(TextsOf(m.text[1]).size()) + " texts (made from this disc's English), " +
               (m.port.empty() ? std::string("the port's rows in English") : std::to_string(m.port.size()) + " port rows") + ", font " +
               std::to_string(m.font.size()) + " bytes, " + (m.rtl ? "right to left" : "left to right") + "]";
        g_loaded.push_back(std::move(m));
        ++rows;
    }
    g_describe = "language packs: " + std::to_string(rows) + " row(s)" + why;
    std::fprintf(stderr, "[twinsanityevolved] %s\n", g_describe.c_str());
}

std::string PortText(uint32_t row) {
    const Loaded* m = ActiveLoaded();
    return m && row < m->port.size() ? m->port[row] : "";
}

bool Active() { return ActiveLoaded() != nullptr; }

bool RightToLeft() {
    const Loaded* m = ActiveLoaded();
    return m && m->rtl;
}

const std::string& Text(uint32_t file) {
    const Loaded* m = ActiveLoaded();
    return m && file < 2 ? m->text[file] : kEmpty;
}

const std::vector<uint8_t>& Font() {
    const Loaded* m = ActiveLoaded();
    return m ? m->font : kNoFont;
}

std::string Describe() { return g_describe; }

}  // namespace te::language::mod

// ---- the seam (include/platform/system.h) and the text layer's imports (tools/platform_imports.tsv) ----------------------
extern "C" {

ps2e_s32 ps2e_system_language_text_size(ps2e_s32 file) {
    return ps2e_s32(te::language::mod::Text(uint32_t(file)).size());
}

void ps2e_system_language_text(ps2e_s32 file, ps2e_u32 out, ps2e_s32 size) {
    const std::string& text = te::language::mod::Text(uint32_t(file));
    if (size <= 0) return;
    uint8_t* p = te::GuestPointer(out, size_t(size));
    if (!p) return;
    const size_t n = std::min(text.size(), size_t(size));
    std::memcpy(p, text.data(), n);
}

ps2e_s32 ps2e_text_direction(void) { return te::language::mod::RightToLeft() ? 1 : 0; }

ps2e_s32 ps2e_text_font_size(void) { return ps2e_s32(te::language::mod::Font().size()); }

ps2e_s32 ps2e_text_font_read(ps2e_u32 out, ps2e_s32 size) {
    const std::vector<uint8_t>& font = te::language::mod::Font();
    if (size <= 0 || font.empty()) return 0;
    uint8_t* p = te::GuestPointer(out, size_t(size));
    if (!p) return 0;
    const size_t n = std::min(font.size(), size_t(size));
    std::memcpy(p, font.data(), n);
    std::fprintf(stderr, "[twinsanityevolved] text: the language's own font read (%zu bytes)\n", n);
    return ps2e_s32(n);
}

ps2e_s32 ps2e_text_diag(void) {
    static const bool on = std::getenv("TE_GFX_DIAG_TEXT") != nullptr;
    return on ? 1 : 0;
}

// One shaped text (the text layer's te::text::Diag in guest memory): its string, the lines' boxes and every glyph drawn, in
// frame pixels like the text-quad census line beside it
void ps2e_text_shaped(ps2e_u32 record) {
    const auto* d = reinterpret_cast<const te::text::Diag*>(te::GuestPointer(record, sizeof(te::text::Diag)));
    if (!d) return;
    const float ox = te::gfx::GsOriginX(), oy = te::gfx::GsOriginY();
    const char* text = d->text ? te::GuestString(d->text, 256) : nullptr;
    std::string line;
    char buf[160];
    std::snprintf(buf, sizeof buf, "[te.text.shape] present %llu rtl %d align %#x anchor (%.1f %.1f) box (%.1f %.1f)-(%.1f %.1f) lines %d glyphs %d missing %d",
                  (unsigned long long)te_graphics_presents() + 1, d->rtl, unsigned(d->alignment), d->anchorX / 16.f - ox, d->anchorY / 16.f - oy,
                  d->boxX0 / 16.f - ox, d->boxY0 / 16.f - oy, d->boxX1 / 16.f - ox, d->boxY1 / 16.f - oy, d->lineCount, d->glyphCount, d->missing);
    line = buf;
    const int lines = std::min<int>(d->lineCount, te::text::kMaxLines);
    for (int l = 0; l < lines; ++l) {
        const te::text::DiagLine& dl = d->lines[l];
        std::snprintf(buf, sizeof buf, " | line %d (%.1f %.1f)-(%.1f %.1f):", l, dl.x0 / 16.f - ox, dl.y0 / 16.f - oy, dl.x1 / 16.f - ox, dl.y1 / 16.f - oy);
        line += buf;
        for (int g = dl.first; g < dl.first + dl.count && g < te::text::kDiagGlyphs && g < d->glyphCount; ++g) {
            const te::text::DiagGlyph& dg = d->glyphs[g];
            std::snprintf(buf, sizeof buf, " %c%u:%04X@%.1f-%.1f", dg.font == te::text::kFontArabic ? 'a' : 'g', unsigned(dg.index), unsigned(dg.codepoint),
                          dg.x0 / 16.f - ox, dg.x1 / 16.f - ox);
            line += buf;
        }
    }
    std::fprintf(stderr, "%s | frame %dx%d string '%s'\n", line.c_str(), te::gfx::ScreenW(), te::gfx::ScreenH(), text ? text : "");
}

}  // extern "C"
