// zip_reader.h implementation (APPNOTE.TXT 6.3: 4.3.7 local header, 4.3.12 central directory, 4.3.16 end of central
// directory, 4.3.14-15 the ZIP64 end record and its locator, 4.5.3 the ZIP64 extra field).
#include "app/host/mod_import/zip_reader.h"

#include <algorithm>
#include <cstring>

#include "app/host/mod_import/hashes.h"
#include "app/host/mod_import/inflate.h"
#include "app/host/mod_import/io.h"

namespace te::modimport {

namespace {

constexpr uint32_t kLocalSig = 0x04034b50u, kCentralSig = 0x02014b50u, kEndSig = 0x06054b50u;
constexpr uint32_t kZip64EndSig = 0x06064b50u, kZip64LocatorSig = 0x07064b50u;
constexpr size_t kChunk = 1u << 20;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32; }

bool PreadAll(io::File& f, uint8_t* dst, size_t n, uint64_t offset) { return n == 0 || f.ReadAt(offset, dst, n); }

// CP437's upper half as UTF-8 (a zip name without the UTF-8 flag)
const char* const kCp437[128] = {
    "\xC3\x87", "\xC3\xBC", "\xC3\xA9", "\xC3\xA2", "\xC3\xA4", "\xC3\xA0", "\xC3\xA5", "\xC3\xA7", "\xC3\xAA", "\xC3\xAB",
    "\xC3\xA8", "\xC3\xAF", "\xC3\xAE", "\xC3\xAC", "\xC3\x84", "\xC3\x85", "\xC3\x89", "\xC3\xA6", "\xC3\x86", "\xC3\xB4",
    "\xC3\xB6", "\xC3\xB2", "\xC3\xBB", "\xC3\xB9", "\xC3\xBF", "\xC3\x96", "\xC3\x9C", "\xC2\xA2", "\xC2\xA3", "\xC2\xA5",
    "\xE2\x82\xA7", "\xC6\x92", "\xC3\xA1", "\xC3\xAD", "\xC3\xB3", "\xC3\xBA", "\xC3\xB1", "\xC3\x91", "\xC2\xAA", "\xC2\xBA",
    "\xC2\xBF", "\xE2\x8C\x90", "\xC2\xAC", "\xC2\xBD", "\xC2\xBC", "\xC2\xA1", "\xC2\xAB", "\xC2\xBB", "\xE2\x96\x91",
    "\xE2\x96\x92", "\xE2\x96\x93", "\xE2\x94\x82", "\xE2\x94\xA4", "\xE2\x95\xA1", "\xE2\x95\xA2", "\xE2\x95\x96",
    "\xE2\x95\x95", "\xE2\x95\xA3", "\xE2\x95\x91", "\xE2\x95\x97", "\xE2\x95\x9D", "\xE2\x95\x9C", "\xE2\x95\x9B",
    "\xE2\x94\x90", "\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\xAC", "\xE2\x94\x9C", "\xE2\x94\x80", "\xE2\x94\xBC",
    "\xE2\x95\x9E", "\xE2\x95\x9F", "\xE2\x95\x9A", "\xE2\x95\x94", "\xE2\x95\xA9", "\xE2\x95\xA6", "\xE2\x95\xA0",
    "\xE2\x95\x90", "\xE2\x95\xAC", "\xE2\x95\xA7", "\xE2\x95\xA8", "\xE2\x95\xA4", "\xE2\x95\xA5", "\xE2\x95\x99",
    "\xE2\x95\x98", "\xE2\x95\x92", "\xE2\x95\x93", "\xE2\x95\xAB", "\xE2\x95\xAA", "\xE2\x94\x98", "\xE2\x94\x8C",
    "\xE2\x96\x88", "\xE2\x96\x84", "\xE2\x96\x8C", "\xE2\x96\x90", "\xE2\x96\x80", "\xCE\xB1", "\xC3\x9F", "\xCE\x93",
    "\xCF\x80", "\xCE\xA3", "\xCF\x83", "\xC2\xB5", "\xCF\x84", "\xCE\xA6", "\xCE\x98", "\xCE\xA9", "\xCE\xB4", "\xE2\x88\x9E",
    "\xCF\x86", "\xCE\xB5", "\xE2\x88\xA9", "\xE2\x89\xA1", "\xC2\xB1", "\xE2\x89\xA5", "\xE2\x89\xA4", "\xE2\x8C\xA0",
    "\xE2\x8C\xA1", "\xC3\xB7", "\xE2\x89\x88", "\xC2\xB0", "\xE2\x88\x99", "\xC2\xB7", "\xE2\x88\x9A", "\xE2\x81\xBF",
    "\xC2\xB2", "\xE2\x96\xA0", "\xC2\xA0"};

std::string NameOf(const uint8_t* p, size_t n, bool utf8) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = p[i];
        if (c == '\\') out.push_back('/');
        else if (c < 0x80 || utf8) out.push_back(char(c));
        else out += kCp437[c - 0x80];
    }
    return out;
}

}  // namespace

