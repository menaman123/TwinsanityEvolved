// disc_regions -- see disc_regions.h.
#include "app/host/disc_regions.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "app/host/disc_json.h"
#include "engine/core/hash/sha256.h"

namespace ps2r::app {
namespace {

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "SLUS_209.09" / "SLUS-20909" / "slus 20909" -> "SLUS-20909"
std::string SerialKey(const std::string& s) {
    std::string letters, digits;
    for (char c : s) {
        if (std::isalpha(static_cast<unsigned char>(c)) && digits.empty()) letters += char(std::toupper(static_cast<unsigned char>(c)));
        else if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
    }
    return letters.size() == 4 && digits.size() == 5 ? letters + "-" + digits : Upper(s);
}

std::vector<RegionEntry> MakeTable() {
    std::vector<RegionEntry> t;
    {
        RegionEntry e;   // the decompilation's own layout: runner/te_env.h RetailImage
        e.serial = "SLES-52568";
        e.executable = "SLES_525.68";
        e.region = DiscIdentity::Region::PAL;
        e.disc_words = "the European disc";
        e.native_hz = 50;
        e.default_hz = 60;
        e.offers_50 = true;
        e.entry = 0x00100008;
        e.image_start = 0x002D9D90;
        e.bss_start = 0x0030A480;
        e.image_end = 0x003DB200;
        e.code_start = 0x00100000;
        e.code_end = 0x002D9D90;
        e.elf_sha256 = "7a609bb3ac32c4ef42129088902c4cf8aa05cf9cb62845e121764e32d2cedc1d";
        e.elf_bytes = 2152836;
        e.native = true;
        e.source = "runner/te_env.h RetailImage; game/splat.yaml (the executable the decompilation was made from)";   // lint: not-ui
        t.push_back(e);
    }
    {
        RegionEntry e;   // the retail v2.00 executable (Redump 13689) = engine/games/slus_20909_crash_twinsanity's
        e.serial = "SLUS-20909";
        e.executable = "SLUS_209.09";
        e.region = DiscIdentity::Region::NTSC_U;
        e.disc_words = "the North American disc";
        e.native_hz = 60;
        e.default_hz = 60;
        e.offers_50 = false;
        e.entry = 0x00100008;
        e.image_start = 0x002D9A10;   // .vutext
        e.bss_start = 0x0030A100;     // .sbss
        e.image_end = 0x003DAE80;     // .bss's end = the PT_LOAD's
        e.code_start = 0x00100000;
        e.code_end = 0x002D9A08;
        e.elf_sha256 = "8d114879fb704d328394771aec6276136bdb4ecf4c5f8de06139b7b42974038f";
        e.elf_bytes = 2151940;
        e.native = false;
        e.map_file = "region_map_SLUS-20909.json";   // lint: not-ui
        e.source = "the SLUS_209.09 of the retail v2.00 disc (Redump 13689): its section headers; built-in map launcher/discs/region_map_SLUS-20909.json";   // lint: not-ui
        t.push_back(e);
    }
    {
        RegionEntry e;   // redump.org/disc/26782: the disc is known, its executable is not
        e.serial = "SLPM-65801";
        e.executable = "SLPM_658.01";
        e.region = DiscIdentity::Region::NTSC_J;
        e.disc_words = "the Japanese disc";
        e.native_hz = 60;
        e.default_hz = 60;
        e.offers_50 = false;
        e.native = false;
        e.map_file = "region_map_SLPM-65801.json";   // lint: not-ui
        e.source = "redump.org/disc/26782 (the disc); the executable's layout is not known here";   // lint: not-ui
        t.push_back(e);
    }
    return t;
}

std::vector<RegionMap>& Maps() {
    static std::vector<RegionMap> maps;
    return maps;
}

struct PendingMap { std::string serial; const char* text = nullptr; std::string owned; };
std::vector<PendingMap>& Pending() {
    static std::vector<PendingMap> pending;
    return pending;
}

// "serial": "SLUS-20909" near the start of a map (the tools write it among the first keys); "" when not there
std::string SerialOfMapText(const char* text) {
    const std::string head(text, strnlen(text, 1024));
    const size_t k = head.find("\"serial\"");
    if (k == std::string::npos) return {};
    size_t at = head.find(':', k);
    if (at == std::string::npos) return {};
    at = head.find('"', at);
    if (at == std::string::npos) return {};
    const size_t end = head.find('"', at + 1);
    return end == std::string::npos ? std::string() : head.substr(at + 1, end - at - 1);
}

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
void Wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

struct Load { uint32_t vaddr, offset, filesz, memsz; };

bool ReadLoads(const std::vector<uint8_t>& elf, std::vector<Load>* out, uint32_t* entry, std::string* error) {
    if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0 || elf[4] != 1 || elf[5] != 1) {
        if (error) *error = "not a 32-bit little-endian ELF";
        return false;
    }
    if (Rd16(&elf[18]) != 8) {
        if (error) *error = "not a MIPS ELF";
        return false;
    }
    *entry = Rd32(&elf[24]);
    const uint32_t phoff = Rd32(&elf[28]);
    const uint16_t phentsize = Rd16(&elf[42]), phnum = Rd16(&elf[44]);
    if (uint64_t(phoff) + uint64_t(phentsize) * phnum > elf.size() || phentsize < 32) {
        if (error) *error = "program headers out of range";
        return false;
    }
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t* ph = &elf[phoff + size_t(i) * phentsize];
        if (Rd32(ph) != 1) continue;   // PT_LOAD
        Load l{Rd32(ph + 8), Rd32(ph + 4), Rd32(ph + 16), Rd32(ph + 20)};
        if (uint64_t(l.offset) + l.filesz > elf.size()) {
            if (error) *error = "a PT_LOAD's file range is out of the ELF";
            return false;
        }
        out->push_back(l);
    }
    if (out->empty()) {
        if (error) *error = "no PT_LOAD segment";
        return false;
    }
    return true;
}

