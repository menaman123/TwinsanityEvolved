// te_cutscenes.h -- the in-game cutscenes' diagnostics over guest memory (read-only; nothing here is reachable from the game):
//   TE_CUTSCENE_CENSUS=1  the behaviour scripts the game has read (its own parser's graphs: states, bodies, conditions,
//                         commands by vtable) and which instances of which chunk run them, as JSON lines on stderr
//                         ("[cs.graph] {...}", "[cs.starter] {...}", "[cs.chunk] {...}"), each script once per run, logged
//                         whenever the scripts' table settles after a load. tools/cutscenes/census.py turns the lines of
//                         every level's run into the census of the cutscenes. Ids, token types, counts and names only.
//   TE_CUTSCENE_STATE=N[@FROM-TO]  every N boundaries (within FROM..TO when given) while the game plays or watches, and at each
//                         Watching / StartingPlay / Playing change, one
//                         "[cs.state] {...}" line: the game state, the player's place and chunk, the loaded chunks' object
//                         instances (their places, flags and running states) and the chunks' saved flags -- what
//                         tools/cutscenes/compare.py compares between a watched and a skipped run.
//   TE_CUTSCENE_START     a test's start of a cutscene (InstallStart below).
#pragma once
#include <cstdint>
#include <string>

namespace te::cutscenes {

void InstallCensus();
void InstallState(const char* spec);
// TE_CUTSCENE_START=NAME[:MESSAGE]@BOUNDARY (a test's way into a cutscene without walking to its trigger): at the first Playing
// boundary from BOUNDARY on, the loaded instance whose object is named NAME is given the behaviour its trigger behaviour for
// MESSAGE (87 by default: what a trigger volume sends the player in) starts -- queued on its runner as ObjectNodeBase::
// OnTriggerMessage queues it (BehaviourRunner::QueueStarter, forced; the player its originator). One line says what was done
void InstallStart(const char* spec);
// TE_CUTSCENE_LINKS=B[,B...]: at each boundary given, one "[cs.links] {...}" line per loaded chunk: its object instances (name,
// place, the properties' state bits, the instances their attachments link) and its message triggers (place, bits, the four
// messages, the instances told) -- who can start a cutscene's director. Names, places and ids only
void InstallLinks(const char* spec);
// TE_CUTSCENE_STARTS=1: one "[cs.starts]" line whenever an object instance's runner starts another graph, with the runner's
// originator (what sent the message or started it): the chain of who starts a cutscene's director
void InstallStarts();

// The awake instance of a loaded chunk whose object is named `name` (its whole name or the part after the last '|'; the first in
// the chunks' order): its address in the game's memory, 0 when none (TE_CUTSCENE_START's lookup; the test requests' names)
uint32_t FindInstance(const std::string& name);
// Whether that instance's runners run `graph` at a level, at its state `state` (-1: any state): the autopilot's until-graph
bool RunsGraph(const std::string& name, int graph, int state);

}  // namespace te::cutscenes
