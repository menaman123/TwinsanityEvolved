// app/host/language_probes.h -- what language a text bank of the disc is written in, by its CONTENT: the ONE table the game
// (platform/nativeee/host/language.cpp: the Options menu's TEXT LANGUAGE row) and the launcher (app/host/disc_languages: the
// Settings page's Text language row) both read, so the two always offer the same languages for the same disc.
//
// A bank is the game's Language\Code\<slot>.txt (inside CRASH6/CRASH.BD): its first lines are the menus' words -- off / on /
// no / yes / back / continue, then new game / load game / options / language / graphic options ... A language is recognised
// when two or more of its tell-tale words open lines of the bank's first 40 lines (English's and Spanish's first lines
// coincide -- off/on/no/yes are English in both -- so one word never decides). Lower case, ASCII where the words differ by
// accents only (UTF-8 and the disc's Latin-1 both pass the ASCII parts). Header-only, plain C++17: no engine dependency.
#pragma once
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace ps2r::app::language_probes {

struct Tell {
    const char* language;
    const char* const* words;
    size_t count;
};

inline const std::vector<Tell>& Tells() {
    static const char* const english[] = {"new game", "load game", "graphic options", "sound options", "game options", "widescreen"};
    static const char* const portuguese[] = {"novo jogo", "carregar jogo", "op", "idioma", "desativar", "ativar", "voltar", "continuar"};
    static const char* const french[] = {"nouvelle partie", "charger", "langue", "options graphiques", "options sonores", "retour", "continuer"};
    static const char* const german[] = {"neues spiel", "spiel laden", "optionen", "sprache", "grafik-optionen", "sound-optionen", "aus", "ein", "nein", "ja"};
    static const char* const italian[] = {"nuova partita", "carica partita", "opzioni", "lingua", "opzioni grafiche", "opzioni audio", "indietro", "continua"};
    static const char* const spanish[] = {"nuevo juego", "nueva partida", "cargar", "opciones", "idioma", "opciones gr", "atr", "volver", "continuar"};
    static const char* const dutch[] = {"nieuw spel", "spel laden", "opties", "taal", "terug", "verder", "uit", "aan", "nee"};
    static const std::vector<Tell> tells = {
        {"english", english, sizeof english / sizeof *english},
        {"portuguese", portuguese, sizeof portuguese / sizeof *portuguese},
        {"french", french, sizeof french / sizeof *french},
        {"german", german, sizeof german / sizeof *german},
        {"italian", italian, sizeof italian / sizeof *italian},
        {"spanish", spanish, sizeof spanish / sizeof *spanish},
        {"dutch", dutch, sizeof dutch / sizeof *dutch},
    };
    return tells;
}

inline std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// The bank's first non-empty lines, lower case
inline std::vector<std::string> Lines(const std::string& text, size_t at_most) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\r' || c == '\n') {
            if (!cur.empty()) { out.push_back(Lower(cur)); cur.clear(); if (out.size() >= at_most) break; }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && out.size() < at_most) out.push_back(Lower(cur));
    return out;
}

// The language a Code bank's text is in ("" when none is recognised); `probe` gets the bank's "new game" line (text 9)
inline std::string Recognize(const std::string& text, std::string* probe) {
    const std::vector<std::string> lines = Lines(text, 40);
    if (probe) *probe = lines.size() > 9 ? lines[9] : "";
    const char* best = nullptr;
    int best_score = 0;
    for (const Tell& tell : Tells()) {
        int score = 0;
        for (const std::string& line : lines)
            for (size_t w = 0; w < tell.count; ++w)
                if (line == tell.words[w] || (std::strlen(tell.words[w]) >= 6 && line.rfind(tell.words[w], 0) == 0)) { ++score; break; }
        if (score > best_score) { best_score = score; best = tell.language; }
    }
    return best && best_score >= 2 ? best : "";
}

// The language a slot's NAME means: the speech bank Crash6\<name>.mb speaks it (retail names its speech banks truthfully;
// "American" is English)
inline std::string LanguageOfSlotName(const std::string& name) {
    const std::string l = Lower(name);
    return l == "american" ? "english" : l;
}

// The slots of the PAL executable's language table, in its order (the US one has only American)
inline const std::vector<std::string>& PalSlots() {
    static const std::vector<std::string> slots = {"English", "French", "German", "Spanish", "Italian"};
    return slots;
}

// How many bytes of a Code bank the recognition reads (the first menu lines lie well inside)
constexpr size_t kProbeBytes = 2048;

}  // namespace ps2r::app::language_probes
