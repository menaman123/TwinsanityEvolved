#include "engine/sound/audio_mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/sound/pcm_source.h"
#include "engine/sound/voice_source.h"

namespace ps2r::sound {
namespace {
bool Gain(float value) { return std::isfinite(value) && value >= 0 && value <= 1; }
}

// std::clamp(x, -1.0f, 1.0f) by the float's bits, stored: |x| > 1 (the infinities included, a NaN not: NaN fails both of
// clamp's compares) -> +-1.0 with x's sign, else x itself. The same bits for every one of the 2^32 inputs (checked
// exhaustively under round-to-nearest / toward-zero, flush-to-zero off / on; the AudioMixer test re-checks 100 M of them
// and every boundary), without the four VFP compares + flag transfers (vmrs) std::clamp costs a frame on the Vita's A9.
void StoreUnitClamped(float* out, float x) {
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    const uint32_t magnitude = bits & 0x7fffffffu;
    if (magnitude > 0x3f800000u && magnitude <= 0x7f800000u) bits = (bits & 0x80000000u) | 0x3f800000u;
    std::memcpy(out, &bits, sizeof bits);
}

bool AudioMixer::SetMaster(float gain) {
    if (!Gain(gain)) return false;
    state_.master = gain;
    return true;
}

bool AudioMixer::SetGroup(uint32_t group, float left, float right) {
    if (group >= kGroups || !Gain(left) || !Gain(right)) return false;
    state_.left[group] = left; state_.right[group] = right;
    return true;
}

bool AudioMixer::Validate(const State& state) {
    if (!Gain(state.master)) return false;
    for (uint32_t group = 0; group < kGroups; ++group)
        if (!Gain(state.left[group]) || !Gain(state.right[group])) return false;
    return true;
}

bool AudioMixer::Restore(const State& state) {
    if (!Validate(state)) return false;
    // Movie PCM lifetime (flush of an abandoned timeline, closed gain) is
    // restored by the Movie owner from MOVI; this sink holds none of it.
    state_ = state;
    return true;
}

void AudioMixer::MixInto(float* stereo, int frames) {
    if (frames <= 0) return;
    voices_.BeginMixTick();
    const auto gain = state_;
    // The Movie owner's gain is read once per pump, like the retained core
    // reads its BVOL shadows once per MixInto.
    const auto movie_gain = movie_ ? movie_->Gains() : std::array<float, 2>{0, 0};
    constexpr int chunk = 128;
    constexpr uint32_t channels = kGroups * kChannelsPerGroup;
    int16_t pcm[chunk * 2];
    // The source's block form (VoiceSource::OwnedChannels): channel-major runs of `chunk` frames into per-frame sums,
    // each frame's channels still added in channel order -- the same additions in the same order as the frame loop.
    uint64_t owned = 0;
    const bool block = voices_.OwnedChannels(owned);
    float sum_left[chunk], sum_right[chunk], voice_left[chunk], voice_right[chunk];
    for (int base = 0; base < frames; base += chunk) {
        const int n = std::min(chunk, frames - base);
        // Popped before the voices step so a muted output still consumes and
        // the movie's A/V clock keeps flowing (OLD SOUND_P5).
        const int movie_frames = movie_ ? movie_->Pop(pcm, n) : 0;
        count_.movie_frames += movie_frames;
        if (movie_gain[0] != 0 || movie_gain[1] != 0) count_.audible_movie_frames += movie_frames;
        if (block) {
            std::fill(sum_left, sum_left + n, 0.0f);
            std::fill(sum_right, sum_right + n, 0.0f);
            // A silent channel (StepVoices false: every output +0) under a group gain without its sign bit contributes
            // exactly +0 to each sum, in every rounding / flush mode. s + (+0) is idempotent ((s + 0) + 0 == s + 0 bit for
            // bit), so a run of such channels between two audible ones is ONE +0 addition, made just before the next
            // audible channel's (or before the movie), and none before the first audible one (+0 + +0 is +0).
            bool added = false, zero_pending = false;
            const auto add_zero = [&] {
                for (int frame = 0; frame < n; ++frame) {
                    sum_left[frame] += 0.0f;
                    sum_right[frame] += 0.0f;
                }
                zero_pending = false;
            };
            for (uint32_t channel = 0; channel < channels; ++channel) {
                if (channel >= 64 || !((owned >> channel) & 1u)) {
                    count_.unowned_samples += static_cast<uint64_t>(n);
                    continue;
                }
                count_.channel_samples += static_cast<uint64_t>(n);
                const float group_left = gain.left[channel / kChannelsPerGroup];
                const float group_right = gain.right[channel / kChannelsPerGroup];
                if (!voices_.StepVoices(channel, n, voice_left, voice_right)) {
                    if (!std::signbit(group_left) && !std::signbit(group_right)) {
                        zero_pending = added;
                        continue;
                    }
                    std::fill(voice_left, voice_left + n, 0.0f);   // a -0 gain: its zeros added as they are
                    std::fill(voice_right, voice_right + n, 0.0f);
                }
                if (zero_pending) add_zero();
                for (int frame = 0; frame < n; ++frame) {
                    sum_left[frame] += voice_left[frame] * group_left;
                    sum_right[frame] += voice_right[frame] * group_right;
                }
                added = true;
            }
            if (zero_pending) add_zero();
            for (int frame = 0; frame < n; ++frame) {
                float left = sum_left[frame], right = sum_right[frame];
                if (frame < movie_frames) {
                    left += static_cast<float>(pcm[frame * 2]) / 32768.0f * movie_gain[0];
                    right += static_cast<float>(pcm[frame * 2 + 1]) / 32768.0f * movie_gain[1];
                }
                StoreUnitClamped(&stereo[(base + frame) * 2], left * gain.master);
                StoreUnitClamped(&stereo[(base + frame) * 2 + 1], right * gain.master);
            }
            continue;
        }
        for (int frame = 0; frame < n; ++frame) {
            float left = 0, right = 0;
            for (uint32_t channel = 0; channel < channels; ++channel) {
                float l = 0, r = 0;
                if (!voices_.StepVoice(channel, l, r)) {
                    ++count_.unowned_samples;
                    continue;
                }
                ++count_.channel_samples;
                left += l * gain.left[channel / kChannelsPerGroup];
                right += r * gain.right[channel / kChannelsPerGroup];
            }
            if (frame < movie_frames) {
                left += static_cast<float>(pcm[frame * 2]) / 32768.0f * movie_gain[0];
                right += static_cast<float>(pcm[frame * 2 + 1]) / 32768.0f * movie_gain[1];
            }
            stereo[(base + frame) * 2] = std::clamp(left * gain.master, -1.0f, 1.0f);
            stereo[(base + frame) * 2 + 1] = std::clamp(right * gain.master, -1.0f, 1.0f);
        }
    }
    ++count_.ticks;
    count_.frames += frames;
}
} // namespace ps2r::sound
