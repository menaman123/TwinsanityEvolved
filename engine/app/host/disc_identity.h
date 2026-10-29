#pragma once
// DiscIdentity -- what a disc image IS, judged by its content and never by its file name: the PlayStation 2 program
// SYSTEM.CNF boots, the region its serial names, and whether the image IS a known retail dump. Plain C++ (no Qt): the
// launcher identifies every image it finds, the game logs the identity at boot.
//
// RETAIL MEANS ONE THING: the whole image -- its size, CRC-32 and SHA-1 -- matches a dump in the
// table of known retail dumps (launcher/discs/redump.json, written from redump.org's disc pages, every entry cited).
// Nothing else makes an image "retail": not a file list made from some disc, not matching sizes.
//
//   1. The image is opened with the engine's IsoFs (a plain 2048-byte-sector .iso or a raw 2352-byte .bin); not ISO 9660
//      = refused.
//   2. Every file is listed. SYSTEM.CNF's BOOT2 line names the program ("cdrom0:\SLES_525.68;1"; VER and VMODE are kept);
//      without SYSTEM.CNF the root's serial-shaped file (ABCD_123.45) stands in. The program's ELF header gives the entry.
//   3. The region is the serial prefix's (SLES / SCES / SCED / SLED = PAL, SLUS / SCUS = NTSC-U, SLPM / SLPS / SCPS /
//      SCAJ / SLAJ = NTSC-J), else VMODE = PAL, else unknown.
//   4. A serial the table lists as NOT the game (the demo discs) is refused; the title's CORE files (the file lists'
//      `core` rows) must be on the disc, else refused with the files named.
//   5. The image against the table: the dumps of the same serial are compared by size first (decisive when it differs:
//      no dump has that size), then -- when IdentifyOptions::hash_image -- by CRC-32 and SHA-1 over the whole image (the
//      file of a 2048-byte image; the user data of a raw one, which is what a DVD dump holds).
//        retail          a dump matched: size, CRC-32 and SHA-1
//        retail by size  the size of a dump of this serial and its retail program, the image not hashed (the game's boot
//                        without --verify-disc)
//        modified        the program IS a known retail executable (its sha256 in the table's `programs`: the release is
//                        certain), but the image is not one of the dumps -- a release's disc with other files
//        unknown         the core files are there, but the program is no known retail executable (an unknown serial, or a
//                        known one with other bytes: another revision, a modified program, a made-up image) -- "unknown but
//                        plausible": accepted, `warning` says so
//   6. The files that differ are NAMED only against a file list of RETAIL PROVENANCE (a launcher/discs/<id>.json whose
//      "retail" is true: made by tools/discs/manifest.py from an image that matched the table). A list of other provenance
//      (launcher/discs/pal.json: the modified PAL image used for development) supplies the core file names and stays a list of
//      "this disc's files" for the mods importer; nothing is ever called different from it.
//
// The functions return true when the disc is ACCEPTED (retail, retail by size, modified, unknown) and false when it is
// refused, with *error a sentence a player can read ("It is not a disc image..."); *out is filled as far as it got.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps2r::app {

struct DiscIdentity {
    enum class Region { Unknown, PAL, NTSC_U, NTSC_J };
    enum class Verdict { Refused, Retail, RetailBySize, Modified, Unknown };
    Region region = Region::Unknown;
    Verdict verdict = Verdict::Refused;
    std::string executable;                    // the program SYSTEM.CNF boots: "SLES_525.68"
    std::string volume_id;                     // the primary volume descriptor's identifier: "CRASH"
    std::string serial;                        // "SLES-52568" (empty when the program is not serial-shaped)
    bool file_set_ok = false;                  // the title's core files are on the disc
    bool retail = false;                       // verdict == Retail: the whole image is a known retail dump
    std::vector<std::string> differing_files;  // named against a RETAIL-provenance list only: size or sha256 differs
    std::vector<std::string> missing_files;    // in that list (or core, without one) but not on the disc
    std::string warning;                       // set when the disc is accepted with a doubt (an unknown program)
    uint64_t bytes = 0;                        // the volume's logical bytes
    bool raw = false;                          // a raw 2352-byte-sector image
    std::vector<std::string> added_files;      // on the disc, not in that list
    std::string title;                         // the title's name ("Crash Twinsanity"), else empty
    std::string manifest;                      // the file list found for this program ("pal"), whatever its provenance
    std::string boot;                          // SYSTEM.CNF BOOT2 as written
    std::string version;                       // SYSTEM.CNF VER
    std::string video_mode;                    // SYSTEM.CNF VMODE
    uint32_t elf_entry = 0;                    // the program's ELF entry point (0 when it is not a 32-bit MIPS ELF)
    uint64_t elf_bytes = 0;
    uint32_t files = 0;                        // files on the disc
    uint32_t files_hashed = 0;                 // files whose sha256 was compared (against a retail-provenance list)
    uint64_t bytes_hashed = 0;
    bool fully_hashed = false;                 // every file present in that list was hashed

