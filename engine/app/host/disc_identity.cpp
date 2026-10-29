// DiscIdentity implementation; the rules are described in the header.
#include "app/host/disc_identity.h"
#include "app/host/disc_regions.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "app/host/disc_image_hash.h"
#include "app/host/disc_json.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/core/hash/sha256.h"

namespace ps2r::app {
namespace {

constexpr int kMaxDepth = 16;             // directory nesting a disc may have before it is called damaged
constexpr size_t kMaxFiles = 65536;       // more files than this is not a game disc
constexpr size_t kChunk = 1u << 20;       // per-file hashing reads, and the progress step
constexpr size_t kImageChunk = 4u << 20;  // whole-image hashing reads
constexpr size_t kCnfMax = 4096;          // SYSTEM.CNF is a few lines
constexpr size_t kNamed = 3;              // files named in a one-line description

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string StripVersion(const std::string& id) {
    const size_t semi = id.find(';');
    return semi == std::string::npos ? id : id.substr(0, semi);
}

// A disc path as the file lists spell it: upper case, '/' separators, no "cdrom0:" prefix, no leading slash, no ";1".
std::string DiscPath(std::string p) {
    p = Trim(p);
    if (p.size() >= 7 && Upper(p.substr(0, 7)) == "CDROM0:") p = p.substr(7);
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    return Upper(StripVersion(p));
}

bool LooksLikeSerial(const std::string& name) {
    // ABCD_123.45: four letters, '_', three digits, '.', two digits
    if (name.size() != 11 || name[4] != '_' || name[8] != '.') return false;
    for (int i = 0; i < 4; ++i)
        if (!std::isalpha(static_cast<unsigned char>(name[i]))) return false;
    for (int i : {5, 6, 7, 9, 10})
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
    return true;
}

// "SLES 52568" / "SLES-52568" / "sles_525.68" -> "SLES-52568"
std::string NormalSerial(const std::string& s) {
    std::string letters, digits;
    for (char c : s) {
        if (std::isalpha(static_cast<unsigned char>(c)) && digits.empty()) letters += char(std::toupper(static_cast<unsigned char>(c)));
        else if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
    }
    if (letters.size() != 4 || digits.size() != 5) return Upper(s);
    return letters + "-" + digits;
}

std::string JoinNamed(const std::vector<std::string>& names, size_t limit, const char* last_joiner) {
    std::string out;
    const size_t n = std::min(limit, names.size());
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) out += (i + 1 == n && names.size() <= limit) ? last_joiner : ", ";
        out += names[i];
    }
    if (names.size() > limit) out += ", ...";
    return out;
}

DiscIdentity::Region RegionFromName(const std::string& name) {
    const std::string u = Upper(name);
    if (u == "PAL") return DiscIdentity::Region::PAL;
    if (u == "NTSC-U" || u == "NTSC_U") return DiscIdentity::Region::NTSC_U;
    if (u == "NTSC-J" || u == "NTSC_J") return DiscIdentity::Region::NTSC_J;
    return DiscIdentity::Region::Unknown;
}

std::vector<RetailManifest>& Registry() {
    static std::vector<RetailManifest> manifests;
    return manifests;
}

std::vector<KnownDump>& Dumps() {
    static std::vector<KnownDump> dumps;
    return dumps;
}

std::vector<KnownProgram>& Programs() {
    static std::vector<KnownProgram> programs;
    return programs;
}


std::string ReadWholeFile(const std::string& path, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot read " + path + ": " + std::strerror(errno);
        return {};
    }
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    return text;
}

// --- the disc --------------------------------------------------------------------------------------------------------
struct DiscFile {
    std::string path;
    uint32_t lba = 0;
    uint64_t size = 0;
};

bool Walk(const IsoFs& fs, const IsoFs::Entry& dir, const std::string& prefix, int depth, std::vector<DiscFile>& out) {
    if (depth > kMaxDepth) return false;
    std::vector<IsoFs::Entry> entries;
    if (!fs.List(dir, entries)) return false;
    for (const IsoFs::Entry& e : entries) {
        const std::string name = Upper(StripVersion(e.name));
        if (e.directory) {
            if (!Walk(fs, e, prefix + name + "/", depth + 1, out)) return false;
            continue;
        }
        if (out.size() >= kMaxFiles) return false;
        out.push_back(DiscFile{prefix + name, e.lba, e.size});
    }
    return true;
}

