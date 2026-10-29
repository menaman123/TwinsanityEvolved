#include "games/slus_20909_crash_twinsanity/hle/audio/volume_ramp.h"

#include <algorithm>
#include <cmath>

namespace title {
namespace {
uint32_t Magnitude(uint16_t volume) { return volume > 0x3fff ? 0x7fff - volume : volume; }
float Sign(uint16_t volume) { return volume > 0x3fff ? -1.0f : 1.0f; }
uint32_t Level(float gain) {
    return static_cast<uint32_t>(std::clamp(std::fabs(gain) * 65534.0f, 0.0f, 32767.0f));
}
float Rate(uint32_t distance) {
    // The title's distance bands choose rates 35,39,43,47,51. The NEW
    // command stepper is independently checked in the model/owner oracle.
    uint32_t band = 0;
    while (band < 4 && ((distance / 2) & (0x2000u >> band)) == 0) ++band;
    const float samples = 4.0f * std::pow(2.0f, static_cast<float>(35 + 4 * band) / 4.0f);
    return 0.5f * (1.0f / samples);
}
}
void VolumeRamp::Target(uint16_t volume, float current) {
    state_.target = volume;
    state_.direction = Level(current) / 2 < Magnitude(volume) ? 1 : -1;
    state_.active = true;
    state_.crossing_zero = false;
    // Changing the target does not change the already-running slope before
    // the next control update.
}
void VolumeRamp::Control(float& gain) {
    if (!state_.active) return;
    const float sign = Sign(state_.target);
    if (gain != 0 && ((gain < 0) != (sign < 0))) {
        state_.crossing_zero = true;
        state_.step = gain > 0 ? -0.5f : 0.5f;
        return;
    }
    if (state_.crossing_zero) {
        state_.direction = 1;
        state_.crossing_zero = false;
    }
    const uint32_t target = Magnitude(state_.target) * 2, current = Level(gain);
    uint32_t distance = current < target ? target - current : current - target;
    if (current > target && state_.direction == 1) distance = 0;
    if (distance <= 0x400) {
        gain = sign * (static_cast<float>(Magnitude(state_.target)) / 32767.0f);
        state_.step = 0;
        state_.active = false;
        return;
    }
    state_.step = sign * (current < target ? Rate(distance) : -Rate(distance));
}
// VolumeRamp::Step is inline in volume_ramp.h (the voices' per-sample loop).
void VolumeRamp::Reset() { state_ = {}; }
const VolumeRamp::State& VolumeRamp::Capture() const { return state_; }
bool VolumeRamp::Valid(const State& state) {
    return state.target <= 0x7fff && state.direction >= -1 && state.direction <= 1 &&
           (!state.active || state.direction != 0) && std::isfinite(state.step) &&
           std::fabs(state.step) <= 0.5f && (state.active || (!state.crossing_zero && state.step == 0));
}
bool VolumeRamp::Restore(const State& state) {
    if (!Valid(state)) return false;
    state_ = state;
    return true;
}
} // namespace title