// n bytes at `vaddr` from the PT_LOADs' file bytes into dst (zero where no file byte is).
void ReadVirtual(const std::vector<uint8_t>& elf, const std::vector<Load>& loads, uint32_t vaddr, uint32_t n, uint8_t* dst) {
    std::memset(dst, 0, n);
    for (const Load& l : loads) {
        const uint64_t lo = std::max<uint64_t>(vaddr, l.vaddr), hi = std::min<uint64_t>(uint64_t(vaddr) + n, uint64_t(l.vaddr) + l.filesz);
        if (lo < hi) std::memcpy(dst + (lo - vaddr), &elf[l.offset + (lo - l.vaddr)], size_t(hi - lo));
    }
}

const uint32_t* Lookup(const std::vector<std::pair<uint32_t, uint32_t>>& pairs, uint32_t key) {
    auto it = std::lower_bound(pairs.begin(), pairs.end(), key, [](const std::pair<uint32_t, uint32_t>& p, uint32_t k) { return p.first < k; });
    return it != pairs.end() && it->first == key ? &it->second : nullptr;
}

bool ParseHexAddress(const json::Value& v, uint32_t* out) {
    uint64_t n = 0;
    if (!json::ToU64(v, &n) || n > 0xFFFFFFFFull) return false;
    *out = uint32_t(n);
    return true;
}

bool ParseRanges(const json::Value* list, std::vector<RegionMap::Range>* out) {
    if (!list || list->kind != json::Value::Array) return false;
    for (const json::Value& row : list->items) {
        RegionMap::Range r;
        const json::Value* pal = row.Field("pal");
        const json::Value* other = row.Field("other");
        uint64_t bytes = 0;
        if (!pal || !other || !ParseHexAddress(*pal, &r.pal) || !ParseHexAddress(*other, &r.other) || !row.Field("bytes") ||
            !json::ToU64(*row.Field("bytes"), &bytes) || bytes > 0xFFFFFFFFull)
            return false;
        r.bytes = uint32_t(bytes);
        out->push_back(r);
    }
    return true;
}

bool ParsePairs(const json::Value* list, std::vector<std::pair<uint32_t, uint32_t>>* out) {
    if (!list) return true;   // optional
    if (list->kind != json::Value::Array) return false;
    for (const json::Value& row : list->items) {
        uint32_t a = 0, b = 0;
        if (row.kind != json::Value::Array || row.items.size() != 2 || !ParseHexAddress(row.items[0], &a) || !ParseHexAddress(row.items[1], &b))
            return false;
        out->emplace_back(a, b);
    }
    std::sort(out->begin(), out->end());
    return true;
}

}  // namespace

const std::vector<RegionEntry>& RegionTable() {
    static const std::vector<RegionEntry> table = MakeTable();
    return table;
}

