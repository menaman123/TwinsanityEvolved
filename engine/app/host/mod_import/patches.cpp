// patches.h implementation.
#include "app/host/mod_import/patches.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "app/host/mod_import/hashes.h"
#include "app/host/mod_import/io.h"

namespace te::modimport {

namespace {

constexpr size_t kChunk = 4u << 20;

// ---- a buffered sequential reader over a patch file ---------------------------------------------------------------------
class Stream {
public:
    bool Open(const std::string& path) {
        if (!f_.OpenRead(path)) return false;
        buf_.resize(1u << 20);
        return true;
    }
    uint64_t Size() const { return f_.Size(); }
    uint64_t Pos() const { return base_ + at_; }
    bool AtEnd() { return Pos() >= f_.Size(); }
    bool Seek(uint64_t pos) {
        if (pos > f_.Size()) return false;
        base_ = pos;
        at_ = len_ = 0;
        return true;
    }
    bool Byte(uint8_t* out) {
        if (at_ == len_ && !Refill()) return false;
        *out = buf_[at_++];
        return true;
    }
    bool Bytes(uint8_t* dst, uint64_t n) {
        while (n) {
            if (at_ == len_ && !Refill()) return false;
            const size_t take = size_t(std::min<uint64_t>(n, len_ - at_));
            if (dst) {
                std::memcpy(dst, buf_.data() + at_, take);
                dst += take;
            }
            at_ += take;
            n -= take;
        }
        return true;
    }
    // VCDIFF's integer: big-endian base 128, the top bit set on every byte but the last
    bool Vcd(uint64_t* out) {
        uint64_t v = 0;
        for (int i = 0; i < 10; ++i) {
            uint8_t b;
            if (!Byte(&b)) return false;
            if (v > (UINT64_MAX >> 7)) return false;
            v = (v << 7) | (b & 0x7Fu);
            if (!(b & 0x80u)) {
                *out = v;
                return true;
            }
        }
        return false;
    }
    // BPS's integer: little-endian base 128 with the +1 shift, the top bit set on the LAST byte
    bool Bps(uint64_t* out) {
        uint64_t v = 0, shift = 1;
        for (int i = 0; i < 10; ++i) {
            uint8_t b;
            if (!Byte(&b)) return false;
            v += uint64_t(b & 0x7Fu) * shift;
            if (b & 0x80u) {
                *out = v;
                return true;
            }
            shift <<= 7;
            v += shift;
        }
        return false;
    }
    io::File& File() { return f_; }

private:
    bool Refill() {
        base_ += len_;
        at_ = len_ = 0;
        if (base_ >= f_.Size()) return false;
        const size_t n = size_t(std::min<uint64_t>(buf_.size(), f_.Size() - base_));
        if (!f_.ReadAt(base_, buf_.data(), n)) return false;
        len_ = n;
        return true;
    }
    io::File f_;
    std::vector<uint8_t> buf_;
    uint64_t base_ = 0;
    size_t at_ = 0, len_ = 0;
};

// ---- the target image: sequential writes, reads back what was written (copies from the target) -------------------------
class Target {
public:
    bool Create(const std::string& path) {
        buf_.reserve(kChunk);
        return f_.Create(path);
    }
    uint64_t Size() const { return flushed_ + buf_.size(); }
    bool Put(const uint8_t* p, size_t n) {
        while (n) {
            const size_t take = std::min(n, kChunk - buf_.size());
            buf_.insert(buf_.end(), p, p + take);
            p += take;
            n -= take;
            if (buf_.size() == kChunk && !Flush()) return false;
        }
        return true;
    }
    bool Flush() {
        if (buf_.empty()) return true;
        if (!f_.WriteAt(flushed_, buf_.data(), buf_.size())) return false;
        flushed_ += buf_.size();
        buf_.clear();
        return true;
    }
    bool ReadBack(uint64_t offset, uint8_t* dst, size_t n) {
        if (offset + n > Size()) return false;
        if (offset < flushed_) {
            const size_t from_file = size_t(std::min<uint64_t>(n, flushed_ - offset));
            if (!f_.ReadAt(offset, dst, from_file)) return false;
            offset += from_file;
            dst += from_file;
            n -= from_file;
        }
        if (n) std::memcpy(dst, buf_.data() + (offset - flushed_), n);
        return true;
    }
    bool Close() { return Flush() && f_.Close(); }

private:
    io::File f_;
    std::vector<uint8_t> buf_;
    uint64_t flushed_ = 0;
};

// A cached window over a file for scattered small reads (VCDIFF copies, BPS source copies)
class Cached {
public:
    explicit Cached(io::File* f) : f_(f) { cache_.resize(kChunk); }
    bool Read(uint64_t offset, uint8_t* dst, size_t n) {
        while (n) {
            if (offset < at_ || offset >= at_ + len_) {
                if (offset >= f_->Size()) return false;
                at_ = offset;
                len_ = size_t(std::min<uint64_t>(cache_.size(), f_->Size() - offset));
                if (!f_->ReadAt(at_, cache_.data(), len_)) { len_ = 0; return false; }
            }
            const size_t take = std::min<size_t>(n, size_t(at_ + len_ - offset));
            std::memcpy(dst, cache_.data() + (offset - at_), take);
            dst += take;
            offset += take;
            n -= take;
        }
        return true;
    }

private:
    io::File* f_;
    std::vector<uint8_t> cache_;
    uint64_t at_ = 0;
    size_t len_ = 0;
};

bool Fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

// ---- VCDIFF -------------------------------------------------------------------------------------------------------------
enum : uint8_t { kNoop = 0, kAdd = 1, kRun = 2, kCopy = 3 };
struct Half { uint8_t type, size, mode; };
struct Code { Half a, b; };

// RFC 3284 5.6: the default code table
const Code* DefaultCodeTable() {
    static Code table[256];
    static bool built = false;
    if (built) return table;
    int i = 0;
    table[i++] = {{kRun, 0, 0}, {kNoop, 0, 0}};
    table[i++] = {{kAdd, 0, 0}, {kNoop, 0, 0}};
    for (int s = 1; s <= 17; ++s) table[i++] = {{kAdd, uint8_t(s), 0}, {kNoop, 0, 0}};
    for (int m = 0; m < 9; ++m) {
        table[i++] = {{kCopy, 0, uint8_t(m)}, {kNoop, 0, 0}};
        for (int s = 4; s <= 18; ++s) table[i++] = {{kCopy, uint8_t(s), uint8_t(m)}, {kNoop, 0, 0}};
    }
    for (int m = 0; m < 9; ++m)
        for (int a = 1; a <= 4; ++a)
            for (int s = 4; s <= (m < 6 ? 6 : 4); ++s) table[i++] = {{kAdd, uint8_t(a), 0}, {kCopy, uint8_t(s), uint8_t(m)}};
    for (int m = 0; m < 9; ++m) table[i++] = {{kCopy, 4, uint8_t(m)}, {kAdd, 1, 0}};
    built = i == 256;
    return table;
}

const char* SecondaryName(uint8_t id) {
    switch (id) {
        case 1: return "djw";
        case 2: return "lzma";
        case 16: return "fgk";
        default: return "an unknown compressor";
    }
}

// A section of a window held in memory with a read cursor
struct Section {
    std::vector<uint8_t> bytes;
    size_t at = 0;
    bool Byte(uint8_t* b) {
        if (at >= bytes.size()) return false;
        *b = bytes[at++];
        return true;
    }
    bool Vcd(uint64_t* out) {
        uint64_t v = 0;
        for (int i = 0; i < 10; ++i) {
            uint8_t b;
            if (!Byte(&b)) return false;
            v = (v << 7) | (b & 0x7Fu);
            if (!(b & 0x80u)) { *out = v; return true; }
        }
        return false;
    }
};

bool VcdiffHeader(Stream& s, PatchHeader* h, std::string* error) {
    uint8_t magic[5];
    if (!s.Bytes(magic, 5) || magic[0] != 0xD6 || magic[1] != 0xC3 || magic[2] != 0xC4)
        return Fail(error, "it is not an xdelta (VCDIFF) patch");
    if (magic[3] != 0x00)
        return Fail(error, "this VCDIFF patch is of a version other than the standard one (its fourth byte is not 0), which cannot be read here");
    const uint8_t ind = magic[4];
    h->format = PatchFormat::Xdelta;
    h->label = "xdelta3 (VCDIFF)";
    if (ind & 0x01) {
        uint8_t id;
        if (!s.Byte(&id)) return Fail(error, "the patch ends inside its header");
        h->secondary = SecondaryName(id);
    }
    if (ind & 0x02)
        return Fail(error, "this xdelta patch carries its own code table, which cannot be read here. Apply it to your disc image "
                           "with xdelta3 or Delta Patcher, then add the patched image instead");
    if (ind & 0x04) {
        uint64_t len;
        if (!s.Vcd(&len) || len > (1u << 20)) return Fail(error, "the patch's header is damaged");
        std::string app(size_t(len), '\0');
        if (!s.Bytes(reinterpret_cast<uint8_t*>(&app[0]), len)) return Fail(error, "the patch ends inside its header");
        h->description = app;
    }
    if (ind & ~0x07u) return Fail(error, "the patch's header has flags this reader does not know");
    return true;
}

bool ApplyVcdiff(const std::string& patch_path, const std::string& source_path, const std::string& target_path,
                 const PatchProgress& progress, PatchHeader* h, std::string* error) {
    Stream s;
    if (!s.Open(patch_path)) return Fail(error, "the patch cannot be read");
    if (!VcdiffHeader(s, h, error)) return false;
    io::File source;
    if (!source.OpenRead(source_path)) return Fail(error, "the disc image cannot be read");
    Target target;
    if (!target.Create(target_path)) return Fail(error, "the temporary image cannot be written");
    Cached src_cache(&source);
    const Code* table = DefaultCodeTable();
    std::vector<uint8_t> window;
    uint64_t windows = 0;
    while (!s.AtEnd()) {
        uint8_t win_ind;
        if (!s.Byte(&win_ind)) break;
        if ((win_ind & 0x03) == 0x03 || (win_ind & ~0x07u)) return Fail(error, "the patch is damaged (a window's flags)");
        uint64_t seg_len = 0, seg_pos = 0;
        const bool from_source = (win_ind & 0x01) != 0, from_target = (win_ind & 0x02) != 0;
        if (from_source || from_target) {
            if (!s.Vcd(&seg_len) || !s.Vcd(&seg_pos)) return Fail(error, "the patch ends inside a window");
            if (from_source && (seg_pos > source.Size() || seg_len > source.Size() - seg_pos))
                return Fail(error, "the patch copies from past the end of the disc image: it was made for a larger image (a different disc)");
            if (from_target && (seg_pos > target.Size() || seg_len > target.Size() - seg_pos))
                return Fail(error, "the patch is damaged (a copy from past the end of the image it makes)");
        }
        uint64_t enc_len, tgt_len, data_len, inst_len, addr_len;
        uint8_t delta_ind;
        if (!s.Vcd(&enc_len) || !s.Vcd(&tgt_len) || !s.Byte(&delta_ind) || !s.Vcd(&data_len) || !s.Vcd(&inst_len) || !s.Vcd(&addr_len))
            return Fail(error, "the patch ends inside a window");
        if (delta_ind)
            return Fail(error, std::string("this xdelta patch was made with xdelta3's secondary compression (") +
                                   (h->secondary.empty() ? "an unknown compressor" : h->secondary) +
                                   "), which cannot be read here. Apply it to your disc image with xdelta3 or Delta Patcher, then add the "
                                   "patched image instead");
        if (tgt_len > (256u << 20) || data_len > (256u << 20) || inst_len > (256u << 20) || addr_len > (256u << 20))
            return Fail(error, "the patch is damaged (a window of more than 256 MB)");
        uint32_t adler = 0;
        const bool checked = (win_ind & 0x04) != 0;
        if (checked) {
            uint8_t b[4];
            if (!s.Bytes(b, 4)) return Fail(error, "the patch ends inside a window");
            adler = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | uint32_t(b[3]);
        }
        Section data, inst, addr;
        data.bytes.resize(size_t(data_len));
        inst.bytes.resize(size_t(inst_len));
        addr.bytes.resize(size_t(addr_len));
        if (!s.Bytes(data.bytes.data(), data_len) || !s.Bytes(inst.bytes.data(), inst_len) || !s.Bytes(addr.bytes.data(), addr_len))
            return Fail(error, "the patch ends inside a window");
        window.assign(size_t(tgt_len), 0);
        uint64_t near[4] = {0, 0, 0, 0};
        uint64_t same[3 * 256] = {};
        int next_near = 0;
        size_t t = 0;   // the position in the target window
        auto seg_bytes = [&](uint64_t a, uint8_t* out, size_t n) -> bool {
            if (from_source) return src_cache.Read(seg_pos + a, out, n);
            return target.ReadBack(seg_pos + a, out, n);
        };
        while (inst.at < inst.bytes.size()) {
            uint8_t code_index;
            inst.Byte(&code_index);
            const Code& code = table[code_index];
            for (const Half* half : {&code.a, &code.b}) {
                if (half->type == kNoop) continue;
                uint64_t size = half->size;
                if (size == 0 && !inst.Vcd(&size)) return Fail(error, "the patch is damaged (an instruction's size)");
                if (size > tgt_len - t) return Fail(error, "the patch is damaged (an instruction past its window)");
                if (half->type == kAdd) {
                    if (size > data.bytes.size() - data.at) return Fail(error, "the patch is damaged (an add past its data)");
                    std::memcpy(window.data() + t, data.bytes.data() + data.at, size_t(size));
                    data.at += size_t(size);
                    t += size_t(size);
                } else if (half->type == kRun) {
                    uint8_t b;
                    if (!data.Byte(&b)) return Fail(error, "the patch is damaged (a run past its data)");
                    std::memset(window.data() + t, b, size_t(size));
                    t += size_t(size);
                } else {
                    const uint64_t here = seg_len + t;
                    uint64_t a = 0;
                    const int mode = half->mode;
                    if (mode == 0) {
                        if (!addr.Vcd(&a)) return Fail(error, "the patch is damaged (an address)");
                    } else if (mode == 1) {
                        uint64_t d;
                        if (!addr.Vcd(&d) || d > here) return Fail(error, "the patch is damaged (an address)");
                        a = here - d;
                    } else if (mode < 6) {
                        uint64_t d;
                        if (!addr.Vcd(&d)) return Fail(error, "the patch is damaged (an address)");
                        a = near[mode - 2] + d;
                    } else {
                        uint8_t b;
                        if (!addr.Byte(&b)) return Fail(error, "the patch is damaged (an address)");
                        a = same[(mode - 6) * 256 + b];
                    }
                    near[next_near] = a;
                    next_near = (next_near + 1) % 4;
                    same[a % (3 * 256)] = a;
                    if (a >= here) return Fail(error, "the patch is damaged (a copy from ahead of itself)");
                    uint64_t left = size;
                    // the part in the source segment
                    if (a < seg_len) {
                        const size_t n = size_t(std::min<uint64_t>(left, seg_len - a));
                        if (!seg_bytes(a, window.data() + t, n)) return Fail(error, "the patch copies bytes the image does not have");
                        t += n;
                        a += n;
                        left -= n;
                    }
                    // the part in this window (may overlap what it writes: byte by byte, forwards)
                    for (; left; --left, ++a) window[t++] = window[size_t(a - seg_len)];
                }
            }
        }
        if (t != tgt_len || data.at != data.bytes.size() || addr.at != addr.bytes.size())
            return Fail(error, "the patch is damaged (a window does not add up)");
        if (checked && Adler32Update(1, window.data(), window.size()) != adler)
            return Fail(error, "the patched image does not check out: the patch was made for a different disc image than yours");
        if (!target.Put(window.data(), window.size())) return Fail(error, "the temporary image cannot be written (is the disk full?)");
        ++windows;
        if (progress && !progress(double(s.Pos()) / double(std::max<uint64_t>(1, s.Size())))) return Fail(error, "stopped");
    }
    h->records = windows;
    h->target_bytes = target.Size();
    if (!target.Close()) return Fail(error, "the temporary image cannot be written (is the disk full?)");
    if (windows == 0) return Fail(error, "the patch has no data");
    return true;
}

// ---- PPF ----------------------------------------------------------------------------------------------------------------
struct PpfLayout {
    int version = 0;          // 1, 2, 3
    uint64_t records_at = 0, records_end = 0;
    bool undo = false;
    bool block = false;
    uint64_t block_offset = 0x9320;   // where the validation block's bytes are in the image
    uint32_t image_bytes = 0;         // 2.0
};

bool PpfParse(io::File& f, PatchHeader* h, PpfLayout* L, std::string* error) {
    uint8_t head[60] = {};
    if (f.Size() < 56 || !f.ReadAt(0, head, size_t(std::min<uint64_t>(60, f.Size()))))
        return Fail(error, "it is not a PPF patch (too short)");
    if (std::memcmp(head, "PPF", 3) != 0) return Fail(error, "it is not a PPF patch");
    if (head[3] == '1' && head[4] == '0') L->version = 1;
    else if (head[3] == '2' && head[4] == '0') L->version = 2;
    else if (head[3] == '3' && head[4] == '0') L->version = 3;
    else return Fail(error, "this PPF patch is of a version this reader does not know");
    h->format = PatchFormat::Ppf;
    h->label = "PPF " + std::to_string(L->version) + ".0";
    std::string desc(reinterpret_cast<const char*>(head + 6), 50);
    while (!desc.empty() && (desc.back() == ' ' || desc.back() == '\0')) desc.pop_back();
    h->description = desc;
    uint64_t end = f.Size();
    if (L->version == 1) {
        L->records_at = 56;
    } else if (L->version == 2) {
        if (f.Size() < 1084) return Fail(error, "the PPF patch is damaged (shorter than its header)");
        L->image_bytes = uint32_t(head[56]) | uint32_t(head[57]) << 8 | uint32_t(head[58]) << 16 | uint32_t(head[59]) << 24;
        h->source_bytes = L->image_bytes;
        L->block = true;
        L->records_at = 1084;
        uint8_t tail[8];
        if (f.Size() >= 1084 + 8 && f.ReadAt(f.Size() - 8, tail, 8) && std::memcmp(tail, ".DIZ", 4) == 0) {
            const uint32_t diz = uint32_t(tail[4]) | uint32_t(tail[5]) << 8 | uint32_t(tail[6]) << 16 | uint32_t(tail[7]) << 24;
            if (uint64_t(diz) + 18 + 16 + 4 > f.Size() - 1084) return Fail(error, "the PPF patch is damaged (its description trailer)");
            end = f.Size() - diz - (18 + 16 + 4);
        }
    } else {
        const uint8_t imagetype = head[56];
        L->block = head[57] != 0;
        L->undo = head[58] != 0;
        L->block_offset = imagetype ? 0x80A0 : 0x9320;
        L->records_at = L->block ? 1084 : 60;
        if (f.Size() < L->records_at) return Fail(error, "the PPF patch is damaged (shorter than its header)");
        uint8_t tail[6];
        if (f.Size() >= L->records_at + 6 && f.ReadAt(f.Size() - 6, tail, 6) && std::memcmp(tail, ".DIZ", 4) == 0) {
            const uint32_t diz = uint32_t(tail[4]) | uint32_t(tail[5]) << 8;
            if (uint64_t(diz) + 18 + 16 + 2 > f.Size() - L->records_at) return Fail(error, "the PPF patch is damaged (its description trailer)");
            end = f.Size() - diz - (18 + 16 + 2);
        }
    }
    h->validation_block = L->block;
    L->records_end = end;
    return true;
}

bool PpfCheck(io::File& f, const PpfLayout& L, const std::string& source_path, std::string* error) {
    io::File src;
    if (!src.OpenRead(source_path)) return Fail(error, "the disc image cannot be read");
    if (L.version == 2 && L.image_bytes && src.Size() != L.image_bytes)
        return Fail(error, "this PPF patch was made for a disc image of " + std::to_string(L.image_bytes) + " bytes; yours has " +
                               std::to_string(src.Size()) + ": it is for a different disc");
    if (L.block) {
        uint8_t want[1024], have[1024];
        if (!f.ReadAt(60, want, sizeof want)) return Fail(error, "the PPF patch is damaged (its validation block)");
        if (!src.ReadAt(L.block_offset, have, sizeof have) || std::memcmp(want, have, sizeof want) != 0)
            return Fail(error, "this PPF patch was made for a different disc image than yours (its check of the image fails)");
    }
    return true;
}

bool ApplyPpf(const std::string& patch_path, const std::string& source_path, const std::string& target_path,
              const PatchProgress& progress, PatchHeader* h, std::string* error) {
    io::File f;
    if (!f.OpenRead(patch_path)) return Fail(error, "the patch cannot be read");
    PpfLayout L;
    if (!PpfParse(f, h, &L, error)) return false;
    if (!PpfCheck(f, L, source_path, error)) return false;
    // the target: a copy of the disc, then every record written in place
    io::File src;
    if (!src.OpenRead(source_path)) return Fail(error, "the disc image cannot be read");
    io::File out;
    if (!out.Create(target_path)) return Fail(error, "the temporary image cannot be written");
    std::vector<uint8_t> buf(kChunk);
    for (uint64_t done = 0; done < src.Size();) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), src.Size() - done));
        if (!src.ReadAt(done, buf.data(), n) || !out.WriteAt(done, buf.data(), n))
            return Fail(error, "the temporary image cannot be written (is the disk full?)");
        done += n;
        if (progress && !progress(0.9 * double(done) / double(src.Size()))) return Fail(error, "stopped");
    }
    const size_t offset_bytes = L.version == 3 ? 8 : 4;
    uint64_t at = L.records_at, records = 0;
    std::vector<uint8_t> rec(255 + 8);
    while (at < L.records_end) {
        if (L.records_end - at < offset_bytes + 1) return Fail(error, "the PPF patch is damaged (a record is cut short)");
        uint8_t hdr[9];
        if (!f.ReadAt(at, hdr, offset_bytes + 1)) return Fail(error, "the PPF patch does not read");
        uint64_t offset = 0;
        for (size_t i = 0; i < offset_bytes; ++i) offset |= uint64_t(hdr[i]) << (8 * i);
        const size_t count = hdr[offset_bytes];
        at += offset_bytes + 1;
        if (L.records_end - at < count + (L.undo ? count : 0)) return Fail(error, "the PPF patch is damaged (a record is cut short)");
        if (count && !f.ReadAt(at, rec.data(), count)) return Fail(error, "the PPF patch does not read");
        at += count + (L.undo ? count : 0);
        if (offset + count > src.Size() + (64u << 20))
            return Fail(error, "this PPF patch writes far past the end of your disc image: it is for a different disc");
        if (count && !out.WriteAt(offset, rec.data(), count)) return Fail(error, "the temporary image cannot be written");
        ++records;
    }
    h->records = records;
    h->target_bytes = out.Size();
    if (!out.Close()) return Fail(error, "the temporary image cannot be written (is the disk full?)");
    if (progress) progress(1.0);
    return true;
}

