// region_map_make -- see region_map_make.h. The algorithm is tools/discs/elf_region_map.py's `map` (schema 2), step for step,
// and the JSON is written exactly as Python's json.dumps(doc, indent=1) writes it: the two tools make the same file
// (platform/nativeee/tests/test_region_map compares them byte for byte).
#include "app/host/region_map_make.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "app/host/disc_regions.h"
#include "engine/cdvd/iso_fs.h"

namespace ps2r::app {
namespace {

// ---------------------------------------------------------------------------------------------------- constants (the tool's)
constexpr uint32_t kPalImageStart = 0x2D9D90, kPalBssStart = 0x30A480, kPalImageEnd = 0x3DB200;
constexpr uint32_t kPatchStart = 0x2E6F00, kPatchEnd = 0x30A460;
constexpr uint64_t kTailBit = 1ull << 40, kPointerToken = 1ull << 41, kCodeTag = 1ull << 42, kOtherCodeTag = 1ull << 43,
                   kStringTag = 1ull << 44, kMask40 = (1ull << 40) - 1;
constexpr uint32_t kShfAlloc = 2, kNoBits = 8;
constexpr uint32_t kNone = 0xFFFFFFFFu;   // "None" for an address
constexpr int kLuiWindow = 64, kGapMax = 64, kCallWindow = 32;
constexpr uint32_t kGpOffset = 0x7FF0;
const char* const kDataSections[] = {".vutext", ".data", ".vudata", ".rodata", ".sdata"};
const char* const kBssSections[] = {".sbss", ".bss"};
const char* const kPointerSections[] = {".data", ".rodata", ".sdata"};
const char* g_default_symbols = nullptr;

bool In(uint32_t op, std::initializer_list<uint32_t> set) {
    for (uint32_t x : set)
        if (x == op) return true;
    return false;
}
const std::initializer_list<uint32_t> kAddiOps = {0x08, 0x09, 0x18, 0x19};
const std::initializer_list<uint32_t> kLoadsGpr = {0x1A, 0x1B, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37};
const std::initializer_list<uint32_t> kLoadsOther = {0x31, 0x36};
const std::initializer_list<uint32_t> kStores = {0x1F, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x39, 0x3E, 0x3F};
const std::initializer_list<uint32_t> kImmAluDest = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19};
const std::initializer_list<uint32_t> kBranchOps = {0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17};
bool MemoryOp(uint32_t op) { return In(op, kLoadsGpr) || In(op, kLoadsOther) || In(op, kStores) || op == 0x33; }

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

std::string Hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// ---------------------------------------------------------------------------------------------------- JSON (Python's form)
struct J {
    enum Kind { Null, Bool, Int, Str, Arr, Obj } kind = Null;
    bool b = false;
    int64_t i = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    J() = default;
    static J N() { return J(); }
    static J B(bool v) { J j; j.kind = Bool; j.b = v; return j; }
    static J I(int64_t v) { J j; j.kind = Int; j.i = v; return j; }
    static J S(std::string v) { J j; j.kind = Str; j.s = std::move(v); return j; }
    static J A() { J j; j.kind = Arr; return j; }
    static J O() { J j; j.kind = Obj; return j; }
    J& Add(const std::string& k, J v) { o.emplace_back(k, std::move(v)); return *this; }
    J& Push(J v) { a.push_back(std::move(v)); return *this; }
};

void DumpString(const std::string& s, std::string& out) {   // json.dumps, ensure_ascii=True
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20 || c > 0x7E) {   // control characters; a byte past ASCII (latin-1) as its code point
                    if (c == 0x7F) { out += char(c); break; }
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
}

void Dump(const J& v, int level, std::string& out) {   // json.dumps(indent=1)
    switch (v.kind) {
        case J::Null: out += "null"; return;
        case J::Bool: out += v.b ? "true" : "false"; return;
        case J::Int: out += std::to_string(v.i); return;
        case J::Str: DumpString(v.s, out); return;
        case J::Arr:
            if (v.a.empty()) { out += "[]"; return; }
            out += "[\n";
            for (size_t k = 0; k < v.a.size(); ++k) {
                out += std::string(size_t(level + 1), ' ');
                Dump(v.a[k], level + 1, out);
                out += k + 1 < v.a.size() ? ",\n" : "\n";
            }
            out += std::string(size_t(level), ' ') + "]";
            return;
        case J::Obj:
            if (v.o.empty()) { out += "{}"; return; }
            out += "{\n";
            for (size_t k = 0; k < v.o.size(); ++k) {
                out += std::string(size_t(level + 1), ' ');
                DumpString(v.o[k].first, out);
                out += ": ";
                Dump(v.o[k].second, level + 1, out);
                out += k + 1 < v.o.size() ? ",\n" : "\n";
            }
            out += std::string(size_t(level), ' ') + "}";
            return;
    }
}

J Pair(uint32_t a, uint32_t b) { return J::A().Push(J::S(Hex(a))).Push(J::S(Hex(b))); }

// ---------------------------------------------------------------------------------------------------- the ELF
struct Section { uint32_t addr = 0, size = 0, type = 0; };

struct Elf {
    const std::vector<uint8_t>* bytes = nullptr;
    uint32_t entry = 0, lo = 0, hi = 0, code_lo = 0, code_hi = 0, gp = 0, image_lo = 0;
    struct Load { uint32_t vaddr, offset, filesz, memsz; };
    std::vector<Load> loads;
    std::map<std::string, Section> sections;

