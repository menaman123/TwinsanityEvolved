#include "game/commands.h"

#include "game/agentparts.h"
#include "game/agents.h"
#include "game/animation.h"
#include "game/camerarig.h"
#include "game/characters.h"
#include "game/clock.h"
#include "game/controllers.h"
#include "game/cutscenereader.h"
#include "game/cutsceneskip.h"
#include "game/gamecontroller.h"
#include "game/language.h"
#include "game/layout.h"
#include "game/objectnode.h"
#include "game/objects.h"
#include "game/oleg.h"
#include "game/player.h"
#include "game/progress.h"
#include "game/string.h"
#include "game/widgets.h"
#include "platform/system.h"

#include <stdio.h>

// The commands that ask the game's controllers: the game controller's requests, the video controller, the music, the player, the
// custom pickups and projectiles

extern "C"
{
    // The playable character's fall reset
    void ResetCharacterFall(Agent* character) RETAIL(FUN_0013f790);
    // The empty string, and the flag SetScriptGlobalFlag sets for condition 629 (nothing clears it)
    extern const char g_EmptyText[] RETAIL(D_003098D8);
    extern s32 g_ScriptGlobalFlag RETAIL(D_003098E8);
}

namespace
{
s32 ClockUnits(f32 seconds)
{
    return static_cast<s32>(seconds * g_ClockUnitsPerSecond);
}

// The object of the node it takes its object from when there's one
GameObject* ObjectOf(ObjectNode* node)
{
    return node->sourceNode != nullptr ? SourceObject(node->sourceNode) : node->object;
}
}


void EndContextMusicCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    FadeOutMusicSlot(fadeSeconds, ContextMusicSlot);
}

void FadeOutMusicSlotCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    FadeOutMusicSlot(fadeSeconds, static_cast<s32>(slot.index));
}

void StartObjectVideoCommand::Execute(TimeClock* clock, BehaviourRunner*, BehaviourLevel*)
{
    StartReadCutscene(G_VideoController, clock);
}

void CancelVideoCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    StopScriptMovie(G_VideoController);
}

void StartQueuedVideoCommand::Execute(TimeClock* clock, BehaviourRunner*, BehaviourLevel*)
{
    StartQueuedMovie(G_VideoController, clock);
}

void StopVideoCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_VideoController->Reset();
}

void ControllerRumbleCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    VibrateForShake(strength);
}

void RestartFromCheckpointCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->RestartFromCheckpoint();
}

void UnlinkCharactersCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    InstanceContext* instance = runner->agentNode->owner;
    if (instance != nullptr && G_GameController != nullptr)
    {
        G_GameController->UnlinkCharacter(instance);
    }
}

// The watching screen shown over half the seconds given (and hidden the same way). TwinsanityEvolved: the runner's agent is the
// cutscene's director, and the graphs it runs say whether the cutscene may be skipped (game/cutsceneskip.h)
void CutsceneStartCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    u32 state = G_GameController->State();
    if (state == GameController::StatePlaying)
    {
        ForgetCutsceneAgents();
    }

    G_GameController->StartCutscene(static_cast<s32>(seconds * 0.5f * g_ClockUnitsPerSecond));
    if (state == GameController::StatePlaying || state == GameController::StateWatching)
    {
        NoteCutsceneDirector(runner->agentNode->owner);
        NoteCutsceneSite(runner);
    }
}

void CutsceneEndCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->EndCutscene(static_cast<s32>(seconds * 0.5f * g_ClockUnitsPerSecond));
}

void ProgressWhackawormCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->progress.AddToCount(countChange);
}

void EndWhackawormCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->EndWhackaworm();
}

void ResetCharacterFallCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    auto* node = static_cast<AgentNode*>(GetGameNode(&runner->agentNode->owner->nodes, NodeCharacter));
    if (node != nullptr)
    {
        ResetCharacterFall(node->agent);
    }
}

void ClearBottomTextCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    StringAssign(&G_GameController->oleg.textLine.text, g_EmptyText);
}

void EnablePlayerControlCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->EnableCharacters(0);
}

void DisablePlayerControlCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->DisablePlayerControl(0);
}

void ForceGameOverCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->ForceGameOver();
}

void ShowBottomTextCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->ShowBottomText(ClockUnits(seconds));
}

void HideBottomTextCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->HideBottomText(ClockUnits(seconds));
}

void SetScriptGlobalFlagCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    g_ScriptGlobalFlag = 1;
}

void AddAmmoCommand::ExecuteOn(GameNode*)
{
    Gun* gun = g_PlayerCharacter->gun;
    if (gun != nullptr)
    {
        gun->AddAmmo(ammo);
    }
}

void DamageBossCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->progress.AddHealth(healthChange);
}

void ExitBossModeCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->ExitBossMode();
}

void PlayCreditsCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    G_GameController->PlayCredits();
}

// The cutscene of the number queued for the node's instance (the object of the node it takes its object from when there's one)
void QueueObjectVideoCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    auto* node = static_cast<ObjectNode*>(runner->agentNode);
    InstanceContext* instance = node->owner;
    GameObject* object = ObjectOf(node);
    QueueObjectMovie(G_VideoController, object, instance, cutscene);
}

// TwinsanityEvolved: a skipped cutscene's speech track not queued (game/cutsceneskip.h; CutsceneMusicReady answers ready, the
// track's StartQueuedVideo finds nothing asked)
void QueueVideoCommand::Execute(TimeClock* clock, BehaviourRunner* runner, BehaviourLevel*)
{
    if (CutsceneSkips(runner->agentNode) != 0)
    {
        return;
    }

    s32 trackNumber = track.IntWith(static_cast<ObjectNode*>(runner->agentNode)->PacketProperties());
    QueueMovie(G_VideoController, trackNumber, clock, flags.loops);
}

// The pitch scale and volume of the node's tracked sound (when the node takes packets)
void SetSoundParamsCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    auto* node = static_cast<ObjectNode*>(runner->agentNode);
    if (CallVirtual<u32>(node, node->vtable, ObjectNode::TakesPacketsSlot) == 0)
    {
        return;
    }

    u8 voice = node->trackedSound;
    if (sets.pitch != 0)
    {
        SetInstanceSoundPitch(pitch, static_cast<s32>(voice));
    }

    if (sets.volume != 0)
    {
        SetInstanceSoundVolume(volume, static_cast<s32>(voice));
    }
}

extern "C"
{
    // A colour made of three channels, alpha 1 (the floats after it)
    void MakeColour(u32* colour, f32 red, f32 green, f32 blue) RETAIL_N32(FUN_0011bea0);
}


namespace
{
constexpr s32 NoModelSlot = -1;

// The bottom text line: a text, a place, a colour, the scale 0.75 and how long it stays (seconds)
void ShowTextLine(const char* text, f32 x, f32 y, f32 red, f32 green, f32 blue, f32 seconds)
{
    constexpr f32 Scale = 0.75f;
    TextLine* line = &G_GameController->oleg.textLine;
    u32 colour;
    MakeColour(&colour, red, green, blue);
    StringAssign(&line->text, text);
    line->duration = ClockUnits(seconds);
    line->colour = colour;
    line->place.x = x;
    line->place.y = y;
    line->scale.x = Scale;
    line->scale.y = Scale;
}

// A script's text by its number: a line of the scripts' text file in force (Language\AgentLab\<language>.txt; the bottom
// texts the commands read at D_00309B14 are g_Texts[AgentLabTexts]). TwinsanityEvolved: retail read the line table unchecked,
// so a number past the file's lines took the words after the table as a text: on the PS2 a stray address (the game stops),
// here an out-of-bounds trap. A mod's level scripts can number texts the disc's text file does not have -- a crate made for
// the North American disc numbers the lines its American.txt adds, which the European program never reads -- so past the
// end the text is empty and the log names the number. Every retail number is within the file (the signs' 32..99 of 100)
const char* ScriptText(u32 text)
{
    const u32 lines = g_TextLineCounts[AgentLabTexts];
    if (text < lines)
    {
        return g_Texts[AgentLabTexts][text];
    }

    printf("[te.mods] tolerated: a script shows text %u, the scripts' text file has %u lines: shown empty\n",
           static_cast<unsigned>(text), static_cast<unsigned>(lines));
    return g_EmptyText;
}

// The model of an object's slot (none for NoModelSlot)
u16 SlotModel(GameObject* object, s32 slot)
{
    u16 model;
    SetUndefinedId(&model);
    if (slot != NoModelSlot)
    {
        u16 found;
        GetObjectModelId(&found, object, static_cast<u32>(slot));
        model = found;
    }

    return model;
}
}

