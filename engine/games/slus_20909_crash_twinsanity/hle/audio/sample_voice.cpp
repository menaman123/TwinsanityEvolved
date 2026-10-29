#include "games/slus_20909_crash_twinsanity/hle/audio/sample_voice.h"

#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

namespace title {
namespace {
float Ramp(uint8_t rate, float divisor) {
    return rate == 0 ? 1.0f : 1.0f / (4.0f * std::pow(2.0f, static_cast<float>(rate) / divisor));
}

// The mix's form (SampleVoice::SetLegacyMix) and the block loop's census (SampleVoice::MixCensus)
std::atomic<bool> g_legacy_mix{false};
SampleVoice::Census g_census;

// The landing slots (SampleLanding): the asset's address (null = free) and its landed mark
struct LandingSlot {
    std::atomic<const std::vector<uint8_t>*> asset{nullptr};
    std::atomic<uint32_t> landed{0};
};
LandingSlot g_landing[SampleLanding::kSlots];
std::atomic<uint64_t> g_landing_waits{0};
std::atomic<uint64_t> g_landing_waited_us{0};
}

std::atomic<int> SampleLanding::active_{0};

int SampleLanding::Begin(const std::vector<uint8_t>* asset, uint32_t landed) {
    if (!asset) return -1;
    // a slot is claimed with a placeholder no asset can be, its mark set, then the asset published: a waiter never pairs
    // the asset with the slot's previous mark
    static const std::vector<uint8_t> claiming;
    for (int k = 0; k < kSlots; ++k) {
        const std::vector<uint8_t>* expected = nullptr;
        if (!g_landing[k].asset.compare_exchange_strong(expected, &claiming, std::memory_order_acq_rel)) continue;
        g_landing[k].landed.store(landed, std::memory_order_release);
        g_landing[k].asset.store(asset, std::memory_order_release);
        active_.fetch_add(1, std::memory_order_acq_rel);
        return k;
    }
    return -1;
}

void SampleLanding::Advance(int slot, uint32_t landed) {
    if (slot < 0 || slot >= kSlots) return;
    g_landing[slot].landed.store(landed, std::memory_order_release);
}

void SampleLanding::End(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    if (g_landing[slot].asset.exchange(nullptr, std::memory_order_acq_rel)) active_.fetch_sub(1, std::memory_order_acq_rel);
}

void SampleLanding::Await(const std::vector<uint8_t>* asset, uint32_t end) {
    for (int k = 0; k < kSlots; ++k) {
        if (g_landing[k].asset.load(std::memory_order_acquire) != asset) continue;
        if (g_landing[k].landed.load(std::memory_order_acquire) >= end) return;
        const auto t0 = std::chrono::steady_clock::now();
        // the reader is ~1000x faster than playback: this is the card stalling; sleep, never spin
        while (g_landing[k].asset.load(std::memory_order_acquire) == asset &&
               g_landing[k].landed.load(std::memory_order_acquire) < end)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        g_landing_waits.fetch_add(1, std::memory_order_relaxed);
        g_landing_waited_us.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
        return;
    }
}

uint64_t SampleLanding::Waits() { return g_landing_waits.load(std::memory_order_relaxed); }
double SampleLanding::WaitedMs() { return double(g_landing_waited_us.load(std::memory_order_relaxed)) / 1000.0; }

float SampleVoice::Volume(uint16_t value) {
    // Source audio.h: bit 14 selects inverted phase; the payload measures from
    // the other endpoint. The mixer oracle's ordinary positive-volume arm is
    // unchanged; signed phase is a title intent not an unsigned host gain.
    const int32_t level = value & 0x7fff;
    const int32_t signed_level = level > 0x3fff ? level - 0x7fff : level;
    return static_cast<float>(signed_level) / 32767.0f;
}

bool SampleVoice::Start(Samples samples, Parameters parameters) {
    if (!samples || samples->empty() || samples->size() % VagDecoder::kBlockBytes != 0 ||
        samples->size() > UINT32_MAX || parameters.attack > 127 ||
        parameters.decay > 15 || parameters.sustain > 15 || parameters.release > 31 ||
        (parameters.left & 0x8000) || (parameters.right & 0x8000)) return false;
    state_ = {};
    state_.samples = std::move(samples);
    state_.parameters = parameters;
    state_.phase = Phase::Attack;
    state_.left = Volume(parameters.left);
    state_.right = Volume(parameters.right);
    // Decode at start, before later controls can change a loop/cursor. This
    // eliminates OLD SOUND_P9's delayed-first-block overwrite by construction.
    Decode();
    return true;
}

void SampleVoice::SetPitch(uint16_t pitch) { state_.parameters.pitch = pitch; }
void SampleVoice::SetVolumes(uint16_t left, uint16_t right) {
    state_.parameters.left = left;
    state_.parameters.right = right;
    state_.left = Volume(left);
    state_.right = Volume(right);
    state_.left_ramp.Reset();
    state_.right_ramp.Reset();
}
void SampleVoice::RampVolumes(uint16_t left, uint16_t right) {
    state_.parameters.left = left;
    state_.parameters.right = right;
    state_.left_ramp.Target(left, state_.left);
    state_.right_ramp.Target(right, state_.right);
}
void SampleVoice::AdvanceControls() {
    state_.left_ramp.Control(state_.left);
    state_.right_ramp.Control(state_.right);
}
void SampleVoice::SetRelease(uint8_t rate) {
    if (rate <= 31) state_.parameters.release = rate;
}
void SampleVoice::Release(uint8_t rate) {
    if (rate > 31 || !Playing()) return;
    SetRelease(rate);
    state_.phase = Phase::Release;
}
void SampleVoice::Stop() {
    state_.phase = Phase::Off;
    state_.envelope = 0;
    state_.samples.reset();
    state_.sample_index = state_.fraction = 0;
}
bool SampleVoice::Playing() const { return state_.phase != Phase::Off; }
uint64_t SampleVoice::Ends() const { return state_.ends; }
const SampleVoice::State& SampleVoice::Capture() const { return state_; }

bool SampleVoice::Valid(const State& s) {
    if (s.phase > Phase::Release || !std::isfinite(s.envelope) ||
        s.envelope < 0 || s.envelope > 1 || !std::isfinite(s.left) ||
        !std::isfinite(s.right) || s.sample_index >= VagDecoder::kBlockSamples ||
        s.fraction >= 0x1000 || s.parameters.attack > 127 || s.parameters.release > 31 ||
        s.parameters.decay > 15 || s.parameters.sustain > 15) return false;
    if (s.history.h1 < -32768 || s.history.h1 > 32767 ||
        s.history.h2 < -32768 || s.history.h2 > 32767 ||
        (s.parameters.left & 0x8000) || (s.parameters.right & 0x8000)) return false;
    if (!VolumeRamp::Valid(s.left_ramp.Capture()) || !VolumeRamp::Valid(s.right_ramp.Capture())) return false;
    if (s.phase == Phase::Off) return true;
    if (!s.samples || s.samples->empty() || s.samples->size() % VagDecoder::kBlockBytes ||
        s.samples->size() > UINT32_MAX || s.block_offset % VagDecoder::kBlockBytes ||
        s.loop_offset % VagDecoder::kBlockBytes ||
        s.block_offset >= s.samples->size() || s.loop_offset >= s.samples->size()) return false;
    return true;
}
bool SampleVoice::Restore(const State& state) {
    if (!Valid(state)) return false;
    state_ = state;
    return true;
}

void SampleVoice::Decode() {
    // A music window still landing (SampleLanding): the block is in place before it is decoded
    if (SampleLanding::Active())
        SampleLanding::Await(state_.samples.get(), state_.block_offset + VagDecoder::kBlockBytes);
    // (the frame loop of the earlier builds decodes with the division form: the A/B arm keeps their per-sample cost)
    const uint8_t* block = state_.samples->data() + state_.block_offset;
    state_.flags = g_legacy_mix.load(std::memory_order_relaxed)
                       ? VagDecoder::DecodeBlockReference(block, state_.history, state_.block.data())
                       : VagDecoder::DecodeBlock(block, state_.history, state_.block.data());
    state_.sample_index = 0;
    if (state_.flags & VagDecoder::kFlagLoopStart) state_.loop_offset = state_.block_offset;
    ++g_census.blocks;
    g_census.frame_int_divisions += VagDecoder::kBlockSamples;   // the division form: one per decoded sample
}

void SampleVoice::Envelope() {
    const auto& p = state_.parameters;
    const float sustain = static_cast<float>(p.sustain) / 15.0f;
    switch (state_.phase) {
    case Phase::Attack:
        state_.envelope += Ramp(p.attack, 4.0f);
        if (state_.envelope >= 1.0f) {
            state_.envelope = 1.0f;
            state_.phase = sustain >= 1.0f ? Phase::Sustain : Phase::Decay;
        }
        break;
    case Phase::Decay:
        state_.envelope -= Ramp(p.decay, 1.0f);
        if (state_.envelope <= sustain) {
            state_.envelope = sustain;
            state_.phase = Phase::Sustain;
        }
        break;
    case Phase::Release:
        state_.envelope -= p.release == 0 ? 0.25f : Ramp(p.release, 2.0f);
        if (state_.envelope <= 0) Stop();
        break;
    case Phase::Sustain:
    case Phase::Off: break;
    }
}

inline void SampleVoice::StepOne(float& left, float& right) {
    left = right = 0;
    if (!Playing()) return;
    state_.fraction += state_.parameters.pitch;
    while (state_.fraction >= 0x1000) {
        state_.fraction -= 0x1000;
        state_.previous = state_.block[state_.sample_index++];
        if (state_.sample_index < VagDecoder::kBlockSamples) continue;
        if (state_.flags & VagDecoder::kFlagLoopEnd) {
            ++state_.ends;
            if (!(state_.flags & VagDecoder::kFlagLoopRepeat)) {
                Stop();
                return;
            }
            state_.block_offset = state_.loop_offset;
        } else {
            state_.block_offset += VagDecoder::kBlockBytes;
            if (state_.block_offset >= state_.samples->size()) {
                Stop(); // an unterminated asset ends at its owned extent
                return;
            }
        }
        Decode();
    }
    const int32_t current = state_.block[state_.sample_index];
    const int32_t sample = state_.previous +
        (((current - state_.previous) * static_cast<int32_t>(state_.fraction)) >> 12);
    Envelope();
    state_.left_ramp.Step(state_.left);
    state_.right_ramp.Step(state_.right);
    const float value = static_cast<float>(sample) / 32768.0f * state_.envelope;
    left = value * state_.left;
    right = value * state_.right;
}
void SampleVoice::Step(float& left, float& right) { StepOne(left, right); }
void SampleVoice::StepRun(uint32_t frames, uint32_t repeats, float* left, float* right) {
    ++g_census.runs;
    if (!g_legacy_mix.load(std::memory_order_relaxed)) {
        StepBlock(frames, repeats, left, right);
        return;
    }
    // The frame loop (TE_AUDIO_MIX=legacy): the earlier builds' per-frame calls, as they were (+ the census' frame count)
    uint64_t playing = 0;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        playing += state_.phase != Phase::Off;
        StepOne(left[frame], right[frame]);
        if (repeats && state_.ends >= repeats) Stop();
    }
    g_census.frames += playing;
}

