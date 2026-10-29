// VagDecoder implementation; the format is described in the header.
#include "engine/sound/vag_decoder.h"

#if defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif

namespace {
// The fixed predictor table (coefficients x64) of the PS1/PS2 SPU ADPCM scheme.
constexpr int32_t kF0[VagDecoder::kFilterCount] = {0, 60, 115, 98, 122};
constexpr int32_t kF1[VagDecoder::kFilterCount] = {0, 0, -52, -55, -60};

int16_t ClampS16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}
// ClampS16 in one instruction where the CPU saturates (SSAT: ARMv6 and later, the Vita's Cortex-A9); the same result
inline int16_t Saturate16(int32_t v) {
#if defined(__ARM_FEATURE_SAT)
    return static_cast<int16_t>(__ssat(v, 16));
#else
    return ClampS16(v);
#endif
}
}  // namespace

uint8_t VagDecoder::DecodeBlock(const uint8_t block[kBlockBytes], History& hist, int16_t out[kBlockSamples]) {
    uint32_t shift = block[0] & 0x0F;
    uint32_t filter = (block[0] >> 4) & 0x0F;
    const uint8_t flags = block[1];
    if (shift > kMaxShift) shift = kMaxShift;
    if (filter >= kFilterCount) filter = 0;
    const int32_t f0 = kF0[filter];
    const int32_t f1 = kF1[filter];
    // DecodeBlockReference's (signed_nibble * 4096) / 2^shift is an EXACT division for every legal shift (0..12): it is
    // signed_nibble * 2^(12 - shift), a multiply -- the reference's per-sample division is a library call on a CPU
    // without a divide instruction (the Vita's Cortex-A9: __divsi3 for every decoded sample). The history is kept in
    // registers and stored once (the block's bytes may alias it, so the reference stores it every sample). Same values.
    const int32_t scale = int32_t{1} << (kMaxShift - shift);
    int32_t h1 = hist.h1, h2 = hist.h2;
    for (int i = 0; i < kBlockSamples; i += 2) {
        const uint32_t byte = block[2 + (i >> 1)];
        // low nibble first (the earlier sample), then the high one; each sign-extended from its 4 bits (the arithmetic
        // shift: the reference's nibble - 16 for a nibble >= 8)
        const int32_t nibbles[2] = {static_cast<int32_t>(byte << 28) >> 28, static_cast<int32_t>(byte << 24) >> 28};
        for (int half = 0; half < 2; ++half) {
            int32_t s = nibbles[half] * scale;
            s += (h1 * f0 + h2 * f1) >> 6;
            const int16_t sample = Saturate16(s);
            out[i + half] = sample;
            h2 = h1;
            h1 = sample;
        }
    }
    hist.h1 = h1;
    hist.h2 = h2;
    return flags;
}

uint8_t VagDecoder::DecodeBlockReference(const uint8_t block[kBlockBytes], History& hist, int16_t out[kBlockSamples]) {
    uint32_t shift = block[0] & 0x0F;
    uint32_t filter = (block[0] >> 4) & 0x0F;
    const uint8_t flags = block[1];
    if (shift > kMaxShift) shift = kMaxShift;
    if (filter >= kFilterCount) filter = 0;
    const int32_t f0 = kF0[filter];
    const int32_t f1 = kF1[filter];
    for (int i = 0; i < kBlockSamples; i++) {
        const uint8_t byte = block[2 + (i >> 1)];
        const uint32_t nibble = (i & 1) ? (byte >> 4) : (byte & 0x0F);   // low nibble first
        // The nibble is the top 4 bits of a 16-bit sample, shifted down by `shift`.
        const int32_t signed_nibble = static_cast<int32_t>(nibble) - (nibble >= 8 ? 16 : 0);
        // The result is exactly divisible by 2^shift for every legal shift
        // (0..12), so division preserves the intended signed scaling without
        // shifting a negative value or relying on signed-right-shift rules.
        int32_t s = (signed_nibble * 4096) / (int32_t{1} << shift);
        s += (hist.h1 * f0 + hist.h2 * f1) >> 6;
        const int16_t sample = ClampS16(s);
        out[i] = sample;
        hist.h2 = hist.h1;
        hist.h1 = sample;
    }
    return flags;
}
