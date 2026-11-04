// platform/nativeee (host side) -- te_loading.h: the loads at the device's speed. See the header.
#include "te_loading.h"

#include <chrono>
#include <cstdio>

#include "ps2e_seam.h"
#include "stream_service.h"
#include "te_harness.h"
#include "te_host.h"
#include "te_services.h"

namespace te::loading {
namespace {

constexpr unsigned kLoadingTitle = 6, kLoadingLevel = 9;
using Steady = std::chrono::steady_clock;

struct Load {
    bool open = false;
    unsigned state = 0;
    bool device = false;          // read at the device's speed
    uint64_t first = 0;           // its first boundary (the first of the loading state)
    Steady::time_point since{};
    stream::StreamService::DeviceCount at_start;
    uint64_t reads_done = 0;      // LoadReadsDone answered 1
};

struct State {
    bool device = false;   // the stream service's input as last set (the game in a loading state at the device's speed)
    Load load;
} g;

void Close(uint64_t boundary) {
    Load& l = g.load;
    const double wall_s = std::chrono::duration<double>(Steady::now() - l.since).count();
    const unsigned long long n = boundary - l.first;
    if (l.device) {
        const stream::StreamService::DeviceCount now = stream::Service().DeviceReads();
        std::fprintf(stderr, "[te.load] %s @%llu-%llu: %llu boundaries in %.3f s at the device's speed: %llu reads, %.1f MB (%.1f ms "
                             "reading in the calls); LoadReadsDone 1 to the loader %llu times\n",
                     te::harness::StateName(l.state), (unsigned long long)l.first, (unsigned long long)boundary, n, wall_s,
                     (unsigned long long)(now.reads - l.at_start.reads), double(now.bytes - l.at_start.bytes) / 1e6,
                     now.read_ms - l.at_start.read_ms, (unsigned long long)l.reads_done);
    } else {
        std::fprintf(stderr, "[te.load] %s @%llu-%llu: %llu boundaries in %.3f s at the console's pace\n", te::harness::StateName(l.state),
                     (unsigned long long)l.first, (unsigned long long)boundary, n, wall_s);
    }
    l.open = false;
}

void OnBoundary(uint64_t boundary) {
    unsigned state = 0, last = 0, next = 0;
    const bool in_load = te::harness::ReadGameState(&state, &last, &next) && (state == kLoadingTitle || state == kLoadingLevel);
    const bool device = in_load && GetSettings().disc_speed == Settings::DiscSpeed::Device;
    // a load is the boundaries of one loading state: it ends at the first boundary of another (a state loaded from before
    // the load's start: it starts over)
    if (g.load.open && boundary < g.load.first) g.load.open = false;
    if (g.load.open && (!in_load || state != g.load.state)) Close(boundary);
    if (in_load && !g.load.open) {
        g.load = Load{};
        g.load.open = true;
        g.load.state = state;
        g.load.device = device;
        g.load.first = boundary;
        g.load.since = Steady::now();
        g.load.at_start = stream::Service().DeviceReads();
    }
    if (device != g.device) {
        g.device = device;
        stream::Service().SetLoading(device);
    }
}

// the seam body's answers 1, counted for the load's line
void NoteReadsDone() { g.load.reads_done++; }

}  // namespace

void Install() {
    g = State{};
    Services::Get().clock().AddBoundaryHook("loads", [](uint64_t boundary) { OnBoundary(boundary); });
    std::fprintf(stderr, "[twinsanityevolved] loads: %s\n",
                 GetSettings().disc_speed == Settings::DiscSpeed::Device
                     ? "the disc at the device's speed behind the loading picture, the loader on as soon as its reads are in (disc.speed = device)"
                     : "the disc at the console's pace (disc.speed = console: the modelled DVD and the loader's waits)");
}

bool DeviceSpeed() { return g.device; }

bool ReadsDone() { return g.device && !stream::Service().InFlight(); }

}  // namespace te::loading

extern "C" {

// Platform::System::LoadReadsDone (game/include/platform/system.h): 1 = the streaming loader starts a wanted chunk's load at once
ps2e_s32 ps2e_system_load_reads_done(void) {
    if (!te::loading::ReadsDone()) return 0;
    te::loading::NoteReadsDone();
    return 1;
}

}  // extern "C"
