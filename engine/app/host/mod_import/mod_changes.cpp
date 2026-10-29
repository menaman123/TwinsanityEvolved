// mod_internal.h's second half: what an artefact changes on the disc (ChangesFrom*), and the mod folder written from it.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include "app/host/mod_import/hashes.h"
#include "app/host/mod_import/io.h"
#include "app/host/mod_import/json.h"
#include "app/host/mod_import/mod_internal.h"
#include "engine/cdvd/disc_overlay.h"
#include "engine/core/hash/sha256.h"

namespace te::modimport::detail {

namespace {

// The title's archive: the index and the data it locates
const char* const kArchiveIndex = "CRASH6/CRASH.BH";
const char* const kArchiveData = "CRASH6/CRASH.BD";
constexpr uint64_t kRecipeAbove = 1u << 20;   // a changed file larger than this becomes a recipe when that saves a quarter
constexpr size_t kChunk = 1u << 20;
constexpr uint32_t kMaxArchiveName = 0x3FF;   // the game's name buffer (game/src/game/archive.cpp PathBufferSize, with its NUL)

bool Fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

bool Equal(ByteSource& a, ByteSource& b, Plan* plan) {
    if (a.Size() != b.Size()) return false;
    std::vector<uint8_t> x(kChunk), y(kChunk);
    for (uint64_t done = 0; done < a.Size();) {
        const size_t n = size_t(std::min<uint64_t>(kChunk, a.Size() - done));
        if (!a.Read(done, x.data(), n) || !b.Read(done, y.data(), n)) return false;
        if (std::memcmp(x.data(), y.data(), n) != 0) return false;
        done += n;
        if (plan && plan->Cancelled()) return false;
    }
    return true;
}

bool ReadAll(ByteSource& s, std::vector<uint8_t>* out) {
    out->resize(size_t(s.Size()));
    return out->empty() || s.Read(0, out->data(), out->size());
}

DiffControl Control(Plan* plan) {
    DiffControl c;
    c.tick = [plan](uint64_t) { return !plan || plan->Step(-1); };
    return c;
}

// A changed (or added) disc file: whole, or a recipe of the disc's bytes when that saves a quarter of a large file
bool AddFile(const Disc& base, const std::string& path, ByteSource* target, ChangeSet* out, Plan* plan, std::string* error) {
    const DiscFile* b = base.Find(path);
    OutFile f;
    f.path = b ? b->path : Upper(path);
    if (!b) {
        f.form = OutFile::Form::Add;
        f.whole = target;
        out->files.push_back(std::move(f));
        return true;
    }
    f.base = b;
    std::unique_ptr<ByteSource> disc_file = base.Source(*b);
    if (Upper(b->path) == Upper(base.Executable())) {
        std::vector<uint8_t> mine, theirs;
        if (!ReadAll(*target, &mine) || !base.ReadFile(*b, &theirs)) return Fail(error, path + " does not read");
        if (mine == theirs) { out->unchanged.push_back(f.path); return true; }
        std::string why;
        if (!DiffExecutables(theirs, mine, &out->executable, &why)) return Fail(error, why);
        out->executable_changed = true;
        f.form = OutFile::Form::Replace;
        f.whole = target;
        if (!out->executable.restored.empty()) {
            // the words that would make the game call the mod's own code keep the disc's values
            for (const ExecutableDiff::Restore& r : out->executable.restored)
                std::memcpy(&mine[size_t(r.file_offset)], &theirs[size_t(r.file_offset)], 4);
            f.whole = out->Own(std::unique_ptr<ByteSource>(new MemorySource(std::move(mine))));
        }
        out->files.push_back(std::move(f));
        return true;
    }
    if (target->Size() <= kRecipeAbove || b->size == 0) {
        if (Equal(*target, *disc_file, plan)) { out->unchanged.push_back(f.path); return true; }
        if (plan && plan->Cancelled()) return Fail(error, "stopped");
        f.form = OutFile::Form::Replace;
        f.whole = target;
        out->files.push_back(std::move(f));
        return true;
    }
    Recipe r;
    ByteSource* base_src = out->Own(std::move(disc_file));
    if (!SameOffsetDiff(*target, 0, target->Size(), *base_src, 0, b->size, &r, Control(plan)))
        return Fail(error, plan && plan->Cancelled() ? "stopped" : path + " does not read");
    if (r.DataBytes() == 0 && r.Size() == b->size) { out->unchanged.push_back(f.path); return true; }
    if (r.DataBytes() * 4 <= target->Size() * 3) {
        f.form = OutFile::Form::Ranges;
        f.recipe = std::move(r);
    } else {
        f.form = OutFile::Form::Replace;
        f.whole = target;
    }
    out->files.push_back(std::move(f));
    return true;
}

// The archive pair: the index whole (when it changed), the data as a recipe over the disc's through both indexes
bool AddArchive(const Disc& base, std::vector<uint8_t> target_bh, ByteSource* target_bd, ChangeSet* out, Plan* plan, std::string* error) {
    const DiscFile* bh = base.Find(kArchiveIndex);
    const DiscFile* bd = base.Find(kArchiveData);
    BhIndex mine, theirs;
    std::string why;
    std::vector<uint8_t> base_bh;
    const bool parsed = bh && bd && base.ReadFile(*bh, &base_bh) && ParseBh(base_bh, &theirs, &why) && ParseBh(target_bh, &mine, &why);
    ByteSource* bh_src = out->Own(std::unique_ptr<ByteSource>(new MemorySource(target_bh)));
    if (!parsed) {
        // not the archive this importer knows: the two files as any other files
        out->warnings.push_back("the archive index does not read as this game's (" + why + "): its files are compared whole");
        if (!AddFile(base, kArchiveIndex, bh_src, out, plan, error)) return false;
        return target_bd ? AddFile(base, kArchiveData, target_bd, out, plan, error) : true;
    }
    if (target_bh != base_bh) {
        OutFile f;
        f.path = bh->path;
        f.form = OutFile::Form::Replace;
        f.whole = bh_src;
        f.base = bh;
        out->files.push_back(std::move(f));
    }
    ByteSource* base_bd = out->Own(base.Source(*bd));
    Recipe r;
    ArchiveDiffStats stats;
    if (!ArchiveDiff(mine, *target_bd, theirs, *base_bd, &r, &stats, Control(plan)))
        return Fail(error, plan && plan->Cancelled() ? "stopped" : "the archive does not read");
    out->archive = stats;
    if (r.DataBytes() == 0 && r.Size() == bd->size && r.Segments().size() == 1 && r.Segments()[0].base_offset == 0) {
        out->unchanged.push_back(bd->path);
        return true;
    }
    out->archive_changed = true;
    OutFile f;
    f.path = bd->path;
    f.form = OutFile::Form::Ranges;
    f.recipe = std::move(r);
    f.base = bd;
    out->files.push_back(std::move(f));
    if (stats.removed)
        out->warnings.push_back(std::to_string(stats.removed) + " entr" + (stats.removed == 1 ? "y" : "ies") +
                                " of the disc's archive are not in the mod's (the mod's index is used as it is)");
    return true;
}

bool IsArchivePath(const std::string& p) {
    const std::string u = Upper(p);
    return u == kArchiveIndex || u == kArchiveData;
}

// The archive `cur` (its index's bytes and its data) with the entries `repl` names replaced (archive key -> the new bytes)
// and the keys `cur` lacks added after the others by name (`shown`: the names to record), compared with the disc's archive
// (AddArchive: the new index whole, the data a recipe) -- a Mod Crate's layer1, and a folder's loose entries
bool RebuildArchive(const Disc& base, const std::vector<uint8_t>& cur_bh_bytes, ByteSource* cur_bd, const std::map<std::string, ByteSource*>& repl,
                    const std::map<std::string, std::string>& shown, ChangeSet* out, Plan* plan, std::string* error) {
    BhIndex cur;
    std::string why;
    if (!ParseBh(cur_bh_bytes, &cur, &why)) return Fail(error, "the archive index does not read as this game's (" + why + ")");
    // pack: the current entries in data order (each replaced one from the mod), then the added ones by name
    std::vector<size_t> order(cur.entries.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cur.entries[a].offset < cur.entries[b].offset; });
    auto comp = std::make_unique<CompositeSource>();
    BhIndex next;
    next.header = cur.header;
    next.entries = cur.entries;
    std::set<std::string> used;
    for (size_t i : order) {
        BhEntry& e = next.entries[i];
        const std::string key = ArchiveKey(e.name);
        const auto r = repl.find(key);
        const uint64_t at = comp->Size();
        if (r != repl.end()) {
            comp->Append(r->second, 0, r->second->Size());
            e.size = uint32_t(r->second->Size());
            used.insert(key);
        } else {
            if (uint64_t(e.offset) + e.size > cur_bd->Size()) return Fail(error, "the archive index names bytes past the end of the archive (" + e.name + ")");
            comp->Append(cur_bd, e.offset, e.size);
        }
        if (at > 0xFFFFFFFFull) return Fail(error, "the rebuilt archive would be larger than 4 GB");
        e.offset = uint32_t(at);
    }
    for (const auto& kv : repl) {
        if (used.count(kv.first)) continue;
        const uint64_t at = comp->Size();
        if (at + kv.second->Size() > 0xFFFFFFFFull) return Fail(error, "the rebuilt archive would be larger than 4 GB");
        comp->Append(kv.second, 0, kv.second->Size());
        const auto s = shown.find(kv.first);
        next.entries.push_back(BhEntry{s == shown.end() ? kv.first : s->second, uint32_t(at), uint32_t(kv.second->Size())});
    }
    ByteSource* target_bd = out->Own(std::move(comp));
    return AddArchive(base, WriteBh(next), target_bd, out, plan, error);
}

// Notes a mod folder may carry beside its files (never a disc file)
bool NoteLike(const std::string& rel) {
    const std::string l = Lower(rel);
    const size_t dot = l.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : l.substr(dot + 1);
    static const std::set<std::string> notes = {"txt", "md", "nfo", "diz", "pdf", "png", "jpg", "jpeg", "gif", "bmp", "webp",
                                                "url", "html", "htm", "rtf", "doc", "docx", "odt", "lnk", "ico"};
    return notes.count(ext) != 0;
}

std::string ParentOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool IsDiscDirectory(const Disc& base, const std::string& dir) {
    if (dir.empty()) return true;
    const std::string u = Upper(dir) + "/";
    for (const DiscFile& f : base.Files())
        if (f.path.compare(0, u.size(), u) == 0) return true;
    return false;
}

// A table of another release than the disc's (defined with the loose files' rules below)
bool OtherRelease(const Disc& base, const ReleaseArchive& r);

}  // namespace