namespace {
// The envelope's per-frame values for one run of a voice: what Envelope() computes EVERY frame from the voice's
// parameters -- its sustain level and the step of the phase it is in (Ramp()) -- evaluated here at the first frame of the
// run that uses each, by the same expressions. The parameters change only through the control calls (between mixes) and
// the thread's FP mode does not change inside a mix, so every frame of the run would compute these same values.
struct EnvelopeSteps {
    const SampleVoice::Parameters& p;
    SampleVoice::Census& census;
    float sustain = 0, attack = 0, decay = 0, release = 0;
    bool have_sustain = false, have_attack = false, have_decay = false, have_release = false;
    float Sustain() {
        if (!have_sustain) {
            sustain = static_cast<float>(p.sustain) / 15.0f;
            have_sustain = true;
            ++census.divisions;
        }
        return sustain;
    }
    float Attack() {
        if (!have_attack) {
            attack = Ramp(p.attack, 4.0f);
            have_attack = true;
            if (p.attack) { ++census.pow_calls; ++census.divisions; }
        }
        return attack;
    }
    float Decay() {
        if (!have_decay) {
            decay = Ramp(p.decay, 1.0f);
            have_decay = true;
            if (p.decay) { ++census.pow_calls; ++census.divisions; }
        }
        return decay;
    }
    float Release() {
        if (!have_release) {
            release = p.release == 0 ? 0.25f : Ramp(p.release, 2.0f);
            have_release = true;
            if (p.release) { ++census.pow_calls; ++census.divisions; }
        }
        return release;
    }
};
}  // namespace

