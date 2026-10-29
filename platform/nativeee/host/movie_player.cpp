// movie_player.cpp -- see movie_player.h. Also the ps2e_movie_* bodies (movie.h's nine imports).
#include "movie_player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <cfenv>
#include <new>
#include <thread>

#include "audio_service.h"
#include "disc_reader.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/platform/ps2e/ps2e_draw.h"
#include "engine/platform/ps2e/ps2e_movie.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "te_graphics_hooks.h"
#include "te_host.h"
#include "te_prof.h"
#include "te_services.h"
#include "te_sync.h"
#include "te_weak.h"
#include "vita_gpu_census.h"
#if defined(__vita__)
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#endif

extern "C" uint64_t te_graphics_presents();   // host/graphics.cpp: frames presented so far

// A host executable with neither the runner nor the test harness (the graphics self-test) has no module to call back
// into: the weak default. The runner's and the harness's definitions are strong (te_weak.h: COFF spells it /alternatename).
TE_WEAK_DEFAULT(int, te_call_void_callback, (uint32_t), { return 0; })

namespace te::movie {

namespace {
bool Trace() {
    static const bool on = [] { const char* v = std::getenv("TE_MOVIE_TRACE"); return v && *v && std::strcmp(v, "0") != 0; }();
    return on;
}
double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

// ---- the Vita's movie pictures in the GPU's storage (TE_MOVIE_PICTURES) ------------------------------------------------
// A movie's pictures go to a RING of slots (ps2e_draw_movie_create: each slot's storage CPU-writable textures of the GPU --
// yuv: the decoder's three 4:2:0 planes, converted by the device's movie program; rgba: swscale's picture), made on the
// drawing thread when the movie opens. The MOVIE THREAD writes each picture it decodes straight into a free slot (no copy
// of it anywhere else: the game thread passes the slot along with the picture, the drawing thread only makes it the slot
// shown -- ps2e_draw_movie_show, a few instructions); a picture the movie thread could not place (decoded before the ring
// existed, or no slot free) keeps its bytes and the drawing thread copies it into a slot when it is shown. A slot is free
// again when no picture holds it and the GPU has finished every scene that may have sampled it -- the scenes ended when it
// stopped being the one shown (vitaGL's scene fence, te::vita_gpu::SceneFence; without one, 4 presents later: the horizon
// vitaGL's own garbage collector and the vertex ring keep). The draw is the device's movie program (GlDevice::
// DrawMoviePicture), not the PS2 program: no lean variant served TFX DECAL, so the movie quad used to take the 229-
// instruction fast program and its mirrored uniforms.
// TE_MOVIE_PICTURES (the Vita; elsewhere always legacy): yuv (default) | rgba (the ring with swscale's RGBA pictures: the
// same texels as legacy) | legacy (the earlier form: one RGBA texture re-specified with glTexImage2D per picture ON THE DRAWING THREAD --
// on the device a 1.2 MB vitaGL allocation + copy + ScaleAlpha + MaxAlpha per 640x480 picture -- drawn by the PS2 program).
// The yuv pictures are 0.45 MB at 640x480 instead of 1.2 MB and skip swscale; the conversion is the GPU's float BT.601
// (bilinear chroma), so a pixel may differ from swscale's RGBA by a level or two.
namespace {
enum class PictureMode { kLegacy, kRgba, kYuv };
PictureMode Mode() {
    static const PictureMode mode = [] {
#if defined(__vita__)
        const char* v = std::getenv("TE_MOVIE_PICTURES");
        if (v && std::strcmp(v, "legacy") == 0) return PictureMode::kLegacy;
        if (v && std::strcmp(v, "rgba") == 0) return PictureMode::kRgba;
        return PictureMode::kYuv;
#else
        return PictureMode::kLegacy;
#endif
    }();
    return mode;
}
const char* ModeName(PictureMode m) { return m == PictureMode::kYuv ? "yuv" : m == PictureMode::kRgba ? "rgba" : "legacy"; }

void CopyBytes(void* dst, const void* src, size_t n) {
#if defined(__vita__)
    sceClibMemcpy(dst, src, SceSize(n));   // (the GPU's storage is uncached: SceLibKernel's copy streams it)
#else
    std::memcpy(dst, src, n);
#endif
}

#if defined(__vita__)
// Where a slot plane's storage lives -- the memory block's type (sceKernelGetMemBlockInfoByAddr; vitaGL
// allocates a texture VRAM -> RAM -> PHYCONT -> BUDGET -> the newlib heap, gpu_utils.c gpu_alloc_mapped_aligned_for_gpu):
// CDRAM (USER_CDRAM_RW, non-cached), vitaGL's RAM / PHYCONT / CDIALOG pools (their non-cached types: this build never
// calls vglUseCachedMem, so vgl_mem_init takes them), or a CACHED block (only vitaGL's last fallback, the newlib heap,
// which it maps for the GPU: the movie thread's stores to it can sit in the CPU's data cache, unseen by the GPU).
enum PlaneKind { kPlaneCdram, kPlaneMainNc, kPlanePhycontNc, kPlaneCdialogNc, kPlaneOtherNc, kPlaneCached, kPlaneUnknown,
                 kPlaneKinds };
const char* const kPlaneKindNames[kPlaneKinds] = {"CDRAM", "RAM uncached", "PHYCONT uncached", "CDIALOG uncached",
                                                  "other uncached", "CACHED", "unknown"};
PlaneKind ClassifyPlane(const void* p, SceUID* block) {
    *block = -1;
    SceKernelMemBlockInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetMemBlockInfoByAddr(const_cast<void*>(p), &info) < 0) return kPlaneUnknown;
    const uint32_t type = uint32_t(info.type);
    // the type's cache field (sysmem.h's SCE_KERNEL_MEMBLOCK_TYPE_*: bits 12-15): 0xD cached (L1 + L2 write-back), 0x4 CDRAM
    // with an L1 write-back cache, 0x8 non-cached; another value (a device / unlisted type): the block's memory type says
    const uint32_t cache = (type >> 12) & 0xFu;
    const bool cached = cache == 0xDu || cache == 0x4u ||
                        (cache != 0x8u && info.memoryType == SCE_KERNEL_MEMORY_TYPE_NORMAL);
    if (cached) {
        *block = sceKernelFindMemBlockByAddr(p, 0);
        return kPlaneCached;
    }
    switch (type) {
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW: return kPlaneCdram;
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE: return kPlaneMainNc;
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW: return kPlanePhycontNc;
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW: return kPlaneCdialogNc;
    default: return cache == 0x8u || info.memoryType == SCE_KERNEL_MEMORY_TYPE_NORMAL_NC ? kPlaneOtherNc : kPlaneUnknown;
    }
}
// TE_MOVIE_CACHE_FLUSH=0|1 (default 1): a plane found in a CACHED block is written back after each picture written into
// it (sceKernelSyncVMDomain over the bytes written: usermode's only data-cache clean -- documented for VM-domain blocks;
// its result on another block is logged); the uncached planes need none (every device and emulator log so far: none
// cached)
bool CacheFlushOn() {
    static const bool on = [] { const char* v = std::getenv("TE_MOVIE_CACHE_FLUSH"); return !(v && std::strcmp(v, "0") == 0); }();
    return on;
}
#endif

void SleepMs(int ms) {
#if defined(__vita__)
    sceKernelDelayThread(SceUInt(ms * 1000));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

// A picture's mean brightness over a sparse grid (1 in 16 x 16): Y for planes, G for RGBA -- the census's check that the
// decoder made a picture (a picture that decoded black shows ~16 / ~0, an untouched slot 0)
int SampleLuma(int w, int h, const uint8_t* rgba, const uint8_t* planes) {
    if (w <= 0 || h <= 0 || (!rgba && !planes)) return -1;
    uint64_t sum = 0, n = 0;
    for (int y = 0; y < h; y += 16)
        for (int x = 0; x < w; x += 16) {
            sum += planes ? planes[size_t(y) * size_t(w) + size_t(x)] : rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + 1];
            ++n;
        }
    return n ? int(sum / n) : -1;
}

// The last resort when a yuv picture cannot go to a ring (the ring could not be made): its planes as RGBA on the CPU (the
// same BT.601 limited-range matrix as the GPU's, in integers), so the RGBA upload can take it
void PlanesToRgba(int w, int h, const uint8_t* planes, std::vector<uint8_t>& rgba) {
    const int cw = (w + 1) / 2;
    const uint8_t* yp = planes;
    const uint8_t* up = planes + size_t(w) * size_t(h);
    const uint8_t* vp = up + size_t(cw) * size_t((h + 1) / 2);
    rgba.resize(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int c = 298 * (int(yp[size_t(y) * size_t(w) + size_t(x)]) - 16);
            const int d = int(up[size_t(y / 2) * size_t(cw) + size_t(x / 2)]) - 128;
            const int e = int(vp[size_t(y / 2) * size_t(cw) + size_t(x / 2)]) - 128;
            uint8_t* o = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            o[0] = uint8_t(std::clamp((c + 409 * e + 128) >> 8, 0, 255));
            o[1] = uint8_t(std::clamp((c - 100 * d - 208 * e + 128) >> 8, 0, 255));
            o[2] = uint8_t(std::clamp((c + 516 * d + 128) >> 8, 0, 255));
            o[3] = 0xFF;
        }
}
}  // namespace

class PictureRing {
public:
    static constexpr int kMaxSlots = 16;
    PictureRing(int width, int height, bool planar) : w(width), h(height), yuv(planar) {}
    const int w, h;
    const bool yuv;
    int slots = 0;
    uint32_t id = 0;   // the ps2e_draw movie texture (0: not made)
    struct Plane { uint8_t* data = nullptr; size_t stride = 0; };
    Plane plane[kMaxSlots][3];

