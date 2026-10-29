// inflate.h implementation. Huffman codes are decoded through a 10-bit lookup table; longer codes (up to 15 bits) by the
// canonical walk (RFC 1951 3.2.2: codes of one length are consecutive integers, shorter lengths first).
#include "app/host/mod_import/inflate.h"

#include <cstring>
#include <vector>

namespace te::modimport {

namespace {

constexpr int kMaxBits = 15;
constexpr int kFastBits = 10;
constexpr size_t kWindow = 32768;
constexpr size_t kFlushAt = (1u << 20) + kWindow;   // the output buffer is flushed (all but the history) past this

const uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                  131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
                                2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Huffman {
    uint16_t count[kMaxBits + 1] = {};   // codes of each length
    std::vector<uint16_t> symbol;        // symbols in canonical order
    uint16_t fast[1u << kFastBits] = {};  // (symbol << 4) | length, 0 = not a code of at most kFastBits bits
    bool complete = false;

    // False when the lengths over-subscribe the code space (an incomplete code is allowed: a single distance code)
    bool Build(const uint8_t* lengths, int n) {
        std::memset(count, 0, sizeof count);
        std::memset(fast, 0, sizeof fast);
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        complete = left == 0;
        uint16_t offs[kMaxBits + 2] = {};
        for (int len = 1; len <= kMaxBits; ++len) offs[len + 1] = uint16_t(offs[len] + count[len]);
        symbol.assign(size_t(offs[kMaxBits + 1]), 0);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = uint16_t(i);
        // the fast table: each code of at most kFastBits bits, bit-reversed (deflate sends a code's top bit first)
        uint32_t code = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            for (int k = 0; k < count[len]; ++k, ++index) {
                if (len <= kFastBits) {
                    uint32_t rev = 0;
                    for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1u) << (len - 1 - b);
                    for (uint32_t fill = rev; fill < (1u << kFastBits); fill += (1u << len))
                        fast[fill] = uint16_t((uint32_t(symbol[size_t(index)]) << 4) | uint32_t(len));
                }
                ++code;
            }
            code <<= 1;
        }
        return true;
    }
};

class Decoder {
public:
    Decoder(const InflateSource& source, const InflateSink& sink) : source_(source), sink_(sink) {
        in_.resize(1u << 16);
        out_.reserve(kFlushAt + 300);
    }

    bool Run(std::string* error) {
        bool last = false;
        while (!last) {
            if (!Need(3)) return Fail(error, "the compressed data ends inside a block header");
            last = Take(1) != 0;
            const uint32_t type = Take(2);
            bool ok;
            if (type == 0) ok = Stored(error);
            else if (type == 1) ok = Fixed(error);
            else if (type == 2) ok = Dynamic(error);
            else return Fail(error, "a block of the reserved type 3");
            if (!ok) return false;
        }
        if (!Flush(true)) return Fail(error, "stopped");
        return true;
    }

    uint64_t Consumed() const { return consumed_ - uint64_t(bits_ / 8); }
    uint64_t Produced() const { return produced_; }

private:
    bool Fail(std::string* error, const char* why) {
        if (error && error->empty()) *error = why;
        return false;
    }

    // Has at least n bits buffered (pulls bytes as needed); false at the end of the input
    bool Need(int n) {
        while (bits_ < n) {
            if (in_at_ == in_len_) {
                in_len_ = source_ ? source_(in_.data(), in_.size()) : 0;
                in_at_ = 0;
                if (in_len_ == 0) return false;
            }
            bitbuf_ |= uint64_t(in_[in_at_++]) << bits_;
            bits_ += 8;
            ++consumed_;
        }
        return true;
    }
    // Buffers as many bits as there are, up to n (the last code of a stream may need fewer than 15)
    void Fill(int n) {
        while (bits_ < n) {
            if (in_at_ == in_len_) {
                in_len_ = source_ ? source_(in_.data(), in_.size()) : 0;
                in_at_ = 0;
                if (in_len_ == 0) return;
            }
            bitbuf_ |= uint64_t(in_[in_at_++]) << bits_;
            bits_ += 8;
            ++consumed_;
        }
    }
    uint32_t Take(int n) {
        const uint32_t v = uint32_t(bitbuf_ & ((uint64_t(1) << n) - 1));
        bitbuf_ >>= n;
        bits_ -= n;
        return v;
    }

