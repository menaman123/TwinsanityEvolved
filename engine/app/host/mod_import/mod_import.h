#pragma once
// te::modimport -- a community mod, as it is shipped, into one folder of the game's mods folder.
//
// What it takes (each recognised by its CONTENT, never by its name), and what it becomes (<mods>/<id>/: mod.json schema 2,
// disc/<DISC PATH> whole files, disc/<DISC PATH>.ranges recipes over the player's disc, data/ the bytes they name):
//   ModCrate    a Crate Mod Loader Mod Crate, a .zip or a folder (ModCrateInfo.txt; layer0/ = disc files, layer1/ = entries
//               of CRASH6/CRASH.BD by their index path): layer0 files -> disc/ files; layer1 -> a rebuilt CRASH6/CRASH.BH and a
//               CRASH6/CRASH.BD.ranges recipe. A crate with ModCrateSettings.txt (Crate Mod Loader's game-code properties) or
//               scripts is refused: those are operations of Crate Mod Loader's own code, not files.
//   PatchedIso  a whole modified disc image (.iso, or a raw .bin): compared with the player's disc file by file; only the
//               files that differ are kept, CRASH6/CRASH.BD through both archive indexes (entry by name) so an archive whose
//               entries moved is still mostly ranges of the player's disc.
//   Xdelta, Ppf, Bps  a binary patch against a disc image (patches.h): applied to a temporary copy of the player's disc
//               under <mods>/.import-tmp (its free space checked first; the copy deleted on every way out), then as PatchedIso.
//   Folder      loose files: laid out like the disc (SLES_525.68, CRASH6/..., FMV/...), or matched to the disc's files by
//               name (two disc files of one name: not imported, said in the warnings). A zip of such files is a Folder too,
//               and a folder or zip holding one patch or image (and notes) is that patch or image.
// A changed SLES_525.68 is kept whole (the game reads its data image from it); its code changes are listed by function
// (Report::code_changes, mod.json executable.code_changes) and never run (elf_diff.h).
//
// Every recipe records what it was made over (files[].base_bytes / base_sha256 / base_sample_*): the game refuses the mod's
// disc layer at start-up when the file below is another one (engine/cdvd/disc_overlay.h BaseCheck). The player's disc
// itself is recorded as mod.json base.disc {size, sha1, redump}. Nothing of the disc is copied into the mod but the bytes a
// mod changes; the disc image is only read.
//
// Plain C++17, no Qt: the launcher, the command line tool (twinsanityevolved-modtool) and the judges share it. Every
// function may run on a worker thread (no shared state); Progress callbacks are called on that thread.
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace te::modimport {

enum class Kind { ModCrate, PatchedIso, Xdelta, Ppf, Bps, Folder, Unknown };
// mod.json's source.kind for a Kind: "modcrate" | "iso" | "xdelta" | "ppf" | "bps" | "folder" ("" for Unknown)
const char* SourceKindName(Kind kind);
// The player's words for it: "a Mod Crate", "a modified disc image", "an xdelta patch", "a PPF patch", "a BPS patch",
// "a folder of files", "not a mod"
const char* KindLabel(Kind kind);

// What the artefact (a file or a folder) is, by its content. *why: one sentence ("a Mod Crate (zip) named Cave Remix",
// or, for Unknown, why it is not taken: "a RAR archive: extract it first, then add what it holds").
Kind Detect(const std::string& artefact_path, std::string* why);

struct Progress {
    // stage: what is happening, for the player ("Reading your disc", "Applying the patch", "Comparing with your disc",
    // "Writing the mod"); fraction: of the whole import, 0..1, never decreasing
    std::function<void(const std::string& stage, double fraction)> report;
    // true = stop as soon as possible (the import then fails with "The import was stopped." and leaves nothing behind)
    std::function<bool()> cancelled;
};

struct Report {
    std::string id, name, mod_json_path;
    std::vector<std::string> files_changed, files_added, warnings;   // disc paths ("CRASH6/CRASH.BD"); sentences
    uint64_t bytes_written = 0;                                      // the mod folder's size
    std::vector<std::string> code_changes;   // "FUN_001ac8d0 Font::ConstructBase": the executable's changed functions
    bool code_changes_unsupported = false;   // there are code changes (they cannot apply in this port)
    bool replaced = false;                   // a mod of this id was in the mods folder and this one took its place
};

// artefact -> <mods_dir>/<id>/ (an overlay folder  + mod.json schema 2). The base disc is the player's
// (its identity is recorded as the mod's base); the disc is never copied whole into the mod. Temporary files live under
// <mods_dir>/.import-tmp and are removed on every way out. The id is the mod's name's (ModIdFor): ONE RULE when the mods
// folder has a mod of that id -- this one replaces it, once it is complete (Report::replaced; the launcher asks the player
// first, the command line tool does not); a folder of that name that is not a mod stops the import. False with *error (a
// sentence for the player) when the artefact is refused or the import fails; the mods folder is then as it was.
bool Import(const std::string& artefact_path, const std::string& base_iso_path, const std::string& mods_dir, const Progress& progress,
            Report* report, std::string* error);

// ---- beyond the import itself (the command line tool's inspect / verify; the launcher may show them) ---------------------

// The id (and *name) Import would give the artefact's mod, without importing: a crate's Name from its ModCrateInfo.txt (read
// from the zip without unpacking it), a mod folder's own id, else the artefact's file or folder name; "" when it is not a mod.
// Cheap (no disc, nothing compared): what the launcher asks about before it replaces a mod of that id.
std::string ModIdFor(const std::string& artefact_path, std::string* name);

// What Import would do, with nothing written. A patch is checked against the disc as far as its format allows without
// applying it (BPS: the size; PPF: the image check); which files it changes is known once it is applied (needs_apply).
struct Inspection {
    Kind kind = Kind::Unknown;
    std::string detected;                     // Detect's sentence
    std::string name, author, version, description;
    std::vector<std::string> notes;           // a Mod Crate's '!' lines (the author's notes to the player)
    bool importable = false;                  // false: `refusal` says why
    std::string refusal;
    std::vector<std::string> files_changed, files_added, files_removed, warnings;
    std::vector<std::string> archive_entries_changed, archive_entries_added;   // CRASH6/CRASH.BD entries by name
    std::vector<std::string> code_changes;
    std::vector<std::pair<std::string, uint64_t>> data_changes;   // {".data", N}, {".rodata", N}, {".sdata", N}, {"vu", N}
    bool needs_apply = false;                 // a patch: its files are known once applied
    uint64_t temporary_bytes = 0;             // the temporary space Import needs beside the mods folder
    std::string patch_format;                 // "xdelta3 (VCDIFF)", "PPF 3.0", "BPS"
};
bool Inspect(const std::string& artefact_path, const std::string& base_iso_path, const Progress& progress, Inspection* out,
             std::string* error);

// A mod folder recomposed over the disc through the game's own overlay (engine/cdvd/disc_overlay.h): every files[] row's
// composed size and sha256 against what mod.json records, the base checks, and the overlay's own refusal if any.
struct VerifyRow {
    std::string path, form;
    uint64_t bytes = 0, recorded_bytes = 0;
    std::string sha256, recorded_sha256;      // recorded "" = not recorded (size only)
    bool ok = false;
    std::string note;
};
struct Verification {
    std::string id;
    bool ok = false;
    std::vector<VerifyRow> files;
    std::vector<std::string> problems;        // sentences (the overlay's refusal among them)
};
bool Verify(const std::string& mod_dir, const std::string& base_iso_path, const Progress& progress, Verification* out,
            std::string* error);

}  // namespace te::modimport