// Lives given (taken only by the played character's own scripts)
void AddLivesCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    GameController* controller = G_GameController;
    InstanceContext* owner = runner->agentNode->owner;
    InstanceContext* played = controller->progress.Instance(controller->progress.play.character);
    if (lives > 0 || owner == played)
    {
        // TwinsanityEvolved: with the platform's infinite lives on, a life taken is not taken -- the lives are shown as
        // they are, and the checkpoint restart that follows a death never finds them gone (RestartFromCheckpoint)
        s32 given = lives;
        if (given < 0 && Platform::System::InfiniteLives() != 0)
        {
            given = 0;
        }

        controller->oleg.AddLives(given, 0);
    }
}

// The vehicle gauge's left icon the model of the object's slot (the object of the node it takes its object from when there's one)
void SetGaugeIconCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    u16 model;
    GetObjectModelId(&model, ObjectOf(static_cast<ObjectNode*>(runner->agentNode)), slot.index);
    if (model != NoModelId)
    {
        u16 icon = model;
        G_GameController->oleg.SetHudIcon(OLEG::HudIconGauge, &icon);
    }
}

// The story raised to an area (AreaFromValue: the tagged area's), never lowered
void RaiseStoryAreaCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    constexpr s32 AreaFromValue = -1;
    GameProgress* progress = &G_GameController->progress;
    s32 raised = area;
    if (raised == AreaFromValue)
    {
        raised = taggedArea.IntWith(static_cast<ObjectNode*>(runner->agentNode)->PacketProperties());
    }

    if (static_cast<s32>(progress->play.story) < raised)
    {
        progress->play.story = static_cast<u32>(raised);
    }
}

void DisplayBottomTextCommand::Execute(TimeClock*, BehaviourRunner*, BehaviourLevel*)
{
    ShowTextLine(ScriptText(static_cast<u32>(text)), x, y, red, green, blue, seconds);
}

// The text of the instance's first integer property (to 10 the texts from 30, past it the texts from 52)
void DisplayBottomTextInstanceCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    constexpr u32 LastLowNumber = 10;
    constexpr u32 LowTextOffset = 30;
    constexpr u32 HighTextOffset = 41;
    u32 number = static_cast<u32>(static_cast<ObjectNode*>(runner->agentNode)->properties->GetInt(0));
    u32 index = number > LastLowNumber ? number + HighTextOffset : number + LowTextOffset;
    ShowTextLine(ScriptText(index), x, y, red, green, blue, seconds);
}

// The boss bar's length and health, its icon the model of the object's slot (the object of the node it takes its object from
// when there's one)
void EnableBossModeCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    auto* node = static_cast<ObjectNode*>(runner->agentNode);
    GameObject* object = ObjectOf(node);
    s32 most = health.IntWith(node->PacketProperties());
    u16 icon = SlotModel(object, iconSlot);
    G_GameController->EnableBossMode(barLength, static_cast<u32>(most), &icon);
}

// Whack-a-worm's time (seconds) and count, its icon the model of the object's slot
void StartWhackawormCommand::Execute(TimeClock*, BehaviourRunner* runner, BehaviourLevel*)
{
    auto* node = static_cast<ObjectNode*>(runner->agentNode);
    GameObject* object = ObjectOf(node);
    PropertyHolder* properties = node->PacketProperties();
    s32 count = total.IntWith(properties);
    s32 time = ClockUnits(seconds.FloatWith(properties));
    u16 icon = SlotModel(object, iconSlot);
    G_GameController->StartWhackaworm(time, static_cast<u32>(count), &icon);
}

namespace
{
// The HUD's gem and crystal widget: how long it appears and holds
constexpr f32 GemShowSeconds = Rounded(0.3);
constexpr f32 GemHoldSeconds = 1.5f;

// The progress of the level play is in (its word)
u32* CurrentLevel(GameProgress* progress)
{
    return &progress->levels[g_AreaLevels[progress->play.area]].value;
}

void ShowGemWidget(OLEG* oleg)
{
    oleg->Show(oleg->screens[OLEG::ScreenPickup], ClockUnits(GemShowSeconds), ClockUnits(GemHoldSeconds));
    oleg->PlayPickupEffect(OLEG::ScreenPickup);
}
}