std::map<std::string, std::string> ReadCrateInfo(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t at = 0;
    if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF) at = 3;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(at, eol - at);
        at = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '!') continue;   // Crate Mod Loader's comment symbol
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
    return out;
}

std::vector<std::string> ReadCrateNotes(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF) at = 3;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(at, eol - at);
        at = eol + 1;
        if (line.empty() || line[0] != '!') continue;
        line = Trim(line.substr(1));
        if (!line.empty() && out.size() < 64) out.push_back(line.size() > 1000 ? line.substr(0, 1000) : line);
    }
    return out;
}

std::vector<std::string> RequestedSettings(const std::vector<std::string>& notes) {
    std::vector<std::string> out;
    for (const std::string& note : notes) {
        const std::string l = Lower(note);
        // "apply" as a word, followed by what to apply
        size_t at = std::string::npos;
        for (size_t p = l.find("apply "); p != std::string::npos; p = l.find("apply ", p + 1))
            if (p == 0 || !((l[p - 1] >= 'a' && l[p - 1] <= 'z') || (l[p - 1] >= '0' && l[p - 1] <= '9'))) { at = p + 6; break; }
        if (at == std::string::npos) continue;
        std::string rest = note.substr(at);
        const size_t end = rest.find_first_of(".!?;:");
        if (end != std::string::npos) rest.resize(end);
        // split at commas and the word "and"
        std::vector<std::string> parts;
        std::string cur;
        auto flush = [&] { const std::string t = Trim(cur); if (!t.empty()) parts.push_back(t); cur.clear(); };
        for (size_t i = 0; i < rest.size(); ++i) {
            if (rest[i] == ',') { flush(); continue; }
            if (Lower(rest.substr(i, 5)) == " and " ) { flush(); i += 4; continue; }
            if (Lower(rest.substr(i, 4)) == "and " && (i == 0 || rest[i - 1] == ',')) { flush(); i += 3; continue; }
            cur.push_back(rest[i]);
        }
        flush();
        for (const std::string& p : parts) {
            const char c = p[0];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) continue;   // a setting's name, not prose
            if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
        }
    }
    return out;
}