    // One symbol; -1 when the input ends or the bits are not a code
    int Decode(const Huffman& h) {
        Fill(kMaxBits);
        if (bits_ <= 0) return -1;
        const uint16_t e = h.fast[bitbuf_ & ((1u << kFastBits) - 1)];
        if (e && int(e & 15) <= bits_) {
            Take(int(e & 15));
            return int(e >> 4);
        }
        // the canonical walk, one bit at a time
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            if (bits_ <= 0) return -1;
            code |= int(Take(1));
            const int count = h.count[len];
            if (code - count < first) return int(h.symbol[size_t(index + (code - first))]);
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    // Emits every byte not emitted yet; unless `all`, keeps only the last 32 KB (the history back-references reach)
    bool Flush(bool all) {
        if (out_.size() > flushed_) {
            if (sink_ && !sink_(out_.data() + flushed_, out_.size() - flushed_)) return false;
            flushed_ = out_.size();
        }
        if (!all && out_.size() > kWindow) {
            const size_t drop = out_.size() - kWindow;
            std::memmove(out_.data(), out_.data() + drop, kWindow);
            out_.resize(kWindow);
            flushed_ = kWindow;
        }
        return true;
    }

    bool Put(uint8_t b) {
        out_.push_back(b);
        ++produced_;
        if (out_.size() >= kFlushAt) return Flush(false);
        return true;
    }

    bool Stored(std::string* error) {
        // to a byte boundary, then LEN and NLEN
        Take(bits_ % 8);
        if (!Need(32)) return Fail(error, "the compressed data ends inside a stored block's header");
        const uint32_t len = Take(16), nlen = Take(16);
        if ((len ^ 0xFFFFu) != nlen) return Fail(error, "a stored block whose length check fails");
        for (uint32_t i = 0; i < len; ++i) {
            if (!Need(8)) return Fail(error, "the compressed data ends inside a stored block");
            if (!Put(uint8_t(Take(8)))) return Fail(error, "stopped");
        }
        return true;
    }

    bool Codes(const Huffman& lit, const Huffman& dist, std::string* error) {
        for (;;) {
            const int sym = Decode(lit);
            if (sym < 0) return Fail(error, "the compressed data ends inside a block, or holds a bad code");
            if (sym < 256) {
                if (!Put(uint8_t(sym))) return Fail(error, "stopped");
                continue;
            }
            if (sym == 256) return true;
            const int li = sym - 257;
            if (li >= 29) return Fail(error, "a length code past 285");
            if (!Need(kLengthExtra[li])) return Fail(error, "the compressed data ends inside a length");
            const uint32_t length = kLengthBase[li] + Take(kLengthExtra[li]);
            const int ds = Decode(dist);
            if (ds < 0 || ds >= 30) return Fail(error, "a bad distance code");
            if (!Need(kDistExtra[ds])) return Fail(error, "the compressed data ends inside a distance");
            const uint32_t distance = kDistBase[ds] + Take(kDistExtra[ds]);
            if (distance > produced_ || distance > out_.size()) return Fail(error, "a distance before the start of the output");
            // (a flush inside the copy moves the history to the front: the source is always `distance` before the end)
            for (uint32_t i = 0; i < length; ++i)
                if (!Put(out_[out_.size() - distance])) return Fail(error, "stopped");
        }
    }

    bool Fixed(std::string* error) {
        if (!fixed_built_) {
            uint8_t lengths[288];
            int i = 0;
            for (; i < 144; ++i) lengths[i] = 8;
            for (; i < 256; ++i) lengths[i] = 9;
            for (; i < 280; ++i) lengths[i] = 7;
            for (; i < 288; ++i) lengths[i] = 8;
            fixed_lit_.Build(lengths, 288);
            uint8_t d[30];
            for (int k = 0; k < 30; ++k) d[k] = 5;
            fixed_dist_.Build(d, 30);
            fixed_built_ = true;
        }
        return Codes(fixed_lit_, fixed_dist_, error);
    }

    bool Dynamic(std::string* error) {
        if (!Need(14)) return Fail(error, "the compressed data ends inside a block's code lengths");
        const int nlen = int(Take(5)) + 257, ndist = int(Take(5)) + 1, ncode = int(Take(4)) + 4;
        if (nlen > 286 || ndist > 30) return Fail(error, "a block with too many codes");
        uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) {
            if (!Need(3)) return Fail(error, "the compressed data ends inside the code lengths");
            lengths[kCodeLengthOrder[i]] = uint8_t(Take(3));
        }
        Huffman lencode;
        if (!lencode.Build(lengths, 19) || !lencode.complete) return Fail(error, "an incomplete code-length code");
        int index = 0;
        std::memset(lengths, 0, sizeof lengths);
        while (index < nlen + ndist) {
            int sym = Decode(lencode);
            if (sym < 0) return Fail(error, "the compressed data ends inside the code lengths");
            if (sym < 16) {
                lengths[index++] = uint8_t(sym);
                continue;
            }
            uint8_t repeat = 0;
            int times;
            if (sym == 16) {
                if (index == 0) return Fail(error, "a repeat with no length before it");
                repeat = lengths[index - 1];
                if (!Need(2)) return Fail(error, "the compressed data ends inside the code lengths");
                times = 3 + int(Take(2));
            } else if (sym == 17) {
                if (!Need(3)) return Fail(error, "the compressed data ends inside the code lengths");
                times = 3 + int(Take(3));
            } else {
                if (!Need(7)) return Fail(error, "the compressed data ends inside the code lengths");
                times = 11 + int(Take(7));
            }
            if (index + times > nlen + ndist) return Fail(error, "code lengths past the codes' count");
            while (times--) lengths[index++] = repeat;
        }
        if (lengths[256] == 0) return Fail(error, "a block without an end code");
        Huffman lit, dist;
        if (!lit.Build(lengths, nlen)) return Fail(error, "an over-subscribed literal code");
        if (!lit.complete && !SingleShortCode(lengths, nlen)) return Fail(error, "an incomplete literal code");
        if (!dist.Build(lengths + nlen, ndist)) return Fail(error, "an over-subscribed distance code");
        if (!dist.complete && !SingleShortCode(lengths + nlen, ndist)) return Fail(error, "an incomplete distance code");
        return Codes(lit, dist, error);
    }

