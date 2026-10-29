// test_mod_import -- engine/app/host/mod_import (te::modimport) over a GENERATED disc (te_test_iso: byte patterns, no game
// data): its inflate against zlib's own streams and a long fixed-code stream; SHA-1 / CRC-32 / Adler-32 vectors; a Mod Crate
// (folder and zip) whose layer1 grows and adds archive entries (the rebuilt index and the archive recipe composed back
// through the game's overlay, entry by entry); the crate refusals; a modified image (an archive entry changed in place, a
// large file's few bytes as a recipe, an added file, a removed one, a modified executable: its code changes named by
// function, its data changes counted, a pointer into its own code given back the disc's value); the same image as xdelta
// (VCDIFF), PPF and BPS patches (written here) importing to the same files; a patch for another disc refused; a folder of
// loose files matched by name (an ambiguous one left out); the base check (a recipe over another base refused by the
// overlay, three ways); the sampled hash's definition; a cancelled import leaving nothing behind; re-import ids.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "app/host/mod_import/archive.h"
#include "app/host/mod_import/elf_diff.h"
#include "app/host/mod_import/hashes.h"
#include "app/host/mod_import/inflate.h"
#include "app/host/mod_import/json.h"
#include "app/host/mod_import/mod_import.h"
#include "app/host/mod_import/patches.h"
#include "app/host/mod_import/zip_reader.h"
#include "engine/cdvd/disc_overlay.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/core/hash/sha256.h"
#include "te_test_iso.h"

namespace fs = std::filesystem;
namespace mi = te::modimport;
using Bytes = std::vector<uint8_t>;

namespace {

int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failures;                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                                 \
    } while (0)

void Put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void Set32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i)); }
void Set16(Bytes& b, size_t at, uint16_t v) { b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); }

