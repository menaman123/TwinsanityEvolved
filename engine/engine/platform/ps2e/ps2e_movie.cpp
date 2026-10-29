// engine/platform/ps2e/ps2e_movie.cpp -- ps2e_movie over PssDemux + FfmpegVideoDecoder + FmvAudioCapture.
//
// The movie object is the demuxer's listener and the capture's PCM sink: video payloads go to the decoder (held back
// in a pending buffer while max_pictures decoded pictures already wait, so a producer that feeds generously does not
// make the decoder run ahead unbounded), audio payloads go to the capture (which selects the channel, validates the
// ADS header and de-interleaves the planar pairs), and the capture's frames queue here until the producer reads them.
#include "engine/platform/ps2e/ps2e_movie.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "engine/ipu/hle/decoded_picture.h"
#include "engine/ipu/hle/ffmpeg_video_decoder.h"
#include "engine/ipu/hle/fmv_audio_capture.h"
#include "engine/ipu/hle/fmv_pcm_sink.h"
#include "engine/ipu/hle/pss_demux.h"
#include "engine/ipu/hle/pss_demux_listener.h"

using namespace ps2r;

struct ps2e_movie final : ipu::PssDemuxListener, ipu::FmvPcmSink {
    explicit ps2e_movie(const ps2e_movie_config& config);
    ~ps2e_movie() override = default;

    // PssDemuxListener
    void OnVideoEs(const uint8_t* data, size_t size, bool contains_sequence_end) override;
    void OnAudioPes(const uint8_t* data, size_t size) override;
    void OnProgramEnd() override;
    // FmvPcmSink (the capture's sink: the gate is the producer's business here, so GateOpen/Flush are no-ops)
    void Push(const int16_t* stereo, uint32_t frames) override;
    void GateOpen() override {}
    void Flush() override {}

    void Feed(const uint8_t* bytes, size_t size);
    void EndInput();
    void DrainPending();
    bool TakePicture(ps2e_movie_picture& out);
    uint32_t ReadPcm(int16_t* stereo, uint32_t frames);

    int max_pictures = 4;
    ipu::PssDemux demux;
    ipu::FfmpegVideoDecoder decoder;
    ipu::FmvAudioCapture capture;
    std::deque<ipu::DecodedPicture> pictures;
    ipu::DecodedPicture taken;            // the picture last handed out (its bytes stay valid until the next take)
    std::vector<uint8_t> pending_es;      // video ES held back while enough pictures wait
    size_t pending_off = 0;
    bool pending_sequence_end = false;
    bool input_ended = false;             // the demuxer saw the end, or end_input was called
    bool decoder_flushed = false;
    std::deque<int16_t> pcm;              // interleaved stereo samples waiting
    ps2e_movie_counters counts{};
};

ps2e_movie::ps2e_movie(const ps2e_movie_config& config) : demux(*this), capture(*this) {
    if (config.max_pictures > 0) max_pictures = config.max_pictures;
    decoder.SetPlanarOutput(config.planar != 0);
    capture.SelectChannel(config.audio_channel < 0 ? ipu::FmvAudioCapture::kAnyChannel : config.audio_channel);
}

void ps2e_movie::OnVideoEs(const uint8_t* data, size_t size, bool contains_sequence_end) {
    if (!data || size == 0) {
        if (contains_sequence_end) input_ended = true;
        return;
    }
    if (!pending_es.empty() || static_cast<int>(pictures.size()) >= max_pictures) {
        pending_es.insert(pending_es.end(), data, data + size);
        if (contains_sequence_end) pending_sequence_end = true;
        return;
    }
    counts.video_es_bytes += size;
    decoder.Feed(data, size, pictures);
    counts.pictures_decoded = decoder.FramesDecoded();
    if (contains_sequence_end) input_ended = true;
}

void ps2e_movie::OnAudioPes(const uint8_t* data, size_t size) {
    ++counts.audio_payloads;
    const uint64_t skipped = capture.SkippedOtherChannel();
    capture.OnAudioPes(data, size);
    counts.audio_skipped += capture.SkippedOtherChannel() - skipped;
}

void ps2e_movie::OnProgramEnd() { input_ended = true; }

void ps2e_movie::Push(const int16_t* stereo, uint32_t frames) {
    if (!stereo || frames == 0) return;
    pcm.insert(pcm.end(), stereo, stereo + size_t(frames) * 2);
    counts.pcm_frames_pushed += frames;
}