    // The GL thread: the slots' storage made, their CPU addresses read (id 0 when the device has no movie textures)
    void Create(ps2e_draw* dev) {
        // the movie thread's lead (3 pictures + a pump's) + the game thread's queue (3) + the one shown + the GPU's lag (the
        // scenes not finished yet: in Vita3K a host pipeline compile can hold several frames' worth)
        const int want = yuv ? 16 : 10;
        id = ps2e_draw_movie_create(dev, w, h, yuv ? PS2E_DRAW_MOVIE_YUV420 : PS2E_DRAW_MOVIE_RGBA, want);
        if (!id) return;
        slots = want;
        for (int s = 0; s < slots; ++s)
            for (int k = 0; k < (yuv ? 3 : 1); ++k) {
                plane[s][k].data = static_cast<uint8_t*>(ps2e_draw_movie_plane(dev, id, s, k, &plane[s][k].stride));
                if (!plane[s][k].data) {
                    ps2e_draw_texture_destroy(dev, id);
                    id = 0;
                    slots = 0;
                    return;
                }
            }
        ps2e_draw_movie_counts(dev, &drawn_at_create, &refused_at_create);
#if defined(__vita__)
        for (int s = 0; s < slots; ++s)
            for (int k = 0; k < (yuv ? 3 : 1); ++k) {
                const PlaneKind kind = ClassifyPlane(plane[s][k].data, &block_[s][k]);
                cached_[s][k] = kind == kPlaneCached;
                ++kinds[kind];
            }
#endif
    }
    // a picture of this movie's kind and size (a decoder's, or one kept in bytes)
    bool Fits(int width, int height, int planar, const uint8_t* rgba, const uint8_t* planes) const {
        return id && width == w && height == h && (planar != 0) == yuv && (yuv ? planes != nullptr : rgba != nullptr);
    }
    // A free slot, held for a picture (any thread); -1 when none is free
    int Reserve() {
        std::lock_guard<sync::Mutex> lock(mutex);
        uint32_t issued = 0, done = 0;
        const bool fence = te::vita_gpu::SceneFence(&issued, &done);
        const uint32_t presents = uint32_t(te_graphics_presents());
        for (int k = 0; k < slots; ++k) {
            const int s = (next_ + k) % slots;
            if (state_[s] == kRetired) {
                const bool gpu_done = fenced_[s] ? fence && int32_t(done - mark_[s]) >= 0 : presents - mark_[s] >= 4u;
                if (gpu_done) state_[s] = kFree;
            }
            if (state_[s] == kFree) {
                state_[s] = kHeld;
                next_ = (s + 1) % slots;
                return s;
            }
        }
        // none free: what held them (the census's worst moment)
        ++full;
        int held = 0, retired = 0;
        for (int s = 0; s < slots; ++s) {
            held += state_[s] == kHeld ? 1 : 0;
            retired += state_[s] == kRetired ? 1 : 0;
        }
        full_held = held;
        full_retired = retired;
        full_lag = fence ? issued - done : 0xFFFFFFFFu;
        return -1;
    }
    // A held slot's picture dropped unseen (any thread): free at once, the GPU never sampled it
    void Unreserve(int s) {
        std::lock_guard<sync::Mutex> lock(mutex);
        if (s >= 0 && s < slots && state_[s] == kHeld) state_[s] = kFree;
    }
    // The GL thread, between frames, right after ps2e_draw_movie_show(s): the slot shown before retired at the scenes
    // ended so far (every scene that drew with it)
    void Shown(int s) {
        std::lock_guard<sync::Mutex> lock(mutex);
        if (shown_ >= 0 && shown_ != s) {
            uint32_t issued = 0;
            fenced_[shown_] = te::vita_gpu::SceneFence(&issued, nullptr);
            mark_[shown_] = fenced_[shown_] ? issued : uint32_t(te_graphics_presents());
            state_[shown_] = kRetired;
        }
        state_[s] = kShown;
        shown_ = s;
        ++shows;
    }
    // A picture's bytes into held slot s (the thread holding it): rows at the storage's stride
    void Write(int s, const uint8_t* rgba, const uint8_t* planes) {
        const auto copy = [](const Plane& p, const uint8_t* src, size_t row, int rows) {
            if (p.stride == row) {
                CopyBytes(p.data, src, row * size_t(rows));
                return;
            }
            for (int y = 0; y < rows; ++y) CopyBytes(p.data + size_t(y) * p.stride, src + size_t(y) * row, row);
        };
        if (yuv) {
            const size_t cw = size_t((w + 1) / 2);
            const int ch = (h + 1) / 2;
            copy(plane[s][0], planes, size_t(w), h);
            copy(plane[s][1], planes + size_t(w) * size_t(h), cw, ch);
            copy(plane[s][2], planes + size_t(w) * size_t(h) + cw * size_t(ch), cw, ch);
        } else {
            copy(plane[s][0], rgba, size_t(w) * 4, h);
        }
        __sync_synchronize();   // the stores reach the GPU's memory before the slot goes on
#if defined(__vita__)
        // a plane in a CACHED block: its bytes written back from the data cache before the GPU may sample them
        // (none so far: the census line at the ring's creation counts them)
        written_.fetch_add(1, std::memory_order_relaxed);
        if (CacheFlushOn()) {
            for (int k = 0; k < (yuv ? 3 : 1); ++k) {
                if (!cached_[s][k]) continue;
                const size_t rows = size_t(k == 0 ? h : (h + 1) / 2);
                const size_t bytes = plane[s][k].stride * rows;
                const int r = block_[s][k] >= 0 ? sceKernelSyncVMDomain(block_[s][k], plane[s][k].data, SceSize(bytes)) : -1;
                if (r < 0) {
                    int none = 0;
                    flush_error_.compare_exchange_strong(none, r);
                    flush_failed_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    flush_ok_.fetch_add(1, std::memory_order_relaxed);
                    flush_bytes_.fetch_add(bytes, std::memory_order_relaxed);
                }
            }
        }
#endif
    }
#if defined(__vita__)
    // the planes' memory (Create): counts by kind (PlaneKind), and the write-backs so far
    int kinds[kPlaneKinds] = {};
    uint64_t Written() const { return written_.load(); }
    uint64_t FlushOk() const { return flush_ok_.load(); }
    uint64_t FlushFailed() const { return flush_failed_.load(); }
    uint64_t FlushBytes() const { return flush_bytes_.load(); }
    int FlushError() const { return flush_error_.load(); }
#endif

    sync::Mutex mutex{"te-movie-ring"};
    // the census (under `mutex` where several threads count)
    uint64_t direct = 0, copied = 0, dropped = 0, fallbacks = 0, slot_waits = 0, shows = 0;
    double slot_wait_ms = 0;
    uint64_t drawn_at_create = 0, refused_at_create = 0;
    // Reserve found no slot free: how often, and the last such moment's held / retired slots and the GPU's scene lag
    uint64_t full = 0;
    int full_held = 0, full_retired = 0;
    uint32_t full_lag = 0;

private:
    enum : uint8_t { kFree = 0, kHeld, kShown, kRetired };
    uint8_t state_[kMaxSlots] = {};
    uint32_t mark_[kMaxSlots] = {};
    bool fenced_[kMaxSlots] = {};
    int shown_ = -1;
    int next_ = 0;
#if defined(__vita__)
    bool cached_[kMaxSlots][3] = {};                 // the plane lies in a cached memory block (Create)
    SceUID block_[kMaxSlots][3] = {};                // that block's id (the write-back's), -1 unknown
    std::atomic<uint64_t> written_{0}, flush_ok_{0}, flush_failed_{0}, flush_bytes_{0};
    std::atomic<int> flush_error_{0};                // the first refused write-back's code
#endif
};

struct SlotLease {
    std::shared_ptr<PictureRing> ring;
    int slot = -1;
};
void SlotLeaseDeleter::operator()(SlotLease* l) const {
    if (l && l->ring && l->slot >= 0) l->ring->Unreserve(l->slot);
    delete l;
}

namespace {
// A decoder's picture as bytes (the planes or the RGBA image), its luma sample with it
void CopyPicture(const ps2e_movie_picture& pic, MoviePlayer::Picture& out) {
    out.width = pic.width;
    out.height = pic.height;
    out.planar = pic.planar;
    out.luma = SampleLuma(pic.width, pic.height, pic.rgba8, pic.planes);
    if (pic.planar && pic.planes) {
        const size_t cw = size_t((pic.width + 1) / 2), ch = size_t((pic.height + 1) / 2);
        out.planes.assign(pic.planes, pic.planes + size_t(pic.width) * size_t(pic.height) + 2 * cw * ch);
    } else if (pic.rgba8) {
        out.rgba.assign(pic.rgba8, pic.rgba8 + size_t(pic.width) * size_t(pic.height) * 4);
    }
}

// A decoder's picture into `out`: written straight into a free slot of `ring` when the ring fits it (waiting a little
// for one while `may_wait`: the game thread is not waiting for this pump), else its bytes copied
void MakePicture(const ps2e_movie_picture& pic, MoviePlayer::Picture& out, const std::shared_ptr<PictureRing>& ring,
                 bool may_wait) {
    if (ring && ring->Fits(pic.width, pic.height, pic.planar, pic.rgba8, pic.planes)) {
        int slot = ring->Reserve();
        if (slot < 0 && may_wait) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 40 && slot < 0; ++i) {
                SleepMs(1);
                slot = ring->Reserve();
            }
            std::lock_guard<sync::Mutex> lock(ring->mutex);
            ++ring->slot_waits;
            ring->slot_wait_ms += MsSince(t0);
        }
        if (slot >= 0) {
            {
                TE_PROF_SCOPE("movie: picture into the GPU's storage");
                ring->Write(slot, pic.rgba8, pic.planes);
            }
            out.width = pic.width;
            out.height = pic.height;
            out.planar = pic.planar;
            out.luma = SampleLuma(pic.width, pic.height, pic.rgba8, pic.planes);
            out.lease.reset(new SlotLease{ring, slot});
            std::lock_guard<sync::Mutex> lock(ring->mutex);
            ++ring->direct;
            return;
        }
        std::lock_guard<sync::Mutex> lock(ring->mutex);
        ++ring->fallbacks;
    }
    CopyPicture(pic, out);
}
}  // namespace

