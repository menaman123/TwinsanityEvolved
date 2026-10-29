#pragma once

#include <algorithm>
#include <cstdint>

namespace title {

// A volume target controlled on the service clock and advanced by output
// samples. State contains gain arithmetic only, without libsd commands,
// register shadows, an IOP callback, or a hardware address.
class VolumeRamp {
public:
    struct State {
        uint16_t target = 0;
        int8_t direction = 0;
        bool active = false, crossing_zero = false;
        float step = 0;
    };
    void Target(uint16_t volume, float current);
    void Control(float& gain);
    // Per output sample (inline: the voices' per-sample loop calls it twice a sample)
    void Step(float& gain) const;
    void Reset();
    const State& Capture() const;
    static bool Valid(const State& state);
    bool Restore(const State& state);

private:
    static float SignOf(uint16_t volume) { return volume > 0x3fff ? -1.0f : 1.0f; }
    State state_;
};

inline void VolumeRamp::Step(float& gain) const {
    if (state_.step == 0) return;
    gain += state_.step;
    if (state_.crossing_zero) {
        gain = state_.step < 0 ? std::max(gain, 0.0f) : std::min(gain, 0.0f);
    } else if (SignOf(state_.target) < 0) {
        gain = std::clamp(gain, -0.5f, 0.0f);
    } else {
        gain = std::clamp(gain, 0.0f, 0.5f);
    }
}

} // namespace title