ZipReader::~ZipReader() { Close(); }

bool ZipReader::LooksLikeZip(const uint8_t* head, size_t n) {
    return n >= 4 && head[0] == 'P' && head[1] == 'K' && ((head[2] == 3 && head[3] == 4) || (head[2] == 5 && head[3] == 6));
}

void ZipReader::Close() {
    file_.Close();
    file_size_ = 0;
    entries_.clear();
}

bool ZipReader::Open(const std::string& path, std::string* error) {
    Close();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        Close();
        return false;
    };
    if (!file_.OpenRead(path)) return fail("cannot open " + path);
    if (file_.Size() < 22) return fail("it is too short to be a zip file");
    file_size_ = file_.Size();
    // the end record: in the last 64 KB + 22 bytes (a comment may follow it)
    const size_t tail_len = size_t(std::min<uint64_t>(file_size_, 65557));
    std::vector<uint8_t> tail(tail_len);
    if (!PreadAll(file_, tail.data(), tail_len, file_size_ - tail_len)) return fail("it does not read");
    size_t end_at = std::string::npos;
    for (size_t i = tail_len - 22 + 1; i-- > 0;)
        if (Le32(&tail[i]) == kEndSig) { end_at = i; break; }
    if (end_at == std::string::npos) return fail("it is not a zip file (no end of central directory)");
    const uint8_t* e = &tail[end_at];
    uint64_t count = Le16(e + 10), cd_size = Le32(e + 12), cd_offset = Le32(e + 16);
    if (Le16(e + 4) != 0 || Le16(e + 6) != 0) return fail("it is one part of a split zip file");
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) {
        // ZIP64: the locator sits just before the end record
        const uint64_t end_pos = file_size_ - tail_len + end_at;
        if (end_pos < 20) return fail("a ZIP64 zip without its locator");
        uint8_t loc[20];
        if (!PreadAll(file_, loc, 20, end_pos - 20) || Le32(loc) != kZip64LocatorSig) return fail("a ZIP64 zip without its locator");
        const uint64_t rec_at = Le64(loc + 8);
        uint8_t rec[56];
        if (!PreadAll(file_, rec, 56, rec_at) || Le32(rec) != kZip64EndSig) return fail("a ZIP64 zip whose end record does not read");
        count = Le64(rec + 32);
        cd_size = Le64(rec + 40);
        cd_offset = Le64(rec + 48);
    }
    if (cd_offset > file_size_ || cd_size > file_size_ - cd_offset || cd_size > (256u << 20))
        return fail("its central directory is outside the file");
    std::vector<uint8_t> cd(static_cast<size_t>(cd_size));
    if (!PreadAll(file_, cd.data(), cd.size(), cd_offset)) return fail("its central directory does not read");
    size_t at = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (at + 46 > cd.size() || Le32(&cd[at]) != kCentralSig) return fail("its central directory is damaged");
        const uint8_t* c = &cd[at];
        Entry ent;
        ent.flags = Le16(c + 8);
        ent.method = Le16(c + 10);
        ent.crc = Le32(c + 16);
        ent.compressed = Le32(c + 20);
        ent.size = Le32(c + 24);
        const size_t name_len = Le16(c + 28), extra_len = Le16(c + 30), comment_len = Le16(c + 32);
        ent.local_offset = Le32(c + 42);
        if (at + 46 + name_len + extra_len + comment_len > cd.size()) return fail("its central directory is damaged");
        ent.name = NameOf(c + 46, name_len, (ent.flags & 0x0800u) != 0);
        // the ZIP64 extra field: the 0xFFFFFFFF fields, in order
        const uint8_t* x = c + 46 + name_len;
        for (size_t k = 0; k + 4 <= extra_len;) {
            const uint16_t id = Le16(x + k), len = Le16(x + k + 2);
            if (k + 4 + len > extra_len) break;
            if (id == 0x0001) {
                const uint8_t* v = x + k + 4;
                size_t off = 0;
                if (ent.size == 0xFFFFFFFFu && off + 8 <= len) { ent.size = Le64(v + off); off += 8; }
                if (ent.compressed == 0xFFFFFFFFu && off + 8 <= len) { ent.compressed = Le64(v + off); off += 8; }
                if (ent.local_offset == 0xFFFFFFFFu && off + 8 <= len) { ent.local_offset = Le64(v + off); off += 8; }
            }
            k += 4 + size_t(len);
        }
        ent.directory = !ent.name.empty() && ent.name.back() == '/';
        entries_.push_back(std::move(ent));
        at += 46 + name_len + extra_len + comment_len;
    }
    return true;
}