// ---- the decoder: ps2e_movie + the disc pump, on the game thread or on the movie thread -----------------------------------
// The player's calls are ps2e_movie's (pictures ready, take, ended, PCM queued / read, the audio flags, the rate) plus the
// pump: one kReadSlice slice of the file off the disc into the decoder per call, the input ended at the file's end.
class Decoder {
public:
    virtual ~Decoder() = default;
    virtual bool Pump(uint32_t* got) = 0;   // false once the input is done (nothing read)
    virtual bool InputDone() const = 0;
    virtual uint32_t ReadCursor() const = 0;
    virtual int PicturesReady() const = 0;
    virtual bool Ended() const = 0;
    virtual bool TakePicture(MoviePlayer::Picture& out) = 0;
    virtual uint32_t PcmQueued() const = 0;
    virtual uint32_t PcmRead(int16_t* stereo, uint32_t frames) = 0;
    virtual bool HasAudio() const = 0;
    virtual bool AudioDisabled() const = 0;
    virtual double Fps() const = 0;
    virtual bool Threaded() const { return false; }
};
void DecoderDeleter::operator()(Decoder* d) const { delete d; }

namespace {

// One pump: MoviePlayer's former Pump body over a ps2e_movie (on whichever thread decodes). `read` gives the slice's bytes.
template <typename Read>
bool PumpSlice(ps2e_movie* movie, uint32_t file_size, uint32_t& cursor, bool& input_done, uint32_t* got_out, Read&& read) {
    *got_out = 0;
    if (!movie || input_done) return false;
    if (cursor >= file_size) {
        input_done = true;
        ps2e_movie_end_input(movie);
        return false;
    }
    const uint32_t n = std::min<uint32_t>(MoviePlayer::kReadSlice, file_size - cursor);
    const uint8_t* bytes = nullptr;
    std::size_t got;
    {
        TE_PROF_SCOPE("movie: disc read");
        got = read(cursor, n, bytes);
    }
    cursor += static_cast<uint32_t>(got);
    *got_out = static_cast<uint32_t>(got);
    if (got) {
        TE_PROF_SCOPE("movie: demux + decode (FFmpeg)");
        ps2e_movie_feed(movie, bytes, got);
    }
    if (got < n || cursor >= file_size) {
        input_done = true;
        ps2e_movie_end_input(movie);
    }
    return true;
}

// The game thread decodes (the desktop's default; TE_MOVIE_THREAD=0 on the Vita): every call is ps2e_movie's, at once.
class SyncDecoder final : public Decoder {
public:
    SyncDecoder(ps2e_movie* movie, const IsoFs* iso, uint64_t image_offset, uint32_t file_size)
        : movie_(movie), iso_(iso), image_offset_(image_offset), file_size_(file_size) {}
    ~SyncDecoder() override { ps2e_movie_destroy(movie_); }
    bool Pump(uint32_t* got) override {
        return PumpSlice(movie_, file_size_, cursor_, input_done_, got, [this](uint32_t at, uint32_t n, const uint8_t*& bytes) {
            slice_.resize(n);
            bytes = slice_.data();
            return iso_ ? iso_->ReadBytes(image_offset_ + at, n, slice_.data()) : std::size_t(0);
        });
    }
    bool InputDone() const override { return input_done_; }
    uint32_t ReadCursor() const override { return cursor_; }
    int PicturesReady() const override { return ps2e_movie_pictures_ready(movie_); }
    bool Ended() const override { return ps2e_movie_ended(movie_) != 0; }
    bool TakePicture(MoviePlayer::Picture& out) override {
        ps2e_movie_picture pic{};
        if (!ps2e_movie_take_picture(movie_, &pic)) return false;
        CopyPicture(pic, out);
        return true;
    }
    uint32_t PcmQueued() const override { return ps2e_movie_pcm_queued(movie_); }
    uint32_t PcmRead(int16_t* stereo, uint32_t frames) override { return ps2e_movie_pcm_read(movie_, stereo, frames); }
    bool HasAudio() const override { return ps2e_movie_has_audio(movie_) != 0; }
    bool AudioDisabled() const override { return ps2e_movie_audio_disabled(movie_) != 0; }
    double Fps() const override { return ps2e_movie_fps(movie_); }
private:
    ps2e_movie* movie_;
    const IsoFs* iso_;
    uint64_t image_offset_;
    uint32_t file_size_, cursor_ = 0;
    bool input_done_ = false;
    std::vector<uint8_t> slice_;
};

// ---- the movie thread: the decode (disc reads, demux, MPEG-2, RGBA) off the game thread ---------------------------------
// The thread runs the SAME pumps on its own ps2e_movie -- slice k of the file is the same bytes whoever reads it -- but
// without the picture hold-back (max_pictures unbounded, every picture and every PCM frame moved out after each pump), and
// records after each pump what the game thread's decoder would then hold in total: the pictures and PCM frames produced,
// the input's end, the audio flags, the rate. The game thread (ThreadDecoder) replays its own pumps from those records:
// after K pumps and T pictures taken it answers "pictures ready" with total(K) - T and "ended" with input ended(K) && none
// left -- exactly the answers its own decoder would give at every point the player asks (that decoder holds video back
// only while 3 pictures wait, so whenever it has none ready it has decoded all it was fed, as the unbounded one has; the
// parser cuts the elementary stream into the same packets however it is chunked) -- and takes the pictures and PCM in
// order. So the player's sequence of Steps, pictures, PCM pops and its end are the same as with the decoder on the game
// thread (the harness legs with TE_MOVIE_THREAD=1 vs 0 check it); only where the work happens changes. The thread runs
// at most kAheadPictures pictures / kAheadPumps pumps ahead of the game thread's takes, and always when the game thread
// waits for a pump. One movie at a time (the game plays one); the thread's state is static (te_sync.h).
class MovieThread {
public:
    struct Record {
        bool ret = false, input_done = false;
        uint32_t got = 0, cursor = 0;
        uint64_t pictures = 0, pcm = 0;   // totals produced after this pump
        bool input_ended = false, has_audio = false, audio_disabled = false;
        double fps = 0.0;
    };
    static constexpr uint64_t kAheadPictures = 3, kAheadPumps = 6;

    static MovieThread& Get() {
        alignas(MovieThread) static unsigned char storage[sizeof(MovieThread)];
        static MovieThread* thread = new (storage) MovieThread;
        return *thread;
    }

    // A movie to decode; false when the thread could not start or another movie still holds it
    bool Begin(const IsoFs* iso, uint64_t image_offset, uint32_t file_size, int channel) {
        std::unique_lock<sync::Mutex> lock(mutex_);
        if (active_) return false;
        if (!started_) {
            started_ = true;
            try {
                std::thread([this] { Run(); }).detach();
                ok_ = true;
            } catch (...) {
                ok_ = false;
                std::fprintf(stderr, "[twinsanityevolved] movie: the decoder thread did not start: decoding on the game thread\n");
            }
        }
        if (!ok_) return false;
        iso_ = iso;
        image_offset_ = image_offset;
        file_size_ = file_size;
        channel_ = channel;
        std::fegetenv(&env_);   // the game thread's (the EE's rounding toward zero): the decode runs under it, as it did here
        records_.clear();
        pictures_.clear();
        pcm_.clear();
        ring_.reset();
        made_ = consumed_ = wanted_ = 0;
        pictures_made_ = pictures_taken_ = 0;
        cancel_ = false;
        active_ = true;
        ++generation_;
        work_.notify_all();
        return true;
    }

    // The game thread: the movie's ring of picture slots (the Vita's movie path) -- the pictures decoded from now on are
    // written into its free slots; nullptr: none (bytes)
    void SetRing(std::shared_ptr<PictureRing> ring) {
        std::lock_guard<sync::Mutex> lock(mutex_);
        ring_ = std::move(ring);
    }

    // The game thread lets the movie go: the thread finishes its pump, drops the decoder, and is free again
    void Cancel() {
        std::unique_lock<sync::Mutex> lock(mutex_);
        if (!active_) return;
        cancel_ = true;
        work_.notify_all();
        while (active_) idle_.wait(lock);
        records_.clear();
        pictures_.clear();   // (their slots released unseen)
        pcm_.clear();
        ring_.reset();
    }