// Video held back goes to the decoder in slices until enough pictures wait again or nothing is left
void ps2e_movie::DrainPending() {
    constexpr size_t kSlice = 64 * 1024;
    while (pending_off < pending_es.size() && static_cast<int>(pictures.size()) < max_pictures) {
        const size_t n = std::min(kSlice, pending_es.size() - pending_off);
        counts.video_es_bytes += n;
        decoder.Feed(pending_es.data() + pending_off, n, pictures);
        counts.pictures_decoded = decoder.FramesDecoded();
        pending_off += n;
    }
    if (pending_off >= pending_es.size()) {
        pending_es.clear();
        pending_off = 0;
        if (pending_sequence_end) {
            pending_sequence_end = false;
            input_ended = true;
        }
    } else if (pending_off > 4 * 1024 * 1024) {
        pending_es.erase(pending_es.begin(), pending_es.begin() + static_cast<std::ptrdiff_t>(pending_off));
        pending_off = 0;
    }
}

void ps2e_movie::Feed(const uint8_t* bytes, size_t size) {
    if (bytes && size) {
        counts.bytes_fed += size;
        demux.Append(bytes, size);
        demux.Process(false);
        counts.packs = demux.PacksSeen();
        if (demux.Ended()) input_ended = true;
    }
    DrainPending();
    // The input ended and no video is held back: the decoder's last pictures come out
    if (input_ended && pending_es.empty() && !decoder_flushed) {
        decoder_flushed = true;
        decoder.Flush(pictures);
        counts.pictures_decoded = decoder.FramesDecoded();
    }
}

void ps2e_movie::EndInput() {
    demux.Process(true);
    counts.packs = demux.PacksSeen();
    input_ended = true;
    Feed(nullptr, 0);
}

bool ps2e_movie::TakePicture(ps2e_movie_picture& out) {
    if (pictures.empty()) return false;
    taken = std::move(pictures.front());
    pictures.pop_front();
    ++counts.pictures_taken;
    out.width = taken.width;
    out.height = taken.height;
    out.planar = taken.planar;
    out.rgba8 = taken.planar ? nullptr : taken.rgba.data();
    out.planes = taken.planar ? taken.planes.data() : nullptr;
    // Room again: the video held back decodes on, and a drained end flushes
    Feed(nullptr, 0);
    return true;
}

uint32_t ps2e_movie::ReadPcm(int16_t* stereo, uint32_t frames) {
    if (!stereo || frames == 0) return 0;
    const uint32_t have = static_cast<uint32_t>(pcm.size() / 2);
    const uint32_t n = std::min(frames, have);
    for (uint32_t i = 0; i < n * 2; ++i) stereo[i] = pcm[i];
    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(n) * 2);
    counts.pcm_frames_read += n;
    return n;
}

extern "C" {

ps2e_movie* ps2e_movie_create(const ps2e_movie_config* config) {
    ps2e_movie_config c{};
    c.audio_channel = -1;
    if (config) c = *config;
    return new ps2e_movie(c);
}

void ps2e_movie_destroy(ps2e_movie* movie) { delete movie; }

int ps2e_movie_feed(ps2e_movie* movie, const void* bytes, size_t size) {
    if (!movie) return 0;
    movie->Feed(static_cast<const uint8_t*>(bytes), size);
    return static_cast<int>(movie->pictures.size());
}

void ps2e_movie_end_input(ps2e_movie* movie) {
    if (movie) movie->EndInput();
}

int ps2e_movie_pictures_ready(const ps2e_movie* movie) { return movie ? static_cast<int>(movie->pictures.size()) : 0; }

int ps2e_movie_take_picture(ps2e_movie* movie, ps2e_movie_picture* picture) {
    if (!movie || !picture) return 0;
    return movie->TakePicture(*picture) ? 1 : 0;
}

int ps2e_movie_input_ended(const ps2e_movie* movie) { return movie && movie->input_ended ? 1 : 0; }

int ps2e_movie_ended(const ps2e_movie* movie) {
    return movie && movie->input_ended && movie->pending_es.empty() && movie->decoder_flushed && movie->pictures.empty() ? 1 : 0;
}

double ps2e_movie_fps(const ps2e_movie* movie) { return movie ? movie->decoder.AuthoredFps() : 0.0; }

int ps2e_movie_has_audio(const ps2e_movie* movie) { return movie && movie->capture.HeaderParsed() ? 1 : 0; }
int ps2e_movie_audio_disabled(const ps2e_movie* movie) { return movie && movie->capture.Disabled() ? 1 : 0; }
uint32_t ps2e_movie_pcm_queued(const ps2e_movie* movie) { return movie ? static_cast<uint32_t>(movie->pcm.size() / 2) : 0; }
uint32_t ps2e_movie_pcm_read(ps2e_movie* movie, int16_t* stereo, uint32_t frames) {
    return movie ? movie->ReadPcm(stereo, frames) : 0;
}

void ps2e_movie_counts(const ps2e_movie* movie, ps2e_movie_counters* out) {
    if (!out) return;
    *out = movie ? movie->counts : ps2e_movie_counters{};
}

const char* ps2e_movie_decoder_version(void) {
    static const std::string version = ipu::FfmpegVideoDecoder::LibraryVersion();
    return version.c_str();
}

}  // extern "C"
