// DiscOverlay implementation; the model is in the header.
#include "engine/cdvd/disc_overlay.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "engine/core/hash/sha256.h"
#include "engine/platform/host/platform.h"

namespace {

constexpr uint64_t kSector = IsoFs::kSectorSize;
constexpr uint64_t kMaxRecipeBytes = 64ull << 20;   // a recipe is text: a rebuilt archive's thousands of lines fit
constexpr uint64_t kMaxFileBytes = 0xFFFFFFFFull;   // ISO9660's 32-bit data length

uint64_t SectorsOf(uint64_t bytes) { return (bytes + kSector - 1) / kSector; }

std::string Upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}
std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// A name the host's file manager leaves in folders (never part of a mod)
bool HostClutter(const std::string& name) {
    const std::string l = Lower(name);
    return name.empty() || name[0] == '.' || l == "thumbs.db" || l == "desktop.ini";
}

// "123" or "0x7B"; false on anything else (no sign, no overflow)
bool ParseUnsigned(const std::string& s, uint64_t* out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (size_t i = 2; i < s.size(); ++i) {
            const char c = s[i];
            uint64_t d;
            if (c >= '0' && c <= '9') d = uint64_t(c - '0');
            else if (c >= 'a' && c <= 'f') d = uint64_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = uint64_t(c - 'A' + 10);
            else return false;
            if (v > (~0ull >> 4)) return false;
            v = (v << 4) | d;
        }
    } else {
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            const uint64_t d = uint64_t(c - '0');
            if (v > (~0ull - d) / 10) return false;
            v = v * 10 + d;
        }
    }
    *out = v;
    return true;
}