    // The game thread's next pump's record (they come in order)
    Record Next() {
        std::unique_lock<sync::Mutex> lock(mutex_);
        const uint64_t k = consumed_ + 1;
        if (wanted_ < k) {
            wanted_ = k;
            work_.notify_all();
        }
        if (made_ < k) {
            TE_PROF_SCOPE("movie: waiting for the decoder thread");
            const auto t0 = std::chrono::steady_clock::now();
            while (made_ < k && active_ && !cancel_) made_cond_.wait(lock);
            const double ms = MsSince(t0);
            ++waits_;
            wait_ms_ += ms;
            wait_max_ms_ = std::max(wait_max_ms_, ms);
        }
        if (records_.empty()) return Record{};   // (cancelled under the game thread: never, it cancels itself)
        Record r = records_.front();
        records_.pop_front();
        ++consumed_;
        return r;
    }
    // The next picture (a record counted it: it is here)
    bool Take(MoviePlayer::Picture& out) {
        std::lock_guard<sync::Mutex> lock(mutex_);
        if (pictures_.empty()) return false;
        out = std::move(pictures_.front());
        pictures_.pop_front();
        ++pictures_taken_;
        work_.notify_all();
        return true;
    }
    // PCM frames a record counted
    uint32_t ReadPcm(int16_t* stereo, uint32_t frames) {
        std::lock_guard<sync::Mutex> lock(mutex_);
        const uint32_t n = std::min<uint32_t>(frames, uint32_t(pcm_.size() / 2));
        for (uint32_t i = 0; i < n * 2; ++i) stereo[i] = pcm_[i];
        pcm_.erase(pcm_.begin(), pcm_.begin() + std::ptrdiff_t(n) * 2);
        return n;
    }
    // The game thread's waits for a pump (the trace's closing line)
    void Waits(uint64_t& n, double& ms, double& max_ms) {
        std::lock_guard<sync::Mutex> lock(mutex_);
        n = waits_; ms = wait_ms_; max_ms = wait_max_ms_;
        waits_ = 0; wait_ms_ = wait_max_ms_ = 0;
    }

private:
    MovieThread() = default;

