// platform/nativeee (host side) -- te_language.h: the game's TEXT language and SPEECH language, two settings. See the
// header for the model.
//
// Enumeration: the slot names come from the retail image in guest memory (GlobalLanguagesArray, a NUL-terminated list of
// pointers to the names, PAL 0x002E6F00 -- the game's g_GameLanguageNames the executable carries), the banks from the disc's
// archive index CRASH6/CRASH.BH (u32 count; then per entry u32 name length, the name, u32 offset, u32 size -- the game's own
// FileStreamOpenArchive reads the same), the first bytes of each Language\Code\<name>.txt out of CRASH6/CRASH.BD, and the
// speech banks Crash6\<name>.mb from the disc's directory. Nothing is extracted to disk (game data is read at run time
// only). The content recognition is the table the launcher's Settings page reads too (app/host/language_probes.h).
#include "te_language.h"
#include "te_language_mod.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "app/host/language_probes.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/core/config/settings_file.h"
#include "te_host.h"
#include "te_mods.h"
#include "te_services.h"

namespace te::language {

namespace {

namespace probes = ps2r::app::language_probes;

constexpr uint32_t kGlobalLanguagesArray = 0x002E6F00;   // game/symbol_addrs.txt GlobalLanguagesArray (PAL SLES-52568)
constexpr uint32_t kMaxSlots = 16;

// The console language the game's GameLanguageOf (game/src/game/gamecontext.cpp) maps to a slot of that NAME: French 2,
// Spanish 3, German 4, Italian 5, everything else English's slot (1). A slot the game has no case for (none on either disc)
// cannot be selected by a code and reads 1.
int32_t CodeOfSlotName(const std::string& name) {
    if (name == "French") return 2;
    if (name == "Spanish") return 3;
    if (name == "German") return 4;
    if (name == "Italian") return 5;
    return 1;
}

std::string Lower(std::string s) { return probes::Lower(std::move(s)); }

Table g_table;
bool g_resolved = false;

std::vector<std::string> SlotNamesFromImage() {
    std::vector<std::string> names;
    for (uint32_t i = 0; i < kMaxSlots; ++i) {
        const uint8_t* p = GuestPointer(RetailAddress(kGlobalLanguagesArray + i * 4), 4);
        if (!p) break;
        uint32_t ptr = 0;
        std::memcpy(&ptr, p, 4);
        if (ptr == 0) break;
        const char* s = GuestString(ptr, 32);
        if (!s || !*s) break;
        // A name is letters only (the retail array's 5 pointers are followed by other data on another executable)
        bool letters = true;
        for (const char* c = s; *c; ++c) letters = letters && ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'));
        if (!letters) break;
        names.emplace_back(s);
    }
    return names;
}

const Bank* BankOfSlot(const std::vector<Bank>& banks, int32_t slot) {
    for (const Bank& b : banks) if (int32_t(b.slot) == slot) return &b;
    return nullptr;
}

int32_t FindText(const Table& t, const std::string& name) {
    for (size_t i = 0; i < t.texts.size(); ++i) if (t.texts[i].name == name) return int32_t(i);
    return -1;
}

int32_t FindSpeech(const Table& t, const std::string& name) {
    for (size_t i = 0; i < t.speeches.size(); ++i) if (t.speeches[i].name == name) return int32_t(i);
    return -1;
}

// english, else the first; -1 for an empty list
template <typename List> int32_t EnglishOrFirst(const List& list) {
    for (size_t i = 0; i < list.size(); ++i) if (list[i].name == "english") return int32_t(i);
    return list.empty() ? -1 : 0;
}

void Choose(Table& t) {
    const Settings& s = GetSettings();
    t.text = t.speech = -1;
    t.raw_slot = -1;
    t.how_text.clear();
    t.how_speech.clear();
    t.raw_how.clear();
    if (t.banks.empty()) return;
    // 1 a raw slot (--language-bank N): text = voice = the slot, exactly what retail did for a console set to it (the judges'
    //   "as retail" arm; the pad logs' `# language-bank=N` pin)
    if (s.language_bank >= 0 && BankOfSlot(t.banks, s.language_bank)) {
        t.raw_slot = s.language_bank;
        t.raw_how = "raw slot " + std::to_string(s.language_bank);
    } else if (s.language >= 0 && s.language_name.empty() && s.text_language_name.empty() && s.speech_language_name.empty()) {
        // 2 a raw console code (--language 0..7, the raw-code form): the slot the game's GameLanguageOf maps it to, raw
        int32_t slot = 0;
        for (const Bank& b : t.banks) if (b.code == s.language && b.code != 1) slot = int32_t(b.slot);
        t.raw_slot = slot;
        t.raw_how = "console code " + std::to_string(s.language) + " -> slot " + std::to_string(slot);
    }
    if (t.raw_slot >= 0) {
        // the rows show the raw slot's languages where they have them (a step on either leaves the raw slot)
        t.requested_text = t.requested_speech = "slot " + std::to_string(t.raw_slot);
        for (size_t i = 0; i < t.texts.size() && t.text < 0; ++i)
            if (t.texts[i].pack.empty() && t.texts[i].slot == t.raw_slot) t.text = int32_t(i);
        for (size_t i = 0; i < t.speeches.size() && t.speech < 0; ++i)
            if (t.speeches[i].slot == t.raw_slot) t.speech = int32_t(i);
        t.how_text = t.how_speech = t.raw_how;
        return;
    }
    // 3 the names: each row's own key, else the legacy one name through the pairing, else english
    const std::string legacy = Lower(s.language_name);
    std::string pair_text, pair_speech;
    LegacyPair(t.texts, t.speeches, legacy.empty() ? "english" : legacy, &pair_text, &pair_speech);
    const bool own_text = !s.text_language_name.empty(), own_speech = !s.speech_language_name.empty();
    t.requested_text = own_text ? Lower(s.text_language_name) : pair_text;
    t.requested_speech = own_speech ? Lower(s.speech_language_name) : pair_speech;
    t.text = FindText(t, t.requested_text);
    if (t.text >= 0) {
        t.how_text = own_text || legacy.empty() || pair_text == legacy || legacy.find('+') != std::string::npos ? "name" : "the pairing of '" + legacy + "'";
    } else {
        t.text = EnglishOrFirst(t.texts);
        t.how_text = "no text language '" + t.requested_text + "' on this disc: " + (t.text >= 0 ? t.texts[size_t(t.text)].name : "none");
    }
    t.speech = FindSpeech(t, t.requested_speech);
    if (t.speech >= 0) {
        t.how_speech = own_speech || legacy.empty() || pair_speech == legacy || legacy.find('+') != std::string::npos ? "name" : "the pairing of '" + legacy + "'";
    } else {
        t.speech = EnglishOrFirst(t.speeches);
        t.how_speech = "no speech language '" + t.requested_speech + "' on this disc: " + (t.speech >= 0 ? t.speeches[size_t(t.speech)].name : "none");
    }
}

// The settings file names both rows from now on: game.text_language and game.speech_language written, the legacy
// game.language line dropped (the two keys replace it), and a language pack's stale mods switch / place in mods.order
// dropped (a language pack is not a mod)
void Persist(const Table& t) {
    const Settings& s = GetSettings();
    if (s.settings_path.empty()) return;
    std::string err;
    const std::string text = t.text >= 0 ? t.texts[size_t(t.text)].name : "english";
    const std::string speech = t.speech >= 0 ? t.speeches[size_t(t.speech)].name : "english";
    bool ok = ps2r::SettingsFile::Set(s.settings_path, "game.text_language", text, &err) &&
              ps2r::SettingsFile::Set(s.settings_path, "game.speech_language", speech, &err) &&
              ps2r::SettingsFile::Remove(s.settings_path, "game.language", &err);
    if (ok) te::mods::DropPackKeys(s.settings_path);
    if (!ok)
        std::fprintf(stderr, "[twinsanityevolved] language: %s not written: %s\n", s.settings_path.c_str(), err.c_str());
    else
        std::fprintf(stderr, "[twinsanityevolved] language: game.text_language = %s, game.speech_language = %s written to %s\n", text.c_str(),
                     speech.c_str(), s.settings_path.c_str());
}

// A choice from the Options menu: the run's settings name both rows (the raw slot, if any, is left)
void Chosen(Table& t) {
    Settings s = GetSettings();
    s.language_bank = -1;
    s.language = -1;
    s.text_language_name = t.text >= 0 ? t.texts[size_t(t.text)].name : "";
    s.speech_language_name = t.speech >= 0 ? t.speeches[size_t(t.speech)].name : "";
    SetSettings(s);
    t.raw_slot = -1;
    t.raw_how.clear();
    t.requested_text = s.text_language_name;
    t.requested_speech = s.speech_language_name;
    Persist(t);
    std::fprintf(stderr, "[twinsanityevolved] %s\n", Describe().c_str());
}

}  // namespace

void LegacyPair(const std::vector<TextLanguage>& texts, const std::vector<SpeechLanguage>& speeches, const std::string& name_in,
                std::string* text, std::string* speech) {
    const std::string name = Lower(name_in);
    const size_t plus = name.find('+');
    if (plus != std::string::npos) {   // the retired "<text>+<speech>" form: the two names
        *text = name.substr(0, plus);
        *speech = name.substr(plus + 1);
        return;
    }
    *text = *speech = name;
    const auto is_text = std::find_if(texts.begin(), texts.end(), [&](const TextLanguage& l) { return l.name == name; });
    const auto is_speech = std::find_if(speeches.begin(), speeches.end(), [&](const SpeechLanguage& l) { return l.name == name; });
    if (is_text == texts.end() && is_speech != speeches.end()) {
        // speech but no text in it (this disc's Spanish): the English text, else the text its own slot carries
        if (std::any_of(texts.begin(), texts.end(), [](const TextLanguage& l) { return l.name == "english"; })) *text = "english";
        else for (const TextLanguage& l : texts) if (l.pack.empty() && l.slot == is_speech->slot) { *text = l.name; break; }
    }
    if (is_text != texts.end() && is_speech == speeches.end()) {
        // text but no speech in it (this disc's Portuguese, a language pack): English's speech, else its own slot's
        if (std::any_of(speeches.begin(), speeches.end(), [](const SpeechLanguage& l) { return l.name == "english"; })) *speech = "english";
        else for (const SpeechLanguage& l : speeches) if (l.slot == is_text->slot) { *speech = l.name; break; }
    }
}

std::string RecognizeContent(const std::string& text, std::string* probe) { return probes::Recognize(text, probe); }

bool EnumerateFromDisc(const std::string& iso_path, const std::vector<std::string>& slot_names_in, std::vector<Bank>& out,
                       std::string* error) {
    IsoFs iso;
    if (!iso.Open(iso_path, error)) { out.clear(); return false; }
    return EnumerateFromVolume(iso, slot_names_in, out, error);
}

bool EnumerateFromVolume(const IsoFs& iso, const std::vector<std::string>& slot_names_in, std::vector<Bank>& out, std::string* error) {
    out.clear();
    std::vector<std::string> slot_names = slot_names_in;
    if (slot_names.empty()) slot_names = probes::PalSlots();
    IsoFs::Entry bh, bd;
    if (!iso.Find("CRASH6/CRASH.BH", bh) || !iso.Find("CRASH6/CRASH.BD", bd) || bh.size < 8 || bh.size > (1u << 20)) {
        if (error) *error = "CRASH6/CRASH.BH or CRASH.BD not on the disc";
        return false;
    }
    std::vector<uint8_t> index(bh.size);
    if (iso.ReadBytes(uint64_t(bh.lba) * IsoFs::kSectorSize, bh.size, index.data()) != bh.size) {
        if (error) *error = "CRASH.BH did not read whole";
        return false;
    }
    // The index: Language\Code\<name>.txt -> (offset, size) in the BD
    struct Hit { uint32_t offset, size; };
    std::vector<std::pair<std::string, Hit>> code_banks;
    size_t at = 4;
    while (at + 4 <= index.size()) {
        uint32_t len = 0;
        std::memcpy(&len, &index[at], 4);
        at += 4;
        if (len > 512 || at + len + 8 > index.size()) break;
        std::string name(reinterpret_cast<const char*>(&index[at]), len);
        at += len;
        Hit h;
        std::memcpy(&h.offset, &index[at], 4);
        std::memcpy(&h.size, &index[at + 4], 4);
        at += 8;
        const std::string prefix = "Language\\Code\\";
        if (name.size() > prefix.size() + 4 && name.compare(0, prefix.size(), prefix) == 0 && name.compare(name.size() - 4, 4, ".txt") == 0)
            code_banks.emplace_back(name.substr(prefix.size(), name.size() - prefix.size() - 4), h);
    }
    for (uint32_t slot = 0; slot < slot_names.size(); ++slot) {
        const std::string& name = slot_names[slot];
        auto it = std::find_if(code_banks.begin(), code_banks.end(), [&](const auto& b) { return Lower(b.first) == Lower(name); });
        if (it == code_banks.end()) continue;
        Bank bank;
        bank.slot = slot;
        bank.name = name;
        bank.code = CodeOfSlotName(name);
        const size_t want = std::min<size_t>(probes::kProbeBytes, it->second.size);
        std::string text(want, '\0');
        const size_t got = iso.ReadBytes(uint64_t(bd.lba) * IsoFs::kSectorSize + it->second.offset, want, reinterpret_cast<uint8_t*>(&text[0]));
        text.resize(got);
        const std::string content = RecognizeContent(text, &bank.probe);
        bank.recognised = !content.empty();
        bank.content = bank.recognised ? content : Lower(name);
        // The speech bank this index streams: Crash6\<name>.mb (SetVoiceBank(Crash6\<slot>) -> LoadSoundBank: <name>.mh + .mb).
        // Retail names it truthfully (nothing in a text patch touches it): its language is the slot's name.
        IsoFs::Entry mb;
        bank.speech = iso.Find("CRASH6/" + name + ".MB", mb) && !mb.directory && mb.size > 0;
        bank.speech_language = probes::LanguageOfSlotName(name);
        out.push_back(bank);
    }
    if (out.empty() && error) *error = "no Language\\Code\\<slot>.txt bank in CRASH.BH for any slot the executable names";
    return !out.empty();
}

std::vector<TextLanguage> TextLanguagesOf(const std::vector<Bank>& banks) {
    std::vector<TextLanguage> out;
    for (const Bank& b : banks) {
        // one language per text content (a disc carrying the same text twice: the first slot's)
        if (std::any_of(out.begin(), out.end(), [&](const TextLanguage& l) { return l.name == b.content; })) continue;
        TextLanguage l;
        l.name = b.content;
        l.slot = int32_t(b.slot);
        out.push_back(l);
    }
    std::stable_partition(out.begin(), out.end(), [](const TextLanguage& l) { return l.name == "english"; });
    return out;
}

std::vector<SpeechLanguage> SpeechLanguagesOf(const std::vector<Bank>& banks) {
    std::vector<SpeechLanguage> out;
    for (const Bank& b : banks) {
        if (!b.speech) continue;
        if (std::any_of(out.begin(), out.end(), [&](const SpeechLanguage& l) { return l.name == b.speech_language; })) continue;
        SpeechLanguage l;
        l.name = b.speech_language;
        l.slot = int32_t(b.slot);
        l.bank = "Crash6\\" + b.name + ".mb";
        out.push_back(l);
    }
    std::stable_partition(out.begin(), out.end(), [](const SpeechLanguage& l) { return l.name == "english"; });
    return out;
}

const Table& Resolve() {
    if (g_resolved) return g_table;
    g_resolved = true;
    g_table = Table{};
    const Settings& s = GetSettings();
    if (s.iso_path.empty()) {
        g_table.error = "no disc image";
    } else {
        std::vector<std::string> names = SlotNamesFromImage();
        if (names.empty()) names = probes::PalSlots();
        std::string error;
        // the services' disc: the image, or the mods' overlay over it (a mod's text banks are the ones recognised)
        Disc& disc = Services::Get().disc();
        disc.Initialise();
        if (!disc.IsOpen()) g_table.error = disc.Error();
        else if (!EnumerateFromVolume(disc.Iso(), names, g_table.banks, &error)) g_table.error = error;
    }
    g_table.texts = TextLanguagesOf(g_table.banks);
    g_table.speeches = SpeechLanguagesOf(g_table.banks);
    mod::AppendRows(g_table);   // the language packs' text languages after the disc's (te_language_mod.h)
    Choose(g_table);
    return g_table;
}

void Reset() {
    g_resolved = false;
    g_table = Table{};
    mod::Reset();
}

int32_t ConsoleLanguage() {
    const Table& t = Resolve();
    int32_t voice = t.raw_slot;
    if (voice < 0 && t.speech >= 0) voice = t.speeches[size_t(t.speech)].slot;
    if (voice < 0 && t.text >= 0) voice = t.texts[size_t(t.text)].slot;   // a disc without speech banks: the text's slot
    if (voice >= 0) {
        const Bank* b = BankOfSlot(t.banks, voice);
        return b ? b->code : 1;
    }
    // No banks to look at (no disc / the tests' fake image): a raw code stands, anything else is English
    const int32_t raw = GetSettings().language;
    return raw >= 0 && raw <= 7 ? raw : 1;
}

int32_t TextBank() {
    const Table& t = Resolve();
    if (t.raw_slot >= 0) return t.raw_slot;
    return t.text >= 0 ? t.texts[size_t(t.text)].slot : -1;
}

uint32_t TextCount() { return uint32_t(Resolve().texts.size()); }
int32_t TextSelected() { return Resolve().text; }
std::string TextLabel(uint32_t choice) {
    const Table& t = Resolve();
    return choice < t.texts.size() ? t.texts[choice].name : "";
}

uint32_t SpeechCount() { return uint32_t(Resolve().speeches.size()); }
int32_t SpeechSelected() { return Resolve().speech; }
std::string SpeechLabel(uint32_t choice) {
    const Table& t = Resolve();
    return choice < t.speeches.size() ? t.speeches[choice].name : "";
}

bool SelectText(uint32_t choice) {
    Table& t = const_cast<Table&>(Resolve());
    if (choice >= t.texts.size()) {
        std::fprintf(stderr, "[twinsanityevolved] language: the Options menu named text language %u, which this disc has no row for\n", choice);
        return false;
    }
    t.text = int32_t(choice);
    t.how_text = "options menu";
    if (t.speech < 0) { t.speech = EnglishOrFirst(t.speeches); t.how_speech = "options menu"; }
    Chosen(t);
    return true;
}

bool SelectSpeech(uint32_t choice) {
    Table& t = const_cast<Table&>(Resolve());
    if (choice >= t.speeches.size()) {
        std::fprintf(stderr, "[twinsanityevolved] language: the Options menu named speech language %u, which this disc has no row for\n", choice);
        return false;
    }
    t.speech = int32_t(choice);
    t.how_speech = "options menu";
    if (t.text < 0) { t.text = EnglishOrFirst(t.texts); t.how_text = "options menu"; }
    Chosen(t);
    return true;
}

std::string RowTitle(uint32_t row) {
    if (row > 1) return "";
    const std::string own = mod::PortText(row);   // a language pack in force carries its own words for the port's rows
    if (!own.empty()) return own;
    // the disc's text languages: the port's two rows in the language of the text around them (lower case like the game's
    // menus, the letters its font draws), as the graphic options' port rows are (MenuStrings); English for any other
    struct Title { const char* language; const char* text; const char* speech; };
    static const Title kTitles[] = {
        {"english", "text language", "speech language"},       {"french", "langue du texte", "langue des voix"},
        {"german", "textsprache", "sprachausgabe"},            {"spanish", "idioma del texto", "idioma de las voces"},
        {"italian", "lingua del testo", "lingua delle voci"},  {"portuguese", "idioma do texto", "idioma das vozes"},
    };
    const Table& t = Resolve();
    std::string language = "english";
    if (t.raw_slot >= 0) {
        if (const Bank* b = BankOfSlot(t.banks, t.raw_slot)) language = b->content;
    } else if (t.text >= 0) {
        language = t.texts[size_t(t.text)].name;
    }
    for (const Title& title : kTitles)
        if (language == title.language) return row == 0 ? title.text : title.speech;
    return row == 0 ? kTitles[0].text : kTitles[0].speech;
}

std::string Describe() {
    const Table& t = Resolve();
    std::string out = "language: ";
    if (t.banks.empty()) return out + "no banks enumerated (" + t.error + "); Language() answers " + std::to_string(ConsoleLanguage());
    out += "disc slots";
    for (const Bank& b : t.banks) {
        out += " [" + std::to_string(b.slot) + " " + b.name + ": text " + b.content + (b.recognised ? "" : " (slot name)") +
               " '" + b.probe + "', speech " + (b.speech ? b.speech_language + " (Crash6\\" + b.name + ".mb)" : "none") + "]";
    }
    out += "; text languages";
    for (const TextLanguage& l : t.texts)
        out += " [" + l.name + ": text slot " + std::to_string(l.slot) + (l.pack.empty() ? "" : ", pack " + l.pack) + "]";
    out += "; speech languages";
    for (const SpeechLanguage& l : t.speeches) out += " [" + l.name + ": voice slot " + std::to_string(l.slot) + " (" + l.bank + ")]";
    out += "; requested text '" + t.requested_text + "' speech '" + t.requested_speech + "' ->";
    const std::string text = t.text >= 0 ? t.texts[size_t(t.text)].name : "none";
    const std::string speech = t.speech >= 0 ? t.speeches[size_t(t.speech)].name : "none";
    out += " text " + text + " by " + t.how_text + ": text slot " + std::to_string(TextBank()) + ";";
    out += " speech " + speech + " by " + t.how_speech + ": voice slot " + std::to_string(t.raw_slot >= 0 ? t.raw_slot : t.speech >= 0 ? t.speeches[size_t(t.speech)].slot : TextBank()) + ";";
    out += " Language() " + std::to_string(ConsoleLanguage());
    return out;
}

}  // namespace te::language
