// archive.h implementation.
#include "app/host/mod_import/archive.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "engine/cdvd/iso_fs.h"

namespace te::modimport {

namespace {

constexpr size_t kBlock = 1u << 20;
// An equal run shorter than this between two changes is stored with them: a chunk with bytes inserted matches its old
// version at the same offsets only by coincidence (zero runs, repeated records), and such runs would only cost recipe lines
constexpr uint64_t kMinBaseRun = 4096;
constexpr uint64_t kTickEvery = 4u << 20;

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

}  // namespace

bool ImageFileSource::Read(uint64_t offset, uint8_t* dst, size_t n) {
    if (offset > size_ || n > size_ - offset) return false;
    return n == 0 || iso_.ReadBytes(uint64_t(lba_) * IsoFs::kSectorSize + offset, n, dst) == n;
}

bool MemorySource::Read(uint64_t offset, uint8_t* dst, size_t n) {
    if (offset > bytes_.size() || n > bytes_.size() - offset) return false;
    if (n) std::memcpy(dst, bytes_.data() + offset, n);
    return true;
}

void CompositeSource::Append(ByteSource* src, uint64_t offset, uint64_t length) {
    if (!length) return;
    pieces_.push_back({size_, length, src, offset});
    size_ += length;
}

void CompositeSource::AppendZeros(uint64_t length) { Append(nullptr, 0, length); }

size_t CompositeSource::PieceAt(uint64_t offset) const {
    const auto it = std::upper_bound(pieces_.begin(), pieces_.end(), offset,
                                     [](uint64_t o, const Piece& p) { return o < p.start + p.length; });
    return size_t(it - pieces_.begin());
}

bool CompositeSource::Read(uint64_t offset, uint8_t* dst, size_t n) {
    if (offset > size_ || n > size_ - offset) return false;
    for (size_t i = PieceAt(offset); n; ++i) {
        if (i >= pieces_.size()) return false;
        const Piece& p = pieces_[i];
        const uint64_t in = offset - p.start;
        const size_t take = size_t(std::min<uint64_t>(n, p.length - in));
        if (p.src) {
            if (!p.src->Read(p.at + in, dst, take)) return false;
        } else {
            std::memset(dst, 0, take);
        }
        dst += take;
        offset += take;
        n -= take;
    }
    return true;
}

bool CompositeSource::Maps(uint64_t offset, uint64_t n, const ByteSource* other, uint64_t* other_offset) const {
    const size_t i = PieceAt(offset);
    if (i >= pieces_.size() || !other) return false;
    const Piece& p = pieces_[i];
    if (!p.src || !(p.src == other || p.src->SameAs(other)) || offset - p.start + n > p.length) return false;
    *other_offset = p.at + (offset - p.start);
    return true;
}

void Recipe::Base(uint64_t offset, uint64_t length) {
    if (!length) return;
    if (!segs_.empty() && segs_.back().kind == Segment::Kind::Base && segs_.back().base_offset + segs_.back().length == offset) {
        segs_.back().length += length;
    } else {
        Segment s;
        s.kind = Segment::Kind::Base;
        s.base_offset = offset;
        s.length = length;
        segs_.push_back(s);
    }
    size_ += length;
    base_ += length;
}

void Recipe::Data(ByteSource* src, uint64_t offset, uint64_t length) {
    if (!length) return;
    if (!segs_.empty() && segs_.back().kind == Segment::Kind::Data && segs_.back().src == src &&
        segs_.back().src_offset + segs_.back().length == offset) {
        segs_.back().length += length;
    } else {
        Segment s;
        s.kind = Segment::Kind::Data;
        s.src = src;
        s.src_offset = offset;
        s.length = length;
        segs_.push_back(s);
    }
    size_ += length;
    data_ += length;
}

void Recipe::Zero(uint64_t length) {
    if (!length) return;
    if (!segs_.empty() && segs_.back().kind == Segment::Kind::Zero) {
        segs_.back().length += length;
    } else {
        Segment s;
        s.kind = Segment::Kind::Zero;
        s.length = length;
        segs_.push_back(s);
    }
    size_ += length;
}

void Recipe::Append(const Recipe& other) {
    for (const Segment& s : other.segs_) {
        switch (s.kind) {
            case Segment::Kind::Base: Base(s.base_offset, s.length); break;
            case Segment::Kind::Data: Data(s.src, s.src_offset, s.length); break;
            case Segment::Kind::Zero: Zero(s.length); break;
        }
    }
}

bool SameOffsetDiff(ByteSource& target, uint64_t t_off, uint64_t len, ByteSource& base, uint64_t b_off, uint64_t b_len,
                    Recipe* out, const DiffControl& control) {
    const uint64_t common = std::min(len, b_len);
    // a piece taken whole from the base at the same place: no comparison
    uint64_t mapped = 0;
    if (common && target.Maps(t_off, common, &base, &mapped) && mapped == b_off) {
        out->Base(b_off, common);
        if (len > common) out->Data(&target, t_off + common, len - common);
        return !control.tick || control.tick(len);
    }
    std::vector<uint8_t> a(kBlock), b(kBlock);
    uint64_t done = 0, since_tick = 0;
    // the pending run: equal (base) or different (data), extended across blocks
    bool pending_equal = false;
    uint64_t pending_from = 0, pending_len = 0;
    auto emit = [&](bool equal, uint64_t from, uint64_t n) {
        if (equal) out->Base(b_off + from, n);
        else out->Data(&target, t_off + from, n);
    };
    auto push = [&](bool equal, uint64_t from, uint64_t n) {
        if (pending_len && pending_equal == equal) { pending_len += n; return; }
        if (pending_len) {
            // a short equal run between two changes is stored with them
            if (pending_equal && pending_len < kMinBaseRun && pending_from != 0) pending_equal = false;
            emit(pending_equal, pending_from, pending_len);
        }
        pending_equal = equal;
        pending_from = from;
        pending_len = n;
    };
    while (done < common) {
        const size_t n = size_t(std::min<uint64_t>(kBlock, common - done));
        if (!target.Read(t_off + done, a.data(), n) || !base.Read(b_off + done, b.data(), n)) return false;
        if (std::memcmp(a.data(), b.data(), n) == 0) {
            push(true, done, n);
        } else {
            size_t i = 0;
            while (i < n) {
                size_t j = i;
                if (a[i] == b[i]) {
                    // equal bytes: in 8-byte steps while they match
                    while (j + 8 <= n && std::memcmp(&a[j], &b[j], 8) == 0) j += 8;
                    while (j < n && a[j] == b[j]) ++j;
                    push(true, done + i, j - i);
                } else {
                    while (j < n && a[j] != b[j]) ++j;
                    push(false, done + i, j - i);
                }
                i = j;
            }
        }
        done += n;
        since_tick += n;
        if (since_tick >= kTickEvery) {
            since_tick = 0;
            if (control.tick && !control.tick(done)) return false;
        }
    }
    if (pending_len) {
        if (pending_equal && pending_len < kMinBaseRun && pending_from != 0 && len > common) pending_equal = false;
        emit(pending_equal, pending_from, pending_len);
    }
    if (len > common) out->Data(&target, t_off + common, len - common);
    return true;
}

bool ParseBh(const std::vector<uint8_t>& bytes, BhIndex* out, std::string* error) {
    *out = BhIndex{};
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (bytes.size() < 4) return fail("the archive index is shorter than its header");
    out->header = int32_t(Le32(bytes.data()));
    size_t at = 4;
    while (at < bytes.size()) {
        if (bytes.size() - at < 4) return fail("the archive index ends inside an entry");
        const int32_t n = int32_t(Le32(&bytes[at]));
        at += 4;
        if (n <= 0 || n > 0x3FF || bytes.size() - at < size_t(n) + 8) return fail("the archive index has a damaged entry");
        BhEntry e;
        e.name.assign(reinterpret_cast<const char*>(&bytes[at]), size_t(n));
        at += size_t(n);
        e.offset = Le32(&bytes[at]);
        e.size = Le32(&bytes[at + 4]);
        at += 8;
        out->entries.push_back(std::move(e));
    }
    if (out->entries.empty()) return fail("the archive index has no entries");
    return true;
}

std::vector<uint8_t> WriteBh(const BhIndex& index) {
    std::vector<uint8_t> out;
    Put32(out, uint32_t(index.header));
    for (const BhEntry& e : index.entries) {
        Put32(out, uint32_t(e.name.size()));
        out.insert(out.end(), e.name.begin(), e.name.end());
        Put32(out, e.offset);
        Put32(out, e.size);
    }
    return out;
}

std::string ArchiveKey(const std::string& name) {
    std::string k = name;
    for (char& c : k) {
        if (c == '/') c = '\\';
        else if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    }
    return k;
}

bool ArchiveDiff(const BhIndex& target_bh, ByteSource& target_bd, const BhIndex& base_bh, ByteSource& base_bd, Recipe* out,
                 ArchiveDiffStats* stats, const DiffControl& control) {
    std::map<std::string, const BhEntry*> base_by_key;
    for (const BhEntry& e : base_bh.entries) base_by_key[ArchiveKey(e.name)] = &e;   // a duplicate name: the later wins (as the game)
    std::map<std::string, bool> seen;
    std::vector<const BhEntry*> order;
    for (const BhEntry& e : target_bh.entries) order.push_back(&e);
    std::stable_sort(order.begin(), order.end(), [](const BhEntry* a, const BhEntry* b) { return a->offset < b->offset; });
    const uint64_t size = target_bd.Size();
    uint64_t cursor = 0, done = 0;
    DiffControl inner;
    inner.tick = [&](uint64_t) { return !control.tick || control.tick(done); };
    for (const BhEntry* e : order) {
        const uint64_t begin = e->offset, end = uint64_t(e->offset) + e->size;
        if (end > size) {
            if (stats) stats->changed_names.push_back(e->name + " (past the end of the archive)");
            continue;
        }
        if (begin > cursor) {   // bytes no entry claims: kept as they are
            out->Data(&target_bd, cursor, begin - cursor);
            cursor = begin;
        }
        if (end <= cursor) continue;   // inside an entry already written (two entries sharing bytes)
        const uint64_t from = std::max<uint64_t>(begin, cursor);
        const std::string key = ArchiveKey(e->name);
        seen[key] = true;
        const auto b = base_by_key.find(key);
        const uint64_t data_before = out->DataBytes();
        if (b == base_by_key.end()) {
            out->Data(&target_bd, from, end - from);
            if (stats) { ++stats->added; stats->added_names.push_back(e->name); }
        } else {
            const uint64_t skip = from - begin;
            const uint64_t b_begin = uint64_t(b->second->offset) + std::min<uint64_t>(skip, b->second->size);
            const uint64_t b_len = b->second->size > skip ? b->second->size - skip : 0;
            Recipe part;
            if (!SameOffsetDiff(target_bd, from, end - from, base_bd, b_begin, b_len, &part, inner)) return false;
            // a changed entry the comparison saves less than a quarter of: stored whole (one segment)
            if (part.DataBytes() * 4 > (end - from) * 3) out->Data(&target_bd, from, end - from);
            else out->Append(part);
            const bool same = out->DataBytes() == data_before && e->size == b->second->size;
            if (stats) {
                if (same) ++stats->same;
                else { ++stats->changed; stats->changed_names.push_back(e->name); }
            }
        }
        cursor = end;
        done += end - from;
    }
    if (cursor < size) out->Data(&target_bd, cursor, size - cursor);
    if (stats)
        for (const BhEntry& e : base_bh.entries)
            if (!seen.count(ArchiveKey(e.name))) { ++stats->removed; stats->removed_names.push_back(e.name); }
    return true;
}

}  // namespace te::modimport
