#pragma once

#include "common.h"

// The platform layer: what the game needs of the machine it runs on. The game's C++ calls these interfaces and a platform
// implements them (src/platform/<platform>/, picked by configure.py). The PS2 one is on PS2SDK; the retail asm that's left calls
// Sony's SDK by name, which src/sce/ puts on the same interfaces. The platform's entry sets things up and calls Main.
namespace Platform::System
{
// Makes the machine ready for the game, first thing: on the PS2 the graphics hardware reset and the I/O processor restarted with
// the disc's modules
void Initialise();
// Starts what the pads, the memory cards and the sound go through
void StartServices();
[[noreturn]] void Exit(s32 status);

// The console's languages (the PS2's system settings')
enum ConsoleLanguage : s32
{
    LanguageJapanese = 0,
    LanguageEnglish = 1,
    LanguageFrench = 2,
    LanguageSpanish = 3,
    LanguageGerman = 4,
    LanguageItalian = 5,
    LanguageDutch = 6,
    LanguagePortuguese = 7,
};

ConsoleLanguage Language();

// TwinsanityEvolved (D49, D56): the languages the inserted disc carries, enumerated by the platform at run time. The game
// has ONE language index (g_CurrentLanguage, the slot of g_LanguageNames) and retail drove everything from it: the two text
// files Language\\Code|AgentLab\\<slot>.txt, the speech bank Crash6\\<slot>.mb, the FMV's sound track, a level's voice table,
// the title-card pictures, the credits. A disc whose TEXT banks were moved between slots (a translation patch) has no single
// slot where text and speech agree, so the TEXT and the SPEECH are two choices: Language() names the slot of everything that
// is NOT the two text files (the retail index, through GameLanguageOf), LanguageTextBank() the slot whose text files are read
// (the same slot on a retail disc). The Options menu's two rows: TEXT LANGUAGE (the text languages, by what the banks
// CONTAIN, then the translations the platform carries) and SPEECH LANGUAGE (the speech banks) -- each row's number of
// choices, the one in force (NoLanguage: none of the row's), a choice's word (the language, lower case like the game's own
// menu texts) copied into `out` (`size` bytes, NUL-terminated), and the player's choice: the platform persists it (its
// settings file) and Language() / LanguageTextBank() answer for it from now on; the game re-reads its texts and swaps its
// speech bank itself. A platform that cannot enumerate (the PS2) answers the game's own slots as the text row, no speech
// row (one language drives both, as retail) and persists nothing.
constexpr u32 NoLanguage = 0xFFFFFFFF;
u32 TextLanguageCount();
u32 TextLanguageSelected();
void TextLanguageLabel(u32 language, char* out, u32 size);
void SetTextLanguage(u32 language);
u32 SpeechLanguageCount();
u32 SpeechLanguageSelected();
void SpeechLanguageLabel(u32 language, char* out, u32 size);
void SetSpeechLanguage(u32 language);
// A language row's title in the text language in force (`row` 0 the text row, 1 the speech row) copied into `out`; empty =
// the game's own text (the text table's "language" for the text row)
void LanguageRowTitle(u32 row, char* out, u32 size);
// The slot the two text files are read from; NoLanguageBank = the slot Language() names (retail)
constexpr u32 NoLanguageBank = 0xFFFFFFFF;
u32 LanguageTextBank();
// TwinsanityEvolved: a text file of the text language in force that the DISC does not carry -- a language pack's
// translation. `file` is the game's TextFile (0 the code's texts, 1 AgentLab's). LanguageTextSize is its size in bytes, 0 when
// the language is the disc's (the game then reads the slot's file as before); LanguageText copies it into `out` (`size` bytes)
u32 LanguageTextSize(u32 file);
void LanguageText(u32 file, void* out, u32 size);

// TwinsanityEvolved: the platform's built-in MODS, shown on the Options menu's MODS page -- how many (the PS2: none), a mod's
// name (lower case like the game's menu texts) copied into `out` (`size` bytes, NUL-terminated), whether the player has it on
// (all off by default; the platform persists the switch), and the player's new choice. Only mods that apply while the game
// runs are listed: a mod that changes the disc's files is switched before the game starts, outside it
u32 ModCount();
void ModLabel(u32 mod, char* out, u32 size);
u32 ModEnabled(u32 mod);
void SetModEnabled(u32 mod, u32 enabled);
// Whether the mod has an off / on switch on the Mods page (1: a toggle row, its own page linked while it is on). A mod without
// one (0) is always available: its row on the Mods page IS the link to its page, and opening that page is what turns the mod on
// (SetModEnabled(mod, 1) as the row is chosen) -- one click, no setup step
u32 ModHasSwitch(u32 mod);
// A mod's own page (Mods > <name>): its rows (0 = no page), its title (the Mods page's link to it reads the same), a row's label,
// and what the game does when the player chooses one
u32 ModPageRows(u32 mod);
void ModPageLabel(u32 mod, char* out, u32 size);
void ModPageRowLabel(u32 mod, u32 row, char* out, u32 size);
enum ModAction : u32
{
    ModActionNone = 0,
    // Start the chunk ModWarpTarget names, through the game's own level-entry path (a saved level's)
    ModActionWarp = 1,
    // Back to the title; the credits
    ModActionTitle = 2,
    ModActionCredits = 3,
};
u32 ModPageRowChosen(u32 mod, u32 row);
// After ModActionWarp: the chunk's path (as a save names it, "levels\\earth\\cavern\\cavent") and the character played there
// (GameProgress's; NoCharacter leaves it to the chunk's start script)
void ModWarpTarget(char* chunk, u32 size, u32* character);

// TwinsanityEvolved: the cutscene the player skipped (the cross button while watching) run ahead of the screen and the
// speakers until it is over -- the platform ends it once the game stops watching. The PS2: nothing
void FastForwardCutscene();

struct DateTime
{
    u16 year;
    // From 1
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
};

// The local date and time
DateTime LocalTime();
}