bool ZipReader::DataOffset(const Entry& entry, uint64_t* offset, std::string* error) const {
    uint8_t h[30];
    if (!file_.IsOpen() || entry.local_offset > file_size_ || !PreadAll(file_, h, 30, entry.local_offset) || Le32(h) != kLocalSig) {
        if (error) *error = entry.name + ": its local header does not read";
        return false;
    }
    *offset = entry.local_offset + 30 + Le16(h + 26) + Le16(h + 28);
    if (*offset > file_size_ || entry.compressed > file_size_ - *offset) {
        if (error) *error = entry.name + ": its data is outside the file";
        return false;
    }
    return true;
}

bool ZipReader::Extract(const Entry& entry, const Sink& sink, std::string* error) const {
    if (entry.flags & 0x0001u) {
        if (error) *error = entry.name + " is encrypted (a password-protected zip): extract it with the password first";
        return false;
    }
    if (entry.method != 0 && entry.method != 8) {
        if (error) *error = entry.name + " is compressed with method " + std::to_string(entry.method) +
                            " (only stored and deflated entries are read): extract the zip first";
        return false;
    }
    uint64_t at = 0;
    if (!DataOffset(entry, &at, error)) return false;
    uint32_t crc = 0;
    uint64_t produced = 0;
    auto counted = [&](const uint8_t* data, size_t n) {
        crc = Crc32Update(crc, data, n);
        produced += n;
        if (produced > entry.size) return false;
        return sink ? sink(data, n) : true;
    };
    if (entry.method == 0) {
        if (entry.compressed != entry.size) {
            if (error) *error = entry.name + ": a stored entry whose sizes disagree";
            return false;
        }
        std::vector<uint8_t> buf(kChunk);
        for (uint64_t done = 0; done < entry.size;) {
            const size_t n = size_t(std::min<uint64_t>(buf.size(), entry.size - done));
            if (!PreadAll(file_, buf.data(), n, at + done)) {
                if (error) *error = entry.name + ": does not read";
                return false;
            }
            if (!counted(buf.data(), n)) {
                if (error) *error = produced > entry.size ? entry.name + ": longer than its recorded size" : std::string("stopped");
                return false;
            }
            done += n;
        }
    } else {
        uint64_t read = 0;
        const InflateSource source = [&](uint8_t* dst, size_t n) -> size_t {
            const size_t take = size_t(std::min<uint64_t>(n, entry.compressed - read));
            if (!take || !PreadAll(file_, dst, take, at + read)) return 0;
            read += take;
            return take;
        };
        std::string why;
        if (!Inflate(source, counted, &why)) {
            if (error) *error = produced > entry.size ? entry.name + ": longer than its recorded size"
                                : why == "stopped" ? why : entry.name + ": its compressed data is damaged (" + why + ")";
            return false;
        }
    }
    if (produced != entry.size || crc != entry.crc) {
        if (error) *error = entry.name + ": its checksum does not match (the zip is damaged or incomplete)";
        return false;
    }
    return true;
}

bool ZipReader::ReadHead(const Entry& entry, size_t n, std::vector<uint8_t>* out, std::string* error) const {
    out->clear();
    if ((entry.flags & 0x0001u) || (entry.method != 0 && entry.method != 8)) return false;
    uint64_t at = 0;
    if (!DataOffset(entry, &at, error)) return false;
    if (entry.method == 0) {
        out->resize(size_t(std::min<uint64_t>(n, entry.size)));
        return PreadAll(file_, out->data(), out->size(), at);
    }
    uint64_t read = 0;
    const InflateSource source = [&](uint8_t* dst, size_t want) -> size_t {
        const size_t take = size_t(std::min<uint64_t>({uint64_t(want), entry.compressed - read, uint64_t(4096)}));
        if (!take || !PreadAll(file_, dst, take, at + read)) return 0;
        read += take;
        return take;
    };
    const InflateSink sink = [&](const uint8_t* data, size_t len) {
        const size_t take = std::min(len, n - out->size());
        out->insert(out->end(), data, data + take);
        return out->size() < n;   // stop once the head is in
    };
    std::string why;
    Inflate(source, sink, &why);   // "stopped" once enough is in: not an error here
    return !out->empty() || entry.size == 0;
}

bool ZipReader::SafeName(const std::string& name, std::string* clean) {
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return false;
    std::string out, part;
    for (size_t i = 0; i <= name.size(); ++i) {
        const char c = i < name.size() ? name[i] : '/';
        if (c == '/' || c == '\\') {
            if (part == "..") return false;
            if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + part;
            part.clear();
        } else {
            if (static_cast<unsigned char>(c) < 0x20) return false;
            part.push_back(c);
        }
    }
    if (out.empty()) return false;
    *clean = out;
    return true;
}

}  // namespace te::modimport