void WriteFile(const fs::path& p, const Bytes& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
void WriteText(const fs::path& p, const std::string& s) { WriteFile(p, Bytes(s.begin(), s.end())); }
Bytes ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string Sha256(const Bytes& b) { return ps2r::hash::Sha256::HexOf(b.data(), b.size()); }
std::string Sha1(const Bytes& b) {
    mi::Sha1 h;
    h.Update(b.data(), b.size());
    return h.HexDigest();
}
Bytes Text(const std::string& s) { return Bytes(s.begin(), s.end()); }

// ---- the archive ----------------------------------------------------------------------------------------------------------
struct Entry { std::string name; Bytes bytes; };
Bytes MakeBh(const std::vector<Entry>& entries) {
    Bytes bh;
    Put32(bh, 1281);
    uint32_t at = 0;
    for (const Entry& e : entries) {
        Put32(bh, uint32_t(e.name.size()));
        bh.insert(bh.end(), e.name.begin(), e.name.end());
        Put32(bh, at);
        Put32(bh, uint32_t(e.bytes.size()));
        at += uint32_t(e.bytes.size());
    }
    return bh;
}
Bytes MakeBd(const std::vector<Entry>& entries) {
    Bytes bd;
    for (const Entry& e : entries) bd.insert(bd.end(), e.bytes.begin(), e.bytes.end());
    return bd;
}
std::map<std::string, Bytes> SplitArchive(const Bytes& bh, const Bytes& bd) {
    std::map<std::string, Bytes> out;
    mi::BhIndex idx;
    std::string why;
    if (!mi::ParseBh(bh, &idx, &why)) return out;
    for (const mi::BhEntry& e : idx.entries)
        if (uint64_t(e.offset) + e.size <= bd.size()) out[e.name] = Bytes(bd.begin() + e.offset, bd.begin() + e.offset + e.size);
    return out;
}

// ---- the executable: the PAL layout's sections over byte patterns -----------------------------------------------------------
Bytes MakeElf() {
    const uint32_t file_end = 0x20B460;   // PT_LOAD: file 0x1000.., vaddr 0x100000.., filesz 0x20A460
    Bytes b = te_test::Pattern(file_end, 77);
    std::memset(b.data(), 0, 0x1000);
    const char* names = "\0.text\0.vutext\0.data\0.rodata\0.sdata\0.shstrtab\0";
    const size_t names_len = 48;
    const uint32_t shstr = uint32_t(b.size());
    b.insert(b.end(), names, names + names_len);
    while (b.size() % 4) b.push_back(0);
    const uint32_t shoff = uint32_t(b.size());
    b.resize(b.size() + 7 * 40, 0);
    auto sec = [&](int i, uint32_t name, uint32_t type, uint32_t addr, uint32_t off, uint32_t size) {
        const size_t s = shoff + size_t(i) * 40;
        Set32(b, s, name); Set32(b, s + 4, type); Set32(b, s + 12, addr); Set32(b, s + 16, off); Set32(b, s + 20, size);
    };
    sec(1, 1, 1, 0x100000, 0x1000, 0x1D9D88);
    sec(2, 7, 1, 0x2D9D90, 0x1DAD90, 0xD140);
    sec(3, 15, 1, 0x2E6F00, 0x1E7F00, 0x5448);
    sec(4, 21, 1, 0x2EC400, 0x1ED400, 0x1D468);
    sec(5, 29, 1, 0x309880, 0x20A880, 0xBE0);
    sec(6, 36, 3, 0, shstr, uint32_t(names_len));
    const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(b.data(), ident, 16);
    Set16(b, 16, 2); Set16(b, 18, 8); Set32(b, 20, 1); Set32(b, 24, 0x00100008); Set32(b, 28, 52); Set32(b, 32, shoff);
    Set16(b, 40, 52); Set16(b, 42, 32); Set16(b, 44, 1); Set16(b, 46, 40); Set16(b, 48, 7); Set16(b, 50, 6);
    Set32(b, 52, 1); Set32(b, 56, 0x1000); Set32(b, 60, 0x100000); Set32(b, 64, 0x100000); Set32(b, 68, 0x20A460);
    Set32(b, 72, 0x2DB200); Set32(b, 76, 7); Set32(b, 80, 0x1000);
    // the two data words the mod changes start as zero
    Set32(b, 0x1000 + (0x2E7000 - 0x100000), 0);
    Set32(b, 0x1000 + (0x2E7004 - 0x100000), 0);
    return b;
}
size_t Off(uint32_t addr) { return 0x1000 + size_t(addr - 0x100000); }

// ---- patch writers (test-side; the judge writes its own in Python) ------------------------------------------------------------
void Vcd(Bytes& b, uint64_t v) {
    uint8_t tmp[10];
    int n = 0;
    do { tmp[n++] = uint8_t(v & 0x7F); v >>= 7; } while (v);
    for (int i = n - 1; i >= 0; --i) b.push_back(uint8_t(tmp[i] | (i ? 0x80 : 0)));
}
// VCDIFF of target against source, windows of `window` target bytes, each with the whole source as its segment: COPY
// (mode SELF) for equal runs, ADD for the rest, xdelta3's Adler-32; the last window also uses a RUN and a target copy
Bytes MakeVcdiff(const Bytes& source, const Bytes& target, size_t window) {
    Bytes out = {0xD6, 0xC3, 0xC4, 0x00, 0x04};
    const std::string app = "target//source/";
    Vcd(out, app.size());
    out.insert(out.end(), app.begin(), app.end());
    for (size_t w = 0; w < target.size(); w += window) {
        const size_t n = std::min(window, target.size() - w);
        Bytes data, inst, addr;
        size_t k = 0;
        while (k < n) {
            const size_t t = w + k;
            size_t eq = 0;
            while (k + eq < n && t + eq < source.size() && source[t + eq] == target[t + eq]) ++eq;
            if (eq >= 8) {
                inst.push_back(19);   // COPY size 0 mode 0
                Vcd(inst, eq);
                Vcd(addr, t);         // SELF: the address in the source segment (the whole source)
                k += eq;
                continue;
            }
            size_t d = 0;
            while (k + d < n) {
                const size_t tt = w + k + d;
                size_t run = 0;
                while (k + d + run < n && tt + run < source.size() && source[tt + run] == target[tt + run]) ++run;
                if (run >= 8) break;
                ++d;
            }
            if (d == 0) d = 1;
            inst.push_back(1);   // ADD size 0
            Vcd(inst, d);
            data.insert(data.end(), target.begin() + std::ptrdiff_t(w + k), target.begin() + std::ptrdiff_t(w + k + d));
            k += d;
        }
        Bytes win;
        win.push_back(0x01 | 0x04);   // VCD_SOURCE | VCD_ADLER32
        Vcd(win, source.size());
        Vcd(win, 0);
        Bytes enc;
        Vcd(enc, n);
        enc.push_back(0);
        Vcd(enc, data.size());
        Vcd(enc, inst.size());
        Vcd(enc, addr.size());
        const uint32_t ad = mi::Adler32Update(1, target.data() + w, n);
        for (int i = 3; i >= 0; --i) enc.push_back(uint8_t(ad >> (8 * i)));
        enc.insert(enc.end(), data.begin(), data.end());
        enc.insert(enc.end(), inst.begin(), inst.end());
        enc.insert(enc.end(), addr.begin(), addr.end());
        Vcd(win, enc.size());
        win.insert(win.end(), enc.begin(), enc.end());
        out.insert(out.end(), win.begin(), win.end());
    }
    return out;
}
// A VCDIFF that is only a RUN and a self-overlapping copy (target window copies), against nothing
Bytes MakeVcdiffRun(uint8_t byte, size_t run, size_t copy) {
    Bytes out = {0xD6, 0xC3, 0xC4, 0x00, 0x00};
    Bytes data = {byte, 'x', 'y'}, inst, addr;
    inst.push_back(0);   // RUN size 0
    Vcd(inst, run);
    inst.push_back(1);   // ADD size 0: 2 bytes
    Vcd(inst, 2);
    inst.push_back(35);  // COPY size 0 mode 1 (HERE)
    Vcd(inst, copy);
    Vcd(addr, 2);        // here - 2: the "xy" just added, copied over itself
    Bytes enc;
    Vcd(enc, run + 2 + copy);
    enc.push_back(0);
    Vcd(enc, data.size());
    Vcd(enc, inst.size());
    Vcd(enc, addr.size());
    enc.insert(enc.end(), data.begin(), data.end());
    enc.insert(enc.end(), inst.begin(), inst.end());
    enc.insert(enc.end(), addr.begin(), addr.end());
    out.push_back(0);   // no source segment
    Vcd(out, enc.size());
    out.insert(out.end(), enc.begin(), enc.end());
    return out;
}
Bytes MakePpf3(const Bytes& source, const Bytes& target) {
    Bytes p = Text("PPF30");
    p.push_back(2);
    std::string desc = "test patch";
    desc.resize(50, ' ');
    p.insert(p.end(), desc.begin(), desc.end());
    p.push_back(0);   // image type BIN
    p.push_back(1);   // validation block
    p.push_back(0);   // no undo
    p.push_back(0);
    p.insert(p.end(), source.begin() + 0x9320, source.begin() + 0x9320 + 1024);
    for (size_t i = 0; i < target.size();) {
        if (i < source.size() && source[i] == target[i]) { ++i; continue; }
        size_t n = 0;
        while (i + n < target.size() && n < 255 && !(i + n < source.size() && source[i + n] == target[i + n])) ++n;
        for (int k = 0; k < 8; ++k) p.push_back(uint8_t(uint64_t(i) >> (8 * k)));
        p.push_back(uint8_t(n));
        p.insert(p.end(), target.begin() + std::ptrdiff_t(i), target.begin() + std::ptrdiff_t(i + n));
        i += n;
    }
    return p;
}
void BpsNum(Bytes& b, uint64_t v) {
    for (;;) {
        const uint8_t x = uint8_t(v & 0x7F);
        v >>= 7;
        if (v == 0) { b.push_back(0x80 | x); return; }
        b.push_back(x);
        --v;
    }
}
Bytes MakeBps(const Bytes& source, const Bytes& target, uint32_t source_crc_override = 0) {
    Bytes p = Text("BPS1");
    BpsNum(p, source.size());
    BpsNum(p, target.size());
    const std::string meta = "test";
    BpsNum(p, meta.size());
    p.insert(p.end(), meta.begin(), meta.end());
    size_t i = 0;
    while (i < target.size()) {
        size_t eq = 0;
        while (i + eq < target.size() && i + eq < source.size() && source[i + eq] == target[i + eq]) ++eq;
        if (eq) { BpsNum(p, ((eq - 1) << 2) | 0); i += eq; continue; }   // SourceRead
        size_t d = 0;
        while (i + d < target.size() && !(i + d < source.size() && source[i + d] == target[i + d])) ++d;
        BpsNum(p, ((d - 1) << 2) | 1);   // TargetRead
        p.insert(p.end(), target.begin() + std::ptrdiff_t(i), target.begin() + std::ptrdiff_t(i + d));
        i += d;
    }
    Put32(p, source_crc_override ? source_crc_override : mi::Crc32Update(0, source.data(), source.size()));
    Put32(p, mi::Crc32Update(0, target.data(), target.size()));
    Put32(p, mi::Crc32Update(0, p.data(), p.size()));
    return p;
}
// A zip of stored entries
void Put16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
Bytes MakeZip(const std::vector<std::pair<std::string, Bytes>>& files) {
    Bytes z, cd;
    for (const auto& f : files) {
        const uint32_t crc = mi::Crc32Update(0, f.second.data(), f.second.size());
        const uint32_t at = uint32_t(z.size()), size = uint32_t(f.second.size());
        const uint16_t name = uint16_t(f.first.size());
        Put32(z, 0x04034b50); Put16(z, 20); Put16(z, 0); Put16(z, 0); Put16(z, 0); Put16(z, 0);
        Put32(z, crc); Put32(z, size); Put32(z, size); Put16(z, name); Put16(z, 0);
        z.insert(z.end(), f.first.begin(), f.first.end());
        z.insert(z.end(), f.second.begin(), f.second.end());
        Put32(cd, 0x02014b50); Put16(cd, 20); Put16(cd, 20); Put16(cd, 0); Put16(cd, 0); Put16(cd, 0); Put16(cd, 0);
        Put32(cd, crc); Put32(cd, size); Put32(cd, size); Put16(cd, name); Put16(cd, 0); Put16(cd, 0); Put16(cd, 0);
        Put16(cd, 0); Put32(cd, 0); Put32(cd, at);
        cd.insert(cd.end(), f.first.begin(), f.first.end());
    }
    const uint32_t cd_at = uint32_t(z.size());
    z.insert(z.end(), cd.begin(), cd.end());
    Put32(z, 0x06054b50); Put16(z, 0); Put16(z, 0); Put16(z, uint16_t(files.size())); Put16(z, uint16_t(files.size()));
    Put32(z, uint32_t(cd.size())); Put32(z, cd_at); Put16(z, 0);
    return z;
}

// ---- deflate: zlib's own streams, and a long one of fixed codes with 32 KB distances (across the decoder's flushes) ------
const uint8_t kFixed[] = {0x2B, 0xC9, 0x48, 0x55, 0x48, 0xCB, 0xAC, 0x48, 0x4D, 0x51, 0x48, 0xCE, 0x4F, 0x49, 0xB5, 0x52, 0x48, 0x4C,
                          0x4A, 0x86, 0x23, 0x85, 0x92, 0x51, 0xC9, 0x51, 0xC9, 0x51, 0xC9, 0x51, 0x49, 0xDA, 0x4B, 0x2A, 0x02, 0x00};
const uint32_t kFixedCrc = 0xBB1A99BAu;   // zlib Z_FIXED of "the fixed code: abcabcabcabc " x 40 + "!"
struct BitWriter {
    Bytes out;
    uint32_t acc = 0;
    int n = 0;
    void Bits(uint32_t v, int count) {   // LSB first
        for (int i = 0; i < count; ++i) { acc |= ((v >> i) & 1u) << n; if (++n == 8) { out.push_back(uint8_t(acc)); acc = 0; n = 0; } }
    }
    void Code(uint32_t code, int len) {   // a Huffman code: its top bit first
        for (int i = len - 1; i >= 0; --i) Bits((code >> i) & 1u, 1);
    }
    void Flush() { if (n) { out.push_back(uint8_t(acc)); acc = 0; n = 0; } }
};
void FixedLiteral(BitWriter& w, uint32_t v) {
    if (v < 144) w.Code(0x30 + v, 8);
    else if (v < 256) w.Code(0x190 + (v - 144), 9);
    else if (v < 280) w.Code(v - 256, 7);
    else w.Code(0xC0 + (v - 280), 8);
}
Bytes FixedStream(const Bytes& data) {
    BitWriter w;
    w.Bits(1, 1);   // final
    w.Bits(1, 2);   // fixed codes
    size_t k = 0;
    while (k < data.size()) {
        if (k >= 32768 && k + 258 <= data.size() && std::memcmp(&data[k], &data[k - 32768], 258) == 0) {
            FixedLiteral(w, 285);          // length 258
            w.Code(29, 5);                 // distance code 29: 24577 + 13 extra bits
            w.Bits(32768 - 24577, 13);
            k += 258;
        } else {
            FixedLiteral(w, data[k++]);
        }
    }
    FixedLiteral(w, 256);
    w.Flush();
    return w.out;
}
bool InflateAll(const Bytes& in, Bytes* out, std::string* why) {
    size_t at = 0;
    out->clear();
    return mi::Inflate([&](uint8_t* dst, size_t n) { const size_t t = std::min(n, in.size() - at); std::memcpy(dst, in.data() + at, t); at += t; return t; },
                       [&](const uint8_t* d, size_t n) { out->insert(out->end(), d, d + n); return true; }, why);
}

std::string Get(const mi::json::Value& v, const std::string& path) {   // "a.b.c"
    const mi::json::Value* cur = &v;
    size_t at = 0;
    while (cur) {
        const size_t dot = path.find('.', at);
        const std::string key = path.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
        cur = cur->Get(key);
        if (dot == std::string::npos) break;
        at = dot + 1;
    }
    if (!cur) return "<absent>";
    if (cur->IsString()) return cur->text;
    if (cur->type == mi::json::Value::Type::Null) return "null";
    return cur->text;
}
mi::json::Value ReadJson(const fs::path& p) {
    const Bytes b = ReadFile(p);
    mi::json::Value v;
    mi::json::Parse(std::string(b.begin(), b.end()), &v, nullptr);
    return v;
}
const mi::json::Value* Row(const mi::json::Value& m, const std::string& path) {
    const mi::json::Value* files = m.Get("files");
    if (!files) return nullptr;
    for (const mi::json::Value& f : files->items)
        if (f.String("path") == path) return &f;
    return nullptr;
}
// A composed file through the game's overlay (one layer)
Bytes Composed(const std::string& iso, const fs::path& mod, const std::string& path, std::string* refused = nullptr) {
    DiscOverlay v({{"m", mod.string(), {}}});
    if (!v.Open(iso)) return {};
    if (refused) *refused = v.GetReport().refused.empty() ? "" : v.GetReport().refused[0];
    IsoFs::Entry e;
    if (!v.Find(path, e)) return {};
    Bytes out(e.size);
    v.ReadBytes(uint64_t(e.lba) * 2048, out.size(), out.data());
    return out;
}

}  // namespace

