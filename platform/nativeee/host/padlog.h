// padlog.h -- the pad log (fixtures/padlogs/*.padlog): pad 0's sample per frame boundary, INPUTS ONLY (fixtures
// hold metadata, never game data). A text file; one line per change:
//
//   # comment
//   <boundary> <buttons> <lx> <ly> <rx> <ry>        the sample from this boundary on (until the next line)
//   hold <from> <to> <buttons> [<lx> <ly> <rx> <ry>] authoring: the buttons held over [from, to] (scripts only)
//
// <buttons> is either a hex word in WIRE order, active high (SELECT bit 0, L3 1, R3 2, START 3, UP 4, RIGHT 5, DOWN 6,
// LEFT 7, L2 8, R2 9, L1 10, R1 11, TRIANGLE 12, CIRCLE 13, CROSS 14, SQUARE 15) or names joined by '+'
// (START+CROSS); sticks are 0..255, 128 neutral (omitted = neutral). Boundaries are te::Clock::BoundaryCount()
// values (the pads are sampled once per boundary), so a log replays bit-exactly under --deterministic.
#pragma once
#include <cstdint>
#include <string>

namespace te::padlog {

// Replace pad 0's sampler with the log's timeline (connected pad). False with *error on a bad file.
bool Play(const std::string& path, std::string* error);
// The same for an authoring script (`hold` lines are accepted; plain lines too).
bool Script(const std::string& path, std::string* error);
// Wrap the current sampler: pad 0's samples are written to `path` (one line per change) until Finish().
bool Record(const std::string& path, std::string* error);
// Flush and close a recording (idempotent).
void Finish();
// a comment line (`# <text>`) written into the recording in progress at this boundary -- the speedrun mod's splits, so a
// recorded run carries its timer. Nothing when not recording. A replay skips comment lines.
void Note(const std::string& text);
bool Recording();

// Button word helpers (wire order). Names: the list above; returns false on an unknown name.
bool ParseButtons(const std::string& text, uint16_t& buttons);
std::string ButtonNames(uint16_t buttons);

}  // namespace te::padlog
