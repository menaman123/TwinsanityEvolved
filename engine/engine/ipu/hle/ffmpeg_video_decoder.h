// engine/ipu/hle/ffmpeg_video_decoder.h -- FfmpegVideoDecoder: the VideoDecoder
// over libavcodec's MPEG-2 video decoder. An elementary stream is fed through an
// AVCodecParserContext (packet boundaries), each packet through the decoder on the
// calling thread (thread_count = 1: decode order equals serve order, no worker
// pool), and every produced frame is converted to RGBA8 with libswscale at the
// picture's own size. Only this .cpp includes FFmpeg headers; the class is
// move-only and owned by one MpegContext.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
#include <string>

#include "engine/ipu/hle/decoded_picture.h"
#include "engine/ipu/hle/video_decoder.h"

struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace ps2r::ipu {

class FfmpegVideoDecoder : public VideoDecoder {
public:
    FfmpegVideoDecoder() = default;
    ~FfmpegVideoDecoder() override;

    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    // VideoDecoder.
    void Feed(const uint8_t* data, size_t size, std::deque<DecodedPicture>& out) override;
    void Flush(std::deque<DecodedPicture>& out) override;
    double AuthoredFps() const override { return authored_fps_; }
    void Shutdown() override;

    // Pictures produced since construction (the count survives Shutdown).
    uint64_t FramesDecoded() const { return frames_decoded_; }

    // Planar output (the Vita's movie textures, ps2e_movie_config.planar): a 4:2:0 picture comes out as its Y, U, V
    // planes (DecodedPicture::planar / planes) instead of swscale's RGBA -- the GPU converts it. A picture of another
    // chroma layout still comes out as RGBA. Off by default.
    void SetPlanarOutput(bool on) { planar_ = on; }

    // The linked libavcodec as "MAJOR.MINOR.MICRO" (the decoded RGBA bytes
    // depend on it: the per-picture golden records this string).
    static std::string LibraryVersion();
    static unsigned LibavcodecVersion();

private:
    bool EnsureInitialized();
    bool SendPacket(const uint8_t* data, int size, std::deque<DecodedPicture>& out);
    bool ReceiveFrames(std::deque<DecodedPicture>& out);
    bool ConvertFrame(std::deque<DecodedPicture>& out);

    AVCodecParserContext* parser_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    uint64_t frames_decoded_ = 0;
    double authored_fps_ = 0.0;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    int sws_w_ = 0;
    // 32-bit ARM: a 64-byte-aligned staging destination for sws_scale when the picture's own buffer is not 16-byte aligned
    // (FFmpeg's NEON yuv420p -> rgba stores with an alignment qualifier that faults on the device otherwise)
    std::vector<uint8_t> aligned_staging_;
    int sws_h_ = 0;
    int sws_fmt_ = -1;  // AVPixelFormat of the current SwsContext (-1 = AV_PIX_FMT_NONE)
    bool initialized_ = false;
    bool drained_ = false;
    bool planar_ = false;   // SetPlanarOutput
};

}  // namespace ps2r::ipu