const DiscFile* FindFile(const std::vector<DiscFile>& files, const std::string& path) {
    for (const DiscFile& f : files)
        if (f.path == path) return &f;
    return nullptr;
}

uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// SYSTEM.CNF: "KEY = VALUE" lines (BOOT2, VER, VMODE); the keys upper-cased.
void ReadSystemCnf(const IsoFs& fs, const DiscFile& cnf, DiscIdentity* id) {
    const size_t n = static_cast<size_t>(std::min<uint64_t>(cnf.size, kCnfMax));
    std::vector<uint8_t> buf(n);
    const size_t got = fs.ReadBytes(uint64_t(cnf.lba) * IsoFs::kSectorSize, n, buf.data());
    const std::string text(reinterpret_cast<const char*>(buf.data()), got);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find_first_of("\r\n", pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Upper(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        if (key == "BOOT2") id->boot = value;
        else if (key == "VER") id->version = value;
        else if (key == "VMODE") id->video_mode = value;
    }
}

void ReadElfHeader(const IsoFs& fs, const DiscFile& program, DiscIdentity* id) {
    uint8_t h[52] = {};
    id->elf_bytes = program.size;
    if (program.size < sizeof h || fs.ReadBytes(uint64_t(program.lba) * IsoFs::kSectorSize, sizeof h, h) != sizeof h) return;
    const bool elf = h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[4] == 1 && h[5] == 1;
    const uint16_t machine = uint16_t(h[18] | (h[19] << 8));
    if (elf && machine == 8) id->elf_entry = Le32(h + 24);   // EM_MIPS
}

// Hashes one file through the volume's logical reads. False when the image ends inside it (the file then differs).
bool HashFile(const IsoFs& fs, const DiscFile& f, std::vector<uint8_t>& buf, uint64_t* done, uint64_t total,
              const IdentifyProgress& progress, bool* stopped, std::string* hex) {
    hash::Sha256 h;
    uint64_t pos = 0;
    while (pos < f.size) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), f.size - pos));
        const size_t got = fs.ReadBytes(uint64_t(f.lba) * IsoFs::kSectorSize + pos, want, buf.data());
        if (got != want) return false;
        h.Update(buf.data(), got);
        pos += got;
        *done += got;
        if (progress && !progress(*done, total, f.path)) { *stopped = true; return false; }
    }
    uint8_t digest[hash::Sha256::kDigestBytes];
    h.Finish(digest);
    *hex = hash::Sha256::Hex(digest);
    return true;
}

// The program file's sha256 (a few MB: milliseconds).
std::string HashProgram(const IsoFs& fs, const DiscFile& f) {
    hash::Sha256 h;
    std::vector<uint8_t> buf(kChunk);
    uint64_t pos = 0;
    while (pos < f.size) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), f.size - pos));
        const size_t got = fs.ReadBytes(uint64_t(f.lba) * IsoFs::kSectorSize + pos, want, buf.data());
        if (got != want) return {};
        h.Update(buf.data(), got);
        pos += got;
    }
    uint8_t digest[hash::Sha256::kDigestBytes];
    h.Finish(digest);
    return hash::Sha256::Hex(digest);
}

// CRC-32 and SHA-1 of the whole logical image (what a DVD dump holds). False when stopped or the image reads short.
bool HashImage(const IsoFs& fs, const IdentifyProgress& progress, DiscIdentity* id, bool* stopped) {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t total = fs.ImageBytes();
    std::vector<uint8_t> buf(kImageChunk);
    Crc32 crc;
    Sha1 sha;
    uint64_t pos = 0;
    while (pos < total) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), total - pos));
        const size_t got = fs.ReadBytes(pos, want, buf.data());
        if (got != want) return false;
        crc.Update(buf.data(), got);
        sha.Update(buf.data(), got);
        pos += got;
        if (progress && !progress(pos, total, std::string())) { *stopped = true; return false; }
    }
    uint8_t digest[Sha1::kDigestBytes];
    sha.Finish(digest);
    id->image_crc32 = crc.Value();
    id->image_sha1 = Sha1::Hex(digest);
    id->image_hashed = true;
    id->image_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

