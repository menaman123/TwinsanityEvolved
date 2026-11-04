#pragma once

#include "common.h"

class GameNode;
struct BehaviourRunner;
struct InstanceContext;

// TwinsanityEvolved: the skip of a scripted cutscene. Every in-game cutscene of the disc is scripted (no cutscene file of
// VideoController's is on it): a director's behaviour runs CutsceneStart, a timeline of waits, its actors' behaviours (started
// on them, told what to do by messages) and CutsceneEnd. The cross button pressed while the game watches one (GameController::
// Watching) ends the scripted WAITS of the cutscene's agents one after another, each as soon as the one before it, until the
// cutscene ends: every frame of the skip, the agents' soonest timed wait (a packet's delay, a move's or a turn's end, a time
// condition's threshold, an animation a condition waits for) is found and the watched frames before the one it ends at are
// PASSED (OrderWaits): for each, the agents' own time is moved on a frame (what their packets, states, last message, character's
// controllers and animations count from, earlier by a frame) and their own updates for it run -- their moves (an interpolation,
// a chase, a spring, a throw: game/src/game/motion.cpp), their animations and the joints an animated hull follows, their
// characters' walk and physics and the triggers they check -- not their scripts nor any other instance's. So the wait ends on
// the next frame, the waits end in the order and at the frames of their agents' own time they end at when the cutscene is
// watched, and the agents are where those frames leave them. A frame in which an agent's script changed state is not passed
// over (its successor acts on it), nor are the frames after one that entered or left a message trigger (the answer comes
// first) -- but an agent going round an idle loop of its own (two to four of its states whose changes run nothing but its
// animations: an animation played again and again, a check made every frame while a message stays fresh) goes at its own pace:
// its rounds hold nothing and its waits are not ordered. A wait for an agent's focus to come near, while the physics world
// moves the focus (a body rolled, thrown, falling), is a wait the passed frames drive when every body the world frames is an
// agent's: they frame the physics world too, after the agents' updates as the game frame does, and the one that brings the focus
// near is the last. The speech track and the agents' sounds are not played. The scripts run their own commands in their own
// order, once a game frame; the game's clock, the physics world and the other instances keep their pace -- and so the scripts
// reach their end in a few frames, leaving what they leave when watched: where the player and the actors are, the flags,
// counters, items, modes and music they set.
//
// The cutscene's agents: the instance whose behaviour runs CutsceneStart (the director), every instance a cutscene agent starts
// a behaviour on (the actors, the player among them), every instance a cutscene agent sends a message to (what the cutscene
// sets going), every instance a message trigger tells because an agent entered or left it, while the cutscene is on, and, in a
// skip, the player and the focus an agent waits to come near. The others run as before.

// A cutscene started in play (its director, CutsceneStart's), and an agent of the cutscene on (an instance `by` a cutscene
// agent started a behaviour on or sent a message to: none when `by` is not one -- during a skip this holds the waits' ends, as
// CutsceneEndsWaits says); the agents forgotten when the cutscene ends
void NoteCutsceneDirector(InstanceContext* director);
void NoteCutsceneAgent(const InstanceContext* by, InstanceContext* agent);
// An instance a message trigger tells because `by` entered or left it: one of the cutscene's agents while one is on and `by` is one
// (the message is the agent's)
void NoteCutsceneTold(const InstanceContext* by, InstanceContext* told);
void ForgetCutsceneAgents();
// An instance destroyed: no longer an agent nor a recent note (its memory is freed after)
void ForgetCutsceneAgent(const InstanceContext* instance);
// The graphs a director's runner runs as it starts the cutscene (CutsceneStart's: the director's graph and the ones it runs as
// children), and at the cross button the graphs every agent of the cutscene runs (a director of several cutscenes runs the one it
// plays after its CutsceneStart), each asked of the platform (Platform::System::CutsceneSkippable: its table of the disc's
// cutscenes says which ones a skip does not leave as watching them leaves them, or were never checked). A cutscene with a
// refused one is not skipped: the cross button does nothing in it and it plays as watched. A part chained onto a skip that is
// on (another director's CutsceneStart in the same watching) does not stop it: the skip was verified with its chain
void NoteCutsceneSite(const BehaviourRunner* runner);
u32 CutsceneSkipRefused();
// Whether the instance is one of the cutscene's agents (the cutscene on, skipped or not)
u32 IsCutsceneAgent(const InstanceContext* instance);

// The skip: started by the cross button in Watching, over with the cutscene (CutsceneEnd: what starts after it keeps its waits)
// or once the game leaves the cutscene's states another way (Watching, a pause in it, a movie it plays, StartingPlay after it).
// StepCutsceneSkip is the game controller's every frame with its state and next state: it ends the skip, and forgets a watched
// cutscene's agents, there
void StartCutsceneSkip();
void EndCutsceneSkip();
void StepCutsceneSkip(u32 state, u32 next);
// Whether the skip has stalled: no timed wait among its agents and none of their states changed for StallFrames (two seconds)
// while the cutscene goes on -- its end in the hands of what it does not drive (another script that started on its own, at its
// own pace). The game controller then gives the rest of the cutscene back to normal play: it plays on as watched
u32 CutsceneSkipStalled();
u32 CutsceneSkipping();
// Whether the node's agent is one of a skipped cutscene's (the skip on): its sounds and the speech track are not played
u32 CutsceneSkips(const GameNode* node);
u32 CutsceneSkipsInstance(const InstanceContext* instance);
// Whether the skip moves the waits on this frame (OrderWaits) and finishes at once the node's agent's untimed move its physics body
// steers (the others step at their watched frames: game/src/game/motion.cpp). Not for two
// frames after a cutscene agent messaged another or started a behaviour on it: the recipients react to it first, as they do while
// the sender's own next wait passes when the cutscene is watched (a director's short pause after telling its actors what to do).
// At most HeldSkipFrames a skip
u32 CutsceneEndsWaits(const GameNode* node);
constexpr u32 HeldSkipFrames = 240;

// Whether the skip is passing a watched frame of its agents (their own updates, not their scripts: game/src/game/cutsceneskip.cpp
// PassFrame), and an event meanwhile that a script may answer (a message trigger entered or left: the frames after it wait)
u32 CutscenePassesFrame();
void NoteCutscenePassedEvent();
// A skipped agent's packet's motion for a frame the skip passes (PacketFrame's but its end: game/src/game/motion.cpp)
void PassPacketFrame(struct BehaviourRunner* runner, struct TimeClock* clock);

// A skipped agent's interpolation at the frames the watched cutscene steps it at, a frame's clock step (`step`) apart from its
// packet's start (game/src/game/motion.cpp): the clock units from its start to the frame that ends it -- the first past its time
// or, with a tolerance, whose point is within it (that frame doesn't move it) -- and its frame now: the watched frame its agent's
// time is at, the frame before the end and the end frame once that is reached
s32 InterpolationEndFrame(struct ObjectNode* node, struct BehaviourRunner* runner, s32 step);
u32 StepSkippedInterpolation(struct ObjectNode* node, struct BehaviourRunner* runner);
// A skipped agent's untimed move (a spring, a throw, a chase) is stepped at every watched frame its agent's time passes, with its
// own step code (game/src/game/motion.cpp PacketFrame); the clock units from its packet's start to the frame that code ends it at
// (stepped on with what it changes put back), NoMoveEnd when it isn't one or doesn't end within a minute
constexpr s32 NoMoveEnd = -1;
s32 SkippedMoveEndFrame(struct ObjectNode* node, struct BehaviourRunner* runner, s32 step);