// A recipe's relpath: relative, inside the layer's folder ('/' or '\' separators; no "..", no drive or device)
bool SafeRelative(const std::string& rel, std::string* clean) {
    if (rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find(':') != std::string::npos) return false;
    std::string out, part;
    for (size_t i = 0; i <= rel.size(); ++i) {
        const char c = i < rel.size() ? rel[i] : '/';
        if (c == '/' || c == '\\') {
            if (part == "..") return false;
            if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + part;
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    if (out.empty()) return false;
    *clean = out;
    return true;
}

bool ReadWhole(const std::string& path, uint64_t limit, std::string* text) {
    const int fd = ps2r::host::FdOpen(path.c_str(), ps2r::host::kOpenRead);
    if (fd < 0) return false;
    const int64_t size = ps2r::host::FdSize(fd);
    bool ok = size >= 0 && uint64_t(size) <= limit;
    if (ok) {
        text->assign(size_t(size), '\0');
        size_t done = 0;
        while (done < text->size()) {
            const ps2r::host::SSize got = ps2r::host::FdPread(fd, &(*text)[done], text->size() - done, done);
            if (got <= 0) break;
            done += size_t(got);
        }
        ok = done == text->size();
    }
    ps2r::host::FdClose(fd);
    return ok;
}

size_t PreadAll(int fd, uint8_t* dst, size_t n, uint64_t offset) {
    size_t done = 0;
    while (done < n) {
        const ps2r::host::SSize got = ps2r::host::FdPread(fd, dst + done, n - done, offset + done);
        if (got <= 0) break;
        done += size_t(got);
    }
    return done;
}

std::string Hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

std::string LastComponent(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string ParentPath(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}
std::string StripVersion(const std::string& name) {
    const size_t semi = name.find(';');
    return semi == std::string::npos ? name : name.substr(0, semi);
}

}  // namespace

DiscOverlay::DiscOverlay(std::vector<Layer> layers) : layers_(std::move(layers)) {}

DiscOverlay::~DiscOverlay() { Release(); }

std::string DiscOverlay::NormalizePath(const std::string& path) {
    std::string s = path;
    const size_t colon = s.find(':');
    if (colon != std::string::npos && Lower(s.substr(0, 5)) == "cdrom") s = s.substr(colon + 1);
    std::string out, part;
    for (size_t i = 0; i <= s.size(); ++i) {
        const char c = i < s.size() ? s[i] : '/';
        if (c == '/' || c == '\\') {
            part = StripVersion(part);
            if (!part.empty()) out += (out.empty() ? "" : "/") + Upper(part);
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    return out;
}

void DiscOverlay::Release() {
    for (auto& kv : fds_) if (kv.second >= 0) ps2r::host::FdClose(kv.second);
    fds_.clear();
    files_.clear();
    by_path_.clear();
    dirs_.clear();
    dir_by_path_.clear();
    intervals_.clear();
    buffers_.clear();
    image_end_ = 0;
    hits_.store(0, std::memory_order_relaxed);
}

void DiscOverlay::Close() {
    Release();
    IsoFs::Close();
}

bool DiscOverlay::Open(const std::string& path, std::string* error) {
    report_ = Report{};
    if (!IsoFs::Open(path, error)) return false;   // (IsoFs::Open closes first: Release ran)
    image_end_ = IsoFs::ImageBytes();
    for (const Layer& layer : layers_) {
        std::string why;
        if (ComposeLayer(layer, &why)) report_.applied.push_back(layer.id);
        else report_.refused.push_back(layer.id + ": " + why);
    }
    Layout();
    return true;
}

int DiscOverlay::PayloadFd(const std::string& host_path, std::string* why) {
    auto it = fds_.find(host_path);
    if (it != fds_.end()) return it->second;
    const int fd = ps2r::host::FdOpen(host_path.c_str(), ps2r::host::kOpenRead);
    if (fd < 0) {
        *why = "cannot open " + host_path;
        return -1;
    }
    fds_[host_path] = fd;
    return fd;
}

bool DiscOverlay::ParentOf(const std::string& path, std::vector<Dir>* new_dirs, std::string* why) const {
    // Every directory above `path` must be a directory: on the image, synthesized already, or synthesized now
    const std::string parent = ParentPath(path);
    if (parent.empty()) return true;
    if (dir_by_path_.count(parent)) return true;
    for (const Dir& d : *new_dirs) if (d.path == parent) return true;
    if (by_path_.count(parent)) { *why = parent + " is a file on the volume below, not a directory"; return false; }
    Entry e;
    if (IsoFs::Find(parent, e)) {
        if (!e.directory) { *why = parent + " is a file on the disc, not a directory"; return false; }
        return true;
    }
    if (!ParentOf(parent, new_dirs, why)) return false;
    Dir d;
    d.path = parent;
    d.name = LastComponent(parent);
    new_dirs->push_back(d);
    return true;
}

bool DiscOverlay::ComposeLayer(const Layer& layer, std::string* why) {
    // The layer's disc/ tree: every file, sorted by disc path (the host's listing order is not the same everywhere)
    struct Item { std::string disc, host, shown; bool recipe; };
    std::vector<Item> items;
    const std::string root = layer.dir + "/disc";
    std::vector<std::pair<std::string, std::string>> stack;   // (host dir, disc prefix)
    if (!layer.dir.empty() && ps2r::host::IsDirectory(root.c_str())) stack.emplace_back(root, std::string());   // no disc/ tree: none of it
    while (!stack.empty()) {
        const auto [host_dir, prefix] = stack.back();
        stack.pop_back();
        std::vector<ps2r::host::DirectoryEntry> entries;
        if (!ps2r::host::ListDirectory(host_dir.c_str(), &entries)) { *why = "cannot list " + host_dir; return false; }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        for (const auto& e : entries) {
            if (HostClutter(e.name)) continue;
            const std::string host = host_dir + "/" + e.name;
            if (e.directory) {
                stack.emplace_back(host, prefix + Upper(e.name) + "/");
                continue;
            }
            const bool recipe = e.name.size() > 7 && Lower(e.name.substr(e.name.size() - 7)) == ".ranges";
            const std::string stem = recipe ? e.name.substr(0, e.name.size() - 7) : e.name;
            const std::string disc = NormalizePath(prefix + stem);
            if (disc.empty() || LastComponent(disc).empty()) { *why = "disc/" + prefix + e.name + ": no disc path"; return false; }
            items.push_back({disc, host, "disc/" + prefix + e.name, recipe});
        }
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.disc < b.disc; });
    for (size_t i = 1; i < items.size(); ++i)
        if (items[i].disc == items[i - 1].disc) {
            *why = items[i - 1].shown + " and " + items[i].shown + " both name " + items[i].disc;
            return false;
        }

    // Stage every file against the volume below; commit only when the whole layer is valid
    std::vector<File> staged;
    std::vector<Dir> new_dirs;
    for (const Item& it : items) {
        File f;
        f.path = it.disc;
        f.parent = 0;
        // the volume below: a file composed by an earlier layer, the image's, or none
        const File* below = nullptr;
        const auto known = by_path_.find(it.disc);
        if (known != by_path_.end()) below = &files_[known->second];
        Entry image_entry;
        const bool on_image = IsoFs::Find(it.disc, image_entry);
        if (on_image && image_entry.directory) { *why = it.shown + ": " + it.disc + " is a directory on the disc"; return false; }
        if (dir_by_path_.count(it.disc)) { *why = it.shown + ": " + it.disc + " is a directory an earlier layer made"; return false; }
        for (const Item& other : items)
            if (other.disc.size() > it.disc.size() && other.disc.compare(0, it.disc.size(), it.disc) == 0 && other.disc[it.disc.size()] == '/') {
                *why = it.shown + " is a file and a folder of this layer at once";
                return false;
            }
        if (!ParentOf(it.disc, &new_dirs, why)) { *why = it.shown + ": " + *why; return false; }
        // the segments of the file below (what "base" reads)
        std::vector<Segment> base;
        uint64_t base_size = 0;
        if (below) {
            base = below->segments;
            base_size = below->size;
            f.on_image = below->on_image;
            f.image_lba = below->image_lba;
            f.image_size = below->image_size;
            f.name = below->name;
            std::memcpy(f.date, below->date, sizeof f.date);
            f.layers = below->layers;
        } else if (on_image) {
            if (image_entry.size) {
                Segment s;
                s.kind = Segment::Kind::Image;
                s.at = uint64_t(image_entry.lba) * kSector;
                s.length = image_entry.size;
                base.push_back(s);
            }
            base_size = image_entry.size;
            f.on_image = true;
            f.image_lba = image_entry.lba;
            f.image_size = image_entry.size;
            f.name = image_entry.name;
            std::memcpy(f.date, image_entry.date, sizeof f.date);
        } else {
            f.name = LastComponent(it.disc) + ";1";
        }
        const bool have_below = below || on_image;
        // a recipe made against a known file: the file below must be that one (else its base ranges would read other bytes)
        if (it.recipe)
            for (const BaseCheck& check : layer.checks) {
                if (NormalizePath(check.path) != it.disc) continue;
                if (!have_below) {
                    *why = it.shown + ": made for a " + it.disc + " of " + std::to_string(check.bytes) + " bytes, which the volume below does not have";
                    return false;
                }
                const uint64_t check_start = ps2r::host::MonotonicNowNs();
                const bool base_ok = CheckBase(check, base, base_size, why);
                report_.base_checks++;
                report_.base_check_ns += ps2r::host::MonotonicNowNs() - check_start;
                if (!base_ok) {
                    *why = it.shown + ": " + *why;
                    return false;
                }
            }
        auto add = [&](Segment s) {
            if (!s.length) return;
            s.start = f.size;
            f.size += s.length;
            f.segments.push_back(s);
        };
        if (!it.recipe) {
            const int fd = PayloadFd(it.host, why);
            if (fd < 0) return false;
            const int64_t size = ps2r::host::FdSize(fd);
            if (size < 0 || uint64_t(size) > kMaxFileBytes) { *why = it.shown + ": not a file of at most 4 GB"; return false; }
            Segment s;
            s.kind = Segment::Kind::Payload;
            s.fd = fd;
            s.length = uint64_t(size);
            add(s);
            f.form = have_below ? "replace" : "add";
        } else {
            std::string text;
            if (!ReadWhole(it.host, kMaxRecipeBytes, &text)) { *why = it.shown + ": does not read (or is over 64 MB)"; return false; }
            size_t line_no = 0, at = 0;
            while (at <= text.size()) {
                size_t nl = text.find('\n', at);
                if (nl == std::string::npos) nl = text.size();
                std::string line = text.substr(at, nl - at);
                at = nl + 1;
                ++line_no;
                if (line_no == 1 && line.size() >= 3 && uint8_t(line[0]) == 0xEF && uint8_t(line[1]) == 0xBB && uint8_t(line[2]) == 0xBF)
                    line.erase(0, 3);
                // a comment: '#' at the start or after a blank
                for (size_t k = 0; k < line.size(); ++k)
                    if (line[k] == '#' && (k == 0 || line[k - 1] == ' ' || line[k - 1] == '\t')) { line.resize(k); break; }
                std::vector<std::string> tok;
                std::vector<size_t> tok_at;
                for (size_t k = 0; k < line.size();) {
                    while (k < line.size() && (line[k] == ' ' || line[k] == '\t' || line[k] == '\r')) ++k;
                    if (k >= line.size()) break;
                    const size_t b = k;
                    while (k < line.size() && line[k] != ' ' && line[k] != '\t' && line[k] != '\r') ++k;
                    tok.push_back(line.substr(b, k - b));
                    tok_at.push_back(b);
                }
                if (tok.empty()) continue;
                const std::string where = it.shown + " line " + std::to_string(line_no) + ": ";
                const std::string op = Lower(tok[0]);
                uint64_t off = 0, len = 0;
                if (op == "base") {
                    if (tok.size() != 3 || !ParseUnsigned(tok[1], &off) || !ParseUnsigned(tok[2], &len)) { *why = where + "base <offset> <length>"; return false; }
                    if (!have_below) { *why = where + "base, but " + it.disc + " is not on the volume below"; return false; }
                    if (off > base_size || len > base_size - off) {
                        *why = where + "base " + Hex(off) + "+" + Hex(len) + " is past the end of the file below (" + std::to_string(base_size) + " bytes)";
                        return false;
                    }
                    // the slice [off, off+len) of the segments below
                    for (const Segment& s : base) {
                        const uint64_t s_end = s.start + s.length;
                        if (s_end <= off || s.start >= off + len) continue;
                        const uint64_t from = std::max(off, s.start), to = std::min(off + len, s_end);
                        Segment piece = s;
                        piece.length = to - from;
                        if (piece.kind != Segment::Kind::Zero) piece.at = s.at + (from - s.start);
                        add(piece);
                    }
                } else if (op == "file") {
                    if (tok.size() < 4 || !ParseUnsigned(tok[tok.size() - 2], &off) || !ParseUnsigned(tok.back(), &len)) {
                        *why = where + "file <relpath> <offset> <length>";
                        return false;
                    }
                    // the relpath: the text between the keyword and the two numbers (a name may hold blanks)
                    std::string rel = line.substr(tok_at[1], tok_at[tok.size() - 2] - tok_at[1]);
                    while (!rel.empty() && (rel.back() == ' ' || rel.back() == '\t')) rel.pop_back();
                    std::string clean;
                    if (!SafeRelative(rel, &clean)) { *why = where + "'" + rel + "' is not a path inside the mod's folder"; return false; }
                    const std::string host = layer.dir + "/" + clean;
                    const int fd = PayloadFd(host, why);
                    if (fd < 0) { *why = where + *why; return false; }
                    const int64_t size = ps2r::host::FdSize(fd);
                    if (size < 0 || off > uint64_t(size) || len > uint64_t(size) - off) {
                        *why = where + "file " + clean + " " + Hex(off) + "+" + Hex(len) + " is past its end (" + std::to_string(size) + " bytes)";
                        return false;
                    }
                    Segment s;
                    s.kind = Segment::Kind::Payload;
                    s.fd = fd;
                    s.at = off;
                    s.length = len;
                    add(s);
                } else if (op == "zero") {
                    if (tok.size() != 2 || !ParseUnsigned(tok[1], &len)) { *why = where + "zero <length>"; return false; }
                    Segment s;
                    s.kind = Segment::Kind::Zero;
                    s.length = len;
                    add(s);
                } else {
                    *why = where + "'" + tok[0] + "' is not base / file / zero";
                    return false;
                }
                if (f.size > kMaxFileBytes) { *why = where + "the file is over 4 GB"; return false; }
            }
            f.form = "ranges";
        }
        f.layers.push_back(layer.id);
        staged.push_back(std::move(f));
    }
    // The computed files: each from the file below it -- this layer's own disc/ tree's version when it has one, else the
    // volume's below
    for (const Computed& c : layer.computed) {
        const std::string disc = NormalizePath(c.path);
        if (disc.empty() || !c.make) { *why = "a computed file with no path or function"; return false; }
        const File* below = nullptr;
        for (const File& f : staged)
            if (f.path == disc) below = &f;
        if (!below) {
            const auto known = by_path_.find(disc);
            if (known != by_path_.end()) below = &files_[known->second];
        }
        Entry image_entry;
        const bool on_image = IsoFs::Find(disc, image_entry);
        if (on_image && image_entry.directory) { *why = disc + " is a directory on the disc"; return false; }
        std::vector<uint8_t> in;
        File f;
        if (below) {
            if (below->size > kMaxRecipeBytes) { *why = disc + ": the file below is over 64 MB (not computed in memory)"; return false; }
            in.resize(size_t(below->size));
            if (ReadSegments(below->segments, below->size, 0, in.size(), in.data()) != in.size()) { *why = disc + ": the file below does not read"; return false; }
            f = *below;
            f.segments.clear();
        } else if (on_image) {
            if (image_entry.size > kMaxRecipeBytes) { *why = disc + ": the disc's file is over 64 MB (not computed in memory)"; return false; }
            in.resize(image_entry.size);
            if (IsoFs::ReadBytes(uint64_t(image_entry.lba) * kSector, in.size(), in.data()) != in.size()) { *why = disc + ": the disc's file does not read"; return false; }
            f.path = disc;
            f.on_image = true;
            f.image_lba = image_entry.lba;
            f.image_size = image_entry.size;
            f.name = image_entry.name;
            std::memcpy(f.date, image_entry.date, sizeof f.date);
        } else {
            if (!ParentOf(disc, &new_dirs, why)) { *why = disc + ": " + *why; return false; }
            f.path = disc;
            f.name = LastComponent(disc) + ";1";
        }
        auto out = std::make_unique<std::vector<uint8_t>>();
        std::string reason;
        if (!c.make(in, out.get(), &reason)) { *why = disc + ": " + reason; return false; }
        if (out->size() > kMaxFileBytes) { *why = disc + ": the computed file is over 4 GB"; return false; }
        f.size = 0;
        if (!out->empty()) {
            Segment s;
            s.kind = Segment::Kind::Memory;
            s.mem = out->data();
            s.length = out->size();
            f.segments.push_back(s);
            f.size = out->size();
        }
        f.form = "computed";
        f.layers.push_back(layer.id);
        buffers_.push_back(std::move(out));
        bool replaced = false;
        for (File& g : staged)
            if (g.path == disc) { g = f; replaced = true; }
        if (!replaced) staged.push_back(std::move(f));
    }
    // Commit
    for (Dir& d : new_dirs) {
        dir_by_path_[d.path] = uint32_t(dirs_.size());
        dirs_.push_back(d);
    }
    for (File& f : staged) {
        const auto known = by_path_.find(f.path);
        if (known != by_path_.end()) {
            files_[known->second] = std::move(f);
        } else {
            by_path_[f.path] = uint32_t(files_.size());
            files_.push_back(std::move(f));
        }
    }
    return true;
}

void DiscOverlay::Layout() {
    // Sort by path (the placement order is the path order: the same layers place the same way on every host)
    std::sort(files_.begin(), files_.end(), [](const File& a, const File& b) { return a.path < b.path; });
    std::sort(dirs_.begin(), dirs_.end(), [](const Dir& a, const Dir& b) { return a.path < b.path; });
    by_path_.clear();
    for (uint32_t i = 0; i < files_.size(); ++i) by_path_[files_[i].path] = i;
    dir_by_path_.clear();
    for (uint32_t i = 0; i < dirs_.size(); ++i) dir_by_path_[dirs_[i].path] = i;
    intervals_.clear();
    report_.files.clear();
    report_.overrides = uint32_t(files_.size());
    report_.relocated = 0;
    if (files_.empty() && dirs_.empty()) {
        image_end_ = IsoFs::ImageBytes();
        return;
    }
    const uint64_t base_bytes = IsoFs::ImageBytes();
    uint64_t next = SectorsOf(std::max<uint64_t>(base_bytes, uint64_t(VolumeSectors()) * kSector));
    const uint64_t first_virtual = next;
    // the synthesized directories: one (zero) sector each, first in the virtual region
    for (Dir& d : dirs_) d.lba = uint32_t(next++);
    auto lba_of_dir = [&](const std::string& path) -> uint32_t {
        if (path.empty()) return RootLba();
        const auto s = dir_by_path_.find(path);
        if (s != dir_by_path_.end()) return dirs_[s->second].lba;
        Entry e;
        return IsoFs::Find(path, e) ? e.lba : RootLba();
    };
    for (Dir& d : dirs_) d.parent = lba_of_dir(ParentPath(d.path));
    for (uint32_t i = 0; i < files_.size(); ++i) {
        File& f = files_[i];
        f.parent = lba_of_dir(ParentPath(f.path));
        const uint64_t allocation = f.on_image ? SectorsOf(f.image_size) : 0;
        std::string placed;
        if (f.on_image && f.size <= allocation * kSector) {
            f.lba = f.image_lba;
            f.sectors = uint32_t(allocation);
            placed = "in place at lba " + std::to_string(f.lba);
        } else {
            f.lba = uint32_t(next);
            f.sectors = uint32_t(SectorsOf(f.size));
            next += f.sectors;
            ++report_.relocated;
            placed = std::string(f.on_image ? "grown: moved" : "added") + " to lba " + std::to_string(f.lba) + " (virtual)";
        }
        if (f.sectors) intervals_.push_back({uint64_t(f.lba) * kSector, (uint64_t(f.lba) + f.sectors) * kSector, i});
        std::string by;
        for (const std::string& l : f.layers) by += (by.empty() ? "" : "+") + l;
        report_.files.push_back(f.path + ": " + f.form + ", " + std::to_string(f.segments.size()) + " segment(s), " +
                                std::to_string(f.size) + " bytes" + (f.on_image ? " (was " + std::to_string(f.image_size) + ")" : "") +
                                ", " + placed + ", by " + by);
    }
    std::sort(intervals_.begin(), intervals_.end(), [](const Interval& a, const Interval& b) { return a.begin < b.begin; });
    // Disjoint by construction on a sane image (two files sharing sectors is not one): a range that overlaps the one before
    // it is dropped from the routing (reported) rather than served twice
    std::vector<Interval> disjoint;
    for (const Interval& iv : intervals_) {
        if (!disjoint.empty() && iv.begin < disjoint.back().end) {
            report_.files.push_back(files_[iv.file].path + ": its sectors overlap " + files_[disjoint.back().file].path + "'s on the disc: not routed");
            continue;
        }
        disjoint.push_back(iv);
    }
    intervals_.swap(disjoint);
    image_end_ = next > first_virtual ? next * kSector : base_bytes;
}

bool DiscOverlay::Find(const std::string& path, Entry& out) const {
    if (files_.empty() && dirs_.empty()) return IsoFs::Find(path, out);
    const std::string key = NormalizePath(path);
    const auto f = by_path_.find(key);
    if (f != by_path_.end()) {
        const File& file = files_[f->second];
        out = Entry{};
        out.lba = file.lba;
        out.size = uint32_t(file.size);
        out.directory = false;
        std::memcpy(out.date, file.date, sizeof out.date);
        out.name = file.name;
        return true;
    }
    const auto d = dir_by_path_.find(key);
    if (d != dir_by_path_.end()) {
        const Dir& dir = dirs_[d->second];
        out = Entry{};
        out.lba = dir.lba;
        out.size = uint32_t(kSector);
        out.directory = true;
        out.name = dir.name;
        return true;
    }
    return IsoFs::Find(path, out);
}

bool DiscOverlay::List(const Entry& dir, std::vector<Entry>& out) const {
    if (files_.empty() && dirs_.empty()) return IsoFs::List(dir, out);
    out.clear();
    if (!dir.directory) return false;
    bool synthesized = false;
    for (const Dir& d : dirs_) synthesized |= d.lba == dir.lba;
    if (!synthesized && !IsoFs::List(dir, out)) return false;
    for (const File& f : files_) {
        if (f.parent != dir.lba) continue;
        Entry* hit = nullptr;
        const std::string want = Upper(StripVersion(f.name));
        for (Entry& e : out)
            if (!e.directory && Upper(StripVersion(e.name)) == want) { hit = &e; break; }
        if (!hit) {
            out.emplace_back();
            hit = &out.back();
            hit->name = f.name;
            std::memcpy(hit->date, f.date, sizeof hit->date);
        }
        hit->lba = f.lba;
        hit->size = uint32_t(f.size);
        hit->directory = false;
    }
    for (const Dir& d : dirs_) {
        if (d.parent != dir.lba || d.lba == dir.lba) continue;
        Entry e;
        e.lba = d.lba;
        e.size = uint32_t(kSector);
        e.directory = true;
        e.name = d.name;
        out.push_back(e);
    }
    return true;
}

size_t DiscOverlay::ReadComposed(const File& f, uint64_t pos, size_t n, uint8_t* dst) const {
    return ReadSegments(f.segments, f.size, pos, n, dst);
}

bool DiscOverlay::CheckBase(const BaseCheck& check, const std::vector<Segment>& base, uint64_t base_size, std::string* why) const {
    const std::string what = NormalizePath(check.path);
    if (base_size != check.bytes) {
        *why = "made for another " + what + " (of " + std::to_string(check.bytes) + " bytes; the one below has " + std::to_string(base_size) +
               "): the disc or a mod before it is not the one this mod was made over";
        return false;
    }
    const ReadFn read = [&](uint64_t offset, size_t n, uint8_t* dst) { return ReadSegments(base, base_size, offset, n, dst) == n; };
    std::string hex;
    const std::string sample = Lower(check.sample_sha256), whole = Lower(check.sha256);
    if (!sample.empty()) {
        if (!SampledSha256(read, base_size, check.sample_windows, check.sample_window_bytes, &hex)) {
            *why = "its check of " + what + " does not compute (" + std::to_string(check.sample_windows) + " windows of " +
                   std::to_string(check.sample_window_bytes) + " bytes)";
            return false;
        }
        if (hex != sample) {
            *why = "made for another " + what + " (the same size, other bytes: sampled sha256 " + hex.substr(0, 16) + ", made over " +
                   sample.substr(0, 16) + "): the disc or a mod before it is not the one this mod was made over";
            return false;
        }
    } else if (!whole.empty()) {
        if (!WholeSha256(read, base_size, &hex)) {
            *why = "its check of " + what + " does not read";
            return false;
        }
        if (hex != whole) {
            *why = "made for another " + what + " (the same size, other bytes: sha256 " + hex.substr(0, 16) + ", made over " +
                   whole.substr(0, 16) + "): the disc or a mod before it is not the one this mod was made over";
            return false;
        }
    }
    return true;
}

bool DiscOverlay::WholeSha256(const ReadFn& read, uint64_t size, std::string* hex) {
    ps2r::hash::Sha256 h;
    std::vector<uint8_t> buf(size_t(std::min<uint64_t>(size ? size : 1, 1u << 20)));
    for (uint64_t done = 0; done < size;) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), size - done));
        if (!read(done, n, buf.data())) return false;
        h.Update(buf.data(), n);
        done += n;
    }
    uint8_t digest[ps2r::hash::Sha256::kDigestBytes];
    h.Finish(digest);
    *hex = ps2r::hash::Sha256::Hex(digest);
    return true;
}

