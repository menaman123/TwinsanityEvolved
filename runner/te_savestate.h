// te_savestate.h -- the runner's save-state orchestration: the module's own sections (head,
// RAM0, globals + runtime TLS, the game fiber's stack + register file) over the port registry (platform/nativeee/host/
// savestate.h), a save taken when the game fiber yields at a checkpoint, a load applied before the first switch into
// the fiber. Files are the engine's .ps2st envelope (PS2SAVE1, CRC-checked sections).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "te_env.h"

namespace te::savestate_runner {

// Register the runner's sections (call once after the module is instantiated and the fiber prepared).
void Register(w2c_env& env, w2c_game& instance);

// Host side, game fiber suspended at a checkpoint: capture everything to `path`.
bool Save(const std::string& path, uint64_t boundary, std::string* err);

// The sidecar of the state Save just wrote: <path>.meta, key=value lines (the engine's savestate-slot sidecar keys --
// frame= the boundary, wall_ms=, build_id=, composition=, sections= the file's TAG:vN in order -- then `detail`'s own lines,
// e.g. level= and place=): what the state is without loading it (a player's F5 state attached to a report). Written
// atomically (a .tmp renamed over it); false + *err when it cannot be.
bool WriteSidecar(const std::string& path, uint64_t boundary, const std::string& detail, std::string* err);

// Host side, before the game fiber ever ran: restore everything from `path` (memory, module, services, the fiber's
// stack and register file). On success the next te::game::Resume() continues the saved game at its checkpoint.
bool Load(const std::string& path, std::string* err);

// A loaded state's boundary (0 when none was loaded).
uint64_t LoadedBoundary();

// CRC32 of this executable: the build identity a state is tied to (also the harness leg's run.meta build_id).
uint32_t BuildId();

// The executable image's load address and size (what a state's stack addresses are relative to) and its slide.
struct ImageInfo { uint64_t base = 0, size = 0; int64_t slide = 0; };
ImageInfo Image();

// Save states need the image at the same address in the saving and the loading process: when ASLR moved it, re-exec
// ourselves with ASLR disabled (macOS posix_spawn _POSIX_SPAWN_DISABLE_ASLR, Linux personality(ADDR_NO_RANDOMIZE)).
// Returns only when nothing had to be done (or the platform cannot: then *warning says so).
void EnsureNoAslr(int argc, char** argv, std::string* warning);

}  // namespace te::savestate_runner
