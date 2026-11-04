#include "game/cutsceneskip.h"

#include "game/agentlab.h"
#include "game/agentparts.h"
#include "game/agents.h"
#include "game/animation.h"
#include "game/behaviours.h"
#include "game/characters.h"
#include "game/chunkdata.h"
#include "game/clock.h"
#include "game/commands.h"
#include "game/controllers.h"
#include "game/cutscenereader.h"
#include "game/cutscenes.h"
#include "game/events.h"
#include "game/gamecontroller.h"
#include "game/instances.h"
#include "game/nodecontrollers.h"
#include "game/objectnode.h"
#include "game/objects.h"
#include "game/player.h"
#include "game/rigidbody.h"
#include "game/sound.h"
#include "platform/system.h"

#include <stdio.h>

// TwinsanityEvolved: the skip of a scripted cutscene (game/cutsceneskip.h)

extern "C"
{
    // The time a packet's end is counted from (none before it: game/src/game/motion.cpp CheckPacketEnd)
    extern s32 g_PacketTimeBase RETAIL(D_0030A918);
}

namespace
{
// The agents a cutscene has (its director, the actors it runs, what it messages: a cutscene has a few dozen at most)
constexpr u32 MostAgents = 128;

const InstanceContext* g_Agents[MostAgents];
u32 g_AgentCount = 0;
u32 g_CutsceneOn = 0;
u32 g_Skipping = 0;
// The frames the skip has run (its log line: how many frames the cutscene's remaining scripts took), the frames left to hold
// the waits' ends (a cutscene agent told another something) and the frames held so far
u32 g_SkipFrames = 0;
u32 g_Hold = 0;
u32 g_HeldFrames = 0;
// The frames the waits were put in order on and the cutscene time they shortened (clock units): the log line's
u32 g_OrderedWaits = 0;
u32 g_SkippedTime = 0;
// The agents' states as one word, and the frames it has not changed with no timed wait left: a skip that stalls so long (the
// cutscene's end in the hands of something it does not drive) gives the rest of the cutscene back to normal play
u32 g_Signature = 0;
u32 g_StalledFrames = 0;
constexpr u32 StallFrames = 120;
// The frames a message or a behaviour started holds the waits' ends: the frame it was sent in ends before the next two updates of
// the world (GameController::Update runs after the chunks' instances, game/src/game/context.cpp)
constexpr u32 HoldFrames = 3;
// What instances started behaviours on or messaged others in the last frames while no cutscene was on: a director starts its
// actors before its CutsceneStart (the same body, or the state before), and they are its agents too
struct RecentNote
{
    const InstanceContext* by;
    InstanceContext* agent;
    u32 frame;
};
constexpr u32 RecentNotes = 64;
constexpr u32 RecentFrames = 60;
RecentNote g_Recent[RecentNotes];
u32 g_RecentNext = 0;
u32 g_Frame = 0;
// The graph of the cutscene on that the platform refuses to skip (NoGraph: none), and whether the cross button was refused in it
// (its log line, once a cutscene)
constexpr u32 NoGraph = 0xFFFFFFFF;
u32 g_RefusedGraph = NoGraph;
u32 g_RefusalTold = 0;
// Whether the skip is passing a watched frame of its agents (PassFrame), and the message triggers entered or left in it
u32 g_Passing = 0;
u32 g_PassedEvents = 0;

// An instance's object node when it is one with behaviours (an ObjectNode: a pickup's and a projectile's object nodes are laid
// out otherwise past the part they share)
ObjectNode* ObjectNodeOf(const InstanceContext* instance)
{
    auto* node = static_cast<ObjectNode*>(GetGameNode(&const_cast<InstanceContext*>(instance)->nodes, NodeObject));
    return node != nullptr && node->vtable == g_ObjectNodeVTable ? node : nullptr;
}

void Add(const InstanceContext* instance)
{
    if (instance == nullptr || IsCutsceneAgent(instance) != 0)
    {
        return;
    }

    if (g_AgentCount < MostAgents)
    {
        g_Agents[g_AgentCount++] = instance;
    }
}

// An agent's runner level as the skip saw it the frame before (TrackLevels): the state it ran and the three it ran before that,
// how many of its last changes were quiet (QuietJump) and whether it goes round an IDLE LOOP of its own -- a cycle of two to four
// of its states whose changes run nothing but its own animations: an animation played again and again, a check made every frame
// while a message it got stays fresh. A loop's rounds are the agent's own pace, not the cutscene's timeline: they hold no wait's
// end and the loop's waits are not ordered (OrderWaits). Leaving the loop (a state outside it, a change that does more) ends it
constexpr u32 TrackedStates = 4;
struct LevelTrack
{
    const InstanceContext* agent;
    const BehaviourRunner* runner;
    u32 level;
    GraphState* states[TrackedStates];
    u32 quiet;
    bool idle;
};
constexpr u32 MostTracks = 256;
LevelTrack g_Tracks[MostTracks];
u32 g_TrackCount = 0;

// A wait of an agent's for its focus to come near (MeToFocusSqrDist): the waiting agent's node, the condition, and the focus
// while the physics world moves it (its body: rolled, thrown, falling). The frames the skip passes take that body's frames as the
// world takes them, and stop at the one that brings it near: the next game frame's script finds it there, at its watched frame
struct NearWait
{
    ObjectNode* node;
    const ScriptCondition* condition;
    InstanceContext* focus;
    DynamicBody* body;
};
constexpr u32 MostNearWaits = 8;
NearWait g_NearWaits[MostNearWaits];
u32 g_NearWaitCount = 0;
// Whether a frame passed brought a near wait's focus near
bool g_NearEnded = false;
// Whether every body the physics world frames is a cutscene agent's (and it frames one): the frames the skip passes then frame the
// world too, as the game frame does after its instances -- what moves those bodies is all in the passed frames
bool g_WorldDriven = false;

// The track of an agent's runner level (none yet: made, with the state it runs, when it runs one)
LevelTrack* TrackOf(const InstanceContext* agent, const BehaviourRunner* runner, u32 level, GraphState* state)
{
    for (u32 index = 0; index < g_TrackCount; index++)
    {
        LevelTrack& track = g_Tracks[index];
        if (track.runner == runner && track.level == level && track.agent == agent)
        {
            return &track;
        }
    }

    if (state == nullptr || g_TrackCount >= MostTracks)
    {
        return nullptr;
    }

    g_Tracks[g_TrackCount] = {agent, runner, level, {state, nullptr, nullptr, nullptr}, 0, false};
    return &g_Tracks[g_TrackCount++];
}

// Whether a state change of a level ran nothing but the agent's own animations: every body of the state left that jumps to the
// state entered runs only DoAnimation (or nothing), and the state entered starts no child behaviour and no move (a delay only)
bool QuietJump(const GraphState* from, const GraphState* to)
{
    if (to->bits.child != NoScriptId || (to->packet != nullptr && to->packet->settings.motion != ControlPacket::NoMotion))
    {
        return false;
    }

    bool jumps = false;
    for (const StateBody* body = from->bodies; body != nullptr; body = body->bits.hasNext != 0 ? body->next : nullptr)
    {
        if (body->bits.hasJump == 0 || body->jump != to)
        {
            continue;
        }

        for (const ScriptCommand* command = body->commands; command != nullptr; command = command->next)
        {
            if (command->vtable != g_DoAnimationCommandVTable)
            {
                return false;
            }
        }

        jumps = true;
    }

    return jumps;
}

// A level's change to `state` (none: the level ended): idle when it closes a cycle of the level's last states whose changes were
// all quiet
void TrackChange(LevelTrack* track, GraphState* state)
{
    if (state == nullptr)
    {
        *track = {track->agent, track->runner, track->level, {nullptr, nullptr, nullptr, nullptr}, 0, false};
        return;
    }

    GraphState* from = track->states[0];
    track->quiet = from != nullptr && QuietJump(from, state) ? track->quiet + 1 : 0;
    u32 cycle = 0;
    for (u32 back = 1; back < TrackedStates && cycle == 0; back++)
    {
        cycle = track->states[back] == state ? back + 1 : 0;
    }

    track->idle = cycle != 0 && track->quiet >= cycle;
    for (u32 back = TrackedStates - 1; back > 0; back--)
    {
        track->states[back] = track->states[back - 1];
    }

    track->states[0] = state;
}

// Whether an agent's runner level goes round an idle loop
bool LevelIdles(const InstanceContext* agent, const BehaviourRunner* runner, u32 level)
{
    for (u32 index = 0; index < g_TrackCount; index++)
    {
        const LevelTrack& track = g_Tracks[index];
        if (track.runner == runner && track.level == level && track.agent == agent)
        {
            return track.idle;
        }
    }

    return false;
}

// Every agent's runner levels looked at (each skip frame): their changes since the frame before tracked
void TrackLevels()
{
    for (u32 index = 0; index < g_AgentCount; index++)
    {
        const InstanceContext* agent = g_Agents[index];
        ObjectNode* node = ObjectNodeOf(agent);
        if (node == nullptr)
        {
            continue;
        }

        for (BehaviourRunner* runner : node->runners)
        {
            if (runner == nullptr)
            {
                continue;
            }

            for (u32 at = 0; at < BehaviourRunner::Levels; at++)
            {
                BehaviourLevel* level = runner->levels[at];
                bool runs = runner->receivers != nullptr && at <= runner->depth && level != nullptr && level->bits.finished == 0;
                GraphState* state = runs ? level->state : nullptr;
                LevelTrack* track = TrackOf(agent, runner, at, state);
                if (track != nullptr && track->states[0] != state)
                {
                    TrackChange(track, state);
                }
            }
        }
    }
}

// An agent let go: its tracks too
void DropTracks(const InstanceContext* agent)
{
    u32 kept = 0;
    for (u32 index = 0; index < g_TrackCount; index++)
    {
        if (g_Tracks[index].agent != agent)
        {
            g_Tracks[kept++] = g_Tracks[index];
        }
    }

    g_TrackCount = kept;
}
}