    bool Parse(const std::vector<uint8_t>& e, std::string* error) {
        bytes = &e;
        if (e.size() < 52 || std::memcmp(e.data(), "\x7f" "ELF", 4) != 0 || e[4] != 1 || e[5] != 1) {
            if (error) *error = "not a 32-bit little-endian ELF";
            return false;
        }
        entry = Rd32(&e[24]);
        const uint32_t phoff = Rd32(&e[28]), shoff = Rd32(&e[32]);
        const uint16_t phentsize = Rd16(&e[42]), phnum = Rd16(&e[44]), shentsize = Rd16(&e[46]), shnum = Rd16(&e[48]),
                       shstrndx = Rd16(&e[50]);
        if (uint64_t(phoff) + uint64_t(phentsize) * phnum > e.size() || phentsize < 32) {
            if (error) *error = "program headers out of range";
            return false;
        }
        for (uint16_t i = 0; i < phnum; ++i) {
            const uint8_t* ph = &e[phoff + size_t(i) * phentsize];
            if (Rd32(ph) != 1) continue;
            Load l{Rd32(ph + 8), Rd32(ph + 4), Rd32(ph + 16), Rd32(ph + 20)};
            if (uint64_t(l.offset) + l.filesz > e.size()) {
                if (error) *error = "a PT_LOAD's file range is out of the ELF";
                return false;
            }
            loads.push_back(l);
        }
        if (loads.empty()) {
            if (error) *error = "no PT_LOAD segment";
            return false;
        }
        lo = 0xFFFFFFFFu;
        for (const Load& l : loads) {
            lo = std::min(lo, l.vaddr);
            hi = std::max(hi, l.vaddr + l.memsz);
        }
        if (shoff && shnum && shentsize >= 40 && uint64_t(shoff) + uint64_t(shentsize) * shnum <= e.size() && shstrndx < shnum) {
            const uint32_t stroff = Rd32(&e[shoff + size_t(shstrndx) * shentsize + 16]);
            for (uint16_t i = 0; i < shnum; ++i) {
                const uint8_t* sh = &e[shoff + size_t(i) * shentsize];
                const uint32_t name = Rd32(sh), type = Rd32(sh + 4), flags = Rd32(sh + 8), addr = Rd32(sh + 12), size = Rd32(sh + 20);
                if (!(flags & kShfAlloc) || !addr || uint64_t(stroff) + name >= e.size()) continue;
                const char* n = reinterpret_cast<const char*>(&e[stroff + name]);
                sections[std::string(n, strnlen(n, e.size() - stroff - name))] = Section{addr, size, type};
            }
        }
        auto t = sections.find(".text");
        if (t != sections.end()) { code_lo = t->second.addr; code_hi = t->second.addr + t->second.size; }
        else { code_lo = code_hi = lo; }
        auto sd = sections.find(".sdata");
        gp = sd != sections.end() ? sd->second.addr + kGpOffset : 0;
        image_lo = hi;
        bool any = false;
        for (const char* n : kDataSections) {
            auto s = sections.find(n);
            if (s == sections.end()) continue;
            image_lo = any ? std::min(image_lo, s->second.addr) : s->second.addr;
            any = true;
        }
        return true;
    }
    const Section* Find(const char* name) const {
        auto it = sections.find(name);
        return it == sections.end() ? nullptr : &it->second;
    }
    void Read(uint32_t vaddr, uint32_t n, uint8_t* out) const {
        std::memset(out, 0, n);
        for (const Load& l : loads) {
            const uint64_t a = std::max<uint64_t>(vaddr, l.vaddr), b = std::min<uint64_t>(uint64_t(vaddr) + n, uint64_t(l.vaddr) + l.filesz);
            if (a < b) std::memcpy(out + (a - vaddr), bytes->data() + l.offset + (a - l.vaddr), size_t(b - a));
        }
    }
    std::vector<uint8_t> Read(uint32_t vaddr, uint32_t n) const {
        std::vector<uint8_t> out(n);
        if (n) Read(vaddr, n, out.data());
        return out;
    }
    uint32_t Word(uint32_t vaddr) const {
        uint8_t b[4];
        Read(vaddr, 4, b);
        return Rd32(b);
    }
    bool PointerLike(uint64_t w) const { return w < (1ull << 32) && w >= lo && w < hi; }
    bool InCode(uint64_t w) const { return w >= code_lo && w < code_hi; }
    bool InImage(uint32_t w) const { return w >= image_lo && w <= hi; }
    // the printable NUL-terminated string at a (1..63 characters)
    bool CString(uint32_t a, std::string* s) const {
        uint8_t b[64];
        Read(a, 64, b);
        const void* z = std::memchr(b, 0, 64);
        if (!z) return false;
        const size_t n = size_t(static_cast<const uint8_t*>(z) - b);
        if (n == 0) return false;
        for (size_t k = 0; k < n; ++k)
            if (!((b[k] >= 0x20 && b[k] < 0x7F) || b[k] == 9 || b[k] == 10 || b[k] == 13)) return false;
        if (s) s->assign(reinterpret_cast<const char*>(b), n);
        return true;
    }
};

std::vector<uint64_t> Words(const std::vector<uint8_t>& blob) {
    std::vector<uint64_t> out;
    const size_t n = blob.size() / 4;
    out.reserve(n + 1);
    for (size_t i = 0; i < n; ++i) out.push_back(Rd32(&blob[i * 4]));
    if (blob.size() % 4) {
        uint64_t tail = 0;
        for (size_t k = 0; k < blob.size() % 4; ++k) tail |= uint64_t(blob[n * 4 + k]) << (8 * k);
        out.push_back(tail | kTailBit);
    }
    return out;
}

uint64_t Fnv40(const std::string& s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001B3ull;
    return h & kMask40;
}

size_t UpperBound(const std::vector<uint32_t>& v, uint32_t x) { return size_t(std::upper_bound(v.begin(), v.end(), x) - v.begin()); }
uint32_t NextFunction(const std::vector<uint32_t>& f, uint32_t p) {
    const size_t i = UpperBound(f, p);
    return i < f.size() ? f[i] : 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------------------------------- 1 code
void CodeScan(const Elf& elf, const std::vector<uint32_t>& ws, std::vector<uint32_t>* refs, std::vector<uint64_t>* toks) {
    refs->assign(ws.size(), kNone);
    toks->assign(ws.begin(), ws.end());
    int64_t hi[32];
    size_t at[32] = {};
    std::fill(std::begin(hi), std::end(hi), -1);
    bool clear_after = false;
    const uint32_t gp = elf.gp;
    for (size_t k = 0; k < ws.size(); ++k) {
        const uint32_t w = ws[k], op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, imm = w & 0xFFFF;
        const int32_t simm = (imm & 0x8000) ? int32_t(imm) - 0x10000 : int32_t(imm);
        if (op == 0x02 || op == 0x03) {
            (*toks)[k] = w & 0xFC000000u;
        } else if (In(op, kBranchOps) || ((op == 0x11 || op == 0x12) && rs == 8)) {
            (*toks)[k] = w & 0xFFFF0000u;
        } else if (op == 0x0F) {
            hi[rt] = int64_t(imm) << 16;
            at[rt] = k;
            (*toks)[k] = w & 0xFFFF0000u;
        } else {
            bool has = false;
            uint32_t v = 0;
            if (In(op, kAddiOps) || MemoryOp(op)) {
                if (rs == 28 && gp) { v = uint32_t(int64_t(gp) + simm); has = true; }
                else if (rs && hi[rs] >= 0 && k - at[rs] <= size_t(kLuiWindow)) { v = uint32_t(hi[rs] + simm); has = true; }
            } else if (op == 0x0D && rs && rs != 28 && hi[rs] >= 0 && k - at[rs] <= size_t(kLuiWindow)) {
                v = uint32_t(hi[rs]) | imm;
                has = true;
            }
            if (has && (elf.PointerLike(v) || v == elf.hi)) {
                (*toks)[k] = w & 0xFFFF0000u;
                if (elf.InImage(v)) (*refs)[k] = v;
            }
            uint32_t dest = 0;
            if (In(op, kImmAluDest) || In(op, kLoadsGpr)) dest = rt;
            else if (op == 0x00 || op == 0x1C) dest = (w >> 11) & 31;
            if (dest) hi[dest] = -1;
        }
        if (clear_after) {
            std::fill(std::begin(hi), std::end(hi), -1);
            clear_after = false;
        }
        if (op == 0x03 || (op == 0 && (w & 0x3F) == 0x09)) clear_after = true;
    }
}

uint32_t DestOf(uint32_t w) {
    const uint32_t op = w >> 26;
    if (In(op, kImmAluDest) || In(op, kLoadsGpr)) return (w >> 16) & 31;
    if (op == 0x00 || op == 0x1C) return (w >> 11) & 31;
    return 0;
}

struct CallSite { uint32_t site; std::map<uint32_t, uint32_t> args; };   // argument register -> address

std::map<uint32_t, std::vector<CallSite>> CallSites(const Elf& elf, const std::vector<uint32_t>& ws, const std::vector<uint32_t>& refs) {
    std::map<uint32_t, std::vector<CallSite>> out;
    const uint32_t lo = elf.code_lo;
    for (size_t k = 0; k < ws.size(); ++k) {
        const uint32_t w = ws[k];
        if (w >> 26 != 0x03) continue;
        const uint32_t site = lo + uint32_t(4 * k);
        const uint32_t t = (site & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
        std::vector<size_t> window;
        if (k + 1 < ws.size()) window.push_back(k + 1);
        for (size_t x = k; x-- > 0 && k - x <= size_t(kCallWindow);) {
            const uint32_t wx = ws[x];
            if (wx >> 26 == 0x03 || (wx >> 26 == 0 && ((wx & 0x3F) == 0x08 || (wx & 0x3F) == 0x09))) break;
            window.push_back(x);
        }
        CallSite cs{site, {}};
        std::set<uint32_t> seen;
        for (size_t x : window) {
            const uint32_t d = DestOf(ws[x]);
            if (d >= 4 && d <= 7 && !seen.count(d)) {
                seen.insert(d);
                if (refs[x] != kNone) cs.args[d] = refs[x];
            }
        }
        out[t].push_back(std::move(cs));
    }
    return out;
}

using Votes = std::map<uint32_t, std::map<uint32_t, uint32_t>>;   // PAL address -> other address -> votes

struct CodeMap {
    const Elf* pal = nullptr;
    const Elf* other = nullptr;
    std::vector<uint32_t> pw, ow, functions;
    std::set<uint32_t> function_set;
    std::vector<int32_t> o2p, p2o;   // instruction index -> instruction index (-1 none)
    std::vector<uint64_t> ptoks;
    Votes votes, gap_votes, call_votes;
    std::vector<uint32_t> pal_refs, other_refs;
    uint64_t equal_pairs = 0, replace_pairs = 0;

    void Build(const Elf& p, const Elf& o, const std::vector<uint32_t>& pal_functions) {
        pal = &p;
        other = &o;
        auto words_of = [](const Elf& e) {
            std::vector<uint32_t> ws;
            if (e.code_hi > e.code_lo) {
                const std::vector<uint8_t> b = e.Read(e.code_lo, e.code_hi - e.code_lo);
                ws.resize(b.size() / 4);
                for (size_t i = 0; i < ws.size(); ++i) ws[i] = Rd32(&b[4 * i]);
            }
            return ws;
        };
        pw = words_of(p);
        ow = words_of(o);
        functions = pal_functions;
        std::sort(functions.begin(), functions.end());
        functions.erase(std::unique(functions.begin(), functions.end()), functions.end());
        function_set.insert(functions.begin(), functions.end());
        o2p.assign(ow.size(), -1);
        p2o.assign(pw.size(), -1);
        std::vector<uint32_t> prefs, orefs;
        std::vector<uint64_t> otoks;
        CodeScan(p, pw, &prefs, &ptoks);
        CodeScan(o, ow, &orefs, &otoks);
        const std::vector<MatchBlock> blocks = MatchingBlocks(ptoks, otoks);
        uint32_t i = 0, j = 0;
        for (const MatchBlock& m : blocks) {
            const uint32_t gi = m.a - i, gj = m.b - j;
            if (gi > 0 && gj > 0) {
                if (gi == gj) {
                    for (uint32_t k = 0; k < gi; ++k) { o2p[j + k] = int32_t(i + k); p2o[i + k] = int32_t(j + k); }
                    replace_pairs += gi;
                }
                if (gi <= uint32_t(kGapMax) && gj <= uint32_t(kGapMax)) {
                    std::vector<uint32_t> pr, orr;
                    for (uint32_t x = i; x < m.a; ++x) if (prefs[x] != kNone) pr.push_back(prefs[x]);
                    for (uint32_t x = j; x < m.b; ++x) if (orefs[x] != kNone) orr.push_back(orefs[x]);
                    if (!pr.empty() && pr.size() == orr.size())
                        for (size_t k = 0; k < pr.size(); ++k) ++gap_votes[pr[k]][orr[k]];
                }
            }
            for (uint32_t k = 0; k < m.n; ++k) {
                o2p[m.b + k] = int32_t(m.a + k);
                p2o[m.a + k] = int32_t(m.b + k);
                const uint32_t pv = prefs[m.a + k], ov = orefs[m.b + k];
                if (pv != kNone && ov != kNone) ++votes[pv][ov];
            }
            equal_pairs += m.n;
            i = m.a + m.n;
            j = m.b + m.n;
        }
        std::set<uint32_t> pr(prefs.begin(), prefs.end()), orr(orefs.begin(), orefs.end());
        pr.erase(kNone);
        orr.erase(kNone);
        pal_refs.assign(pr.begin(), pr.end());
        other_refs.assign(orr.begin(), orr.end());
        // the arguments of the calls to one function, paired call site by call site when both have as many
        const auto pcalls = CallSites(p, pw, prefs), ocalls = CallSites(o, ow, orefs);
        std::vector<uint32_t> otargets;
        for (const auto& kv : ocalls) otargets.push_back(kv.first);
        for (const auto& kv : pcalls) {
            const uint32_t t = kv.first;
            if (t < p.code_lo || t >= p.code_hi) continue;
            const int32_t jj = p2o[(t - p.code_lo) / 4];
            if (jj < 0) continue;
            const int64_t guess = int64_t(o.code_lo) + 4 * int64_t(jj);
            size_t at = size_t(std::lower_bound(otargets.begin(), otargets.end(), uint32_t(std::max<int64_t>(0, guess - 16))) - otargets.begin());
            if (guess - 16 < 0) at = 0;
            bool any = false;
            uint32_t best = 0;
            for (size_t x = at; x < otargets.size() && x < at + 8; ++x) {
                const int64_t d = std::llabs(int64_t(otargets[x]) - guess);
                if (d > 16) continue;
                if (!any || d < std::llabs(int64_t(best) - guess) || (d == std::llabs(int64_t(best) - guess) && otargets[x] < best)) best = otargets[x];
                any = true;
            }
            if (!any) continue;
            const std::vector<CallSite>& ps = kv.second;
            const std::vector<CallSite>& os = ocalls.at(best);
            if (ps.size() != os.size() || ps.size() > 16) continue;
            for (size_t k = 0; k < ps.size(); ++k)
                for (const auto& reg : ps[k].args) {
                    auto it = os[k].args.find(reg.first);
                    if (it != os[k].args.end()) ++call_votes[reg.second][it->second];
                }
        }
    }

    uint32_t Translate(uint32_t o) const {
        if (o < other->code_lo || o >= other->code_hi) return kNone;
        const int32_t i = o2p[(o - other->code_lo) / 4];
        return i < 0 ? kNone : pal->code_lo + 4 * uint32_t(i) + (o - other->code_lo) % 4;
    }
    uint32_t Function(uint32_t o) const {
        const uint32_t t = Translate(o);
        if (t == kNone || function_set.count(t) || functions.empty()) return t;
        const size_t k = UpperBound(functions, t);
        if (k == 0) return t;
        const uint32_t f = functions[k - 1];
        if (t - f > 16) return t;
        const int32_t j = p2o[(f - pal->code_lo) / 4];
        if (j < 0) return t;
        const uint64_t of = uint64_t(other->code_lo) + 4 * uint64_t(j);
        return o <= of && of <= uint64_t(o) + 16 ? f : t;
    }
    bool Twins(uint32_t f, uint32_t g) const {
        if (!function_set.count(f) || !function_set.count(g)) return false;
        const uint32_t fe = NextFunction(functions, f), ge = NextFunction(functions, g);
        if (uint64_t(fe) - f != uint64_t(ge) - g || uint64_t(fe) - f > 4096) return false;
        const size_t a = (f - pal->code_lo) / 4, b = (g - pal->code_lo) / 4, n = (fe - f) / 4;
        if (a + n > ptoks.size() || b + n > ptoks.size()) return false;
        return std::equal(ptoks.begin() + long(a), ptoks.begin() + long(a + n), ptoks.begin() + long(b));
    }
    // (other, votes, kind) -- other = kNone for none
    std::tuple<uint32_t, uint32_t, std::string> Vote(uint32_t p, uint32_t kinds = 7) const {
        const Votes* tables[] = {&votes, &call_votes, &gap_votes};
        const char* names[] = {"code", "call", "code gap"};
        for (int k = 0; k < 3; ++k) {
            if (!(kinds & (1u << k))) continue;
            auto it = tables[k]->find(p);
            if (it == tables[k]->end() || it->second.empty()) continue;
            uint32_t best = 0, count = 0;
            for (const auto& v : it->second)
                if (v.second > count) { best = v.first; count = v.second; }   // ties: the lowest address (the map is sorted)
            return {best, count, names[k]};
        }
        return {kNone, 0, ""};
    }
};

// ---------------------------------------------------------------------------------------------------- 2 data
template <typename Tr>
std::vector<uint64_t> DataTokens(const Elf& elf, uint32_t addr, uint32_t size, const Tr& code_tr) {
    std::vector<uint64_t> out;
    for (uint64_t w : Words(elf.Read(addr, size))) {
        if (w >= (1ull << 32)) out.push_back(w);
        else if (elf.InCode(w)) {
            const uint32_t t = code_tr(uint32_t(w));
            out.push_back(t != kNone ? (kCodeTag | t) : (kOtherCodeTag | w));
        } else if (elf.PointerLike(w) || w == elf.hi) {
            std::string s;
            if (w < elf.hi && elf.CString(uint32_t(w), &s)) out.push_back(kStringTag | Fnv40(s));
            else out.push_back(kPointerToken);
        } else {
            out.push_back(w);
        }
    }
    return out;
}

struct Run { uint32_t i, j, n; std::string kind; };

template <typename Rule>
std::vector<Run> AlignedRuns(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b, const Rule& start_rule) {
    std::vector<Run> out;
    uint32_t i = 0, j = 0;
    for (const MatchBlock& m : MatchingBlocks(a, b)) {
        const uint32_t gi = m.a - i, gj = m.b - j;
        if (gi > 0 && gj > 0) {
            if (gi == gj) out.push_back({i, j, gi, "replace"});
            else if (start_rule(i, m.a, j, m.b)) out.push_back({i, j, std::min(gi, gj), "start"});
        }
        if (m.n) out.push_back({m.a, m.b, m.n, "equal"});
        i = m.a + m.n;
        j = m.b + m.n;
    }
    return out;
}

bool InsideOne(const std::vector<uint32_t>& starts, uint32_t lo, uint32_t hi) {
    const size_t k = UpperBound(starts, lo);
    return k >= starts.size() || starts[k] >= hi;
}

struct Row { std::string section; uint32_t pal = 0, other = 0, bytes = 0; std::string kind; };

template <typename Tr>
std::vector<Row> AlignBlocks(const Elf& pal, const Elf& other, uint32_t pa, uint32_t pn, uint32_t oa, uint32_t on, const Tr& code_tr,
                             const std::string& section, const std::vector<uint32_t>* pal_starts, const std::vector<uint32_t>* other_starts) {
    const auto identity = [](uint32_t w) { return w; };
    const std::vector<uint64_t> pt = DataTokens(pal, pa, pn, identity), ot = DataTokens(other, oa, on, code_tr);
    auto rule = [&](uint32_t i, uint32_t ie, uint32_t j, uint32_t je) {
        if (!pal_starts) return true;
        return InsideOne(*pal_starts, pa + 4 * i, pa + 4 * ie) && InsideOne(*other_starts, oa + 4 * j, oa + 4 * je);
    };
    std::vector<Row> rows;
    for (const Run& r : AlignedRuns(pt, ot, rule)) {
        const int64_t b = std::min<int64_t>({int64_t(4) * r.n, int64_t(pa) + pn - (int64_t(pa) + 4 * int64_t(r.i)),
                                             int64_t(oa) + on - (int64_t(oa) + 4 * int64_t(r.j))});
        if (b > 0) rows.push_back({section, pa + 4 * r.i, oa + 4 * r.j, uint32_t(b), r.kind});
    }
    return rows;
}

std::vector<Row> MergeRanges(const std::vector<Row>& rows) {
    std::vector<Row> out;
    for (const Row& r : rows) {
        if (!out.empty() && out.back().section == r.section && out.back().pal + out.back().bytes == r.pal && out.back().other + out.back().bytes == r.other) {
            out.back().bytes += r.bytes;
            if (out.back().kind != r.kind) out.back().kind = "mixed";
        } else {
            out.push_back(r);
        }
    }
    return out;
}

struct Ranges {
    struct T { uint32_t key, value, bytes; };
    std::vector<T> by_other, by_pal;
    explicit Ranges(const std::vector<Row>& rows) {
        for (const Row& r : rows) {
            by_other.push_back({r.other, r.pal, r.bytes});
            by_pal.push_back({r.pal, r.other, r.bytes});
        }
        auto less = [](const T& a, const T& b) { return a.key != b.key ? a.key < b.key : a.value < b.value; };
        std::stable_sort(by_other.begin(), by_other.end(), less);
        std::stable_sort(by_pal.begin(), by_pal.end(), less);
    }
    static uint32_t Find(const std::vector<T>& rows, uint32_t v) {
        auto it = std::upper_bound(rows.begin(), rows.end(), v, [](uint32_t k, const T& t) { return k < t.key; });
        if (it == rows.begin()) return kNone;
        --it;
        if (v >= it->key && uint64_t(v) < uint64_t(it->key) + it->bytes) return it->value + (v - it->key);
        return kNone;
    }
    uint32_t ToPal(uint32_t v) const { return Find(by_other, v); }
    uint32_t ToOther(uint32_t a) const { return Find(by_pal, a); }
    bool CoversPal(uint32_t a) const { return ToOther(a) != kNone; }
};

std::vector<std::pair<uint32_t, uint32_t>> RowWords(const Row& r) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (uint32_t w = (r.pal + 3) & ~3u; uint64_t(w) + 4 <= uint64_t(r.pal) + r.bytes; w += 4) out.emplace_back(w, r.other + (w - r.pal));
    return out;
}

// ---------------------------------------------------------------------------------------------------- 3 objects
std::vector<uint32_t> ObjectStarts(const Elf& elf, const std::vector<uint32_t>& refs, const std::vector<uint32_t>& symbols, uint32_t lo, uint32_t hi) {
    std::set<uint32_t> starts(symbols.begin(), symbols.end());
    for (const char* n : kDataSections) if (const Section* s = elf.Find(n)) starts.insert(s->addr);
    for (const char* n : kBssSections) if (const Section* s = elf.Find(n)) starts.insert(s->addr);
    for (uint32_t v : refs) if (v >= lo && v < hi) starts.insert(v);
    for (const char* n : kPointerSections) {
        const Section* s = elf.Find(n);
        if (!s) continue;
        for (uint64_t w : Words(elf.Read(s->addr, s->size))) {
            if (w >= (1ull << 32) || elf.InCode(w) || w < lo || w >= hi || starts.count(uint32_t(w))) continue;
            uint8_t before = 1;
            if (w % 4 == 0 || (elf.CString(uint32_t(w), nullptr) && (elf.Read(uint32_t(w) - 1, 1, &before), before == 0))) starts.insert(uint32_t(w));
        }
    }
    std::vector<uint32_t> out;
    for (uint32_t a : starts) if (a >= lo && a < hi) out.push_back(a);
    return out;
}

uint32_t Extent(const std::vector<uint32_t>& starts, uint32_t a, uint32_t section_end) {
    const size_t i = UpperBound(starts, a);
    const uint32_t nxt = i < starts.size() ? starts[i] : section_end;
    return std::min(nxt, section_end) - a;
}

struct Bounds { const char* name = nullptr; uint32_t lo = 0, hi = 0; };
template <size_t N>
Bounds SectionBounds(const Elf& elf, uint32_t a, const char* const (&names)[N]) {
    for (const char* n : names) {
        const Section* s = elf.Find(n);
        if (s && a >= s->addr && uint64_t(a) < uint64_t(s->addr) + s->size) return {n, s->addr, s->addr + s->size};
    }
    return {};
}

struct Symbol { std::string name; uint32_t addr; bool func; };

std::vector<Symbol> ParseSymbols(const std::string& text) {   // elf_region_map.py read_symbols: (\w+)\s*=\s*0x([0-9A-Fa-f]+);\s*(?://\s*(.*))?
    std::vector<Symbol> out;
    std::map<std::string, size_t> at;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        size_t k = 0;
        auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        while (k < line.size() && word(line[k])) ++k;
        if (k == 0) continue;
        const std::string name = line.substr(0, k);
        while (k < line.size() && std::isspace(static_cast<unsigned char>(line[k]))) ++k;
        if (k >= line.size() || line[k] != '=') continue;
        ++k;
        while (k < line.size() && std::isspace(static_cast<unsigned char>(line[k]))) ++k;
        if (k + 2 > line.size() || line[k] != '0' || line[k + 1] != 'x') continue;
        k += 2;
        const size_t h = k;
        while (k < line.size() && std::isxdigit(static_cast<unsigned char>(line[k]))) ++k;
        if (k == h || k >= line.size() || line[k] != ';') continue;
        const uint32_t addr = uint32_t(std::strtoull(line.substr(h, k - h).c_str(), nullptr, 16));
        ++k;
        std::string rest;
        size_t c = k;
        while (c < line.size() && std::isspace(static_cast<unsigned char>(line[c]))) ++c;
        if (c + 1 < line.size() && line[c] == '/' && line[c + 1] == '/') rest = line.substr(c + 2);
        const bool func = rest.find("type:func") != std::string::npos;
        auto known = at.find(name);
        if (known != at.end()) out[known->second] = {name, addr, func};   // a later line wins, the first place stays (a dict)
        else { at[name] = out.size(); out.push_back({name, addr, func}); }
    }
    return out;
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>* out, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot read " + path + ": " + std::strerror(errno);
        return false;
    }
    out->clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

// The program a disc's SYSTEM.CNF boots (or the file itself when it is an ELF): its bytes, name and serial.
bool ReadProgram(const std::string& path, std::vector<uint8_t>* bytes, std::string* exe, std::string* serial, std::string* error) {
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            if (error) *error = "cannot read " + path + ": " + std::strerror(errno);
            return false;
        }
        uint8_t h[4] = {};
        const size_t got = std::fread(h, 1, 4, f);
        std::fclose(f);
        if (got == 4 && std::memcmp(h, "\x7f" "ELF", 4) == 0) {
            const size_t slash = path.find_last_of("/\\");
            *exe = path.substr(slash == std::string::npos ? 0 : slash + 1);
            for (char& c : *exe) c = char(std::toupper(static_cast<unsigned char>(c)));
            *serial = SerialOf(*exe);
            return ReadFileBytes(path, bytes, error);
        }
    }
    IsoFs fs;
    if (!fs.Open(path, error)) return false;
    IsoFs::Entry cnf;
    if (!fs.Find("SYSTEM.CNF", cnf)) {
        if (error) *error = path + " has no SYSTEM.CNF";
        return false;
    }
    std::string text(std::min<uint32_t>(cnf.size, 4096), '\0');
    text.resize(fs.ReadBytes(uint64_t(cnf.lba) * IsoFs::kSectorSize, text.size(), reinterpret_cast<uint8_t*>(&text[0])));
    std::string boot;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find_first_of("\r\n", pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        key.erase(std::remove_if(key.begin(), key.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), key.end());
        for (char& c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (key == "BOOT2") boot = line.substr(eq + 1);
    }
    size_t a = boot.find_first_not_of(" \t");
    boot = a == std::string::npos ? std::string() : boot.substr(a);
    const size_t colon = boot.find(':');
    std::string p = colon == std::string::npos ? boot : boot.substr(colon + 1);
    p = p.substr(0, p.find(';'));
    while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
    while (!p.empty() && (p[0] == '\\' || p[0] == '/')) p.erase(0, 1);
    IsoFs::Entry prog;
    if (p.empty() || !fs.Find(p, prog) || prog.directory) {
        if (error) *error = path + " boots " + (p.empty() ? std::string("nothing") : p) + ", which is not on the disc";
        return false;
    }
    const size_t slash = p.find_last_of("/\\");
    *exe = p.substr(slash == std::string::npos ? 0 : slash + 1);
    for (char& c : *exe) c = char(std::toupper(static_cast<unsigned char>(c)));
    *serial = SerialOf(*exe);
    bytes->resize(prog.size);
    if (fs.ReadBytes(uint64_t(prog.lba) * IsoFs::kSectorSize, prog.size, bytes->data()) != prog.size) {
        if (error) *error = "short read of " + p;
        return false;
    }
    return true;
}

}  // namespace

