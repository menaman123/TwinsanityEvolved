// platform/nativeee/host -- cutscene_skip.cpp: see te_cutscene_skip.h. Platform::System::CutsceneSkippable answered from the
// table made of the cutscene skip judge's fixture; a refused cutscene is named in the log once a run.
#include "te_cutscene_skip.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "ps2e_seam.h"

namespace te::cutscene_skip {

namespace {
bool SkipAll() {
    const char* v = std::getenv("TE_CUTSCENE_SKIP_ALL");
    return v != nullptr && std::strcmp(v, "1") == 0;
}
}  // namespace

const Graph* Find(unsigned graph) {
    for (unsigned i = 0; i < kGraphCount; ++i)
        if (kGraphs[i].graph == graph) return &kGraphs[i];
    return nullptr;
}

unsigned Skippable(unsigned graph) {
    if (SkipAll()) return 1;
    const Graph* row = Find(graph);
    return row == nullptr || row->skippable ? 1 : 0;
}

}  // namespace te::cutscene_skip

extern "C" ps2e_s32 ps2e_system_cutscene_skippable(ps2e_s32 graph) {
    const unsigned id = static_cast<unsigned>(graph);
    const unsigned skippable = te::cutscene_skip::Skippable(id);
    if (!skippable) {
        static std::set<unsigned> told;
        const te::cutscene_skip::Graph* row = te::cutscene_skip::Find(id);
        if (row != nullptr && told.insert(row->site).second)
            std::fprintf(stderr, "[te.cutscene] the cutscene of site %u cannot be skipped: %s\n", row->site, row->why);
    }
    return static_cast<ps2e_s32>(skippable);
}
