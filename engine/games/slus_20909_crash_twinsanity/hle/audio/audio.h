#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/audio/sample_voice.h"

namespace title {

// Game-owned sample registry and voice controls. Only the game execution
// owner mutates/mixes this service; the device consumes completed PCM blocks.
// Banks identify immutable assets, never addresses inside guest SPU memory.
class Audio {
public:
    static constexpr uint32_t kVoices = 48, kGroups = 4, kFramesPerTick = 800;
    struct LoadTicket {
        uint32_t sound = UINT32_MAX;
        uint64_t generation = 0, sequence = 0;
        explicit operator bool() const { return generation != 0 && sequence != 0; }
    };
    struct Sample {
        uint64_t generation = 0, load_sequence = 0;
        SampleVoice::Samples bytes;
        bool ready = false;
    };
    struct Voice {
        SampleVoice player;
        uint32_t sound = UINT32_MAX, group = 0;
        uint16_t left = 0, right = 0;
        uint32_t repeats = 0;
        uint32_t music_stream = UINT32_MAX;
        uint64_t music_generation = 0;
        bool music_started = false;
        // Held by the service pause that found this voice playing. The title
        // pauses the sound on the title->menu transition and on a movie start,
        // then keys menu samples and the next music pair while paused; the
        // module keeps the voices it paused and plays the ones keyed after.
        bool held = false;
    };
    struct State {
        uint64_t generation = 1, ticks = 0;
        uint32_t sample_limit = 0;
        bool paused = false, movie_owned = false;
        std::map<uint32_t, Sample> samples;
        std::array<Voice, kVoices> voices;
        std::array<uint16_t, kGroups> group_left{0x1000, 0x1000, 0x1000, 0x1000};
        std::array<uint16_t, kGroups> group_right{0x1000, 0x1000, 0x1000, 0x1000};
    };
    struct Snapshot {
        State playback;
        uint64_t load_generation = 1, load_sequence = 0;
        std::map<uint32_t, uint64_t> pending_loads;
    };

    void Reset();
    void ReserveSounds(uint16_t count);
    // A request owns its ticket through read/delivery. Free, replacement,
    // reset and restore invalidate it before its late bytes can be published.
    LoadTicket BeginSoundLoad(uint32_t id);
    bool CompleteSoundLoad(LoadTicket ticket, std::vector<uint8_t> bytes);
    bool CancelSoundLoad(LoadTicket ticket);
    bool SoundBankLoaded(LoadTicket ticket);
    // Complete owned bank bytes first, then expose readiness at SoundBankLoaded.
    bool StoreSound(uint32_t id, std::vector<uint8_t> bytes);
    bool SoundBankLoaded(uint32_t id);
    bool ReleaseSound(uint32_t id);
    bool HasSound(uint32_t id) const;
    int32_t PlaySound(uint32_t id, uint32_t voice_and_group, int16_t left, int16_t right,
                      uint16_t pitch, uint32_t attack, uint32_t release, uint32_t loops);
    // Preparing a submitted play takes an immutable lease. A later bank free
    // cannot invalidate the play before the command batch is accepted.
    int32_t PrepareSound(uint32_t id, uint32_t voice_and_group, int16_t left, int16_t right,
                         uint16_t pitch, uint32_t attack, uint32_t release, uint32_t loops,
                         Voice& voice) const;
    bool StartVoice(uint32_t index, Voice voice);
    bool ReserveMusicVoice(uint32_t stream, uint64_t generation, uint32_t voice_and_group,
                           uint16_t pitch, uint16_t left, uint16_t right, uint16_t raw_left,
                           uint16_t raw_right, uint8_t attack = 0, uint8_t release = 0);
    bool StartMusicVoice(uint32_t stream, uint64_t generation, uint32_t channel,
                         SampleVoice::Samples samples);
    void StopMusicVoice(uint32_t stream, uint64_t generation, uint32_t channel);
    bool IsMusicVoice(uint32_t channel) const;
    int32_t SetVoiceVolume(uint32_t voice, int16_t left, int16_t right);
    int32_t SetVoiceLevels(uint32_t voice, uint16_t raw_left, uint16_t raw_right,
                           uint16_t left, uint16_t right, bool smooth);
    bool SetVoiceGroup(uint32_t voice, uint32_t group);
    int32_t SetVoicePitch(uint32_t voice, uint16_t pitch);
    int32_t SetVoiceRelease(uint32_t voice, uint32_t rate);
    int32_t KeyOff(uint32_t voice);
    int32_t ReleaseVoice(uint32_t voice, uint32_t rate);
    int32_t IsVoiceFree(uint32_t voice) const;
    bool OwnsVoice(uint32_t voice) const;
    // The common mixer asks before stepping the retained voice. A handled
    // silent voice still suppresses that old voice until an explicit handoff.
    bool StepVoice(uint32_t voice, float& left, float& right);
    // `frames` StepVoice calls of one OWNED voice (the mixer's block form, VoiceSource::StepVoices): frame f's output into
    // left[f] / right[f], the voice's state stepped exactly as by those calls. Returns false, writing nothing, when every
    // output is +0 (the voice is held or not playing: its outputs are the +0 StepVoice writes). Voices step independently
    // of one another: no voice's step reads or writes another's state.
    bool StepVoices(uint32_t voice, uint32_t frames, float* left, float* right);
    void BeginMixTick();
    // Explicit service update: callers may share this with the owned command
    // batch/clock. It does not advance playback or read host wall time.
    void AdvanceControls();
    // The music adapter calls this before assigning the channel to a stream.
    void HandOffVoice(uint32_t voice);
    void StopVoices();
    int32_t SetGroupVolume(uint32_t group, uint32_t left, uint32_t right);
    int32_t StoreGroupVolume(uint32_t group, uint32_t left, uint32_t right);
    // Pause holds every voice playing at that moment; a voice started or
    // keyed on afterwards is not held. Resume releases the held voices at
    // their stored volume targets.
    void PauseAll();
    void ResumeAll();
    void LendToMovie();
    void ReclaimFromMovie();
    // An output frame count, not wall time, drives playback. Pause freezes
    // the sample cursor and mutes output; envelopes retain this sample clock.
    std::vector<float> MixTick();
    const State& Inspect() const;
    // Pending payloads belong to the stream owner. Until that owner's request
    // is captured too, decline rather than save an unresumable partial bank.
    bool Capture(State& state) const;
    bool Restore(const State& state);
    static bool Validate(const State& state);
    static bool Validate(const Voice& voice);
    // The stream owner must capture every pending ticket with its payload.
    // Restore renews the delivery epoch; rewrite those saved payloads to the
    // returned generation before restoring the file schedule.
    Snapshot CaptureLoads() const;
    static bool Validate(const Snapshot& state);
    bool CanRestoreLoads(const Snapshot& state) const;
    bool RestoreLoads(const Snapshot& state, uint64_t& generation);
    static int32_t PitchOfRate(int32_t rate);

private:
    static uint16_t GroupScale(uint16_t volume, uint16_t group);
    void ApplyVolumes(Voice& voice, bool smooth = false);
    bool Current(LoadTicket ticket) const;
    void InvalidateLoads();
    State state_;
    uint64_t load_generation_ = 1, load_sequence_ = 0;
    std::map<uint32_t, uint64_t> pending_loads_;
};
} // namespace title
