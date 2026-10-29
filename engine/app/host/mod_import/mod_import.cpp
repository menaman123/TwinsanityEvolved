// mod_import.h implementation: what an artefact is (Detect), the disc and its identity, and the Import / Inspect / Verify
// flows. What an artefact changes, and the folder written from it, are in mod_changes.cpp.
#include "app/host/mod_import/mod_import.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <set>

#include "app/host/mod_import/hashes.h"
#include "app/host/mod_import/io.h"
#include "app/host/mod_import/json.h"
#include "app/host/mod_import/mod_internal.h"
#include "app/host/mod_import/patches.h"
#include "app/host/mod_import/zip_reader.h"
#include "engine/cdvd/disc_overlay.h"
#include "engine/core/hash/sha256.h"

namespace te::modimport {

namespace detail {

std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
std::string Upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}
std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}
std::string FileName(const std::string& path) {
    std::string p = path;
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}
std::string Gb(uint64_t bytes) {
    char b[32];
    if (bytes >= (1ull << 30)) std::snprintf(b, sizeof b, "%.1f GB", double(bytes) / double(1ull << 30));
    else std::snprintf(b, sizeof b, "%.0f MB", double(bytes) / double(1ull << 20) + 0.5);
    return b;
}

// ---- the disc -----------------------------------------------------------------------------------------------------------
namespace {

std::string StripVersion(const std::string& name) {
    const size_t semi = name.find(';');
    return semi == std::string::npos ? name : name.substr(0, semi);
}

bool Walk(const IsoFs& fs, const IsoFs::Entry& dir, const std::string& prefix, int depth, std::vector<DiscFile>& out) {
    if (depth > 16) return false;
    std::vector<IsoFs::Entry> entries;
    if (!fs.List(dir, entries)) return false;
    for (const IsoFs::Entry& e : entries) {
        const std::string name = Upper(StripVersion(e.name));
        if (name.empty()) continue;
        if (e.directory) {
            if (!Walk(fs, e, prefix + name + "/", depth + 1, out)) return false;
            continue;
        }
        if (out.size() >= 65536) return false;
        out.push_back(DiscFile{prefix + name, e.lba, e.size});
    }
    return true;
}

std::string RegionOfSerial(const std::string& exe) {
    const std::string p = Upper(exe.substr(0, std::min<size_t>(4, exe.size())));
    if (p == "SLES" || p == "SCES" || p == "SCED" || p == "SLED") return "PAL";
    if (p == "SLUS" || p == "SCUS") return "NTSC-U";
    if (p == "SLPM" || p == "SLPS" || p == "SCPS" || p == "SCAJ" || p == "SLAJ") return "NTSC-J";
    return "any";
}

}  // namespace

bool Disc::Open(const std::string& path, std::string* error) {
    path_ = path;
    files_.clear();
    by_path_.clear();
    std::string why;
    if (!iso_.Open(path, &why)) {
        if (error) *error = "it is not a disc image (" + why + ")";
        return false;
    }
    if (!Walk(iso_, iso_.Root(), "", 0, files_)) {
        if (error) *error = "its folders cannot be read: the image is damaged or incomplete";
        return false;
    }
    for (size_t i = 0; i < files_.size(); ++i) by_path_[files_[i].path] = i;
    // the program SYSTEM.CNF boots ("BOOT2 = cdrom0:\SLES_525.68;1")
    if (const DiscFile* cnf = Find("SYSTEM.CNF")) {
        std::vector<uint8_t> bytes;
        if (cnf->size <= 4096 && ReadFile(*cnf, &bytes)) {
            const std::string text(bytes.begin(), bytes.end());
            size_t at = 0;
            while (at < text.size()) {
                size_t eol = text.find_first_of("\r\n", at);
                if (eol == std::string::npos) eol = text.size();
                const std::string line = text.substr(at, eol - at);
                at = eol + 1;
                const size_t eq = line.find('=');
                if (eq == std::string::npos || Upper(Trim(line.substr(0, eq))) != "BOOT2") continue;
                std::string v = Trim(line.substr(eq + 1));
                if (Lower(v.substr(0, 7)) == "cdrom0:") v = v.substr(7);
                std::replace(v.begin(), v.end(), '\\', '/');
                while (!v.empty() && v[0] == '/') v.erase(0, 1);
                executable_ = Upper(StripVersion(v));
            }
        }
    }
    region_ = RegionOfSerial(FileName(executable_));
    return true;
}

const DiscFile* Disc::Find(const std::string& disc_path) const {
    std::string p = Upper(disc_path);
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    const auto it = by_path_.find(StripVersion(p));
    return it == by_path_.end() ? nullptr : &files_[it->second];
}

bool Disc::ReadFile(const DiscFile& f, std::vector<uint8_t>* out) const {
    out->resize(size_t(f.size));
    return out->empty() || iso_.ReadBytes(uint64_t(f.lba) * IsoFs::kSectorSize, out->size(), out->data()) == out->size();
}

std::unique_ptr<ByteSource> Disc::Source(const DiscFile& f) const {
    return std::unique_ptr<ByteSource>(new ImageFileSource(iso_, f.lba, f.size));
}

// ---- progress -----------------------------------------------------------------------------------------------------------
Plan::Plan(const Progress& p, std::vector<std::pair<std::string, double>> stages) : progress_(p), stages_(std::move(stages)) {
    for (const auto& s : stages_) total_ += s.second;
    if (total_ <= 0) total_ = 1;
}

void Plan::Enter(size_t stage) {
    before_ = 0;
    for (size_t i = 0; i < stage && i < stages_.size(); ++i) before_ += stages_[i].second;
    at_ = stage;
    Step(0);
}

bool Plan::Step(double f) {
    if (Cancelled()) return false;
    if (f < 0) return true;   // a tick: only the stop is asked
    if (at_ >= stages_.size()) return true;
    const double whole = std::min(1.0, (before_ + stages_[at_].second * std::min(1.0, f)) / total_);
    if (whole < last_ + 0.002 && f < 1.0 && f > 0.0) return true;   // a report per 0.2 %
    last_ = std::max(last_, whole);
    if (progress_.report) progress_.report(stages_[at_].first, last_);
    return true;
}

