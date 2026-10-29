#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"

#include <algorithm>
#include <utility>

namespace title {
namespace {
bool ValidSample(const std::vector<uint8_t>& bytes) {
    return !bytes.empty() && bytes.size() % VagDecoder::kBlockBytes == 0 && bytes.size() <= UINT32_MAX;
}
}
void Audio::InvalidateLoads() {
    pending_loads_.clear();
    if (load_generation_ < UINT64_MAX) ++load_generation_;
    else load_sequence_ = UINT64_MAX; // exhausted identities must never alias
}
void Audio::Reset() {
    InvalidateLoads();
    const auto next = state_.generation == UINT64_MAX ? UINT64_MAX : state_.generation + 1;
    state_ = {};
    state_.generation = next;
}
void Audio::ReserveSounds(uint16_t count) {
    // The source reserves a table of count entries, each with an independent
    // 32-bit sound label. It does not bound labels: the title jingle is 0x8000.
    // NEW's command implementation b3e4 allocates only once; a repeat reserve
    // or zero count leaves the existing table and its pending loads intact.
    if (count == 0 || state_.sample_limit != 0) return;
    state_.sample_limit = count;
}
Audio::LoadTicket Audio::BeginSoundLoad(uint32_t id) {
    if (id == UINT32_MAX || load_sequence_ == UINT64_MAX ||
        (!state_.samples.count(id) && !pending_loads_.count(id) &&
         state_.samples.size() + pending_loads_.size() >= state_.sample_limit)) return {};
    const auto sequence = ++load_sequence_;
    pending_loads_[id] = sequence;
    state_.samples.erase(id);
    return {id, load_generation_, sequence};
}
bool Audio::Current(LoadTicket ticket) const {
    return ticket && ticket.generation == load_generation_;
}
bool Audio::CompleteSoundLoad(LoadTicket ticket, std::vector<uint8_t> bytes) {
    const auto pending = pending_loads_.find(ticket.sound);
    if (!Current(ticket) || pending == pending_loads_.end() || pending->second != ticket.sequence ||
        !ValidSample(bytes)) return false;
    Sample sample;
    sample.generation = state_.generation;
    sample.load_sequence = ticket.sequence;
    sample.bytes = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    state_.samples[ticket.sound] = std::move(sample);
    pending_loads_.erase(pending);
    return true;
}
bool Audio::CancelSoundLoad(LoadTicket ticket) {
    const auto pending = pending_loads_.find(ticket.sound);
    if (!Current(ticket) || pending == pending_loads_.end() || pending->second != ticket.sequence) return false;
    pending_loads_.erase(pending);
    return true;
}
bool Audio::StoreSound(uint32_t id, std::vector<uint8_t> bytes) {
    if (!ValidSample(bytes)) return false;
    const auto ticket = BeginSoundLoad(id);
    return ticket && CompleteSoundLoad(ticket, std::move(bytes));
}
bool Audio::SoundBankLoaded(LoadTicket ticket) {
    const auto sample = state_.samples.find(ticket.sound);
    if (!Current(ticket) || sample == state_.samples.end() ||
        sample->second.load_sequence != ticket.sequence) return false;
    return SoundBankLoaded(ticket.sound);
}
bool Audio::SoundBankLoaded(uint32_t id) {
    const auto it = state_.samples.find(id);
    if (it == state_.samples.end() || it->second.generation != state_.generation) return false;
    it->second.ready = true;
    return true;
}
bool Audio::ReleaseSound(uint32_t id) {
    // A playing voice holds a lease on its immutable asset until it stops.
    // Unloading prevents future plays without exposing freed storage.
    const bool pending = pending_loads_.erase(id) != 0;
    return state_.samples.erase(id) != 0 || pending;
}
bool Audio::HasSound(uint32_t id) const { return state_.samples.count(id) != 0; }
uint16_t Audio::GroupScale(uint16_t volume, uint16_t group) {
    const bool inverted = volume > 0x3fff;
    const uint32_t magnitude = inverted ? 0x7fff - volume : volume;
    const uint32_t scaled = magnitude * group / 0x1000;
    return static_cast<uint16_t>(inverted ? 0x7fff - scaled : scaled);
}
void Audio::ApplyVolumes(Voice& voice, bool smooth) {
    const uint32_t group = voice.group;
    const auto left = group ? GroupScale(voice.left, state_.group_left[group - 1]) : voice.left;
    const auto right = group ? GroupScale(voice.right, state_.group_right[group - 1]) : voice.right;
    if (smooth) voice.player.RampVolumes(left, right);
    else voice.player.SetVolumes(left, right);
}
int32_t Audio::PlaySound(uint32_t id, uint32_t voice_and_group, int16_t left, int16_t right,
                        uint16_t pitch, uint32_t attack, uint32_t release, uint32_t loops) {
    Voice voice;
    const auto result = PrepareSound(id, voice_and_group, left, right, pitch, attack, release, loops, voice);
    if (result) return result;
    state_.voices[voice_and_group & 0xffff] = std::move(voice);
    return 0;
}
int32_t Audio::PrepareSound(uint32_t id, uint32_t voice_and_group, int16_t left, int16_t right,
                           uint16_t pitch, uint32_t attack, uint32_t release, uint32_t loops,
                           Voice& prepared) const {
    const uint32_t index = voice_and_group & 0xffff;
    const uint32_t group = voice_and_group >> 16;
    if (index >= kVoices || group > kGroups) return -1;
    if (loops > 0xffff) return -2;
    const auto sample = state_.samples.find(id);
    if (sample == state_.samples.end() || !sample->second.ready) return -3;
    if (attack > 127 || release > 31 || left < 0 || right < 0) return -4;
    Voice voice;
    voice.sound = id;
    voice.group = group;
    voice.left = static_cast<uint16_t>(left);
    voice.right = static_cast<uint16_t>(right);
    voice.repeats = loops;
    SampleVoice::Parameters parameters;
    parameters.pitch = pitch;
    parameters.attack = static_cast<uint8_t>(attack);
    parameters.decay = 15; // the title's play intent uses ADSR1's low byte 0xff
    parameters.release = static_cast<uint8_t>(release);
    if (!voice.player.Start(sample->second.bytes, parameters)) return -4;
    voice.player.SetVolumes(group ? GroupScale(voice.left, state_.group_left[group - 1]) : voice.left,
                            group ? GroupScale(voice.right, state_.group_right[group - 1]) : voice.right);
    prepared = std::move(voice);
    return 0;
}
bool Audio::StartVoice(uint32_t index, Voice voice) {
    if (index >= kVoices || voice.sound == UINT32_MAX || voice.group > kGroups ||
        !voice.player.Playing()) return false;
    voice.held = false; // keyed after any pause in force
    state_.voices[index] = std::move(voice);
    return true;
}
bool Audio::ReserveMusicVoice(uint32_t stream, uint64_t generation, uint32_t encoded,
                             uint16_t pitch, uint16_t left, uint16_t right, uint16_t raw_left,
                             uint16_t raw_right, uint8_t attack, uint8_t release) {
    const uint32_t channel = encoded & 0xffff, group = encoded >> 16;
    if (stream >= kVoices || generation == 0 || channel >= kVoices || group > kGroups ||
        ((left | right | raw_left | raw_right) & 0x8000) || attack > 127 || release > 31) return false;
    Voice voice;
    voice.group = group; voice.left = raw_left; voice.right = raw_right;
    voice.music_stream = stream; voice.music_generation = generation;
    voice.player.SetPitch(pitch);
    voice.player.SetVolumes(left, right);
    auto controls = voice.player.Capture();
    controls.parameters.attack = attack; controls.parameters.release = release;
    if (!voice.player.Restore(controls)) return false;
    state_.voices[channel] = std::move(voice);
    return true;
}
bool Audio::StartMusicVoice(uint32_t stream, uint64_t generation, uint32_t channel,
                           SampleVoice::Samples samples) {
    if (channel >= kVoices) return false;
    auto& voice = state_.voices[channel];
    if (voice.music_stream != stream || voice.music_generation != generation || voice.music_started) return false;
    const auto controls = voice.player.Capture();
    SampleVoice player;
    auto parameters = controls.parameters;
    parameters.decay = 15;
    if (!player.Start(std::move(samples), parameters)) return false;
    // AllowKeyOn uses the requested targets as initial fixed levels:
    // STREAM bc28/c134 -> 2908/2988 -> 72c0, for parent and child. A ramp
    // requested while prepared is not a fade from the parked silence voice.
    voice.player = std::move(player);
    voice.music_started = true;
    voice.held = false; // keyed after any pause in force
    return true;
}
void Audio::StopMusicVoice(uint32_t stream, uint64_t generation, uint32_t channel) {
    if (channel >= kVoices) return;
    auto& voice = state_.voices[channel];
    if (voice.music_stream != stream || voice.music_generation != generation) return;
    voice.player.Stop();
    voice.music_started = true; // retain silent ownership until explicit reuse
}
bool Audio::IsMusicVoice(uint32_t channel) const {
    return channel < kVoices && state_.voices[channel].music_stream != UINT32_MAX;
}
int32_t Audio::SetVoiceVolume(uint32_t index, int16_t left, int16_t right) {
    if (index >= kVoices) return -1;
    if (left < 0 || right < 0) return -2;
    auto& voice = state_.voices[index];
    voice.left = static_cast<uint16_t>(left);
    voice.right = static_cast<uint16_t>(right);
    ApplyVolumes(voice, true);
    return 0;
}
int32_t Audio::SetVoiceLevels(uint32_t index, uint16_t raw_left, uint16_t raw_right,
                            uint16_t left, uint16_t right, bool smooth) {
    if (index >= kVoices) return -1;
    if ((raw_left | raw_right | left | right) & 0x8000) return -2;
    auto& voice = state_.voices[index];
    voice.left = raw_left; voice.right = raw_right;
    if (smooth) voice.player.RampVolumes(left, right);
    else voice.player.SetVolumes(left, right);
    return 0;
}
bool Audio::SetVoiceGroup(uint32_t index, uint32_t group) {
    if (index >= kVoices || group > kGroups) return false;
    state_.voices[index].group = group;
    return true;
}
int32_t Audio::SetVoicePitch(uint32_t index, uint16_t pitch) {
    if (index >= kVoices) return -1;
    state_.voices[index].player.SetPitch(pitch);
    return 0;
}
int32_t Audio::ReleaseVoice(uint32_t index, uint32_t rate) {
    const auto result = SetVoiceRelease(index, rate);
    return result ? result : KeyOff(index);
}
int32_t Audio::SetVoiceRelease(uint32_t index, uint32_t rate) {
    if (rate >= 32) return -1;
    if (index >= kVoices) return -2;
    state_.voices[index].player.SetRelease(static_cast<uint8_t>(rate));
    return 0;
}
int32_t Audio::KeyOff(uint32_t index) {
    if (index >= kVoices) return -1;
    auto& player = state_.voices[index].player;
    player.Release(player.Capture().parameters.release);
    return 0;
}
int32_t Audio::IsVoiceFree(uint32_t index) const {
    if (index >= kVoices) return -1;
    const auto& voice = state_.voices[index];
    if (IsMusicVoice(index) && !voice.music_started) return 0;
    return !voice.player.Playing();
}
bool Audio::OwnsVoice(uint32_t index) const {
    return index < kVoices && (state_.voices[index].sound != UINT32_MAX || IsMusicVoice(index));
}
bool Audio::StepVoice(uint32_t index, float& left, float& right) {
    if (!OwnsVoice(index)) return false;
    left = right = 0;
    auto& voice = state_.voices[index];
    if (!voice.held) {
        voice.player.Step(left, right);
        if (voice.repeats && voice.player.Ends() >= voice.repeats) voice.player.Stop();
    } else voice.player.StepPaused();
    return true;
}
bool Audio::StepVoices(uint32_t index, uint32_t frames, float* left, float* right) {
    // Exactly `frames` StepVoice(index) calls of an owned voice. `held` and `repeats` change only through the control
    // calls, never inside a mix; a stopped player stays stopped through it (only Start revives one).
    auto& voice = state_.voices[index];
    auto& player = voice.player;
    if (voice.held) {
        // StepVoice writes +0 and only the envelope runs (StepPaused: nothing at all once the player is off)
        player.StepPausedRun(frames);
        return false;
    }
    if (!player.Playing()) {
        // Step writes +0 and changes nothing; the repeat check's Stop is idempotent on a stopped player (it only resets
        // the fields Stop sets), so one check stands for all `frames` of them. Ends() does not move while stopped.
        if (frames && voice.repeats && player.Ends() >= voice.repeats) player.Stop();
        return false;
    }
    player.StepRun(frames, voice.repeats, left, right);   // Step + the repeat check, frame by frame
    return true;
}
void Audio::BeginMixTick() { ++state_.ticks; }
void Audio::AdvanceControls() {
    for (auto& voice : state_.voices) if (!voice.held) voice.player.AdvanceControls();
}
void Audio::HandOffVoice(uint32_t index) {
    if (index < kVoices) state_.voices[index] = {};
}
void Audio::StopVoices() {
    state_.voices = {};
    state_.paused = false;
}
int32_t Audio::StoreGroupVolume(uint32_t encoded, uint32_t left, uint32_t right) {
    const auto group = encoded >> 16;
    if (group == 0 || group > kGroups) return -1;
    if (left > 0x1000) return -2;
    if (right > 0x1000) return -3;
    state_.group_left[group - 1] = static_cast<uint16_t>(left);
    state_.group_right[group - 1] = static_cast<uint16_t>(right);
    return 0;
}
int32_t Audio::SetGroupVolume(uint32_t encoded, uint32_t left, uint32_t right) {
    const auto result = StoreGroupVolume(encoded, left, right);
    if (result) return result;
    const auto group = encoded >> 16;
    for (auto& voice : state_.voices) if (voice.group == group) ApplyVolumes(voice);
    return 0;
}
void Audio::PauseAll() {
    // The module's pause command holds the channels it finds keyed; the
    // title's menu samples and its next music pair are keyed on afterwards
    // and play (SoundControlPausedPlayOracle, the same inputs through the
    // actual module arm). A reserved, not yet started music voice is not
    // playing and is therefore not held either.
    state_.paused = true;
    for (auto& voice : state_.voices) if (voice.player.Playing()) voice.held = true;
}
void Audio::ResumeAll() {
    // NEW's source command 84c4/8b48 restores the stored left/right targets
    // when the channels resume. An interrupted ramp does not resume from its
    // pre-pause intermediate gain (OLD 's re-arm contract).
    for (auto& voice : state_.voices) {
        const auto parameters = voice.player.Capture().parameters;
        voice.player.SetVolumes(parameters.left, parameters.right);
        voice.held = false;
    }
    state_.paused = false;
}
void Audio::LendToMovie() { state_.movie_owned = true; }
void Audio::ReclaimFromMovie() { state_.movie_owned = false; }
std::vector<float> Audio::MixTick() {
    std::vector<float> output(kFramesPerTick * 2);
    BeginMixTick();
    for (uint32_t frame = 0; frame < kFramesPerTick; ++frame) {
        float left = 0, right = 0;
        for (uint32_t index = 0; index < kVoices; ++index) {
            float l = 0, r = 0;
            StepVoice(index, l, r);
            left += l; right += r;
        }
        output[frame * 2] = std::clamp(left, -1.0f, 1.0f);
        output[frame * 2 + 1] = std::clamp(right, -1.0f, 1.0f);
    }
    return output;
}
const Audio::State& Audio::Inspect() const { return state_; }
bool Audio::Capture(State& state) const {
    if (!pending_loads_.empty()) return false;
    state = state_;
    return true;
}
bool Audio::Restore(const State& state) {
    if (!Validate(state)) return false;
    InvalidateLoads();
    state_ = state;
    for (const auto& sample : state.samples)
        load_sequence_ = std::max(load_sequence_, sample.second.load_sequence);
    return true;
}
bool Audio::Validate(const State& state) {
    if (state.generation == 0 || state.sample_limit > 0xffff ||
        state.samples.size() > state.sample_limit) return false;
    for (const auto& pair : state.samples) {
        const auto& sample = pair.second;
        if (pair.first == UINT32_MAX || sample.generation != state.generation ||
            sample.load_sequence == 0 || !sample.bytes || !ValidSample(*sample.bytes)) return false;
    }
    for (uint32_t group = 0; group < kGroups; ++group)
        if (state.group_left[group] > 0x1000 || state.group_right[group] > 0x1000) return false;
    for (const auto& voice : state.voices) if (!Validate(voice)) return false;
    return true;
}
bool Audio::Validate(const Voice& voice) {
    SampleVoice check;
    if (voice.group > kGroups || voice.left > 0x7fff || voice.right > 0x7fff ||
        voice.repeats > 0xffff || !check.Restore(voice.player.Capture())) return false;
    return voice.music_stream == UINT32_MAX ||
        (voice.music_stream < kVoices && voice.music_generation != 0 && voice.sound == UINT32_MAX);
}
Audio::Snapshot Audio::CaptureLoads() const {
    return {state_, load_generation_, load_sequence_, pending_loads_};
}
bool Audio::Validate(const Snapshot& state) {
    if (!Validate(state.playback) || state.load_generation == 0 ||
        state.pending_loads.size() + state.playback.samples.size() > state.playback.sample_limit) return false;
    for (const auto& sample : state.playback.samples)
        if (sample.second.load_sequence > state.load_sequence) return false;
    for (const auto& pending : state.pending_loads)
        if (pending.first == UINT32_MAX || pending.second == 0 || pending.second > state.load_sequence ||
            state.playback.samples.count(pending.first)) return false;
    return true;
}
bool Audio::CanRestoreLoads(const Snapshot& state) const {
    return Validate(state) && std::max(load_generation_, state.load_generation) < UINT64_MAX;
}
bool Audio::RestoreLoads(const Snapshot& state, uint64_t& generation) {
    if (!CanRestoreLoads(state)) return false;
    // In-flight completions from either this process or an earlier restore
    // cannot publish into the renewed owner, even when IDs/sequences match.
    load_generation_ = std::max(load_generation_, state.load_generation) + 1;
    load_sequence_ = std::max(load_sequence_, state.load_sequence);
    pending_loads_ = state.pending_loads;
    state_ = state.playback;
    generation = load_generation_;
    return true;
}
int32_t Audio::PitchOfRate(int32_t rate) {
    return static_cast<int32_t>(static_cast<int64_t>(rate) * 0x1000 / 48000);
}
} // namespace title
