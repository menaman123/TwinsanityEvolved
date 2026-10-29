// te_tick_lock.h -- the Vita's TICK LOCK: a vsync-aligned, adaptive 60 / 30 / 20 Hz game tick for a steady picture.
//
// The device (before the lock) runs the hub at 28-34 presents a second with the game ticking 40-44 times a second: every tick fires the
// fields that elapsed (1 or 2: frameTime 150 or 300), the low-latency hand-over replaces 12-31 % of the pictures before
// they are drawn, and the display shows them for 1, 2 or 3 refreshes -- a 20-60 fps swing the eye reads as judder. The
// lock makes the cadence steady instead: while the last ticks do not fit a field, every PICTURE tick spans exactly N fields
// (N = 2: 30 Hz; N = 3: 20 Hz, the floor) -- the boundary parks until N pacer edges are noted (BoundaryProtocol::
// ParkForPacerTicks) and fires them together, so the game's clock advances N fields per tick (GameTimeTick's frameTime
// 300 / 450: the PS2 game itself at 30 / 20 fps -- the PAL disc's frames are 180 units at 50 Hz and its drops 360) -- the
// pacer's edges are the display's vertical blanks (FieldPacer's external edge: sceDisplayWaitVblankStart), every picture
// of the lock is drawn (no low-latency replacement: the drawing thread has N fields per picture) and the display holds it
// for exactly N refreshes (vitaGL's display queue callback: the swap interval per picture). The input is sampled every
// tick (the pads' boundary hook), the audio is the device thread's (wall time).
//
// WHICH TICKS: a picture tick is a Present with draws that is not a movie picture, whose previous boundary was a picture
// tick too. Everything else -- WaitVSync, a stream wait, the movie player's field loop, the first picture after one of them
// -- stays one field per boundary (the movie player and its per-boundary sound track count boundaries as fields).
//
// THE POLICY (FRAME RATE auto, the Vita's default): 60 while the picture ticks fit; a MISS is a tick that fired more fields
// than its lock (the game thread, or the hand-over waiting for the drawing thread, ran past N fields), a picture replaced
// before drawn (60), or a picture the display held longer than its lock (the GPU / the drawing thread late). The
// misses make a SCORE over the last 30 picture ticks -- a slow tick, a drain or a picture held long weighs 2, a replaced
// picture 1 (the game itself was on time), and a hiccup (a run of missing ticks within 100 ms: one stall of the drawing
// thread, the GPU or the memory card) at most 4 whatever its length -- 12 drop 60 -> 30 (6 slow ticks; or 4 with the game
// work's p90 over 14 ms), 20 drop 30 -> 20 (persistent misses at 30); a lock rises (30 -> 60, 20 -> 30) after a calm second
// whose every tick's game work AND drawing-thread work stayed under 13 ms (to 60, none over 16) / 28 ms (to 30, none over
// 31); two switches are at least 3 s apart. A drop within 6 s of a rise doubles the calm time asked next (1, 2, 4, 8, 16 s);
// 10 s held at the higher rate resets it. (an earlier rule dropped 60 -> 30 on 3 replaced pictures in 30 ticks with the game at 10 ms:
// the window-end census stalling the drawing thread on the memory card -- device-run8.log.) Knobs: TE_VITA_LOCK_DOWN60 /
// _DOWN30 (the scores), _WINDOW, _HICCUP_CAP, _DOWN60_P90_MS / _DOWN60_P90_SCORE, _UP60_MS / _UP30_MS / _UP60_MAX_MS /
// _UP30_MAX_MS, _DWELL_MS.
//
// SETTINGS: the title's OPTIONS > GRAPHIC OPTIONS > FRAME RATE row on the Vita = auto (60/30) | 60 | 30 (display.tick_lock =
// auto | 60 | 30 in ps2r.ini -- the engine's key; --frame-rate auto|60|30); TE_VITA_TICK_LOCK=auto|60|30|20|ab overrides it
// (ab: auto and off (= 60, the behaviour without the lock) alternating every TE_VITA_TICK_LOCK_AB_EVERY presents, default 600). Off the
// Vita, under the deterministic profile, or with fixed 60: every call is the field-per-tick of before.
//
// THE 50 Hz GAME RATE (game.hz = 50 / --hz 50: the PAL disc's own loop -- the Vita's built-in arguments): the
// same lock in its TIMER family. A 60 Hz panel has no 50 Hz refresh, so the pacer's fields stay its 20 ms timer edges at every
// rate and each picture is shown from the first refresh after it is ready (the display callback's swap interval 1: a 50 Hz
// picture held 1 refresh 4 times in 5 and 2 once -- the cadence 1,1,1,1,2 -- a 25 Hz one 2 or 3). The rates are 50 and its
// half-rate fallback 25 (2 timer fields a picture tick: frameTime 360, the PAL game dropping a frame); the row reads
// auto (50/25) | 50 | 25 (the same three display.tick_lock values: auto, the full rate, the half); TE_VITA_TICK_LOCK also takes
// 50 / 25. Its policy's knobs: TE_VITA_LOCK_DOWN50 (12), _DOWN50_P90_MS (23) / _DOWN50_P90_SCORE (6: above one hiccup's
// capped 4), _UP50_MS (19), _UP50_MAX_MS (24); the census lines say "50/25 Hz" and "the 50 Hz game rate".
//
// THE CENSUS: a line per pace window (graphics.cpp NotePace) "[te.vita.lock] ..." and one at exit (the clock census).
// Each window line ends with the GAME TIME VS WALL: "game time vs wall: G game fields / W wall fields at H Hz
// (g s of game time in w s, R %); drift D ms this window, C ms since boundary B" -- G from the game's own clock (T0
// counts / counts per field), W from the wall (ms x H / 1000); R = 100 % and D ~ 0 when the game runs in real time.
#pragma once
#include <cstdint>
#include <string>

