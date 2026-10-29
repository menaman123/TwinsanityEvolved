// te_language.h -- the game's languages as two settings: the TEXT language and the SPEECH language.
//
// The retail game has ONE language index, g_CurrentLanguage (0..4 on the PAL executable: English, French, German, Spanish,
// Italian; 0 `American` on the US one), picked from the console's OSD language (Platform::System::Language() -> GameLanguageOf).
// EVERYTHING follows that index: the two text files Language\Code|AgentLab\<slot>.txt, the streamed speech bank
// Crash6\<slot>.mb/.mh (SetVoiceBank; every long dialogue line), the FMV's sound track (GameController::StateMovie:
// channel = g_CurrentLanguage), a level's voice table (CRASH.BD subsection VoicesSubsection + index), the title-card pictures
// (Language\Titles\<slot>\*.psm), the credits text. On a retail disc every component of slot N is in language N.
//
// The PAL image used for development (SLES-52568) is a Brazilian-Portuguese fan patch of CRASH.BH/BD ONLY (2013 dates; every .mb, .mh and
// .pss is the 2004 retail file): it put Portuguese Code/AgentLab text into the `English` slot and the original English text into
// the `Spanish` slot, and touched nothing else -- so slot 0 = Portuguese text + English speech/FMV/pictures, slot 3 = English text
// + Spanish speech/FMV/pictures. The game already reads its two text files from a slot of their own (g_CurrentTextLanguage,
// LanguageTextBank()), so the text and the speech are two choices:
//
//   * TEXT LANGUAGE   the disc's text banks BY CONTENT (the slot whose Code text IS that language, recognised from its menu
//                     lines: app/host/language_probes.h), one per language, english first, then slot order; then the LANGUAGE
//                     PACKS (te_language_mod.h: a translation in the mods folder whose bank lines up with this disc's English,
//                     "arabic"). The text slot goes to the game through LanguageTextBank() and only the two text files read it.
//   * SPEECH LANGUAGE the disc's speech banks Crash6\<name>.mb by the slot's NAME (retail names its speech banks truthfully:
//                     nothing in a text patch touched them), english first, then slot order; a language pack never adds one.
//                     The voice slot is the RETAIL INDEX the game runs on (Language() answers its console code): the speech
//                     bank, the FMV sound track, a level's voices, the title cards, the legal screen and the credits pictures
//                     follow it as retail did.
//
// The settings: ps2r.ini game.text_language and game.speech_language (--text-language / --speech-language for a run). The
// LEGACY key game.language (and --language) names one language for BOTH when the two keys are absent, through the pairing the
// single setting had: a text language without speech of its own speaks English ("portuguese" -> portuguese text, english
// speech), a speech language without text reads the English text ("spanish" -> english text, spanish speech); its retired
// form "<text>+<speech>" ("arabic+french") is the two names. Default english for both. On a retail disc "french" is (1, 1)
// and the game behaves exactly as retail.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class IsoFs;   // engine/cdvd/iso_fs.h

