// engine/ipu/hle/ffmpeg_video_decoder.cpp -- FfmpegVideoDecoder bodies (see the
// header). The libav call sequence is the contract: parser -> send_packet (EAGAIN:
// drain then retry once) -> receive_frame loop -> sws_scale to RGBA at w x h.
#include "engine/ipu/hle/ffmpeg_video_decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "engine/core/log/log.h"

namespace ps2r::ipu {

namespace {

void LogAvError(const char* what, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    PS2R_LOG(IPU, Warn, "video decoder: %s failed: %s", what, buf);
}

#if defined(__vita__)
// Whether this CPU executes the two NEON instructions the NEON simple IDCT needs and Vita3K's ARM JIT (dynarmic) lacks:
// VADDHN / VSUBHN (simple_idct_neon.S idct_col4_neon). Probed once on known operands: the Cortex-A9 computes them;
// Vita3K logs "Unimplemented instruction", leaves the destination as it was (zero here) and goes on -- the IDCT's blocks
// then decode to garbage. A Vita3K that implements them passes the probe and decodes correctly with the NEON IDCT too.
bool NeonNarrowingAddWorks() {
    alignas(16) static const uint32_t a[4] = {0x00018000u, 0x7fff0001u, 0x12345678u, 0xffff0000u};
    alignas(16) static const uint32_t b[4] = {0x00027fffu, 0x00010001u, 0x11111111u, 0x00010000u};
    // volatile: the check below stays scalar (LDRH + CMP), so no other NEON instruction takes part in the verdict
    alignas(16) volatile uint16_t sum[4] = {0, 0, 0, 0}, difference[4] = {0, 0, 0, 0};
    __asm__ volatile(
        "vld1.32 {d16-d17}, [%[a]]\n\t"
        "vld1.32 {d18-d19}, [%[b]]\n\t"
        "vmov.i32 d20, #0\n\t"
        "vmov.i32 d21, #0\n\t"
        "vaddhn.i32 d20, q8, q9\n\t"
        "vsubhn.i32 d21, q8, q9\n\t"
        "vst1.16 {d20}, [%[s]]\n\t"
        "vst1.16 {d21}, [%[d]]\n\t"
        :
        : [a] "r"(a), [b] "r"(b), [s] "r"(sum), [d] "r"(difference)
        : "d16", "d17", "d18", "d19", "d20", "d21", "memory");
    for (int i = 0; i < 4; ++i) {
        const uint16_t want_sum = uint16_t((a[i] + b[i]) >> 16), want_difference = uint16_t((a[i] - b[i]) >> 16);
        if (sum[i] != want_sum || difference[i] != want_difference) return false;
    }
    return true;
}

// The IDCT for the Vita: the NEON simple IDCT where the CPU runs it (the device), the ARMv6 one where it does not
// (Vita3K). TE_MOVIE_IDCT=neon|armv6 forces one (an A/B knob). Both are FFmpeg's fixed-point "simple" IDCT (IEEE 1180
// accurate); they may round a pixel differently, so a forced NEON IDCT under Vita3K decodes to garbage.
int VitaIdctAlgorithm() {
    static const int algorithm = [] {
        const char* forced = std::getenv("TE_MOVIE_IDCT");
        const bool probe = NeonNarrowingAddWorks();
        bool neon = probe;
        if (forced && std::strcmp(forced, "neon") == 0) neon = true;
        if (forced && std::strcmp(forced, "armv6") == 0) neon = false;
        std::fprintf(stderr, "[twinsanityevolved] movie: %s IDCT (VADDHN/VSUBHN probe: %s%s)\n", neon ? "the NEON" : "the ARMv6",
                     probe ? "computed -- the device" : "not executed -- an emulator",
                     forced ? ", TE_MOVIE_IDCT set" : "");
        return neon ? FF_IDCT_AUTO : FF_IDCT_SIMPLEARMV6;   // AUTO = the simple NEON IDCT on armv7 with NEON
    }();
    return algorithm;
}
#endif

}  // namespace

FfmpegVideoDecoder::~FfmpegVideoDecoder() { Shutdown(); }

std::string FfmpegVideoDecoder::LibraryVersion() {
    const unsigned v = avcodec_version();
    return std::to_string(AV_VERSION_MAJOR(v)) + "." + std::to_string(AV_VERSION_MINOR(v)) + "." +
           std::to_string(AV_VERSION_MICRO(v));
}

unsigned FfmpegVideoDecoder::LibavcodecVersion() { return avcodec_version(); }

void FfmpegVideoDecoder::Feed(const uint8_t* data, size_t size, std::deque<DecodedPicture>& out) {
    if (!data || size == 0) return;
    if (!EnsureInitialized()) return;

    // av_parser_parse2 requires zero padding beyond the logical input.
    // PES payloads borrow the demux buffer and cannot guarantee that tail.
    if (size > std::numeric_limits<size_t>::max() - AV_INPUT_BUFFER_PADDING_SIZE) return;
    std::vector<uint8_t> input(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(input.data(), data, size);
    const uint8_t* cursor = input.data();
    size_t remaining = size;
    while (remaining > 0) {
        uint8_t* pkt_data = nullptr;
        int pkt_size = 0;
        const int chunk = static_cast<int>(
            std::min<size_t>(remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
        const int used = av_parser_parse2(parser_, codec_ctx_, &pkt_data, &pkt_size, cursor, chunk,
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) {
            LogAvError("av_parser_parse2", used);
            return;
        }
        if (used == 0 && pkt_size == 0) break;

        cursor += used;
        remaining -= static_cast<size_t>(used);

        if (pkt_size > 0 && !SendPacket(pkt_data, pkt_size, out)) return;
    }
}

void FfmpegVideoDecoder::Flush(std::deque<DecodedPicture>& out) {
    if (!initialized_ || drained_) return;

    // A NULL parser packet flushes the packet the parser still holds.
    if (parser_) {
        uint8_t* pkt_data = nullptr;
        int pkt_size = 0;
        (void)av_parser_parse2(parser_, codec_ctx_, &pkt_data, &pkt_size, nullptr, 0, AV_NOPTS_VALUE,
                               AV_NOPTS_VALUE, 0);
        if (pkt_size > 0 && !SendPacket(pkt_data, pkt_size, out)) return;
    }

    const int ret = avcodec_send_packet(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        LogAvError("avcodec_send_packet(nullptr)", ret);
        return;
    }

    (void)ReceiveFrames(out);
    drained_ = true;
}

void FfmpegVideoDecoder::Shutdown() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (frame_) av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (parser_) {
        av_parser_close(parser_);
        parser_ = nullptr;
    }
    sws_w_ = 0;
    sws_h_ = 0;
    sws_fmt_ = AV_PIX_FMT_NONE;
    authored_fps_ = 0.0;  // the next playback latches its own header rate
    initialized_ = false;
    drained_ = false;
}

bool FfmpegVideoDecoder::EnsureInitialized() {
    if (initialized_) return true;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!codec) {
        PS2R_LOG(IPU, Warn, "video decoder: MPEG-2 decoder not found");
        return false;
    }

    parser_ = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    if (!parser_) {
        PS2R_LOG(IPU, Warn, "video decoder: MPEG-video parser not available");
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!codec_ctx_ || !frame_ || !packet_) {
        PS2R_LOG(IPU, Warn, "video decoder: failed to allocate decoder state");
        Shutdown();
        return false;
    }

    codec_ctx_->thread_count = 1;  // decode on the calling thread
#if defined(__vita__)
    // The IDCT: the NEON simple IDCT on the device, the ARMv6 one under Vita3K (VitaIdctAlgorithm: its ARM JIT lacks
    // VADDHN / VSUBHN, which the NEON IDCT (simple_idct_neon.S, idct_col4_neon) uses -- every block logged "Unimplemented
    // instruction" and decoded to garbage there). No other NEON code in the eboot uses an instruction it lacks (the
    // motion compensation and the RGBA conversion stay NEON).
    codec_ctx_->idct_algo = VitaIdctAlgorithm();
    // Slice threads: the calling thread and two workers decode a picture's slices in parallel (MPEG-2's macroblock rows
    // are independent). On the calling thread alone the movies missed their 25 fps in Vita3K and, the movie's sound
    // being mixed per boundary, padded the sound. The picture is complete when the call returns (slice threads never
    // delay one, unlike frame threads) and equals the one-thread decode. TE_MOVIE_DECODE_THREADS=1: the calling thread only.
    {
        int threads = 3;
        if (const char* e = std::getenv("TE_MOVIE_DECODE_THREADS")) threads = std::clamp(std::atoi(e), 1, 4);
        codec_ctx_->thread_count = threads;
        codec_ctx_->thread_type = FF_THREAD_SLICE;
    }
#endif
    const int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        LogAvError("avcodec_open2", ret);
        Shutdown();
        return false;
    }

    initialized_ = true;
    drained_ = false;
    return true;
}

bool FfmpegVideoDecoder::SendPacket(const uint8_t* data, int size, std::deque<DecodedPicture>& out) {
    if (!data || size <= 0) return true;

    av_packet_unref(packet_);
    const int alloc = av_new_packet(packet_, size);
    if (alloc < 0) {
        LogAvError("av_new_packet", alloc);
        return false;
    }
    std::memcpy(packet_->data, data, static_cast<size_t>(size));

    int ret = avcodec_send_packet(codec_ctx_, packet_);
    if (ret == AVERROR(EAGAIN)) {
        // Decoder full: drain its frames and retry once.
        if (!ReceiveFrames(out)) {
            av_packet_unref(packet_);
            return false;
        }
        ret = avcodec_send_packet(codec_ctx_, packet_);
    }
    av_packet_unref(packet_);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        LogAvError("avcodec_send_packet", ret);
        return false;
    }
    return ReceiveFrames(out);
}