// ---- BPS ----------------------------------------------------------------------------------------------------------------
bool BpsParse(Stream& s, PatchHeader* h, uint64_t* actions_at, std::string* error) {
    uint8_t magic[4];
    if (s.Size() < 4 + 3 + 12 || !s.Bytes(magic, 4) || std::memcmp(magic, "BPS1", 4) != 0) return Fail(error, "it is not a BPS patch");
    uint64_t meta = 0;
    if (!s.Bps(&h->source_bytes) || !s.Bps(&h->target_bytes) || !s.Bps(&meta) || meta > (1u << 20) || s.Pos() + meta > s.Size() - 12)
        return Fail(error, "the BPS patch is damaged (its header)");
    std::string text(size_t(meta), '\0');
    if (meta && !s.Bytes(reinterpret_cast<uint8_t*>(&text[0]), meta)) return Fail(error, "the BPS patch is damaged (its header)");
    h->format = PatchFormat::Bps;
    h->label = "BPS";
    h->description = text;
    *actions_at = s.Pos();
    uint8_t foot[12];
    if (!s.File().ReadAt(s.Size() - 12, foot, 12)) return Fail(error, "the BPS patch does not read");
    auto le = [&](int at) { return uint32_t(foot[at]) | uint32_t(foot[at + 1]) << 8 | uint32_t(foot[at + 2]) << 16 | uint32_t(foot[at + 3]) << 24; };
    h->source_crc = le(0);
    h->target_crc = le(4);
    h->source_crc_known = true;
    return true;
}

