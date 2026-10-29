// disc_image_hash -- see disc_image_hash.h.
#include "app/host/disc_image_hash.h"

#include <cstring>

namespace ps2r::app {
namespace {

// The eight tables of the sliced CRC-32 (reflected polynomial 0xEDB88320), built once.
struct CrcTables {
    uint32_t t[8][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};

const CrcTables& Tables() {
    static const CrcTables tables;
    return tables;
}

inline uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

const char kHexDigits[] = "0123456789abcdef";

}  // namespace

void Crc32::Update(const void* data, size_t len) {
    const CrcTables& tb = Tables();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = state_;
    while (len >= 8) {
        // little-endian assembly of the eight bytes (byte order independent of the host)
        const uint32_t lo = c ^ (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
        const uint32_t hi = uint32_t(p[4]) | uint32_t(p[5]) << 8 | uint32_t(p[6]) << 16 | uint32_t(p[7]) << 24;
        c = tb.t[7][lo & 0xFF] ^ tb.t[6][(lo >> 8) & 0xFF] ^ tb.t[5][(lo >> 16) & 0xFF] ^ tb.t[4][lo >> 24] ^
            tb.t[3][hi & 0xFF] ^ tb.t[2][(hi >> 8) & 0xFF] ^ tb.t[1][(hi >> 16) & 0xFF] ^ tb.t[0][hi >> 24];
        p += 8;
        len -= 8;
    }
    while (len--) c = tb.t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    state_ = c;
}

uint32_t Crc32::Of(const void* data, size_t len) {
    Crc32 c;
    c.Update(data, len);
    return c.Value();
}

std::string Crc32::Hex(uint32_t crc) {
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i, crc >>= 4) out[size_t(i)] = kHexDigits[crc & 0xF];
    return out;
}

Sha1::Sha1() { Reset(); }

void Sha1::Reset() {
    h_[0] = 0x67452301u;
    h_[1] = 0xEFCDAB89u;
    h_[2] = 0x98BADCFEu;
    h_[3] = 0x10325476u;
    h_[4] = 0xC3D2E1F0u;
    buffered_ = 0;
    total_ = 0;
}

void Sha1::Block(const uint8_t b[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | uint32_t(b[4 * i + 3]);
    for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999u; }
        else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = bb ^ c ^ d; k = 0xCA62C1D6u; }
        const uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rol(bb, 30);
        bb = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += bb;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::Update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_ += len;
    if (buffered_) {
        const size_t n = len < 64 - buffered_ ? len : 64 - buffered_;
        std::memcpy(buffer_ + buffered_, p, n);
        buffered_ += n;
        p += n;
        len -= n;
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
    const uint64_t bits = total_ * 8;
    const uint8_t one = 0x80;
    Update(&one, 1);
    const uint8_t zero = 0;
    while (buffered_ != 56) Update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
    Update(len, 8);
    for (int i = 0; i < 5; ++i) {
        digest[4 * i] = uint8_t(h_[i] >> 24);
        digest[4 * i + 1] = uint8_t(h_[i] >> 16);
        digest[4 * i + 2] = uint8_t(h_[i] >> 8);
        digest[4 * i + 3] = uint8_t(h_[i]);
    }
}

std::string Sha1::Hex(const uint8_t digest[kDigestBytes]) {
    std::string out(kDigestBytes * 2, '0');
    for (size_t i = 0; i < kDigestBytes; ++i) {
        out[2 * i] = kHexDigits[digest[i] >> 4];
        out[2 * i + 1] = kHexDigits[digest[i] & 0xF];
    }
    return out;
}

std::string Sha1::HexOf(const void* data, size_t len) {
    Sha1 s;
    s.Update(data, len);
    uint8_t d[kDigestBytes];
    s.Finish(d);
    return Hex(d);
}

}  // namespace ps2r::app
