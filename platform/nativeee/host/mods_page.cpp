// platform/nativeee (host side) -- te_mods_page.h: the Options > MODS page's rows -- the mods that apply while the game runs
// (the built-ins). A mod of the mods folder is switched in the launcher and is never on this page.
#include "te_mods_page.h"

#include <vector>

#include "te_mods.h"

namespace te::mods::page {

namespace {

// The page's rows: the registry's live mods (the built-ins, registered first), in registration order. Asked a few dozen times
// while the Options page is built; the registry is a handful of entries
std::vector<uint32_t> Rows() {
    std::vector<uint32_t> rows;
    for (uint32_t i = 0; i < te::mods::Count(); ++i)
        if (AppliesLive(i)) rows.push_back(i);
    return rows;
}

}  // namespace

uint32_t Count() { return uint32_t(Rows().size()); }

uint32_t Entry(uint32_t row) {
    const std::vector<uint32_t> rows = Rows();
    return row < rows.size() ? rows[row] : kNoMod;
}

bool Shown(uint32_t row) { return Enabled(Entry(row)); }

void Choose(uint32_t row, bool on) {
    const uint32_t index = Entry(row);
    if (index != kNoMod) SetEnabled(index, on, true);   // a built-in: at once, persisted -- as before
}

std::string Label(uint32_t row) {
    const Mod* m = At(Entry(row));
    return m ? m->name : std::string();
}

}  // namespace te::mods::page