namespace te::language {

// A slot of the game's language table (g_LanguageNames order) as the disc fills it
struct Bank {
    uint32_t slot = 0;             // the game's language index
    std::string name;              // the slot's name as the executable names it ("English", "American")
    std::string content;           // the language its Code text is written in, lower case ("portuguese"); the slot name
                                   // lower-cased when not recognised
    bool recognised = false;       // the content was recognised from the text (false: the slot's name stands)
    std::string probe;             // the line the recognition read (the bank's "new game" text; the log's evidence)
    int32_t code = 1;              // the Platform::System::ConsoleLanguage the game's GameLanguageOf maps to this slot
    bool speech = false;           // Crash6\<name>.mb is on the disc: the retail speech bank this index streams
    std::string speech_language;   // the speech bank's language = the slot's name, lower case ("american" -> "english")
};

// A TEXT language: what the two text files are in
struct TextLanguage {
    std::string name;              // "english", "portuguese", "arabic": the setting's value and the row's word
    int32_t slot = 0;              // the slot whose text files are read (a language pack: English's, which its files stand in for)
    std::string pack;              // a language pack's id (te_language_mod.h); "" = the disc's own text
};

// A SPEECH language: a speech bank of the disc
struct SpeechLanguage {
    std::string name;              // "english" (American's too), "french"
    int32_t slot = 0;              // the voice slot: the retail index (Language()'s console code maps to it)
    std::string bank;              // "Crash6\English.mb"
};

struct Table {
    std::vector<Bank> banks;                   // every slot the executable names that the disc carries a Code bank for, in slot order
    std::vector<TextLanguage> texts;           // the TEXT LANGUAGE row: english first, then slot order, then the language packs
    std::vector<SpeechLanguage> speeches;      // the SPEECH LANGUAGE row: english first, then slot order
    std::string requested_text, requested_speech;   // the names asked for (lower case)
    int32_t text = -1, speech = -1;            // the selection: indexes into texts / speeches (-1: none -- no disc, no banks)
    std::string how_text, how_speech;          // how each was chosen ("name", "options menu", "game.language 'spanish' has no text ...")
    int32_t raw_slot = -1;                     // a RAW slot (--language-bank N, a console code): text = voice = it, as retail ran a
                                               // console set to it (the judges' "as retail" arm, the pad logs' `# language-bank=N`)
    std::string raw_how;
    std::string error;                         // why enumeration found nothing (empty when it did)
};

// The disc's banks, the two lists and the selection, enumerated once (lazily: the retail image and the disc must be in place
// -- after the runner loaded the image, before or after the services started). Re-resolved by Reset.
const Table& Resolve();
// Forget the enumeration (a test, or a changed disc).
void Reset();
// Platform::System::Language(): the console code of the VOICE slot in force (1 = English when nothing is selected).
int32_t ConsoleLanguage();
// Platform::System::LanguageTextBank(): the TEXT slot in force; -1 when nothing is selected (the game then reads the slot
// Language() names, as retail did).
int32_t TextBank();
// The Options rows (Platform::System::TextLanguage* / SpeechLanguage*): how many choices, the one in force (-1: none of the
// row's -- a raw slot whose language the row has not), a choice's word.
uint32_t TextCount();
int32_t TextSelected();
std::string TextLabel(uint32_t choice);
uint32_t SpeechCount();
int32_t SpeechSelected();
std::string SpeechLabel(uint32_t choice);
// The player's choice from the Options menu: applied now (Language() / LanguageTextBank() answer for it) and persisted --
// ps2r.ini game.text_language AND game.speech_language (the file names both from then on; the legacy game.language line is
// dropped) when the settings name a file. False (logged) when the choice is unknown.
bool SelectText(uint32_t choice);
bool SelectSpeech(uint32_t choice);
// A row's title in the TEXT language in force: row 0 "text language", 1 "speech language" -- a language pack's own words
// when it carries them (te_language_mod.h PortText), else the disc text language's (english, french, german, spanish,
// italian, portuguese: the port's rows speak the text's language, as the graphic options' do), else English.
// Windows-1256 / Latin-1 bytes as the game draws them.
std::string RowTitle(uint32_t row);
// One line for the log and the judges: the slots found, their contents and speech banks, the two lists, the selection.
std::string Describe();

// The legacy pairing (public for the tests): what ONE name (game.language / --language) means for each row -- `text` and
// `speech` receive the names to look up. "<a>+<b>" is the two names; a speech language without text on this disc reads the
// English text; a text language without speech speaks English.
void LegacyPair(const std::vector<TextLanguage>& texts, const std::vector<SpeechLanguage>& speeches, const std::string& name,
                std::string* text, std::string* speech);

// The content recognition alone (public for the tests): the Code text's lines (the first few hundred bytes) -> the language
// name, "" when none matches; `probe` receives the line the decision rests on (app/host/language_probes.h).
std::string RecognizeContent(const std::string& text, std::string* probe);
// The disc image's banks without the retail image (public for the tests / tools): slot order = the given names (or the
// PAL order when empty), filtered to the Code banks the BH names; `speech` set for the slots whose Crash6\<name>.mb exists.
bool EnumerateFromDisc(const std::string& iso_path, const std::vector<std::string>& slot_names, std::vector<Bank>& out,
                       std::string* error);
// The same over a volume already open: the services' disc (the mods' overlay when one is on) -- what Resolve() reads
bool EnumerateFromVolume(const IsoFs& iso, const std::vector<std::string>& slot_names, std::vector<Bank>& out, std::string* error);
// The two lists a set of banks offers (public for the tests): one text language per content (english first, then slot
// order); one speech language per speech bank (english first, then slot order; none when the disc has no speech bank).
std::vector<TextLanguage> TextLanguagesOf(const std::vector<Bank>& banks);
std::vector<SpeechLanguage> SpeechLanguagesOf(const std::vector<Bank>& banks);

}  // namespace te::language
