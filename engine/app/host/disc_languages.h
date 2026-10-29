// app/host/disc_languages.h -- what languages a disc of the game offers, read from the disc itself: the TEXT languages
// (the text banks Language\Code\<slot>.txt inside CRASH6/CRASH.BD, by what they CONTAIN: app/host/language_probes.h, the
// table the game's Options menu uses) and the SPEECH languages (the speech banks CRASH6/<slot>.MB, by the slot's name), plus
// what a language pack must have been made from to line up with this disc's English text (the sha256 keys the pack's bank
// names). Plain C++17 over the engine's ISO reader and SHA-256: the launcher's Settings page derives its two language rows
// with it, so they offer what the game's two rows offer for the same disc.
//
// The order is the game's: english first, then the slots in the PAL executable's order (English, French, German, Spanish,
// Italian), then any other slot the archive names (the North American disc's American).
#pragma once
#include <string>
#include <vector>

class IsoFs;

namespace ps2r::app {

struct DiscLanguages {
    bool ok = false;
    std::string error;                    // why nothing was read
    std::vector<std::string> text;        // the text languages ("english", "portuguese", ...)
    std::vector<std::string> speech;      // the speech languages ("english", "french", ...)
    // the sha256 of the disc's ENGLISH text files (their non-blank lines joined by '\n'): a language pack's bank sections
    // name the English they translate (te_language_mod.h) -- "" when the disc has no English text
    std::string english_code_key, english_agentlab_key;
};

bool ReadDiscLanguages(const IsoFs& iso, DiscLanguages* out);
bool ReadDiscLanguages(const std::string& iso_path, DiscLanguages* out);

// A language pack's bank text: the sha256 its Code and AgentLab sections name ("" when a section is missing)
void BankKeys(const std::string& bank, std::string* code_key, std::string* agentlab_key);
// Whether a pack whose bank names these keys lines up with this disc's English text (the game offers it then)
bool PackFits(const DiscLanguages& disc, const std::string& code_key, const std::string& agentlab_key);

// The legacy one name (ps2r.ini game.language, the launcher's earlier Language row) as the two rows' values, the single
// setting's pairing: "<a>+<b>" is the two; a speech language without text reads english; a text language without speech
// speaks english (the game's te::language::LegacyPair, by names)
void LegacyPair(const std::vector<std::string>& text, const std::vector<std::string>& speech, const std::string& name,
                std::string* text_name, std::string* speech_name);

}  // namespace ps2r::app