namespace
{
// The cutscene's agents by their objects' names, one log line (the verification's list of what the cutscene drives), written
// whole: the host's own lines go between the module's writes
void LogAgents(const char* how)
{
    char line[2048];
    s32 used = snprintf(line, sizeof(line), "[te.cutscene] %s, %u agents:", how, g_AgentCount);
    for (u32 index = 0; index < g_AgentCount && used > 0 && used < static_cast<s32>(sizeof(line)) - 2; index++)
    {
        auto* node = static_cast<ObjectNodeBase*>(GetGameNode(&const_cast<InstanceContext*>(g_Agents[index])->nodes, NodeObject));
        GameObject* object = node != nullptr ? node->object : nullptr;
        used += snprintf(line + used, sizeof(line) - static_cast<u32>(used), " %s",
                         object != nullptr && object->name.string != nullptr ? object->name.string : "?");
    }

    printf("%s\n", line);
}
}

void NoteCutsceneDirector(InstanceContext* director)
{
    g_CutsceneOn = 1;
    Add(director);
    // the director's (and so its agents') starts and messages of the last frames before it
    for (u32 pass = 0; pass < 2; pass++)
    {
        for (const RecentNote& note : g_Recent)
        {
            if (note.agent != nullptr && g_Frame - note.frame <= RecentFrames && IsCutsceneAgent(note.by) != 0)
            {
                Add(note.agent);
            }
        }
    }
}

void NoteCutsceneAgent(const InstanceContext* by, InstanceContext* agent)
{
    if (g_CutsceneOn == 0 && by != nullptr && agent != nullptr)
    {
        g_Recent[g_RecentNext] = {by, agent, g_Frame};
        g_RecentNext = (g_RecentNext + 1) % RecentNotes;
        return;
    }

    if (g_CutsceneOn != 0 && agent != nullptr && IsCutsceneAgent(by) != 0)
    {
        Add(agent);
        if (g_Skipping != 0 && agent != by && g_HeldFrames < HeldSkipFrames)
        {
            g_Hold = HoldFrames;
        }
    }
}

