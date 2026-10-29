#pragma once

#include <cstdint>

namespace ps2r::sound {

// Samples supplied by an owner on the game's audio clock. A handled silent
// channel still owns the slot; it must not revive a retained voice.
class VoiceSource {
public:
    virtual ~VoiceSource() = default;
    virtual void BeginMixTick() = 0;
    virtual bool StepVoice(uint32_t channel, float& left, float& right) = 0;

    // The block form (optional). A source whose channels step independently of one another (stepping one reads and
    // writes no other channel's state) and whose ownership cannot change during a mix (from its BeginMixTick to the end
    // of the MixInto that called it) answers true with the channels it owns (bit c = channel c, c < 64). The mixer then
    // steps each owned channel a run of frames at a time through StepVoices and never asks an unowned one: per channel
    // the same steps in the same order as frame-by-frame StepVoice calls, and per output frame the channels summed in the
    // same order -- the mix is bit for bit the frame-by-frame one. The default (false) keeps StepVoice per frame.
    virtual bool OwnedChannels(uint64_t& owned) {
        (void)owned;
        return false;
    }
    // `frames` consecutive StepVoice(channel, ...) outputs of an owned channel: frame f into left[f] / right[f]. May
    // return false instead, having written nothing, when every one of those outputs is +0 (a channel that is silent for
    // the whole run): adding +0 to the mixer's per-frame sums changes no bit -- a sum that starts at +0 is never -0 in
    // any rounding mode but toward negative infinity, and x + (+0) is x for every other x --, so the mixer skips the
    // channel's additions then (and adds the zeros under FE_DOWNWARD, where +0 + -0 is -0).
    virtual bool StepVoices(uint32_t channel, int frames, float* left, float* right) {
        for (int frame = 0; frame < frames; ++frame) {
            left[frame] = right[frame] = 0;
            StepVoice(channel, left[frame], right[frame]);
        }
        return true;
    }
};

}  // namespace ps2r::sound