// The block loop: StepRun's frames with the voice's per-frame state in locals (written back at the end of the run and
// before every Stop(), which resets part of it). Per frame, in StepOne's order: the cursor and its blocks, the
// interpolated sample, the envelope (Envelope()'s switch, inline), the volume ramps, the outputs, the repeat check.
// Instantiated per (left ramp moving, right ramp moving, sustain level): which of them hold is fixed for a run (the ramps'
// steps and the envelope's phase at its sustain level change only through the control calls), so the loop of a run
// carries no test for the parts that cannot run in it -- the same operations as the frame loop's for every frame.
template <bool kRampLeft, bool kRampRight, bool kSustain>
void SampleVoice::StepBlockLoop(uint32_t frames, uint32_t repeats, float* left, float* right, BlockRun& run) {
    State& s = state_;
    EnvelopeSteps steps{s.parameters, g_census};
    const uint32_t pitch = s.parameters.pitch;
    Phase phase = s.phase;
    uint32_t fraction = s.fraction, index = s.sample_index;
    int32_t previous = s.previous;
    float envelope = s.envelope, gain_left = s.left, gain_right = s.right;
    uint32_t attack = 0, decay = 0, release = 0;
    bool stopped = false, ended = false;   // a Stop() in the run (the cursor ended / the release / the repeats)
    uint32_t frame = 0;
    for (; frame < frames; ++frame) {
        fraction += pitch;
        while (fraction >= 0x1000) {
            fraction -= 0x1000;
            previous = s.block[index++];
            if (index < VagDecoder::kBlockSamples) continue;
            if (s.flags & VagDecoder::kFlagLoopEnd) {
                ++s.ends;
                if (!(s.flags & VagDecoder::kFlagLoopRepeat)) {
                    ended = true;
                    break;
                }
                s.block_offset = s.loop_offset;
            } else {
                s.block_offset += VagDecoder::kBlockBytes;
                if (s.block_offset >= s.samples->size()) {
                    ended = true;   // an unterminated asset ends at its owned extent
                    break;
                }
            }
            Decode();   // the block at block_offset; its sample_index 0
            index = 0;
        }
        if (ended) {
            // StepOne's Stop(); return: this frame's outputs are the +0 it wrote first. The fields Stop() leaves alone
            // keep this run's values (the cursor's are reset by it).
            left[frame] = right[frame] = 0;
            s.previous = static_cast<int16_t>(previous);
            s.envelope = envelope;
            s.left = gain_left;
            s.right = gain_right;
            Stop();
            ++frame;
            stopped = true;
            break;
        }
        const int32_t current = s.block[index];
        const int32_t sample = previous + (((current - previous) * static_cast<int32_t>(fraction)) >> 12);
        bool released = false;
        if (!kSustain) {
            switch (phase) {
            case Phase::Attack:
                ++attack;
                envelope += steps.Attack();
                if (envelope >= 1.0f) {
                    envelope = 1.0f;
                    phase = steps.Sustain() >= 1.0f ? Phase::Sustain : Phase::Decay;
                }
                break;
            case Phase::Decay:
                ++decay;
                envelope -= steps.Decay();
                if (envelope <= steps.Sustain()) {
                    envelope = steps.Sustain();
                    phase = Phase::Sustain;
                }
                break;
            case Phase::Release:
                ++release;
                envelope -= steps.Release();
                if (envelope <= 0) {
                    released = true;   // Envelope()'s Stop(): the envelope is +0 for the rest of the frame
                    envelope = 0;
                }
                break;
            case Phase::Sustain:
            case Phase::Off: break;
            }
        }
        if (kRampLeft) s.left_ramp.Step(gain_left);
        if (kRampRight) s.right_ramp.Step(gain_right);
        const float value = static_cast<float>(sample) / 32768.0f * envelope;
        left[frame] = value * gain_left;
        right[frame] = value * gain_right;
        if (released || (repeats && s.ends >= repeats)) {
            s.previous = static_cast<int16_t>(previous);
            s.envelope = envelope;
            s.left = gain_left;
            s.right = gain_right;
            s.phase = phase;
            s.fraction = fraction;
            s.sample_index = index;
            Stop();   // (the release's, then the repeat check's: the second one changes nothing)
            ++frame;
            stopped = true;
            break;
        }
    }
    if (!stopped) {
        s.phase = phase;
        s.fraction = fraction;
        s.sample_index = index;
        s.previous = static_cast<int16_t>(previous);
        s.envelope = envelope;
        s.left = gain_left;
        s.right = gain_right;
    }
    run.frames = frame;
    run.attack = attack;
    run.decay = decay;
    run.release = release;
    run.stopped = stopped;
    run.ended = ended;
}

