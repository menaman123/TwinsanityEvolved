// platform/nativeee (host side) -- the LEVEL SELECT mod: Options > Mods > "level select" is ONE row that opens the list at
// once (no switch, no setup step: the mod has no off / on -- opening the list is what counts it as on for the session, so the
// saves made from then on go to the -mods card), the completion map's 26 rows by display name. A
// chosen row answers ModActionWarp with the row's chunk and character: the game
// starts it through its OWN saved-level path (GameController::WarpToChunk -- the mechanism tools/warp_save.py drives from a
// card: the progress taken as a save would, the chunk and the character written over it, StateLoadingLevel, the save
// controller restored at the load). The title row quits to the title (the game's QuitToTitle), the credits row plays the
// credits (PlayCredits). Characters as the entry fixtures name them (fixtures/padlogs/entries/*: `# character=`): the
// chunk's start script decides (NoCharacter) except where the fixture says Crash.
#include <string>

#include "te_mods.h"

namespace te::mods {

namespace {

constexpr uint32_t kNoCharacter = 6, kCrash = 0;

struct Row {
    const char* name;      // the display name (lower case: the game's menu font is case-free in style)
    const char* chunk;     // "" = not a chunk: title / credits
    uint32_t character;
    Action action;
};

// The level list's rows 0..25, in order
const Row kRows[] = {
    {"title screen", "", kNoCharacter, ActionTitle},                                              // 0: boot / title / main menu
    {"n. sanity island", "levels\\earth\\hub\\beach", kNoCharacter, ActionWarp},                 // 1: the hub
    {"jungle bungle", "levels\\earth\\hub\\huba", kNoCharacter, ActionWarp},                     // 2
    {"cavern catastrophe", "levels\\earth\\cavern\\cavent", kNoCharacter, ActionWarp},           // 3
    {"boss: mecha-bandicoot", "levels\\earth\\hub\\bossarea", kNoCharacter, ActionWarp},         // 4
    {"totem hokum", "levels\\earth\\totem\\l03beach", kNoCharacter, ActionWarp},                 // 5
    {"boss: tikimon / cortex's throne", "levels\\earth\\cavern\\cortthro", kNoCharacter, ActionWarp},   // 6
    {"iceberg lab", "levels\\ice\\hub\\labext", kNoCharacter, ActionWarp},                       // 7: the hub
    {"ice climb", "levels\\ice\\iceclimb\\caveent", kNoCharacter, ActionWarp},                   // 8
    {"slip slide icecapades", "levels\\ice\\slipslide\\l05start", kNoCharacter, ActionWarp},     // 9
    {"high seas hi-jinks", "levels\\ice\\highseas\\gpa01", kNoCharacter, ActionWarp},            // 10
    {"boss: dingodile", "levels\\school\\boiler\\boiler_6", kNoCharacter, ActionWarp},           // 11 (the boiler room's arena)
    {"boss: uka uka", "levels\\ice\\iceclimb\\ukafight", kNoCharacter, ActionWarp},              // 12
    {"academy of evil", "levels\\school\\sch_hub\\sch_hub", kNoCharacter, ActionWarp},           // 13: the hub
    {"boiler room doom", "levels\\school\\boiler\\boiler_1", kNoCharacter, ActionWarp},          // 14
    {"classroom chaos", "levels\\school\\crash\\crashent", kNoCharacter, ActionWarp},            // 15
    {"rooftop rampage", "levels\\school\\rooftop\\roof01", kNoCharacter, ActionWarp},            // 16
    {"boss: madame amberly", "levels\\school\\madame\\amberly", kNoCharacter, ActionWarp},       // 17
    {"twinsanity island", "levels\\altearth\\hub\\alta", kNoCharacter, ActionWarp},              // 18: the hub
    {"rockslide rumble", "levels\\altearth\\rockslid\\l10start", kNoCharacter, ActionWarp},      // 19
    {"bandicoot pursuit", "levels\\altearth\\hub\\altdoc", kNoCharacter, ActionWarp},            // 20
    {"ant agony", "levels\\altearth\\core\\corea", kNoCharacter, ActionWarp},                    // 21
    {"10th dimension", "levels\\altearth\\lab\\labext", kNoCharacter, ActionWarp},               // 22
    {"gone a bit coco", "levels\\ice\\hub\\ptexit", kCrash, ActionWarp},                         // 23 (tentative chunk; Crash stands in psycho)
    {"boss: evil twins", "levels\\altearth\\core\\throne", kNoCharacter, ActionWarp},            // 24
    {"credits", "", kNoCharacter, ActionCredits},                                                   // 25
};
constexpr uint32_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
static_assert(kRowCount == 26, "the completion map has 26 rows");

}  // namespace

const char* LevelSelectChunk(uint32_t row) { return row < kRowCount ? kRows[row].chunk : ""; }
const char* LevelSelectName(uint32_t row) { return row < kRowCount ? kRows[row].name : ""; }
uint32_t LevelSelectRows() { return kRowCount; }

void RegisterLevelSelect() {
    Mod m;
    m.id = "level_select";
    m.name = "level select";
    m.version = "1.0.0";
    m.has_switch = false;             // always available: the row opens the list, the list open = the mod on
    m.page_title = "level select";
    m.page_rows = [] { return kRowCount; };
    m.page_row_label = [](uint32_t row) { return std::string(row < kRowCount ? kRows[row].name : ""); };
    m.page_row_chosen = [](uint32_t row, WarpTarget& warp) {
        if (row >= kRowCount) return ActionNone;
        warp.chunk = kRows[row].chunk;
        warp.character = kRows[row].character;
        return kRows[row].action;
    };
    Register(m);
}

}  // namespace te::mods
