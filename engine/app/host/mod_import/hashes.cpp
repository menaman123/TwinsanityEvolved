// hashes.h implementation: SHA-1 (FIPS 180-4), CRC-32 (reflected 0xEDB88320, slicing-by-4), Adler-32 (RFC 1950).
#include "app/host/mod_import/hashes.h"

#include <cstring>

namespace te::modimport {

namespace {

inline uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

struct CrcTables {
    uint32_t t[4][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 4; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
    }
};

const CrcTables& Tables() {
    static const CrcTables tables;
    return tables;
}

}  // namespace

void Sha1::Reset() {
    h_[0] = 0x67452301u;
    h_[1] = 0xEFCDAB89u;
    h_[2] = 0x98BADCFEu;
    h_[3] = 0x10325476u;
    h_[4] = 0xC3D2E1F0u;
    length_ = 0;
    buffered_ = 0;
}

void Sha1::Block(const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 | uint32_t(block[i * 4 + 2]) << 8 | uint32_t(block[i * 4 + 3]);
    for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        const uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rol(b, 30);
        b = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::Update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    length_ += len;
    if (buffered_) {
        const size_t take = len < 64 - buffered_ ? len : 64 - buffered_;
        std::memcpy(buffer_ + buffered_, p, take);
        buffered_ += take;
        p += take;
        len -= take;
        if (buffered_ < 64) return;
        Block(buffer_);
        buffered_ = 0;
    }
    while (len >= 64) {
        Block(p);
        p += 64;
        len -= 64;
    }
    if (len) {
        std::memcpy(buffer_, p, len);
        buffered_ = len;
    }
}

void Sha1::Finish(uint8_t digest[kDigestBytes]) {
    const uint64_t bits = length_ * 8;
    const uint8_t one = 0x80;
    Update(&one, 1);
    const uint8_t zero = 0;
    while (buffered_ != 56) Update(&zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = uint8_t(bits >> (56 - 8 * i));
    Update(tail, 8);
    for (int i = 0; i < 5; ++i)
        for (int k = 0; k < 4; ++k) digest[i * 4 + k] = uint8_t(h_[i] >> (24 - 8 * k));
    Reset();
}

std::string Sha1::HexDigest() {
    uint8_t d[kDigestBytes];
    Finish(d);
    return HexBytes(d, sizeof d);
}

uint32_t Crc32Update(uint32_t crc, const void* data, size_t len) {
    const CrcTables& T = Tables();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = ~crc;
    while (len >= 4) {
        c ^= uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        c = T.t[3][c & 0xFFu] ^ T.t[2][(c >> 8) & 0xFFu] ^ T.t[1][(c >> 16) & 0xFFu] ^ T.t[0][c >> 24];
        p += 4;
        len -= 4;
    }
    while (len--) c = T.t[0][(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

uint32_t Adler32Update(uint32_t adler, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t a = adler & 0xFFFFu, b = adler >> 16;
    while (len) {
        size_t n = len < 5552 ? len : 5552;   // the largest run before the sums can overflow 32 bits
        len -= n;
        while (n--) {
            a += *p++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

std::string HexBytes(const uint8_t* data, size_t len) {
    static const char* const digits = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
    return out;
}

}  // namespace te::modimport
