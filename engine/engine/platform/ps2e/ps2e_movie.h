/* engine/platform/ps2e/ps2e_movie.h -- ps2e_movie: the movie decoder a NON-recompiled producer (a decompiled game's
 * native platform arm over this engine) plays its FMVs through.
 *
 * The engine's owned movie path serves the recompiled title's sceMpeg calls over guest memory (engine/ipu/hle/
 * mpeg_api.h: a SceMpeg context layout, an IPU-layout frame writer, a tick-domain pacer). A decomp has none of
 * that: it has the movie file's bytes, it wants decoded pictures to show and PCM to play, and it keeps its own
 * presentation clock. This header is that surface, in C so a foreign toolchain (wasm2c output, a C runner) can
 * call it without the engine's C++ types:
 *
 *   movie = ps2e_movie_create(&config)              one demuxer + one MPEG-2 video decoder + one PCM capture
 *   ps2e_movie_feed(movie, bytes, n)                program-stream (PSS) bytes in file order, any chunking
 *   ps2e_movie_take_picture(movie, &picture)        the next decoded picture, RGBA8, in presentation order
 *   ps2e_movie_pcm_read(movie, stereo, frames)      the sound track: 48 kHz 16-bit stereo, interleaved
 *   ps2e_movie_end_input(movie)                     the file's end: the decoder drains
 *
 * The container is Sony's PSS: an ISO/IEC 13818-1 program stream of one MPEG-2 video elementary stream (0xE0) and
 * raw PCM16LE 48 kHz stereo sound tracks in private_stream_1 packets (0xBD, substream prefix FF A0 cc cc: one
 * channel cc per sound track -- a movie with one track per language carries several), each track with its own Sony
 * ADS header. The decoder decodes whatever it is fed: the producer paces the feeding (feed until a picture is ready,
 * show it, feed again) so decoding stays just ahead of presentation, as the PS2's did.
 *
 * Threading: one thread owns a movie from create to destroy. Pictures and PCM are produced on the feeding thread.
 */
#ifndef PS2E_MOVIE_H
#define PS2E_MOVIE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2e_movie ps2e_movie;

typedef struct ps2e_movie_config {
    int audio_channel;   /* the sound track to play: the channel cc of the FF A0 cc cc prefix; -1 = every channel (a
                            single-track movie) */
    int max_pictures;    /* decoded pictures kept waiting before feed stops decoding video (the rest of the fed bytes
                            wait inside the decoder); 0 = the default (4) */
    int planar;          /* 1 = 4:2:0 pictures come out as their Y, U, V planes (ps2e_movie_picture.planes), no RGBA
                            conversion (a consumer that converts on the GPU: the Vita's movie textures); 0 = RGBA8 */
} ps2e_movie_config;

ps2e_movie* ps2e_movie_create(const ps2e_movie_config* config);
void ps2e_movie_destroy(ps2e_movie* movie);

/* Appends `size` program-stream bytes and demuxes / decodes every packet they complete. Returns the pictures ready
 * after the call (ps2e_movie_pictures_ready). Bytes past the stream's end (the program end code, or a video sequence
 * end) are discarded. */
int ps2e_movie_feed(ps2e_movie* movie, const void* bytes, size_t size);
/* No more bytes will come (the file's end): the demuxer takes its tail, the video decoder drains its last pictures. */
void ps2e_movie_end_input(ps2e_movie* movie);

/* ---- pictures ------------------------------------------------------------------------------------------------ */
typedef struct ps2e_movie_picture {
    int width;             /* the picture's own size (the sequence header's; the game knows the movie's) */
    int height;
    const uint8_t* rgba8;  /* width * height * 4 bytes, top row first, alpha 0xFF; valid until the next take or destroy
                              (NULL for a planar picture) */
    int planar;            /* 1: a planar picture (ps2e_movie_config.planar): `planes` holds Y (width x height), then U,
                              then V (each (width + 1) / 2 x (height + 1) / 2), top rows first, no row padding, limited
                              (MPEG) range; valid until the next take or destroy. 0: RGBA in rgba8 */
    const uint8_t* planes;
} ps2e_movie_picture;

int ps2e_movie_pictures_ready(const ps2e_movie* movie);
/* Pops the oldest decoded picture. Returns 1, or 0 when none is ready (*picture untouched). */
int ps2e_movie_take_picture(ps2e_movie* movie, ps2e_movie_picture* picture);
/* The stream's end was seen (a program end code, a video sequence end, or end_input) */
int ps2e_movie_input_ended(const ps2e_movie* movie);
/* Nothing more will come out: the input ended and every decoded picture was taken */
int ps2e_movie_ended(const ps2e_movie* movie);
/* The video's frame rate from its sequence header (known once the first picture decoded), 0.0 before */
double ps2e_movie_fps(const ps2e_movie* movie);

/* ---- sound ---------------------------------------------------------------------------------------------------- */
#define PS2E_MOVIE_PCM_RATE 48000
/* 1 once the selected track's ADS header was accepted (PCM16LE 48 kHz stereo); 0 before, or when the track is of
 * another format (then no PCM ever comes: play silence) */
int ps2e_movie_has_audio(const ps2e_movie* movie);
int ps2e_movie_audio_disabled(const ps2e_movie* movie);
/* Decoded stereo frames waiting */
uint32_t ps2e_movie_pcm_queued(const ps2e_movie* movie);
/* Pops up to `frames` interleaved stereo frames (L R L R ...) into `stereo`. Returns the frames copied. */
uint32_t ps2e_movie_pcm_read(ps2e_movie* movie, int16_t* stereo, uint32_t frames);

/* ---- counters -------------------------------------------------------------------------------------------------- */
typedef struct ps2e_movie_counters {
    uint64_t bytes_fed;
    uint64_t packs;              /* program-stream packs walked */
    uint64_t video_es_bytes;     /* video elementary-stream bytes handed to the decoder */
    uint64_t audio_payloads;     /* audio PES payloads seen (every channel) */
    uint64_t audio_skipped;      /* of another channel than the selected one */
    uint64_t pictures_decoded;
    uint64_t pictures_taken;
    uint64_t pcm_frames_pushed;
    uint64_t pcm_frames_read;
} ps2e_movie_counters;
void ps2e_movie_counts(const ps2e_movie* movie, ps2e_movie_counters* out);

/* The video decoder's library as "MAJOR.MINOR.MICRO" (the decoded bytes depend on it; a judge records it) */
const char* ps2e_movie_decoder_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2E_MOVIE_H */