bool Plan::Cancelled() const { return progress_.cancelled && progress_.cancelled(); }

// ---- the disc's record (identity + per-file hashes), cached beside the mods ----------------------------------------------
namespace {

struct RedumpDump { uint64_t size; const char* sha1; const char* name; };
// Redump's dumps of the game, the same rows as launcher/discs/redump.json (the identity module's table, every entry cited
// there): an image matching one is that retail disc. redump.org discs 4514 (PAL), 42680 (NTSC-U v1.00 Original), 13689
// (NTSC-U v2.00 Action Pack), 26782 (Japan). Kept here as plain constants so the library needs no table file at run time;
// a new row goes in BOTH places.
const RedumpDump kRedump[] = {{2558263296ull, "7cd787a8cb35d1bcde63ad742746d2222813145e", "SLES-52568 v1.01"},
                              {2108096512ull, "0d0252b6656422b75c37f325cb0002705f13bada", "SLUS-20909 v1.00"},
                              {2108489728ull, "4f1a56f633c1b500bf3e39a0df1fdc8f101018ff", "SLUS-20909 v2.00"},
                              {1954578432ull, "53fd7c66e14fb3f24896164e5861b5a27858df36", "SLPM-65801 v1.00"}};

std::string CacheKey(const std::string& path, uint64_t size) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(io::Path(path), ec);
    const long long stamp = ec ? 0 : (long long)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    std::error_code ec2;
    const std::string abs = io::Utf8(std::filesystem::absolute(io::Path(path), ec2));
    return (ec2 ? path : abs) + "|" + std::to_string(size) + "|" + std::to_string(stamp);
}

}  // namespace

bool DiscRecord::Load() {
    loaded_ = true;
    files_.clear();
    std::string text;
    json::Value root;
    if (cache_path_.empty() || !io::ReadWhole(cache_path_, 8u << 20, &text) || !json::Parse(text, &root, nullptr)) return false;
    const json::Value* discs = root.Get("discs");
    if (!discs || !discs->IsArray()) return false;
    for (const json::Value& d : discs->items) {
        if (d.String("key") != key_) continue;
        sha1 = d.String("sha1");
        if (const json::Value* f = d.Get("files"); f && f->IsObject())
            for (const auto& kv : f->members)
                files_[kv.first] = FileHashes{kv.second.String("sha256"), kv.second.String("sample_sha256")};
        return sha1.size() == 40;
    }
    return false;
}

void DiscRecord::Save() const {
    if (cache_path_.empty()) return;
    std::string text;
    json::Value root;
    if (!io::ReadWhole(cache_path_, 8u << 20, &text) || !json::Parse(text, &root, nullptr) || !root.IsObject()) root = json::Value::Obj();
    json::Value discs = json::Value::Arr();
    json::Value mine = json::Value::Obj();
    mine.Set("key", json::Value::Str(key_));
    mine.Set("sha1", json::Value::Str(sha1));
    json::Value files = json::Value::Obj();
    for (const auto& kv : files_) {
        json::Value h = json::Value::Obj();
        h.Set("sha256", json::Value::Str(kv.second.sha256));
        h.Set("sample_sha256", json::Value::Str(kv.second.sample_sha256));
        files.Set(kv.first, std::move(h));
    }
    mine.Set("files", std::move(files));
    discs.Push(std::move(mine));
    if (const json::Value* old = root.Get("discs"); old && old->IsArray())
        for (const json::Value& d : old->items)
            if (d.String("key") != key_ && discs.items.size() < 8) discs.Push(d);
    json::Value out = json::Value::Obj();
    out.Set("format", json::Value::Num(1));
    out.Set("note", json::Value::Str("the mod importer's record of the disc images it read (their SHA-1 and file hashes): safe to delete"));
    out.Set("sample", json::Value::Str(std::to_string(kSampleWindows) + " x " + std::to_string(kSampleWindowBytes)));
    out.Set("discs", std::move(discs));
    io::WriteWhole(cache_path_, json::Write(out));
}

bool DiscRecord::Identify(const Disc& disc, const std::string& cache_dir, Plan* plan, std::string* error) {
    io::File f;
    if (!f.OpenRead(disc.Path())) {
        if (error) *error = "your disc image cannot be read (" + disc.Path() + ")";
        return false;
    }
    size = f.Size();
    cache_path_ = cache_dir.empty() ? std::string() : io::Join(cache_dir, ".import-cache.json");
    key_ = CacheKey(disc.Path(), size) + "|" + std::to_string(kSampleWindows) + "x" + std::to_string(kSampleWindowBytes);
    if (!Load()) {
        Sha1 h;
        std::vector<uint8_t> buf(4u << 20);
        for (uint64_t done = 0; done < size;) {
            const size_t n = size_t(std::min<uint64_t>(buf.size(), size - done));
            if (!f.ReadAt(done, buf.data(), n)) {
                if (error) *error = "your disc image does not read to its end";
                return false;
            }
            h.Update(buf.data(), n);
            done += n;
            if (plan && !plan->Step(double(done) / double(size))) {
                if (error) *error = "stopped";
                return false;
            }
        }
        sha1 = h.HexDigest();
        Save();
    }
    redump.clear();
    for (const RedumpDump& r : kRedump)
        if (r.size == size && sha1 == r.sha1) redump = r.name;
    if (plan) plan->Step(1.0);
    return true;
}

bool DiscRecord::Hashes(const Disc& disc, const DiscFile& f, Plan* plan, FileHashes* out, std::string* error) {
    if (!loaded_) Load();
    const auto it = files_.find(f.path);
    if (it != files_.end() && it->second.sha256.size() == 64 && it->second.sample_sha256.size() == 64) {
        *out = it->second;
        return true;
    }
    std::unique_ptr<ByteSource> src = disc.Source(f);
    bool stopped = false;
    const DiscOverlay::ReadFn read = [&](uint64_t offset, size_t n, uint8_t* dst) {
        if (plan && !plan->Step(-1)) { stopped = true; return false; }
        return src->Read(offset, dst, n);
    };
    FileHashes h;
    if (!DiscOverlay::WholeSha256(read, f.size, &h.sha256) ||
        !DiscOverlay::SampledSha256(read, f.size, kSampleWindows, kSampleWindowBytes, &h.sample_sha256)) {
        if (error) *error = stopped ? "stopped" : "your disc's " + f.path + " does not read";
        return false;
    }
    files_[f.path] = h;
    Save();
    *out = h;
    return true;
}

}  // namespace detail