void SetDefaultPalSymbols(const char* symbol_addrs_text) { g_default_symbols = symbol_addrs_text; }
const char* DefaultPalSymbols() { return g_default_symbols; }

std::vector<MatchBlock> MatchingBlocks(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    // b2j minus the popular elements (autojunk: n >= 200, more than n/100 + 1 occurrences); no junk function
    std::unordered_map<uint64_t, std::vector<uint32_t>> b2j;
    b2j.reserve(b.size());
    for (uint32_t j = 0; j < b.size(); ++j) b2j[b[j]].push_back(j);
    if (b.size() >= 200) {
        const size_t ntest = b.size() / 100 + 1;
        for (auto it = b2j.begin(); it != b2j.end();) {
            if (it->second.size() > ntest) it = b2j.erase(it);
            else ++it;
        }
    }
    // j2len as a pair of dense arrays touched only where set (the dict of difflib): the same answers, no hashing per step
    std::vector<uint32_t> len_a(b.size() + 1, 0), len_b(b.size() + 1, 0);
    std::vector<uint32_t> touched_a, touched_b;
    auto longest = [&](uint32_t alo, uint32_t ahi, uint32_t blo, uint32_t bhi) {
        uint32_t besti = alo, bestj = blo, bestsize = 0;
        std::vector<uint32_t>* j2len = &len_a;
        std::vector<uint32_t>* next = &len_b;
        std::vector<uint32_t>* tj = &touched_a;
        std::vector<uint32_t>* tn = &touched_b;
        for (uint32_t i = alo; i < ahi; ++i) {
            auto it = b2j.find(a[i]);
            if (it != b2j.end()) {
                for (uint32_t j : it->second) {
                    if (j < blo) continue;
                    if (j >= bhi) break;
                    const uint32_t k = (j ? (*j2len)[j - 1 + 1] : 0) + 1;   // j2len[j - 1] (index shifted by one: j2len[-1] is 0)
                    (*next)[j + 1] = k;
                    tn->push_back(j + 1);
                    if (k > bestsize) { besti = i - k + 1; bestj = j - k + 1; bestsize = k; }
                }
            }
            for (uint32_t x : *tj) (*j2len)[x] = 0;   // the old row forgotten
            tj->clear();
            std::swap(j2len, next);
            std::swap(tj, tn);
        }
        for (uint32_t x : *tj) (*j2len)[x] = 0;
        tj->clear();
        while (besti > alo && bestj > blo && a[besti - 1] == b[bestj - 1]) { --besti; --bestj; ++bestsize; }
        while (besti + bestsize < ahi && bestj + bestsize < bhi && a[besti + bestsize] == b[bestj + bestsize]) ++bestsize;
        return MatchBlock{besti, bestj, bestsize};
    };
    std::vector<MatchBlock> blocks;
    std::vector<std::array<uint32_t, 4>> queue{{0, uint32_t(a.size()), 0, uint32_t(b.size())}};
    while (!queue.empty()) {
        const auto q = queue.back();
        queue.pop_back();
        const MatchBlock m = longest(q[0], q[1], q[2], q[3]);
        if (!m.n) continue;
        blocks.push_back(m);
        if (q[0] < m.a && q[2] < m.b) queue.push_back({q[0], m.a, q[2], m.b});
        if (m.a + m.n < q[1] && m.b + m.n < q[3]) queue.push_back({m.a + m.n, q[1], m.b + m.n, q[3]});
    }
    std::sort(blocks.begin(), blocks.end(), [](const MatchBlock& x, const MatchBlock& y) {
        return std::tie(x.a, x.b, x.n) < std::tie(y.a, y.b, y.n);
    });
    std::vector<MatchBlock> out;
    MatchBlock cur{0, 0, 0};
    for (const MatchBlock& m : blocks) {
        if (cur.a + cur.n == m.a && cur.b + cur.n == m.b) cur.n += m.n;
        else {
            if (cur.n) out.push_back(cur);
            cur = m;
        }
    }
    if (cur.n) out.push_back(cur);
    out.push_back(MatchBlock{uint32_t(a.size()), uint32_t(b.size()), 0});
    return out;
}