bool DiscOverlay::SampledSha256(const ReadFn& read, uint64_t size, uint32_t windows, uint64_t window_bytes, std::string* hex) {
    if (windows == 0 || windows > 4096 || window_bytes == 0 || window_bytes > (16ull << 20)) return false;
    if (size <= uint64_t(windows) * window_bytes) return WholeSha256(read, size, hex);
    ps2r::hash::Sha256 h;
    std::vector<uint8_t> buf(size_t(std::min<uint64_t>(window_bytes, 1u << 20)));
    for (uint32_t i = 0; i < windows; ++i) {
        const uint64_t start = windows == 1 ? 0 : (size - window_bytes) * i / (windows - 1);
        for (uint64_t done = 0; done < window_bytes;) {
            const size_t n = size_t(std::min<uint64_t>(buf.size(), window_bytes - done));
            if (!read(start + done, n, buf.data())) return false;
            h.Update(buf.data(), n);
            done += n;
        }
    }
    uint8_t digest[ps2r::hash::Sha256::kDigestBytes];
    h.Finish(digest);
    *hex = ps2r::hash::Sha256::Hex(digest);
    return true;
}

size_t DiscOverlay::ReadSegments(const std::vector<Segment>& segments, uint64_t size, uint64_t pos, size_t n, uint8_t* dst) const {
    size_t done = 0;
    // the first segment that ends past pos (segments are in file order, start = prefix sum)
    auto it = std::upper_bound(segments.begin(), segments.end(), pos,
                               [](uint64_t p, const Segment& s) { return p < s.start + s.length; });
    while (done < n) {
        const uint64_t at = pos + done;
        if (at >= size || it == segments.end()) {   // the padding to the end of the sector range
            std::memset(dst + done, 0, n - done);
            return n;
        }
        const Segment& s = *it;
        const uint64_t in = at - s.start;
        const size_t take = size_t(std::min<uint64_t>(n - done, s.length - in));
        size_t got = take;
        switch (s.kind) {
            case Segment::Kind::Zero: std::memset(dst + done, 0, take); break;
            case Segment::Kind::Image: got = IsoFs::ReadBytes(s.at + in, take, dst + done); break;
            case Segment::Kind::Payload: got = PreadAll(s.fd, dst + done, take, s.at + in); break;
            case Segment::Kind::Memory: std::memcpy(dst + done, s.mem + s.at + in, take); break;
        }
        done += got;
        if (got != take) return done;   // a source that ended early (a layer's file changed under the game)
        ++it;
    }
    return done;
}