using namespace detail;

// ---- what an artefact is ------------------------------------------------------------------------------------------------
namespace {

constexpr size_t kHead = 16 * 2352 + 32;   // enough for a raw image's primary volume descriptor

enum class Sniffed { Iso, Xdelta, Ppf, Bps, Zip, Other };

Sniffed SniffHead(const std::vector<uint8_t>& h, std::string* other, bool* generic = nullptr) {
    if (generic) *generic = false;
    auto at = [&](size_t o, const char* s, size_t n) { return h.size() >= o + n && std::memcmp(&h[o], s, n) == 0; };
    if (h.size() >= 4 && h[0] == 0xD6 && h[1] == 0xC3 && h[2] == 0xC4) return Sniffed::Xdelta;
    if (at(0, "PPF10", 5) || at(0, "PPF20", 5) || at(0, "PPF30", 5)) return Sniffed::Ppf;
    if (at(0, "BPS1", 4)) return Sniffed::Bps;
    if (ZipReader::LooksLikeZip(h.data(), h.size())) return Sniffed::Zip;
    if (h.size() > 0x8006 && h[0x8000] == 1 && at(0x8001, "CD001", 5)) return Sniffed::Iso;
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (h.size() >= 12 && std::memcmp(h.data(), sync, 12) == 0 && (at(16 * 2352 + 17, "CD001", 5) || at(16 * 2352 + 25, "CD001", 5)))
        return Sniffed::Iso;
    if (other) {
        if (at(0, "Rar!\x1A\x07", 6)) *other = "a RAR archive: extract it first, then add what it holds";
        else if (at(0, "7z\xBC\xAF\x27\x1C", 6)) *other = "a 7-Zip archive: extract it first, then add what it holds";
        else if (h.size() >= 2 && h[0] == 0x1F && h[1] == 0x8B) *other = "a gzip file: extract it first, then add what it holds";
        else if (at(0, "UPS1", 4)) *other = "a UPS patch, which the game does not take: apply it to your disc image with a UPS patcher, then add the patched image";
        else if (at(0, "PATCH", 5)) *other = "an IPS patch, which cannot address a disc image of this size";
        else if (at(0, "CISO", 4) || at(0, "ZISO", 4)) *other = "a compressed disc image (CSO / ZSO): convert it to .iso first";
        else if (at(0, "MComprHD", 8)) *other = "a CHD disc image: convert it to .iso first";
        else if (at(0, "ECM", 3)) *other = "an ECM-packed disc image: unpack it first";
        else if (at(0, "FILE ", 5) || at(0, "REM ", 4)) *other = "a .cue sheet: add the .bin or .iso it names instead";
        else {
            *other = "a file that is not a mod for this game (not a Mod Crate, a disc image, an xdelta, PPF or BPS patch, or a zip of them)";
            if (generic) *generic = true;
        }
    }
    return Sniffed::Other;
}

std::vector<uint8_t> FileHead(const std::string& path, size_t n) {
    io::File f;
    std::vector<uint8_t> out;
    if (!f.OpenRead(path)) return out;
    out.resize(size_t(std::min<uint64_t>(n, f.Size())));
    if (!out.empty() && !f.ReadAt(0, out.data(), out.size())) out.clear();
    return out;
}

Kind KindOf(Sniffed s) {
    switch (s) {
        case Sniffed::Iso: return Kind::PatchedIso;
        case Sniffed::Xdelta: return Kind::Xdelta;
        case Sniffed::Ppf: return Kind::Ppf;
        case Sniffed::Bps: return Kind::Bps;
        default: return Kind::Unknown;
    }
}

bool NoteName(const std::string& name) {
    const std::string l = Lower(name);
    const size_t dot = l.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : l.substr(dot + 1);
    static const std::set<std::string> notes = {"txt", "md", "nfo", "diz", "pdf", "png", "jpg", "jpeg", "gif", "bmp", "webp",
                                                "url", "html", "htm", "rtf", "doc", "docx", "odt", "lnk", "ico"};
    return notes.count(ext) != 0;
}

// What an artefact resolves to (inside an extraction, for a zip)
struct Resolved {
    Kind kind = Kind::Unknown;
    std::string why;
    std::string path;         // the file or folder to read
    std::string name;         // what the artefact says it is called (a crate's Name), else ""
    bool zip = false;
    bool mod_folder = false;  // a folder that already is a mod folder (mod.json): added as it is
    std::string mod_id;       // its mod.json id
};

// A mod folder already: mod.json with schema 1 or 2, an id of [a-z0-9_]+, kind disc or
// language. *id = its id.
bool IsModFolder(const std::string& dir, std::string* id, std::string* name) {
    std::string text;
    json::Value m;
    if (!io::ReadWhole(io::Join(dir, "mod.json"), 1u << 20, &text) || !json::Parse(text, &m, nullptr) || !m.IsObject()) return false;
    const uint64_t schema = m.Unsigned("schema", 1);
    const std::string kind = m.String("kind"), mid = m.String("id");
    if ((schema != 1 && schema != 2) || (kind != "disc" && kind != "language") || mid.empty() || mid.size() > 64) return false;
    for (char c : mid)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    *id = mid;
    *name = m.String("name", mid);
    return true;
}

// A crate's Name from its info text
std::string CrateName(const std::string& text) {
    const auto info = ReadCrateInfo(text);
    const auto it = info.find("name");
    return it == info.end() ? std::string() : it->second;
}

// Zip: `extract_to` "" = only say what it is
bool ResolveZip(const std::string& path, const std::string& extract_to, Resolved* r, Plan* plan, std::string* error) {
    ZipReader z;
    std::string why;
    if (!z.Open(path, &why)) {
        r->kind = Kind::Unknown;
        r->why = "a damaged zip file (" + why + ")";
        return true;
    }
    r->zip = true;
    bool crate = false, encrypted = false;
    std::string info_entry;
    std::vector<const ZipReader::Entry*> files;
    for (const ZipReader::Entry& e : z.Entries()) {
        if (e.flags & 1u) encrypted = true;
        std::string clean;
        if (!ZipReader::SafeName(e.name, &clean)) continue;
        const size_t depth = size_t(std::count(clean.begin(), clean.end(), '/'));
        const std::string base = Lower(FileName(clean));
        if (!e.directory && base == "modcrateinfo.txt" && depth <= 1) { crate = true; info_entry = e.name; }
        const std::string first = Lower(clean.substr(0, clean.find('/')));
        std::string second;
        if (depth >= 1) {
            const std::string rest = clean.substr(clean.find('/') + 1);
            second = Lower(rest.substr(0, rest.find('/')));
        }
        auto layer = [](const std::string& s) { return s.size() > 5 && s.compare(0, 5, "layer") == 0 && s.find_first_not_of("0123456789", 5) == std::string::npos; };
        if ((depth >= 1 && layer(first)) || (depth >= 2 && layer(second))) crate = true;
        if (!e.directory && !io::HostClutter(FileName(clean)) && clean.find("__MACOSX") != 0) files.push_back(&e);
    }
    if (encrypted) {
        r->kind = Kind::Unknown;
        r->why = "a password-protected zip: extract it with its password first, then add what it holds";
        return true;
    }
    const ZipReader::Entry* single = nullptr;
    size_t payload = 0;
    for (const ZipReader::Entry* e : files)
        if (!NoteName(e->name)) { ++payload; single = e; }
    Sniffed inner = Sniffed::Other;
    if (!crate && payload == 1) {
        std::vector<uint8_t> head;
        z.ReadHead(*single, kHead, &head, nullptr);
        inner = SniffHead(head, nullptr);
    }
    if (crate) {
        r->kind = Kind::ModCrate;
        std::string name;
        for (const ZipReader::Entry& e : z.Entries())
            if (e.name == info_entry) {
                std::string text;
                z.Extract(e, [&](const uint8_t* d, size_t n) { text.append(reinterpret_cast<const char*>(d), n); return text.size() < (1u << 20); }, nullptr);
                name = CrateName(text);
            }
        r->name = name;
        r->why = "a Mod Crate (zip)" + (name.empty() ? std::string() : " named " + name);
    } else if (KindOf(inner) != Kind::Unknown) {
        r->kind = KindOf(inner);
        r->why = std::string("a zip holding ") + KindLabel(r->kind) + " (" + FileName(single->name) + ")";
    } else if (payload > 0) {
        r->kind = Kind::Folder;
        r->why = "a zip of " + std::to_string(files.size()) + " file(s)";
    } else {
        r->kind = Kind::Unknown;
        r->why = "a zip with no files of a mod in it";
    }
    if (extract_to.empty() || r->kind == Kind::Unknown) return true;
    // extract: everything (a crate, a folder) or the one patch / image
    uint64_t need = 0;
    for (const ZipReader::Entry* e : files) need += (r->kind == Kind::ModCrate || r->kind == Kind::Folder || e == single) ? e->size : 0;
    const uint64_t free_bytes = io::FreeBytes(extract_to);
    if (free_bytes && need + (64u << 20) > free_bytes) {
        if (error) *error = "unpacking it needs " + Gb(need) + " of free space beside the mods folder; " + Gb(free_bytes) + " are free";
        return false;
    }
    uint64_t done = 0;
    for (const ZipReader::Entry* e : files) {
        if (!(r->kind == Kind::ModCrate || r->kind == Kind::Folder || e == single)) continue;
        std::string clean;
        if (!ZipReader::SafeName(e->name, &clean)) continue;
        const std::string out_path = io::Join(extract_to, clean);
        io::MakeDirs(io::Utf8(io::Path(out_path).parent_path()));
        io::File out;
        if (!out.Create(out_path)) {
            if (error) *error = "cannot unpack " + clean;
            return false;
        }
        bool write_failed = false;
        const bool ok = z.Extract(*e, [&](const uint8_t* d, size_t n) {
            if (!out.Append(d, n)) { write_failed = true; return false; }
            done += n;
            return !plan || plan->Step(need ? double(done) / double(need) : 1.0);
        }, &why);
        if (!out.Close() || !ok) {
            if (error) *error = plan && plan->Cancelled() ? "stopped" : write_failed ? "cannot unpack " + clean + " (is the disk full?)" : why;
            return false;
        }
        if (r->kind != Kind::ModCrate && r->kind != Kind::Folder) r->path = out_path;
    }
    if (r->kind == Kind::ModCrate || r->kind == Kind::Folder) r->path = extract_to;
    return true;
}

bool Resolve(const std::string& artefact, const std::string& extract_to, Resolved* r, Plan* plan, std::string* error) {
    *r = Resolved{};
    r->path = artefact;
    if (io::IsDir(artefact)) {
        if (IsModFolder(artefact, &r->mod_id, &r->name)) {
            r->kind = Kind::Folder;
            r->mod_folder = true;
            r->why = "a mod folder (mod.json, id " + r->mod_id + "): it is added as it is";
            return true;
        }
        bool layers = false;
        const std::string root = CrateRoot(artefact, &layers);
        if (!root.empty()) {
            std::string text;
            io::ReadWhole(io::Join(root, "ModCrateInfo.txt"), 1u << 20, &text);
            if (text.empty()) {   // any case of the name
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(io::Path(root), ec))
                    if (Lower(io::Utf8(e.path().filename())) == "modcrateinfo.txt") io::ReadWhole(io::Utf8(e.path()), 1u << 20, &text);
            }
            r->kind = Kind::ModCrate;
            r->name = CrateName(text);
            r->why = "a Mod Crate (folder)" + (r->name.empty() ? std::string() : " named " + r->name);
            return true;
        }
        std::vector<std::string> rels;
        if (!io::ListFiles(artefact, &rels)) {
            r->why = "a folder that cannot be read";
            return true;
        }
        std::vector<std::string> payload;
        for (const std::string& rel : rels)
            if (!NoteName(rel)) payload.push_back(rel);
        if (payload.size() == 1) {
            const Sniffed s = SniffHead(FileHead(io::Join(artefact, payload[0]), kHead), nullptr);
            if (KindOf(s) != Kind::Unknown) {
                r->kind = KindOf(s);
                r->path = io::Join(artefact, payload[0]);
                r->why = std::string("a folder holding ") + KindLabel(r->kind) + " (" + FileName(payload[0]) + ")";
                return true;
            }
        }
        if (rels.empty()) {
            r->why = "an empty folder";
            return true;
        }
        r->kind = Kind::Folder;
        r->why = "a folder of " + std::to_string(rels.size()) + " file(s)";
        return true;
    }
    if (!io::IsFile(artefact)) {
        r->why = "there is no such file or folder";
        return true;
    }
    std::string other;
    bool generic = false;
    const std::vector<uint8_t> head = FileHead(artefact, kHead);
    const Sniffed s = SniffHead(head, &other, &generic);
    if (s == Sniffed::Zip) return ResolveZip(artefact, extract_to, r, plan, error);
    r->kind = KindOf(s);
    switch (r->kind) {
        case Kind::PatchedIso: r->why = "a disc image"; break;
        case Kind::Xdelta: r->why = "an xdelta patch"; break;
        case Kind::Ppf: r->why = "a PPF patch"; break;
        case Kind::Bps: r->why = "a BPS patch"; break;
        default:
            if (generic && !head.empty()) {
                // one loose file: a file of the disc or an entry of its archive, by its name (decided against the disc)
                r->kind = Kind::Folder;
                r->why = "a single file (" + FileName(artefact) + "), which replaces your disc's file or its archive's entry of that name";
            } else {
                r->why = head.empty() && generic ? "an empty file" : other;
            }
            break;
    }
    return true;
}