size_t ListScore(const RetailManifest& m, const std::vector<DiscFile>& files) {
    size_t score = 1;
    for (const RetailFile& r : m.files) {
        const DiscFile* f = FindFile(files, r.path);
        if (f && f->size == r.bytes) ++score;
    }
    return score;
}

// The file list for `executable` with the most files matching by path and size; `retail_only` = of retail provenance.
const RetailManifest* PickManifest(const std::vector<DiscFile>& files, const std::string& executable, bool retail_only) {
    const RetailManifest* best = nullptr;
    size_t best_score = 0;
    for (const RetailManifest& m : Registry()) {
        if (Upper(m.executable) != executable || (retail_only && !m.retail)) continue;
        const size_t score = ListScore(m, files);
        if (score > best_score) { best = &m; best_score = score; }
    }
    return best;
}

std::vector<std::string> CoreFiles(const RetailManifest* matched) {
    std::set<std::string> core;
    if (matched) {
        for (const RetailFile& r : matched->files)
            if (r.core) core.insert(r.path);
        core.erase(Upper(matched->executable));   // the program is judged by its own list, not as a core file
        return {core.begin(), core.end()};
    }
    for (const RetailManifest& m : Registry()) {
        for (const RetailFile& r : m.files)
            if (r.core && r.path != Upper(m.executable)) core.insert(r.path);
    }
    return {core.begin(), core.end()};
}

std::string TitleName() {
    for (const RetailManifest& m : Registry())
        if (!m.title.empty()) return m.title;
    for (const KnownDump& d : Dumps())
        if (d.game && !d.title.empty()) return d.title;
    return "the game";
}

uint64_t Distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

}  // namespace

