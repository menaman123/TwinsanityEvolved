// te_harness.h -- the port's harness leg: the SAME hash streams the engine's title binary writes for a
// `--harness-out` leg, through the engine's own writers (ps2r::verify::VerifyHost: frames.fhash / audio.ahash /
// run.meta; the mem lane off), so tools/oracle/diff.py compares a port run with the recomp oracle's leg
// mechanically (the goal: "frames/audio hashes within tolerance of the oracle on the same padlogs").
//
//   frames.fhash   one line per presented frame, "<key>,0x<fnv1a64>": key = the boundary the present precedes
//                  (1-based, the clock's numbering = --trace-state / fixtures/padlogs), the hash the engine's
//                  FrameHasher::HashNativeScale over the presented RGB (the oracle's convention: key = boundary + 1).
//   audio.ahash    one line per COMPLETED 100 ms window of the mixer's output (4800 stereo frames: 6 ticks at 60 Hz,
//                  5 at 50 Hz), the engine's AudioHasher; a silent window hashes to the hasher's constant.
//   run.meta       the engine's frozen identity keys + the port's census (clock, audio, graphics, heap, state trace).
//   state.trace    PORT ONLY (NEW-only lane; diff_legs.py ignores it): "<boundary>,<state>,<name>" per
//                  GameController::State change -- the alignment anchors diff.py uses.
//   frames.pdesc   PORT ONLY: one perceptual descriptor per presented frame (tools/oracle/pdesc.py mirrors it bit for
//                  bit from a PPM): "<key>,<w>,<h>,<sky>,<mid>,<ground>,<r>,<g>,<b>,<clear_share>,<dhash>" -- the
//                  region luminances (the arena-judge bands), mean RGB, the dominant 12-bit colour's share, a 9x8 dHash.
//                  The fallback when the hashes differ (they do across discs/renderers: the oracle is NOT a byte gate).
//
// Armed from the runner's `--harness-out DIR` only (never the environment). Disarmed, every entry is one branch.
#pragma once
#include <cstdint>
#include <string>

typedef struct ps2e_draw ps2e_draw;

namespace te::harness {

// Arms the leg in `out_dir` (created). build_id labels run.meta. False + *err when the directory cannot be made.
bool Arm(const std::string& out_dir, const std::string& build_id, std::string* err);
bool Armed();
const std::string& LegDir();

// graphics.cpp, after ps2e_draw_present: the device's held presented image (capture armed by graphics when the harness
// is) hashed and described under `key` (the boundary this present precedes).
void OnPresent(ps2e_draw* device, uint64_t key);
// A present the leg does not read back (TE_HARNESS_KEYS names the pictures it does): hash 0 under the key, so the frames
// lane's index stays one line per present.
void OnPresentSkipped(uint64_t key);
// audio_service.cpp, once per boundary after the mix: the tick's interleaved stereo float frames into the audio lane.
// (Also the boundary edge of the engine's VerifyHost: one call per boundary.)
void OnAudioTick(const float* interleaved, int frames);
// The frame boundary (hooked on te::Clock by Arm): the game state trace.
void OnBoundary(uint64_t boundary);
// A NEW-only run.meta key.
void Set(const std::string& key, const std::string& value);
// Tails + the final run.meta (idempotent). The runner calls it with the census already Set().
void Finalize();

// The game controller's state word (include/game/gamecontroller.h GameControllerStates: bits 44..49 = state,
// 38..43 = lastState, 50..55 = nextState) through the retail global G_GameController (PAL 0x00309888). False when the
// controller does not exist yet.
bool ReadGameState(unsigned* state, unsigned* last_state, unsigned* next_state);
const char* StateName(unsigned state);

// The perceptual descriptor of one top-down RGBA8 (or RGB8 when `channels` == 3) image, as frames.pdesc writes it
// (without the key). Shared with the tests.
std::string Describe(const uint8_t* pixels, int w, int h, int channels);

}  // namespace te::harness
