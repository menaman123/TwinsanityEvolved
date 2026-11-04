// platform/nativeee/host -- te_cutscene_skip.h: the scripted cutscenes the cross button may skip (Platform::System::
// CutsceneSkippable, game/cutsceneskip.h). The table is made at build time from the cutscene skip judge's fixture
// (fixtures/judges/cutscene_skip_pal.json, by tools/cutscenes/skip_table.py): each cutscene of the disc by its site (the graph
// that runs its CutsceneStart, as tools/cutscenes/census.py finds them) and the graphs only it runs, whether a skip of it was
// verified to leave what watching it leaves (the judge's watched and skipped legs), and in plain words why not.
#pragma once

#include <cstdint>

namespace te::cutscene_skip {

struct Graph {
    uint16_t graph;      // a graph the cutscene's director runs
    uint16_t site;       // the cutscene it belongs to (its site's graph)
    uint8_t skippable;   // 1: its skip leaves the watched state
    const char* why;     // why it cannot be skipped ("" when it can)
};
extern const Graph kGraphs[];
extern const unsigned kGraphCount;

// The table's row of a graph (nullptr: no cutscene of the table runs it)
const Graph* Find(unsigned graph);
// The answer for a graph: 0 when its cutscene cannot be skipped, else 1 (a graph the table does not name: 1, the skip as it is).
// TE_CUTSCENE_SKIP_ALL=1 answers 1 for every graph: the skip without the table, as tools/cutscenes/verify.py measures it
unsigned Skippable(unsigned graph);

}  // namespace te::cutscene_skip
