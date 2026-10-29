// elf_diff.h implementation, and function_names.h's lookups.
#include "app/host/mod_import/elf_diff.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "app/host/mod_import/function_names.h"

namespace te::modimport {

namespace {

constexpr uint32_t kEntry = 0x00100008u;   // the PAL executable's entry (runner/te_env.cpp refuses any other)
// The retail layout (tools/gen_retail_image.py SECTIONS, .text from the PAL executable's section headers)
struct RetailRange { const char* name; uint32_t begin, end; };
const RetailRange kRetail[] = {{".text", 0x00100000u, 0x002D9D88u}, {".vutext", 0x002D9D90u, 0x002E6ED0u},
                               {".data", 0x002E6F00u, 0x002EC348u}, {".vudata", 0x002EC348u, 0x002EC3D2u},
                               {".rodata", 0x002EC400u, 0x00309868u}, {".sdata", 0x00309880u, 0x0030A460u}};
constexpr uint32_t kDataImageBegin = 0x002D9D90u, kDataImageEnd = 0x0030A480u;   // what LoadRetailImage copies

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

std::string Hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

enum class Class { Text, Data, Rodata, Sdata, Vu, Other };

Class ClassOf(const std::string& name) {
    if (name == ".text") return Class::Text;
    if (name == ".data") return Class::Data;
    if (name == ".rodata") return Class::Rodata;
    if (name == ".sdata") return Class::Sdata;
    if (name == ".vutext" || name == ".vudata") return Class::Vu;
    return Class::Other;
}

// A byte region of the file and what it holds: a loaded section (with its address) or not
struct Region { uint64_t begin, end; Class cls; uint32_t addr; std::string name; };

std::vector<Region> RegionsOf(const ElfImage& elf, size_t file_size) {
    std::vector<Region> out;
    if (!elf.sections.empty()) {
        for (const ElfSection& s : elf.sections) {
            if (s.type == 8 /* SHT_NOBITS */ || s.size == 0 || s.type == 0) continue;
            const uint64_t b = s.offset, e = uint64_t(s.offset) + s.size;
            if (b >= file_size) continue;
            // a section the port loads has an address (the .DVP overlays and the tables have none)
            const Class c = s.addr ? ClassOf(s.name) : Class::Other;
            out.push_back({b, std::min<uint64_t>(e, file_size), c, s.addr, s.name});
        }
    } else {
        for (const ElfImage::Load& l : elf.loads)
            for (const RetailRange& r : kRetail) {
                const uint32_t lo = std::max(l.vaddr, r.begin), hi = std::min(l.vaddr + l.filesz, r.end);
                if (lo >= hi) continue;
                out.push_back({uint64_t(l.offset) + (lo - l.vaddr), uint64_t(l.offset) + (hi - l.vaddr), ClassOf(r.name), lo, r.name});
            }
    }
    std::sort(out.begin(), out.end(), [](const Region& a, const Region& b) { return a.begin < b.begin; });
    return out;
}

}  // namespace

std::string FunctionAt(uint32_t address, uint32_t* start) {
    if (start) *start = address;
    if (kFunctionNameCount == 0 || address < kCodeStart || address >= kCodeEnd) return "code at " + Hex32(address);
    const FunctionName* begin = kFunctionNames;
    const FunctionName* end = kFunctionNames + kFunctionNameCount;
    const FunctionName* it = std::upper_bound(begin, end, address, [](uint32_t a, const FunctionName& f) { return a < f.address; });
    if (it == begin) return "code at " + Hex32(address);
    --it;
    if (start) *start = it->address;
    std::string s = it->symbol;
    if (it->cxx && it->cxx[0]) s += std::string(" ") + it->cxx;
    return s;
}

bool IsFunctionStart(uint32_t address) {
    const FunctionName* begin = kFunctionNames;
    const FunctionName* end = kFunctionNames + kFunctionNameCount;
    const FunctionName* it = std::lower_bound(begin, end, address, [](const FunctionName& f, uint32_t a) { return f.address < a; });
    return it != end && it->address == address;
}

bool ParseElf(const std::vector<uint8_t>& b, ElfImage* out, std::string* error) {
    *out = ElfImage{};
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (b.size() < 52 || b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') return fail("it is not an ELF executable");
    if (b[4] != 1 || b[5] != 1) return fail("it is not a 32-bit little-endian ELF executable");
    if (Le16(&b[18]) != 8) return fail("it is not a MIPS executable");
    out->entry = Le32(&b[24]);
    const uint32_t phoff = Le32(&b[28]), shoff = Le32(&b[32]);
    const uint16_t phentsize = Le16(&b[42]), phnum = Le16(&b[44]), shentsize = Le16(&b[46]), shnum = Le16(&b[48]), shstrndx = Le16(&b[50]);
    if (phentsize < 32 || uint64_t(phoff) + uint64_t(phentsize) * phnum > b.size()) return fail("its program headers are outside the file");
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t* p = &b[phoff + size_t(i) * phentsize];
        if (Le32(p) != 1) continue;
        ElfImage::Load l{Le32(p + 4), Le32(p + 8), Le32(p + 16), Le32(p + 20)};
        if (uint64_t(l.offset) + l.filesz > b.size()) return fail("a loaded segment is outside the file");
        out->loads.push_back(l);
    }
    if (shnum && shentsize >= 40 && uint64_t(shoff) + uint64_t(shentsize) * shnum <= b.size() && shstrndx < shnum) {
        const uint8_t* strhdr = &b[shoff + size_t(shstrndx) * shentsize];
        const uint32_t str_off = Le32(strhdr + 16), str_size = Le32(strhdr + 20);
        const bool names_ok = uint64_t(str_off) + str_size <= b.size();
        for (uint16_t i = 0; i < shnum; ++i) {
            const uint8_t* s = &b[shoff + size_t(i) * shentsize];
            ElfSection sec;
            const uint32_t name_at = Le32(s);
            if (names_ok && name_at < str_size) {
                const char* p = reinterpret_cast<const char*>(&b[str_off + name_at]);
                size_t n = 0;
                while (name_at + n < str_size && p[n]) ++n;
                sec.name.assign(p, n);
            }
            sec.type = Le32(s + 4);
            sec.flags = Le32(s + 8);
            sec.addr = Le32(s + 12);
            sec.offset = Le32(s + 16);
            sec.size = Le32(s + 20);
            out->sections.push_back(sec);
        }
    }
    return true;
}

bool DiffExecutables(const std::vector<uint8_t>& base, const std::vector<uint8_t>& mod, ExecutableDiff* d, std::string* error) {
    *d = ExecutableDiff{};
    ElfImage be, me;
    std::string why;
    if (!ParseElf(mod, &me, &why)) {
        if (error) *error = "the mod's SLES_525.68 is not the game's kind of program: " + why;
        return false;
    }
    if (!ParseElf(base, &be, &why)) {
        if (error) *error = "the disc's SLES_525.68 does not read as a program: " + why;
        return false;
    }
    d->base_entry = be.entry;
    d->mod_entry = me.entry;
    d->same_size = base.size() == mod.size();
    if (me.entry != kEntry)
        d->warnings.push_back("its program starts at " + Hex32(me.entry) + ", not " + Hex32(kEntry) +
                              ": the game will not start with it (the game reads only the retail PAL program's layout)");
    auto add = [&](Class c, uint64_t n) {
        switch (c) {
            case Class::Text: d->text += n; break;
            case Class::Data: d->data += n; break;
            case Class::Rodata: d->rodata += n; break;
            case Class::Sdata: d->sdata += n; break;
            case Class::Vu: d->vu += n; break;
            case Class::Other: d->other += n; break;
        }
    };
    std::set<uint32_t> changed_code;     // .text addresses
    std::set<uint32_t> changed_words;    // 4-aligned data-image addresses whose word changed
    // Same layout (the usual case: a patched executable keeps every section where it was): compare byte by byte, classified
    // by the base's sections; otherwise section by section by name.
    const std::vector<Region> regions = RegionsOf(be, base.size());
    bool same_layout = d->same_size && be.sections.size() == me.sections.size();
    for (size_t i = 0; same_layout && i < be.sections.size(); ++i) {
        const ElfSection &a = be.sections[i], &b = me.sections[i];
        same_layout = a.name == b.name && a.addr == b.addr && a.offset == b.offset && a.size == b.size && a.type == b.type;
    }
    if (same_layout && be.sections.empty()) same_layout = be.loads.size() == me.loads.size();
    if (same_layout) {
        size_t r = 0;
        for (size_t off = 0; off < base.size(); ++off) {
            if (base[off] == mod[off]) continue;
            while (r < regions.size() && regions[r].end <= off) ++r;
            const bool inside = r < regions.size() && regions[r].begin <= off;
            const Class c = inside ? regions[r].cls : Class::Other;
            add(c, 1);
            if (inside && c != Class::Other) {
                const uint32_t addr = regions[r].addr + uint32_t(off - regions[r].begin);
                if (c == Class::Text) changed_code.insert(addr);
                else changed_words.insert(addr & ~3u);
            }
        }
    } else {
        // by section name: the bytes of each section compared where both have them, the rest counted as changed
        std::map<std::string, const ElfSection*> mine;
        for (const ElfSection& s : me.sections) mine[s.name] = &s;
        for (const Region& rg : regions) {
            const auto it = mine.find(rg.name);
            const uint64_t len = rg.end - rg.begin;
            if (it == mine.end()) { add(rg.cls, len); continue; }
            const ElfSection& s = *it->second;
            const uint64_t mlen = std::min<uint64_t>(s.size, mod.size() > s.offset ? mod.size() - s.offset : 0);
            const uint64_t common = std::min(len, mlen);
            for (uint64_t k = 0; k < common; ++k) {
                if (base[size_t(rg.begin + k)] == mod[size_t(s.offset + k)]) continue;
                add(rg.cls, 1);
                if (rg.cls == Class::Text) changed_code.insert(rg.addr + uint32_t(k));
                else if (rg.cls != Class::Other) changed_words.insert((rg.addr + uint32_t(k)) & ~3u);
            }
            add(rg.cls, std::max(len, mlen) - common);
            if (s.addr != rg.addr && rg.cls != Class::Other)
                d->warnings.push_back("its " + rg.name + " section moved from " + Hex32(rg.addr) + " to " + Hex32(s.addr) +
                                      ": the game reads the data image at the retail addresses, so this program will not work");
        }
        d->warnings.push_back("its layout differs from the disc's program (sections resized or moved): the changes are counted section by section");
    }
    // code ranges and the functions they fall in
    for (uint32_t a : changed_code) {
        if (!d->code_ranges.empty() && d->code_ranges.back().second == a) d->code_ranges.back().second = a + 1;
        else d->code_ranges.emplace_back(a, a + 1);
    }
    std::vector<std::pair<uint32_t, std::string>> funcs;
    for (const auto& r : d->code_ranges)
        for (uint32_t a = r.first; a < r.second;) {
            uint32_t start = a;
            const std::string name = FunctionAt(a, &start);
            if (funcs.empty() || funcs.back().second != name) funcs.emplace_back(start, name);
            // jump to the next function's start (or the range's end)
            const FunctionName* end = kFunctionNames + kFunctionNameCount;
            const FunctionName* it = std::upper_bound(kFunctionNames, end, a, [](uint32_t x, const FunctionName& f) { return x < f.address; });
            const uint32_t next = (kFunctionNameCount && it != end) ? it->address : r.second;
            a = std::max(a + 1, std::min(next, r.second));
        }
    std::sort(funcs.begin(), funcs.end());
    funcs.erase(std::unique(funcs.begin(), funcs.end()), funcs.end());
    for (const auto& f : funcs) d->functions.push_back(f.second);
    // data words that now point into the code: the port turns a word holding a function's address into its own function
    // index at start-up, so a pointer moved to another existing function works (the port's version of it runs); one moved
    // into the mod's own changed code, or into the middle of a function, would make the game call an address it has no
    // function for -- that word is part of the code change and is given back its disc value
    auto in_changed_code = [&](uint32_t v) {
        for (const auto& r : d->code_ranges)
            if (v >= r.first && v < r.second) return true;
        uint32_t start = v;
        const std::string fn = FunctionAt(v, &start);
        return std::find(d->functions.begin(), d->functions.end(), fn) != d->functions.end();
    };
    auto class_at = [](uint32_t addr) {
        for (const RetailRange& r : kRetail)
            if (addr >= r.begin && addr < r.end) return ClassOf(r.name);
        return Class::Other;
    };
    for (uint32_t w : changed_words) {
        if (w < kDataImageBegin || w + 4 > kDataImageEnd || !same_layout) continue;
        uint64_t off = UINT64_MAX;
        for (const ElfImage::Load& l : be.loads)
            if (w >= l.vaddr && w + 4 <= l.vaddr + l.filesz) off = uint64_t(l.offset) + (w - l.vaddr);
        if (off == UINT64_MAX || off + 4 > mod.size() || off + 4 > base.size()) continue;
        const uint32_t now = Le32(&mod[size_t(off)]), was = Le32(&base[size_t(off)]);
        if (now == was || now < 0x00100000u || now >= 0x002D9D88u) continue;
        const bool mods_code = in_changed_code(now);
        if (!mods_code && IsFunctionStart(now)) continue;   // another existing function: the port's version of it runs
        ExecutableDiff::Restore r;
        r.addr = w;
        r.file_offset = off;
        r.was = was;
        r.now = now;
        r.target = FunctionAt(now);
        d->restored.push_back(r);
        uint64_t bytes = 0;
        for (int k = 0; k < 4; ++k) bytes += base[size_t(off) + k] != mod[size_t(off) + k];
        switch (class_at(w)) {
            case Class::Data: d->data -= std::min(d->data, bytes); break;
            case Class::Rodata: d->rodata -= std::min(d->rodata, bytes); break;
            case Class::Sdata: d->sdata -= std::min(d->sdata, bytes); break;
            case Class::Vu: d->vu -= std::min(d->vu, bytes); break;
            default: break;
        }
        d->warnings.push_back("the data word at " + Hex32(w) + " now points to " + Hex32(now) + " (" + r.target + ")" +
                              (mods_code ? ", the mod's own new code," : ", the middle of a function,") +
                              " which this port cannot run: it keeps the disc's value (" + Hex32(was) + ", " + FunctionAt(was) +
                              "), so that part of the mod does not apply");
    }
    if (d->other)
        d->warnings.push_back(std::to_string(d->other) + " changed byte(s) of the program lie outside what the game loads "
                              "(its headers or the VU overlay copies): they change nothing");
    return true;
}

}  // namespace te::modimport