bool FfmpegVideoDecoder::ReceiveFrames(std::deque<DecodedPicture>& out) {
    for (;;) {
        const int ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        if (ret < 0) {
            LogAvError("avcodec_receive_frame", ret);
            return false;
        }
        const bool ok = ConvertFrame(out);
        av_frame_unref(frame_);
        if (!ok) return false;
    }
}

bool FfmpegVideoDecoder::ConvertFrame(std::deque<DecodedPicture>& out) {
    const int w = frame_->width;
    const int h = frame_->height;
    const int fmt = frame_->format;
    if (w <= 0 || h <= 0 || fmt == AV_PIX_FMT_NONE) return false;
    // The sequence header's frame rate is known once the first picture has
    // been decoded; latch it once per playback.
    const auto latch_fps = [this] {
        if (authored_fps_ == 0.0 && codec_ctx_) {
            const AVRational fr = codec_ctx_->framerate;
            if (fr.num > 0 && fr.den > 0) {
                authored_fps_ = static_cast<double>(fr.num) / static_cast<double>(fr.den);
            }
        }
    };

    // Planar output (SetPlanarOutput): a limited-range 4:2:0 picture's planes copied out as they are (the GPU converts
    // them); every other layout takes the RGBA path below
    if (planar_ && fmt == AV_PIX_FMT_YUV420P && frame_->color_range != AVCOL_RANGE_JPEG && frame_->linesize[0] >= w &&
        frame_->linesize[1] >= (w + 1) / 2 && frame_->linesize[2] >= (w + 1) / 2) {
        DecodedPicture pic;
        pic.width = w;
        pic.height = h;
        pic.planar = 1;
        const int cw = (w + 1) / 2, ch = (h + 1) / 2;
        pic.planes.resize(static_cast<size_t>(w) * static_cast<size_t>(h) + 2u * static_cast<size_t>(cw) * static_cast<size_t>(ch));
        uint8_t* dst = pic.planes.data();
        const auto copy_plane = [&dst](const uint8_t* src, int linesize, int pw, int ph) {
            if (linesize == pw) {
                std::memcpy(dst, src, static_cast<size_t>(pw) * static_cast<size_t>(ph));
                dst += static_cast<size_t>(pw) * static_cast<size_t>(ph);
                return;
            }
            for (int y = 0; y < ph; ++y) {
                std::memcpy(dst, src + static_cast<size_t>(y) * static_cast<size_t>(linesize), static_cast<size_t>(pw));
                dst += pw;
            }
        };
        copy_plane(frame_->data[0], frame_->linesize[0], w, h);
        copy_plane(frame_->data[1], frame_->linesize[1], cw, ch);
        copy_plane(frame_->data[2], frame_->linesize[2], cw, ch);
        out.push_back(std::move(pic));
        ++frames_decoded_;
        latch_fps();
        return true;
    }

    if (!sws_ctx_ || sws_w_ != w || sws_h_ != h || sws_fmt_ != fmt) {
        if (sws_ctx_) sws_freeContext(sws_ctx_);
        sws_ctx_ = sws_getContext(w, h, static_cast<AVPixelFormat>(fmt), w, h, AV_PIX_FMT_RGBA,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            PS2R_LOG(IPU, Warn, "video decoder: sws_getContext failed (%dx%d fmt %d)", w, h, fmt);
            return false;
        }
        sws_w_ = w;
        sws_h_ = h;
        sws_fmt_ = fmt;
    }

    DecodedPicture pic;
    pic.width = w;
    pic.height = h;
    const size_t rgba_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    pic.rgba.resize(rgba_bytes);
    uint8_t* dst0 = pic.rgba.data();
    bool staged = false;
#if defined(__arm__)
    // FFmpeg's NEON yuv420p -> rgba converter (libswscale/arm/yuv2rgb_neon.S) stores each row with
    // `vst4.8 {..}, [dst,:128]!`: the destination row starts MUST be 16-byte aligned -- on the device a misaligned one is
    // an alignment fault (a data abort in ff_yuv420p_to_rgba_neon: the first movie picture killed the PS Vita run), while
    // an emulator ignores the qualifier. std::vector's storage is malloc's 8-byte alignment, so a misaligned picture is
    // converted into a 64-byte-aligned staging buffer and copied (one memcpy per picture; most pictures land aligned).
    if ((reinterpret_cast<uintptr_t>(dst0) & 15u) != 0 || ((static_cast<unsigned>(w) * 4u) & 15u) != 0) {
        aligned_staging_.resize(rgba_bytes + 64);
        dst0 = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(aligned_staging_.data()) + 63u) & ~uintptr_t(63));
        staged = true;
    }
#endif

    uint8_t* dst[4] = {dst0, nullptr, nullptr, nullptr};
    int dst_ls[4] = {w * 4, 0, 0, 0};
    const int rows = sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, h, dst, dst_ls);
    if (staged && rows >= 0) std::memcpy(pic.rgba.data(), dst0, rgba_bytes);
#if defined(__arm__)
    // 32-bit ARM: FFmpeg's NEON unscaled converters (libswscale/arm/swscale_unscaled.c, the yuv420p -> rgba path every
    // movie takes: width a multiple of 16, height even) return 0, not the slice height (6.1 through 8.0 and master), so
    // only a negative result is a failure there -- the Vita dropped every picture on this check.
    if (rows < 0) {
#else
    if (rows <= 0) {
#endif
        PS2R_LOG(IPU, Warn, "video decoder: sws_scale produced 0 rows");
        return false;
    }

    out.push_back(std::move(pic));
    ++frames_decoded_;
    latch_fps();
    return true;
}

}  // namespace ps2r::ipu
