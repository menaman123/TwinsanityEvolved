// te_language_mod.h -- a LANGUAGE PACK as a text language of the language table (te_language.h): a translation the disc does
// not carry, read from the mods folder (te_mods.h, mod.json kind "language"). A language pack is NOT a mod: it has no
// switch and no place on the Mods pages or in mods.order, never sends the saves to the -mods card, and is offered -- only as
// a choice of the Options menu's TEXT LANGUAGE row -- whenever it is in the mods folder and its bank lines up with this
// disc's English.
//
// A language pack brings TEXT only (D-D): its bank (mod.json language.text_bank) replaces the two text files the game reads,
// its font (language.font) is drawn beside the game's own font by the text layer, its direction (language.direction "rtl")
// tells the text layer to shape and lay it out right to left. Its row's word is language.label ("arabic"; the pack's id
// when it has none). The speech is the SPEECH LANGUAGE row's (a pack never adds one).
//
// The bank file: Windows-1256 text, CR LF lines, in sections, each opened by a line
//     @@ <file> <texts> <sha256>        (file: Code | AgentLab | Port; texts: how many; sha256: of the English text it translates)
// followed by exactly <texts> non-blank lines. The sha256 is over the English file's texts as the game splits them (blank lines
// dropped) joined by '\n': a bank is offered only on a disc whose English text it was made from, so its texts line up with the
// game's text numbers (another disc's English -- the NTSC releases -- has other texts in another order). The optional Port
// section translates the port's own row titles (PortEnglish(): "text language", "speech language"); it is used only while its
// sha256 is that of the port's English.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "te_language.h"

namespace te::language::mod {

// Appends one text language per language pack whose bank fits this disc (Resolve calls it after the disc's languages)
void AppendRows(Table& table);
// Forget the loaded banks and fonts (te::language::Reset)
void Reset();

// The text language in force, when it is a language pack's: its text files, its font, its direction (empty / false otherwise)
bool Active();
bool RightToLeft();
const std::string& Text(uint32_t file);           // the bank's section for the game's TextFile (0 Code, 1 AgentLab)
const std::vector<uint8_t>& Font();
// The pack's own words for the port's row `row` (PortEnglish()'s order), "" when the pack in force has none (or none is)
std::string PortText(uint32_t row);
// The port's English row titles, in order: what a bank's Port section translates
const std::vector<std::string>& PortEnglish();
// One line for the log: the rows appended and why any pack was not
std::string Describe();

// ---- the bank format (public for the tests) ----
struct Section {
    std::string file;      // "Code" | "AgentLab" | "Port"
    uint32_t texts = 0;
    std::string sha256;    // of the English file it translates (its texts joined by '\n')
    std::string text;      // the section's lines, CR LF
};
bool ParseBank(const std::string& data, std::vector<Section>& out, std::string* error);
// The game's text file as ReadTextFile splits it: the non-blank lines
std::vector<std::string> TextsOf(const std::string& file);
// The sha256 the bank's header names for an English file
std::string EnglishKey(const std::string& file);

}  // namespace te::language::mod