    // the image as a whole, against the table of known retail dumps
    bool known_program = false;                // the serial has dumps in the table: a release of the title
    bool known_executable = false;             // the program's bytes are a known retail executable (the table's programs)
    std::string program_sha256;                // the program's sha256 (64 hex digits; "" when it is not on the disc)
    uint64_t image_bytes = 0;                  // the bytes compared and hashed (the logical image)
    bool image_hashed = false;
    uint32_t image_crc32 = 0;
    std::string image_sha1;                    // 40 hex digits once hashed
    double image_seconds = 0;                  // how long the hash took
    uint32_t dump_id = 0;                      // the table entry: the match (retail, retail by size) or the closest dump
    std::string dump_version;                  // its version ("1.01")
    std::string dump_edition;                  // its edition ("Original, Platinum, Crash Bandicoot Action Pack")
    uint64_t dump_bytes = 0;                   // its size: the one the image was compared with
    uint32_t program_dumps = 0;                // how many dumps of this serial the table has
    bool same_size = false;                    // the image has a dump's size
    // the file list compared for the differing files' names, and where that list came from
    bool list_retail = false;                  // a list of retail provenance was compared
    std::string list_provenance;               // the provenance of the program's list ("a modified PAL image used for development ...")
};

struct RetailFile {
    std::string path;     // "CRASH6/CRASH.BD": upper case, '/' separators, no ";1"
    uint64_t bytes = 0;
    std::string sha256;   // 64 lower-case hex digits
    bool core = false;    // a disc of the title cannot be played without it
};

// A disc's file list (tools/discs/manifest.py). `retail` is true only for a list made from an image that matched the
// table of known dumps; `provenance` says where it came from in words.
struct RetailManifest {
    std::string id;           // "pal"
    std::string title;        // "Crash Twinsanity"
    DiscIdentity::Region region = DiscIdentity::Region::Unknown;
    std::string serial;       // "SLES-52568"
    std::string executable;   // "SLES_525.68"
    std::string volume_id;    // "CRASH"
    uint32_t elf_entry = 0;
    bool retail = false;      // of retail provenance (the image matched a known dump when the list was made)
    std::string provenance;   // "a modified PAL image used for development (Redump mismatch)"
    std::vector<RetailFile> files;
};

// One dump of the table of known retail dumps (launcher/discs/redump.json).
struct KnownDump {
    uint32_t id = 0;              // the disc page's number (redump.org/disc/<id>/)
    std::string url;
    std::string title;            // "Crash Twinsanity"
    std::string serial;           // the program's serial "SLES-52568" (the key)
    DiscIdentity::Region region = DiscIdentity::Region::Unknown;
    std::string region_words;     // "Europe, Australia"
    std::string version;          // "1.01"
    std::string edition;          // "Original"
    std::string volume_id;        // "CRASH" when the page names it
    uint64_t sectors = 0;
    uint64_t bytes = 0;
    uint32_t crc32 = 0;
    std::string md5, sha1;        // lower-case hex
    bool game = true;             // false: a disc of another title or a demo (refused: `why` says what it is)
    std::string why;              // "a demo disc (Crash Twinsanity / Spyro: A Hero's Tail)"
};