// The artefact's sha256: a file's own; a folder's over its files ("<path>\t<size>\t<sha256>\n" each, sorted)
bool ArtefactSha256(const std::string& path, Plan* plan, std::string* hex) {
    auto file_hash = [&](const std::string& p, std::string* out) {
        io::File f;
        if (!f.OpenRead(p)) return false;
        ps2r::hash::Sha256 h;
        std::vector<uint8_t> buf(4u << 20);
        for (uint64_t done = 0; done < f.Size();) {
            const size_t n = size_t(std::min<uint64_t>(buf.size(), f.Size() - done));
            if (!f.ReadAt(done, buf.data(), n)) return false;
            h.Update(buf.data(), n);
            done += n;
            if (plan && !plan->Step(double(done) / double(f.Size()))) return false;
        }
        uint8_t d[32];
        h.Finish(d);
        *out = ps2r::hash::Sha256::Hex(d);
        return true;
    };
    if (!io::IsDir(path)) return file_hash(path, hex);
    std::vector<std::string> rels;
    if (!io::ListFiles(path, &rels)) return false;
    std::string listing;
    for (const std::string& rel : rels) {
        std::string h;
        io::File f;
        if (!f.OpenRead(io::Join(path, rel))) return false;
        const uint64_t size = f.Size();
        f.Close();
        if (!file_hash(io::Join(path, rel), &h)) return false;
        listing += rel + "\t" + std::to_string(size) + "\t" + h + "\n";
    }
    *hex = ps2r::hash::Sha256::HexOf(listing.data(), listing.size());
    return true;
}

