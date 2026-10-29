#pragma once
// disc_image_hash -- the two whole-image hashes the table of known retail dumps carries (launcher/discs/redump.json: the
// size, the CRC-32 and the SHA-1 of each dump, as redump.org publishes them): CRC-32 (IEEE 802.3, reflected, the zlib /
// PKZIP one) and SHA-1 (FIPS 180-4). Our own implementations, plain C++17, streaming: Update over any number of chunks,
// then Finish. The CRC is sliced by eight (a few GB/s); SHA-1 is the portable form (several hundred MB/s): a 2.6 GB image
// is a matter of seconds either way, and the disc read is usually the slower part.
#include <cstddef>
#include <cstdint>
#include <string>

namespace ps2r::app {

class Crc32 {
public:
    void Update(const void* data, size_t len);
    uint32_t Value() const { return ~state_; }
    static uint32_t Of(const void* data, size_t len);
    static std::string Hex(uint32_t crc);   // 8 lower-case hex digits

private:
    uint32_t state_ = 0xFFFFFFFFu;
};

class Sha1 {
public:
    static constexpr size_t kDigestBytes = 20;
    Sha1();
    void Update(const void* data, size_t len);
    // Finalizes into `digest`; the object is consumed (Reset to reuse).
    void Finish(uint8_t digest[kDigestBytes]);
    void Reset();
    static std::string Hex(const uint8_t digest[kDigestBytes]);   // 40 lower-case hex digits
    static std::string HexOf(const void* data, size_t len);

private:
    void Block(const uint8_t block[64]);
    uint32_t h_[5];
    uint8_t buffer_[64];
    size_t buffered_ = 0;
    uint64_t total_ = 0;
};

}  // namespace ps2r::app
