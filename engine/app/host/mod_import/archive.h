#pragma once
// The pieces a mod folder's files are made of, and how a changed disc file becomes one:
//   * ByteSource: bytes to read -- a file on a disc image (the player's disc, a patched image), a host file (a crate's
//     layer file, a folder's file), or a composition of such pieces (a rebuilt archive before it is written);
//   * Recipe: a file of the mod as `base` ranges of the disc's version, `data` ranges stored in the mod's payload and
//     `zero` runs (written out as a <DISC PATH>.ranges recipe and one data/<DISC PATH>.bin payload);
//   * SameOffsetDiff: a changed file against the disc's -- equal runs become `base`, the rest `data`;
//   * ArchiveDiff: a changed CRASH6/CRASH.BD against the disc's through both CRASH.BH indexes, entry by name, so an
//     archive whose entries moved (a grown or added entry pushes the later ones) is still mostly `base` ranges;
//   * the CRASH.BH index: one little-endian 32-bit word the game skips, then {s32 name length, name, u32 offset,
//     u32 size} to the end (game/src/game/archive.cpp ArchiveSectionReader::Read; the game upper-cases the names and
//     keeps them sorted itself, so the index's order is free).
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/host/mod_import/io.h"

class IsoFs;

namespace te::modimport {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t Size() const = 0;
    virtual bool Read(uint64_t offset, uint8_t* dst, size_t n) = 0;
    // When [offset, offset+n) of this source is exactly `other`'s bytes at *other_offset (a composition's piece taken
    // whole from it): the diff then needs no comparison
    virtual bool Maps(uint64_t offset, uint64_t n, const ByteSource* other, uint64_t* other_offset) const {
        (void)offset; (void)n; (void)other; (void)other_offset;
        return false;
    }
    // What this source reads, as an identity: two sources over one file of one image have the same key
    struct Key { const void* owner; uint64_t at, size; };
    virtual Key Identity() const { return Key{this, 0, 0}; }
    bool SameAs(const ByteSource* other) const {
        if (!other) return false;
        const Key a = Identity(), b = other->Identity();
        return a.owner == b.owner && a.at == b.at && a.size == b.size;
    }
};

// A file of a disc image: `size` bytes from sector `lba`
class ImageFileSource final : public ByteSource {
public:
    ImageFileSource(const IsoFs& iso, uint32_t lba, uint64_t size) : iso_(iso), lba_(lba), size_(size) {}
    uint64_t Size() const override { return size_; }
    bool Read(uint64_t offset, uint8_t* dst, size_t n) override;
    Key Identity() const override { return Key{&iso_, lba_, size_}; }
    uint32_t Lba() const { return lba_; }

private:
    const IsoFs& iso_;
    uint32_t lba_;
    uint64_t size_;
};

class HostFileSource final : public ByteSource {
public:
    bool Open(const std::string& path) { path_ = path; return f_.OpenRead(path); }
    uint64_t Size() const override { return f_.Size(); }
    bool Read(uint64_t offset, uint8_t* dst, size_t n) override { return n == 0 || f_.ReadAt(offset, dst, n); }
    const std::string& HostPath() const { return path_; }

private:
    io::File f_;
    std::string path_;
};

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    uint64_t Size() const override { return bytes_.size(); }
    bool Read(uint64_t offset, uint8_t* dst, size_t n) override;
    const std::vector<uint8_t>& Bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

// Pieces of other sources back to back (a rebuilt archive): pieces are appended in order
class CompositeSource final : public ByteSource {
public:
    void Append(ByteSource* src, uint64_t offset, uint64_t length);
    void AppendZeros(uint64_t length);
    uint64_t Size() const override { return size_; }
    bool Read(uint64_t offset, uint8_t* dst, size_t n) override;
    bool Maps(uint64_t offset, uint64_t n, const ByteSource* other, uint64_t* other_offset) const override;

private:
    struct Piece { uint64_t start, length; ByteSource* src; uint64_t at; };
    size_t PieceAt(uint64_t offset) const;
    std::vector<Piece> pieces_;
    uint64_t size_ = 0;
};

// ---- recipes ------------------------------------------------------------------------------------------------------------
struct Segment {
    enum class Kind : uint8_t { Base, Data, Zero };
    Kind kind = Kind::Zero;
    uint64_t length = 0;
    uint64_t base_offset = 0;     // Base: in the disc's file
    ByteSource* src = nullptr;    // Data: where the bytes come from
    uint64_t src_offset = 0;
};

class Recipe {
public:
    void Base(uint64_t offset, uint64_t length);
    void Data(ByteSource* src, uint64_t offset, uint64_t length);
    void Zero(uint64_t length);
    void Append(const Recipe& other);   // another recipe's segments after these (merged where they meet)
    uint64_t Size() const { return size_; }
    uint64_t DataBytes() const { return data_; }
    uint64_t BaseBytes() const { return base_; }
    const std::vector<Segment>& Segments() const { return segs_; }

private:
    std::vector<Segment> segs_;
    uint64_t size_ = 0, data_ = 0, base_ = 0;
};

struct DiffControl {
    std::function<bool(uint64_t bytes_done)> tick;   // false = stop (progress + cancel), called about every 4 MB
};

// target [t_off, t_off+len) against base [b_off, b_off+b_len) at the same relative offsets: equal runs of at least
// kMinBaseRun bytes are `base`, the rest `data` (and the part past the base's end). False when a read fails or `tick` stops.
bool SameOffsetDiff(ByteSource& target, uint64_t t_off, uint64_t len, ByteSource& base, uint64_t b_off, uint64_t b_len,
                    Recipe* out, const DiffControl& control);

struct BhEntry {
    std::string name;   // as recorded ("Levels\\Earth\\Hub\\beach.rm2")
    uint32_t offset = 0, size = 0;
};
struct BhIndex {
    int32_t header = 0;
    std::vector<BhEntry> entries;   // in the index's order
};
bool ParseBh(const std::vector<uint8_t>& bytes, BhIndex* out, std::string* error);
std::vector<uint8_t> WriteBh(const BhIndex& index);
std::string ArchiveKey(const std::string& name);   // the game's lookup key: upper case, '\' separators

struct ArchiveDiffStats {
    uint32_t same = 0, changed = 0, added = 0, removed = 0;   // entries, by name
    std::vector<std::string> changed_names, added_names, removed_names;
};
// The target archive (its index + its data) against the disc's: per target entry in data order, the bytes of the disc's
// entry of the same name compared at the same relative offsets. False when a read fails or `control` stops.
bool ArchiveDiff(const BhIndex& target_bh, ByteSource& target_bd, const BhIndex& base_bh, ByteSource& base_bd, Recipe* out,
                 ArchiveDiffStats* stats, const DiffControl& control);

}  // namespace te::modimport