std::string Thousands(uint64_t n) {
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

std::string DiscTableKind(const std::string& text) {
    // the tools write "kind" among the first keys: read it there without parsing the table (a region map is ~500 KB)
    const std::string head = text.substr(0, 512);
    const size_t k = head.find("\"kind\"");
    if (k != std::string::npos) {
        size_t at = k + 6;
        while (at < head.size() && (head[at] == ' ' || head[at] == '\t' || head[at] == '\n' || head[at] == '\r')) ++at;
        if (at < head.size() && head[at] == ':') {
            ++at;
            while (at < head.size() && (head[at] == ' ' || head[at] == '\t' || head[at] == '\n' || head[at] == '\r')) ++at;
            if (at < head.size() && head[at] == '"') {
                const size_t end = head.find('"', at + 1);
                if (end != std::string::npos && head.find('\\', at + 1) > end) return head.substr(at + 1, end - at - 1);
            }
        }
    }
    json::Value root;
    if (!json::Parse(text, &root, nullptr) || root.kind != json::Value::Object) return {};
    const std::string kind = root.Text("kind");
    if (!kind.empty()) return kind;
    const json::Value* files = root.Field("files");
    return files && files->kind == json::Value::Array ? "files" : std::string();
}

bool ParseRetailManifest(const std::string& text, RetailManifest* out, std::string* error) {
    json::Value root;
    std::string why;
    if (!json::Parse(text, &root, &why)) {
        if (error) *error = "the file list does not read: " + why;
        return false;
    }
    if (root.kind != json::Value::Object) {
        if (error) *error = "the file list is not one JSON object";
        return false;
    }
    if (!root.Text("kind").empty() && root.Text("kind") != "files") {
        if (error) *error = "the file list is a table of another kind (" + root.Text("kind") + ")";
        return false;
    }
    RetailManifest m;
    m.id = root.Text("id");
    m.title = root.Text("title");
    m.region = RegionFromName(root.Text("region"));
    m.serial = root.Text("serial");
    m.executable = Upper(root.Text("executable"));
    m.volume_id = root.Text("volume_id");
    m.retail = root.Flag("retail", false);
    m.provenance = root.Text("provenance");
    if (const json::Value* elf = root.Field("elf")) m.elf_entry = uint32_t(elf->U64("entry", 0));
    const json::Value* files = root.Field("files");
    if (m.id.empty() || m.executable.empty() || !files || files->kind != json::Value::Array || files->items.empty()) {
        if (error) *error = "the file list needs an id, an executable and a non-empty files array";
        return false;
    }
    for (const json::Value& row : files->items) {
        RetailFile f;
        f.path = DiscPath(row.Text("path"));
        f.sha256 = Lower(row.Text("sha256"));
        const json::Value* bytes = row.Field("bytes");
        if (f.path.empty() || !bytes || bytes->kind != json::Value::Number || bytes->number < 0 || f.sha256.size() != 64) {
            if (error) *error = "a file of the file list has no path, size or sha256";
            return false;
        }
        f.bytes = static_cast<uint64_t>(bytes->number);
        f.core = row.Flag("core", false);
        m.files.push_back(std::move(f));
    }
    *out = std::move(m);
    return true;
}

bool LoadRetailManifest(const std::string& path, RetailManifest* out, std::string* error) {
    std::string read_error;
    const std::string text = ReadWholeFile(path, &read_error);
    if (!read_error.empty()) {
        if (error) *error = read_error;
        return false;
    }
    return ParseRetailManifest(text, out, error);
}

void AddRetailManifest(const RetailManifest& manifest) {
    for (RetailManifest& m : Registry()) {
        if (m.id == manifest.id) { m = manifest; return; }
    }
    Registry().push_back(manifest);
}

void ClearRetailManifests() { Registry().clear(); }

const std::vector<RetailManifest>& RetailManifests() { return Registry(); }

bool ParseKnownDumps(const std::string& text, std::vector<KnownDump>* out, std::string* error) {
    json::Value root;
    std::string why;
    if (!json::Parse(text, &root, &why)) {
        if (error) *error = "the table of known dumps does not read: " + why;
        return false;
    }
    const json::Value* dumps = root.Field("dumps");
    if (root.Text("kind") != "redump" || !dumps || dumps->kind != json::Value::Array || dumps->items.empty()) {
        if (error) *error = "the table of known dumps needs \"kind\": \"redump\" and a non-empty dumps array";
        return false;
    }
    std::vector<KnownDump> list;
    for (const json::Value& row : dumps->items) {
        KnownDump d;
        d.id = uint32_t(row.U64("id", 0));
        d.url = row.Text("url");
        d.title = row.Text("title");
        d.serial = NormalSerial(row.Text("serial"));
        d.region = RegionFromName(row.Text("region"));
        d.region_words = row.Text("region_words");
        d.version = row.Text("version");
        d.edition = row.Text("edition");
        d.volume_id = row.Text("volume_id");
        d.sectors = row.U64("sectors", 0);
        d.bytes = row.U64("bytes", 0);
        d.md5 = Lower(row.Text("md5"));
        d.sha1 = Lower(row.Text("sha1"));
        d.game = row.Flag("game", true);
        d.why = row.Text("why");
        const std::string crc = row.Text("crc32");
        char* stop = nullptr;
        d.crc32 = uint32_t(std::strtoul(crc.c_str(), &stop, 16));
        if (!d.id || d.serial.size() != 10 || !d.bytes || crc.size() != 8 || stop != crc.c_str() + crc.size() || d.sha1.size() != 40) {
            if (error) *error = "a dump of the table has no id, serial, size, crc32 or sha1 (" + row.Text("serial") + ")";
            return false;
        }
        list.push_back(std::move(d));
    }
    *out = std::move(list);
    return true;
}

namespace {
bool ParsePrograms(const json::Value& root, std::vector<KnownProgram>* out, std::string* error) {
    const json::Value* programs = root.Field("programs");
    if (!programs) return true;   // optional
    if (programs->kind != json::Value::Array) {
        if (error) *error = "the table's programs is not an array";
        return false;
    }
    for (const json::Value& row : programs->items) {
        KnownProgram p;
        p.serial = NormalSerial(row.Text("serial"));
        p.executable = Upper(row.Text("executable"));
        p.bytes = row.U64("bytes", 0);
        p.sha256 = Lower(row.Text("sha256"));
        p.source = row.Text("source");
        if (p.serial.size() != 10 || p.sha256.size() != 64) {
            if (error) *error = "a program of the table has no serial or sha256 (" + row.Text("executable") + ")";
            return false;
        }
        out->push_back(std::move(p));
    }
    return true;
}
}  // namespace

bool LoadKnownDumps(const std::string& path, std::vector<KnownDump>* out, std::string* error) {
    std::string read_error;
    const std::string text = ReadWholeFile(path, &read_error);
    if (!read_error.empty()) {
        if (error) *error = read_error;
        return false;
    }
    return ParseKnownDumps(text, out, error);
}

void AddKnownDumps(const std::vector<KnownDump>& dumps) {
    for (const KnownDump& d : dumps) {
        bool replaced = false;
        for (KnownDump& k : Dumps()) {
            if (k.id == d.id) { k = d; replaced = true; break; }
        }
        if (!replaced) Dumps().push_back(d);
    }
}

void ClearKnownDumps() {
    Dumps().clear();
    Programs().clear();
}

const std::vector<KnownDump>& KnownDumps() { return Dumps(); }
const std::vector<KnownProgram>& KnownPrograms() { return Programs(); }

bool RegisterDiscTable(const std::string& text, std::string* error) {
    const std::string kind = DiscTableKind(text);
    if (kind == "redump") {
        std::vector<KnownDump> dumps;
        if (!ParseKnownDumps(text, &dumps, error)) return false;
        json::Value root;
        std::vector<KnownProgram> programs;
        if (!json::Parse(text, &root, error) || !ParsePrograms(root, &programs, error)) return false;
        AddKnownDumps(dumps);
        for (const KnownProgram& p : programs) {
            bool replaced = false;
            for (KnownProgram& k : Programs())
                if (k.sha256 == p.sha256) { k = p; replaced = true; }
            if (!replaced) Programs().push_back(p);
        }
        return true;
    }
    if (kind == "files") {
        RetailManifest m;
        if (!ParseRetailManifest(text, &m, error)) return false;
        AddRetailManifest(m);
        return true;
    }
    if (kind == "region-map") {   // another release's address map: kept as text, read when a disc of its serial needs it
        if (AddRegionMapText(text)) return true;
        if (error) *error = "a region map with no serial";
        return false;
    }
    if (error) *error = kind.empty() ? std::string("not a disc table") : "a disc table of another kind (" + kind + ")";
    return false;
}

bool LoadDiscTable(const std::string& path, std::string* error) {
    std::string read_error;
    const std::string text = ReadWholeFile(path, &read_error);
    if (!read_error.empty()) {
        if (error) *error = read_error;
        return false;
    }
    return RegisterDiscTable(text, error);
}

const char* RegionName(DiscIdentity::Region region) {
    switch (region) {
        case DiscIdentity::Region::PAL: return "PAL";
        case DiscIdentity::Region::NTSC_U: return "NTSC-U";
        case DiscIdentity::Region::NTSC_J: return "NTSC-J";
        case DiscIdentity::Region::Unknown: break;
    }
    return "unknown region";
}

const char* VerdictName(DiscIdentity::Verdict verdict) {
    switch (verdict) {
        case DiscIdentity::Verdict::Retail: return "retail";
        case DiscIdentity::Verdict::RetailBySize: return "retail-size";
        case DiscIdentity::Verdict::Modified: return "modified";
        case DiscIdentity::Verdict::Unknown: return "unknown";
        case DiscIdentity::Verdict::Refused: break;
    }
    return "refused";
}

DiscIdentity::Region RegionOfSerial(const std::string& name) {
    const std::string p = Upper(name.substr(0, std::min<size_t>(4, name.size())));
    if (p == "SLES" || p == "SCES" || p == "SCED" || p == "SLED") return DiscIdentity::Region::PAL;
    if (p == "SLUS" || p == "SCUS") return DiscIdentity::Region::NTSC_U;
    if (p == "SLPM" || p == "SLPS" || p == "SCPS" || p == "SCAJ" || p == "SLAJ") return DiscIdentity::Region::NTSC_J;
    return DiscIdentity::Region::Unknown;
}

std::string SerialOf(const std::string& executable) {
    const std::string e = Upper(executable);
    if (!LooksLikeSerial(e)) return {};
    return e.substr(0, 4) + "-" + e.substr(5, 3) + e.substr(9, 2);
}

bool IdentifyDisc(const std::string& path, DiscIdentity* out, std::string* error) {
    return IdentifyDiscWith(path, IdentifyOptions{}, out, error);
}

bool IdentifyDiscFully(const std::string& path, DiscIdentity* out, std::string* error, const IdentifyProgress& progress) {
    IdentifyOptions o;
    o.hash_image = true;
    o.hash_all = true;
    o.progress = progress;
    return IdentifyDiscWith(path, o, out, error);
}

bool IdentifyDiscWith(const std::string& path, const IdentifyOptions& options, DiscIdentity* out, std::string* error) {
    DiscIdentity local;
    DiscIdentity& id = out ? *out : local;
    id = DiscIdentity{};
    auto refuse = [&](const std::string& why) {
        if (error) *error = why;
        id.file_set_ok = false;
        id.retail = false;
        id.verdict = DiscIdentity::Verdict::Refused;
        return false;
    };
    {
        std::FILE* probe = std::fopen(path.c_str(), "rb");
        if (!probe) return refuse(std::string("It cannot be opened (") + std::strerror(errno) + ").");
        std::fclose(probe);
    }
    IsoFs fs;
    if (!fs.Open(path))
        return refuse("It is not a disc image (it has no ISO 9660 volume). Images in .cue, .mdf, .nrg or .chd form, or "
                      "inside an archive, need converting to .iso first.");
    id.volume_id = fs.VolumeId();
    id.bytes = fs.ImageBytes();
    id.image_bytes = fs.ImageBytes();
    id.raw = fs.RawSectors();
    std::vector<DiscFile> files;
    if (!Walk(fs, fs.Root(), "", 0, files)) return refuse("Its folders cannot be read: the image is damaged or incomplete.");
    id.files = uint32_t(files.size());

    // the program: SYSTEM.CNF's BOOT2, else the root's serial-shaped file
    std::string program_path;
    if (const DiscFile* cnf = FindFile(files, "SYSTEM.CNF")) {
        ReadSystemCnf(fs, *cnf, &id);
        if (!id.boot.empty()) program_path = DiscPath(id.boot);
    }
    if (program_path.empty()) {
        for (const DiscFile& f : files) {
            if (f.path.find('/') == std::string::npos && LooksLikeSerial(f.path)) { program_path = f.path; break; }
        }
    }
    if (program_path.empty()) return refuse("It is a disc image, but not of a PlayStation 2 game (it has no SYSTEM.CNF).");
    const size_t slash = program_path.rfind('/');
    id.executable = slash == std::string::npos ? program_path : program_path.substr(slash + 1);
    id.serial = SerialOf(id.executable);
    id.region = RegionOfSerial(id.executable);
    if (id.region == DiscIdentity::Region::Unknown && Upper(id.video_mode) == "PAL") id.region = DiscIdentity::Region::PAL;
    const DiscFile* program = FindFile(files, program_path);
    if (program) {
        ReadElfHeader(fs, *program, &id);
        id.program_sha256 = HashProgram(fs, *program);
        for (const KnownProgram& p : Programs())
            if (!id.program_sha256.empty() && p.sha256 == id.program_sha256 && p.serial == id.serial) id.known_executable = true;
    }

    // the table's dumps of this serial; a serial the table knows as another disc (a demo) is refused by name
    std::vector<const KnownDump*> dumps;
    for (const KnownDump& d : Dumps()) {
        if (id.serial.empty() || d.serial != id.serial) continue;
        if (!d.game) {
            id.title = d.title;
            return refuse("It is a PlayStation 2 disc (" + id.serial + "), but " + (d.why.empty() ? "not the game" : d.why) + ", not " +
                          TitleName() + " itself.");
        }
        dumps.push_back(&d);
    }

    // the core files decide whether this is the title at all
    const RetailManifest* manifest = PickManifest(files, Upper(id.executable), false);
    std::vector<std::string> missing_core;
    for (const std::string& c : CoreFiles(manifest))
        if (!FindFile(files, c)) missing_core.push_back(c);
    if (!program) missing_core.insert(missing_core.begin(), program_path);
    if (manifest) {
        id.manifest = manifest->id;
        id.title = manifest->title;
        id.list_provenance = manifest->provenance;
    }
    if (id.title.empty() && !dumps.empty()) id.title = dumps.front()->title;
    if (!missing_core.empty()) {
        id.missing_files = missing_core;
        const std::string what = id.serial.empty() ? std::string("a PlayStation 2 disc") : "a PlayStation 2 disc (" + id.serial + ")";
        return refuse("It is " + what + ", but not " + TitleName() + ": " + JoinNamed(missing_core, 4, " and ") +
                      (missing_core.size() == 1 ? " is" : " are") + " missing.");
    }
    id.file_set_ok = true;

    // the image against the dumps: the size first (decisive when no dump has it), then CRC-32 + SHA-1 when asked
    id.known_program = !dumps.empty();
    id.program_dumps = uint32_t(dumps.size());
    const KnownDump* closest = nullptr;
    for (const KnownDump* d : dumps) {
        if (d->bytes == id.image_bytes) id.same_size = true;
        if (!closest || Distance(d->bytes, id.image_bytes) < Distance(closest->bytes, id.image_bytes)) closest = d;
    }
    if (options.hash_image) {
        bool stopped = false;
        if (!HashImage(fs, options.progress, &id, &stopped)) {
            if (stopped) return refuse("The check was stopped.");
            return refuse("The image ends before its last sector: it is damaged or incomplete.");
        }
    }
    const KnownDump* matched = nullptr;
    for (const KnownDump* d : dumps) {
        if (d->bytes != id.image_bytes) continue;
        if (!id.image_hashed) {   // retail by size: the first dump of that size -- and only with the retail program
            if (id.known_executable) { matched = d; break; }
            continue;
        }
        if (d->crc32 == id.image_crc32 && d->sha1 == id.image_sha1) { matched = d; break; }
    }
    const KnownDump* reference = matched ? matched : closest;
    if (reference) {
        id.dump_id = reference->id;
        id.dump_version = reference->version;
        id.dump_edition = reference->edition;
        id.dump_bytes = reference->bytes;
    }
    if (matched && id.image_hashed) {
        id.verdict = DiscIdentity::Verdict::Retail;   // the dump itself: whatever else is known
        id.retail = true;
    } else if (!id.known_program || !id.known_executable) {
        id.verdict = DiscIdentity::Verdict::Unknown;
        if (!id.known_program)
            id.warning = "This version of " + TitleName() + " (" + (id.serial.empty() ? id.executable : id.serial) + ", " +
                         RegionName(id.region) + ") is not one the launcher knows: it may not play as expected.";
        else
            id.warning = "This disc of " + TitleName() + " (" + id.serial + ", " + RegionName(id.region) + ") does not carry the "
                         "original release's program (" + id.executable + "): it may not play as expected.";
    } else if (matched) {
        id.verdict = DiscIdentity::Verdict::RetailBySize;
    } else {
        id.verdict = DiscIdentity::Verdict::Modified;
    }
    if (id.retail) return true;   // a known dump: nothing to name

    // the files that differ, named only against a list of RETAIL provenance for this program
    const RetailManifest* retail_list = PickManifest(files, Upper(id.executable), true);
    if (!retail_list) return true;
    id.list_retail = true;
    id.manifest = retail_list->id;
    id.list_provenance = retail_list->provenance;
    std::vector<uint8_t> buf(kChunk);
    std::vector<const RetailFile*> to_hash;
    std::set<std::string> listed;
    uint64_t total = 0;
    size_t same_size = 0;   // files whose size matches: the ones a hash can still tell apart
    for (const RetailFile& r : retail_list->files) {
        listed.insert(r.path);
        const DiscFile* f = FindFile(files, r.path);
        if (!f) { id.missing_files.push_back(r.path); continue; }
        if (f->size != r.bytes) { id.differing_files.push_back(r.path); continue; }
        ++same_size;
        if (options.hash_all || r.bytes < options.hash_below) { to_hash.push_back(&r); total += r.bytes; }
    }
    for (const DiscFile& f : files)
        if (!listed.count(f.path)) id.added_files.push_back(f.path);
    uint64_t done = 0;
    for (const RetailFile* r : to_hash) {
        const DiscFile* f = FindFile(files, r->path);
        std::string hex;
        bool stopped = false;
        const bool read = HashFile(fs, *f, buf, &done, total, options.progress, &stopped, &hex);
        if (stopped) return refuse("The check was stopped.");
        ++id.files_hashed;
        id.bytes_hashed += f->size;
        if (!read || hex != r->sha256) id.differing_files.push_back(r->path);
    }
    std::sort(id.differing_files.begin(), id.differing_files.end());
    id.fully_hashed = to_hash.size() == same_size;
    return true;
}

std::string ClassifyDisc(const DiscIdentity& id) {
    const std::string serial = id.serial.empty() ? id.executable : id.serial;
    const std::string dump = serial + (id.program_dumps > 1 ? " v" + id.dump_version : std::string()) + "'s dump";
    switch (id.verdict) {
        case DiscIdentity::Verdict::Retail: return "retail " + serial + " v" + id.dump_version + " (Redump)";
        case DiscIdentity::Verdict::RetailBySize:
            return std::string(RegionName(id.region)) + " image with the size of " + dump + " (not verified)";
        case DiscIdentity::Verdict::Modified: {
            std::string how;
            if (id.image_bytes > id.dump_bytes) how = Thousands(id.image_bytes - id.dump_bytes) + " bytes larger than " + dump;
            else if (id.image_bytes < id.dump_bytes) how = Thousands(id.dump_bytes - id.image_bytes) + " bytes smaller than " + dump;
            else how = "the size of " + dump + ", other contents";
            return "modified " + std::string(RegionName(id.region)) + " image (not a known retail dump; " + how + ")";
        }
        case DiscIdentity::Verdict::Unknown: return "unknown but plausible";
        case DiscIdentity::Verdict::Refused: break;
    }
    return "refused";
}

std::string DescribeDisc(const DiscIdentity& id) {
    const std::string region = RegionName(id.region);
    const std::string who = id.serial.empty() ? (id.executable.empty() ? std::string() : " " + id.executable) : " " + id.serial;
    const std::string volume = "volume " + (id.volume_id.empty() ? std::string("unnamed") : id.volume_id);
    switch (id.verdict) {
        case DiscIdentity::Verdict::Retail:
            return "retail " + region + " image" + who + " v" + id.dump_version + " (Redump disc " + std::to_string(id.dump_id) + "; " +
                   volume + ")";
        case DiscIdentity::Verdict::RetailBySize:
            return region + " image" + who + " v" + id.dump_version + " by size (the retail dump's " + Thousands(id.dump_bytes) +
                   " bytes; contents not verified: --verify-disc hashes the image; " + volume + ")";
        case DiscIdentity::Verdict::Modified: {
            const char* article = id.program_dumps > 1 ? "a" : "the";
            std::string why;
            if (!id.same_size)
                why = std::string("not ") + article + " retail dump: " + Thousands(id.image_bytes) + " bytes vs " + Thousands(id.dump_bytes) +
                      (id.program_dumps > 1 ? " (v" + id.dump_version + ", the closest of " + std::to_string(id.program_dumps) + ")" : std::string());
            else
                why = std::string("not ") + article + " retail dump: the size of the v" + id.dump_version + " dump, a different SHA-1";
            if (id.list_retail) {
                std::vector<std::string> all = id.differing_files;
                all.insert(all.end(), id.added_files.begin(), id.added_files.end());
                all.insert(all.end(), id.missing_files.begin(), id.missing_files.end());
                if (all.empty()) why += "; every file matches the retail file list";
                else why += "; " + std::to_string(all.size()) + (all.size() == 1 ? " file differs" : " files differ") +
                            " from the retail file list: " + JoinNamed(all, kNamed, ", ");
            }
            return "modified " + region + " image" + who + " (" + why + "; " + volume + ")";
        }
        case DiscIdentity::Verdict::Unknown:
            return "unknown but plausible " + region + " image" + who + " (" +
                   (id.known_program ? std::string("its program is not a known retail executable") : std::string("no retail dump of this program is known")) +
                   "; " + volume + ")";
        case DiscIdentity::Verdict::Refused: break;
    }
    const std::string missing = id.missing_files.empty() ? std::string() : "missing: " + JoinNamed(id.missing_files, kNamed, ", ") + "; ";
    return "refused " + (id.region == DiscIdentity::Region::Unknown ? std::string("disc") : region + " image") + who + " (" +
           missing + volume + ")";
}

}  // namespace ps2r::app