std::string CrateRoot(const std::string& dir, bool* has_layers) {
    if (has_layers) *has_layers = false;
    std::error_code ec;
    auto scan = [&](const std::string& d, bool* info, bool* layers) {
        *info = *layers = false;
        for (const auto& e : std::filesystem::directory_iterator(io::Path(d), ec)) {
            const std::string name = Lower(io::Utf8(e.path().filename()));
            if (name == "modcrateinfo.txt" && e.is_regular_file(ec)) *info = true;
            if (e.is_directory(ec) && name.size() > 5 && name.compare(0, 5, "layer") == 0 &&
                name.find_first_not_of("0123456789", 5) == std::string::npos)
                *layers = true;
        }
    };
    bool info = false, layers = false;
    scan(dir, &info, &layers);
    if (info || layers) {
        if (has_layers) *has_layers = layers;
        return dir;
    }
    // one folder deep (a zip of a folder): its only sub-folder
    std::vector<std::string> subs;
    for (const auto& e : std::filesystem::directory_iterator(io::Path(dir), ec))
        if (e.is_directory(ec) && !io::HostClutter(io::Utf8(e.path().filename()))) subs.push_back(io::Utf8(e.path()));
    if (subs.size() == 1) {
        scan(subs[0], &info, &layers);
        if (info || layers) {
            if (has_layers) *has_layers = layers;
            return subs[0];
        }
    }
    return "";
}

bool ChangesFromImage(const Disc& base, const std::string& image_path, ChangeSet* out, Plan* plan, std::string* error) {
    auto img = std::make_shared<Disc>();
    std::string why;
    if (!img->Open(image_path, &why)) return Fail(error, "the modified image does not read: " + why);
    out->images.push_back(img);
    if (!img->Executable().empty() && !base.Executable().empty() && Upper(img->Executable()) != Upper(base.Executable()))
        out->warnings.push_back("the modified image starts " + img->Executable() + ", your disc " + base.Executable() +
                                ": it is another version of the game, and its files are compared with yours as they are");
    std::set<std::string> seen;
    for (const DiscFile& f : img->Files()) {
        seen.insert(f.path);
        if (IsArchivePath(f.path)) continue;
        ByteSource* src = out->Own(img->Source(f));
        if (!AddFile(base, f.path, src, out, plan, error)) return false;
    }
    const DiscFile* ibh = img->Find(kArchiveIndex);
    const DiscFile* ibd = img->Find(kArchiveData);
    if (ibh && ibd) {
        std::vector<uint8_t> bh;
        if (!img->ReadFile(*ibh, &bh)) return Fail(error, "the modified image's archive index does not read");
        if (!AddArchive(base, bh, out->Own(img->Source(*ibd)), out, plan, error)) return false;
    } else {
        for (const DiscFile* f : {ibh, ibd})
            if (f && !AddFile(base, f->path, out->Own(img->Source(*f)), out, plan, error)) return false;
    }
    for (const DiscFile& f : base.Files())
        if (!seen.count(f.path)) out->removed.push_back(f.path);
    if (!out->removed.empty())
        out->warnings.push_back(std::to_string(out->removed.size()) + " file(s) of your disc are not on the modified image (" +
                                out->removed.front() + (out->removed.size() > 1 ? ", ..." : "") +
                                "): a mod cannot remove a file, so the game keeps your disc's");
    return true;
}