size_t DiscOverlay::ReadBytes(uint64_t offset, size_t size, uint8_t* dst) const {
    // Nothing routed (also while the image itself opens: its descriptor read comes through here)
    if (intervals_.empty() && dirs_.empty()) return IsoFs::ReadBytes(offset, size, dst);
    if (!IsOpen() || size == 0 || offset >= image_end_) return 0;
    const size_t n = size_t(std::min<uint64_t>(size, image_end_ - offset));
    const uint64_t base_bytes = IsoFs::ImageBytes();
    // the first interval that ends past offset
    auto it = std::upper_bound(intervals_.begin(), intervals_.end(), offset,
                               [](uint64_t p, const Interval& iv) { return p < iv.end; });
    size_t done = 0;
    bool touched = false;
    while (done < n) {
        const uint64_t at = offset + done;
        if (it != intervals_.end() && it->begin <= at) {
            const size_t take = size_t(std::min<uint64_t>(n - done, it->end - at));
            const size_t got = ReadComposed(files_[it->file], at - it->begin, take, dst + done);
            touched = true;
            done += got;
            if (got != take) break;
            ++it;
            continue;
        }
        const uint64_t until = it != intervals_.end() ? std::min<uint64_t>(it->begin, image_end_) : image_end_;
        const size_t take = size_t(std::min<uint64_t>(n - done, until - at));
        size_t got = 0;
        if (at < base_bytes) {
            const size_t from_image = size_t(std::min<uint64_t>(take, base_bytes - at));
            got = IsoFs::ReadBytes(at, from_image, dst + done);
            if (got == from_image && from_image < take) {   // the rest lies in the virtual region's gaps: zeros
                std::memset(dst + done + got, 0, take - got);
                got = take;
            }
        } else {
            std::memset(dst + done, 0, take);   // a synthesized directory's sector, the pad before the virtual region
            got = take;
        }
        done += got;
        if (got != take) break;
    }
    if (touched) hits_.fetch_add(1, std::memory_order_relaxed);
    return done;
}
