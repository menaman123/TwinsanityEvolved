#pragma once
// The importer's insides, shared by mod_import.cpp (detection, the import / inspect / verify flows) and mod_changes.cpp
// (an artefact's changes against the disc, and the mod folder written from them). Not part of the library's interface.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/host/mod_import/archive.h"
#include "app/host/mod_import/elf_diff.h"
#include "app/host/mod_import/mod_import.h"
#include "engine/cdvd/iso_fs.h"

namespace te::modimport::detail {

// The sampled hash the importer records for a recipe's base (the game recomputes it at start-up: DiscOverlay::SampledSha256)
constexpr uint32_t kSampleWindows = 64;
constexpr uint64_t kSampleWindowBytes = 64u << 10;

std::string Lower(std::string s);
std::string Upper(std::string s);
std::string Trim(const std::string& s);
std::string FileName(const std::string& path);   // the last component of a host path
std::string Gb(uint64_t bytes);                  // "2.6 GB"

// A disc image opened read-only: its files by disc path (upper case, '/' separators, no ";1")
struct DiscFile {
    std::string path;
    uint32_t lba = 0;
    uint64_t size = 0;
};
class Disc {
public:
    bool Open(const std::string& path, std::string* error);
    const DiscFile* Find(const std::string& disc_path) const;   // any case
    const std::vector<DiscFile>& Files() const { return files_; }
    const IsoFs& Iso() const { return iso_; }
    const std::string& Path() const { return path_; }
    const std::string& Executable() const { return executable_; }   // SYSTEM.CNF's BOOT2 program ("SLES_525.68"), "" when none
    const std::string& Region() const { return region_; }           // "PAL" | "NTSC-U" | "NTSC-J" | "any"
    bool ReadFile(const DiscFile& f, std::vector<uint8_t>* out) const;
    std::unique_ptr<ByteSource> Source(const DiscFile& f) const;

private:
    IsoFs iso_;
    std::string path_, executable_, region_;
    std::vector<DiscFile> files_;
    std::map<std::string, size_t> by_path_;
};

// The overall progress of one flow: stages with weights; Step(fraction of the stage) reports the whole's fraction
class Plan {
public:
    Plan(const Progress& p, std::vector<std::pair<std::string, double>> stages);
    void Enter(size_t stage);
    bool Step(double stage_fraction);   // false = cancelled
    bool Cancelled() const;
    const Progress& Callbacks() const { return progress_; }

private:
    const Progress& progress_;
    std::vector<std::pair<std::string, double>> stages_;
    size_t at_ = 0;
    double before_ = 0, total_ = 0, last_ = -1;
};

// The disc as the importer records it (mod.json base.disc), and its files' hashes as recipes need them (cached)
struct FileHashes {
    std::string sha256, sample_sha256;
};
class DiscRecord {
public:
    // The image's size and SHA-1 (one pass over the image file; cached in <cache_dir>/.import-cache.json by path, size and
    // modification time) and its Redump name when it is a dump Redump lists
    bool Identify(const Disc& disc, const std::string& cache_dir, Plan* plan, std::string* error);
    // A disc file's SHA-256 and sampled SHA-256 (cached alike)
    bool Hashes(const Disc& disc, const DiscFile& f, Plan* plan, FileHashes* out, std::string* error);
    uint64_t size = 0;
    std::string sha1, redump;   // redump "" = not a dump Redump lists

private:
    bool Load();
    void Save() const;
    std::string cache_path_, key_;
    std::map<std::string, FileHashes> files_;
    bool loaded_ = false;
};

// One file of the mod to write
struct OutFile {
    enum class Form { Replace, Add, Ranges };
    std::string path;              // the disc path
    Form form = Form::Replace;
    ByteSource* whole = nullptr;   // Replace / Add: its bytes
    Recipe recipe;                 // Ranges
    const DiscFile* base = nullptr;   // the disc's file it replaces (Replace / Ranges)
};

// What an artefact changes on the disc
struct ChangeSet {
    std::string name, author, version, description;
    std::vector<std::string> notes;                   // a crate's '!' lines (Crate Mod Loader's comments: the author's notes)
    std::vector<std::string> game_code_settings;      // what those notes ask Crate Mod Loader to apply ("Classic Slide Jump for Crash")
    std::vector<std::shared_ptr<Disc>> images;        // images the sources read from (a patched image)
    std::vector<std::unique_ptr<ByteSource>> owned;   // every source the files read from
    std::vector<OutFile> files;
    std::vector<std::string> removed, warnings, unchanged;
    bool executable_changed = false;
    ExecutableDiff executable;
    ArchiveDiffStats archive;
    bool archive_changed = false;
    ByteSource* Own(std::unique_ptr<ByteSource> s) {
        owned.push_back(std::move(s));
        return owned.back().get();
    }
};

// The changes of each kind of artefact (mod_changes.cpp). `plan` is in its comparing stage.
bool ChangesFromImage(const Disc& base, const std::string& image_path, ChangeSet* out, Plan* plan, std::string* error);
bool ChangesFromCrate(const Disc& base, const std::string& crate_dir, ChangeSet* out, Plan* plan, std::string* error);
// Loose files: a folder of them, or one file (`path` a file): each a file of the disc (by its path, else by its name), else an
// entry of the disc's archive (by its index path, else by its last name: a unique match), else an added file in a folder
// the disc has
bool ChangesFromFolder(const Disc& base, const std::string& path, ChangeSet* out, Plan* plan, std::string* error);

// The mod folder from a change set (mod_changes.cpp): mod.json, disc/, data/ under `dir` (which must not exist yet)
struct SourceRecord {
    std::string kind, artefact, sha256;
};
bool WriteModFolder(const ChangeSet& changes, const Disc& base, DiscRecord* record, const std::string& dir, const std::string& id,
                    const SourceRecord& source, Plan* plan, Report* report, std::string* error);

// A Mod Crate's ModCrateInfo.txt (key=value lines, '!' comments): the keys lower-cased
std::map<std::string, std::string> ReadCrateInfo(const std::string& text);
// Its '!' lines (Crate Mod Loader's comment symbol; authors write the player's instructions there), in order, trimmed
std::vector<std::string> ReadCrateNotes(const std::string& text);
// The Crate Mod Loader settings those notes ask the player to apply: "For the mod to work, apply Classic Slide Jump for Crash
// and Classic Explosion Damage." -> {"Classic Slide Jump for Crash", "Classic Explosion Damage"} (the words after "apply" up
// to the sentence's end, split at commas and "and"; each must start with a capital letter or a digit)
std::vector<std::string> RequestedSettings(const std::vector<std::string>& notes);
// The crate's root inside a folder: the folder holding modcrateinfo.txt (the folder itself or its only sub-folder), "" when none
std::string CrateRoot(const std::string& dir, bool* has_layers);

// The releases whose archive index the importer knows (launcher/discs/archives/*.json, built in by mod_import.cmake: every
// entry's name and size, no byte of the game): what tells a loose entry made from another release
struct ReleaseArchive {
    std::string serial, executable, region, version, provenance;
    bool retail = false;
    std::map<std::string, std::pair<std::string, uint32_t>> entries;   // ArchiveKey(name) -> {the name as recorded, size}
};
const std::vector<ReleaseArchive>& KnownArchives();
// The player's words for a region's disc: "the European disc", "the North American disc", "the Japanese disc", "another disc"
std::string RegionDisc(const std::string& region);

}  // namespace te::modimport::detail