bool ChangesFromCrate(const Disc& base, const std::string& crate_dir, ChangeSet* out, Plan* plan, std::string* error) {
    bool has_layers = false;
    const std::string root = CrateRoot(crate_dir, &has_layers);
    if (root.empty()) return Fail(error, "it is not a Mod Crate (it has no ModCrateInfo.txt and no layer folders)");
    std::map<std::string, std::string> info;
    std::error_code ec;
    std::string settings, scripts;
    std::map<std::string, std::string> layers;   // lower-case name -> the folder's own name
    for (const auto& e : std::filesystem::directory_iterator(io::Path(root), ec)) {
        const std::string name = io::Utf8(e.path().filename()), l = Lower(name);
        if (l == "modcrateinfo.txt") {
            std::string text;
            if (io::ReadWhole(io::Utf8(e.path()), 1u << 20, &text)) {
                info = ReadCrateInfo(text);
                out->notes = ReadCrateNotes(text);
                out->game_code_settings = RequestedSettings(out->notes);
            }
        } else if (l == "modcratesettings.txt") {
            settings = name;
        } else if (e.is_directory(ec) && l.find("script") != std::string::npos) {
            scripts = name;
        } else if (e.is_directory(ec) && l.size() > 5 && l.compare(0, 5, "layer") == 0) {
            layers[l] = name;
        }
    }
    out->name = info.count("name") ? info["name"] : FileName(root);
    out->author = info.count("author") ? info["author"] : "";
    out->version = info.count("version") ? info["version"] : "";
    out->description = info.count("description") ? info["description"] : "";
    const std::string game = Lower(info.count("game") ? info["game"] : "");
    if (!game.empty() && game != "crashts" && game != "all")
        return Fail(error, out->name + " is a Mod Crate for another game (" + info["game"] + "), not Crash Twinsanity");
    if (!settings.empty())
        return Fail(error, out->name + " changes the game's code settings (Crate Mod Loader properties), which this port cannot apply");
    if (!scripts.empty())
        return Fail(error, out->name + " carries scripts for Crate Mod Loader's own code (its " + scripts + " folder), which this port cannot run");
    for (const auto& l : layers)
        if (l.first != "layer0" && l.first != "layer1")
            return Fail(error, out->name + " has a " + l.second + " folder, which Crate Mod Loader does not use for this game (layer0: the "
                                           "disc's files, layer1: the archive's): it cannot be imported");
    if (info.empty()) out->warnings.push_back("it has no ModCrateInfo.txt: named after its folder");
    if (!out->game_code_settings.empty()) {
        std::string list;
        for (const std::string& s : out->game_code_settings) list += (list.empty() ? "" : ", ") + s;
        out->warnings.push_back("this mod asks for game-code settings of Crate Mod Loader (" + list + ") that this port cannot apply");
    }
    // layer0: the disc's files
    const std::string l0 = layers.count("layer0") ? io::Join(root, layers["layer0"]) : std::string();
    const std::string l1 = layers.count("layer1") ? io::Join(root, layers["layer1"]) : std::string();
    std::vector<std::string> files0, files1;
    if (!l0.empty() && !io::ListFiles(l0, &files0)) return Fail(error, "its layer0 folder does not list");
    if (!l1.empty() && !io::ListFiles(l1, &files1)) return Fail(error, "its layer1 folder does not list");
    if (files0.empty() && files1.empty()) return Fail(error, out->name + " has no files to apply (its layer folders are empty)");
    ByteSource* l0_bh = nullptr;
    ByteSource* l0_bd = nullptr;
    for (const std::string& rel : files0) {
        auto src = std::make_unique<HostFileSource>();
        if (!src->Open(io::Join(l0, rel))) return Fail(error, "layer0/" + rel + " does not read");
        ByteSource* s = out->Own(std::move(src));
        const std::string u = Upper(rel);
        if (u == kArchiveIndex) { l0_bh = s; continue; }
        if (u == kArchiveData) { l0_bd = s; continue; }
        if (!AddFile(base, rel, s, out, plan, error)) return false;
    }
    if (files1.empty()) {
        if (l0_bh && l0_bd) {
            std::vector<uint8_t> bh;
            if (!ReadAll(*l0_bh, &bh)) return Fail(error, "layer0's CRASH.BH does not read");
            return AddArchive(base, bh, l0_bd, out, plan, error);
        }
        for (ByteSource* s : {l0_bh, l0_bd})
            if (s && !AddFile(base, s == l0_bh ? kArchiveIndex : kArchiveData, s, out, plan, error)) return false;
        return true;
    }
    // layer1: entries of the archive by their index path -- the archive rebuilt over the layer0 one (or the disc's)
    const DiscFile* dbh = base.Find(kArchiveIndex);
    const DiscFile* dbd = base.Find(kArchiveData);
    if ((!l0_bh || !l0_bd) && (!dbh || !dbd)) return Fail(error, "your disc has no CRASH6/CRASH.BH and CRASH.BD for its layer1 files");
    std::vector<uint8_t> cur_bh_bytes;
    ByteSource* cur_bd = nullptr;
    if (l0_bh && l0_bd) {
        if (!ReadAll(*l0_bh, &cur_bh_bytes)) return Fail(error, "layer0's CRASH.BH does not read");
        cur_bd = l0_bd;
    } else {
        if (!base.ReadFile(*dbh, &cur_bh_bytes)) return Fail(error, "your disc's archive index does not read");
        cur_bd = out->Own(base.Source(*dbd));
    }
    std::map<std::string, ByteSource*> repl;    // archive key -> the crate's file
    std::map<std::string, std::string> shown;   // key -> the name to record for an added entry ('\' separators)
    for (const std::string& rel : files1) {
        std::string name = rel;
        std::replace(name.begin(), name.end(), '/', '\\');
        if (name.size() > kMaxArchiveName) {
            out->warnings.push_back("layer1/" + rel + ": its name is longer than the game reads: not imported");
            continue;
        }
        auto src = std::make_unique<HostFileSource>();
        if (!src->Open(io::Join(l1, rel))) return Fail(error, "layer1/" + rel + " does not read");
        if (src->Size() > 0xFFFFFFFFull) return Fail(error, "layer1/" + rel + " is larger than the archive can hold");
        const std::string key = ArchiveKey(name);
        repl[key] = out->Own(std::move(src));
        shown[key] = name;
    }
    // entries this disc's archive lacks that another release's archive has: the crate was made for that release. A warning,
    // not a refusal: a crate may add entries of its own, and the ones it replaces still apply
    {
        BhIndex cur;
        std::string why;
        if (ParseBh(cur_bh_bytes, &cur, &why)) {
            std::set<std::string> have;
            for (const BhEntry& e : cur.entries) have.insert(ArchiveKey(e.name));
            for (const ReleaseArchive& r : KnownArchives()) {
                if (!OtherRelease(base, r)) continue;
                size_t n = 0;
                std::string first;
                for (const auto& kv : repl)
                    if (!have.count(kv.first) && r.entries.count(kv.first) && n++ == 0) first = r.entries.at(kv.first).first;
                if (n) {
                    out->warnings.push_back("it looks made for " + RegionDisc(r.region) + ": " + std::to_string(n) + " of its archive entries (" +
                                            first + (n > 1 ? ", ..." : "") + ") are entries of that disc's archive, not of this disc's");
                    break;
                }
            }
        }
    }
    return RebuildArchive(base, cur_bh_bytes, cur_bd, repl, shown, out, plan, error);
}

namespace {

// One name matched to one target, with the rule for two files that claim it: a match by the whole path wins over a match by
// the last name; two of a kind = which one is meant is unknown, so neither is taken
class Claims {
public:
    // false = not taken (`warnings` says why)
    bool Claim(const std::string& target, const std::string& rel, bool by_path, const std::string& shown, std::vector<std::string>* warnings) {
        if (conflicted_.count(target)) {
            warnings->push_back(rel + " is also " + shown + ": not imported");
            return false;
        }
        const auto have = mapped_.find(target);
        if (have != mapped_.end()) {
            if (exact_[target] && !by_path) {
                warnings->push_back(rel + ": " + have->second + " is already " + shown + ": not imported");
                return false;
            }
            if (by_path == exact_[target]) {
                warnings->push_back(rel + " and " + have->second + " are both " + shown + ": neither is imported");
                mapped_.erase(have);
                conflicted_.insert(target);
                return false;
            }
            warnings->push_back(have->second + ": " + rel + " is already " + shown + ": not imported");
        }
        mapped_[target] = rel;
        exact_[target] = by_path;
        return true;
    }
    const std::map<std::string, std::string>& Mapped() const { return mapped_; }   // target -> rel
    void Drop(const std::string& target) { mapped_.erase(target); }

private:
    std::map<std::string, std::string> mapped_;
    std::map<std::string, bool> exact_;
    std::set<std::string> conflicted_;
};

std::string LastName(const std::string& archive_name) {
    const size_t slash = archive_name.find_last_of("\\/");
    return slash == std::string::npos ? archive_name : archive_name.substr(slash + 1);
}

// A table of another release than the disc's (they boot another program)
bool OtherRelease(const Disc& base, const ReleaseArchive& r) { return Upper(r.executable) != Upper(FileName(base.Executable())); }

uint64_t Distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

// The bytes of `a` that are `b`'s: equal 4 KiB blocks at the same offset from their starts, plus those at the same distance
// from their ends (an edit that inserted or removed bytes keeps the blocks before it in place and the blocks after it at
// their distance from the end), at most a's size
uint64_t SharedBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    constexpr size_t kBlock = 4096;
    const size_t n = std::min(a.size(), b.size());
    uint64_t head = 0, tail = 0;
    for (size_t i = 0; i + kBlock <= n; i += kBlock)
        if (std::memcmp(&a[i], &b[i], kBlock) == 0) head += kBlock;
    for (size_t i = kBlock; i <= n; i += kBlock)
        if (std::memcmp(&a[a.size() - i], &b[b.size() - i], kBlock) == 0) tail += kBlock;
    return std::min<uint64_t>(a.size(), head + tail);
}

