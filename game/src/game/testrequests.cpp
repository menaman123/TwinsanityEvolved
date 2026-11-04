#include "game/testrequests.h"

#include "game/agents.h"
#include "game/chunkloading.h"
#include "game/cutsceneskip.h"
#include "game/events.h"
#include "game/instances.h"
#include "game/memory.h"
#include "game/objectnode.h"
#include "game/place.h"
#include "game/progress.h"
#include "game/reference.h"
#include "game/vehicles.h"
#include "platform/system.h"

// TwinsanityEvolved: a test's requests carried out (game/testrequests.h)

extern "C"
{
    // The game's counters (the chunk manager's), which SetCounter sets
    extern void* G_ChunkManager;
}

namespace
{

// Warped as PositionWarp's WarpTo warps an instance (game/src/game/commandsfocus.cpp): its place's position taken from the matrix
// first when that moved, moved there (the instance queued for its collision), the position kept as where its node's frame started.
// A character on a vehicle (the Humiliskate's board: the vehicle moves its riders from its own matrix) has the vehicle started
// again at the new place, as the vehicle starts when it is made
void Warp(InstanceContext* instance, const Vector4& position)
{
    ObjectPlace* place = instance->place;
    place->SyncPosition();
    if (place->MoveTo(&position))
    {
        QueueObject(instance);
    }

    auto* node = static_cast<ObjectNode*>(GetGameNode(&instance->nodes, NodeObject));
    if (node != nullptr)
    {
        node->frameStart = position;
    }

    auto* character = static_cast<AgentNode*>(GetGameNode(&instance->nodes, NodeCharacter));
    auto* agent = character != nullptr ? static_cast<CharacterAgent*>(character->agent) : nullptr;
    if (agent != nullptr && agent->vehicle != nullptr)
    {
        agent->vehicle->StartVirtual();
    }
}

// Sent as SendUserMessage sends one (game/src/game/commandsattach.cpp MakeMessage and SendMessage): the message with a reference
// to its sender as its argument, queued for the target's object node
void Send(u32 message, InstanceContext* sender, InstanceContext* target)
{
    Reference* argument = sender != nullptr ? AddReference(sender) : nullptr;
    GameEvent* event =
        GameEvent::Construct(static_cast<GameEvent*>(MemoryAllocate(sizeof(GameEvent))), message, &argument, ObjectNodeKinds);
    if (sender != nullptr)
    {
        NoteCutsceneAgent(sender, target);
    }

    Reference* handle = event != nullptr ? AddEventReference(event) : nullptr;
    QueueEvent(target, &handle);
}

}

void ApplyTestRequests(GameProgress* progress)
{
    Platform::System::TestRequest request;
    while (Platform::System::NextTestRequest(&request) != 0)
    {
        InstanceContext* instance = request.instance != 0 ? reinterpret_cast<InstanceContext*>(request.instance)
                                                           : progress->Instance(progress->play.character);
        auto* other = reinterpret_cast<InstanceContext*>(request.other);
        switch (request.kind)
        {
        case Platform::System::TestWarp:
            if (instance != nullptr)
            {
                Warp(instance, Vector4{request.x, request.y, request.z, 1.0f});
            }

            break;
        case Platform::System::TestMessage:
            if (instance != nullptr)
            {
                Send(static_cast<u32>(request.value), other, instance);
            }

            break;
        case Platform::System::TestCounter:
            SetGameCounter(G_ChunkManager, request.counter, request.value);
            break;
        case Platform::System::TestAgentCounter:
        {
            // As SetCounter sets an agent's (game/src/game/commandsfocus.cpp CounterOf: the agent's counters)
            auto* node = instance != nullptr ? static_cast<ObjectNode*>(GetGameNode(&instance->nodes, NodeObject)) : nullptr;
            Agent* agent = node != nullptr ? node->agent : nullptr;
            if (agent != nullptr && request.counter < Agent::Counters)
            {
                agent->counters[request.counter] = static_cast<u8>(request.value);
            }

            break;
        }
        default:
            break;
        }
    }
}
