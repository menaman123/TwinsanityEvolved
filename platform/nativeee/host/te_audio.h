// te_audio.h -- the audio owner's hooks for the OTHER host modules (no engine includes needed):
//   * the audio tick is a HOOK of the services' te::Clock (te_services.h): te::Clock::Boundary() -- which the
//     graphics owner's WaitVSync/Present call once per field -- runs OnBoundary() once: one logical audio tick
//     (48000 / BoundaryHz frames, BoundaryHz = te::Settings::field_hz) mixed and pushed to the device. Nobody else
//     calls OnBoundary() (a second call per boundary makes the ring drop; it is public for the tests, which have no
//     clock). Under the real-time producer (play mode on the Vita, TE_AUDIO_RT; audio_service.h) the device thread
//     mixes by wall time instead and OnBoundary() mixes only while a movie's sound track plays.
//   * the runner calls Configure(false) for --no-audio (no device is opened; the mix still runs for hashing).
// The full service (tests, counters) is audio_service.h.
#pragma once
#include <cstdint>

namespace te::audio {
void Configure(bool open_device);
void SetBoundaryHz(uint32_t hz);
void OnBoundary();
// The device ring's health for the health checks (dropped frames and underrun events since the device opened).
struct RingHealth { uint64_t pushed = 0, pulled = 0, dropped = 0, underruns = 0, overruns = 0, padded = 0; bool device = false; };
RingHealth Health();
// The owner's census (audio, stream, movie counters) as lines on fd 2: at exit and from the watchdog (write(2)
// only: no allocation, no stdio lock).
void Report();
}  // namespace te::audio
