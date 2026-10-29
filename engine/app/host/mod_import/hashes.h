#pragma once
// The checksums the importer needs beside the engine's SHA-256 (engine/core/hash/sha256.h): SHA-1 (a disc image's
// identity as Redump lists it), CRC-32 (zip entries, BPS patches, PPF), Adler-32 (xdelta3's per-window checksum).
// Incremental; plain C++17.
#include <cstddef>
#include <cstdint>
#include <string>

namespace te::modimport {

class Sha1 {
public:
    static constexpr size_t kDigestBytes = 20;
    Sha1() { Reset(); }
    void Reset();
    void Update(const void* data, size_t len);
    void Finish(uint8_t digest[kDigestBytes]);
    std::string HexDigest();   // Finish + lower-case hex

private:
    void Block(const uint8_t block[64]);
    uint32_t h_[5] = {};
    uint64_t length_ = 0;
    uint8_t buffer_[64] = {};
    size_t buffered_ = 0;
};

// CRC-32 (ISO-HDLC, the zip / PNG / BPS one): Crc32Update(0, ...) starts a value; the result is final after each call.
uint32_t Crc32Update(uint32_t crc, const void* data, size_t len);

// Adler-32 (RFC 1950): start with 1.
uint32_t Adler32Update(uint32_t adler, const void* data, size_t len);

std::string HexBytes(const uint8_t* data, size_t len);

}  // namespace te::modimport