int main() {
    // ---- hashes and inflate
    {
        CHECK(Sha1(Text("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
        CHECK(Sha1(Text("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        CHECK(Sha1(Bytes(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
        CHECK(mi::Crc32Update(0, "123456789", 9) == 0xCBF43926u);
        CHECK(mi::Adler32Update(1, "Wikipedia", 9) == 0x11E60398u);
        Bytes out;
        std::string why;
        CHECK(InflateAll(Bytes(kFixed, kFixed + sizeof kFixed), &out, &why) && out.size() == 1161 && mi::Crc32Update(0, out.data(), out.size()) == kFixedCrc);
        // stored blocks, written here
        const Bytes plain = te_test::Pattern(100000, 9);
        Bytes stored;
        for (size_t at = 0; at < plain.size(); at += 65535) {
            const size_t n = std::min<size_t>(65535, plain.size() - at);
            stored.push_back(at + n == plain.size() ? 1 : 0);
            stored.push_back(uint8_t(n)); stored.push_back(uint8_t(n >> 8));
            stored.push_back(uint8_t(~n)); stored.push_back(uint8_t(~n >> 8));
            stored.insert(stored.end(), plain.begin() + std::ptrdiff_t(at), plain.begin() + std::ptrdiff_t(at + n));
        }
        CHECK(InflateAll(stored, &out, &why) && out == plain);
        // 3 MB of fixed codes, most of it copies from 32768 bytes back
        Bytes big = te_test::Pattern(32768, 11);
        while (big.size() < (3u << 20)) big.push_back(big[big.size() - 32768] ^ uint8_t(big.size() % 997 == 0 ? 0x5A : 0));
        const Bytes stream = FixedStream(big);
        CHECK(stream.size() < big.size() / 2);
        CHECK(InflateAll(stream, &out, &why) && out == big);
        // damage refused
        Bytes bad = stream;
        bad[bad.size() / 2] ^= 0xFF;
        Bytes junk;
        CHECK(!InflateAll(Bytes{0x07}, &junk, &why));   // the reserved block type
        (void)bad;
        // the sampled hash: windows spread from the first byte to the last
        const Bytes five = te_test::Pattern(5u << 20, 3);
        std::string hex;
        const DiscOverlay::ReadFn read = [&](uint64_t o, size_t n, uint8_t* d) { std::memcpy(d, five.data() + o, n); return true; };
        CHECK(DiscOverlay::SampledSha256(read, five.size(), 64, 65536, &hex));
        ps2r::hash::Sha256 h;
        for (uint64_t i = 0; i < 64; ++i) h.Update(five.data() + (five.size() - 65536) * i / 63, 65536);
        uint8_t dg[32];
        h.Finish(dg);
        CHECK(hex == ps2r::hash::Sha256::Hex(dg));
        CHECK(DiscOverlay::SampledSha256(read, 100000, 64, 65536, &hex) && hex == Sha256(Bytes(five.begin(), five.begin() + 100000)));
        CHECK(!DiscOverlay::SampledSha256(read, 100, 0, 65536, &hex));
    }

    // ---- the disc
    const fs::path root = fs::temp_directory_path() / ("te_mod_import_" + std::to_string(::time(nullptr)) + "_" + std::to_string(std::rand()));
    fs::create_directories(root);
    const std::vector<Entry> entries = {{"Language\\Code\\English.txt", Text(std::string(2900, 'e') + "graphic options\r\n")},
                                        {"Levels\\Earth\\Hub\\beach.rm2", te_test::Pattern(200000, 21)},
                                        {"Startup\\Default.rm2", te_test::Pattern(50000, 22)},
                                        {"Levels\\AltEarth\\Core\\corea.rm2", te_test::Pattern(100000, 23)},
                                        {"Fonts\\Main.psf", te_test::Pattern(10000, 24)}};
    const Bytes bh = MakeBh(entries), bd = MakeBd(entries), elf = MakeElf();
    const Bytes music = te_test::Pattern(1500000, 31), mh = te_test::Pattern(2868, 32);
    const Bytes cnf = Text("BOOT2 = cdrom0:\\SLES_525.68;1\r\nVER = 1.01\r\nVMODE = PAL\r\n");
    const std::string iso = te_test::WriteTestIso({{"", "SYSTEM.CNF", cnf}, {"", "SLES_525.68", elf}, {"CRASH6", "CRASH.BH", bh},
                                                   {"CRASH6", "CRASH.BD", bd}, {"CRASH6", "MUSIC.MB", music}, {"CRASH6", "ENGLISH.MH", mh},
                                                   {"CRASH6", "SYSTEM.CNF", Text("not the boot file")}});
    CHECK(!iso.empty());
    const Bytes iso_bytes = ReadFile(iso);
    const fs::path mods = root / "mods";

    // ---- a Mod Crate (folder): layer0 changes SYSTEM.CNF; layer1 grows corea.rm2 (16 bytes changed, 20000 added) and adds an entry
    Bytes corea2 = entries[3].bytes;
    for (int i = 0; i < 16; ++i) corea2[size_t(5000 + i)] ^= 0xFF;
    const Bytes tail = te_test::Pattern(20000, 41);
    corea2.insert(corea2.end(), tail.begin(), tail.end());
    const Bytes added = te_test::Pattern(65536, 42);
    const Bytes cnf2 = Text("BOOT2 = cdrom0:\\SLES_525.68;1\r\nVER = 1.02\r\nVMODE = PAL\r\n");
    const fs::path crate = root / "crate";
    WriteText(crate / "ModCrateInfo.txt", "Name=Test Crate\r\nDescription=a test\r\nAuthor=Tester\r\nVersion=v1\r\n!a comment line\r\nModLoaderVersion=v1.3.0\r\nGame=CrashTS\r\n");
    WriteFile(crate / "layer0" / "SYSTEM.CNF", cnf2);
    WriteFile(crate / "layer1" / "Levels" / "AltEarth" / "Core" / "corea.rm2", corea2);
    WriteFile(crate / "layer1" / "Mods" / "Test" / "added.bin", added);
    WriteFile(crate / "ModCrateIcon.png", te_test::Pattern(100, 43));
    {
        std::string why;
        CHECK(mi::Detect(crate.string(), &why) == mi::Kind::ModCrate && why.find("Test Crate") != std::string::npos);
        mi::Report r;
        std::vector<std::string> stages;
        double last = -1;
        bool monotonic = true;
        mi::Progress p;
        p.report = [&](const std::string& s, double f) { if (stages.empty() || stages.back() != s) stages.push_back(s); monotonic &= f >= last; last = f; };
        CHECK(mi::Import(crate.string(), iso, mods.string(), p, &r, &why));
        std::fprintf(stderr, "  crate import: %s\n", why.c_str());
        CHECK(monotonic && last == 1.0 && stages.size() >= 3);
        CHECK(r.id == "test_crate" && r.name == "Test Crate");
        CHECK(r.files_changed.size() == 3 && r.files_added.empty() && !r.code_changes_unsupported);
        const fs::path dir = mods / "test_crate";
        CHECK(fs::is_regular_file(dir / "disc" / "SYSTEM.CNF") && fs::is_regular_file(dir / "disc" / "CRASH6" / "CRASH.BH"));
        CHECK(fs::is_regular_file(dir / "disc" / "CRASH6" / "CRASH.BD.ranges") && fs::is_regular_file(dir / "data" / "CRASH6" / "CRASH.BD.bin"));
        CHECK(!fs::exists(mods / ".import-tmp"));
        const mi::json::Value m = ReadJson(dir / "mod.json");
        CHECK(Get(m, "schema") == "2" && Get(m, "kind") == "disc" && Get(m, "source.kind") == "modcrate" && Get(m, "author") == "Tester");
        CHECK(Get(m, "base.region") == "PAL" && Get(m, "base.executable") == "SLES_525.68");
        CHECK(Get(m, "base.disc.size") == std::to_string(iso_bytes.size()) && Get(m, "base.disc.sha1") == Sha1(iso_bytes) && Get(m, "base.disc.redump") == "null");
        const mi::json::Value* bdrow = Row(m, "CRASH6/CRASH.BD");
        CHECK(bdrow && bdrow->String("form") == "ranges" && bdrow->Unsigned("base_bytes") == bd.size() && bdrow->String("base_sha256") == Sha256(bd));
        CHECK(bdrow && bdrow->Unsigned("base_sample_windows") == 64 && bdrow->Unsigned("base_sample_window_bytes") == 65536);
        // the payload holds only the new bytes; the recipe is a few lines
        CHECK(fs::file_size(dir / "data" / "CRASH6" / "CRASH.BD.bin") < 20000 + 65536 + 16 + 8192);
        const Bytes recipe = ReadFile(dir / "disc" / "CRASH6" / "CRASH.BD.ranges");
        CHECK(std::count(recipe.begin(), recipe.end(), '\n') < 12);
        // composed back through the game's overlay: every entry is the crate's or the disc's
        const Bytes cbh = Composed(iso, dir, "CRASH6/CRASH.BH"), cbd = Composed(iso, dir, "CRASH6/CRASH.BD");
        std::map<std::string, Bytes> got = SplitArchive(cbh, cbd);
        CHECK(got.size() == 6);
        CHECK(got["Levels\\AltEarth\\Core\\corea.rm2"] == corea2 && got["Mods\\Test\\added.bin"] == added);
        for (size_t i = 0; i < entries.size(); ++i)
            if (i != 3) CHECK(got[entries[i].name] == entries[i].bytes);
        CHECK(Composed(iso, dir, "SYSTEM.CNF") == cnf2);
        CHECK(Sha256(cbd) == bdrow->String("sha256"));
        mi::Verification v;
        CHECK(mi::Verify(dir.string(), iso, {}, &v, &why) && v.ok && v.files.size() == 3);
        // the same crate again: the same folder (same artefact)
        CHECK(mi::Import(crate.string(), iso, mods.string(), {}, &r, &why) && r.id == "test_crate");
        // as a zip (a different artefact of the same name): ONE RULE -- it replaces the folder; the same files
        std::vector<std::pair<std::string, Bytes>> zf = {{"ModCrateInfo.txt", ReadFile(crate / "ModCrateInfo.txt")},
                                                         {"layer0/SYSTEM.CNF", cnf2},
                                                         {"layer1/Levels/AltEarth/Core/corea.rm2", corea2},
                                                         {"layer1/Mods/Test/added.bin", added}};
        WriteFile(root / "crate.zip", MakeZip(zf));
        CHECK(mi::Detect((root / "crate.zip").string(), &why) == mi::Kind::ModCrate && why.find("zip") != std::string::npos);
        std::string idname;
        CHECK(mi::ModIdFor((root / "crate.zip").string(), &idname) == "test_crate" && idname == "Test Crate");
        CHECK(mi::ModIdFor(crate.string(), &idname) == "test_crate");
        CHECK(mi::Import((root / "crate.zip").string(), iso, mods.string(), {}, &r, &why) && r.id == "test_crate" && r.replaced);
        CHECK(!fs::exists(mods / "test_crate_2") && !fs::exists(mods / ".import-tmp"));
        const mi::json::Value m2 = ReadJson(mods / "test_crate" / "mod.json");
        CHECK(Get(m2, "source.artefact") == "crate.zip");
        CHECK(Row(m2, "CRASH6/CRASH.BD") && Row(m2, "CRASH6/CRASH.BD")->String("sha256") == bdrow->String("sha256"));
        // refusals
        WriteText(crate / "ModCrateSettings.txt", "SomeProperty=1\r\n");
        CHECK(!mi::Import(crate.string(), iso, mods.string(), {}, &r, &why) &&
              why == "Test Crate changes the game's code settings (Crate Mod Loader properties), which this port cannot apply.");
        fs::remove(crate / "ModCrateSettings.txt");
        WriteText(crate / "ModCrateInfo.txt", "Name=Other Game\r\nGame=CrashBandicoot2\r\n");
        CHECK(!mi::Import(crate.string(), iso, mods.string(), {}, &r, &why) && why.find("another game") != std::string::npos);
        CHECK(!fs::exists(mods / "other_game") && !fs::exists(mods / ".import-tmp"));
        // nothing to import: unknown kinds
        WriteFile(root / "x.rar", Text("Rar!\x1A\x07\x01\x00 junk"));
        CHECK(mi::Detect((root / "x.rar").string(), &why) == mi::Kind::Unknown && why.find("RAR") != std::string::npos);
        CHECK(mi::ModIdFor((root / "x.rar").string(), nullptr).empty());
        CHECK(!mi::Import((root / "x.rar").string(), iso, mods.string(), {}, &r, &why) && why.find("RAR") != std::string::npos);
    }

    // ---- a modified image: an entry changed in place, MUSIC.MB's few bytes, an added file, ENGLISH.MH removed, the program
    std::vector<Entry> entries2 = entries;
    std::memcpy(&entries2[0].bytes[2900], "graphic optionz", 15);
    Bytes music2 = music;
    for (int i = 0; i < 100; ++i) music2[size_t(700000 + i)] ^= 0x33;
    Bytes elf2 = elf;
    for (int i = 0; i < 8; ++i) elf2[Off(0x1AC8D4) + size_t(i)] ^= 0xA5;   // inside FUN_001ac8d0 Font::ConstructBase
    std::memcpy(&elf2[Off(0x2F0000)], "TEST", 4);                          // .rodata
    Set32(elf2, Off(0x2E7000), 0x001AC8D0);                                // .data: a pointer into the changed function
    Set32(elf2, Off(0x2E7004), 0x00100228);                                // .data: a pointer to an existing function
    const Bytes newbin = te_test::Pattern(3000, 51);
    const Bytes bd2 = MakeBd(entries2);
    const std::string iso2 = te_test::WriteTestIso({{"", "SYSTEM.CNF", cnf}, {"", "SLES_525.68", elf2}, {"CRASH6", "CRASH.BH", MakeBh(entries2)},
                                                    {"CRASH6", "CRASH.BD", bd2}, {"CRASH6", "MUSIC.MB", music2},
                                                    {"CRASH6", "SYSTEM.CNF", Text("not the boot file")}, {"CRASH6", "NEW.BIN", newbin}});
    CHECK(!iso2.empty());
    const Bytes iso2_bytes = ReadFile(iso2);
    std::map<std::string, std::string> image_hashes;   // the files every patch kind must import to
    {
        std::string why;
        mi::Report r;
        CHECK(mi::Detect(iso2, &why) == mi::Kind::PatchedIso);
        mi::Inspection in;
        CHECK(mi::Inspect(iso2, iso, {}, &in, &why) && in.importable && in.files_removed.size() == 1 && in.code_changes.size() == 1);
        CHECK(mi::Import(iso2, iso, mods.string(), {}, &r, &why));
        const fs::path dir = mods / r.id;
        const mi::json::Value m = ReadJson(dir / "mod.json");
        CHECK(Get(m, "source.kind") == "iso" && Get(m, "source.sha256") == Sha256(iso2_bytes));
        CHECK(r.files_added.size() == 1 && r.files_added[0] == "CRASH6/NEW.BIN");
        CHECK(Row(m, "CRASH6/CRASH.BD") && Row(m, "CRASH6/CRASH.BD")->String("form") == "ranges");
        CHECK(Row(m, "CRASH6/MUSIC.MB") && Row(m, "CRASH6/MUSIC.MB")->String("form") == "ranges");
        CHECK(!Row(m, "CRASH6/CRASH.BH") && !Row(m, "SYSTEM.CNF"));   // unchanged
        CHECK(r.code_changes.size() == 1 && r.code_changes[0] == "FUN_001ac8d0 Font::ConstructBase" && r.code_changes_unsupported);
        const mi::json::Value* ex = m.Get("executable");
        const mi::json::Value* dc = ex ? ex->Get("data_changes") : nullptr;
        CHECK(dc && dc->Unsigned(".rodata") == 4 && dc->Unsigned(".data") == 3 && dc->Unsigned(".sdata") == 0 && dc->Unsigned("vu") == 0);
        CHECK(ex && ex->Get("data_kept_from_disc") && ex->Get("data_kept_from_disc")->items.size() == 1);
        const mi::json::Value* removed = m.Get("removed");
        CHECK(removed && removed->items.size() == 1 && removed->items[0].text == "CRASH6/ENGLISH.MH");
        CHECK(Composed(iso, dir, "CRASH6/CRASH.BD") == bd2 && Composed(iso, dir, "CRASH6/MUSIC.MB") == music2);
        CHECK(Composed(iso, dir, "CRASH6/NEW.BIN") == newbin);
        Bytes elf_kept = elf2;
        Set32(elf_kept, Off(0x2E7000), 0);   // the pointer into the mod's code keeps the disc's value
        CHECK(Composed(iso, dir, "SLES_525.68") == elf_kept);
        CHECK(fs::file_size(dir / "data" / "CRASH6" / "MUSIC.MB.bin") < 8192);   // a few bytes, not 1.5 MB
        mi::Verification v;
        CHECK(mi::Verify(dir.string(), iso, {}, &v, &why) && v.ok);
        for (const mi::json::Value& f : m.Get("files")->items) image_hashes[f.String("path")] = f.String("sha256");
        CHECK(image_hashes.size() == 4);

        // ---- the same image as patches
        const std::vector<std::pair<std::string, Bytes>> patches = {
            {"image.xdelta", MakeVcdiff(iso_bytes, iso2_bytes, 1u << 20)}, {"image.ppf", MakePpf3(iso_bytes, iso2_bytes)},
            {"image.bps", MakeBps(iso_bytes, iso2_bytes)}};
        for (const auto& pt : patches) {
            WriteFile(root / pt.first, pt.second);
            mi::Report pr;
            CHECK(mi::Import((root / pt.first).string(), iso, mods.string(), {}, &pr, &why));
            std::fprintf(stderr, "  %s -> %s %s\n", pt.first.c_str(), pr.id.c_str(), why.c_str());
            const mi::json::Value pm = ReadJson(mods / pr.id / "mod.json");
            std::map<std::string, std::string> hashes;
            if (pm.Get("files"))
                for (const mi::json::Value& f : pm.Get("files")->items) hashes[f.String("path")] = f.String("sha256");
            CHECK(hashes == image_hashes);
            CHECK(!fs::exists(mods / ".import-tmp"));
        }
        // a patch for another disc
        WriteFile(root / "other.bps", MakeBps(iso_bytes, iso2_bytes, 0x12345678u));
        CHECK(!mi::Import((root / "other.bps").string(), iso, mods.string(), {}, &r, &why) && why.find("different disc") != std::string::npos);
        Bytes badppf = MakePpf3(iso_bytes, iso2_bytes);
        badppf[60] ^= 0xFF;   // its check of the image
        WriteFile(root / "other.ppf", badppf);
        CHECK(!mi::Import((root / "other.ppf").string(), iso, mods.string(), {}, &r, &why) && why.find("different disc") != std::string::npos);
        // a VCDIFF of a run and a self-overlapping copy (no source)
        WriteFile(root / "run.vcdiff", MakeVcdiffRun('z', 1000, 5000));
        mi::PatchHeader ph;
        CHECK(mi::ApplyPatch(mi::PatchFormat::Xdelta, (root / "run.vcdiff").string(), iso, (root / "run.out").string(), {}, &ph, &why));
        Bytes want(1000, 'z');
        for (int i = 0; i < 2502; ++i) { want.push_back('x'); want.push_back('y'); }
        want.resize(1000 + 2 + 5000);
        CHECK(ReadFile(root / "run.out") == want);

        // ---- the base check: the overlay refuses a recipe over another base (three ways), applies it over its own
        const fs::path copy = root / "mods_check" / "check";
        fs::create_directories(copy.parent_path());
        fs::copy(dir, copy, fs::copy_options::recursive);
        const Bytes manifest = ReadFile(copy / "mod.json");
        const std::string text(manifest.begin(), manifest.end());
        auto with = [&](const std::string& from, const std::string& to) {
            std::string t = text;
            const size_t at = t.find(from);
            if (at != std::string::npos) t.replace(at, from.size(), to);
            WriteText(copy / "mod.json", t);
        };
        mi::Verification v2;
        const std::string sample = Row(m, "CRASH6/CRASH.BD")->String("base_sample_sha256");
        with("\"base_sample_sha256\": \"" + sample + "\"", "\"base_sample_sha256\": \"" + std::string(64, '0') + "\"");
        CHECK(mi::Verify(copy.string(), iso, {}, &v2, &why) && !v2.ok && !v2.problems.empty() &&
              v2.problems[0].find("made for another CRASH6/CRASH.BD") != std::string::npos);
        with("\"base_bytes\": " + std::to_string(bd.size()), "\"base_bytes\": " + std::to_string(bd.size() + 1));
        CHECK(mi::Verify(copy.string(), iso, {}, &v2, &why) && !v2.ok && v2.problems[0].find("bytes; the one below has") != std::string::npos);
        // no sample recorded: the whole file's hash is checked
        std::string t = text;
        const size_t s_at = t.find("\"base_sample_sha256\": \"" + sample + "\"");
        CHECK(s_at != std::string::npos);
        t.replace(s_at, std::string("\"base_sample_sha256\": \"" + sample + "\"").size(), "\"base_sample_sha256\": \"\"");
        const std::string full = Row(m, "CRASH6/CRASH.BD")->String("base_sha256");
        const size_t f_at = t.find(full);
        t.replace(f_at, full.size(), std::string(64, 'f'));
        WriteText(copy / "mod.json", t);
        CHECK(mi::Verify(copy.string(), iso, {}, &v2, &why) && !v2.ok && v2.problems[0].find("(the same size, other bytes: sha256") != std::string::npos);
        WriteText(copy / "mod.json", text);
        CHECK(mi::Verify(copy.string(), iso, {}, &v2, &why) && v2.ok);
        // over another disc (the modified image as the disc): refused
        CHECK(mi::Verify(copy.string(), iso2, {}, &v2, &why) && !v2.ok);
    }

    // ---- loose files matched by name; an ambiguous name left out; notes ignored
    {
        const fs::path loose = root / "loose";
        WriteFile(loose / "CRASH.BD", bd2);
        WriteFile(loose / "CRASH.BH", MakeBh(entries2));
        WriteFile(loose / "sub" / "SYSTEM.CNF", cnf2);   // two disc files of this name
        WriteText(loose / "readme.txt", "notes");
        std::string why;
        mi::Report r;
        CHECK(mi::Detect(loose.string(), &why) == mi::Kind::Folder);
        CHECK(mi::Import(loose.string(), iso, mods.string(), {}, &r, &why));
        CHECK(r.files_changed.size() == 1 && r.files_changed[0] == "CRASH6/CRASH.BD");   // the index is the same
        bool ambiguous = false;
        for (const std::string& w : r.warnings) ambiguous |= w.find("sub/SYSTEM.CNF: the disc has 2 files of this name") != std::string::npos;
        CHECK(ambiguous);
        CHECK(Composed(iso, mods / r.id, "CRASH6/CRASH.BD") == bd2);
        // a folder holding one patch and a note: the patch
        const fs::path holder = root / "holder";
        WriteFile(holder / "the patch.bps", MakeBps(iso_bytes, iso2_bytes));
        WriteText(holder / "README.md", "apply me");
        CHECK(mi::Detect(holder.string(), &why) == mi::Kind::Bps);
        // a mod folder already (mod.json): added as it is, under its own id; a second add replaces it
        const fs::path ready = root / "ready";
        fs::copy(mods / r.id, ready, fs::copy_options::recursive);
        const Bytes rm = ReadFile(ready / "mod.json");
        std::string t(rm.begin(), rm.end());
        const size_t idp = t.find("\"id\": \"" + r.id + "\"");
        CHECK(idp != std::string::npos);
        t.replace(idp, std::string("\"id\": \"" + r.id + "\"").size(), "\"id\": \"ready_mod\"");
        WriteText(ready / "mod.json", t);
        CHECK(mi::Detect(ready.string(), &why) == mi::Kind::Folder && why.find("mod folder") != std::string::npos);
        mi::Report rr;
        CHECK(mi::Import(ready.string(), iso, mods.string(), {}, &rr, &why) && rr.id == "ready_mod");
        CHECK(fs::is_regular_file(mods / "ready_mod" / "disc" / "CRASH6" / "CRASH.BD.ranges"));
        CHECK(rr.files_changed.size() == 1 && rr.files_changed[0] == "CRASH6/CRASH.BD");
        CHECK(Composed(iso, mods / "ready_mod", "CRASH6/CRASH.BD") == bd2);
        CHECK(mi::Import(ready.string(), iso, mods.string(), {}, &rr, &why) && rr.id == "ready_mod" && rr.replaced && !fs::exists(mods / "ready_mod_2"));
        CHECK(mi::ModIdFor(ready.string(), nullptr) == "ready_mod");
        // a folder of the mod's id that is not a mod: the player's, never replaced
        fs::create_directories(mods / "notamod");
        WriteText(mods / "notamod" / "keep.txt", "mine");
        WriteFile(root / "notamod.bin", te_test::Pattern(10, 5));
        CHECK(!mi::Import((root / "notamod.bin").string(), iso, mods.string(), {}, &rr, &why) &&
              why == "The mods folder has a folder named notamod that is not a mod: move it away, then add the mod again." &&
              fs::is_regular_file(mods / "notamod" / "keep.txt"));
    }

    // ---- loose ARCHIVE ENTRIES (a bare file, a folder that mirrors neither the disc nor a crate): matched by name into the
    //      archive index -- a unique last name, or the index path; several entries of the name, or none: refused, listed
    {
        const fs::path lroot = root / "loose_entries";
        std::string why;
        mi::Report r;
        // a bare Default.rm2, 4 bytes shorter than the disc's entry (the next entries' offsets move: the index is rebuilt)
        const Bytes def4(entries[2].bytes.begin(), entries[2].bytes.end() - 4);
        WriteFile(lroot / "Default.rm2", def4);
        CHECK(mi::Detect((lroot / "Default.rm2").string(), &why) == mi::Kind::Folder && why.find("single file (Default.rm2)") != std::string::npos);
        mi::Inspection in;
        CHECK(mi::Inspect((lroot / "Default.rm2").string(), iso, {}, &in, &why) && in.importable &&
              in.archive_entries_changed == std::vector<std::string>{"Startup\\Default.rm2"});
        CHECK(mi::Import((lroot / "Default.rm2").string(), iso, mods.string(), {}, &r, &why));
        CHECK(r.id == "default" && r.name == "Default" && r.files_added.empty());
        CHECK(r.files_changed == (std::vector<std::string>{"CRASH6/CRASH.BD", "CRASH6/CRASH.BH"}));
        {
            const fs::path dir = mods / "default";
            const mi::json::Value m = ReadJson(dir / "mod.json");
            CHECK(Get(m, "source.kind") == "folder" && Get(m, "source.artefact") == "Default.rm2" && Get(m, "author").empty());
            CHECK(m.Get("import") && m.Get("import")->Get("archive") && m.Get("import")->Get("archive")->Get("entries_changed") &&
                  m.Get("import")->Get("archive")->Get("entries_changed")->items.size() == 1);
            const Bytes cbh = Composed(iso, dir, "CRASH6/CRASH.BH"), cbd = Composed(iso, dir, "CRASH6/CRASH.BD");
            CHECK(cbd.size() == bd.size() - 4);
            std::map<std::string, Bytes> got = SplitArchive(cbh, cbd);
            CHECK(got.size() == entries.size() && got["Startup\\Default.rm2"] == def4);
            for (size_t i = 0; i < entries.size(); ++i)
                if (i != 2) CHECK(got[entries[i].name] == entries[i].bytes);
            mi::BhIndex idx;
            CHECK(mi::ParseBh(cbh, &idx, &why) && idx.entries.size() == entries.size() &&
                  idx.entries[3].offset + 4 == uint32_t(entries[0].bytes.size() + entries[1].bytes.size() + entries[2].bytes.size()));
            mi::Verification v;
            CHECK(mi::Verify(dir.string(), iso, {}, &v, &why) && v.ok && v.files.size() == 2);
        }
        // laid out like the index (Startup/Default.rm2): by the index path; the bare and the laid-out file both: the path wins
        WriteFile(lroot / "byindex" / "Startup" / "Default.rm2", def4);
        CHECK(mi::Import((lroot / "byindex").string(), iso, mods.string(), {}, &r, &why) && r.files_changed.size() == 2);
        WriteFile(lroot / "both" / "Startup" / "Default.rm2", def4);
        WriteFile(lroot / "both" / "Default.rm2", te_test::Pattern(50000, 99));
        CHECK(mi::Import((lroot / "both").string(), iso, mods.string(), {}, &r, &why));
        bool path_won = false;
        for (const std::string& w : r.warnings) path_won |= w.find("Default.rm2: Startup/Default.rm2 is already Startup\\Default.rm2") != std::string::npos;
        CHECK(path_won && SplitArchive(Composed(iso, mods / r.id, "CRASH6/CRASH.BH"), Composed(iso, mods / r.id, "CRASH6/CRASH.BD"))["Startup\\Default.rm2"] == def4);
        // a disc file dropped bare: a disc file replacement (by its name)
        Bytes mh2 = mh;
        mh2[100] ^= 0x5A;
        WriteFile(lroot / "ENGLISH.MH", mh2);
        CHECK(mi::Import((lroot / "ENGLISH.MH").string(), iso, mods.string(), {}, &r, &why) &&
              r.files_changed == std::vector<std::string>{"CRASH6/ENGLISH.MH"} && Composed(iso, mods / r.id, "CRASH6/ENGLISH.MH") == mh2);
        // no entry and no file of the name: refused with the names tried
        WriteFile(lroot / "Nothing" / "thing.rm2", te_test::Pattern(1000, 98));
        CHECK(!mi::Import((lroot / "Nothing" / "thing.rm2").string(), iso, mods.string(), {}, &r, &why) &&
              why == "Neither your disc nor its archive has a file named thing.rm2 (looked for THING.RM2 among the disc's files and any of "
                     "either named thing.rm2).");
        // another release's program, by its name
        WriteFile(lroot / "SLUS_209.09", te_test::Pattern(1000, 97));
        CHECK(!mi::Import((lroot / "SLUS_209.09").string(), iso, mods.string(), {}, &r, &why) &&
              why == "The file SLUS_209.09 is the program of the North American disc, not of this disc (SLES_525.68): it was made for the "
                     "North American disc.");
        // the size of the North American disc's entry (built-in table) and nothing of this disc's entry: refused
        WriteFile(lroot / "us" / "Default.rm2", te_test::Pattern(2497049, 96));
        CHECK(!mi::Import((lroot / "us" / "Default.rm2").string(), iso, mods.string(), {}, &r, &why) &&
              why == "The file Default.rm2 matches the North American disc's entry Startup\\Default.rm2 (2497049 bytes; it has 2497049), not "
                     "this disc's (50000 bytes): it was made for the North American disc.");
        CHECK(!fs::exists(mods / "default_2") && !fs::exists(mods / ".import-tmp"));
        // the other way round, on a North American disc whose entry has the size of that disc's (2,497,049): a file 4 bytes
        // shorter than the European entry (2,154,611) made of other bytes is refused; this disc's entry cut to that size is an
        // edit of this disc's entry (more than half of it is that entry's bytes) and is taken
        const Bytes us_default = te_test::Pattern(2497049, 70);
        const std::vector<Entry> us_entries = {{"Startup\\Default.rm2", us_default}, {"Language\\Code\\American.txt", te_test::Pattern(5000, 71)}};
        const std::string iso4 = te_test::WriteTestIso({{"", "SYSTEM.CNF", Text("BOOT2 = cdrom0:\\SLUS_209.09;1\r\nVER = 2.00\r\nVMODE = NTSC\r\n")},
                                                        {"", "SLUS_209.09", Text("program")}, {"CRASH6", "CRASH.BH", MakeBh(us_entries)},
                                                        {"CRASH6", "CRASH.BD", MakeBd(us_entries)}});
        CHECK(!iso4.empty());
        WriteFile(lroot / "eu" / "Default.rm2", te_test::Pattern(2154607, 72));
        CHECK(!mi::Import((lroot / "eu" / "Default.rm2").string(), iso4, mods.string(), {}, &r, &why) &&
              why == "The file Default.rm2 matches the European disc's entry Startup\\Default.rm2 (2154611 bytes; it has 2154607), not this "
                     "disc's (2497049 bytes): it was made for the European disc.");
        WriteFile(lroot / "cut" / "Default.rm2", Bytes(us_default.begin(), us_default.begin() + 2154607));
        CHECK(mi::ModIdFor((lroot / "cut" / "Default.rm2").string(), nullptr) == "default");
        CHECK(mi::Import((lroot / "cut" / "Default.rm2").string(), iso4, mods.string(), {}, &r, &why) && r.id == "default" && r.replaced);
        CHECK(Get(ReadJson(mods / "default" / "mod.json"), "base.region") == "NTSC-U" &&
              Get(ReadJson(mods / "default" / "mod.json"), "base.executable") == "SLUS_209.09");
        std::error_code ec4;
        fs::remove(iso4, ec4);
        // a name several entries share: listed, not imported
        const std::vector<Entry> twins = {{"Language\\Code\\English.txt", te_test::Pattern(3000, 61)},
                                          {"Language\\Credits\\English.txt", te_test::Pattern(4000, 62)}};
        const std::string iso3 = te_test::WriteTestIso({{"", "SYSTEM.CNF", cnf}, {"", "SLES_525.68", Text("program")},
                                                        {"CRASH6", "CRASH.BH", MakeBh(twins)}, {"CRASH6", "CRASH.BD", MakeBd(twins)}});
        CHECK(!iso3.empty());
        WriteFile(lroot / "English.txt", te_test::Pattern(3000, 63));
        CHECK(!mi::Import((lroot / "English.txt").string(), iso3, mods.string(), {}, &r, &why) &&
              why == "The file English.txt could match 2 entries of the archive (Language\\Code\\English.txt, Language\\Credits\\English.txt): put "
                     "it in folders named like the one it replaces (Language/Code/English.txt) to choose.");
        WriteFile(lroot / "texts" / "Language" / "Credits" / "English.txt", te_test::Pattern(4100, 64));
        CHECK(mi::Import((lroot / "texts").string(), iso3, mods.string(), {}, &r, &why) &&
              SplitArchive(Composed(iso3, mods / r.id, "CRASH6/CRASH.BH"), Composed(iso3, mods / r.id, "CRASH6/CRASH.BD"))["Language\\Credits\\English.txt"] ==
                  te_test::Pattern(4100, 64));
        std::error_code ec3;
        fs::remove(iso3, ec3);
    }

    // ---- a crate's '!' notes: kept in mod.json; "apply <Crate Mod Loader settings>" -> the warning that the port cannot apply them
    {
        const fs::path nc = root / "notes_crate";
        WriteText(nc / "ModCrateInfo.txt", "Name=Notes Crate\r\nAuthor=Tester\r\nVersion=v2\r\n!Made for the test.\r\n"
                                           "!For the mod to work, apply Classic Slide Jump for Crash and Classic Explosion Damage.\r\nGame=CrashTS\r\n");
        WriteFile(nc / "layer1" / "Fonts" / "Main.psf", te_test::Pattern(10000, 65));
        std::string why;
        mi::Report r;
        CHECK(mi::Import(nc.string(), iso, mods.string(), {}, &r, &why) && r.id == "notes_crate");
        const mi::json::Value m = ReadJson(mods / "notes_crate" / "mod.json");
        const mi::json::Value* notes = m.Get("notes");
        CHECK(notes && notes->items.size() == 2 && notes->items[1].text == "For the mod to work, apply Classic Slide Jump for Crash and Classic Explosion Damage.");
        bool warned = false;
        for (const std::string& w : r.warnings)
            warned |= w == "this mod asks for game-code settings of Crate Mod Loader (Classic Slide Jump for Crash, Classic Explosion Damage) that this port cannot apply";
        CHECK(warned);
        mi::Inspection in;
        CHECK(mi::Inspect(nc.string(), iso, {}, &in, &why) && in.notes.size() == 2 && in.importable);
    }

    // ---- cancelled: nothing left behind
    {
        const fs::path empty = root / "mods_cancel";
        int calls = 0;
        mi::Progress p;
        p.cancelled = [&] { return ++calls > 3; };
        mi::Report r;
        std::string why;
        CHECK(!mi::Import(iso2, iso, empty.string(), p, &r, &why) && why == "The import was stopped.");
        std::vector<std::string> left;
        for (const auto& e : fs::directory_iterator(empty)) left.push_back(e.path().filename().string());
        CHECK(left.empty() || (left.size() == 1 && left[0] == ".import-cache.json"));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove(iso, ec);
    fs::remove(iso2, ec);
    std::printf("test_mod_import: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