const RegionEntry* RegionEntryFor(const std::string& serial) {
    const std::string key = SerialKey(serial);
    for (const RegionEntry& e : RegionTable())
        if (e.serial == key) return &e;
    return nullptr;
}

bool ParseRegionMap(const std::string& text, RegionMap* out, std::string* error) {
    json::Value root;
    std::string why;
    if (!json::Parse(text, &root, &why)) {
        if (error) *error = "the region map does not read: " + why;
        return false;
    }
    if (root.Text("kind") != "region-map") {
        if (error) *error = "not a region map (kind is not region-map)";
        return false;
    }
    RegionMap m;
    m.serial = SerialKey(root.Text("serial"));
    m.executable = Upper(root.Text("executable"));
    const json::Value* elf = root.Field("elf");
    const json::Value* pal = root.Field("pal_elf");
    const json::Value* image = root.Field("image");
    if (!elf || !image) {
        if (error) *error = "the region map has no elf or image block";
        return false;
    }
    m.elf_sha256 = elf->Text("sha256");
    m.elf_bytes = elf->U64("bytes", 0);
    m.entry = uint32_t(elf->U64("entry", 0));
    if (pal) m.pal_elf_sha256 = pal->Text("sha256");
    m.image_start = uint32_t(image->U64("pal_start", 0));
    m.bss_start = uint32_t(image->U64("pal_bss_start", 0));
    m.image_end = uint32_t(image->U64("pal_end", 0));
    if (m.elf_sha256.size() != 64 || !m.image_start || m.bss_start <= m.image_start || m.image_end < m.bss_start) {
        if (error) *error = "the region map's executable hash or PAL image range is missing";
        return false;
    }
    if (!ParseRanges(root.Field("segments"), &m.segments) || !ParseRanges(root.Field("bss"), &m.bss)) {
        if (error) *error = "the region map's segments do not read";
        return false;
    }
    const json::Value* pointers = root.Field("pointers");
    if (!pointers || pointers->kind != json::Value::Array) {
        if (error) *error = "the region map has no pointers list";
        return false;
    }
    m.pointers.reserve(pointers->items.size());
    for (const json::Value& p : pointers->items) {
        uint32_t a = 0;
        if (!ParseHexAddress(p, &a)) {
            if (error) *error = "a pointer address of the region map does not read";
            return false;
        }
        m.pointers.push_back(a);
    }
    if (!ParsePairs(root.Field("code"), &m.code) || !ParsePairs(root.Field("data"), &m.data) || !ParsePairs(root.Field("fills"), &m.fills)) {
        if (error) *error = "the region map's address pairs do not read";
        return false;
    }
    m.schema = uint32_t(root.U64("schema", 1));
    if (const json::Value* names = root.Field("names")) {
        if (names->kind != json::Value::Array) {
            if (error) *error = "the region map's names do not read";
            return false;
        }
        for (const json::Value& row : names->items) {
            RegionMap::Name n;
            n.pal = row.Text("pal");
            n.other = row.Text("other");
            if (const json::Value* at = row.Field("at")) ParseHexAddress(*at, &n.at);
            if (n.pal.empty() || n.other.empty()) {
                if (error) *error = "a name of the region map has no pal or other spelling";
                return false;
            }
            m.names.push_back(n);
        }
    }
    *out = std::move(m);
    return true;
}

bool LoadRegionMap(const std::string& path, RegionMap* out, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot read " + path + ": " + std::strerror(errno);
        return false;
    }
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    if (!ParseRegionMap(text, out, error)) return false;
    out->origin = path;
    return true;
}

void AddRegionMap(const RegionMap& map) {
    for (RegionMap& m : Maps()) {
        if (m.serial == map.serial && m.elf_sha256 == map.elf_sha256) { m = map; return; }
    }
    Maps().push_back(map);
}

bool AddRegionMapText(const char* text) {
    const std::string serial = SerialOfMapText(text);
    if (serial.empty()) return false;
    Pending().push_back(PendingMap{SerialKey(serial), text, {}});
    return true;
}

bool AddRegionMapText(const std::string& text) {
    const std::string serial = SerialOfMapText(text.c_str());
    if (serial.empty()) return false;
    PendingMap p;
    p.serial = SerialKey(serial);
    p.owned = text;
    Pending().push_back(std::move(p));
    return true;
}

