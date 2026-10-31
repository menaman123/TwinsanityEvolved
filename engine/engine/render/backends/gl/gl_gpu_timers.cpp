#include "engine/render/backends/gl/gl_gpu_timers.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ps2r {
namespace render {

const char* GlGpuTimers::Name(int phase) {
    static const char* const kNames[kPhaseCount] = {"clear", "sky", "scene", "blended", "volumes", "composite", "copies",
                                                    "hud", "present", "wait"};
    return phase >= 0 && phase < kPhaseCount ? kNames[phase] : "?";
}

const char* GlGpuTimers::ModeName() const {
    switch (mode_) {
    case kPhases: return "cut at every phase";
    case kBreaks: return "cut where the frame's render pass ends";
    default: return "off";
    }
}

#if PS2R_HOST_VITA
// vitaGL has no timer queries: the census is compiled out (SetMode leaves the timer off)
void GlGpuTimers::SetMode(int, bool) { mode_ = kOff; }
void GlGpuTimers::Mark(int, uint64_t) {}
void GlGpuTimers::Break(uint64_t) {}
void GlGpuTimers::EndFrame(uint64_t) {}
void GlGpuTimers::Settle(uint64_t) {}
std::string GlGpuTimers::Lines(double) { return std::string(); }
uint32_t GlGpuTimers::TakeQuery(std::vector<uint32_t>&, size_t&) { return 0; }
void GlGpuTimers::BeginTime() {}
void GlGpuTimers::EndTime() {}
void GlGpuTimers::BeginSamples(int) {}
void GlGpuTimers::EndSamples() {}
void GlGpuTimers::OpenFrame() {}
void GlGpuTimers::Harvest() {}
#else

void GlGpuTimers::SetMode(int mode, bool tile_based) {
    if (mode == kAuto) mode = tile_based ? kBreaks : kPhases;
    mode_ = std::clamp(mode, int(kOff), int(kBreaks));
}

uint32_t GlGpuTimers::TakeQuery(std::vector<uint32_t>& pool, size_t& used) {
    if (used == pool.size()) {
        uint32_t q = 0;
        glGenQueries(1, &q);
        pool.push_back(q);
    }
    return pool[used++];
}

void GlGpuTimers::OpenFrame() {
    Frame& f = frames_[cur_];
    if (f.closed) {
        // the ring is full: the oldest closed frame's results are still not in -- it leaves the census
        f.closed = false;
        win_dropped_++;
        oldest_ = (oldest_ + 1) % kSlots;
        pending_--;
    }
    f.times.clear();
    f.samples.clear();
    f.used_time = f.used_samples = 0;
    f.open = true;
    std::memset(frame_fill_, 0, sizeof frame_fill_);
    std::memset(frame_count_, 0, sizeof frame_count_);
    std::memset(frame_cpu_ms_, 0, sizeof frame_cpu_ms_);
}

void GlGpuTimers::BeginTime() {
    Frame& f = frames_[cur_];
    TimeRec r;
    r.query = TakeQuery(f.pool_time, f.used_time);
    seg_mask_ = 0;
    glBeginQuery(GL_TIME_ELAPSED, r.query);
    f.times.push_back(r);
    time_open_ = true;
}

void GlGpuTimers::EndTime() {
    if (!time_open_) return;
    glEndQuery(GL_TIME_ELAPSED);
    // a segment in which nothing was issued (two render-pass ends in a row) is named by the phase it ran under
    frames_[cur_].times.back().mask = seg_mask_ ? seg_mask_ : (gpu_phase_ >= 0 ? 1u << gpu_phase_ : 0u);
    time_open_ = false;
}

void GlGpuTimers::Settle(uint64_t work) {
    if (gpu_phase_ >= 0 && work != phase_work_) seg_mask_ |= 1u << gpu_phase_;
    phase_work_ = work;
}

void GlGpuTimers::BeginSamples(int phase) {
    Frame& f = frames_[cur_];
    SampleRec r;
    r.query = TakeQuery(f.pool_samples, f.used_samples);
    r.phase = phase;
    glBeginQuery(GL_SAMPLES_PASSED, r.query);
    f.samples.push_back(r);
    samples_open_ = true;
}

void GlGpuTimers::EndSamples() {
    if (!samples_open_) return;
    glEndQuery(GL_SAMPLES_PASSED);
    samples_open_ = false;
}

void GlGpuTimers::Mark(int phase, uint64_t work) {
    if (mode_ == kOff || phase < 0 || phase >= kPhaseCount) return;
    const auto now = std::chrono::steady_clock::now();
    if (cpu_phase_ >= 0) frame_cpu_ms_[cpu_phase_] += std::chrono::duration<double, std::milli>(now - cpu_t0_).count();
    cpu_phase_ = phase;
    cpu_t0_ = now;
    if (phase == kWait) return;   // (the GPU's queries keep running for the phase before)
    if (!frames_[cur_].open) OpenFrame();
    if (gpu_phase_ == phase && time_open_) return;
    Settle(work);
    // kPhases: every phase its own query; kBreaks: the segment goes on until the frame's render pass ends
    const bool cut = mode_ == kPhases || !time_open_;
    if (cut) EndTime();   // (named by the phase that ran in it: gpu_phase_ is still that one)
    gpu_phase_ = phase;
    EndSamples();
    BeginSamples(phase);
    if (cut) BeginTime();
}

void GlGpuTimers::Break(uint64_t work) {
    if (mode_ != kBreaks || !time_open_) return;
    Settle(work);
    EndTime();
    BeginTime();
}

void GlGpuTimers::EndFrame(uint64_t work) {
    if (mode_ == kOff) return;
    const auto now = std::chrono::steady_clock::now();
    if (cpu_phase_ >= 0) frame_cpu_ms_[cpu_phase_] += std::chrono::duration<double, std::milli>(now - cpu_t0_).count();
    cpu_phase_ = -1;
    for (int p = 0; p < kPhaseCount; ++p) {
        win_cpu_[p] += frame_cpu_ms_[p];
        win_count_[p] += frame_count_[p];
    }
    win_cpu_frames_++;
    Frame& f = frames_[cur_];
    if (f.open) {
        Settle(work);
        EndTime();
        EndSamples();
        std::memcpy(f.fill, frame_fill_, sizeof f.fill);
        f.open = false;
        f.closed = true;
        pending_++;
        cur_ = (cur_ + 1) % kSlots;
    }
    gpu_phase_ = -1;
    Harvest();
}

void GlGpuTimers::Harvest() {
    while (pending_ > 0) {
        Frame& f = frames_[oldest_];
        // the frame's last queries (they finish last): available = every query of the frame is
        GLint avail = 1;
        if (!f.times.empty()) glGetQueryObjectiv(f.times.back().query, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (avail && !f.samples.empty()) glGetQueryObjectiv(f.samples.back().query, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) return;
        for (const TimeRec& r : f.times) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(r.query, GL_QUERY_RESULT, &ns);
            auto it = std::find_if(win_segs_.begin(), win_segs_.end(), [&](const Seg& s) { return s.mask == r.mask; });
            if (it == win_segs_.end()) {
                win_segs_.push_back(Seg{r.mask, 0.0});
                it = win_segs_.end() - 1;
            }
            it->ms += double(ns) / 1e6;
        }
        double samples[kPhaseCount] = {};
        for (const SampleRec& r : f.samples) {
            GLuint64 n = 0;
            glGetQueryObjectui64v(r.query, GL_QUERY_RESULT, &n);
            samples[r.phase] += double(n);
        }
        for (int p = 0; p < kPhaseCount; ++p) {
            win_samples_[p] += samples[p];
            win_fill_[p] += f.fill[p];
        }
        win_frames_++;
        f.closed = false;
        oldest_ = (oldest_ + 1) % kSlots;
        pending_--;
    }
}

std::string GlGpuTimers::Lines(double wall_ms) {
    if (mode_ == kOff || (!win_frames_ && !win_cpu_frames_)) return std::string();
    std::string out;
    char buf[256];
    const double n = double(std::max<uint64_t>(1, win_frames_));
    const double nc = double(std::max<uint64_t>(1, win_cpu_frames_));
    // segments in the frame's order of phases (the lowest phase of each mask first)
    std::vector<Seg> segs = win_segs_;
    std::stable_sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) {
        return __builtin_ctz(a.mask | 0x80000000u) < __builtin_ctz(b.mask | 0x80000000u);
    });
    double total = 0;
    std::snprintf(buf, sizeof buf, "[te.gfx] gpu phases per frame (TE_GFX_GPU_TIMERS, %s):", ModeName());
    out += buf;
    bool first = true;
    for (const Seg& s : segs) {
        std::string name;
        for (int p = 0; p < kPhaseCount; ++p)
            if (s.mask & (1u << p)) name += (name.empty() ? "" : "+") + std::string(Name(p));
        if (name.empty()) name = "between";
        const double ms = s.ms / n;
        total += ms;
        std::snprintf(buf, sizeof buf, "%s %s %.2f", first ? "" : ",", name.c_str(), ms);
        out += buf;
        first = false;
        // the counts the brief's reader wants beside the volumes and the copies
        const bool vol = s.mask == (1u << kVolumes), cop = s.mask == (1u << kCopies);
        if (vol) {
            std::snprintf(buf, sizeof buf, " (%.1f draws)", double(win_count_[kVolumes]) / nc);
            out += buf;
        }
        if (cop) {
            std::snprintf(buf, sizeof buf, " (%.1f, %.2f Mpx)", double(win_count_[kCopies]) / nc, win_fill_[kCopies] / n / 1e6);
            out += buf;
        }
    }
    std::snprintf(buf, sizeof buf, " ms; total %.2f of %.2f ms wall (%llu frames read, %llu dropped)\n", total, wall_ms,
                  (unsigned long long)win_frames_, (unsigned long long)win_dropped_);
    out += buf;
    // the fill: the area a phase rasterises where the producer knows it, else the fragments that passed the tests
    out += "[te.gfx] gpu fill per frame (Mpx; passed = fragments through the depth / stencil tests):";
    first = true;
    for (int p = 0; p < kPhaseCount; ++p) {
        if (p == kWait) continue;
        const double drawn = win_fill_[p] / n / 1e6, passed = win_samples_[p] / n / 1e6;
        if (drawn <= 0 && passed <= 0) continue;
        if (drawn > 0)
            std::snprintf(buf, sizeof buf, "%s %s %.2f drawn %.2f passed", first ? "" : ",", Name(p), drawn, passed);
        else
            std::snprintf(buf, sizeof buf, "%s %s %.2f passed", first ? "" : ",", Name(p), passed);
        out += buf;
        if (win_count_[p] && (p == kVolumes || p == kComposite || p == kCopies)) {
            std::snprintf(buf, sizeof buf, " (%.1f %s)", double(win_count_[p]) / nc, p == kVolumes ? "draws" : "passes");
            out += buf;
        }
        first = false;
    }
    out += "\n";
    // the drawing thread's wall time per phase (the GL calls and what the driver blocks on; wait = for the builds)
    out += "[te.gfx] gl cpu per frame (the drawing thread, ms):";
    double cpu_total = 0;
    first = true;
    for (int p = 0; p < kPhaseCount; ++p) {
        const double ms = win_cpu_[p] / nc;
        if (ms <= 0) continue;
        cpu_total += ms;
        std::snprintf(buf, sizeof buf, "%s %s %.2f", first ? "" : ",", Name(p), ms);
        out += buf;
        first = false;
    }
    std::snprintf(buf, sizeof buf, "; the swap %.2f; total %.2f\n", win_swap_ / nc, cpu_total + win_swap_ / nc);
    out += buf;
    win_segs_.clear();
    std::memset(win_samples_, 0, sizeof win_samples_);
    std::memset(win_fill_, 0, sizeof win_fill_);
    std::memset(win_cpu_, 0, sizeof win_cpu_);
    std::memset(win_count_, 0, sizeof win_count_);
    win_swap_ = 0;
    win_frames_ = win_dropped_ = win_cpu_frames_ = 0;
    return out;
}
#endif

}  // namespace render
}  // namespace ps2r