    static double MsSince(std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    void Run() {
#if defined(__vita__)
        // below the game (96) and the render thread (128: it presents the pictures), above the builders (144: idle while a
        // movie plays); FFmpeg's slice workers keep the pthread default. TE_MOVIE_PRIO=64..191 for an A/B.
        int prio = 136;
        if (const char* v = std::getenv("TE_MOVIE_PRIO")) { const int q = std::atoi(v); if (q >= 64 && q <= 191) prio = q; }
        sceKernelChangeThreadPriority(0, prio);
#endif
        te::prof::BindThread(7);   // TE_PROF's "movie" slot; the CPU census's te-movie
        std::unique_lock<sync::Mutex> lock(mutex_);
        for (;;) {
            {
                TE_PROF_SCOPE("movie thread idle");
                while (!active_ || cancel_) {
                    if (cancel_) {
                        cancel_ = false;
                        active_ = false;
                        idle_.notify_all();
                    }
                    work_.wait(lock);
                }
            }
            const uint64_t generation = generation_;
            const IsoFs* iso = iso_;
            const uint64_t image_offset = image_offset_;
            const uint32_t file_size = file_size_;
            ps2e_movie_config cfg{};
            cfg.audio_channel = channel_;
            cfg.max_pictures = 1 << 20;   // nothing held back: every picture comes out at the pump that decoded it
            cfg.planar = Mode() == PictureMode::kYuv ? 1 : 0;
            std::fesetenv(&env_);
            lock.unlock();
            ps2e_movie* movie = ps2e_movie_create(&cfg);
            uint32_t cursor = 0;
            bool input_done = false;
            Record last;
            te::io::Handle<te::io::Job> next;   // the next slice's read, in flight while this one decodes
            const auto submit = [&](uint32_t at) {
                if (at >= file_size || !iso) return;
                next.reset(new te::io::Job);
                next->offset = image_offset + at;
                next->size = std::min<uint32_t>(MoviePlayer::kReadSlice, file_size - at);
                next->site = "movie slice";
                te::io::Submit(next.get(), iso);
            };
            submit(0);
            lock.lock();
            while (!cancel_ && generation == generation_ && !input_done) {
                // run when the game thread waits for a pump, or while it is not too far behind
                {
                    TE_PROF_SCOPE("movie thread ahead (waits for the game's takes)");
                    while (!cancel_ && !(wanted_ > made_ || (pictures_made_ - pictures_taken_ < kAheadPictures && made_ - consumed_ < kAheadPumps)))
                        work_.wait(lock);
                }
                if (cancel_) break;
                // the ring as it stands (pictures go to its slots), and whether the game thread waits for this pump (then a
                // picture never waits for a slot: it keeps its bytes)
                const std::shared_ptr<PictureRing> ring = ring_;
                const bool game_waits = wanted_ > made_;
                lock.unlock();
                Record r;
                std::vector<uint8_t> slice;
                r.ret = PumpSlice(movie, file_size, cursor, input_done, &r.got, [&](uint32_t at, uint32_t n, const uint8_t*& bytes) {
                    std::size_t got = 0;
                    if (next && next->offset == image_offset + at && next->size == n) {
                        te::io::Wait(next.get(), "the movie thread's next slice");
                        slice = std::move(next->bytes);
                        got = next->got;
                        next.reset();
                    } else if (iso) {
                        slice.resize(n);
                        got = iso->ReadBytes(image_offset + at, n, slice.data());
                    }
                    submit(at + uint32_t(got));   // the slice after it, read while this one decodes
                    bytes = slice.data();
                    return got;
                });
                r.input_done = input_done;
                r.cursor = cursor;
                // everything the pump produced, out of the decoder
                std::deque<MoviePlayer::Picture> made;
                std::vector<int16_t> pcm;
                uint32_t frames = 0;
                {
                    TE_PROF_SCOPE("movie: pictures + PCM out of the decoder");
                    ps2e_movie_picture pic{};
                    while (ps2e_movie_take_picture(movie, &pic)) {
                        made.emplace_back();
                        MakePicture(pic, made.back(), ring, !game_waits);
                    }
                    pcm.resize(size_t(ps2e_movie_pcm_queued(movie)) * 2);
                    frames = pcm.empty() ? 0 : ps2e_movie_pcm_read(movie, pcm.data(), uint32_t(pcm.size() / 2));
                }
                r.pictures = last.pictures + made.size();
                r.pcm = last.pcm + frames;
                r.input_ended = ps2e_movie_input_ended(movie) != 0;
                r.has_audio = ps2e_movie_has_audio(movie) != 0;
                r.audio_disabled = ps2e_movie_audio_disabled(movie) != 0;
                r.fps = ps2e_movie_fps(movie);
                last = r;
                lock.lock();
                if (cancel_ || generation != generation_) break;
                for (auto& m : made) pictures_.push_back(std::move(m));
                pictures_made_ += made.size();
                pcm_.insert(pcm_.end(), pcm.begin(), pcm.begin() + std::ptrdiff_t(frames) * 2);
                records_.push_back(r);
                ++made_;
                made_cond_.notify_all();
            }
            // the movie's input is done (its records all made) or it was let go: wait for the cancel, then drop the decoder
            {
                TE_PROF_SCOPE("movie thread idle");
                while (!cancel_ && generation == generation_) work_.wait(lock);
            }
            lock.unlock();
            next.reset();
            ps2e_movie_destroy(movie);
            lock.lock();
        }
    }

    sync::Mutex mutex_{"te-movie"};
    sync::Condition work_{mutex_, "te-movie-work"}, made_cond_{mutex_, "te-movie-made"}, idle_{mutex_, "te-movie-idle"};
    bool started_ = false, ok_ = false, active_ = false, cancel_ = false;
    uint64_t generation_ = 0;
    const IsoFs* iso_ = nullptr;
    uint64_t image_offset_ = 0;
    uint32_t file_size_ = 0;
    int channel_ = -1;
    std::fenv_t env_{};
    std::deque<Record> records_;
    uint64_t made_ = 0, consumed_ = 0, wanted_ = 0;
    std::deque<MoviePlayer::Picture> pictures_;
    uint64_t pictures_made_ = 0, pictures_taken_ = 0;
    std::deque<int16_t> pcm_;
    std::shared_ptr<PictureRing> ring_;   // SetRing's (the Vita's movie path)
    uint64_t waits_ = 0;
    double wait_ms_ = 0, wait_max_ms_ = 0;
};

// The game thread's view of the movie thread's decoder: its own pumps replayed from the records (see MovieThread)
class ThreadDecoder final : public Decoder {
public:
    ~ThreadDecoder() override { MovieThread::Get().Cancel(); }
    bool Pump(uint32_t* got) override {
        *got = 0;
        if (last_.input_done) return false;
        last_ = MovieThread::Get().Next();
        *got = last_.got;
        return last_.ret;
    }
    bool InputDone() const override { return last_.input_done; }
    uint32_t ReadCursor() const override { return last_.cursor; }
    int PicturesReady() const override { return static_cast<int>(last_.pictures - taken_); }
    bool Ended() const override { return last_.input_ended && last_.pictures == taken_; }
    bool TakePicture(MoviePlayer::Picture& out) override {
        if (last_.pictures == taken_ || !MovieThread::Get().Take(out)) return false;
        ++taken_;
        return true;
    }
    uint32_t PcmQueued() const override { return static_cast<uint32_t>(last_.pcm - pcm_read_); }
    uint32_t PcmRead(int16_t* stereo, uint32_t frames) override {
        const uint32_t n = MovieThread::Get().ReadPcm(stereo, std::min(frames, PcmQueued()));
        pcm_read_ += n;
        return n;
    }
    bool HasAudio() const override { return last_.has_audio; }
    bool AudioDisabled() const override { return last_.audio_disabled; }
    double Fps() const override { return last_.fps; }
    bool Threaded() const override { return true; }
private:
    MovieThread::Record last_;   // the state after the pumps so far (none: a fresh decoder's)
    uint64_t taken_ = 0, pcm_read_ = 0;
};

// TE_MOVIE_THREAD=0|1: the decode on the game thread / on the movie thread (unset: the Vita on, elsewhere off)
bool MovieThreadWanted() {
    static const bool on = [] {
        const char* v = std::getenv("TE_MOVIE_THREAD");
#if defined(__vita__)
        return !(v && v[0] == '0');
#else
        return v && v[0] == '1';
#endif
    }();
    return on;
}

}  // namespace

MoviePlayer::MoviePlayer() = default;
MoviePlayer::~MoviePlayer() {
    for (auto& [address, p] : players_) DestroyDecoder(p);
}

MoviePlayer::Player* MoviePlayer::Find(uint32_t player) {
    const auto it = players_.find(player);
    if (it == players_.end()) { ++count_.unknown_player; return nullptr; }
    return &it->second;
}

const MoviePlayer::Player* MoviePlayer::Inspect(uint32_t player) const {
    const auto it = players_.find(player);
    return it == players_.end() ? nullptr : &it->second;
}

void MoviePlayer::Reset() {
    for (auto& [address, p] : players_) DestroyDecoder(p);
    players_.clear();
    playing_ = nullptr;
    count_ = {};
}

uint64_t MoviePlayer::DueField(uint64_t present, double fps, uint32_t hz) {
    if (!(fps > 0.0) || hz == 0) return present * 2;   // the rate unknown: every second blank, as the PS2 presented
    return static_cast<uint64_t>(std::floor(static_cast<double>(present) * static_cast<double>(hz) / fps + 1e-9));
}

// ---- lifecycle -------------------------------------------------------------------------------------------------------

void MoviePlayer::Construct(uint32_t player) {
    // The player's state is the game's 0x41D0 bytes: the PS2's constructor zeroed and set them up; nothing of the
    // host lives there, so they are zeroed and left alone (a savestate carries them like any guest bytes).
    if (uint8_t* bytes = te::GuestPointer(player, kPlayerSize)) std::memset(bytes, 0, kPlayerSize);
    auto it = players_.find(player);
    if (it != players_.end()) DestroyDecoder(it->second);
    players_[player] = Player{};
    players_[player].constructed = true;
    ++count_.constructs;
}

void MoviePlayer::BeginPresenting(uint32_t player, uint32_t present) {
    Player* p = Find(player);
    if (!p) return;
    p->presenting = true;
    p->present = present;
    p->queue.clear();
}

bool MoviePlayer::Open(uint32_t player, const char* file, uint32_t audioChannel, int32_t width, uint32_t lentMemory) {
    Player* p = Find(player);
    if (!p) return false;
    if (p->open) Close(player);
    p->file = file ? file : "";
    p->audio_channel = audioChannel;
    p->width = width;
    p->lent_memory = lentMemory;
    p->steps = p->waits = p->queued = p->draws = p->uploads = 0;
    p->trace_decodes = p->trace_uploads = 0;
    p->trace_decode_ms = p->trace_decode_max_ms = p->trace_upload_ms = p->trace_upload_max_ms = 0;
    p->pcm_popped = 0;
    p->frames = 0;
    p->fields = p->presented = 0;
    p->fps = 0.0;
    p->sound_started = false;
    p->volume = 0.f;
    p->decoded.reset();
    p->queue.clear();
    p->texture_filled = false;
    p->ring_failed = false;
    p->luma_first[0] = p->luma_first[1] = p->luma_first[2] = -1;
    p->luma_min = 256;
    p->luma_max = -1;
    p->luma_n = 0;
    ++count_.opens;

    if (skip_) {
        p->open = true;
        p->input_done = true;
        std::fprintf(stderr, "[twinsanityevolved] movie: %s skipped (skip movies)\n", p->file.c_str());
        return true;
    }

    // The file on the disc (the game's ISO name: "\FMV\VIVENDI.PSS;1"; IsoFs takes either separator, any case)
    te::Disc& disc = te::Services::Get().disc();
    disc.Initialise();   // opened on first use (a seam body reached before System::Initialise constructs what it needs)
    IsoFs::Entry entry;
    if (!disc.IsOpen() || p->file.empty() || !disc.Iso().Find(p->file, entry) || entry.directory || entry.size == 0) {
        ++count_.open_failed;
        std::fprintf(stderr, "[twinsanityevolved] movie: %s not on the disc: not played\n", p->file.c_str());
        return false;
    }
    p->image_offset = uint64_t{entry.lba} * IsoFs::kSectorSize;
    p->file_size = entry.size;
    p->read_cursor = 0;
    p->input_done = false;

    if (MovieThreadWanted() && MovieThread::Get().Begin(&disc.Iso(), p->image_offset, p->file_size, static_cast<int>(audioChannel))) {
        p->decoder.reset(new ThreadDecoder);
    } else {
        ps2e_movie_config cfg{};
        cfg.audio_channel = static_cast<int>(audioChannel);
        cfg.max_pictures = 3;
        cfg.planar = Mode() == PictureMode::kYuv ? 1 : 0;
        ps2e_movie* movie = ps2e_movie_create(&cfg);
        if (!movie) {
            ++count_.open_failed;
            return false;
        }
        p->decoder.reset(new SyncDecoder(movie, &disc.Iso(), p->image_offset, p->file_size));
    }
    p->open = true;
    playing_ = p;
    EnsureMixerSource();

    // The first two pictures (one queued for the screen, one decoded and waiting), then the sound buffer filled. The
    // Vita's movie path makes the movie's ring of picture slots from the first one: the pictures decoded after it go
    // straight into the GPU's storage.
    if (DecodeNext(*p)) {
        p->queue.push_back(std::move(p->decoded));
        ++p->queued;
        ++count_.pictures_queued;
        EnsureRing(*p, *p->queue.back());
        DecodeNext(*p);
    }
    FeedSound(*p);
    if (p->fps <= 0.0) p->fps = p->decoder->Fps();
    const Picture* first = p->queue.empty() ? nullptr : p->queue.front().get();
    std::fprintf(stderr, "[twinsanityevolved] movie: %s opened: %u bytes, %dx%d @ %.3f fps, sound track %u (%s), %u PCM frames buffered%s\n",
                 p->file.c_str(), p->file_size, first ? first->width : 0, first ? first->height : 0, p->fps, audioChannel,
                 p->decoder->HasAudio() ? "PCM 48 kHz" : (p->decoder->AudioDisabled() ? "unsupported format" : "none yet"),
                 p->decoder->PcmQueued(), p->decoder->Threaded() ? ", decoded on the movie thread" : "");
    return true;
}

void MoviePlayer::StartSound(uint32_t player, float volume) {
    Player* p = Find(player);
    if (!p || !p->open) return;
    p->volume = std::clamp(volume, 0.f, 1.f);
    p->sound_started = true;
}

bool MoviePlayer::Step(uint32_t player) {
    Player* p = Find(player);
    if (!p || !p->open) return false;
    ++p->steps;
    if (skip_ || !p->decoder) {
        ++count_.skipped;
        return false;   // the movie has ended: the controller stops it this frame
    }
    if (Ended(*p)) return false;
    if (p->decoded && p->queue.size() < kPictureQueue) {
        p->queue.push_back(std::move(p->decoded));
        ++p->queued;
        ++count_.pictures_queued;
        DecodeNext(*p);
        p->frames++;
    }
    FeedSound(*p);
    return true;
}

void MoviePlayer::WaitFrame(uint32_t player) {
    Player* p = Find(player);
    if (!p || !p->open) return;
    ++p->waits;
    if (skip_ || !p->decoder) return;   // nothing is ever presented
    // The first blank (the PS2 handler's toggle), then more until the next picture is due by the movie's rate; the
    // present callback's own frame boundary is the blank the picture is shown at. Each Boundary() is a save-state
    // checkpoint: the fiber may be saved inside it and resumed in another process whose map nodes are elsewhere,
    // so `p` is looked up again after every boundary (no host heap pointer lives across the checkpoint).
    const uint64_t due = DueField(p->presented, p->fps, te::GetSettings().field_hz);
    p = Boundary(player);
    for (int extra = 0; p && p->fields + 1 < due && extra < 8; ++extra) p = Boundary(player);
    if (!p || !p->open) return;
    if (p->presenting && p->present) {
        if (Trace()) std::fprintf(stderr, "[movie] present %llu at field %llu (due %llu), queue %lu\n", (unsigned long long)p->presented,
                                  (unsigned long long)p->fields, (unsigned long long)due, (unsigned long)p->queue.size());
        ++p->presented;
        ++count_.presents;
        p->fields++;   // the callback's PresentFromInterrupt is one boundary (graphics.cpp)
        count_.fields++;
        te_call_void_callback(p->present);
        p = Find(player);   // the callback ran boundaries (checkpoints): look the player up again
        if (p) p->quad_pending = false;   // the frame with Draw's quad is on the screen
    }
}

void MoviePlayer::QueuePicture(uint32_t player) {
    Player* p = Find(player);
    if (!p || !p->open || p->queue.empty()) return;
    std::unique_ptr<Picture> picture = std::move(p->queue.front());
    p->queue.pop_front();
    if (!picture) return;
    const bool timed = Trace();
    const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // the census's luma samples of the pictures sent to the screen
    if (picture->luma >= 0) {
        if (p->luma_n < 3) p->luma_first[p->luma_n] = picture->luma;
        p->luma_min = std::min(p->luma_min, picture->luma);
        p->luma_max = std::max(p->luma_max, picture->luma);
        ++p->luma_n;
    }
    ShowPicture(*p, std::move(picture), false);
    if (timed) {
        const double ms = MsSince(t0);
        ++p->trace_uploads;
        p->trace_upload_ms += ms;
        p->trace_upload_max_ms = std::max(p->trace_upload_max_ms, ms);
    }
}

void MoviePlayer::Draw(uint32_t player, uint32_t area, int32_t width, int32_t height, uint32_t skippedRows) {
    Player* p = Find(player);
    if (!p || !p->open) return;
    ++p->draws;
    const auto* a = reinterpret_cast<const int32_t*>(te::GuestPointer(area, 16));   // Platform::Movie::Area {x, y, width, height}
    if (Trace() && p->draws <= 3)
        std::fprintf(stderr, "[movie] draw %u: area {%d %d %d %d} %dx%d skipped %u, frames %u texture %u%s\n", p->draws, a ? a[0] : -1,
                     a ? a[1] : -1, a ? a[2] : -1, a ? a[3] : -1, width, height, skippedRows, p->frames, p->texture,
                     p->texture_filled ? " filled" : "");
    if (skip_ || !p->decoder || p->frames < 2 || !p->texture || !p->texture_filled) return;
    if (!a || a[2] <= 0 || a[3] <= 0 || height <= 0) return;
    (void)width;
    const float x0 = float(a[0]), y0 = float(a[1]), x1 = float(a[0] + a[2]), y1 = float(a[1] + a[3]);
    const float v0 = float(skippedRows) / float(height), v1 = float(height - int32_t(skippedRows)) / float(height);
    te_graphics_record_movie_quad(p->texture, x0, y0, x1, y1, 0.f, v0, 1.f, v1);
    p->quad_pending = true;
    p->quad[0] = x0; p->quad[1] = y0; p->quad[2] = x1; p->quad[3] = y1; p->quad[4] = v0; p->quad[5] = v1;
    ++count_.draws;
}

void MoviePlayer::Close(uint32_t player) {
    Player* p = Find(player);
    if (!p) return;
    if (p->open && Trace() && (p->trace_decodes || p->trace_uploads)) {
        uint64_t waits = 0;
        double wait_ms = 0, wait_max_ms = 0;
        if (p->decoder && p->decoder->Threaded()) MovieThread::Get().Waits(waits, wait_ms, wait_max_ms);
        std::fprintf(stderr, "[movie] %s closed: %u decodes (mean %.2f ms, max %.2f ms: disc read + demux + decode + RGBA%s), "
                     "%u uploads (mean %.2f ms, max %.2f ms), %llu presents in %llu fields; waits for the movie thread %llu (%.1f ms, max %.1f)\n",
                     p->file.c_str(), p->trace_decodes, p->trace_decodes ? p->trace_decode_ms / p->trace_decodes : 0.0,
                     p->trace_decode_max_ms, p->decoder && p->decoder->Threaded() ? " -- on the movie thread: the game thread's wait" : "",
                     p->trace_uploads, p->trace_uploads ? p->trace_upload_ms / p->trace_uploads : 0.0, p->trace_upload_max_ms,
                     (unsigned long long)p->presented, (unsigned long long)p->fields, (unsigned long long)waits, wait_ms, wait_max_ms);
    }
    if (p->open) ++count_.closes;
    p->open = false;
    p->presenting = false;
    p->sound_started = false;
    p->decoded.reset();
    p->queue.clear();
    p->quad_pending = false;
    DestroyDecoder(*p);
    if (playing_ == p) playing_ = nullptr;
}

// ---- the decoder ------------------------------------------------------------------------------------------------------

bool MoviePlayer::Pump(Player& p) {
    if (!p.decoder || p.input_done) return false;
    uint32_t got = 0;
    const bool pumped = p.decoder->Pump(&got);
    p.read_cursor = p.decoder->ReadCursor();
    p.input_done = p.decoder->InputDone();
    count_.bytes_read += got;
    return pumped;
}

bool MoviePlayer::DecodeNext(Player& p) {
    p.decoded.reset();
    if (!p.decoder) return false;
    const bool timed = Trace();
    const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    struct Timer {
        bool on; Player& p; std::chrono::steady_clock::time_point t0;
        ~Timer() { if (on) { const double ms = MsSince(t0); ++p.trace_decodes; p.trace_decode_ms += ms; p.trace_decode_max_ms = std::max(p.trace_decode_max_ms, ms); } }
    } timer{timed, p, t0};
    while (p.decoder->PicturesReady() == 0) {
        if (p.decoder->Ended() || !Pump(p)) {
            if (p.decoder->PicturesReady() == 0) return false;
            break;
        }
    }
    auto out = std::make_unique<Picture>();
    if (!p.decoder->TakePicture(*out)) return false;
    p.decoded = std::move(out);
    ++count_.pictures_decoded;
    if (p.fps <= 0.0) p.fps = p.decoder->Fps();
    return true;
}

// The PS2 kept its 0x6000-byte ring full ahead of the SPU2; here the decoder's PCM queue is kept at that depth (the
// decoder decodes pictures too as it demuxes, up to its max_pictures; past that the video waits inside it).
void MoviePlayer::FeedSound(Player& p) {
    if (!p.decoder) return;
    for (int pumps = 0; p.decoder->PcmQueued() < kSoundBufferFrames && pumps < 16; ++pumps) {
        if (p.decoder->AudioDisabled()) return;
        if (!Pump(p)) return;
    }
}

bool MoviePlayer::Ended(const Player& p) const {
    return p.decoder && p.decoder->Ended() && !p.decoded;
}

MoviePlayer::Player* MoviePlayer::Boundary(uint32_t player) {
    te::Services::Get().clock().Boundary();   // the checkpoint: the player map may be another process's afterwards
    Player* p = Find(player);
    if (p) p->fields++;
    count_.fields++;
    count_.wait_boundaries++;
    return p;
}

void MoviePlayer::UploadPicture(Player& p, const Picture& picture) {
    ps2e_draw* dev = te_graphics_device();
    if (!dev || picture.width <= 0 || picture.height <= 0) return;
    // The device's calls run on the graphics side's render thread, synchronously
    struct Job { ps2e_draw* dev; Player* p; const Picture* picture; int uploaded; } job{dev, &p, &picture, 0};
    te_graphics_on_render([](void* user) {
        Job& j = *static_cast<Job*>(user);
        Player& p = *j.p;
        const Picture& picture = *j.picture;
        if (p.texture && (p.texture_w != picture.width || p.texture_h != picture.height)) {
            ps2e_draw_texture_destroy(j.dev, p.texture);
            p.texture = 0;
        }
        if (!p.texture) {
            p.texture = ps2e_draw_texture_create(j.dev, picture.width, picture.height, nullptr);
            p.texture_w = picture.width;
            p.texture_h = picture.height;
            if (!p.texture) return;
        }
        // The decoder's alpha is 0xFF: ps2e_draw's upload scales PS2 alpha bytes (0x80 = 1.0) and saturates, so the
        // picture is opaque either way
        j.uploaded = ps2e_draw_texture_upload(j.dev, p.texture, 0, 0, picture.width, picture.height, picture.rgba.data(), 0);
    }, &job);
    if (job.uploaded) {
        p.texture_filled = true;
        ++p.uploads;
        ++count_.pictures_uploaded;
    }
}

#if defined(__vita__)
// The Vita: a picture's upload (ps2e_draw's alpha pass and vitaGL's copy of up to 1.2 MB -- ~7 ms of the game thread's
// 40 ms a picture in Vita3K, while the render thread idles through a movie) is posted to the render thread and not
// waited for. The render thread runs posted work before the next frame it is handed (RenderLoop), so the frame with this
// picture's quad samples it, and a later destroy (posted after it) finds it done. A new texture (the first picture, a new
// size) is made synchronously as on the other hosts: its id is the game thread's before Draw records the quad.
void MoviePlayer::UploadPictureAsync(Player& p, std::unique_ptr<Picture> picture) {
    ps2e_draw* dev = te_graphics_device();
    if (!dev || picture->width <= 0 || picture->height <= 0) return;
    if (!p.texture || p.texture_w != picture->width || p.texture_h != picture->height) {
        UploadPicture(p, *picture);
        return;
    }
    struct Job { ps2e_draw* dev; uint32_t texture; std::unique_ptr<Picture> picture; };
    te_graphics_post_render([](void* user) {
        std::unique_ptr<Job> j(static_cast<Job*>(user));
        ps2e_draw_texture_upload(j->dev, j->texture, 0, 0, j->picture->width, j->picture->height, j->picture->rgba.data(), 0);
    }, new Job{dev, p.texture, std::move(picture)});
    p.texture_filled = true;
    ++p.uploads;
    ++count_.pictures_uploaded;
}
#endif

void MoviePlayer::ShowPicture(Player& p, std::unique_ptr<Picture> picture, bool wait) {
    if (!picture) return;
    if (ShowInRing(p, picture, wait)) return;
    // the RGBA upload (TE_MOVIE_PICTURES=legacy, every other host, or a movie whose ring could not be made): a picture that
    // has only its planes is converted first
    if (picture->rgba.empty() && !picture->planes.empty())
        PlanesToRgba(picture->width, picture->height, picture->planes.data(), picture->rgba);
    if (picture->rgba.empty()) return;   // (a picture held in a slot of a ring that is gone: nothing to upload)
#if defined(__vita__)
    if (!wait) {
        UploadPictureAsync(p, std::move(picture));
        return;
    }
#endif
    (void)wait;
    UploadPicture(p, *picture);
}

// The movie's ring of picture slots, made on the drawing thread (synchronously: its texture id is the game thread's before
// Draw records a quad) from the first picture's size and kind, and handed to the movie thread
bool MoviePlayer::EnsureRing(Player& p, const Picture& first) {
    if (Mode() == PictureMode::kLegacy || p.ring_failed) return false;
    if (p.ring) return true;
    ps2e_draw* dev = te_graphics_device();
    if (!dev || first.width <= 0 || first.height <= 0) return false;
    auto ring = std::make_shared<PictureRing>(first.width, first.height, first.planar != 0);
    struct Job { ps2e_draw* dev; PictureRing* ring; } job{dev, ring.get()};
    te_graphics_on_render([](void* user) { Job& j = *static_cast<Job*>(user); j.ring->Create(j.dev); }, &job);
    if (!ring->id) {
        p.ring_failed = true;
        std::fprintf(stderr, "[twinsanityevolved] movie: %s: no picture ring in the GPU's storage (the device has no movie "
                     "textures): RGBA uploads\n", p.file.c_str());
        return false;
    }
    if (p.texture) {   // (an RGBA texture of an earlier picture of this playback: gone, the ring's draws take over)
        struct Old { ps2e_draw* dev; uint32_t texture; } old{dev, p.texture};
        te_graphics_on_render([](void* user) { Old& o = *static_cast<Old*>(user); ps2e_draw_texture_destroy(o.dev, o.texture); }, &old);
    }
    p.ring = ring;
    p.texture = ring->id;
    p.texture_w = first.width;
    p.texture_h = first.height;
    p.texture_filled = false;
#if defined(__vita__)
    {
        // the census of the ring's memory: cached / uncached planes by kind, and whether cached ones are
        // written back after each picture (TE_MOVIE_CACHE_FLUSH)
        const int planes_per_slot = ring->yuv ? 3 : 1;
        const int total = ring->slots * planes_per_slot;
        const int cached = ring->kinds[kPlaneCached];
        const size_t slot_bytes = ring->yuv ? size_t(ring->w) * size_t(ring->h) + 2 * size_t((ring->w + 1) / 2) * size_t((ring->h + 1) / 2)
                                            : size_t(ring->w) * size_t(ring->h) * 4;
        std::string kinds;
        for (int k = 0; k < kPlaneKinds; ++k) {
            if (!ring->kinds[k]) continue;
            if (!kinds.empty()) kinds += ", ";
            kinds += std::string(kPlaneKindNames[k]) + " " + std::to_string(ring->kinds[k]);
        }
        std::fprintf(stderr, "[twinsanityevolved] movie ring: %s %d slots x %d planes of %dx%d %s (%u KB): %d cached / %d uncached "
                     "planes (%s); cache write-back after each slot write %s (TE_MOVIE_CACHE_FLUSH) -- %s\n",
                     p.file.c_str(), ring->slots, planes_per_slot, ring->w, ring->h, ring->yuv ? "YUV" : "RGBA",
                     unsigned(slot_bytes * size_t(ring->slots) / 1024), cached, total - cached, kinds.c_str(),
                     CacheFlushOn() ? "on" : "off",
                     cached ? "the cached planes' bytes are written back after each picture"
                            : "0 bytes a picture: every plane is in uncached memory");
    }
#endif
    if (p.decoder && p.decoder->Threaded()) MovieThread::Get().SetRing(ring);
    return true;
}

// The picture to the screen through the ring: a picture the movie thread wrote into a slot becomes the slot shown (the
// drawing thread's whole work for it); one kept in bytes is copied into a free slot on the drawing thread first. Posted
// (run before the next frame the drawing thread takes, as the RGBA uploads were), or run and waited for (`wait`).
bool MoviePlayer::ShowInRing(Player& p, std::unique_ptr<Picture>& picture, bool wait) {
    if (Mode() == PictureMode::kLegacy || p.ring_failed) return false;
    if (!p.ring && !EnsureRing(p, *picture)) return false;
    ps2e_draw* dev = te_graphics_device();
    if (!dev) return false;
    std::shared_ptr<PictureRing> ring = p.ring;
    struct Job {
        ps2e_draw* dev;
        std::shared_ptr<PictureRing> ring;
        int slot;                           // the picture's slot, or -1: copy `bytes` into one
        std::unique_ptr<Picture> bytes;
    };
    Job* job = new Job{dev, ring, -1, nullptr};
    if (picture->lease && picture->lease->ring == ring && picture->lease->slot >= 0) {
        job->slot = picture->lease->slot;
        picture->lease->slot = -1;   // the slot is the show's now (shown, then retired by the ring)
        picture.reset();
    } else if (ring->Fits(picture->width, picture->height, picture->planar,
                          picture->rgba.empty() ? nullptr : picture->rgba.data(),
                          picture->planes.empty() ? nullptr : picture->planes.data())) {
        job->bytes = std::move(picture);
    } else {
        // a picture of another size or kind than the movie's first (not on this disc), or held by an older ring: dropped
        {
            std::lock_guard<sync::Mutex> lock(ring->mutex);
            ++ring->dropped;
        }
        picture.reset();
        delete job;
        return true;
    }
    const auto run = [](void* user) {
        std::unique_ptr<Job> j(static_cast<Job*>(user));
        int slot = j->slot;
        if (slot < 0) {
            // a picture kept in bytes: into a free slot (the GPU frees one as it finishes its scenes: a short wait at most)
            slot = j->ring->Reserve();
            for (int i = 0; i < 8 && slot < 0; ++i) {
                SleepMs(1);
                slot = j->ring->Reserve();
            }
            if (slot < 0) {
                std::lock_guard<sync::Mutex> lock(j->ring->mutex);
                ++j->ring->dropped;
                return;
            }
            j->ring->Write(slot, j->bytes->rgba.empty() ? nullptr : j->bytes->rgba.data(),
                           j->bytes->planes.empty() ? nullptr : j->bytes->planes.data());
            {
                std::lock_guard<sync::Mutex> lock(j->ring->mutex);
                ++j->ring->copied;
            }
        }
        ps2e_draw_movie_show(j->dev, j->ring->id, slot);
        j->ring->Shown(slot);
    };
    if (wait) te_graphics_on_render(run, job);
    else te_graphics_post_render(run, job);
    p.texture = ring->id;
    p.texture_filled = true;
    ++p.uploads;
    ++count_.pictures_uploaded;
    return true;
}

// The movie's ring let go (its storage destroyed on the drawing thread after every show posted before; the movie thread
// stopped writing it already), with the movie's picture census line
void MoviePlayer::DestroyRing(Player& p) {
    if (!p.ring) return;
    std::shared_ptr<PictureRing> ring = std::move(p.ring);
    p.ring.reset();   // (the movie thread dropped its own at the decoder's cancel)
    uint64_t drawn = 0, refused = 0;
    if (ps2e_draw* dev = te_graphics_device()) {
        struct Job { ps2e_draw* dev; uint32_t id; uint64_t* drawn; uint64_t* refused; } job{dev, ring->id, &drawn, &refused};
        te_graphics_on_render([](void* user) {
            Job& j = *static_cast<Job*>(user);
            ps2e_draw_movie_counts(j.dev, j.drawn, j.refused);
            ps2e_draw_texture_destroy(j.dev, j.id);
        }, &job);
    }
    if (p.texture == ring->id) {
        p.texture = 0;
        p.texture_filled = false;
    }
    std::lock_guard<sync::Mutex> lock(ring->mutex);
    const size_t slot_bytes = ring->yuv ? size_t(ring->w) * size_t(ring->h) + 2 * size_t((ring->w + 1) / 2) * size_t((ring->h + 1) / 2)
                                        : size_t(ring->w) * size_t(ring->h) * 4;
    std::fprintf(stderr, "[twinsanityevolved] movie: %s pictures (TE_MOVIE_PICTURES=%s): %llu shown from the GPU's storage -- %llu "
                 "written there by the movie thread, %llu copied in by the drawing thread, %llu dropped; ring %d slots of %dx%d %s "
                 "(%u KB each), %llu decoded into bytes (no slot free), %llu waits for a slot (%.1f ms), no slot free %llu times "
                 "(last: %d held, %d waiting for the GPU, %s); the movie's draws %llu (nothing shown yet %llu); luma of the "
                 "pictures sent %d %d %d .. min %d max %d over %u\n",
                 p.file.c_str(), ModeName(Mode()), (unsigned long long)ring->shows, (unsigned long long)ring->direct,
                 (unsigned long long)ring->copied, (unsigned long long)ring->dropped, ring->slots, ring->w, ring->h,
                 ring->yuv ? "YUV 4:2:0" : "RGBA", unsigned(slot_bytes / 1024), (unsigned long long)ring->fallbacks,
                 (unsigned long long)ring->slot_waits, ring->slot_wait_ms, (unsigned long long)ring->full, ring->full_held,
                 ring->full_retired, ring->full_lag == 0xFFFFFFFFu ? "no fence" :
                 (std::string("GPU ") + std::to_string(ring->full_lag) + " scenes behind").c_str(),
                 (unsigned long long)(drawn - ring->drawn_at_create), (unsigned long long)(refused - ring->refused_at_create),
                 p.luma_first[0], p.luma_first[1], p.luma_first[2], p.luma_n ? p.luma_min : -1, p.luma_max, p.luma_n);
#if defined(__vita__)
    {
        const uint64_t written = ring->Written(), ok = ring->FlushOk(), failed = ring->FlushFailed();
        std::fprintf(stderr, "[twinsanityevolved] movie ring: %s closed: %llu pictures written into %d slots (%d cached planes); "
                     "cache write-backs %llu (%llu KB, %.1f KB a picture), refused %llu (first code 0x%08X); TE_MOVIE_CACHE_FLUSH %s\n",
                     p.file.c_str(), (unsigned long long)written, ring->slots, ring->kinds[kPlaneCached], (unsigned long long)ok,
                     (unsigned long long)(ring->FlushBytes() / 1024), written ? double(ring->FlushBytes()) / 1024.0 / double(written) : 0.0,
                     (unsigned long long)failed, unsigned(ring->FlushError()), CacheFlushOn() ? "on" : "off");
    }
#endif
}

void MoviePlayer::DestroyDecoder(Player& p) {
    p.decoder.reset();   // (the movie thread's: it lets the movie go)
    DestroyRing(p);      // (the Vita's movie path: the ring's storage, after the movie thread stopped writing it)
    if (p.texture) {
        if (ps2e_draw* dev = te_graphics_device()) {
            struct Job { ps2e_draw* dev; uint32_t texture; } job{dev, p.texture};
            te_graphics_on_render([](void* user) { Job& j = *static_cast<Job*>(user); ps2e_draw_texture_destroy(j.dev, j.texture); }, &job);
        }
        p.texture = 0;
        p.texture_filled = false;
    }
}

// ---- the sound track (the audio mixer's movie source) -------------------------------------------------------------------

void MoviePlayer::EnsureMixerSource() {
    if (mixer_hooked_) return;
    mixer_hooked_ = true;
    te::audio::Service().SetMovieSource(this);
}

std::array<float, 2> MoviePlayer::Gains() const {
    if (!playing_ || !playing_->open || !playing_->sound_started) return {0.f, 0.f};
    return {playing_->volume, playing_->volume};
}

int MoviePlayer::Pop(int16_t* stereo, int frames) {
    if (!playing_ || !playing_->open || !playing_->sound_started || !playing_->decoder || frames <= 0) return 0;
    const uint32_t got = playing_->decoder->PcmRead(stereo, static_cast<uint32_t>(frames));
    playing_->pcm_popped += got;
    count_.pcm_frames += got;
    if (got < static_cast<uint32_t>(frames) && !playing_->decoder->Ended()) count_.pcm_silence_frames += uint32_t(frames) - got;
    return static_cast<int>(got);
}

// ---- the save-state section --------------------------------------------------------------------------------------------
// The game's state machine (GameController::StateMovie) holds a Platform::Movie player across frame boundaries: a state
// taken while a movie plays (or between Construct and Open) must restore a player the ps2e_movie_* calls find, or every
// later Step/WaitFrame/Close returns through Find()'s nullptr and the movie neither ends nor presents (Step false ->
// the controller leaves StateMovie at once: 3 frames early on the hub pad log, every frame after it shifted).
void MoviePlayer::Serialize(std::vector<uint8_t>& out) const {
    savestate::Put(out, uint32_t(2));   // version 2: + the pending quad (quad_pending, 6 floats) per player
    savestate::Put(out, uint8_t(skip_ ? 1 : 0));
    savestate::Put(out, uint32_t(players_.size()));
    for (const auto& [address, p] : players_) {
        savestate::Put(out, address);
        savestate::Put(out, uint8_t(p.constructed)); savestate::Put(out, uint8_t(p.presenting));
        savestate::Put(out, uint8_t(p.open)); savestate::Put(out, uint8_t(p.sound_started));
        savestate::Put(out, p.present); savestate::Put(out, p.audio_channel); savestate::Put(out, p.lent_memory);
        savestate::Put(out, p.width);
        savestate::PutString(out, p.file);
        savestate::Put(out, p.fields); savestate::Put(out, p.presented); savestate::Put(out, p.frames);
        savestate::Put(out, p.uploads); savestate::Put(out, p.pcm_popped);
        savestate::Put(out, p.volume);
        savestate::Put(out, uint8_t(p.quad_pending ? 1 : 0));
        for (float f : p.quad) savestate::Put(out, f);
    }
}

bool MoviePlayer::Restore(const std::vector<uint8_t>& payload, std::string* err) {
    savestate::PayloadReader in(payload);
    uint32_t version = 0, count = 0;
    uint8_t skip = 0;
    auto fail = [&](const char* why) { if (err) *err = std::string("te::movie::MoviePlayer: ") + why; return false; };
    if (!in.Get(&version) || (version != 1 && version != 2)) return fail("payload version");
    if (!in.Get(&skip) || !in.Get(&count) || count > 64) return fail("payload header");
    Reset();
    skip_ = skip != 0;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t address = 0;
        uint8_t constructed = 0, presenting = 0, open = 0, sound_started = 0;
        Player p;
        if (!in.Get(&address) || !in.Get(&constructed) || !in.Get(&presenting) || !in.Get(&open) || !in.Get(&sound_started) ||
            !in.Get(&p.present) || !in.Get(&p.audio_channel) || !in.Get(&p.lent_memory) || !in.Get(&p.width) ||
            !savestate::GetString(in, p.file, 0x100) || !in.Get(&p.fields) || !in.Get(&p.presented) || !in.Get(&p.frames) ||
            !in.Get(&p.uploads) || !in.Get(&p.pcm_popped) || !in.Get(&p.volume))
            return fail("payload truncated");
        uint8_t quad_pending = 0;
        float quad[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
        if (version >= 2) {
            if (!in.Get(&quad_pending)) return fail("payload truncated (quad)");
            for (float& f : quad) if (!in.Get(&f)) return fail("payload truncated (quad)");
        }
        p.constructed = constructed != 0;
        p.presenting = presenting != 0;
        const uint64_t fields = p.fields, presented = p.presented;
        const uint32_t frames = p.frames, uploads = p.uploads, pcm_popped = p.pcm_popped, present = p.present,
                       audio_channel = p.audio_channel, lent_memory = p.lent_memory;
        const int32_t width = p.width;
        const float volume = p.volume;
        const std::string file = p.file;
        Player& q = players_[address];
        q = std::move(p);
        if (open) {
            // Re-open from the disc and bring the decoder to where it was: the saving run had decoded 2 + frames
            // pictures (Open's two, one per Step that queued), uploaded `uploads` of them and popped `pcm_popped` PCM
            // frames; the same sequence is decoded again (deterministic) and the consumed part discarded.
            if (!Open(address, file.c_str(), audio_channel, width, lent_memory)) {
                if (!skip_) std::fprintf(stderr, "[twinsanityevolved] movie: %s could not be re-opened on restore\n", file.c_str());
                q.open = true;   // the game believes it is open: Step ends it, Close closes it
                q.input_done = true;
            } else if (q.decoder) {
                std::deque<std::unique_ptr<Picture>> all;   // the decode sequence so far
                for (auto& pic : q.queue) all.push_back(std::move(pic));
                q.queue.clear();
                if (q.decoded) all.push_back(std::move(q.decoded));
                while (all.size() < size_t(2) + frames && DecodeNext(q)) all.push_back(std::move(q.decoded));
                q.decoded.reset();
                std::unique_ptr<Picture> last_uploaded;
                for (uint32_t u = 0; u < uploads && !all.empty(); ++u) { last_uploaded = std::move(all.front()); all.pop_front(); }
                if (last_uploaded) ShowPicture(q, std::move(last_uploaded), true);
                if (!all.empty()) { q.decoded = std::move(all.back()); all.pop_back(); }
                for (auto& pic : all) q.queue.push_back(std::move(pic));
                std::vector<int16_t> sink(2 * 4096);
                for (uint32_t left = pcm_popped; left > 0;) {
                    FeedSound(q);
                    const uint32_t got = q.decoder->PcmRead(sink.data(), std::min<uint32_t>(left, 4096));
                    if (!got) break;
                    left -= got;
                }
                q.uploads = uploads;
                q.pcm_popped = pcm_popped;
                // The quad the saving run had drawn for the present its WaitFrame was waiting on (see the header): into
                // this process's frame, as Draw recorded it
                if (quad_pending && q.texture && q.texture_filled) {
                    te_graphics_record_movie_quad(q.texture, quad[0], quad[1], quad[2], quad[3], 0.f, quad[4], 1.f, quad[5]);
                    q.quad_pending = true;
                    std::copy(std::begin(quad), std::end(quad), std::begin(q.quad));
                }
                std::fprintf(stderr, "[twinsanityevolved] movie: %s re-opened on restore: %u pictures decoded (%u queued, %s waiting), "
                             "%u uploaded, %u PCM frames discarded (%u queued now), %u of %u bytes read%s\n",
                             file.c_str(), 2 + frames, unsigned(q.queue.size()), q.decoded ? "one" : "none", uploads, pcm_popped,
                             q.decoder->PcmQueued(), q.read_cursor, q.file_size, q.input_done ? ", input done" : "");
            }
            q.fields = fields; q.presented = presented; q.frames = frames;
            q.sound_started = sound_started != 0;
            q.volume = volume;
            q.presenting = presenting != 0;
            q.present = present;
        }
    }
    return true;
}

MoviePlayer& Service() {
    static MoviePlayer player;
    static const bool registered = [] {
        savestate::Register(savestate::kTagMovie, "te::movie::MoviePlayer", false,
            [](std::vector<uint8_t>& out) { Service().Serialize(out); },
            [](const savestate::Blob& blob, std::string* error) { return Service().Restore(blob.payload, error); });
        return true;
    }();
    (void)registered;
    return player;
}

}  // namespace te::movie