namespace te::tick_lock {

// The FRAME RATE row's choice on the Vita (te_display.h Settings::tick_lock): 0 auto (60/30 -- 50/25 at the 50 Hz game rate;
// 20 the floor at 60), 1 the full rate (60 / 50), 2 the half (30 / 25).
void Configure(int setting);
// The lock may engage (the Vita, play mode, a 60 or 50 Hz game rate, a setting or knob other than the fixed full rate).
bool Active();

// ---- the game thread ----------------------------------------------------------------------------------------------------
// PresentFrame, at the hand-over of a picture: the fields the boundary after this present will span when it is a picture
// tick (`picture`: draws, not a movie picture) -- 1, 2 or 3 -- or 0 when it is not one. A pure read: CommitPlan makes it
// the plan, right before the present's boundary.
uint32_t PeekPlan(bool picture);
void CommitPlan(uint32_t fields);
// The hand-over replaced a picture the drawing thread had not taken (the low-latency mode at 60): a miss.
void NoteReplaced();
// The hand-over of a locked picture had to wait for the drawing thread (no free slot: the pipeline a picture or more
// behind): the boundary after it is a DRAIN tick -- one field more than its lock, once -- so the queues empty instead of
// keeping that latency for good (the game's and the display's rates are equal at the lock: a backlog cannot drain alone).
void NoteBackpressure();
// Clock::Boundary: the plan for this boundary (0 = not a picture tick: one field), consumed.
uint32_t TakePlan();
// Clock::Boundary, after the fields fired: the outcome, and the tick's work (release -> arrival, ms; < 0 unknown).
void NoteBoundary(uint64_t boundary, uint32_t planned, uint32_t fired, double work_ms);
// Clock::Boundary (play), right after NoteBoundary: the game's T0 counts now (Platform::Time's 576000 a second
// of game time) -- with the wall instant, the window line's GAME TIME VS WALL: the fields of game time the window gave
// the game (T0 counts / the field's counts) against the wall's (ms x hz / 1000), and the drift between the two (this
// window and since the first window). In sync the two are equal (the pacer's fields are wall time and every field adds
// one field of T0); before the 50 Hz clock fix the 50 Hz rate showed 5/6 (9600 counts a field for the 50 Hz field's 11520).
void NoteGameTime(uint64_t boundary, uint64_t t0_counts);
// Clock::Boundary's first statement (a TEST knob, the Vita in play mode): TE_VITA_DIAG_GAME_DELAY_US=us makes every tick's
// game work that much longer (over boundaries TE_VITA_DIAG_GAME_DELAY_FROM .. _TO; 0 = unbounded) -- the device's game
// thread (20-33 ms a tick in the hub) in the emulator, for the lock's engage / release and its A/B.
void DiagGameDelay(uint64_t boundary);
// The pacer follows the display's vertical blank (the lock is engaged); the pacer thread's edge wait.
bool WantDisplayEdge();
void WaitDisplayEdge();

// ---- the drawing thread -------------------------------------------------------------------------------------------------
// Right before a picture's present (its swap): the picture's lock (0 = not a picture tick) -- the display holds it that many
// refreshes. Installs the display callback on the first call while Active().
void BeforeSwap(uint32_t fields);
// A picture drawn: its work on the drawing thread (taken -> presented, the present call itself excluded) and the present.
void NotePictureDrawn(double work_ms, double present_ms);
// The presents counted (NotePresented): the A/B arms flip by them.
void NotePresents(uint64_t presents);

// ---- the census ---------------------------------------------------------------------------------------------------------
// The pace window's line over presents [from, to] that took `seconds` (NotePace; any thread -- the counters are atomic).
std::string WindowLine(uint64_t from, uint64_t to, double seconds);
// The run's line (the runner's exit census).
std::string ExitLine();

}  // namespace te::tick_lock