bool MakeRegionMap(const std::vector<uint8_t>& pal_bytes, const std::vector<uint8_t>& other_bytes, const RegionMapMakeOptions& options,
                   std::string* json, RegionMapMakeReport* report, std::string* error) {
    RegionMapMakeReport local;
    RegionMapMakeReport& rep = report ? *report : local;
    rep = RegionMapMakeReport{};
    Elf pal, other;
    std::string why;
    if (!pal.Parse(pal_bytes, &why)) { if (error) *error = "the PAL executable: " + why; return false; }
    if (!other.Parse(other_bytes, &why)) { if (error) *error = "the other executable: " + why; return false; }
    // the PAL symbols (game/symbol_addrs.txt): a file, the text given, or the copy built into the mod tool
    std::string symbols_text = options.symbols_text;
    if (symbols_text.empty() && !options.symbols_path.empty()) {
        std::vector<uint8_t> b;
        if (!ReadFileBytes(options.symbols_path, &b, error)) return false;
        symbols_text.assign(b.begin(), b.end());
    }
    if (symbols_text.empty() && g_default_symbols) symbols_text = g_default_symbols;
    const std::vector<Symbol> symbols = ParseSymbols(symbols_text);
    std::vector<uint32_t> pal_functions, pal_data_syms;
    for (const Symbol& s : symbols) {
        if (s.func) pal_functions.push_back(s.addr);
        else if (s.addr >= kPalImageStart && s.addr < kPalImageEnd) pal_data_syms.push_back(s.addr);
    }
    std::sort(pal_functions.begin(), pal_functions.end());
    pal_functions.erase(std::unique(pal_functions.begin(), pal_functions.end()), pal_functions.end());
    std::sort(pal_data_syms.begin(), pal_data_syms.end());
    pal_data_syms.erase(std::unique(pal_data_syms.begin(), pal_data_syms.end()), pal_data_syms.end());
    const std::set<uint32_t> fset(pal_functions.begin(), pal_functions.end());

    // ---- 1 code
    CodeMap cm;
    cm.Build(pal, other, pal_functions);
    const auto code_tr = [&cm](uint32_t w) { return cm.Function(w); };
    const auto identity = [](uint32_t w) { return w; };
    // ---- the objects of both executables
    const std::vector<uint32_t> pal_starts = ObjectStarts(pal, cm.pal_refs, pal_data_syms, kPalImageStart, kPalImageEnd);
    const std::vector<uint32_t> other_starts0 = ObjectStarts(other, cm.other_refs, {}, other.image_lo, other.hi);
    // ---- 2 data: each section by content
    std::vector<Row> content;
    J sections = J::A();
    for (const char* name : kDataSections) {
        const Section* ps = pal.Find(name);
        const Section* os = other.Find(name);
        if (!ps || !os || ps->type == kNoBits || os->type == kNoBits) {
            sections.Push(J::O().Add("section", J::S(name)).Add("pal_bytes", J::I(ps ? ps->size : 0)).Add("other_bytes", J::I(os ? os->size : 0))
                              .Add("aligned_bytes", J::I(0)).Add("why", J::S("not in both executables")));
            continue;
        }
        const std::vector<Row> rows = AlignBlocks(pal, other, ps->addr, ps->size, os->addr, os->size, code_tr, name, &pal_starts, &other_starts0);
        uint64_t aligned = 0;
        for (const Row& r : rows) aligned += r.bytes;
        content.insert(content.end(), rows.begin(), rows.end());
        sections.Push(J::O().Add("section", J::S(name)).Add("pal", J::S(Hex(ps->addr))).Add("pal_bytes", J::I(ps->size)).Add("other", J::S(Hex(os->addr)))
                          .Add("other_bytes", J::I(os->size)).Add("aligned_bytes", J::I(int64_t(aligned))));
    }
    std::stable_sort(content.begin(), content.end(), [](const Row& x, const Row& y) { return x.pal < y.pal; });
    const Ranges cr(content);
    std::vector<uint32_t> content_starts;
    for (const Row& r : content) content_starts.push_back(r.pal);
    auto content_run = [&](uint32_t a) -> const Row* {
        const size_t k = UpperBound(content_starts, a);
        if (k == 0) return nullptr;
        const Row& r = content[k - 1];
        return a >= r.pal && uint64_t(a) < uint64_t(r.pal) + r.bytes ? &r : nullptr;
    };
    Votes ptr_votes;
    for (const Row& r : content)
        for (const auto& wv : RowWords(r)) {
            const uint32_t pw = pal.Word(wv.first), ow = other.Word(wv.second);
            if (!(pal.PointerLike(pw) && !pal.InCode(pw) && other.PointerLike(ow) && !other.InCode(ow))) continue;
            std::string ps, os;
            const bool hp = pal.CString(pw, &ps), ho = other.CString(ow, &os);
            bool same;
            if (hp || ho) same = hp && ho && ps == os;
            else same = pw % 4 == 0 && ow % 4 == 0 && DataTokens(pal, pw, 16, identity) == DataTokens(other, ow, 16, code_tr);
            if (same) ++ptr_votes[pw][ow];
        }
    // ---- 3 every PAL data object's counterpart
    auto strength = [](const std::string& k) {
        if (k == "content") return 7;
        if (k == "code") return 6;
        if (k == "call") return 5;
        if (k == "pointers") return 4;
        if (k == "content (part)") return 3;
        if (k == "code gap") return 2;
        if (k == "elsewhere") return 1;
        return 0;
    };
    struct Obj { uint32_t a, n; const char* sec; };
    struct ObInfo { std::string section; uint32_t bytes = 0; uint32_t other = kNone; std::string evidence = "none"; uint32_t other_bytes = 0; bool has_other_bytes = false; };
    std::vector<Obj> objs;
    for (uint32_t a : pal_starts) {
        const Bounds b = SectionBounds(pal, a, kDataSections);
        if (b.name) objs.push_back({a, Extent(pal_starts, a, b.hi), b.name});
    }
    std::map<uint32_t, ObInfo> objects;
    struct Decided { int strength; uint32_t a, o; std::string kind; };
    std::vector<Decided> decided;
    for (const Obj& ob : objs) {
        std::vector<std::pair<std::string, uint32_t>> cands;
        const uint32_t o_c = cr.ToOther(ob.a);
        if (o_c != kNone) {
            const Row* run = content_run(ob.a);
            const bool strong = run && run->kind == "equal" && uint64_t(run->pal) + run->bytes >= uint64_t(ob.a) + ob.n;
            cands.emplace_back(strong ? "content" : "content (part)", o_c);
        }
        for (uint32_t k = 0; k < 3; ++k) {
            const auto v = cm.Vote(ob.a, 1u << k);
            if (std::get<0>(v) != kNone) cands.emplace_back(std::get<2>(v), std::get<0>(v));
        }
        auto pv = ptr_votes.find(ob.a);
        if (pv != ptr_votes.end() && !pv->second.empty()) {
            uint32_t best = 0, count = 0;
            for (const auto& v : pv->second) if (v.second > count) { best = v.first; count = v.second; }
            cands.emplace_back("pointers", best);
        }
        std::stable_sort(cands.begin(), cands.end(), [&](const auto& x, const auto& y) { return strength(x.first) > strength(y.first); });
        ObInfo info;
        info.section = ob.sec;
        info.bytes = ob.n;
        objects[ob.a] = info;
        if (!cands.empty()) decided.push_back({strength(cands[0].first), ob.a, cands[0].second, cands[0].first});
    }
    struct Claim { uint32_t lo, hi, a; };
    std::vector<Claim> claimed;   // sorted by (lo, hi, a)
    auto claim = [&](uint32_t o, uint32_t on, uint32_t a) -> uint32_t {
        const size_t k = size_t(std::upper_bound(claimed.begin(), claimed.end(), o, [](uint32_t v, const Claim& c) { return v < c.lo; }) - claimed.begin());
        for (size_t x = k ? k - 1 : 0; x < claimed.size() && x <= k; ++x)
            if (claimed[x].lo < uint64_t(o) + on && o < claimed[x].hi) return claimed[x].a;
        const Claim c{o, o + on, a};
        claimed.insert(std::upper_bound(claimed.begin(), claimed.end(), c, [](const Claim& x, const Claim& y) {
                           return std::tie(x.lo, x.hi, x.a) < std::tie(y.lo, y.hi, y.a);
                       }), c);
        return kNone;
    };
    std::stable_sort(decided.begin(), decided.end(), [](const Decided& x, const Decided& y) {
        return x.strength != y.strength ? x.strength > y.strength : x.a < y.a;
    });
    for (const Decided& d : decided) {
        ObInfo& ob = objects[d.a];
        const Bounds ob_ = SectionBounds(other, d.o, kDataSections);
        if (!ob_.name) {
            ob.evidence = d.kind + " (into " + Hex(d.o) + ": not loaded)";
            continue;
        }
        const uint32_t on = std::max<uint32_t>(1, std::min(ob.bytes, Extent(other_starts0, d.o, ob_.hi)));
        const uint32_t winner = claim(d.o, on, d.a);
        if (winner != kNone) {
            ob.evidence = d.kind + " " + Hex(d.o) + " lost to " + Hex(winner);
            continue;
        }
        ob.other = d.o;
        ob.evidence = d.kind;
    }
    // the same tokens elsewhere, then the neighbours' displacement
    std::vector<std::pair<uint32_t, std::vector<uint64_t>>> other_tokens;
    for (const char* name : kPointerSections)
        if (const Section* s = other.Find(name)) other_tokens.emplace_back(s->addr, DataTokens(other, s->addr, s->size, code_tr));
    for (const Obj& o : objs) {
        ObInfo& ob = objects[o.a];
        if (ob.other != kNone || o.n < 8 || o.a % 4) continue;
        const std::vector<uint64_t> pt = DataTokens(pal, o.a, std::min<uint32_t>(o.n, 64), identity);
        bool skip = true;
        for (size_t k = 0; k < pt.size() && k < 2; ++k) skip = skip && (pt[k] == 0 || pt[k] == kPointerToken);
        if (skip) continue;
        size_t best_len = 0;
        std::vector<uint32_t> hits;
        for (const auto& sec : other_tokens) {
            const std::vector<uint64_t>& ot = sec.second;
            for (size_t k = 0; k < ot.size(); ++k) {
                if (ot[k] != pt[0]) continue;
                size_t m = 0;
                while (m < pt.size() && k + m < ot.size() && ot[k + m] == pt[m]) ++m;
                if (m < 2) continue;
                if (m > best_len) { best_len = m; hits.assign(1, sec.first + uint32_t(4 * k)); }
                else if (m == best_len) hits.push_back(sec.first + uint32_t(4 * k));
            }
        }
        if (hits.size() == 1 && claim(hits[0], uint32_t(4 * best_len), o.a) == kNone) {
            ob.other = hits[0];
            ob.evidence = "elsewhere";
        }
    }
    for (size_t k = 0; k < objs.size(); ++k) {
        ObInfo& ob = objects[objs[k].a];
        if (ob.other != kNone || k == 0 || k + 1 >= objs.size()) continue;
        const uint32_t pa = objs[k - 1].a, pn = objs[k + 1].a;
        const uint32_t po = objects[pa].other, no = objects[pn].other;
        if (po == kNone || no == kNone || int64_t(po) - pa != int64_t(no) - pn || std::strcmp(objs[k - 1].sec, objs[k].sec) != 0 ||
            std::strcmp(objs[k + 1].sec, objs[k].sec) != 0)
            continue;
        const uint32_t o = uint32_t(int64_t(objs[k].a) + (int64_t(po) - pa));
        if (SectionBounds(other, o, kDataSections).name && claim(o, objs[k].n, objs[k].a) == kNone) {
            ob.other = o;
            ob.evidence = "neighbours";
        }
    }
    std::set<uint32_t> cps;
    for (const auto& kv : objects) if (kv.second.other != kNone) cps.insert(kv.second.other);
    std::set<uint32_t> os_set(other_starts0.begin(), other_starts0.end());
    os_set.insert(cps.begin(), cps.end());
    const std::vector<uint32_t> other_starts(os_set.begin(), os_set.end()), counterpart_starts(cps.begin(), cps.end());
    // ---- the layout, object by object
    std::vector<Row> rows_out;
    uint32_t moved = 0;
    for (const Obj& o : objs) {
        ObInfo& ob = objects[o.a];
        if (ob.other == kNone) continue;
        const Bounds b = SectionBounds(other, ob.other, kDataSections);
        const uint32_t on = Extent(counterpart_starts, ob.other, b.hi);
        ob.other_bytes = on;
        ob.has_other_bytes = true;
        if (ob.evidence == "content") {
            rows_out.push_back({o.sec, o.a, ob.other, o.n, "content equal"});
            continue;
        }
        const std::vector<uint32_t> one_p{o.a}, one_o{ob.other};
        std::vector<Row> rows = AlignBlocks(pal, other, o.a, o.n, ob.other, on, code_tr, o.sec, &one_p, &one_o);
        if (rows.empty()) rows.push_back({o.sec, o.a, ob.other, std::min(o.n, on), "start"});
        if (ob.evidence != "content (part)") ++moved;
        for (Row& r : rows) r.kind = ob.evidence + " " + r.kind;
        rows_out.insert(rows_out.end(), rows.begin(), rows.end());
    }
    uint64_t moved_bytes = 0, content_bytes = 0;
    for (const Row& r : rows_out) (r.kind.rfind("content", 0) == 0 ? content_bytes : moved_bytes) += r.bytes;
    std::vector<Row> sorted_rows = rows_out;
    std::stable_sort(sorted_rows.begin(), sorted_rows.end(), [](const Row& x, const Row& y) { return x.pal < y.pal; });
    std::vector<Row> segments = MergeRanges(sorted_rows);
    // the file names the two programs spell differently
    J names_json = J::A();
    uint32_t names_count = 0;
    for (const Obj& o : objs) {
        const uint32_t other_a = objects[o.a].other;
        if (other_a == kNone) continue;
        std::string ps, os;
        if (pal.CString(o.a, &ps) && other.CString(other_a, &os) && ps != os && ps.find('\\') != std::string::npos && os.find('\\') != std::string::npos) {
            names_json.Push(J::O().Add("pal", J::S(ps)).Add("other", J::S(os)).Add("at", J::S(Hex(o.a))));
            ++names_count;
        }
    }
    // ---- 4 bss
    std::vector<Row> bss;
    std::map<std::string, int64_t> bss_check;
    for (const char* name : kBssSections) {
        const Section* ps = pal.Find(name);
        const Section* os = other.Find(name);
        if (!ps || !os) continue;
        const int64_t d = int64_t(os->addr) - ps->addr;
        bss.push_back({name, ps->addr, uint32_t(int64_t(ps->addr) + d), std::min(ps->size, os->size), "section start"});
        for (const auto& kv : cm.votes) {
            if (kv.first < ps->addr || uint64_t(kv.first) >= uint64_t(ps->addr) + ps->size) continue;
            const uint32_t o = std::get<0>(cm.Vote(kv.first));
            ++bss_check[int64_t(o) - kv.first == d ? "agree" : "disagree"];
        }
    }
    // ---- 5 pointers, over the laid-out image
    std::vector<Row> placed;
    auto covered = [&](uint32_t p) {
        if (Ranges(segments).CoversPal(p)) return true;
        for (const Row& r : placed) if (p >= r.pal && uint64_t(p) < uint64_t(r.pal) + r.bytes) return true;
        return false;
    };
    const Ranges seg_ranges(segments);   // the segments do not change during the pass (the placements are their own rows)
    auto covered_fast = [&](uint32_t p) {
        if (seg_ranges.CoversPal(p)) return true;
        for (const Row& r : placed) if (p >= r.pal && uint64_t(p) < uint64_t(r.pal) + r.bytes) return true;
        return false;
    };
    (void)covered;
    auto pal_only_run = [&](uint32_t p, uint32_t* lo_out, uint32_t* hi_out) {
        const Bounds b = SectionBounds(pal, p, kDataSections);
        if (!b.name || covered_fast(p)) return false;
        uint32_t lo = p, hi = p;
        while (lo > b.lo && !covered_fast(lo - 1)) --lo;
        while (hi < b.hi && !covered_fast(hi)) ++hi;
        *lo_out = lo;
        *hi_out = hi;
        return true;
    };
    auto with_bss = [&](const std::vector<Row>& extra) {
        std::vector<Row> all = segments;
        all.insert(all.end(), extra.begin(), extra.end());
        all.insert(all.end(), bss.begin(), bss.end());
        return Ranges(all);
    };
    Ranges ranges = with_bss({});
    std::vector<uint32_t> pointers;
    std::map<uint32_t, uint32_t> code_pairs, fills;
    Votes data_exc_votes;
    std::map<std::string, int64_t> stats;
    J no_pal_function = J::A(), unmapped_data = J::A(), other_function = J::A();
    std::vector<Row> todo = segments;
    for (size_t ti = 0; ti < todo.size(); ++ti) {
        const Row r = todo[ti];
        for (const auto& wv : RowWords(r)) {
            const uint32_t w = wv.first, ow = other.Word(wv.second), pw = pal.Word(w);
            uint32_t t = kNone;
            if (other.InCode(ow)) {
                t = code_tr(ow);
                if (t != kNone) code_pairs[ow] = t;
            }
            if (ow == pw || !(other.PointerLike(ow) || ow == other.hi)) continue;
            if (other.InCode(ow)) {
                if (t != kNone && t != pw && fset.count(pw) && ((pw <= t && t < NextFunction(cm.functions, pw)) || cm.Twins(pw, t))) {
                    fills[w] = pw;
                    ++stats["code_same_function"];
                } else if (t != kNone) {
                    pointers.push_back(w);
                    ++stats["code"];
                    if (pal.InCode(pw) && t != pw) {
                        ++stats["code_other_function"];
                        other_function.Push(J::A().Push(J::S(Hex(w))).Push(J::S(Hex(ow))).Push(J::S(Hex(t))).Push(J::S(Hex(pw))));
                    }
                } else if (pal.InCode(pw)) {
                    fills[w] = pw;
                    ++stats["code_no_pal_function_pal_slot"];
                    no_pal_function.Push(J::A().Push(J::S(Hex(w))).Push(J::S(Hex(ow))).Push(J::S(Hex(pw))));
                } else {
                    ++stats["code_no_pal_function"];
                    no_pal_function.Push(J::A().Push(J::S(Hex(w))).Push(J::S(Hex(ow))).Push(J::S("")));
                }
                continue;
            }
            t = ow == other.hi ? pal.hi : ranges.ToPal(ow);
            if (t != kNone) {
                pointers.push_back(w);
                ++stats["data"];
                const bool exact = r.kind.size() >= 5 && (r.kind.compare(r.kind.size() - 5, 5, "equal") == 0 ||
                                                          r.kind.compare(r.kind.size() - 5, 5, "mixed") == 0);
                if (exact && pal.PointerLike(pw) && t != pw) ++data_exc_votes[ow][pw];
                continue;
            }
            const Bounds ob = SectionBounds(other, ow, kDataSections);
            if (ob.name && pal.PointerLike(pw) && !pal.InCode(pw)) {
                uint32_t lo = 0, hi = 0;
                const bool run = pal_only_run(pw, &lo, &hi);
                const uint32_t on = Extent(other_starts, ow, ob.hi);
                if (run && uint64_t(pw) + on <= hi) {
                    const Bounds pb = SectionBounds(pal, pw, kDataSections);
                    const Row row{pb.name ? pb.name : "", pw, ow, on, "placed"};
                    placed.push_back(row);
                    todo.push_back(row);
                    ranges = with_bss(placed);
                    pointers.push_back(w);
                    ++stats["data_placed"];
                    continue;
                }
            }
            ++stats["data_unmapped"];
            unmapped_data.Push(J::A().Push(J::S(Hex(w))).Push(J::S(Hex(ow))).Push(J::S(pal.PointerLike(pw) ? Hex(pw) : "")));
        }
    }
    std::map<uint32_t, uint32_t> data_exceptions;
    for (const auto& kv : data_exc_votes) {
        uint32_t best = 0, count = 0;
        for (const auto& v : kv.second) if (v.second > count) { best = v.first; count = v.second; }
        data_exceptions[kv.first] = best;
    }
    if (other.hi != pal.hi) data_exceptions[other.hi] = pal.hi;
    {
        std::vector<Row> all = segments;
        all.insert(all.end(), placed.begin(), placed.end());
        std::stable_sort(all.begin(), all.end(), [](const Row& x, const Row& y) { return x.pal < y.pal; });
        segments = MergeRanges(all);
    }
    // ---- 6 fills: a PAL object no byte of the other executable reaches keeps its PAL code pointers
    const Ranges final_r(segments);
    uint32_t pal_only_objects = 0;
    for (uint32_t a : pal_starts) {
        const Bounds b = SectionBounds(pal, a, kDataSections);
        if (!b.name) continue;
        const uint32_t n = Extent(pal_starts, a, b.hi);
        bool any = false;
        for (uint32_t x = a; x < a + n && !any; ++x) any = final_r.CoversPal(x);
        if (any) continue;
        ++pal_only_objects;
        for (uint32_t w = (a + 3) & ~3u; int64_t(w) < int64_t(a) + n - 3; w += 4) {
            if (w < kPatchStart || w >= kPatchEnd) continue;
            const uint32_t pw = pal.Word(w);
            if (pal.InCode(pw)) {
                fills[w] = pw;
                ++stats["fill_pal_only_object"];
            }
        }
    }
    uint32_t other_overlaps = 0;
    {
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> srt;
        for (const Row& r : segments) srt.emplace_back(r.other, r.other + r.bytes, r.pal);
        std::sort(srt.begin(), srt.end());
        for (size_t k = 1; k < srt.size(); ++k) if (std::get<0>(srt[k]) < std::get<1>(srt[k - 1])) ++other_overlaps;
    }
    // ---- the census counts: every PAL data symbol
    std::map<uint32_t, std::vector<std::string>> names_at;   // address -> names in file order
    for (const Symbol& s : symbols)
        if (!s.func && s.addr >= kPalImageStart && s.addr < kPalImageEnd) names_at[s.addr].push_back(s.name);
    std::map<std::string, int64_t> ev_count, size_count;
    uint32_t sym_total = 0, sym_mapped = 0;
    const Ranges bss_r(bss);
    for (const auto& kv : names_at) {
        const uint32_t a = kv.first;
        std::string ev, size;
        uint32_t o = kNone;
        const Bounds b = SectionBounds(pal, a, kDataSections);
        if (!b.name) {
            const Bounds bb = SectionBounds(pal, a, kBssSections);
            o = bb.name ? bss_r.ToOther(a) : kNone;
            const uint32_t o_v = std::get<0>(cm.Vote(a));
            if (o != kNone) ev = o_v == o ? "bss+code" : "bss";   // " (the code says ...)" starts with "bss" too
            else ev = "none";
            size = "bss";
        } else {
            const auto it = objects.find(a);
            const std::string evidence = it != objects.end() ? it->second.evidence : "none";
            const uint32_t ob_other = it != objects.end() ? it->second.other : kNone;
            const uint32_t pn = Extent(pal_starts, a, b.hi);
            uint32_t cov = 0;
            for (uint32_t x = a; x < a + pn; ++x) cov += final_r.CoversPal(x) ? 1 : 0;
            o = final_r.ToOther(a);
            uint32_t on = 0;
            if (o != kNone) {
                const Bounds obb = SectionBounds(other, o, kDataSections);
                on = obb.name ? Extent(other_starts, o, obb.hi) : 0;
            }
            ev = evidence;
            if (ob_other == kNone && o != kNone) ev = "placed";
            if (o == kNone) size = "unmatched";
            else if (cov < pn) size = "other smaller";
            else if (on > pn) size = "other larger";
            else size = "same";
        }
        for (size_t k = 0; k < kv.second.size(); ++k) {
            ++sym_total;
            if (o != kNone) ++sym_mapped;
            ++ev_count[ev.substr(0, ev.find(' '))];
            ++size_count[size];
        }
    }
    // ---- the composed image against the PAL one
    std::vector<uint8_t> composed(kPalImageEnd, 0), native(kPalImageEnd, 0);
    {
        RegionMap m;
        m.elf_sha256 = Sha256OfBytes(other_bytes);
        m.image_start = kPalImageStart;
        m.bss_start = kPalBssStart;
        m.image_end = kPalImageEnd;
        for (const Row& r : segments) m.segments.push_back({r.pal, r.other, r.bytes});
        for (const Row& r : bss) m.bss.push_back({r.pal, r.other, r.bytes});
        std::set<uint32_t> ps(pointers.begin(), pointers.end());
        m.pointers.assign(ps.begin(), ps.end());
        for (const auto& kv : code_pairs) m.code.emplace_back(kv.first, kv.second);
        for (const auto& kv : data_exceptions) m.data.emplace_back(kv.first, kv.second);
        for (const auto& kv : fills) m.fills.emplace_back(kv.first, kv.second);
        std::string e;
        if (!LoadImageThroughMap(other_bytes, m, composed.data(), composed.size(), nullptr, &e) ||
            !LoadImageNative(pal_bytes, kPalImageStart, kPalBssStart, kPalImageEnd, native.data(), native.size(), &e)) {
            if (error) *error = "the map does not lay the image out: " + e;
            return false;
        }
    }
    std::vector<std::pair<uint32_t, uint32_t>> diffs;
    {
        const uint32_t N = kPalBssStart - kPalImageStart;
        const uint8_t* c = composed.data() + kPalImageStart;
        const uint8_t* n = native.data() + kPalImageStart;
        uint32_t i = 0;
        while (i < N) {
            if (c[i] != n[i]) {
                uint32_t j = i;
                while (j < N && (c[j] != n[j] || (j + 1 < N && j + 1 - i < 4 && c[j + 1] != n[j + 1]))) ++j;
                diffs.emplace_back(kPalImageStart + i, j - i);
                i = j;
            } else {
                ++i;
            }
        }
    }
    J diff_rows = J::A();
    uint64_t differing = 0;
    for (const auto& d : diffs) {
        uint32_t cov = 0;
        for (uint32_t x = d.first; x < d.first + d.second; ++x) cov += final_r.CoversPal(x) ? 1 : 0;
        // the PAL object the bytes lie in: its symbol_addrs.txt name, else D_<address>, + the offset
        const size_t k = UpperBound(pal_starts, d.first);
        std::string at;
        if (k) {
            const uint32_t o = pal_starts[k - 1];
            const auto named = names_at.find(o);
            char buf[48];
            std::snprintf(buf, sizeof buf, "D_%08X", o);
            char off[24];
            std::snprintf(off, sizeof off, "+0x%X", d.first - o);
            at = (named != names_at.end() ? named->second.front() : std::string(buf)) + off;
        } else {
            at = Hex(d.first);
        }
        diff_rows.Push(J::O().Add("pal", J::S(Hex(d.first))).Add("bytes", J::I(d.second)).Add("at", J::S(at))
                           .Add("kind", J::S(cov == d.second ? "the other executable's bytes" : (cov == 0 ? "zero (no counterpart)" : "mixed"))));
        differing += d.second;
    }
    // ---- the counts
    std::map<std::string, int64_t> obj_ev;
    for (const auto& kv : objects) ++obj_ev[kv.second.other != kNone ? kv.second.evidence.substr(0, kv.second.evidence.find(' ')) : "none"];
    auto counter = [](const std::map<std::string, int64_t>& m) {
        J o = J::O();
        for (const auto& kv : m) o.Add(kv.first, J::I(kv.second));
        return o;
    };
    auto stat = [&](const char* k) { auto it = stats.find(k); return it == stats.end() ? int64_t(0) : it->second; };
    uint64_t aligned_bytes = 0, placed_bytes = 0;
    for (const Row& r : segments) aligned_bytes += r.bytes;
    for (const Row& r : placed) placed_bytes += r.bytes;
    uint32_t at_starts = 0;
    for (const auto& kv : code_pairs) at_starts += fset.count(kv.second) ? 1 : 0;
    J coverage = J::O();
    coverage.Add("data_image_bytes", J::I(kPalBssStart - kPalImageStart))
        .Add("aligned_bytes", J::I(int64_t(aligned_bytes)))
        .Add("content_bytes", J::I(int64_t(content_bytes)))
        .Add("moved_bytes", J::I(int64_t(moved_bytes)))
        .Add("placed_bytes", J::I(int64_t(placed_bytes)))
        .Add("pointer_words", J::I(int64_t(pointers.size())))
        .Add("code_pairs", J::I(int64_t(code_pairs.size())))
        .Add("code_pairs_at_function_starts", J::I(at_starts))
        .Add("code_conflicts", J::I(0))
        .Add("code_instructions", J::A().Push(J::I(int64_t(cm.pw.size()))).Push(J::I(int64_t(cm.ow.size()))))
        .Add("code_aligned", J::O().Add("equal", J::I(int64_t(cm.equal_pairs))).Add("replace", J::I(int64_t(cm.replace_pairs))))
        .Add("code_pointer_words", J::I(stat("code")))
        .Add("code_pointers_to_another_function", J::I(stat("code_other_function")))
        .Add("code_pointers_snapped_to_the_slot_function", J::I(stat("code_same_function")))
        .Add("code_pointers_no_pal_function", J::I(stat("code_no_pal_function") + stat("code_no_pal_function_pal_slot")))
        .Add("code_pointers_no_pal_function_pal_slot", J::I(stat("code_no_pal_function_pal_slot")))
        .Add("data_pointer_words", J::I(stat("data") + stat("data_placed")))
        .Add("data_pointers_placed", J::I(stat("data_placed")))
        .Add("data_pointers_unmapped", J::I(stat("data_unmapped")))
        .Add("data_exceptions", J::I(int64_t(data_exceptions.size())))
        .Add("fills", J::I(int64_t(fills.size())))
        .Add("fills_pal_only_objects", J::I(stat("fill_pal_only_object")))
        .Add("names", J::I(names_count))
        .Add("objects", J::I(int64_t(objects.size())))
        .Add("objects_moved", J::I(moved))
        .Add("objects_by_evidence", counter(obj_ev))
        .Add("objects_pal_only", J::I(pal_only_objects))
        .Add("other_overlaps", J::I(other_overlaps))
        .Add("bss_votes", counter(bss_check))
        .Add("pal_data_symbols", J::I(sym_total))
        .Add("pal_data_symbols_mapped", J::I(sym_mapped))
        .Add("symbols_by_evidence", counter(ev_count))
        .Add("symbols_by_size", counter(size_count))
        .Add("differing_bytes", J::I(int64_t(differing)));
    // ---- the document (the keys and their order: elf_region_map.py make_map)
    auto row_json = [](const Row& r) {
        return J::O().Add("section", J::S(r.section)).Add("pal", J::S(Hex(r.pal))).Add("other", J::S(Hex(r.other))).Add("bytes", J::I(r.bytes))
            .Add("kind", J::S(r.kind));
    };
    J segs = J::A(), bss_json = J::A(), ptrs = J::A(), code = J::A(), data = J::A(), fills_json = J::A();
    for (const Row& r : segments) segs.Push(row_json(r));
    for (const Row& r : bss) bss_json.Push(row_json(r));
    for (uint32_t p : std::set<uint32_t>(pointers.begin(), pointers.end())) ptrs.Push(J::S(Hex(p)));
    for (const auto& kv : code_pairs) code.Push(Pair(kv.first, kv.second));
    for (const auto& kv : data_exceptions) data.Push(Pair(kv.first, kv.second));
    for (const auto& kv : fills) fills_json.Push(Pair(kv.first, kv.second));
    J doc = J::O();
    doc.Add("schema", J::I(2))
        .Add("kind", J::S("region-map"))
        .Add("serial", J::S(options.serial))
        .Add("executable", J::S(options.executable))
        .Add("elf", J::O().Add("sha256", J::S(Sha256OfBytes(other_bytes))).Add("bytes", J::I(int64_t(other_bytes.size()))).Add("entry", J::S(Hex(other.entry)))
                        .Add("load", Pair(other.lo, other.hi)).Add("text", Pair(other.code_lo, other.code_hi)))
        .Add("pal_elf", J::O().Add("sha256", J::S(Sha256OfBytes(pal_bytes))).Add("bytes", J::I(int64_t(pal_bytes.size()))).Add("entry", J::S(Hex(pal.entry))))
        .Add("image", J::O().Add("pal_start", J::S(Hex(kPalImageStart))).Add("pal_bss_start", J::S(Hex(kPalBssStart))).Add("pal_end", J::S(Hex(kPalImageEnd))))
        .Add("made_by", J::S(kRegionMapMadeBy))
        .Add("sections", sections)
        .Add("segments", segs)
        .Add("bss", bss_json)
        .Add("pointers", ptrs)
        .Add("code", code)
        .Add("data", data)
        .Add("fills", fills_json)
        .Add("names", names_json)
        .Add("coverage", coverage)
        .Add("differences", diff_rows)
        .Add("code_pointers_to_another_function", other_function)
        .Add("code_without_pal_function", no_pal_function)
        .Add("data_pointers_unmapped", unmapped_data);
    std::string text;
    Dump(doc, 0, text);
    text += "\n";
    rep.segments = uint32_t(segments.size());
    rep.bss = uint32_t(bss.size());
    rep.pointer_words = uint32_t(pointers.size());
    rep.code_pairs = uint32_t(code_pairs.size());
    rep.code_conflicts = 0;
    rep.data_pairs = uint32_t(stat("data") + stat("data_placed"));
    rep.data_consistent = rep.data_pairs;
    rep.data_exceptions = uint32_t(data_exceptions.size());
    rep.symbols = sym_total;
    rep.symbols_mapped = sym_mapped;
    rep.aligned_bytes = aligned_bytes;
    rep.data_image_bytes = kPalBssStart - kPalImageStart;
    rep.fills = uint32_t(fills.size());
    rep.differing_bytes = differing;
    rep.code_no_pal_function = uint32_t(stat("code_no_pal_function") + stat("code_no_pal_function_pal_slot"));
    rep.data_unmapped = uint32_t(stat("data_unmapped"));
    rep.names = names_count;
    if (json) *json = std::move(text);
    return true;
}