// A known retail executable (the table's "programs": the program a release's SYSTEM.CNF boots, by its bytes).
struct KnownProgram {
    std::string serial;       // "SLES-52568"
    std::string executable;   // "SLES_525.68"
    uint64_t bytes = 0;
    std::string sha256;       // lower-case hex (the key)
    std::string source;       // where the hash comes from
};

// The kind of a disc table JSON: "files" (a file list), "redump" (known dumps), "region-map" (disc_regions.h), "" unknown.
std::string DiscTableKind(const std::string& json);

// Reads a file list (the JSON tools/discs/manifest.py writes). False with *error when it does not parse.
bool ParseRetailManifest(const std::string& json, RetailManifest* out, std::string* error);
bool LoadRetailManifest(const std::string& path, RetailManifest* out, std::string* error);
// The process-wide lists the identity uses (the launcher's from title.json, the game's built in).
void AddRetailManifest(const RetailManifest& manifest);
void ClearRetailManifests();
const std::vector<RetailManifest>& RetailManifests();

// The table of known retail dumps (launcher/discs/redump.json): parse, load, register (process-wide), read.
bool ParseKnownDumps(const std::string& json, std::vector<KnownDump>* out, std::string* error);
bool LoadKnownDumps(const std::string& path, std::vector<KnownDump>* out, std::string* error);
void AddKnownDumps(const std::vector<KnownDump>& dumps);   // replaces entries of the same id
void ClearKnownDumps();   // the dumps and the programs
const std::vector<KnownDump>& KnownDumps();
const std::vector<KnownProgram>& KnownPrograms();
// Registers any disc table by its kind (files / redump); false with *error for one that does not read or another kind.
bool RegisterDiscTable(const std::string& json, std::string* error);
bool LoadDiscTable(const std::string& path, std::string* error);

// (bytes hashed so far, bytes to hash, what is being hashed: a disc path, or "" for the whole image) -> false stops it.
using IdentifyProgress = std::function<bool(uint64_t, uint64_t, const std::string&)>;

struct IdentifyOptions {
    bool hash_image = false;             // CRC-32 + SHA-1 of the whole image (the retail check; seconds for a DVD)
    uint64_t hash_below = 64ull << 20;   // with a retail-provenance list: files smaller than this are hashed
    bool hash_all = false;               // with a retail-provenance list: every file hashed
    IdentifyProgress progress;
};

// The directories, the program and the size against the table: no hashing (a few milliseconds).
bool IdentifyDisc(const std::string& path, DiscIdentity* out, std::string* error);
// The whole image hashed (and every file, against a retail-provenance list); progress per MB.
bool IdentifyDiscFully(const std::string& path, DiscIdentity* out, std::string* error, const IdentifyProgress& progress = {});
bool IdentifyDiscWith(const std::string& path, const IdentifyOptions& options, DiscIdentity* out, std::string* error);

const char* RegionName(DiscIdentity::Region region);           // "PAL", "NTSC-U", "NTSC-J", "unknown region"
const char* VerdictName(DiscIdentity::Verdict verdict);        // "retail", "retail-size", "modified", "unknown", "refused"
DiscIdentity::Region RegionOfSerial(const std::string& name);  // SLES_525.68 or SLES-52568 -> PAL
std::string SerialOf(const std::string& executable);            // "SLES_525.68" -> "SLES-52568"; "" when not serial-shaped
std::string Thousands(uint64_t n);                              // 2579234816 -> "2,579,234,816"
// The classification in a few words: "retail SLES-52568 v1.01 (Redump)", "modified PAL image (not a known retail dump;
// 20,971,520 bytes larger than SLES-52568's dump)", "unknown but plausible", "refused".
std::string ClassifyDisc(const DiscIdentity& identity);
// The game's boot line: "modified PAL image SLES-52568 (not the retail dump: 2,579,234,816 bytes vs 2,558,263,296;
// volume CRASH)", "retail PAL image SLES-52568 v1.01 (Redump disc 4514; volume CRASH)", ...
std::string DescribeDisc(const DiscIdentity& identity);

}  // namespace ps2r::app