void SampleVoice::StepBlock(uint32_t frames, uint32_t repeats, float* left, float* right) {
    State& s = state_;
    uint32_t frame = 0;
    if (s.phase == Phase::Off) {
        // StepOne writes +0 and changes nothing; the repeat check's Stop() then resets the fields it resets, the same
        // values for every frame after the first
        if (frames && repeats && s.ends >= repeats) Stop();
    } else if (frames) {
        // VolumeRamp::Step returns at once while its step is 0; the step changes only through the control calls
        const bool ramp_left = s.left_ramp.Capture().step != 0, ramp_right = s.right_ramp.Capture().step != 0;
        const bool sustain = s.phase == Phase::Sustain;   // the envelope stays at its level for the whole run
        BlockRun run;
        switch ((ramp_left ? 4 : 0) | (ramp_right ? 2 : 0) | (sustain ? 1 : 0)) {
        case 0: StepBlockLoop<false, false, false>(frames, repeats, left, right, run); break;
        case 1: StepBlockLoop<false, false, true>(frames, repeats, left, right, run); break;
        case 2: StepBlockLoop<false, true, false>(frames, repeats, left, right, run); break;
        case 3: StepBlockLoop<false, true, true>(frames, repeats, left, right, run); break;
        case 4: StepBlockLoop<true, false, false>(frames, repeats, left, right, run); break;
        case 5: StepBlockLoop<true, false, true>(frames, repeats, left, right, run); break;
        case 6: StepBlockLoop<true, true, false>(frames, repeats, left, right, run); break;
        default: StepBlockLoop<true, true, true>(frames, repeats, left, right, run); break;
        }
        frame = run.frames;
        // the census: `frame` frames were stepped playing (the last one possibly ending the voice before its envelope)
        Census& census = g_census;
        const uint64_t enveloped = frame - (run.ended ? 1u : 0u);
        census.frames += frame;
        census.attack += run.attack;
        census.decay += run.decay;
        census.release += run.release;
        if (ramp_left || ramp_right) census.ramped += enveloped;
        census.stops += run.stopped;
        census.frame_calls += enveloped;
        census.frame_ramp_tests += 2 * enveloped;
        census.frame_divisions += enveloped;   // the sustain level, every Envelope() call
        const uint64_t pows = (s.parameters.attack ? run.attack : 0) + (s.parameters.decay ? run.decay : 0) +
                              (s.parameters.release ? run.release : 0);
        census.frame_pow_calls += pows;
        census.frame_divisions += pows;
    }
    for (; frame < frames; ++frame) left[frame] = right[frame] = 0;
}