void ParsePendingRegionMaps(const std::string& serial) {
    const std::string key = SerialKey(serial);
    std::vector<PendingMap>& pending = Pending();
    for (size_t i = 0; i < pending.size();) {
        if (!key.empty() && pending[i].serial != key && !pending[i].serial.empty()) { ++i; continue; }
        const PendingMap p = std::move(pending[i]);
        pending.erase(pending.begin() + long(i));
        RegionMap m;
        std::string error;
        if (ParseRegionMap(p.text ? std::string(p.text) : p.owned, &m, &error)) AddRegionMap(m);
        else std::fprintf(stderr, "[twinsanityevolved] region: a region map does not read: %s\n", error.c_str());
    }
}

void ClearRegionMaps() {
    Maps().clear();
    Pending().clear();
}

const std::vector<RegionMap>& RegionMaps() { return Maps(); }

const RegionMap* FindRegionMap(const std::string& serial, const std::string& elf_sha256) {
    const std::string key = SerialKey(serial);
    if (!Pending().empty()) ParsePendingRegionMaps(key);
    for (const RegionMap& m : Maps())
        if ((m.serial == key || key.empty()) && m.elf_sha256 == elf_sha256) return &m;
    return nullptr;
}

bool HasRegionMap(const std::string& serial) {
    const std::string key = SerialKey(serial);
    for (const RegionMap& m : Maps())
        if (m.serial == key) return true;
    for (const PendingMap& p : Pending())
        if (p.serial == key) return true;
    return false;
}

std::string RegionRefusal(const RegionEntry& e) {
    return "This is " + e.disc_words + " (" + e.serial + "). The port runs the European game's code and needs this disc's address "
           "map, which has not been made yet: run twinsanityevolved-modtool region-map --pal <the European disc> --disc <this disc> "
           "once (or tools/discs/elf_region_map.py map with the two discs), and put the map it writes (" + e.map_file + ") beside the game.";
}

std::string RegionNotPlayable(const std::string& serial) {
    const RegionEntry* e = RegionEntryFor(serial);
    if (!e || e->native || HasRegionMap(serial)) return {};
    return RegionRefusal(*e);
}

std::string Sha256OfBytes(const std::vector<uint8_t>& bytes) { return hash::Sha256::HexOf(bytes.data(), bytes.size()); }

bool RenameArchiveEntries(const std::vector<uint8_t>& index, const std::vector<RegionMap::Name>& names, std::vector<uint8_t>* out,
                          std::vector<std::string>* renamed, std::string* why) {
    struct Row { std::string name; uint32_t offset, size; };
    std::vector<Row> rows;
    if (index.size() < 4) {
        if (why) *why = "the archive index is shorter than its header";
        return false;
    }
    size_t at = 4;
    while (at < index.size()) {
        if (at + 4 > index.size()) {
            if (why) *why = "the archive index ends inside an entry";
            return false;
        }
        const uint32_t len = Rd32(&index[at]);
        if (len == 0 || len > 1024 || at + 4 + len + 8 > index.size()) {
            if (why) *why = "an entry of the archive index does not read (at byte " + std::to_string(at) + ")";
            return false;
        }
        Row r;
        r.name.assign(reinterpret_cast<const char*>(&index[at + 4]), len);
        r.offset = Rd32(&index[at + 4 + len]);
        r.size = Rd32(&index[at + 8 + len]);
        rows.push_back(r);
        at += 4 + len + 8;
    }
    auto lower = [](std::string x) {
        for (char& c : x) c = char(std::tolower(static_cast<unsigned char>(c)));
        return x;
    };
    std::vector<std::string> present;
    for (const Row& r : rows) present.push_back(lower(r.name));
    for (const RegionMap::Name& n : names) {
        const std::string other = lower(n.other);
        for (Row& r : rows) {
            const std::string l = lower(r.name);
            std::string ext;
            if (l == other) ext.clear();
            else if (l.size() > other.size() && l.compare(0, other.size(), other) == 0 && l[other.size()] == '.') ext = r.name.substr(other.size());
            else continue;
            const std::string to = n.pal + ext;
            if (std::find(present.begin(), present.end(), lower(to)) != present.end()) continue;   // the disc has the PAL name already
            if (renamed) renamed->push_back(r.name + " -> " + to);
            present.push_back(lower(to));
            r.name = to;
        }
    }
    out->assign(index.begin(), index.begin() + 4);
    for (const Row& r : rows) {
        const uint32_t len = uint32_t(r.name.size());
        uint8_t word[4];
        Wr32(word, len);
        out->insert(out->end(), word, word + 4);
        out->insert(out->end(), r.name.begin(), r.name.end());
        Wr32(word, r.offset);
        out->insert(out->end(), word, word + 4);
        Wr32(word, r.size);
        out->insert(out->end(), word, word + 4);
    }
    return true;
}