std::string MakeId(const std::string& name) {
    std::string id;
    bool gap = false;
    for (const char c0 : name) {
        const char c = (c0 >= 'A' && c0 <= 'Z') ? char(c0 - 'A' + 'a') : c0;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (gap && !id.empty()) id.push_back('_');
            id.push_back(c);
            gap = false;
        } else {
            gap = true;
        }
        if (id.size() >= 40) break;
    }
    if (id.empty()) id = "mod";
    // the built-in mods' ids (mods/<id>/mod.json kind builtin): a folder may not take them
    if (id == "level_select" || id == "speedrun") id += "_mod";
    return id;
}

// The mod's name, known before anything is unpacked or compared: a crate's Name (ModCrateInfo.txt), else the artefact's own
// (a file without its extension, a folder's name)
std::string ModName(const Resolved& r, const std::string& artefact) {
    if (!r.name.empty()) return r.name;
    std::string stem = FileName(artefact);
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos && dot > 0 && !io::IsDir(artefact)) stem = stem.substr(0, dot);
    return stem;
}

// A job's temporary folder under <mods>/.import-tmp, removed on every way out
class Job {
public:
    explicit Job(const std::string& mods_dir) {
        static std::atomic<unsigned> counter{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = io::Join(io::Join(mods_dir, ".import-tmp"), "job-" + std::to_string(now) + "-" + std::to_string(counter++));
        ok_ = io::MakeDirs(dir_);
    }
    ~Job() {
        io::RemoveAll(dir_);
        // the parent too, when no other job is using it
        std::error_code ec;
        std::filesystem::remove(io::Path(dir_).parent_path(), ec);
    }
    bool Ok() const { return ok_; }
    std::string Path(const std::string& name) const { return io::Join(dir_, name); }

private:
    std::string dir_;
    bool ok_ = false;
};

bool Fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

std::string Sentence(const std::string& why) {
    if (why == "stopped") return "The import was stopped.";
    std::string s = why;
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = char(s[0] - 'a' + 'A');
    if (!s.empty() && s.back() != '.' && s.back() != '!' && s.back() != '?') s.push_back('.');
    return s;
}

// one rule for an id the mods folder has -- the new mod replaces it. Only a mod is replaced: a folder of that name
// without a mod.json is the player's, and the import stops with a sentence.
bool CheckReplace(const std::string& mods_dir, const std::string& id, bool* replace, std::string* error) {
    const std::string dir = io::Join(mods_dir, id);
    *replace = false;
    if (!io::IsDir(dir) && !io::IsFile(dir)) return true;
    if (!io::IsFile(io::Join(dir, "mod.json")))
        return Fail(error, "The mods folder has a folder named " + id + " that is not a mod: move it away, then add the mod again.");
    *replace = true;
    return true;
}

PatchFormat FormatOf(Kind k) {
    return k == Kind::Ppf ? PatchFormat::Ppf : k == Kind::Bps ? PatchFormat::Bps : PatchFormat::Xdelta;
}

// The changes of a resolved artefact (an applied patch's image at `image`)
bool Changes(const Disc& base, const Resolved& r, const std::string& image, ChangeSet* out, Plan* plan, std::string* error) {
    switch (r.kind) {
        case Kind::ModCrate: return ChangesFromCrate(base, r.path, out, plan, error);
        case Kind::Folder: return ChangesFromFolder(base, r.path, out, plan, error);
        case Kind::PatchedIso: return ChangesFromImage(base, r.path, out, plan, error);
        case Kind::Xdelta:
        case Kind::Ppf:
        case Kind::Bps: return ChangesFromImage(base, image, out, plan, error);
        case Kind::Unknown: break;
    }
    return Fail(error, r.why);
}

}  // namespace