extern "C" {

void ps2e_movie_construct(ps2e_u32 player) {
    te::movie::Service().SetSkipMovies(te::GetSettings().skip_movies);
    te::movie::Service().Construct(player);
}
void ps2e_movie_begin_presenting(ps2e_u32 player, ps2e_u32 present) { te::movie::Service().BeginPresenting(player, present); }
ps2e_s32 ps2e_movie_open(ps2e_u32 player, ps2e_u32 file, ps2e_s32 audioChannel, ps2e_s32 width, ps2e_u32 lentMemory) {
    return te::movie::Service().Open(player, te::GuestString(file, 0x100), static_cast<uint32_t>(audioChannel), width, lentMemory) ? 1 : 0;
}
void ps2e_movie_start_sound(ps2e_u32 player, ps2e_f32 volume) { te::movie::Service().StartSound(player, volume); }
ps2e_s32 ps2e_movie_step(ps2e_u32 player) { return te::movie::Service().Step(player) ? 1 : 0; }
void ps2e_movie_wait_frame(ps2e_u32 player) { te::movie::Service().WaitFrame(player); }
void ps2e_movie_queue_picture(ps2e_u32 player) { te::movie::Service().QueuePicture(player); }
void ps2e_movie_draw(ps2e_u32 player, ps2e_u32 area, ps2e_s32 width, ps2e_s32 height, ps2e_s32 skippedRows) {
    te::movie::Service().Draw(player, area, width, height, static_cast<uint32_t>(skippedRows));
}
void ps2e_movie_close(ps2e_u32 player) { te::movie::Service().Close(player); }

}  // extern "C"