// a loose file named like `entry` of the disc's archive, made from ANOTHER release's entry of that name. Its size is
// nearer that release's entry (whose size differs from this disc's) than this disc's entry, and less than half of it is this
// disc's entry's bytes: then the sentence saying so; "" = it is taken as an edit of this disc's entry. The sizes alone cannot
// decide: 114 entries differ between the European and the North American archives, many by 104 to 267 bytes of several MB,
// so an edit of one disc's chunk can land nearer the other's size; and the two releases' chunks share bytes of their own
// (a PAL-made Default.rm2 is 87 % the European entry's bytes and still 27 % the North American one's, all of it
// aligned from the end).
bool MadeForAnother(const Disc& base, const std::string& rel, const BhEntry& entry, ByteSource& file, ByteSource& archive,
                    std::string* why, std::string* error) {
    why->clear();
    const uint64_t f = file.Size(), t = entry.size;
    if (f == t) return true;
    const ReleaseArchive* nearest = nullptr;
    uint64_t o = 0;
    for (const ReleaseArchive& r : KnownArchives()) {
        if (!OtherRelease(base, r)) continue;
        const auto it = r.entries.find(ArchiveKey(entry.name));
        if (it == r.entries.end() || it->second.second == t) continue;
        const uint64_t size = it->second.second;
        if (Distance(f, size) >= Distance(f, t)) continue;
        if (!nearest || Distance(f, size) < Distance(f, o)) { nearest = &r; o = size; }
    }
    if (!nearest) return true;
    std::vector<uint8_t> mine(static_cast<size_t>(f)), theirs(static_cast<size_t>(t));
    if ((f && !file.Read(0, mine.data(), mine.size())) || (t && !archive.Read(entry.offset, theirs.data(), theirs.size())))
        return Fail(error, rel + " does not read");
    if (SharedBytes(mine, theirs) * 2 >= f) return true;   // mostly this disc's entry: an edit of it
    const std::string disc = RegionDisc(nearest->region);
    *why = "The file " + rel + " matches " + disc + "'s entry " + entry.name + " (" + std::to_string(o) + " bytes; it has " + std::to_string(f) +
           "), not this disc's (" + std::to_string(t) + " bytes): it was made for " + disc;
    return true;
}

// A name this disc lacks that another release has: its program, or an entry of its archive ("" when none)
std::string AnotherReleasesName(const Disc& base, const std::string& rel) {
    const std::string last = Upper(FileName(rel));
    std::string key = rel;
    std::replace(key.begin(), key.end(), '/', '\\');
    key = ArchiveKey(key);
    for (const ReleaseArchive& r : KnownArchives()) {
        if (!OtherRelease(base, r)) continue;
        const std::string disc = RegionDisc(r.region);
        if (Upper(r.executable) == last)
            return "The file " + rel + " is the program of " + disc + ", not of this disc (" + FileName(base.Executable()) + "): it was made for " + disc;
        auto it = r.entries.find(key);
        if (it == r.entries.end())
            for (it = r.entries.begin(); it != r.entries.end(); ++it)
                if (Upper(LastName(it->second.first)) == last) break;
        if (it != r.entries.end())
            return "The file " + rel + " is an entry of " + disc + "'s archive (" + it->second.first + "), not of this disc's: it was made for " + disc;
    }
    return "";
}

}  // namespace