bool FileCrc(io::File& f, uint64_t bytes, uint32_t* crc, const PatchProgress& progress, double from, double span) {
    std::vector<uint8_t> buf(kChunk);
    uint32_t c = 0;
    for (uint64_t done = 0; done < bytes;) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), bytes - done));
        if (!f.ReadAt(done, buf.data(), n)) return false;
        c = Crc32Update(c, buf.data(), n);
        done += n;
        if (progress && !progress(from + span * double(done) / double(bytes))) return false;
    }
    *crc = c;
    return true;
}

bool ApplyBps(const std::string& patch_path, const std::string& source_path, const std::string& target_path,
              const PatchProgress& progress, PatchHeader* h, std::string* error) {
    Stream s;
    if (!s.Open(patch_path)) return Fail(error, "the patch cannot be read");
    uint64_t actions_at = 0;
    if (!BpsParse(s, h, &actions_at, error)) return false;
    // the patch's own checksum (of every byte but its last four)
    {
        uint32_t crc = 0;
        if (!FileCrc(s.File(), s.Size() - 4, &crc, nullptr, 0, 0)) return Fail(error, "the BPS patch does not read");
        uint8_t b[4];
        if (!s.File().ReadAt(s.Size() - 4, b, 4)) return Fail(error, "the BPS patch does not read");
        const uint32_t want = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
        if (crc != want) return Fail(error, "the BPS patch is damaged (its checksum does not match)");
    }
    io::File source;
    if (!source.OpenRead(source_path)) return Fail(error, "the disc image cannot be read");
    if (source.Size() != h->source_bytes)
        return Fail(error, "this BPS patch was made for a disc image of " + std::to_string(h->source_bytes) + " bytes; yours has " +
                               std::to_string(source.Size()) + ": it is for a different disc");
    {
        uint32_t crc = 0;
        bool stopped = false;
        const PatchProgress p = [&](double f) {
            if (progress && !progress(f)) { stopped = true; return false; }
            return true;
        };
        if (!FileCrc(source, source.Size(), &crc, p, 0.0, 0.3)) return Fail(error, stopped ? "stopped" : "the disc image cannot be read");
        if (crc != h->source_crc) return Fail(error, "this BPS patch was made for a different disc image than yours (its checksum of the image differs)");
    }
    if (!s.Seek(actions_at)) return Fail(error, "the BPS patch does not read");
    Target target;
    if (!target.Create(target_path)) return Fail(error, "the temporary image cannot be written");
    Cached src(&source);
    const uint64_t actions_end = s.Size() - 12;
    uint64_t out = 0;   // bytes of the target written
    int64_t source_rel = 0, target_rel = 0;
    std::vector<uint8_t> buf(kChunk);
    uint32_t tcrc = 0;
    uint64_t next_report = 0;
    while (s.Pos() < actions_end) {
        uint64_t data;
        if (!s.Bps(&data)) return Fail(error, "the BPS patch is damaged (an action)");
        const uint64_t command = data & 3, length = (data >> 2) + 1;
        if (length > h->target_bytes - out) return Fail(error, "the BPS patch is damaged (an action past the image it makes)");
        if (command == 0) {   // SourceRead: the source's bytes at the same position
            if (out + length > source.Size()) return Fail(error, "the BPS patch is damaged (a read past the source)");
            for (uint64_t k = 0; k < length;) {
                const size_t n = size_t(std::min<uint64_t>(buf.size(), length - k));
                if (!src.Read(out + k, buf.data(), n) || !target.Put(buf.data(), n)) return Fail(error, "the temporary image cannot be written");
                tcrc = Crc32Update(tcrc, buf.data(), n);
                k += n;
            }
        } else if (command == 1) {   // TargetRead: bytes from the patch
            for (uint64_t k = 0; k < length;) {
                const size_t n = size_t(std::min<uint64_t>(buf.size(), length - k));
                if (s.Pos() + n > actions_end || !s.Bytes(buf.data(), n)) return Fail(error, "the BPS patch is damaged (a read past its end)");
                if (!target.Put(buf.data(), n)) return Fail(error, "the temporary image cannot be written");
                tcrc = Crc32Update(tcrc, buf.data(), n);
                k += n;
            }
        } else {
            uint64_t d;
            if (!s.Bps(&d)) return Fail(error, "the BPS patch is damaged (an offset)");
            const int64_t delta = (d & 1) ? -int64_t(d >> 1) : int64_t(d >> 1);
            if (command == 2) {   // SourceCopy
                source_rel += delta;
                if (source_rel < 0 || uint64_t(source_rel) + length > source.Size()) return Fail(error, "the BPS patch is damaged (a copy past the source)");
                for (uint64_t k = 0; k < length;) {
                    const size_t n = size_t(std::min<uint64_t>(buf.size(), length - k));
                    if (!src.Read(uint64_t(source_rel) + k, buf.data(), n) || !target.Put(buf.data(), n))
                        return Fail(error, "the temporary image cannot be written");
                    tcrc = Crc32Update(tcrc, buf.data(), n);
                    k += n;
                }
                source_rel += int64_t(length);
            } else {   // TargetCopy: from the target already made (overlapping: in steps of the distance)
                target_rel += delta;
                if (target_rel < 0 || uint64_t(target_rel) >= out) return Fail(error, "the BPS patch is damaged (a copy from ahead of itself)");
                for (uint64_t k = 0; k < length;) {
                    const uint64_t distance = out - uint64_t(target_rel);   // the copy's source runs this far behind
                    const size_t n = size_t(std::min<uint64_t>({uint64_t(buf.size()), length - k, distance}));
                    if (!target.ReadBack(uint64_t(target_rel) + k, buf.data(), n) || !target.Put(buf.data(), n))
                        return Fail(error, "the temporary image cannot be written");
                    tcrc = Crc32Update(tcrc, buf.data(), n);
                    k += n;
                }
                target_rel += int64_t(length);
            }
        }
        out += length;
        if (out >= next_report) {
            next_report = out + (64u << 20);
            if (progress && !progress(0.3 + 0.7 * double(out) / double(std::max<uint64_t>(1, h->target_bytes)))) return Fail(error, "stopped");
        }
    }
    if (out != h->target_bytes) return Fail(error, "the BPS patch is damaged (it makes fewer bytes than it says)");
    if (!target.Close()) return Fail(error, "the temporary image cannot be written (is the disk full?)");
    if (tcrc != h->target_crc) return Fail(error, "the patched image does not check out (its checksum differs from the patch's)");
    return true;
}

}  // namespace