bool LoadImageNative(const std::vector<uint8_t>& elf, uint32_t image_start, uint32_t bss_start, uint32_t image_end, uint8_t* mem,
                     size_t mem_size, std::string* error) {
    std::vector<Load> loads;
    uint32_t entry = 0;
    if (!ReadLoads(elf, &loads, &entry, error)) return false;
    if (image_end > mem_size || bss_start < image_start || image_end < bss_start) {
        if (error) *error = "the image range does not fit the memory";
        return false;
    }
    ReadVirtual(elf, loads, image_start, bss_start - image_start, mem + image_start);
    std::memset(mem + bss_start, 0, image_end - bss_start);
    return true;
}

bool LoadImageThroughMap(const std::vector<uint8_t>& elf, const RegionMap& map, uint8_t* mem, size_t mem_size, MapLoadReport* report,
                         std::string* error) {
    MapLoadReport local;
    MapLoadReport& r = report ? *report : local;
    r = MapLoadReport{};
    const std::string sha = Sha256OfBytes(elf);
    if (sha != map.elf_sha256) {
        if (error) *error = "the region map was made for another executable (sha256 " + map.elf_sha256.substr(0, 16) + "..., this one " +
                            sha.substr(0, 16) + "...)";
        return false;
    }
    std::vector<Load> loads;
    uint32_t entry = 0;
    if (!ReadLoads(elf, &loads, &entry, error)) return false;
    if (map.entry && entry != map.entry) {
        if (error) *error = "the executable's entry point is not the map's";
        return false;
    }
    if (map.image_end > mem_size) {
        if (error) *error = "the region map's image does not fit the memory";
        return false;
    }
    std::memset(mem + map.image_start, 0, map.image_end - map.image_start);
    for (const RegionMap::Range& s : map.segments) {
        const uint64_t lo = std::max<uint64_t>(s.pal, map.image_start), hi = std::min<uint64_t>(uint64_t(s.pal) + s.bytes, map.bss_start);
        if (lo >= hi) continue;
        ReadVirtual(elf, loads, uint32_t(s.other + (lo - s.pal)), uint32_t(hi - lo), mem + lo);
        ++r.segments;
        r.bytes_copied += hi - lo;
    }
    // the other executable's addresses -> PAL: the code pairs, the data pairs, then the ranges (segments + bss) by start
    std::vector<RegionMap::Range> ranges = map.segments;
    ranges.insert(ranges.end(), map.bss.begin(), map.bss.end());
    std::sort(ranges.begin(), ranges.end(), [](const RegionMap::Range& a, const RegionMap::Range& b) {
        return a.other != b.other ? a.other < b.other : a.pal < b.pal;   // the tool's order (tools/discs/elf_region_map.py apply_map)
    });
    for (uint32_t a : map.pointers) {
        if (a < map.image_start || uint64_t(a) + 4 > map.bss_start) continue;
        ++r.pointers;
        const uint32_t v = Rd32(mem + a);
        if (const uint32_t* t = Lookup(map.code, v)) { Wr32(mem + a, *t); ++r.by_code; continue; }
        if (const uint32_t* t = Lookup(map.data, v)) { Wr32(mem + a, *t); ++r.by_data; continue; }
        auto it = std::upper_bound(ranges.begin(), ranges.end(), v, [](uint32_t k, const RegionMap::Range& x) { return k < x.other; });
        if (it != ranges.begin()) {
            --it;
            if (v >= it->other && uint64_t(v) < uint64_t(it->other) + it->bytes) {
                Wr32(mem + a, it->pal + (v - it->other));
                ++r.by_range;
                continue;
            }
        }
        ++r.unmapped;
    }
    for (const auto& f : map.fills) {
        if (f.first < map.image_start || uint64_t(f.first) + 4 > map.bss_start) continue;
        Wr32(mem + f.first, f.second);
        ++r.fills;
    }
    return true;
}

}  // namespace ps2r::app
