#pragma once
// ZipReader -- a minimal reader for the .zip files mods are shipped in (a Mod Crate, a zipped patch, a zipped folder):
// the central directory (with ZIP64 sizes and offsets), entries stored (method 0) or deflated (method 8, inflate.h),
// every entry's CRC-32 and size checked as it is extracted. Encrypted entries and other methods are named in the error
// (a crate is never encrypted; LZMA / bzip2 / zstd zips are rare and refused). Names are UTF-8 (flag bit 11) or CP437,
// '/' separated as the format says ('\' is taken as '/' too: some Windows tools write it). Reads through the engine's
// file layer (io.h: UTF-8 paths, 64-bit offsets).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app/host/mod_import/io.h"

namespace te::modimport {

class ZipReader {
public:
    struct Entry {
        std::string name;          // '/' separators; a directory ends in '/'
        uint64_t compressed = 0;   // bytes in the archive
        uint64_t size = 0;         // bytes once extracted
        uint32_t crc = 0;
        uint16_t method = 0;       // 0 stored, 8 deflated
        uint16_t flags = 0;        // bit 0: encrypted
        uint64_t local_offset = 0; // the local header
        bool directory = false;
    };
    using Sink = std::function<bool(const uint8_t* data, size_t n)>;

    ZipReader() = default;
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    // True when the file starts like a zip ("PK\3\4", or an empty archive's "PK\5\6")
    static bool LooksLikeZip(const uint8_t* head, size_t n);

    bool Open(const std::string& path, std::string* error);
    void Close();
    const std::vector<Entry>& Entries() const { return entries_; }

    // The entry's bytes, pushed to `sink` in chunks; CRC-32 and size checked at the end. False with *error.
    bool Extract(const Entry& entry, const Sink& sink, std::string* error) const;
    // The first `n` bytes (fewer when the entry is shorter): what a file is, by its content
    bool ReadHead(const Entry& entry, size_t n, std::vector<uint8_t>* out, std::string* error) const;
    // A relative path inside the archive that is safe to create under a folder ("a/b.txt"; never "..", "/x", "C:x")
    static bool SafeName(const std::string& name, std::string* clean);

private:
    bool DataOffset(const Entry& entry, uint64_t* offset, std::string* error) const;

    mutable io::File file_;
    uint64_t file_size_ = 0;
    std::vector<Entry> entries_;
};

}  // namespace te::modimport