bool ReadPatchHeader(PatchFormat format, const std::string& patch_path, PatchHeader* out, std::string* error) {
    *out = PatchHeader{};
    if (format == PatchFormat::Xdelta) {
        Stream s;
        if (!s.Open(patch_path)) return Fail(error, "the patch cannot be read");
        return VcdiffHeader(s, out, error);
    }
    if (format == PatchFormat::Ppf) {
        io::File f;
        if (!f.OpenRead(patch_path)) return Fail(error, "the patch cannot be read");
        PpfLayout L;
        return PpfParse(f, out, &L, error);
    }
    Stream s;
    if (!s.Open(patch_path)) return Fail(error, "the patch cannot be read");
    uint64_t at = 0;
    return BpsParse(s, out, &at, error);
}

bool CheckPatchSource(const PatchHeader& header, const std::string& patch_path, const std::string& source_path, std::string* error) {
    if (header.format == PatchFormat::Ppf) {
        io::File f;
        if (!f.OpenRead(patch_path)) return Fail(error, "the patch cannot be read");
        PatchHeader h;
        PpfLayout L;
        if (!PpfParse(f, &h, &L, error)) return false;
        return PpfCheck(f, L, source_path, error);
    }
    if (header.format == PatchFormat::Bps) {
        io::File src;
        if (!src.OpenRead(source_path)) return Fail(error, "the disc image cannot be read");
        if (src.Size() != header.source_bytes)
            return Fail(error, "this BPS patch was made for a disc image of " + std::to_string(header.source_bytes) + " bytes; yours has " +
                                   std::to_string(src.Size()) + ": it is for a different disc");
    }
    return true;
}

bool ApplyPatch(PatchFormat format, const std::string& patch_path, const std::string& source_path, const std::string& target_path,
                const PatchProgress& progress, PatchHeader* header, std::string* error) {
    PatchHeader local;
    PatchHeader* h = header ? header : &local;
    *h = PatchHeader{};
    switch (format) {
        case PatchFormat::Xdelta: return ApplyVcdiff(patch_path, source_path, target_path, progress, h, error);
        case PatchFormat::Ppf: return ApplyPpf(patch_path, source_path, target_path, progress, h, error);
        case PatchFormat::Bps: return ApplyBps(patch_path, source_path, target_path, progress, h, error);
    }
    return Fail(error, "an unknown patch format");
}

}  // namespace te::modimport