    // An incomplete code is allowed only when it is one code of length 1 (RFC 1951 3.2.7; zlib's and puff's rule)
    static bool SingleShortCode(const uint8_t* lengths, int n) {
        int ones = 0;
        for (int i = 0; i < n; ++i) {
            if (lengths[i] > 1) return false;
            ones += lengths[i];
        }
        return ones <= 1;
    }

    const InflateSource& source_;
    const InflateSink& sink_;
    std::vector<uint8_t> in_;
    size_t in_at_ = 0, in_len_ = 0;
    uint64_t bitbuf_ = 0;
    int bits_ = 0;
    uint64_t consumed_ = 0;
    std::vector<uint8_t> out_;
    size_t flushed_ = 0;   // out_[0, flushed_) has gone to the sink
    uint64_t produced_ = 0;
    Huffman fixed_lit_, fixed_dist_;
    bool fixed_built_ = false;
};

}  // namespace

bool Inflate(const InflateSource& source, const InflateSink& sink, std::string* error, uint64_t* consumed_out,
             uint64_t* produced_out) {
    Decoder d(source, sink);
    std::string why;
    const bool ok = d.Run(&why);
    if (!ok && error) *error = why;
    if (consumed_out) *consumed_out = d.Consumed();
    if (produced_out) *produced_out = d.Produced();
    return ok;
}

}  // namespace te::modimport