int RegionMapCommand(int argc, char** argv) {
    std::string pal_path, other_path, out, serial, symbols;
    bool as_json = false;
    for (int i = 1; i < argc; ++i) {   // argv[0] = "region-map"
        const std::string a = argv[i];
        if (a == "--pal" && i + 1 < argc) pal_path = argv[++i];
        else if (a == "--disc" && i + 1 < argc) other_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--serial" && i + 1 < argc) serial = argv[++i];
        else if (a == "--symbols" && i + 1 < argc) symbols = argv[++i];
        else if (a == "--json") as_json = true;
        else {
            std::fprintf(stderr, "usage: twinsanityevolved-modtool region-map --pal <the European disc or SLES_525.68> --disc <the other disc or its "
                                 "executable> [--out region_map_<serial>.json] [--serial S] [--symbols symbol_addrs.txt] [--json]\n");
            return 2;
        }
    }
    if (pal_path.empty() || other_path.empty()) {
        std::fprintf(stderr, "twinsanityevolved-modtool region-map: --pal and --disc are both needed\n");
        return 2;
    }
    if (symbols.empty() && !g_default_symbols) {
        std::fprintf(stderr, "twinsanityevolved-modtool region-map: no symbol list (--symbols game/symbol_addrs.txt; this build has none built in)\n");
        return 2;
    }
    std::vector<uint8_t> pal, other;
    std::string pal_exe, pal_serial, exe, other_serial, error;
    if (!ReadProgram(pal_path, &pal, &pal_exe, &pal_serial, &error) || !ReadProgram(other_path, &other, &exe, &other_serial, &error)) {
        std::fprintf(stderr, "twinsanityevolved-modtool region-map: %s\n", error.c_str());
        return 1;
    }
    const RegionEntry* pal_row = RegionEntryFor("SLES-52568");
    if (Sha256OfBytes(pal) != pal_row->elf_sha256)
        std::fprintf(stderr, "twinsanityevolved-modtool region-map: note: %s is not the retail PAL executable the port was made from\n", pal_exe.c_str());
    RegionMapMakeOptions options;
    options.serial = serial.empty() ? other_serial : serial;
    options.executable = exe;
    options.symbols_path = symbols;
    std::string json;
    RegionMapMakeReport r;
    if (!MakeRegionMap(pal, other, options, &json, &r, &error)) {
        std::fprintf(stderr, "twinsanityevolved-modtool region-map: %s\n", error.c_str());
        return 1;
    }
    if (out.empty()) out = "region_map_" + (options.serial.empty() ? std::string("unknown") : options.serial) + ".json";
    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f || std::fwrite(json.data(), 1, json.size(), f) != json.size()) {
        if (f) std::fclose(f);
        std::fprintf(stderr, "twinsanityevolved-modtool region-map: cannot write %s\n", out.c_str());
        return 1;
    }
    std::fclose(f);
    if (as_json) {
        std::printf("{\"ok\": true, \"map\": \"%s\", \"serial\": \"%s\", \"segments\": %u, \"aligned_bytes\": %llu, \"pointer_words\": %u, \"code_pairs\": %u, "
                    "\"code_without_pal_function\": %u, \"data_unmapped\": %u, \"fills\": %u, \"names\": %u, \"symbols\": %u, \"symbols_mapped\": %u, "
                    "\"differing_bytes\": %llu}\n",
                    out.c_str(), options.serial.c_str(), r.segments, (unsigned long long)r.aligned_bytes, r.pointer_words, r.code_pairs,
                    r.code_no_pal_function, r.data_unmapped, r.fills, r.names, r.symbols, r.symbols_mapped, (unsigned long long)r.differing_bytes);
    } else {
        std::printf("%s (%s) -> PAL: %u segments (%llu of %llu data-image bytes), %u bss ranges, %u pointer words (%u code pointers with no PAL "
                    "function, %u data pointers unmapped), %u fills, %u file names, PAL data symbols mapped %u of %u, %llu bytes differ from the "
                    "PAL image -- written %s\n",
                    exe.c_str(), options.serial.c_str(), r.segments, (unsigned long long)r.aligned_bytes, (unsigned long long)r.data_image_bytes, r.bss,
                    r.pointer_words, r.code_no_pal_function, r.data_unmapped, r.fills, r.names, r.symbols_mapped, r.symbols,
                    (unsigned long long)r.differing_bytes, out.c_str());
    }
    return 0;
}

}  // namespace ps2r::app