bool ChangesFromFolder(const Disc& base, const std::string& path, ChangeSet* out, Plan* plan, std::string* error) {
    // one loose file, or a folder of them
    const bool single = !io::IsDir(path);
    std::string dir = path;
    std::vector<std::string> rels;
    if (single) {
        dir = io::Utf8(io::Path(path).parent_path());
        if (dir.empty()) dir = ".";
        rels.push_back(FileName(path));
    } else if (!io::ListFiles(dir, &rels)) {
        return Fail(error, "the folder does not list");
    }
    out->name = FileName(path);
    std::map<std::string, std::string> refused;   // rel -> why it is not imported (the error when it is the only file)
    auto refuse = [&](const std::string& rel, const std::string& why) {
        refused[rel] = why;
        out->warnings.push_back(why + ": not imported");
    };
    // 1. the disc's files: its own path when the disc has it, else the disc file of its name when there is one
    Claims files;
    std::vector<std::string> pending, ignored;
    std::map<std::string, std::vector<std::string>> by_name;
    for (const DiscFile& f : base.Files()) by_name[FileName(f.path)].push_back(f.path);
    for (const std::string& rel : rels) {
        const std::string u = Upper(rel);
        if (const DiscFile* f = base.Find(u)) {
            files.Claim(f->path, rel, true, f->path, &out->warnings);
            continue;
        }
        const auto hits = by_name.find(Upper(FileName(rel)));
        if (hits != by_name.end() && hits->second.size() == 1) {
            files.Claim(hits->second[0], rel, false, hits->second[0], &out->warnings);
        } else if (hits != by_name.end()) {
            std::string list;
            for (const std::string& p : hits->second) list += (list.empty() ? "" : ", ") + p;
            refuse(rel, rel + ": the disc has " + std::to_string(hits->second.size()) + " files of this name (" + list +
                            "): put it in the folder the disc has it in");
        } else {
            pending.push_back(rel);
        }
    }
    // the archive the other names are looked for in: the folder's own CRASH.BH + CRASH.BD when it has both, else the disc's
    const auto loose_bh = files.Mapped().find(kArchiveIndex);
    const auto loose_bd = files.Mapped().find(kArchiveData);
    const bool loose_pair = loose_bh != files.Mapped().end() && loose_bd != files.Mapped().end();
    const bool loose_one = !loose_pair && (loose_bh != files.Mapped().end() || loose_bd != files.Mapped().end());
    std::vector<uint8_t> cur_bh_bytes;
    ByteSource* cur_bd = nullptr;
    BhIndex cur;
    bool have_archive = false;
    if (!pending.empty() && !loose_one) {
        std::string why;
        if (loose_pair) {
            auto bh = std::make_unique<HostFileSource>();
            auto bd = std::make_unique<HostFileSource>();
            if (!bh->Open(io::Join(dir, loose_bh->second)) || !bd->Open(io::Join(dir, loose_bd->second)) || !ReadAll(*bh, &cur_bh_bytes))
                return Fail(error, "the folder's CRASH.BH and CRASH.BD do not read");
            cur_bd = out->Own(std::move(bd));
        } else if (const DiscFile* dbh = base.Find(kArchiveIndex); dbh && base.Find(kArchiveData)) {
            if (!base.ReadFile(*dbh, &cur_bh_bytes)) return Fail(error, "your disc's archive index does not read");
            cur_bd = out->Own(base.Source(*base.Find(kArchiveData)));
        }
        have_archive = cur_bd && ParseBh(cur_bh_bytes, &cur, &why);
    }
    // 2. the archive's entries: by the index path (Startup/Default.rm2 = Startup\Default.rm2), else by the last name when one
    //    entry has it ("Default.rm2"); a name several entries share is left out with them listed
    Claims entries;
    std::map<std::string, size_t> by_key;
    std::map<std::string, std::vector<size_t>> by_last;
    if (have_archive)
        for (size_t i = 0; i < cur.entries.size(); ++i) {
            by_key[ArchiveKey(cur.entries[i].name)] = i;
            by_last[Upper(LastName(cur.entries[i].name))].push_back(i);
        }
    for (const std::string& rel : pending) {
        const std::string u = Upper(rel);
        std::string index_path = rel;
        std::replace(index_path.begin(), index_path.end(), '/', '\\');
        if (have_archive) {
            if (const auto k = by_key.find(ArchiveKey(index_path)); k != by_key.end()) {
                entries.Claim(k->first, rel, true, cur.entries[k->second].name, &out->warnings);
                continue;
            }
            if (const auto hits = by_last.find(Upper(FileName(rel))); hits != by_last.end()) {
                if (hits->second.size() == 1) {
                    const BhEntry& e = cur.entries[hits->second[0]];
                    entries.Claim(ArchiveKey(e.name), rel, false, e.name, &out->warnings);
                } else {
                    std::string list;
                    for (size_t i : hits->second) list += (list.empty() ? "" : ", ") + cur.entries[i].name;
                    std::string example = cur.entries[hits->second[0]].name;
                    std::replace(example.begin(), example.end(), '\\', '/');
                    refuse(rel, "The file " + rel + " could match " + std::to_string(hits->second.size()) + " entries of the archive (" + list +
                                    "): put it in folders named like the one it replaces (" + example + ") to choose");
                }
                continue;
            }
        }
        if (ParentOf(u).size() && IsDiscDirectory(base, ParentOf(u))) {
            files.Claim(u, rel, true, u, &out->warnings);   // an added file in a folder the disc has
        } else if (NoteLike(rel)) {
            ignored.push_back(rel);
        } else if (const std::string other = AnotherReleasesName(base, rel); !other.empty()) {
            refuse(rel, other);
        } else {
            // the names tried
            std::string tried = "looked for " + u + " among the disc's files";
            if (u.find('/') != std::string::npos) tried += ", " + index_path + " among the archive's entries";
            tried += " and any of either named " + FileName(rel);
            refuse(rel, "Neither your disc nor its archive has a file named " + FileName(rel) + " (" + tried + ")" +
                            (loose_one ? " -- a CRASH.BH without its CRASH.BD (or the reverse) is not an archive to look in" : ""));
        }
    }
    // an entry made from another release's is not taken
    std::map<std::string, ByteSource*> repl;   // archive key -> the file
    for (const auto& kv : entries.Mapped()) {
        auto src = std::make_unique<HostFileSource>();
        if (!src->Open(io::Join(dir, kv.second))) return Fail(error, kv.second + " does not read");
        if (src->Size() > 0xFFFFFFFFull) return Fail(error, kv.second + " is larger than the archive can hold");
        std::string why;
        if (!MadeForAnother(base, kv.second, cur.entries[by_key[kv.first]], *src, *cur_bd, &why, error)) return false;
        if (!why.empty()) {
            refuse(kv.second, why);
            continue;
        }
        repl[kv.first] = out->Own(std::move(src));
    }
    if (files.Mapped().empty() && repl.empty()) {
        if (rels.size() == 1 && refused.count(rels[0])) return Fail(error, refused[rels[0]]);
        std::string list;
        for (size_t i = 0; i < rels.size() && i < 4; ++i) list += (i ? ", " : "") + rels[i];
        return Fail(error, "none of its files is a file of the disc or an entry of its archive" +
                               (list.empty() ? std::string() : " (it holds " + list + (rels.size() > 4 ? ", ..." : "") + ")"));
    }
    if (!ignored.empty()) out->warnings.push_back(std::to_string(ignored.size()) + " note(s) beside the files left out (" + ignored.front() +
                                                  (ignored.size() > 1 ? ", ..." : "") + ")");
    ByteSource* bh = nullptr;
    ByteSource* bd = nullptr;
    for (const auto& kv : files.Mapped()) {
        auto src = std::make_unique<HostFileSource>();
        if (!src->Open(io::Join(dir, kv.second))) return Fail(error, kv.second + " does not read");
        ByteSource* s = out->Own(std::move(src));
        if (kv.first == kArchiveIndex) { bh = s; continue; }
        if (kv.first == kArchiveData) { bd = s; continue; }
        if (!AddFile(base, kv.first, s, out, plan, error)) return false;
    }
    // the loose entries: the archive rebuilt with them, exactly as a crate's layer1 (over the folder's pair or the disc's)
    if (!repl.empty()) return RebuildArchive(base, cur_bh_bytes, cur_bd, repl, {}, out, plan, error);
    if (bh && bd) {
        std::vector<uint8_t> bytes;
        if (!ReadAll(*bh, &bytes)) return Fail(error, "CRASH.BH does not read");
        return AddArchive(base, bytes, bd, out, plan, error);
    }
    if (bh && !AddFile(base, kArchiveIndex, bh, out, plan, error)) return false;
    if (bd && !AddFile(base, kArchiveData, bd, out, plan, error)) return false;
    return true;
}

