// te_test_requests.h -- what a TEST asks of the game while it plays (Platform::System::NextTestRequest; game/testrequests.h
// carries each out through the game's own code at the start of the next frame of play). Off unless a test asks: the release game
// never queues one. Two ways in, both test tools:
//   the autopilot's plan steps (runner/te_autopilot.cpp): teleport, message, counter -- each queued at the boundary it runs at;
//   TE_TEST_ACTIONS=SPEC@BOUNDARY[;SPEC@BOUNDARY...] -- each queued at the first boundary of play from BOUNDARY on (a fixture's
//     replay of what its recording's plan asked: tools/cutscenes/reach.py writes the recording's "[te.test]" lines as this).
// SPEC (the plan steps' words):
//   teleport X Y Z [NAME]   the instance (NAME: an object's name, its whole name or the part after the last '|'; none or
//                           "player": the played character) warped to (X, Y, Z) in its chunk's frame, as PositionWarp warps one
//   message NAME ID [FROM]  the user message ID sent to NAME's instance from FROM's (none: no sender), as SendUserMessage sends one
//   counter N V [NAME]      the game counter N (0..999) set to V, as SetCounter sets one; with NAME that instance's own counter N
//                           (0..3), as SetCounter sets an agent's (a boss's count of what is left of a phase)
// Every request is logged when the game takes it: "[te.test] b BOUNDARY: SPEC" (the boundary it was asked at: the replay's) and
// what it found ("[te.test]   ...").
#pragma once
#include <cstdint>
#include <string>

namespace te::test_requests {

struct Request {
    unsigned kind = 0;        // Platform::System::TestRequestKind: 1 teleport, 2 message, 3 counter, 4 an instance's counter
    std::string instance;     // the instance's name ("" = the player)
    std::string other;        // the message's sender ("" = none)
    unsigned counter = 0;
    int32_t value = 0;
    float x = 0, y = 0, z = 0;
};

// SPEC parsed (the words above); false with the reason
bool Parse(const std::string& spec, Request* out, std::string* error);
// The request written back as its SPEC
std::string Spec(const Request& request);
// Queued: the game takes it at the start of its next frame of play
void Ask(const Request& request, uint64_t boundary);
// TE_TEST_ACTIONS
bool InstallFromSpec(const char* spec, std::string* error);
// The instance of a loaded chunk whose object has the name (its address in the game's memory, 0 = none): the runner's census
// of the loaded chunks answers it (runner/te_cutscenes.cpp)
using Finder = uint32_t (*)(const std::string& name);
void SetFinder(Finder finder);

}  // namespace te::test_requests