void SampleVoice::StepPaused() {
    if (Playing()) Envelope();
}

void SampleVoice::StepPausedRun(uint32_t frames) {
    if (!g_legacy_mix.load(std::memory_order_relaxed)) {
        StepPausedBlock(frames);
        return;
    }
    uint64_t playing = 0;
    for (uint32_t frame = 0; frame < frames && Playing(); ++frame) {
        StepPaused();
        ++playing;
    }
    g_census.held += playing;
}

// StepPausedRun's block form: Envelope() per frame while the voice plays, with the steps of the run evaluated once; a
// voice at its sustain level (or one that reaches it) is unchanged by the rest of the run's frames.
void SampleVoice::StepPausedBlock(uint32_t frames) {
    State& s = state_;
    if (s.phase == Phase::Off || frames == 0) return;
    Census& census = g_census;
    EnvelopeSteps steps{s.parameters, census};
    Phase phase = s.phase;
    float envelope = s.envelope;
    uint64_t attack = 0, decay = 0, release = 0;
    bool stopped = false;
    for (uint32_t frame = 0; frame < frames && phase != Phase::Sustain; ++frame) {
        if (phase == Phase::Attack) {
            ++attack;
            envelope += steps.Attack();
            if (envelope >= 1.0f) {
                envelope = 1.0f;
                phase = steps.Sustain() >= 1.0f ? Phase::Sustain : Phase::Decay;
            }
        } else if (phase == Phase::Decay) {
            ++decay;
            envelope -= steps.Decay();
            if (envelope <= steps.Sustain()) {
                envelope = steps.Sustain();
                phase = Phase::Sustain;
            }
        } else {   // Release (Off ends the loop below)
            ++release;
            envelope -= steps.Release();
            if (envelope <= 0) {
                Stop();   // phase Off, envelope +0
                stopped = true;
                break;
            }
        }
    }
    if (!stopped) {
        s.phase = phase;
        s.envelope = envelope;
    }
    // the frame loop calls Envelope() for every frame the voice plays: all of them unless the release ended it
    const uint64_t enveloped = stopped ? attack + decay + release : frames;
    census.held += enveloped;
    census.frame_calls += enveloped;
    census.frame_divisions += enveloped;
    const uint64_t pows = (s.parameters.attack ? attack : 0) + (s.parameters.decay ? decay : 0) +
                          (s.parameters.release ? release : 0);
    census.frame_pow_calls += pows;
    census.frame_divisions += pows;
}

void SampleVoice::SetLegacyMix(bool legacy) { g_legacy_mix.store(legacy, std::memory_order_relaxed); }
bool SampleVoice::LegacyMix() { return g_legacy_mix.load(std::memory_order_relaxed); }
SampleVoice::Census& SampleVoice::MixCensus() { return g_census; }
} // namespace title
