#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "engine/sound/voice.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (false)

std::vector<uint8_t> Sample(uint32_t blocks, bool loop, uint32_t seed) {
    std::mt19937 random(seed);
    std::vector<uint8_t> result(blocks * VagDecoder::kBlockBytes);
    for (uint32_t block = 0; block < blocks; ++block) {
        const auto offset = block * VagDecoder::kBlockBytes;
        result[offset] = static_cast<uint8_t>((random() % 5) * 16 + random() % 13);
        for (int byte = 2; byte < VagDecoder::kBlockBytes; ++byte)
            result[offset + byte] = static_cast<uint8_t>(random());
    }
    result[1] = 4;
    result[result.size() - VagDecoder::kBlockBytes + 1] = loop ? 3 : 1;
    return result;
}
bool Silent(const std::vector<float>& samples) {
    return std::all_of(samples.begin(), samples.end(), [](float v) { return v == 0; });
}
title::Audio::State Save(const title::Audio& audio) {
    title::Audio::State state;
    CHECK(audio.Capture(state));
    return state;
}
}

int main() {
    // SELF-ORACLE: same immutable ADPCM blocks and controls into NEW's retained
    // Voice arithmetic and the owned SampleVoice. The model's 2 MB memory is
    // a test fixture only; production Audio never allocates or addresses it.
    std::mt19937 random(0x51a81);
    std::vector<uint8_t> ram(ps2r::sound::kSpuRamSize);
    uint64_t compared = 0, mismatches = 0, silent_signs = 0;
    for (uint32_t test = 0; test < 96; ++test) {
        const auto bytes = Sample(2 + test % 13, test % 2 == 0, test + 3);
        std::copy(bytes.begin(), bytes.end(), ram.begin());
        title::SampleVoice owned;
        title::SampleVoice::Parameters p;
        p.pitch = static_cast<uint16_t>(random() % 0x4001);
        p.left = static_cast<uint16_t>(random() % 0x4000);
        p.right = static_cast<uint16_t>(random() % 0x4000);
        p.attack = static_cast<uint8_t>(random() % 24);
        p.decay = static_cast<uint8_t>(random() % 16);
        p.sustain = static_cast<uint8_t>(random() % 16);
        p.release = static_cast<uint8_t>(random() % 32);
        CHECK(owned.Start(std::make_shared<const std::vector<uint8_t>>(bytes), p));
        ps2r::sound::Voice model;
        model.SetVolL(p.left); model.SetVolR(p.right); model.SetPitch(p.pitch);
        model.SetAdsr1(static_cast<uint16_t>((p.attack << 8) | (p.decay << 4) | p.sustain));
        model.SetAdsr2(p.release);
        model.SetSsa(0); model.SetLsax(0); model.KeyOn(ram.data());
        for (uint32_t frame = 0; frame < 5000; ++frame) {
            if (frame == 800) {
                const uint16_t pitch = static_cast<uint16_t>(random() % 0x4001);
                owned.SetPitch(pitch); model.SetPitch(pitch);
            }
            if (frame == 1300) {
                owned.SetVolumes(p.right, p.left); model.SetVolL(p.right); model.SetVolR(p.left);
            }
            if (frame == 2500) { owned.Release(p.release); model.KeyOff(); }
            float al, ar, bl, br;
            owned.Step(al, ar); model.Step(ram.data(), bl, br);
            // The retained voice keeps stepping a silent terminal loop and
            // can return -0. The service stops the finished asset (+0).
            // The actual mixer starts its sum at +0, erasing that sign.
            if (al == bl && ar == br &&
                (std::signbit(al) != std::signbit(bl) || std::signbit(ar) != std::signbit(br))) ++silent_signs;
            al = 0.0f + al; ar = 0.0f + ar;
            bl = 0.0f + bl; br = 0.0f + br;
            if (std::memcmp(&al, &bl, sizeof(float)) || std::memcmp(&ar, &br, sizeof(float))) {
                if (mismatches < 8)
                    std::printf("ORACLE case=%u frame=%u output=%a,%a model=%a,%a\n", test, frame, al, ar, bl, br);
                ++mismatches;
            }
            ++compared;
        }
        title::SampleVoice restored;
        CHECK(restored.Restore(owned.Capture()));
    }
    CHECK(mismatches == 0);
    std::printf("SampleVoice self-oracle: %llu stereo frames, %llu mismatches, %llu terminal zero signs attributed\n",
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(mismatches),
                static_cast<unsigned long long>(silent_signs));

    title::Audio audio;
    audio.ReserveSounds(128);
    const auto sample = Sample(17, true, 100);
    CHECK(audio.StoreSound(0x8000, sample)); // OLD SOUND_P6 title jingle
    CHECK(!audio.StoreSound(UINT32_MAX, sample)); // table's empty sentinel
    CHECK(!audio.StoreSound(2, std::vector<uint8_t>(17)));
    CHECK(audio.StoreSound(2, sample));
    CHECK(audio.PlaySound(2, 0, 10000, 8000, 0x1000, 0, 8, 0) == -3);
    CHECK(audio.SoundBankLoaded(2));
    CHECK(!audio.SoundBankLoaded(99));
    CHECK(audio.PlaySound(2, 48, 10000, 8000, 0x1000, 0, 8, 0) == -1);
    CHECK(audio.PlaySound(2, 0, 10000, 8000, 0x1000, 0, 8, 0x10000) == -2);
    CHECK(audio.PlaySound(2, 0x10000, 10000, 8000, 0x1000, 0, 8, 0) == 0);
    CHECK(audio.IsVoiceFree(0) == 0 && audio.IsVoiceFree(48) == -1);
    CHECK(!Silent(audio.MixTick()));

    // OLD PLAYTEST_R1 / BUG-003: a long pause preserves the exact cursor,
    // pitch, gain and group, then resumes the same samples without a batch.
    const auto before_pause = Save(audio);
    title::Audio control;
    CHECK(control.Restore(before_pause));
    audio.PauseAll();
    for (int tick = 0; tick < 3600; ++tick) CHECK(Silent(audio.MixTick()));
    audio.ResumeAll();
    CHECK(audio.MixTick() == control.MixTick());
    CHECK(audio.Inspect().voices[0].group == 1);

    // Freeing/replacing a bank cannot invalidate an active voice's lease.
    CHECK(audio.ReleaseSound(2));
    CHECK(!audio.HasSound(2));
    CHECK(audio.PlaySound(2, 1, 10000, 8000, 0x1000, 0, 0, 0) == -3);
    CHECK(audio.MixTick() == control.MixTick());
    CHECK(audio.StoreSound(2, Sample(3, true, 202)) && audio.SoundBankLoaded(2));
    CHECK(audio.MixTick() == control.MixTick());

    // Phase-aware group scaling: an inverted side has the opposite sign,
    // including after group changes. No absolute-value gain conversion.
    title::Audio phases;
    phases.ReserveSounds(8);
    CHECK(phases.StoreSound(2, sample) && phases.SoundBankLoaded(2));
    CHECK(phases.PlaySound(2, 0x10000, 10000, 0x7fff - 10000, 0x1000, 0, 0, 0) == 0);
    CHECK(phases.SetGroupVolume(0x10000, 0x800, 0x800) == 0);
    const auto phase_pcm = phases.MixTick();
    for (size_t i = 0; i < phase_pcm.size(); i += 2) CHECK(phase_pcm[i] == -phase_pcm[i + 1]);
    CHECK(phases.SetGroupVolume(0, 0x1000, 0x1000) == -1);
    CHECK(phases.SetGroupVolume(0x10000, 0x1001, 0x1000) == -2);

    // Movie lending changes transfer ownership, not voice pitch/pause. Source
    // MoviePlayer::LendSound clears reverb and lends the callback; only an
    // explicit PauseAll may freeze voices. PCM itself remains a separate input.
    CHECK(control.Restore(Save(audio)));
    audio.LendToMovie();
    for (int tick = 0; tick < 120; ++tick) CHECK(audio.MixTick() == control.MixTick());
    CHECK(audio.Inspect().movie_owned && !audio.Inspect().paused);
    audio.ReclaimFromMovie();
    CHECK(audio.MixTick() == control.MixTick());
    CHECK(!audio.Inspect().movie_owned);
    CHECK(audio.ReleaseVoice(0, 0) == 0);
    audio.MixTick();
    CHECK(audio.IsVoiceFree(0) == 1);
    CHECK(audio.ReleaseVoice(0, 32) == -1);
    CHECK(audio.ReleaseVoice(48, 0) == -2);
    CHECK(title::Audio::PitchOfRate(48000) == 0x1000);
    CHECK(title::Audio::PitchOfRate(32000) == 0xaaa);

    // State restore validates everything before changing the current state.
    auto corrupt = Save(phases);
    corrupt.voices[0].group = 5;
    CHECK(!audio.Restore(corrupt) && audio.IsVoiceFree(0) == 1);
    title::SampleVoice voice;
    title::SampleVoice::Parameters parameters;
    CHECK(voice.Start(std::make_shared<const std::vector<uint8_t>>(sample), parameters));
    auto broken = voice.Capture();
    broken.sample_index = 28;
    CHECK(!voice.Restore(broken));
    broken = voice.Capture();
    broken.loop_offset = 1;
    CHECK(!voice.Restore(broken));
    audio.Reset();
    CHECK(!audio.HasSound(2) && audio.IsVoiceFree(0) == 1);

    // OLD SOUND_P6: every bank in a batch owns its request; a later bank
    // cannot overwrite a scalar transfer latch. SOUND_P8: generation changes
    // reject stale completion/acknowledgement after free, replace or reset.
    title::Audio loads;
    loads.ReserveSounds(128);
    std::vector<title::Audio::LoadTicket> tickets;
    for (uint32_t id = 0; id < 120; ++id) {
        tickets.push_back(loads.BeginSoundLoad(id));
        CHECK(tickets.back());
    }
    auto untouched = Save(phases);
    const auto preserved_ticks = untouched.ticks;
    CHECK(!loads.Capture(untouched) && untouched.ticks == preserved_ticks);
    for (uint32_t id = 0; id < tickets.size(); ++id) {
        CHECK(!loads.SoundBankLoaded(tickets[id]));
        CHECK(loads.CompleteSoundLoad(tickets[id], Sample(2, true, id + 123)));
        CHECK(loads.SoundBankLoaded(tickets[id]));
        CHECK(!loads.CompleteSoundLoad(tickets[id], sample));
    }
    CHECK(loads.Inspect().samples.size() == 120);
    const auto before_restore = loads.BeginSoundLoad(2);
    CHECK(loads.ReleaseSound(2));
    CHECK(!loads.CompleteSoundLoad(before_restore, sample));
    const auto replacing = loads.BeginSoundLoad(2);
    const auto replacement = loads.BeginSoundLoad(2);
    CHECK(!loads.CompleteSoundLoad(replacing, sample));
    CHECK(loads.CompleteSoundLoad(replacement, sample));
    CHECK(!loads.SoundBankLoaded(replacing) && loads.SoundBankLoaded(replacement));
    const auto checkpoint_loads = Save(loads);
    const auto future = loads.BeginSoundLoad(3);
    CHECK(loads.Restore(checkpoint_loads));
    CHECK(!loads.CompleteSoundLoad(future, sample) && !loads.SoundBankLoaded(replacement));
    const auto reset_job = loads.BeginSoundLoad(4);
    loads.Reset();
    loads.ReserveSounds(128);
    const auto reset_reuse = loads.BeginSoundLoad(4);
    CHECK(!loads.CompleteSoundLoad(reset_job, sample));
    CHECK(loads.CompleteSoundLoad(reset_reuse, sample) && loads.SoundBankLoaded(reset_reuse));
    const auto shrunk = loads.BeginSoundLoad(90);
    loads.ReserveSounds(8);
    CHECK(loads.CompleteSoundLoad(shrunk, sample));
    CHECK(loads.Inspect().sample_limit == 128);
    const auto cancelled = loads.BeginSoundLoad(4);
    CHECK(loads.CancelSoundLoad(cancelled) && !loads.CancelSoundLoad(cancelled));

    // Capacity counts live and pending registrations, independently of IDs.
    // Replacing the same key can use its slot in a full table.
    title::Audio capacity;
    capacity.ReserveSounds(2);
    const auto high = capacity.BeginSoundLoad(0x8000);
    CHECK(high && capacity.StoreSound(0x12345678, sample));
    CHECK(!capacity.BeginSoundLoad(1));
    CHECK(capacity.CompleteSoundLoad(high, sample) && capacity.SoundBankLoaded(high));
    CHECK(capacity.PlaySound(0x8000, 23, 10000, 9000, 0x1000, 0, 8, 0) == 0);
    CHECK(capacity.StoreSound(0x12345678, sample));
    auto valid_high = Save(capacity);
    CHECK(capacity.Restore(valid_high));
    valid_high.sample_limit = 1;
    CHECK(!capacity.Restore(valid_high));
    capacity.ReserveSounds(1);
    CHECK(capacity.IsVoiceFree(23) == 0 && capacity.HasSound(0x8000));
    CHECK(capacity.Inspect().sample_limit == 2);
    CHECK(capacity.ReleaseSound(0x8000));
    CHECK(capacity.StoreSound(0xfffffffe, sample));
    CHECK(!capacity.StoreSound(0x8000, sample));

    // The per-voice mixer provider preserves the sum's ordering and advances
    // once per frame. Silence remains owned; the old voice must not restart.
    title::Audio provider, block_mix;
    CHECK(provider.Restore(Save(phases)) && block_mix.Restore(Save(phases)));
    const auto expected = block_mix.MixTick();
    provider.BeginMixTick();
    std::vector<float> streamed(expected.size());
    for (uint32_t frame = 0; frame < title::Audio::kFramesPerTick; ++frame) {
        float l = 0, r = 0;
        for (uint32_t index = 0; index < title::Audio::kVoices; ++index) {
            float vl = 0, vr = 0;
            provider.StepVoice(index, vl, vr);
            l += vl; r += vr;
        }
        streamed[frame * 2] = std::clamp(l, -1.0f, 1.0f);
        streamed[frame * 2 + 1] = std::clamp(r, -1.0f, 1.0f);
    }
    CHECK(streamed == expected && provider.Inspect().ticks == block_mix.Inspect().ticks);
    CHECK(provider.SetVoiceRelease(0, 0) == 0 && provider.IsVoiceFree(0) == 0);
    CHECK(provider.KeyOff(0) == 0);
    provider.MixTick();
    float l = 123, r = 456;
    CHECK(provider.StepVoice(0, l, r) && l == 0 && r == 0);
    CHECK(provider.OwnsVoice(0));
    provider.HandOffVoice(0);
    CHECK(!provider.StepVoice(0, l, r) && !provider.OwnsVoice(0));
    CHECK(!loads.CompleteSoundLoad(cancelled, sample));
    // The pause command mutes and parks the sample cursor, while a release
    // still progresses on the sample clock. Resume restores requested gain.
    title::Audio paused_release;
    paused_release.ReserveSounds(1);
    CHECK(paused_release.StoreSound(1, sample) && paused_release.SoundBankLoaded(1));
    CHECK(paused_release.PlaySound(1, 0, 12000, 9000, 0x1000, 0, 0, 0) == 0);
    paused_release.MixTick();
    CHECK(paused_release.SetVoiceVolume(0, 2000, 5000) == 0);
    paused_release.AdvanceControls();
    paused_release.MixTick();
    paused_release.PauseAll();
    const auto cursor = paused_release.Inspect().voices[0].player.Capture().block_offset;
    CHECK(Silent(paused_release.MixTick()));
    CHECK(paused_release.Inspect().voices[0].player.Capture().block_offset == cursor);
    paused_release.ResumeAll();
    CHECK(paused_release.Inspect().voices[0].player.Capture().left == 2000.0f / 32767.0f);
    paused_release.PauseAll();
    CHECK(paused_release.KeyOff(0) == 0);
    CHECK(Silent(paused_release.MixTick()) && paused_release.IsVoiceFree(0) == 1);
    {
        // The voices' block loop (the default) against the frame loop of the earlier builds (SetLegacyMix(true)): the same
        // voice copied, every parameter random (attack 0..127, decay, sustain 0..15 -- the Decay phase --, release, pitch
        // 0..0x3fff, inverted-phase volumes), volume ramps targeted between runs, releases, held runs, runs of random length:
        // the outputs and every state field bit for bit.
        std::mt19937 random(20261027);
        std::vector<std::shared_ptr<const std::vector<uint8_t>>> assets;
        for (uint32_t k = 0; k < 8; ++k)
            assets.push_back(std::make_shared<const std::vector<uint8_t>>(Sample(2 + random() % 30, k % 2 == 0, 100 + k)));
        uint64_t compared = 0;
        bool same = true;
        for (int v = 0; v < 400 && same; ++v) {
            title::SampleVoice::Parameters parameters;
            parameters.pitch = static_cast<uint16_t>(random() % 3 ? 0x1000 : random() % 0x4000);
            parameters.left = static_cast<uint16_t>(random() % 0x8000);
            parameters.right = static_cast<uint16_t>(random() % 0x8000);
            parameters.attack = static_cast<uint8_t>(random() % 2 ? 0 : random() % 128);
            parameters.decay = static_cast<uint8_t>(random() % 16);
            parameters.sustain = static_cast<uint8_t>(random() % 2 ? 15 : random() % 16);
            parameters.release = static_cast<uint8_t>(random() % 32);
            title::SampleVoice frame_loop, block_loop;
            CHECK(frame_loop.Start(assets[random() % assets.size()], parameters));
            block_loop = frame_loop;
            const uint32_t repeats = random() % 4 ? 0 : 1 + random() % 3;
            for (int run = 0; run < 40 && same; ++run) {
                const uint32_t op = random() % 8, a = random() % 0x8000, b = random() % 0x8000;
                for (title::SampleVoice* voice : {&frame_loop, &block_loop}) {
                    if (op == 0) voice->RampVolumes(static_cast<uint16_t>(a), static_cast<uint16_t>(b));
                    else if (op == 1) voice->Release(static_cast<uint8_t>(a % 32));
                    else if (op == 2) voice->SetPitch(static_cast<uint16_t>(a % 0x4000));
                    voice->AdvanceControls();
                }
                const uint32_t frames = 1 + random() % 200;
                const bool held = random() % 6 == 0;
                std::vector<float> fl(frames, 7.0f), fr(frames, 7.0f), bl(frames, 9.0f), br(frames, 9.0f);
                title::SampleVoice::SetLegacyMix(true);
                if (held) frame_loop.StepPausedRun(frames);
                else frame_loop.StepRun(frames, repeats, fl.data(), fr.data());
                title::SampleVoice::SetLegacyMix(false);
                if (held) block_loop.StepPausedRun(frames);
                else block_loop.StepRun(frames, repeats, bl.data(), br.data());
                const auto& x = frame_loop.Capture();
                const auto& y = block_loop.Capture();
                const auto bits = [](const auto& value_a, const auto& value_b) { return std::memcmp(&value_a, &value_b, sizeof value_a) == 0; };
                same = (held || (std::memcmp(fl.data(), bl.data(), frames * 4) == 0 && std::memcmp(fr.data(), br.data(), frames * 4) == 0)) &&
                       x.phase == y.phase && x.block_offset == y.block_offset && x.loop_offset == y.loop_offset &&
                       x.fraction == y.fraction && x.sample_index == y.sample_index && x.history.h1 == y.history.h1 &&
                       x.history.h2 == y.history.h2 && x.block == y.block && x.flags == y.flags && x.previous == y.previous &&
                       bits(x.envelope, y.envelope) && bits(x.left, y.left) && bits(x.right, y.right) && x.ends == y.ends &&
                       (x.samples == nullptr) == (y.samples == nullptr) && bits(x.left_ramp.Capture().step, y.left_ramp.Capture().step);
                compared += held ? 0 : frames;
            }
        }
        CHECK(same);
        CHECK(compared > 100000);
        title::SampleVoice::SetLegacyMix(false);
    }
    std::printf("Audio: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