// The gem of the level play is in found, shown on the HUD in its colour
void AddGemCommand::ExecuteOn(GameNode*)
{
    GameController* controller = G_GameController;
    OLEG* oleg = &controller->oleg;
    MarkGem(reinterpret_cast<u8*>(CurrentLevel(&controller->progress)), static_cast<u32>(gem));
    s32 found = gem;
    oleg->pickupSprite = static_cast<u8>(found + OLEG::SpriteGems);
    oleg->pickupGem.gem = found;
    ShowGemWidget(oleg);
}

// The crystal of the level play is in found, shown on the HUD
void AddCrystalCommand::ExecuteOn(GameNode*)
{
    GameController* controller = G_GameController;
    OLEG* oleg = &controller->oleg;
    MarkCrystal(CurrentLevel(&controller->progress));
    oleg->pickupSprite = OLEG::SpritePickupCrystal;
    ShowGemWidget(oleg);
}

// The played character's part gets a hit point, at most 3: a fourth makes it invincible
void PickUpHealthCommand::ExecuteOn(GameNode*)
{
    constexpr u32 MostHitPoints = 3;
    // CharacterAgent::StartInvincibility's kind of 8 seconds
    constexpr u32 LongInvincibility = 1;
    GameProgress* progress = &G_GameController->progress;
    InstanceContext* played = progress->Instance(progress->play.character);
    if (played == nullptr)
    {
        return;
    }

    auto* agent = static_cast<CharacterAgent*>(static_cast<AgentNode*>(GetGameNode(&played->nodes, NodeCharacter))->agent);
    auto* part = static_cast<CreaturePart*>(agent->part);
    u32 points = part->flags.hitPoints + 1;
    if (points > MostHitPoints)
    {
        agent->StartInvincibility(LongInvincibility);
        points = MostHitPoints;
    }

    part->flags.hitPoints = points;
}

// The wumpa fruit to add to the count; an instance with a crate's node (but no pickup node) loses as many of its crate's wumpa
// fruit (none left at least)
void PickUpWumpaCommand::ExecuteOn(GameNode* node)
{
    NodeList* nodes = &node->owner->nodes;
    OLEG* oleg = &G_GameController->oleg;
    if (GetGameNode(nodes, NodePickup) == nullptr)
    {
        auto* crate = static_cast<AgentNode*>(GetGameNode(nodes, NodeCrate));
        if (crate != nullptr)
        {
            auto* part = static_cast<CratePart*>(crate->agent->part);
            u32 crateWumpa = part->crate.wumpaFruit;
            u32 left = crateWumpa < static_cast<u32>(wumpaFruit) ? 0 : crateWumpa - wumpaFruit;
            part->crate.wumpaFruit = left;
        }
    }

    oleg->wumpaToAdd = static_cast<u8>(oleg->wumpaToAdd + wumpaFruit);
}

namespace
{
u32 CustomSlotOf(GameNode* node)
{
    return CallVirtual<u32>(node, node->vtable, ObjectNode::CustomSlotSlot);
}
}

// The node's custom pickup: its radius, its focus's pull on it, the speed it flees at and the flags' bits 0-10
void SetCustomPickupCommand::ExecuteOn(GameNode* node)
{
    CustomPickup* pickup = g_CustomPickups[CustomSlotOf(node)];
    pickup->flags.value = (pickup->flags.value & ~CustomPickupFlags::CommandBits) | (flags.value & CustomPickupFlags::CommandBits);
    pickup->radius = radius;
    pickup->radiusSquared = radius * radius;
    pickup->pull = pull;
    pickup->fleeSpeed = fleeSpeed;
}

// The node's custom projectile: its speed, and what the settings give
void SetCustomProjectileCommand::ExecuteOn(GameNode* node)
{
    CustomProjectile* projectile = g_CustomCommandSlots.projectiles[CustomSlotOf(node)];
    projectile->speed = speed;
    if (settings.falls != 0)
    {
        projectile->gravity = gravity.FloatWith(static_cast<ObjectNode*>(node)->PacketProperties());
        projectile->flags.falls = 1;
    }

    if (settings.homes != 0)
    {
        projectile->turn = turn;
        projectile->sideTurnScale = sideTurnScale;
        projectile->flags.homes = 1;
    }

    if (settings.homesForATime != 0)
    {
        projectile->homingTime = homingTime;
        projectile->flags.homesForATime = 1;
    }

    if (settings.playersShot != 0)
    {
        projectile->flags.playersShot = 1;
    }
}
