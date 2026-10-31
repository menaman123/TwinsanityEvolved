// render/backends/gl/gl_gpu_timers.h -- the GPU PHASE TIMER: where a frame's GPU time and fill go, by phase (a
// diagnosis, off by default; the producer switches it on: ps2e_draw_set_gpu_timers).
//
// The producer names the phase its next draws belong to (GlDevice::MarkGpuPhase: the frame's clear, the sky, the opaque
// scene, the blended scene, the shadow volumes' counting, the shadow composite with its caster marks, the screen copies,
// the HUD and text, the present). Per phase the timer keeps, summed over a census window:
//  - the GPU's time: GL_TIME_ELAPSED queries (ARB_timer_query, core since GL 3.3), read back frames later when the
//    driver says the result is available (never waited for: a frame whose results are not in after kSlots frames is
//    dropped from the census and counted);
//  - the fragments that passed the depth and stencil tests: GL_SAMPLES_PASSED queries, one per phase (the shaded fill);
//    where the producer knows the area a phase rasterises (a full-frame pass, a copy, the volumes' triangles) it notes it
//    (NoteFill) and that area is the phase's fill;
//  - the drawing thread's wall time between two phase marks (the GL calls' CPU time and what the driver blocks on).
// Where the time queries cut the frame. On an immediate-mode GPU (the desktop's discrete and integrated GPUs) a query
// boundary costs nothing: every phase is timed apart (mode kPhases). On a TILE-BASED GPU (Apple's: the renderer string
// starts with "Apple") a query boundary inside a render pass ends the pass -- the tiles stored and loaded again, ~0.36 ms
// a boundary for a 2048x2048 frame on an M4 -- so mode kBreaks cuts only where the frame's own render pass ends already
// (a framebuffer switch, a screen copy, the present): the phases drawn between two such ends are timed together and named
// together ("sky+scene"). kAuto = kBreaks on a tile-based GPU, kPhases elsewhere.
// The Vita's GL library has no timer queries: there everything here is a no-op.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2r {
namespace render {

class GlGpuTimers {
public:
    enum Phase : int { kClear = 0, kSky, kScene, kBlended, kVolumes, kComposite, kCopies, kHud, kPresent, kWait, kPhaseCount };
    enum Mode : int { kOff = 0, kAuto = 1, kPhases = 2, kBreaks = 3 };
    static const char* Name(int phase);

    // mode (kOff .. kBreaks); tile_based: the GPU ends a render pass at a query boundary (kAuto then cuts at the breaks)
    void SetMode(int mode, bool tile_based);
    bool On() const { return mode_ != kOff; }
    // the phase the next GL work belongs to (kWait: the drawing thread's own work between draws -- waiting for builds --
    // timed on the CPU only: the GPU's queries keep running for the phase before it). `work`: the device's count of GL
    // work issued so far (draws, clears, copies): a segment is named by the phases that issued work in it
    void Mark(int phase, uint64_t work);
    // the frame's render pass ends here (the draw framebuffer changed, a copy): a time boundary in kBreaks mode
    void Break(uint64_t work);
    // the fill a phase rasterises, known to the producer (pixels of the frame target)
    void NoteFill(int phase, double pixels) {
        if (mode_ != kOff) frame_fill_[phase] += pixels;
    }
    void NoteCount(int phase, uint32_t n = 1) {
        if (mode_ != kOff) frame_count_[phase] += n;
    }
    // the present's swap (CPU only: the wait for the display / the driver's queue)
    void NoteSwap(double ms) {
        if (mode_ != kOff) win_swap_ += ms;
    }
    // the present's scanout is drawn: the frame's queries closed, the finished frames' results read
    void EndFrame(uint64_t work);
    // The window's two lines ("" when no frame was read since the last call), and the window reset
    std::string Lines(double wall_ms_per_frame);
    bool Active() const { return mode_ == kPhases || mode_ == kBreaks; }
    const char* ModeName() const;

private:
    struct TimeRec {
        uint32_t query = 0;
        uint32_t mask = 0;   // the phases drawn in the segment
    };
    struct SampleRec {
        uint32_t query = 0;
        int phase = 0;
    };
    struct Frame {
        std::vector<TimeRec> times;
        std::vector<SampleRec> samples;
        std::vector<uint32_t> pool_time, pool_samples;   // the slot's query objects (reused)
        size_t used_time = 0, used_samples = 0;
        double fill[kPhaseCount] = {};
        uint32_t count[kPhaseCount] = {};
        bool closed = false, open = false;
    };
    static constexpr int kSlots = 8;
    uint32_t TakeQuery(std::vector<uint32_t>& pool, size_t& used);
    void BeginTime();
    void EndTime();
    void BeginSamples(int phase);
    void EndSamples();
    void OpenFrame();
    void Harvest();
    int mode_ = kOff;
    Frame frames_[kSlots];
    int cur_ = 0;                 // the slot being recorded
    int oldest_ = 0, pending_ = 0;   // the closed slots not read yet (oldest_ first)
    int gpu_phase_ = -1;          // the phase the GPU's queries run for (-1: none open)
    int cpu_phase_ = -1;          // the phase the CPU clock runs for
    uint32_t seg_mask_ = 0;       // the phases that issued work in the open time segment
    uint64_t phase_work_ = 0;     // the work count when gpu_phase_ was marked (or its segment began)
    void Settle(uint64_t work);   // gpu_phase_ joins the segment's name when it issued work since phase_work_
    bool time_open_ = false, samples_open_ = false;
    std::chrono::steady_clock::time_point cpu_t0_{};
    double frame_fill_[kPhaseCount] = {};
    uint32_t frame_count_[kPhaseCount] = {};
    double frame_cpu_ms_[kPhaseCount] = {};
    // the window's sums
    struct Seg {
        uint32_t mask = 0;
        double ms = 0;
    };
    std::vector<Seg> win_segs_;   // in the order the masks first appeared
    double win_samples_[kPhaseCount] = {}, win_fill_[kPhaseCount] = {}, win_cpu_[kPhaseCount] = {};
    uint64_t win_count_[kPhaseCount] = {};
    double win_swap_ = 0;
    uint64_t win_frames_ = 0, win_dropped_ = 0, win_cpu_frames_ = 0;
};

}  // namespace render
}  // namespace ps2r