const char* SourceKindName(Kind kind) {
    switch (kind) {
        case Kind::ModCrate: return "modcrate";
        case Kind::PatchedIso: return "iso";
        case Kind::Xdelta: return "xdelta";
        case Kind::Ppf: return "ppf";
        case Kind::Bps: return "bps";
        case Kind::Folder: return "folder";
        case Kind::Unknown: break;
    }
    return "";
}

const char* KindLabel(Kind kind) {
    switch (kind) {
        case Kind::ModCrate: return "a Mod Crate";
        case Kind::PatchedIso: return "a modified disc image";
        case Kind::Xdelta: return "an xdelta patch";
        case Kind::Ppf: return "a PPF patch";
        case Kind::Bps: return "a BPS patch";
        case Kind::Folder: return "a folder of files";
        case Kind::Unknown: break;
    }
    return "not a mod";
}

Kind Detect(const std::string& artefact_path, std::string* why) {
    try {
        Resolved r;
        std::string error;
        Resolve(artefact_path, "", &r, nullptr, &error);
        if (why) *why = r.why;
        return r.kind;
    } catch (const std::exception& e) {
        if (why) *why = std::string("it cannot be read (") + e.what() + ")";
        return Kind::Unknown;
    }
}

std::string ModIdFor(const std::string& artefact_path, std::string* name) {
    try {
        Resolved r;
        std::string error;
        Resolve(artefact_path, "", &r, nullptr, &error);
        if (name) name->clear();
        if (r.kind == Kind::Unknown) return "";
        if (r.mod_folder) {
            if (name) *name = r.name;
            return r.mod_id;
        }
        const std::string n = ModName(r, artefact_path);
        if (name) *name = n;
        return MakeId(n);
    } catch (const std::exception&) {
        return "";
    }
}

