// te_mods.h -- the port's built-in MOD framework. A mod is a C object registered at start-up (compiled in; its
// manifest `mods/<id>/mod.json` -- id, name, version, hooks, settings -- embedded by CMake and checked against the
// registration), switched on and off from the title's own Options > MODS page (the seam: Platform::System::Mod*), all OFF
// by default, each switch persisted as `mods.<id>.enabled` in ps2r.ini. While ANY mod is on, the memory card the game
// writes is the mod-namespaced card (<mc_dir>-mods, seeded from the vanilla card the first time) -- a modded save never
// touches the vanilla card (te::Saves).
//
// Hook points (the C hook API; a Lua runtime is deferred): every frame boundary (the game controller's state, the
// player's chunk, paused), a level entered / left (the player's chunk path changed: the chunk name the trace logs), the
// menu built (the Options page asks for the rows), a card save / load (te::Saves' Write / Read). A mod may also offer a
// PAGE (Mods > <title>): rows the player chooses, answered with a ModAction the game performs (a warp). A mod without a
// switch (Mod::has_switch false) IS its page: one row on the Mods page opens it and counts the mod as on for the session.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class IsoFs;   // engine/cdvd/iso_fs.h (the disc's volume: MakeDiscVolume)

namespace te::mods {

enum class Hook : uint32_t { Frame = 0, LevelEnter, LevelExit, MenuBuild, Save, Load, Count };
const char* HookName(Hook hook);

// What the frame hook sees (read from the game at the boundary; see mods.cpp)
struct FrameContext {
    uint64_t boundary = 0;
    unsigned state = 0;          // GameController::State (12 = Playing, 15 = Paused, 9 = LoadingLevel ...)
    bool paused = false;         // the pause menu / a notice is up (state Paused)
    bool playing = false;        // state Playing or Watching (the in-game clock runs)
    std::string chunk;           // the player's chunk path ("levels\\earth\\hub\\huba"; "" when there is no player)
};

// The actions the game performs for a chosen page row (include/platform/system.h ModAction)
enum Action : uint32_t { ActionNone = 0, ActionWarp = 1, ActionTitle = 2, ActionCredits = 3 };
struct WarpTarget { std::string chunk; uint32_t character = 6; };   // GameProgress::NoCharacter = 6

struct Mod {
    std::string id;        // [a-z0-9_]+ -- the ps2r.ini key mods.<id>.enabled and the manifest folder mods/<id>/
    std::string name;      // the Mods page row (lower case like the game's menu texts)
    std::string version;
    // Whether the Mods page shows an off / on switch for it (persisted). A mod WITHOUT one is always available: its row on
    // the Mods page opens its page at once and that turns it on for the SESSION (never persisted: the next boot starts
    // vanilla); ps2r.ini's mods.<id>.enabled is not read for it. --mod <id> still turns it on for a run.
    bool has_switch = true;
    // The C hooks (any may be null)
    std::function<void(bool on)> on_enable;                        // the switch changed (also at start-up when on)
    std::function<void(const FrameContext&)> on_frame;
    std::function<void(const FrameContext&, const std::string& chunk)> on_level_enter;
    std::function<void(const FrameContext&, const std::string& chunk)> on_level_exit;
    std::function<void()> on_menu_build;
    std::function<void(const std::string& save, const std::string& file)> on_save;
    std::function<void(const std::string& save, const std::string& file)> on_load;
    // The mod's page (none when page_rows is null or answers 0)
    std::string page_title;
    std::function<uint32_t()> page_rows;
    std::function<std::string(uint32_t row)> page_row_label;
    std::function<Action(uint32_t row, WarpTarget& warp)> page_row_chosen;
};

// Registration (the built-in mods, at start-up before the module runs). RegisterBuiltins only registers (no service is touched:
// the runner may call it before te::SetSettings to learn the ids whose ps2r.ini keys to read); InstallBuiltins registers and
// installs the frame hook on te::Clock (after te::SetSettings: constructing the services reads the settings).
void Register(const Mod& mod);
void RegisterBuiltins();
void InstallBuiltins();
uint32_t Count();
const Mod* At(uint32_t index);
const Mod* Find(const std::string& id);
// The switches: read from ps2r.ini by the runner (Configure), toggled by the menu (SetEnabled: applied + persisted),
// forced for one run (--mod <id>, never persisted).
void Configure(const std::vector<std::string>& enabled_ids);
bool Enabled(uint32_t index);
bool Enabled(const std::string& id);
void SetEnabled(uint32_t index, bool on, bool persist = true);
bool AnyEnabled();
// Whether the registry's mod `index` applies WHILE THE GAME RUNS: a built-in (its hooks act at once). A folder mod's disc
// layers are composed when the game starts, so it is switched before the game starts (the launcher), never on the title's
// Options > MODS page, which lists exactly these (te_mods_page.h)
bool AppliesLive(uint32_t index);
// The manifests embedded at build time (mods/<id>/mod.json): the text of one, "" when none; every registered mod must have one
// whose id / name / version agree (checked at InstallBuiltins, logged).
std::string Manifest(const std::string& id);
// One line for the log.
std::string Describe();

// The hook sources call these
void OnBoundary(uint64_t boundary);            // te::Clock's boundary hook (installed by InstallBuiltins)
void OnMenuBuild();                            // Platform::System::ModCount (the Options page being built)
void OnSave(const std::string& save, const std::string& file);
void OnLoad(const std::string& save, const std::string& file);
const FrameContext& LastFrame();

// ---- FOLDER MODS: <mods>/<id>/mod.json (the format is fixed in the stage protocol) -------------------------
// The mods folder (ps2r.ini paths.mods, default <the folder that holds ps2r.ini>/mods; --mods-dir; the Vita
// ux0:data/twinsanity/mods) holds one sub-folder per mod: mod.json (schema 1 or 2), disc/<DISC PATH> (a whole file laid out
// like the disc, or <DISC PATH>.ranges: a recipe of base / file / zero segments) and data/ (the payload the recipes name).
// ScanFolder registers every valid folder of kind "disc" BESIDE the built-ins: a registry entry with a switch
// (mods.<id>.enabled: the launcher's Mods page writes it, the runner reads it at start-up -- the title's Options > MODS page
// lists the built-ins only and writes nothing for a folder mod), no hooks. An enabled folder mod counts for the
// "-mods card" rule (AnyEnabled). The disc layers (enabled folder mods that have a disc/ tree, in mods.order) are what
// te::Disc composes over the image at its open (engine/cdvd/disc_overlay.h): a switch the launcher changes while the game
// runs takes effect at the next start. A folder of kind "language" is a LANGUAGE PACK, not a mod: it is kept apart
// (LanguagePacks) -- no registry entry, so no row on the Mods page, no switch (a stale mods.<id>.enabled is ignored), no
// place in mods.order, no -mods card, nothing in the census row -- and the language table offers it as a text language
// (te_language_mod.h).
enum class Kind : uint32_t { Builtin = 0, Disc = 1, Language = 2 };
const char* KindName(Kind kind);   // "builtin" | "disc" | "language"

// A manifest files[] row ("ranges" | "replace" | "add"). For a recipe the importer also records what it was made against
// (base_bytes, base_sha256, base_sample_*: the disc's file): the overlay refuses the layer when
// the file below differs (engine/cdvd/disc_overlay.h BaseCheck)
struct FileRow {
    std::string path, form;
    uint64_t bytes = 0;
    uint64_t base_bytes = 0;            // 0 = not recorded (no check)
    std::string base_sha256, base_sample_sha256;
    uint32_t base_sample_windows = 0;
    uint64_t base_sample_window_bytes = 0;
};
struct SourceInfo { std::string kind, artefact, sha256; };                // where the mod came from ("iso" | "xdelta" | ... | "folder" | "repo")
struct BaseInfo { std::string region, executable; };                      // the disc it applies to ("PAL" | "NTSC-U" | "NTSC-J" | "any")
struct LanguageInfo { std::string id, label, text_bank, font, direction; };   // kind language: paths relative to the mod's folder

// One row of List(): a built-in (from its embedded manifest) or a folder mod (from its mod.json)
struct Info {
    std::string id, name, author, version, description;
    Kind kind = Kind::Builtin;
    uint32_t schema = 1;          // the manifest's "schema" (1 when absent: the built-ins' manifests)
    bool folder = false;          // a folder mod (else compiled into the game)
    std::string dir;              // its folder, "<mods>/<id>" ("" for a built-in)
    bool enabled = false;         // on now (ps2r.ini mods.<id>.enabled, --mod <id>, the Options menu)
    bool forced = false;          // on for this run only (--mod <id>, or a switchless mod opened this session)
    bool has_switch = true;
    bool has_disc = false;        // a folder mod with a disc/ tree: an overlay layer when it is on at start-up
    int32_t layer = -1;           // its place in the disc overlay composed at start-up (0 = applied first), -1 = none
    SourceInfo source;
    BaseInfo base;
    std::vector<FileRow> files;   // the manifest's files[] (informational: the disc/ tree is what is applied)
    LanguageInfo language;
    // "executable": { "data_changes": {".data": N, ...}, "code_changes": ["FUN_... name", ...] } (a modded executable)
    std::vector<std::pair<std::string, uint64_t>> data_changes;
    std::vector<std::string> code_changes;
    std::string manifest;         // the mod.json text
};
// Every registered mod, built-ins first (registration order), then the folder mods (sorted by id)
std::vector<Info> List();
const Info* FindInfo(const std::string& id, Info* storage);   // one row by id (copied into *storage), nullptr when unknown
// The LANGUAGE PACKS of the mods folder (mod.json kind "language", by id): translations the language table offers as text
// languages (te_language_mod.h) -- not mods: no switch, no place on the Mods page or in mods.order, never the -mods card
std::vector<Info> LanguagePacks();
// A language pack's stale lines in a settings file -- `mods.<pack>.enabled` (an earlier version's switch) and its place in
// `mods.order` -- dropped (the file is written only when something went)
void DropPackKeys(const std::string& settings_path);

// Scans `mods_dir` for <id>/mod.json and registers each valid disc folder (after InstallBuiltins, before Configure), keeping
// the language packs apart (LanguagePacks). A folder whose manifest is missing, malformed, names another id, has an id
// outside [a-z0-9_]+, a kind other than disc / language, or an id a built-in already has is skipped with one log line. Returns
// the folder mods registered (the language packs not counted); "" or an absent folder: 0. A second call (a test) forgets the
// previous folder's mods and packs first.
uint32_t ScanFolder(const std::string& mods_dir);
const std::string& Folder();   // the folder ScanFolder last read ("" when none)
// The overlay order: ps2r.ini mods.order (comma- or space-separated ids; later wins). Enabled folder mods it does not name
// follow it, sorted by id; unknown ids are ignored (logged).
void SetOrder(const std::string& order);
std::vector<std::string> Order();   // the order in force (every folder mod's id, overlay order)

// The enabled folder mods with a disc/ tree, in overlay order: what te::Disc composes at its open
struct DiscLayer { std::string id, dir; };
std::vector<DiscLayer> DiscLayers();
// The disc's overlay reports here when it is composed (te::Disc at its open): the layers applied (their ids), the disc files
// they replace or add, and where the reads served from it are counted (asked each time CensusRow runs). A layer the
// overlay refused (a broken recipe) is not in `applied`.
void NoteOverlay(const std::vector<std::string>& applied, uint32_t overrides, std::function<uint64_t()> hits);

// The disc's region as its identity says ("PAL" | "NTSC-U" | "NTSC-J"; "" unknown), set by the runner before the disc's
// volume is made: a layer the overlay leaves out names it when the mod was made for another region's disc (the start-up log
// line; the game draws nothing about a folder mod -- the launcher's row says "Left out on this disc")
void SetDiscRegion(const std::string& region);
const std::string& DiscRegion();
// "the European disc" / "the North American disc" / "the Japanese disc" for a region; "" for "any" or one not known
std::string RegionDisc(const std::string& region);
// A folder mod made for another region's disc than this one (both known, and they differ)
bool OtherRegion(const std::string& mod_region);

// One line for the boot log: "mods: 1 folder (hubtest disc on), order: hubtest; language packs (not mods): arabic"
std::string DescribeFolders();
// The census row: "mods=<layers>:<overrides>:<hits>" -- the overlay's layers applied at start-up, the disc files they replace
// or add, the reads served from an overlay so far ("mods=0:0:0" with no overlay)
std::string CensusRow();
// te::Disc's volume (mods_disc.cpp): the image as it is -- a plain IsoFs, exactly the disc layer without mods -- when
// DiscLayers() is empty, else the image with those layers composed over it (engine/cdvd/disc_overlay.h), which at its open
// logs what it composed and reports to NoteOverlay. Decided when te::Disc is constructed (after the runner configured the
// mods): a switch changed later applies at the next start.
std::unique_ptr<IsoFs> MakeDiscVolume();
// The region loader's files (runner/te_region.cpp: a region map's "names"): disc files computed in memory from
// the volume below them -- the image with the mods composed -- as ONE last layer `layer_id` (the mods' recipes keep
// seeing the disc's own files, so their base checks hold). Set before te::Disc is constructed; none: nothing changes (a
// PAL disc never has any). Not a mod: the census row counts the mods only.
using ComputeFile = std::function<bool(const std::vector<uint8_t>& below, std::vector<uint8_t>* out, std::string* why)>;
void SetRegionFiles(std::string layer_id, std::vector<std::pair<std::string, ComputeFile>> files);
// Reads `relpath` (relative to a folder mod's or a language pack's own folder, '/' separators; no "..", not absolute) whole.
// False when the id is unknown / not a folder's, the path escapes the folder, or the file does not read.
bool ReadModFile(const std::string& id, const std::string& relpath, std::vector<uint8_t>& out, std::string* error = nullptr);

}  // namespace te::mods