// ---- the mod folder -----------------------------------------------------------------------------------------------------

namespace {

struct Hasher {
    ps2r::hash::Sha256 h;
    std::string Hex() {
        uint8_t d[ps2r::hash::Sha256::kDigestBytes];
        h.Finish(d);
        return ps2r::hash::Sha256::Hex(d);
    }
};

std::string FormName(OutFile::Form f) {
    switch (f) {
        case OutFile::Form::Replace: return "replace";
        case OutFile::Form::Add: return "add";
        case OutFile::Form::Ranges: return "ranges";
    }
    return "replace";
}

bool CopyOut(ByteSource& src, uint64_t offset, uint64_t length, io::File* out, Hasher* hash, std::vector<uint8_t>& buf, Plan* plan) {
    for (uint64_t done = 0; done < length;) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), length - done));
        if (!src.Read(offset + done, buf.data(), n)) return false;
        if (out && !out->Append(buf.data(), n)) return false;
        if (hash) hash->h.Update(buf.data(), n);
        done += n;
        if (plan && !plan->Step(-1)) return false;
    }
    return true;
}

}  // namespace

bool WriteModFolder(const ChangeSet& changes, const Disc& base, DiscRecord* record, const std::string& dir, const std::string& id,
                    const SourceRecord& source, Plan* plan, Report* report, std::string* error) {
    if (!io::MakeDirs(dir)) return Fail(error, "the mod's folder cannot be made (" + dir + ")");
    std::vector<const OutFile*> files;
    for (const OutFile& f : changes.files) files.push_back(&f);
    std::sort(files.begin(), files.end(), [](const OutFile* a, const OutFile* b) { return a->path < b->path; });
    uint64_t total = 0, done_bytes = 0;
    for (const OutFile* f : files) total += f->form == OutFile::Form::Ranges ? f->recipe.Size() : f->whole->Size();
    json::Value rows = json::Value::Arr();
    std::vector<uint8_t> buf(kChunk);
    uint64_t written = 0;
    Report r;
    for (const OutFile* f : files) {
        const std::string disc_rel = "disc/" + f->path;
        json::Value row = json::Value::Obj();
        row.Set("path", json::Value::Str(f->path));
        row.Set("form", json::Value::Str(FormName(f->form)));
        Hasher hash;
        if (f->form != OutFile::Form::Ranges) {
            const std::string host = io::Join(dir, disc_rel);
            io::MakeDirs(io::Utf8(io::Path(host).parent_path()));
            io::File out;
            if (!out.Create(host)) return Fail(error, "cannot write " + host);
            if (!CopyOut(*f->whole, 0, f->whole->Size(), &out, &hash, buf, plan) || !out.Close())
                return Fail(error, plan && plan->Cancelled() ? "stopped" : "cannot write " + host + " (is the disk full?)");
            row.Set("bytes", json::Value::Num(f->whole->Size()));
            row.Set("sha256", json::Value::Str(hash.Hex()));
            written += f->whole->Size();
            done_bytes += f->whole->Size();
        } else {
            // the payload (data/<DISC PATH>.bin: every `data` segment back to back) and the recipe naming it
            const std::string payload_rel = "data/" + f->path + ".bin";
            const std::string payload = io::Join(dir, payload_rel), recipe = io::Join(dir, disc_rel + ".ranges");
            io::MakeDirs(io::Utf8(io::Path(payload).parent_path()));
            io::MakeDirs(io::Utf8(io::Path(recipe).parent_path()));
            io::File out;
            const bool has_data = f->recipe.DataBytes() > 0;
            if (has_data && !out.Create(payload)) return Fail(error, "cannot write " + payload);
            std::unique_ptr<ByteSource> base_file = base.Source(*f->base);
            std::string text = "# " + id + ": " + f->path + " as your disc's bytes (base) and this mod's (file); " +
                               std::to_string(f->recipe.Size()) + " bytes, made over the disc's " + std::to_string(f->base->size) + "\n";
            uint64_t payload_at = 0;
            for (const Segment& s : f->recipe.Segments()) {
                switch (s.kind) {
                    case Segment::Kind::Base:
                        if (!CopyOut(*base_file, s.base_offset, s.length, nullptr, &hash, buf, plan))
                            return Fail(error, plan && plan->Cancelled() ? "stopped" : "your disc's " + f->path + " does not read");
                        text += "base " + std::to_string(s.base_offset) + " " + std::to_string(s.length) + "\n";
                        break;
                    case Segment::Kind::Data:
                        if (!CopyOut(*s.src, s.src_offset, s.length, &out, &hash, buf, plan))
                            return Fail(error, plan && plan->Cancelled() ? "stopped" : "cannot write " + payload + " (is the disk full?)");
                        text += "file " + payload_rel + " " + std::to_string(payload_at) + " " + std::to_string(s.length) + "\n";
                        payload_at += s.length;
                        break;
                    case Segment::Kind::Zero: {
                        std::fill(buf.begin(), buf.end(), uint8_t(0));
                        for (uint64_t k = 0; k < s.length;) {
                            const size_t n = size_t(std::min<uint64_t>(buf.size(), s.length - k));
                            hash.h.Update(buf.data(), n);
                            k += n;
                        }
                        text += "zero " + std::to_string(s.length) + "\n";
                        break;
                    }
                }
                done_bytes += s.length;
            }
            if (has_data && !out.Close()) return Fail(error, "cannot write " + payload + " (is the disk full?)");
            if (!io::WriteWhole(recipe, text)) return Fail(error, "cannot write " + recipe);
            written += payload_at + text.size();
            FileHashes base_hashes;
            if (!record->Hashes(base, *f->base, plan, &base_hashes, error)) return false;
            row.Set("bytes", json::Value::Num(f->recipe.Size()));
            row.Set("sha256", json::Value::Str(hash.Hex()));
            row.Set("base_bytes", json::Value::Num(f->base->size));
            row.Set("base_sha256", json::Value::Str(base_hashes.sha256));
            row.Set("base_sample_sha256", json::Value::Str(base_hashes.sample_sha256));
            row.Set("base_sample_windows", json::Value::Num(kSampleWindows));
            row.Set("base_sample_window_bytes", json::Value::Num(kSampleWindowBytes));
            row.Set("payload_bytes", json::Value::Num(payload_at));
        }
        (f->form == OutFile::Form::Add ? r.files_added : r.files_changed).push_back(f->path);
        rows.Push(std::move(row));
        if (plan && !plan->Step(total ? double(done_bytes) / double(total) : 1.0)) return Fail(error, "stopped");
    }
    // mod.json (schema 2)
    json::Value m = json::Value::Obj();
    m.Set("schema", json::Value::Num(2));
    m.Set("id", json::Value::Str(id));
    m.Set("kind", json::Value::Str("disc"));
    m.Set("name", json::Value::Str(changes.name.empty() ? id : changes.name));
    m.Set("author", json::Value::Str(changes.author));
    m.Set("version", json::Value::Str(changes.version));
    m.Set("description", json::Value::Str(changes.description));
    if (!changes.notes.empty()) {
        // the author's '!' lines of ModCrateInfo.txt (Crate Mod Loader's comments: what the player is told)
        json::Value notes = json::Value::Arr();
        for (const std::string& n : changes.notes) notes.Push(json::Value::Str(n));
        m.Set("notes", std::move(notes));
    }
    json::Value src = json::Value::Obj();
    src.Set("kind", json::Value::Str(source.kind));
    src.Set("artefact", json::Value::Str(source.artefact));
    src.Set("sha256", json::Value::Str(source.sha256));
    m.Set("source", std::move(src));
    json::Value b = json::Value::Obj();
    b.Set("region", json::Value::Str(base.Region()));
    b.Set("executable", json::Value::Str(base.Executable()));
    json::Value disc = json::Value::Obj();
    disc.Set("size", json::Value::Num(record->size));
    disc.Set("sha1", json::Value::Str(record->sha1));
    disc.Set("redump", record->redump.empty() ? json::Value() : json::Value::Str(record->redump));
    b.Set("disc", std::move(disc));
    m.Set("base", std::move(b));
    m.Set("files", std::move(rows));
    if (!changes.removed.empty()) {
        json::Value removed = json::Value::Arr();
        for (const std::string& p : changes.removed) removed.Push(json::Value::Str(p));
        m.Set("removed", std::move(removed));
    }
    if (changes.executable_changed) {
        const ExecutableDiff& x = changes.executable;
        json::Value ex = json::Value::Obj();
        json::Value data = json::Value::Obj();
        data.Set(".data", json::Value::Num(x.data));
        data.Set(".rodata", json::Value::Num(x.rodata));
        data.Set(".sdata", json::Value::Num(x.sdata));
        data.Set("vu", json::Value::Num(x.vu));
        ex.Set("data_changes", std::move(data));
        json::Value code = json::Value::Arr();
        for (const std::string& fn : x.functions) code.Push(json::Value::Str(fn));
        ex.Set("code_changes", std::move(code));
        ex.Set("code_bytes", json::Value::Num(x.text));
        ex.Set("other_bytes", json::Value::Num(x.other));
        if (!x.restored.empty()) {
            json::Value kept = json::Value::Arr();
            for (const ExecutableDiff::Restore& rw : x.restored) {
                char addr[16], was[16], now[16];
                std::snprintf(addr, sizeof addr, "0x%08X", rw.addr);
                std::snprintf(was, sizeof was, "0x%08X", rw.was);
                std::snprintf(now, sizeof now, "0x%08X", rw.now);
                json::Value o = json::Value::Obj();
                o.Set("address", json::Value::Str(addr));
                o.Set("disc", json::Value::Str(was));
                o.Set("mod", json::Value::Str(now));
                o.Set("points_to", json::Value::Str(rw.target));
                kept.Push(std::move(o));
            }
            ex.Set("data_kept_from_disc", std::move(kept));
        }
        m.Set("executable", std::move(ex));
        r.code_changes = x.functions;
        r.code_changes_unsupported = !x.functions.empty();
    }
    json::Value imp = json::Value::Obj();
    imp.Set("tool", json::Value::Str("twinsanityevolved-modtool"));
    imp.Set("format", json::Value::Num(1));
    std::vector<std::string> warnings = changes.warnings;
    if (changes.executable_changed) {
        if (!changes.executable.functions.empty())
            warnings.push_back("code changes not supported: " + std::to_string(changes.executable.functions.size()) +
                               " function(s) of the program differ, and this port runs its own code for them (the mod's data changes still apply)");
        for (const std::string& w : changes.executable.warnings) warnings.push_back("SLES_525.68: " + w);
    }
    json::Value warn = json::Value::Arr();
    for (const std::string& w : warnings) warn.Push(json::Value::Str(w));
    imp.Set("warnings", std::move(warn));
    if (!changes.game_code_settings.empty()) {
        // what the crate's notes ask Crate Mod Loader to apply to the game's code (this port cannot): the Mods page names them
        json::Value settings = json::Value::Arr();
        for (const std::string& g : changes.game_code_settings) settings.Push(json::Value::Str(g));
        imp.Set("game_code_settings", std::move(settings));
    }
    if (changes.archive_changed) {
        json::Value a = json::Value::Obj();
        json::Value ch = json::Value::Arr(), add = json::Value::Arr();
        for (const std::string& n : changes.archive.changed_names) ch.Push(json::Value::Str(n));
        for (const std::string& n : changes.archive.added_names) add.Push(json::Value::Str(n));
        a.Set("entries_changed", std::move(ch));
        a.Set("entries_added", std::move(add));
        imp.Set("archive", std::move(a));
    }
    m.Set("import", std::move(imp));
    const std::string manifest = io::Join(dir, "mod.json");
    const std::string text = json::Write(m);
    if (!io::WriteWhole(manifest, text)) return Fail(error, "cannot write " + manifest);
    written += text.size();
    r.id = id;
    r.name = changes.name.empty() ? id : changes.name;
    r.mod_json_path = manifest;
    r.warnings = warnings;
    r.bytes_written = written;
    if (report) *report = std::move(r);
    return true;
}

}  // namespace te::modimport::detail