bool Import(const std::string& artefact_path, const std::string& base_iso_path, const std::string& mods_dir, const Progress& progress,
            Report* report, std::string* error) {
    try {
        if (report) *report = Report{};
        if (mods_dir.empty() || !io::MakeDirs(mods_dir)) return Fail(error, "The mods folder cannot be made (" + mods_dir + ").");
        Job job(mods_dir);
        if (!job.Ok()) return Fail(error, "The mods folder cannot be written (" + mods_dir + ").");
        // what it is; a zip is unpacked into the job's folder as the first stage
        Resolved r;
        std::string why;
        Resolve(artefact_path, "", &r, nullptr, &why);
        if (r.kind == Kind::Unknown) return Fail(error, Sentence("This is not a mod the game can take: it is " + r.why));
        if (r.mod_folder) {
            // already a mod folder: copied whole (staged, then moved into place; the same id is replaced)
            const std::string final_dir = io::Join(mods_dir, r.mod_id);
            std::error_code ec;
            const bool same = std::filesystem::equivalent(io::Path(artefact_path), io::Path(final_dir), ec);
            bool replacing = false;
            if (!same && !CheckReplace(mods_dir, r.mod_id, &replacing, error)) return false;
            if (!same) {
                const std::string staged = job.Path("mod");
                std::filesystem::copy(io::Path(artefact_path), io::Path(staged), std::filesystem::copy_options::recursive, ec);
                if (ec) return Fail(error, "The mod folder cannot be copied (" + ec.message() + ").");
                if (io::IsDir(final_dir) && !io::Rename(final_dir, job.Path("old")))
                    return Fail(error, "The mod's earlier folder cannot be replaced (" + final_dir + "): is it open?");
                if (!io::Rename(staged, final_dir)) return Fail(error, "The mod cannot be moved into the mods folder (" + final_dir + ").");
            }
            Report out;
            out.id = r.mod_id;
            out.name = r.name;
            out.replaced = replacing;
            out.mod_json_path = io::Join(final_dir, "mod.json");
            std::vector<std::string> rels;
            Disc disc;
            const bool have_disc = disc.Open(base_iso_path, nullptr);
            if (io::ListFiles(final_dir, &rels))
                for (const std::string& rel : rels) {
                    io::File f;
                    if (f.OpenRead(io::Join(final_dir, rel))) out.bytes_written += f.Size();
                    if (rel.compare(0, 5, "disc/") == 0) {
                        std::string p = Upper(rel.substr(5));
                        if (p.size() > 7 && p.compare(p.size() - 7, 7, ".RANGES") == 0) p.resize(p.size() - 7);
                        (have_disc && !disc.Find(p) ? out.files_added : out.files_changed).push_back(p);
                    }
                }
            std::string text;
            json::Value m;
            if (io::ReadWhole(out.mod_json_path, 1u << 20, &text) && json::Parse(text, &m, nullptr))
                if (const json::Value* x = m.Get("executable"); x && x->Get("code_changes") && x->Get("code_changes")->IsArray())
                    for (const json::Value& c : x->Get("code_changes")->items)
                        if (c.IsString()) out.code_changes.push_back(c.text);
            out.code_changes_unsupported = !out.code_changes.empty();
            if (progress.report) progress.report("Writing the mod", 1.0);
            if (report) *report = std::move(out);
            return true;
        }
        // its name and id, known now (ModIdFor answers the same): a mod of that id is replaced
        const std::string mod_name = ModName(r, artefact_path);
        const std::string id = MakeId(mod_name);
        bool replace = false;
        if (!CheckReplace(mods_dir, id, &replace, error)) return false;
        const bool zipped = r.zip;
        const bool patch = r.kind == Kind::Xdelta || r.kind == Kind::Ppf || r.kind == Kind::Bps;
        std::vector<std::pair<std::string, double>> stages;
        if (zipped) stages.push_back({"Unpacking", 0.1});
        stages.push_back({"Reading your disc", 0.25});
        if (patch) {
            // the space a patch needs is said as it is applied (and checked before)
            io::File disc_file;
            const uint64_t image = disc_file.OpenRead(base_iso_path) ? disc_file.Size() : 0;
            stages.push_back({"Applying the patch (to a temporary " + Gb(image) + " copy of your disc; " + Gb(io::FreeBytes(mods_dir)) + " free)", 0.25});
        }
        if (r.kind == Kind::PatchedIso && !zipped) stages.push_back({"Reading the modified image", 0.15});
        stages.push_back({"Comparing with your disc", 0.25});
        stages.push_back({"Writing the mod", 0.25});
        Plan plan(progress, stages);
        size_t stage = 0;
        if (zipped) {
            plan.Enter(stage++);
            if (!Resolve(artefact_path, job.Path("unpacked"), &r, &plan, &why)) return Fail(error, Sentence(why));
            if (r.kind == Kind::Unknown) return Fail(error, Sentence("This is not a mod the game can take: it is " + r.why));
        }
        Disc base;
        if (!base.Open(base_iso_path, &why)) return Fail(error, Sentence("Your disc image cannot be read: " + why));
        if (base.Executable().empty()) return Fail(error, "Your disc image is not a PlayStation 2 disc (it has no SYSTEM.CNF).");
        plan.Enter(stage++);
        DiscRecord record;
        if (!record.Identify(base, mods_dir, &plan, &why)) return Fail(error, Sentence(why));
        // a patch: applied to a temporary copy of the disc
        std::string image;
        if (patch) {
            plan.Enter(stage++);
            PatchHeader header;
            if (!ReadPatchHeader(FormatOf(r.kind), r.path, &header, &why)) return Fail(error, Sentence(why));
            if (!CheckPatchSource(header, r.path, base_iso_path, &why)) return Fail(error, Sentence(why));
            const uint64_t need = std::max<uint64_t>(header.target_bytes, record.size) + (256u << 20);
            const uint64_t free_bytes = io::FreeBytes(mods_dir);
            if (free_bytes && need > free_bytes)
                return Fail(error, "Applying this patch needs " + Gb(need) + " of free space beside the mods folder for a while; " +
                                       Gb(free_bytes) + " are free.");
            image = job.Path("patched.iso");
            if (!ApplyPatch(FormatOf(r.kind), r.path, base_iso_path, image, [&](double f) { return plan.Step(f); }, &header, &why))
                return Fail(error, Sentence(why));
        }
        // the artefact's own sha256, as it was given (a zip, a patch, an image, a folder's files)
        std::string sha;
        {
            Progress none;
            none.cancelled = progress.cancelled;
            Plan quiet(none, {{"", 1.0}});
            const bool shown = r.kind == Kind::PatchedIso && !zipped;
            if (shown) plan.Enter(stage++);
            if (!ArtefactSha256(artefact_path, shown ? &plan : &quiet, &sha))
                return Fail(error, plan.Cancelled() ? "The import was stopped." : "The mod cannot be read whole.");
        }
        plan.Enter(stage++);
        ChangeSet changes;
        if (!Changes(base, r, image, &changes, &plan, &why)) return Fail(error, Sentence(why));
        if (changes.files.empty()) return Fail(error, "It changes nothing on your disc: there is nothing to add.");
        changes.name = mod_name;
        plan.Enter(stage++);
        SourceRecord source{SourceKindName(r.kind), FileName(artefact_path), sha};
        const std::string staged = job.Path("mod");
        Report out;
        if (!WriteModFolder(changes, base, &record, staged, id, source, &plan, &out, &why)) return Fail(error, Sentence(why));
        // into place: the new folder, then the old one (an import of the same artefact again) removed
        const std::string final_dir = io::Join(mods_dir, id);
        if (!CheckReplace(mods_dir, id, &replace, error)) return false;   // the folder as it is now
        if (replace) {
            const std::string old = job.Path("old");
            if (!io::Rename(final_dir, old)) return Fail(error, "The mod's earlier folder cannot be replaced (" + final_dir + "): is it open?");
        }
        if (!io::Rename(staged, final_dir)) return Fail(error, "The mod cannot be moved into the mods folder (" + final_dir + ").");
        out.mod_json_path = io::Join(final_dir, "mod.json");
        out.replaced = replace;
        plan.Step(1.0);
        if (report) *report = std::move(out);
        return true;
    } catch (const std::exception& e) {
        return Fail(error, std::string("The import failed: ") + e.what() + ".");
    }
}