void NoteCutsceneTold(const InstanceContext* by, InstanceContext* told)
{
    // No hold of the waits: a trigger tells what stays in it every frame (its entry, while the skip passes frames, holds them:
    // NoteCutscenePassedEvent), and the told agent's answer is a change of its state (OrderWaits)
    if (g_CutsceneOn != 0 && told != nullptr && IsCutsceneAgent(by) != 0)
    {
        Add(told);
    }
}

void ForgetCutsceneAgents()
{
    g_AgentCount = 0;
    g_TrackCount = 0;
    g_CutsceneOn = 0;
    g_RefusedGraph = NoGraph;
    g_RefusalTold = 0;
}

namespace
{
// The first graph a running runner's levels run that the platform refuses to skip (Platform::System::CutsceneSkippable), NoGraph
// none
u32 RefusedGraphOf(const BehaviourRunner* runner)
{
    if (runner == nullptr || runner->receivers == nullptr)
    {
        return NoGraph;
    }

    for (u32 index = 0; index <= runner->depth && index < BehaviourRunner::Levels; index++)
    {
        const BehaviourLevel* level = runner->levels[index];
        const GraphData* graph = level != nullptr && level->bits.finished == 0 ? level->graph : nullptr;
        if (graph != nullptr && Platform::System::CutsceneSkippable(graph->bits.id) == 0)
        {
            return graph->bits.id;
        }
    }

    return NoGraph;
}

// What the cutscene's agents run when the cross button is pressed: a director that directs several cutscenes runs the one of
// them it plays (its content graph) only after its CutsceneStart
void NoteAgentsGraphs()
{
    for (u32 index = 0; index < g_AgentCount && g_RefusedGraph == NoGraph; index++)
    {
        ObjectNode* node = ObjectNodeOf(g_Agents[index]);
        if (node == nullptr)
        {
            continue;
        }

        for (BehaviourRunner* runner : node->runners)
        {
            if (g_RefusedGraph == NoGraph)
            {
                g_RefusedGraph = RefusedGraphOf(runner);
            }
        }
    }
}
}

void NoteCutsceneSite(const BehaviourRunner* runner)
{
    if (g_CutsceneOn != 0 && g_RefusedGraph == NoGraph)
    {
        g_RefusedGraph = RefusedGraphOf(runner);
    }
}

u32 CutsceneSkipRefused()
{
    return g_CutsceneOn != 0 && g_RefusedGraph != NoGraph ? 1 : 0;
}

void ForgetCutsceneAgent(const InstanceContext* instance)
{
    // The agents and the recent notes are kept by the instances' addresses: one destroyed while they are kept (a cutscene's drone
    // that destroys itself, an actor its chunk lets go) is let go here, so that nothing reads its memory once it is freed (the
    // watched cutscene's agents line at its end, the skip's every frame). The others keep their order
    u32 kept = 0;
    for (u32 index = 0; index < g_AgentCount; index++)
    {
        if (g_Agents[index] != instance)
        {
            g_Agents[kept++] = g_Agents[index];
        }
    }

    g_AgentCount = kept;
    g_NearWaitCount = 0;
    DropTracks(instance);
    for (RecentNote& note : g_Recent)
    {
        if (note.by == instance || note.agent == instance)
        {
            note = {nullptr, nullptr, 0};
        }
    }
}

u32 IsCutsceneAgent(const InstanceContext* instance)
{
    if (instance == nullptr)
    {
        return 0;
    }

    for (u32 index = 0; index < g_AgentCount; index++)
    {
        if (g_Agents[index] == instance)
        {
            return 1;
        }
    }

    return 0;
}

void StartCutsceneSkip()
{
    if (g_Skipping != 0 || g_CutsceneOn == 0)
    {
        return;
    }

    NoteAgentsGraphs();
    if (CutsceneSkipRefused() != 0)
    {
        if (g_RefusalTold == 0)
        {
            g_RefusalTold = 1;
            printf("[te.cutscene] not skipped: this cutscene (graph %u) cannot be skipped; it plays as watched\n", g_RefusedGraph);
        }

        return;
    }

    // The player is one of a skipped cutscene's agents: his own updates (a fall, a slide, the vehicle he rides) run in the frames
    // the skip passes, his times move on with the agents'
    Add(g_PlayerInstance != nullptr ? static_cast<InstanceContext*>(g_PlayerInstance->object) : nullptr);
    g_Skipping = 1;
    g_SkipFrames = 0;
    g_Hold = 0;
    g_HeldFrames = 0;
    g_OrderedWaits = 0;
    g_SkippedTime = 0;
    g_Signature = 0;
    g_StalledFrames = 0;
    g_TrackCount = 0;
    g_NearWaitCount = 0;
    // The cutscene's speech track (VideoController's music-only video) stopped as its end stops it: one that plays finishes on the
    // controller's next frame (UpdatePlayingCutscene: the context music no longer playing), one only queued is finished now
    VideoController* video = G_VideoController;
    if (video != nullptr && video->bits.musicOnly != 0)
    {
        StopCutsceneMusic(video);
        video->bits.musicAsked = 0;
        video->bits.waitingForMusic = 0;
        if (video->state == VideoController::StateQueued)
        {
            StopCutscene(video);
            video->bits.musicOnly = 0;
            video->state = VideoController::StateFinished;
        }
    }

    printf("[te.cutscene] skipped: %u agents\n", g_AgentCount);
}

u32 CutsceneSkipStalled()
{
    return g_Skipping != 0 && g_StalledFrames >= StallFrames ? 1 : 0;
}

void EndCutsceneSkip()
{
    if (g_Skipping == 0)
    {
        return;
    }

    if (CutsceneSkipStalled() != 0)
    {
        printf("[te.cutscene] skip given back: the agents waited %u frames on nothing they drive; the cutscene plays on\n",
               g_StalledFrames);
    }

    g_Skipping = 0;
    printf("[te.cutscene] skip over after %u frames (%u held for the agents' answers; %u agents; %.2f s of the cutscene's waits "
           "ended)\n",
           g_SkipFrames, g_HeldFrames, g_AgentCount, static_cast<f32>(g_SkippedTime) * g_SecondsPerClockUnit);
    LogAgents("skipped");
    ForgetCutsceneAgents();
}

u32 CutscenePassesFrame()
{
    return g_Passing;
}

