#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/sound/vag_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/volume_ramp.h"

namespace title {

// Sample assets whose bytes are still LANDING: a music window the platform reads in pieces on its disc reader thread while
// the voice already plays the first ones (the PS2's STREAM.IRX fed the SPU2 the same way, block by block). The asset is
// allocated whole first (its storage never moves) and its bytes land in order, front to back; a Decode of a block past
// the landed mark waits for it, so a voice plays exactly the bytes it would have played from a whole read -- only the
// wall time of the read changes. No asset landing (the common case): Decode pays one relaxed load. Slots and marks live
// in static storage (a waiter never reads freed memory).
class SampleLanding {
public:
    static constexpr int kSlots = 8;
    // Claims a slot for the asset (`landed` bytes in place so far); -1 when every slot is taken (the caller lands the
    // asset whole before any voice plays it).
    static int Begin(const std::vector<uint8_t>* asset, uint32_t landed);
    // The bytes below `landed` are in place (store-release: a waiter that sees the mark sees the bytes)
    static void Advance(int slot, uint32_t landed);
    // The asset is whole (or abandoned): its slot is free again
    static void End(int slot);
    // Until [0, end) of the asset has landed (an asset in no slot is whole)
    static void Await(const std::vector<uint8_t>* asset, uint32_t end);
    static bool Active() { return active_.load(std::memory_order_relaxed) != 0; }
    // Decodes that had to wait, and the time they waited (the census)
    static uint64_t Waits();
    static double WaitedMs();

private:
    static std::atomic<int> active_;
};

// Playback arithmetic over one immutable sample asset. There is no SPU RAM,
// register window, address mask, libsd command queue or guest pointer here.
class SampleVoice {
public:
    using Samples = std::shared_ptr<const std::vector<uint8_t>>;
    enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release };
    struct Parameters {
        uint16_t pitch = 0x1000;
        uint16_t left = 0x3fff, right = 0x3fff;
        uint8_t attack = 0, decay = 0, sustain = 15, release = 0;
    };
    struct State {
        Samples samples;
        Parameters parameters;
        Phase phase = Phase::Off;
        uint32_t block_offset = 0, loop_offset = 0, fraction = 0, sample_index = 0;
        VagDecoder::History history;
        std::array<int16_t, VagDecoder::kBlockSamples> block{};
        uint8_t flags = 0;
        int16_t previous = 0;
        float envelope = 0, left = 0, right = 0;
        VolumeRamp left_ramp, right_ramp;
        uint64_t ends = 0;
    };

    bool Start(Samples samples, Parameters parameters);
    void SetPitch(uint16_t pitch);
    void SetVolumes(uint16_t left, uint16_t right);
    void RampVolumes(uint16_t left, uint16_t right);
    void AdvanceControls();
    void SetRelease(uint8_t rate);
    void Release(uint8_t rate);
    void Stop();
    void Step(float& left, float& right);
    // `frames` Steps (output f into left[f] / right[f]), each followed by the repeat check title::Audio::StepVoice
    // makes (Stop() once a nonzero `repeats` <= Ends()): exactly the calls of that loop, without a call per sample.
    // The block loop (the default): the voice's state in registers through the run, the envelope inline with its
    // per-phase step and sustain level evaluated once per run (the parameters do not change within a run -- they change
    // only through the control calls, between mixes -- and neither does the thread's FP mode, so each is the value the
    // frame loop computes every frame), the volume ramps' steps only while a ramp moves. Same operations on the same
    // values in the same order per frame: the same bits (SetLegacyMix(true): the frame loop itself, for an A/B).
    void StepRun(uint32_t frames, uint32_t repeats, float* left, float* right);
    // Muted pause keeps the sample cursor fixed while release/attack still
    // follows the output-sample clock. Resume restores the requested gains.
    void StepPaused();
    // `frames` StepPaused calls, stopping once the voice is off (title::Audio::StepVoices' held voice): the envelope
    // only, its step evaluated once per run; a voice in its sustain level is left alone for the rest of the run.
    void StepPausedRun(uint32_t frames);
    bool Playing() const;
    uint64_t Ends() const;
    const State& Capture() const;
    bool Restore(const State& state);

    // The mix's form, process-wide (set before any voice is mixed; the audio owner reads TE_AUDIO_MIX): the block loop
    // (false, the default) or the frame loop of the earlier builds (true: StepOne per frame, the envelope per frame, the
    // division-form ADPCM decode) -- an A/B arm for the device and the identity check: both give the same bits.
    static void SetLegacyMix(bool legacy);
    static bool LegacyMix();
    // The block loop's work census (the mixing thread adds to it under the audio owner's lock; the owner reads it there).
    // `frames` = voice-frames stepped while playing; of them, the ones in an attack / decay / release ramp of the
    // envelope (the rest sustain) and the ones with a volume ramp moving; `held` = paused voice-frames (the envelope
    // only); `blocks` = ADPCM blocks decoded (28 samples each). The ops the block loop made (`pow_calls` = envelope steps
    // computed with powf, `divisions` = float divisions: the sustain level and the steps' reciprocals) and the ones the
    // frame loop makes for the same frames (`frame_*`: per frame an Envelope() call with its sustain division and two
    // ramp tests, per ramping frame at a nonzero rate one powf + one more division, per decoded sample one integer
    // division -- the earlier code, from its disassembly). The frame loop (SetLegacyMix(true)) adds nothing here.
    struct Census {
        uint64_t runs = 0, frames = 0, attack = 0, decay = 0, release = 0, ramped = 0, held = 0, blocks = 0, stops = 0;
        uint64_t pow_calls = 0, divisions = 0;
        uint64_t frame_calls = 0, frame_pow_calls = 0, frame_divisions = 0, frame_int_divisions = 0, frame_ramp_tests = 0;
    };
    static Census& MixCensus();

private:
    static bool Valid(const State& state);
    static float Volume(uint16_t value);
    void Decode();
    void Envelope();
    inline void StepOne(float& left, float& right);
    void StepBlock(uint32_t frames, uint32_t repeats, float* left, float* right);
    struct BlockRun {
        uint32_t frames = 0, attack = 0, decay = 0, release = 0;
        bool stopped = false, ended = false;
    };
    template <bool kRampLeft, bool kRampRight, bool kSustain>
    void StepBlockLoop(uint32_t frames, uint32_t repeats, float* left, float* right, BlockRun& run);
    void StepPausedBlock(uint32_t frames);
    State state_;
};

} // namespace title