bool Inspect(const std::string& artefact_path, const std::string& base_iso_path, const Progress& progress, Inspection* out,
             std::string* error) {
    try {
        *out = Inspection{};
        std::error_code ec;
        const std::string tmp = io::Utf8(std::filesystem::temp_directory_path(ec)) + "/twinsanityevolved-inspect-" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        struct Cleanup { std::string d; ~Cleanup() { io::RemoveAll(d); } } cleanup{tmp};
        Plan plan(progress, {{"Unpacking", 0.3}, {"Comparing with your disc", 0.7}});
        plan.Enter(0);
        Resolved r;
        std::string why;
        io::MakeDirs(tmp);
        if (!Resolve(artefact_path, io::Join(tmp, "unpacked"), &r, &plan, &why)) return Fail(error, Sentence(why));
        out->kind = r.kind;
        out->detected = r.why;
        out->name = r.name;
        if (r.kind == Kind::Unknown) {
            out->refusal = Sentence("This is not a mod the game can take: it is " + r.why);
            return true;
        }
        if (r.mod_folder) {
            std::vector<std::string> rels;
            if (io::ListFiles(artefact_path, &rels))
                for (const std::string& rel : rels)
                    if (rel.compare(0, 5, "disc/") == 0) {
                        std::string p = Upper(rel.substr(5));
                        if (p.size() > 7 && p.compare(p.size() - 7, 7, ".RANGES") == 0) p.resize(p.size() - 7);
                        out->files_changed.push_back(p);
                    }
            out->importable = true;
            return true;
        }
        Disc base;
        if (!base.Open(base_iso_path, &why)) return Fail(error, Sentence("Your disc image cannot be read: " + why));
        plan.Enter(1);
        if (r.kind == Kind::Xdelta || r.kind == Kind::Ppf || r.kind == Kind::Bps) {
            PatchHeader h;
            if (!ReadPatchHeader(FormatOf(r.kind), r.path, &h, &why)) { out->refusal = Sentence(why); return true; }
            out->patch_format = h.label;
            out->description = h.description;
            out->needs_apply = true;
            io::File disc;
            disc.OpenRead(base_iso_path);
            out->temporary_bytes = std::max<uint64_t>(h.target_bytes, disc.Size());
            if (!CheckPatchSource(h, r.path, base_iso_path, &why)) { out->refusal = Sentence(why); return true; }
            if (!h.secondary.empty())
                out->warnings.push_back("it declares xdelta3's secondary compression (" + h.secondary + "): it can be imported only if no part of it uses it");
            out->importable = true;
            return true;
        }
        ChangeSet c;
        if (!Changes(base, r, "", &c, &plan, &why)) { out->refusal = Sentence(why); return true; }
        out->name = c.name.empty() ? out->name : c.name;
        out->author = c.author;
        out->version = c.version;
        out->description = c.description;
        out->notes = c.notes;
        for (const OutFile& f : c.files) (f.form == OutFile::Form::Add ? out->files_added : out->files_changed).push_back(f.path);
        out->files_removed = c.removed;
        out->warnings = c.warnings;
        out->archive_entries_changed = c.archive.changed_names;
        out->archive_entries_added = c.archive.added_names;
        if (c.executable_changed) {
            out->code_changes = c.executable.functions;
            out->data_changes = {{".data", c.executable.data}, {".rodata", c.executable.rodata}, {".sdata", c.executable.sdata}, {"vu", c.executable.vu}};
            for (const std::string& w : c.executable.warnings) out->warnings.push_back("SLES_525.68: " + w);
        }
        out->importable = !c.files.empty();
        if (!out->importable) out->refusal = "It changes nothing on your disc.";
        plan.Step(1.0);
        return true;
    } catch (const std::exception& e) {
        return Fail(error, std::string("The mod cannot be read: ") + e.what() + ".");
    }
}

bool Verify(const std::string& mod_dir, const std::string& base_iso_path, const Progress& progress, Verification* out,
            std::string* error) {
    try {
        *out = Verification{};
        std::string text, why;
        json::Value m;
        if (!io::ReadWhole(io::Join(mod_dir, "mod.json"), 1u << 20, &text)) return Fail(error, "There is no mod.json in " + mod_dir + ".");
        if (!json::Parse(text, &m, &why) || !m.IsObject()) return Fail(error, "Its mod.json does not read (" + why + ").");
        out->id = m.String("id", FileName(mod_dir));
        DiscOverlay::Layer layer{out->id, mod_dir, {}};
        const json::Value* files = m.Get("files");
        if (files && files->IsArray())
            for (const json::Value& f : files->items)
                if (f.String("form") == "ranges" && f.Unsigned("base_bytes")) {
                    DiscOverlay::BaseCheck c;
                    c.path = f.String("path");
                    c.bytes = f.Unsigned("base_bytes");
                    c.sha256 = f.String("base_sha256");
                    c.sample_sha256 = f.String("base_sample_sha256");
                    c.sample_windows = uint32_t(f.Unsigned("base_sample_windows"));
                    c.sample_window_bytes = f.Unsigned("base_sample_window_bytes");
                    layer.checks.push_back(c);
                }
        DiscOverlay volume({layer});
        if (!volume.Open(base_iso_path, &why)) return Fail(error, "Your disc image cannot be read (" + why + ").");
        for (const std::string& r : volume.GetReport().refused) out->problems.push_back("the game would leave it out: " + r);
        uint64_t total = 0;
        if (files && files->IsArray())
            for (const json::Value& f : files->items) total += f.Unsigned("bytes");
        Plan plan(progress, {{"Checking the mod", 1.0}});
        plan.Enter(0);
        uint64_t done = 0;
        std::vector<uint8_t> buf(1u << 20);
        if (files && files->IsArray())
            for (const json::Value& f : files->items) {
                VerifyRow row;
                row.path = f.String("path");
                row.form = f.String("form");
                row.recorded_bytes = f.Unsigned("bytes");
                row.recorded_sha256 = Lower(f.String("sha256"));
                IsoFs::Entry e;
                if (!volume.Find(row.path, e) || e.directory) {
                    row.note = "not on the composed disc";
                    out->files.push_back(row);
                    continue;
                }
                row.bytes = e.size;
                ps2r::hash::Sha256 h;
                bool read_ok = true;
                for (uint64_t k = 0; k < e.size;) {
                    const size_t n = size_t(std::min<uint64_t>(buf.size(), e.size - k));
                    if (volume.ReadBytes(uint64_t(e.lba) * IsoFs::kSectorSize + k, n, buf.data()) != n) { read_ok = false; break; }
                    h.Update(buf.data(), n);
                    k += n;
                    done += n;
                    if (!plan.Step(total ? double(done) / double(total) : 1.0)) return Fail(error, "The check was stopped.");
                }
                uint8_t d[32];
                h.Finish(d);
                row.sha256 = read_ok ? ps2r::hash::Sha256::Hex(d) : "";
                row.ok = read_ok && row.bytes == row.recorded_bytes && (row.recorded_sha256.empty() || row.sha256 == row.recorded_sha256);
                if (!read_ok) row.note = "does not read whole";
                else if (row.bytes != row.recorded_bytes) row.note = "composes to " + std::to_string(row.bytes) + " bytes, mod.json says " + std::to_string(row.recorded_bytes);
                else if (!row.ok) row.note = "composes to other bytes than mod.json records";
                out->files.push_back(row);
            }
        out->ok = out->problems.empty() && !out->files.empty();
        for (const VerifyRow& row : out->files) {
            if (!row.ok) {
                out->ok = false;
                out->problems.push_back(row.path + ": " + row.note);
            }
        }
        if (out->files.empty()) out->problems.push_back("its mod.json lists no files");
        return true;
    } catch (const std::exception& e) {
        return Fail(error, std::string("The check failed: ") + e.what() + ".");
    }
}

}  // namespace te::modimport