void NoteCutscenePassedEvent()
{
    g_PassedEvents += g_Passing;
}

u32 CutsceneSkipping()
{
    return g_Skipping;
}

u32 CutsceneSkipsInstance(const InstanceContext* instance)
{
    return g_Skipping != 0 && IsCutsceneAgent(instance) != 0 ? 1 : 0;
}

u32 CutsceneSkips(const GameNode* node)
{
    return node != nullptr && CutsceneSkipsInstance(node->owner) != 0 ? 1 : 0;
}

u32 CutsceneEndsWaits(const GameNode* node)
{
    return g_Hold == 0 && CutsceneSkips(node) != 0 ? 1 : 0;
}

namespace
{
// The conditions a state waits on time or an animation with (game/src/game/conditions.cpp's IDs)
constexpr u32 TimeInUnitId = 0x5;
constexpr u32 AnimationFinishedId = 0x7;
constexpr u32 AnimationTimeRemainingId = 0x3A;
constexpr u32 TimeSinceMarkId = 0xAC;
// The condition a state waits for its focus to come near with (its squared distance to the focus)
constexpr u32 MeToFocusSqrDistId = 0xA;
// No timed wait
constexpr s32 NoWait = 0x7FFFFFFF;

s32 Sooner(s32 a, s32 b)
{
    return b < a ? b : a;
}

s32 Units(f32 seconds)
{
    return static_cast<s32>(seconds * g_ClockUnitsPerSecond) + 1;
}

// The animation the root plays's time left (GetAnimationProgress: clock units). An animation another plays over is let go once
// that one has blended in (ProgressAnimationChain): its time left counts only until then
s32 AnimationLeft(InstanceContext* instance, s32 now)
{
    auto* model = static_cast<ModelNode*>(GetGameNode(&instance->nodes, NodeModel));
    OgiAnimator* animator = model != nullptr ? model->animator : nullptr;
    if (animator == nullptr)
    {
        return 0;
    }

    s32 left = Units(GetAnimationProgress(animator, OgiAnimator::RootJoint));
    AnimationChain* chain = animator->root != nullptr ? animator->root->chain : nullptr;
    if (chain == nullptr || chain->main == nullptr || chain->main->next == nullptr)
    {
        return left;
    }

    s32 longest = 0;
    for (const AnimationStatus* status = chain->main; status != nullptr; status = status->next)
    {
        s32 end = status->length - status->time;
        if (status->bits.loops != 0)
        {
            return left;
        }

        const AnimationStatus* next = status->next;
        if (next != nullptr)
        {
            s32 blended = next->bits.blendsIn == 0 ? 0 : next->blendTime - (next->bits.started != 0 ? now - next->blendStart : 0);
            end = blended < end ? blended : end;
        }

        longest = end < longest ? longest : end;
    }

    return longest + 1 < left ? longest + 1 : left;
}

// A wait that begins on the agent's next update: a packet a body started that its motion has not taken up yet (the next update's
// StepPacket starts it; BehaviourRunner::Update)
constexpr s32 WaitStarting = -1;

// Whether the runner's packet is the one of a level that goes round an idle loop
bool PacketIdles(const BehaviourRunner* runner, const ObjectNode* node)
{
    for (u32 index = 0; index <= runner->depth && index < BehaviourRunner::Levels; index++)
    {
        const BehaviourLevel* level = runner->levels[index];
        if (level != nullptr && level->state != nullptr && level->state->packet == runner->packet)
        {
            return LevelIdles(node->owner, runner, index);
        }
    }

    return false;
}

// The clock units from now until a runner's soonest TIMED wait ends (NoWait when it waits on none; WaitStarting while a packet's
// wait is about to begin): its packet's delay, its packet's timed motion (an interpolation, a turn: their duration), and its
// levels' states' time and animation conditions -- not those of a level going round an idle loop (its own pace)
s32 RunnerWait(BehaviourRunner* runner, ObjectNode* node, s32 now)
{
    s32 soonest = NoWait;
    if (runner == nullptr || runner->receivers == nullptr)
    {
        return NoWait;
    }

    ControlPacket* packet = runner->packet != nullptr && !PacketIdles(runner, node) ? runner->packet : nullptr;
    if (packet != nullptr && runner->flags.packetWaiting == 0)
    {
        return WaitStarting;
    }

    if (packet != nullptr && runner->flags.packetWaiting != 0 && packet->settings.stalls == 0)
    {
        if (g_PacketTimeBase < runner->packetEnd)
        {
            soonest = Sooner(soonest, runner->packetEnd - now);
        }

        MotionState* motion = node->motion;
        ControlPacketSettings settings = packet->settings;
        if (motion != nullptr && settings.motion != ControlPacket::NoMotion)
        {
            bool moves = settings.translates != 0 && !motion->bits.translationDone;
            bool turns = settings.rotates != 0 && !motion->bits.rotationDone;
            s32 step = static_cast<s32>(GetContextClock(node->owner)->advance);
            s32 moveEnd = NoMoveEnd;
            if (settings.motion == ControlPacket::LinearInterpolation && (moves || turns))
            {
                // at the watched frame that ends it: its time's or its tolerance's (game/src/game/motion.cpp)
                soonest = Sooner(soonest, runner->packetStart + InterpolationEndFrame(node, runner, step) - now);
            }
            else if ((moves || turns) && (moveEnd = SkippedMoveEndFrame(node, runner, step)) != NoMoveEnd)
            {
                // an untimed move: at the watched frame its own steps end it at
                soonest = Sooner(soonest, runner->packetStart + moveEnd - now);
            }
            else if (turns && !moves)
            {
                f32 inverse = motion->inverseDuration;
                s32 length = 0.0f < inverse ? Units(1.0f / inverse) : 0;
                soonest = Sooner(soonest, runner->packetStart + length - now);
            }
        }
    }

    for (u32 index = 0; index <= runner->depth && index < BehaviourRunner::Levels; index++)
    {
        BehaviourLevel* level = runner->levels[index];
        if (level == nullptr || level->bits.finished != 0 || level->state == nullptr || LevelIdles(node->owner, runner, index))
        {
            continue;
        }

        for (StateBody* body = level->state->bodies; body != nullptr; body = body->bits.hasNext != 0 ? body->next : nullptr)
        {
            ScriptCondition* condition = body->bits.hasCondition != 0 ? body->condition : nullptr;
            if (condition == nullptr)
            {
                continue;
            }

            u32 id = condition->bits.id;
            bool inverted = condition->bits.inverted != 0;
            f32 threshold = condition->threshold;
            if (id == TimeInUnitId && !inverted)
            {
                soonest = Sooner(soonest, static_cast<s32>(level->time) + Units(threshold) - now);
            }
            else if (id == TimeSinceMarkId && !inverted && runner->markedTime != 0)
            {
                soonest = Sooner(soonest, static_cast<s32>(runner->markedTime) + Units(threshold) - now);
            }
            else if (id == AnimationFinishedId && !inverted)
            {
                soonest = Sooner(soonest, AnimationLeft(node->owner, now));
            }
            else if (id == AnimationTimeRemainingId && inverted)
            {
                soonest = Sooner(soonest, AnimationLeft(node->owner, now) - Units(threshold));
            }
        }
    }

    return soonest;
}

void ShiftStatuses(AnimationStatus* status, s32 by)
{
    for (; status != nullptr; status = status->next)
    {
        if (status->bits.started != 0)
        {
            status->began -= by;
        }

        status->blendStart -= by;
    }
}

void ShiftChain(AnimationChain* chain, s32 by)
{
    if (chain != nullptr)
    {
        ShiftStatuses(chain->main, by);
        ShiftStatuses(chain->blendShapes, by);
    }
}

// A time an agent's part keeps (0: none) moved on by `by`
template <typename T> void ShiftTime(T* time, s32 by)
{
    if (*time != 0)
    {
        *time = static_cast<T>(static_cast<s32>(*time) - by);
    }
}

// A character's own times moved on: when its mode, its controllers' states and their presses started, when it stood, left the
// ground, was tied to the other character
void ShiftCharacter(CharacterAgent* character, s32 by)
{
    ShiftTime(&character->modeStart, by);
    ShiftTime(&character->linkTime, by);
    if (character->crouch != nullptr)
    {
        ShiftTime(&character->crouch->stateStart, by);
    }

    if (JumpController* jump = character->jump; jump != nullptr)
    {
        ShiftTime(&jump->stateStart, by);
        ShiftTime(&jump->leftGroundTime, by);
        ShiftTime(&jump->crossTime, by);
        ShiftTime(&jump->circleTime, by);
    }

    if (character->spin != nullptr)
    {
        ShiftTime(&character->spin->stateStart, by);
    }

    if (WalkController* walk = character->walk; walk != nullptr)
    {
        ShiftTime(&walk->stateStart, by);
        ShiftTime(&walk->stoodTime, by);
    }

    if (ClawController* claw = character->claw; claw != nullptr)
    {
        ShiftTime(&claw->stateStart, by);
        ShiftTime(&claw->circleTime, by);
    }

    if (character->gun != nullptr)
    {
        ShiftTime(&character->gun->stateStart, by);
    }
}

// An agent's own time moved on by `by` clock units: what its runners count from (their packets' start and end, their levels'
// state times, their marked time), when it got its last message, its character's times and what its animations count from,
// earlier by that much
void ShiftAgent(InstanceContext* instance, s32 by)
{
    ObjectNode* node = ObjectNodeOf(instance);
    if (node != nullptr)
    {
        for (BehaviourRunner* runner : node->runners)
        {
            if (runner == nullptr)
            {
                continue;
            }

            runner->packetStart -= by;
            if (g_PacketTimeBase < runner->packetEnd)
            {
                runner->packetEnd -= by;
            }

            if (runner->markedTime != 0)
            {
                runner->markedTime -= static_cast<u32>(by);
            }

            for (BehaviourLevel* level : runner->levels)
            {
                if (level != nullptr)
                {
                    level->time -= static_cast<u32>(by);
                }
            }
        }

        // The last message it got (GotUserMessageEquals counts its age)
        if (node->message != NoMessage)
        {
            node->messageTime -= static_cast<u32>(by);
        }
    }

    auto* agent = static_cast<AgentNode*>(GetGameNode(&instance->nodes, NodeCharacter));
    if (agent != nullptr && agent->agent != nullptr)
    {
        ShiftCharacter(static_cast<CharacterAgent*>(agent->agent), by);
    }

    auto* model = static_cast<ModelNode*>(GetGameNode(&instance->nodes, NodeModel));
    OgiAnimator* animator = model != nullptr ? model->animator : nullptr;
    if (animator == nullptr)
    {
        return;
    }

    if (animator->root != nullptr)
    {
        ShiftChain(animator->root->chain, by);
    }

    SizedArray<JointAnimation*>* joints = animator->reactJoints;
    for (s32 index = 0; joints != nullptr && index < joints->size; index++)
    {
        if (joints->data[index] != nullptr)
        {
            ShiftChain(joints->data[index]->chain, by);
        }
    }
}

// The frames the skip passes in one game frame at most (each runs the agents' own updates: PassFrame)
constexpr s32 MostPassedFrames = 60;
// The node kinds whose updates move an agent in a frame the skip passes: its object node's (PassObjectFrame), its model's
// (PassModelFrame), its controls, its character's or creature's frame (a walk, a fall, the physics, the triggers it checks) and
// its attachments
bool PassedKind(u32 kind)
{
    return kind == NodeObject || kind == NodeModel || kind == NodeControls || kind == NodeCharacter || kind == NodeCreature ||
           kind == NodeAttachments;
}

// An agent's object node in a frame the skip passes: ObjectNode::Update's steps but its scripts -- its controller, its
// trajectory, its head tracking, its packets' motion (PassPacketFrame), its rigid body's movement and a pinned agent's move
// kept as the frame's, as its update has them
void PassObjectFrame(ObjectNode* node, TimeClock* clock)
{
    ObjectPlace* place = node->owner->place;
    place->SyncPosition();
    node->frameStart = place->position;
    node->flags.updated = 1;
    node->flags.moves = 0;
    node->flags.unused4 = 0;
    if (node->controller != nullptr)
    {
        CallVirtual<void>(node->controller, node->controller->vtable, NodeController::FrameSlot, clock);
    }

    if (node->trajectory != nullptr)
    {
        StepTrajectory(node->trajectory, clock, node);
    }

    if (node->headTracking != nullptr)
    {
        StepHeadTracking(node->headTracking, clock, node);
    }

    bool runs = false;
    for (u32 slot = 2; slot-- > 0;)
    {
        BehaviourRunner* runner = node->runners[slot];
        if (runner == nullptr || runner->receivers == nullptr)
        {
            continue;
        }

        runs = true;
        if (runner->packet != nullptr && runner->flags.packetWaiting != 0)
        {
            PassPacketFrame(runner, clock);
        }

        if (node->trajectory != nullptr)
        {
            TrajectoryFrame(node->trajectory, node);
        }
    }

    if (!runs && node->trajectory != nullptr)
    {
        TrajectoryFrame(node->trajectory, node);
    }

    StepMovement(node, clock);
    if (node->flags.pinned)
    {
        place = node->owner->place;
        place->SyncPosition();
        node->frameMove = place->position;
        node->frameMove.x = node->frameMove.x - node->frameStart.x;
        node->frameMove.y = node->frameMove.y - node->frameStart.y;
        node->frameMove.z = node->frameMove.z - node->frameStart.z;
        if (!node->flags.moves && place->MoveTo(&node->frameStart))
        {
            QueueObject(node->owner);
        }
    }

    node->GameNode::Update(clock);
}

// The joints' matrices an animator makes in a frame the skip passes (the frame's own memory is the drawing's)
constexpr u32 MostPassedJoints = 256;
Matrix4x4 g_PassedJoints[MostPassedJoints];

// An agent's model in a frame the skip passes: its animations at the frame's time, and its joints posed as its update poses them
// when it was animated the frame before (an animated hull follows them: what it touches and the triggers it is in)
void PassModelFrame(ModelNode* model, TimeClock* clock)
{
    OgiAnimator* animator = model->animator;
    if (animator == nullptr || model->ogi == nullptr)
    {
        model->GameNode::Update(clock);
        return;
    }

    u32 detail = animator->matrices.detail;
    ProgressAnimator(animator, static_cast<s32>(clock->time), detail);
    if (animator->bits.animated != 0 && model->ogi->jointCount <= MostPassedJoints)
    {
        JointMatrices kept = animator->matrices;
        animator->matrices.start = g_PassedJoints;
        animator->matrices.end = g_PassedJoints;
        TransformJoints(animator->root, nullptr, nullptr, &animator->matrices);
        animator->matrices = kept;
        UpdateExitPoints(animator, 1);
    }

    model->GameNode::Update(clock);
}

// The physics world's body that moves an instance (none: its rigid body has none, or the world leaves it out)
DynamicBody* WorldBodyOf(const InstanceContext* instance)
{
    ObjectNode* node = ObjectNodeOf(instance);
    ObjectRigidBody* rigid = node != nullptr ? node->rigidBody : nullptr;
    DynamicBody* body = rigid != nullptr ? rigid->physicsBody : nullptr;
    return body != nullptr && body->bodyFlags.leftOut == 0 && !instance->flags.asleep ? body : nullptr;
}

// The agents' waits for their focus to come near (each skip frame, the levels going round an idle loop left out): a focus is one
// of the cutscene's agents (its own waits are the cutscene's), and one the physics world moves is a near wait
void GatherNearWaits()
{
    g_NearWaitCount = 0;
    for (u32 index = 0; index < g_AgentCount; index++)
    {
        const InstanceContext* agent = g_Agents[index];
        ObjectNode* node = ObjectNodeOf(agent);
        InstanceContext* focus = node != nullptr && node->flags.focusInstance ? node->focusInstance : nullptr;
        if (focus == nullptr)
        {
            continue;
        }

        for (BehaviourRunner* runner : node->runners)
        {
            u32 levels = runner != nullptr && runner->receivers != nullptr ? runner->depth + 1u : 0u;
            for (u32 at = 0; at < levels && at < BehaviourRunner::Levels; at++)
            {
                BehaviourLevel* level = runner->levels[at];
                if (level == nullptr || level->bits.finished != 0 || level->state == nullptr || LevelIdles(agent, runner, at))
                {
                    continue;
                }

                // the bodies its state checks (the completion body left out: BehaviourRunner's BestBody)
                StateBody* first = level->state->bodies;
                first = first != nullptr && level->state->bits.hasCompletion != 0 ? first->next : first;
                for (StateBody* body = first; body != nullptr; body = body->bits.hasNext != 0 ? body->next : nullptr)
                {
                    ScriptCondition* condition = body->bits.hasCondition != 0 ? body->condition : nullptr;
                    if (condition == nullptr || condition->bits.id != MeToFocusSqrDistId)
                    {
                        continue;
                    }

                    Add(focus);
                    DynamicBody* moved = WorldBodyOf(focus);
                    if (moved != nullptr && g_NearWaitCount < MostNearWaits)
                    {
                        g_NearWaits[g_NearWaitCount++] = {node, condition, focus, moved};
                    }
                }
            }
        }
    }
}

// Whether the physics world frames a body (as StepPhysicsWorld picks them: its instance awake, its clock running, not left out)
bool WorldFrames(const DynamicBody* body)
{
    InstanceContext* instance = body->instance;
    return instance != nullptr && !instance->flags.asleep && GetContextClock(instance)->flags.running != 0 &&
           body->bodyFlags.leftOut == 0;
}

// Whether every body the physics world frames is an agent's, and there is one
bool AgentsDriveWorld()
{
    PhysicsWorld* world = g_PhysicsWorld;
    u32 framed = 0;
    for (s32 slot = 0; world != nullptr && slot < PhysicsWorld::Slots; slot++)
    {
        const DynamicBody* body = world->bodies[slot];
        if (body == nullptr || !WorldFrames(body))
        {
            continue;
        }

        if (IsCutsceneAgent(body->instance) == 0)
        {
            return false;
        }

        framed++;
    }

    return framed != 0;
}

// Whether a near wait's condition passes with the places as they are (the squared distance from the waiting agent's place to its
// focus's against the threshold, as the state's body checks it)
bool NearWaitPasses(const NearWait& wait)
{
    ObjectPlace* place = wait.node->owner->place;
    place->SyncPosition();
    ObjectPlace* other = wait.focus->place;
    other->SyncPosition();
    f32 dx = place->position.x - other->position.x;
    f32 dy = place->position.y - other->position.y;
    f32 dz = place->position.z - other->position.z;
    f32 result = dx * dx + dy * dy + dz * dz;
    f32 threshold = wait.condition->threshold;
    return wait.condition->bits.inverted != 0 ? result < threshold : threshold < result;
}

// Whether a near wait's body moves (a body at rest brings nothing nearer)
bool NearWaitMoves(const NearWait& wait)
{
    constexpr f32 RestingSpeedSquared = 1e-4f;
    const Vector4& velocity = wait.body->velocity;
    return RestingSpeedSquared < velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z;
}

// Whether a near wait waits on what the passed frames move: its condition does not pass yet, its body moves, and the passed frames
// frame the physics world (g_WorldDriven)
bool NearWaitPending(const NearWait& wait)
{
    return g_WorldDriven && !NearWaitPasses(wait) && NearWaitMoves(wait);
}

// Whether an agent's own updates run in the frames the skip passes: the game updated it the frame before (one far from the camera
// is updated seldom: it is left as it is), and it has no rigid body (the physics world moves it and what it stands on) -- unless
// its body is the physics world's and the world frames only agents' bodies: then the passed frames frame the world too
bool Passes(const InstanceContext* instance)
{
    ObjectNode* object = ObjectNodeOf(instance);
    return instance->chunk != nullptr && object != nullptr && object->flags.updated != 0 &&
           (object->rigidBody == nullptr || (g_WorldDriven && WorldBodyOf(instance) != nullptr));
}

// An agent's follower, updated with it in the frames the skip passes: the mask that follows the player (its controller's)
bool FollowsAgent(const GameNode* node, u32 kind)
{
    // An object node of the plain kind (the pickups' and the projectiles' are laid out otherwise)
    if (kind != NodeObject || node->vtable != g_ObjectNodeVTable)
    {
        return false;
    }

    const NodeController* controller = static_cast<const ObjectNode*>(node)->controller;
    return controller != nullptr && controller->kind == NodeController::KindMask &&
           IsCutsceneAgent(static_cast<const MaskController*>(controller)->character) != 0;
}

// The agents' nodes a passed frame updates (and their followers'), in the order the game updates them: the chunks' (UpdateChunks:
// the current one first), each chunk's node kinds in the step order (ChunkInstances::Update), each kind's nodes in its list; a
// kind's last node marks where the queued objects are stepped
constexpr u32 MostPassedNodes = 512;
struct PassedNode
{
    GameNode* node;
    u32 kind;
    bool lastOfKind;
};
PassedNode g_PassedNodes[MostPassedNodes];
u32 g_PassedNodeCount = 0;

void GatherChunkNodes(ChunkData* chunk)
{
    if (chunk == nullptr || chunk->flags.state != ChunkShown || chunk->instances == nullptr)
    {
        return;
    }

    for (u32 at = 0; at < g_StepKindCount; at++)
    {
        u32 kind = g_StepKinds[at];
        u32 bit = 1u << (kind & ShiftMask);
        if (!PassedKind(kind) || (static_cast<u32>(chunk->steppedKinds) & bit) != bit)
        {
            continue;
        }

        u32 first = g_PassedNodeCount;
        GameNode* node = chunk->instances->nodes[kind];
        for (; node != nullptr && g_PassedNodeCount < MostPassedNodes; node = node->next)
        {
            if ((IsCutsceneAgent(node->owner) != 0 || FollowsAgent(node, kind)) && Passes(node->owner))
            {
                g_PassedNodes[g_PassedNodeCount++] = {node, kind, false};
            }
        }

        if (first < g_PassedNodeCount)
        {
            g_PassedNodes[g_PassedNodeCount - 1].lastOfKind = true;
        }
    }
}

void GatherPassedNodes()
{
    g_PassedNodeCount = 0;
    ChunkList* list = GetChunkList();
    ChunkData* current = list->current != nullptr ? list->current->chunk : nullptr;
    GatherChunkNodes(current);
    for (ChunkData* chunk = list->first; chunk != nullptr; chunk = chunk->next)
    {
        if (chunk != current)
        {
            GatherChunkNodes(chunk);
        }
    }
}

// A watched frame the skip passes: every agent's time a frame on, then their own updates for it -- the nodes of the kinds that
// move them (PassedKind) in the order the game updates them (GatherPassedNodes), each a clock step after its last update; not
// their scripts (the waits the skip shortens are theirs) nor any other instance's (the world keeps its pace). Whether no message
// trigger was entered or left in it (the agents' moves crossed one): the frames after one wait for the answer
bool PassFrame(s32 step)
{
    g_SkippedTime += static_cast<u32>(step);
    for (u32 index = 0; index < g_AgentCount; index++)
    {
        ShiftAgent(const_cast<InstanceContext*>(g_Agents[index]), step);
    }

    g_Passing = 1;
    g_PassedEvents = 0;
    for (u32 index = 0; index < g_PassedNodeCount; index++)
    {
        const PassedNode& passed = g_PassedNodes[index];
        GameNode* node = passed.node;
        TimeClock* clock = GetContextClock(node->owner);
        node->time = clock->time - static_cast<u32>(step);
        if (passed.kind == NodeObject)
        {
            if (node->vtable == g_ObjectNodeVTable)
            {
                PassObjectFrame(static_cast<ObjectNode*>(node), clock);
            }
        }
        else if (passed.kind == NodeModel)
        {
            PassModelFrame(static_cast<ModelNode*>(node), clock);
        }
        else
        {
            CallVirtual<u32>(node, node->vtable, GameNode::UpdateSlot, clock);
        }

        node->time = clock->time;
        if (passed.lastOfKind && g_QueuedObjects.count != 0)
        {
            StepQueuedObjects();
        }
    }

    // The physics world's frame when it frames only agents' bodies, after the instances as in the game frame
    // (game/src/game/context.cpp); a near wait's focus that came near ends the frames passed: the next game frame's script finds it
    // there
    if (g_WorldDriven)
    {
        StepPhysicsWorld(g_PhysicsWorld);
    }

    for (u32 index = 0; index < g_NearWaitCount; index++)
    {
        g_NearEnded = g_NearEnded || NearWaitPasses(g_NearWaits[index]);
    }

    g_Passing = 0;
    return g_PassedEvents == 0 && !g_NearEnded;
}

// The agents' states as one word: what each runner's levels run (but a level going round an idle loop), its packet (but an idle
// loop's) and its starter's receivers
u32 AgentsSignature()
{
    u32 signature = 2166136261u;
    for (u32 index = 0; index < g_AgentCount; index++)
    {
        ObjectNode* node = ObjectNodeOf(g_Agents[index]);
        if (node == nullptr)
        {
            continue;
        }

        for (BehaviourRunner* runner : node->runners)
        {
            if (runner == nullptr)
            {
                continue;
            }

            for (u32 at = 0; at < BehaviourRunner::Levels; at++)
            {
                BehaviourLevel* level = runner->levels[at];
                GraphState* state = level != nullptr && !LevelIdles(node->owner, runner, at) ? level->state : nullptr;
                signature = (signature ^ reinterpret_cast<u32>(state)) * 16777619u;
            }

            ControlPacket* packet = runner->packet != nullptr && !PacketIdles(runner, node) ? runner->packet : nullptr;
            signature = (signature ^ reinterpret_cast<u32>(packet) ^ reinterpret_cast<u32>(runner->receivers)) * 16777619u;
        }
    }

    return signature;
}

// A frame of the skip: the agents' soonest timed wait found, and the watched frames before the one it ends at passed (PassFrame:
// every agent's time moved on a clock step each -- the step of the clock of the agent that waits -- and their own updates for
// it), so it ends on the next frame and the later ones that much nearer: the waits end one after another in the order and at the
// frames of their agents' own time they end at when the cutscene is watched, and the agents are where those frames leave them
void OrderWaits()
{
    s32 soonest = NoWait;
    s32 step = 0;
    GatherNearWaits();
    g_WorldDriven = AgentsDriveWorld();
    // A frame in which an agent's script changed state is followed by the frame that acts on it (the new state's entry, an agent
    // that waits on another's behaviour): the agents' time is not moved on past it
    u32 signature = AgentsSignature();
    bool changed = signature != g_Signature;
    g_Signature = signature;
    if (changed && g_HeldFrames < HeldSkipFrames)
    {
        g_HeldFrames++;
        g_StalledFrames = 0;
        return;
    }

    for (u32 index = 0; index < g_AgentCount; index++)
    {
        auto* instance = const_cast<InstanceContext*>(g_Agents[index]);
        ObjectNode* node = ObjectNodeOf(instance);
        if (node == nullptr)
        {
            continue;
        }

        s32 now = static_cast<s32>(GetContextClock(instance)->time);
        for (BehaviourRunner* runner : node->runners)
        {
            s32 wait = RunnerWait(runner, node, now);
            if (wait == WaitStarting && g_HeldFrames < HeldSkipFrames)
            {
                // its length is known once it begins: counted on the next frame
                g_HeldFrames++;
                return;
            }

            if (wait < soonest)
            {
                soonest = wait;
                step = static_cast<s32>(GetContextClock(instance)->advance);
            }
        }
    }

    // A near wait that passes now is the next frame's (nothing passes before it); one pending passes the frames while its body
    // moves, at most up to the soonest timed wait's
    bool near = false;
    bool pending = false;
    s32 nearStep = 0;
    for (u32 index = 0; index < g_NearWaitCount; index++)
    {
        near |= NearWaitPasses(g_NearWaits[index]);
        if (NearWaitPending(g_NearWaits[index]))
        {
            pending = true;
            nearStep = static_cast<s32>(GetContextClock(g_NearWaits[index].focus)->advance);
        }
    }

    pending = pending && !near;
    if (pending && (soonest == NoWait || step <= 0))
    {
        step = nearStep;
        soonest = (MostPassedFrames + 1) * step;
    }

    g_OrderedWaits++;
    g_StalledFrames = soonest == NoWait && !changed ? g_StalledFrames + 1 : 0;
    // A wait that ends on the next frame (or no step to count frames by) moves nothing
    if (soonest == NoWait || step <= 0 || soonest <= step || near)
    {
        return;
    }

    // The watched frames before the one the soonest wait ends at, passed (at most MostPassedFrames a game frame; the next frames
    // pass the rest); one in which a message trigger was entered or left holds the waits for the answer, one that brought a near
    // wait's focus near ends them
    s32 frames = (soonest + step - 1) / step - 1;
    GatherPassedNodes();
    g_NearEnded = false;
    for (s32 frame = 0; frame < frames && frame < MostPassedFrames; frame++)
    {
        if (!PassFrame(step))
        {
            if (g_PassedEvents != 0 && g_HeldFrames < HeldSkipFrames)
            {
                g_Hold = HoldFrames;
            }

            return;
        }
    }
}

bool CutsceneState(u32 state)
{
    return state == GameController::StateWatching || state == GameController::StatePaused || state == GameController::StateWaiting ||
           state == GameController::StateMovie || state == GameController::StateStartingPlay;
}
}

void StepCutsceneSkip(u32 state, u32 next)
{
    g_Frame++;
    bool inCutscene = CutsceneState(state) || CutsceneState(next);
    if (g_Skipping != 0)
    {
        g_SkipFrames++;
        TrackLevels();
        if (g_Hold != 0)
        {
            g_Hold--;
            g_HeldFrames++;
        }

        if (g_Hold == 0 && inCutscene)
        {
            OrderWaits();
        }

        if (!inCutscene)
        {
            EndCutsceneSkip();
        }

        return;
    }

    if (g_CutsceneOn != 0 && !inCutscene)
    {
        LogAgents("watched");
        ForgetCutsceneAgents();
    }
}
