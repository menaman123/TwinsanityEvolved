// runner/te_region.cpp -- see te_region.h.
#include "te_region.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "te_disc_log.h"
#include "te_mods.h"

namespace te::region {

namespace {
void Register(const std::string& path, std::vector<std::string>* origins) {
    ps2r::app::RegionMap m;
    std::string error;
    if (!ps2r::app::LoadRegionMap(path, &m, &error)) {
        std::fprintf(stderr, "[twinsanityevolved] region: the map %s does not read: %s\n", path.c_str(), error.c_str());
        return;
    }
    ps2r::app::AddRegionMap(m);
    if (origins) origins->push_back(path);
}
}  // namespace

Selection Select(const ps2r::app::DiscIdentity& id, bool identified, const std::string& map_arg, const std::string& game_folder) {
    Selection s;
    te::disc::RegisterBuiltInTables();   // the built-in maps with the other tables
    s.serial = id.serial;
    s.executable = id.executable;
    s.identified = identified;
    s.entry = identified ? ps2r::app::RegionEntryFor(id.serial) : nullptr;
    // another release's disc: its built-in maps read now (before the named ones, which replace a built-in one of the same
    // executable); a PAL disc reads none
    if (s.entry && !s.entry->native) ps2r::app::ParsePendingRegionMaps(s.entry->serial);
    if (!map_arg.empty()) Register(map_arg, &s.maps);
    if (!s.entry) return s;   // no disc, or a program the table does not know: the PAL layout, as before
    s.default_hz = s.entry->default_hz;
    s.offers_50 = s.entry->offers_50;
    if (s.entry->native) return s;
    s.native = false;
    if (!game_folder.empty() && !s.entry->map_file.empty()) {
        std::error_code ec;
        const std::filesystem::path beside = std::filesystem::path(game_folder) / s.entry->map_file;
        const bool named = !map_arg.empty() && std::filesystem::equivalent(beside, map_arg, ec);   // --region-map named this one
        if (!named && std::filesystem::is_regular_file(beside, ec)) Register(beside.generic_string(), &s.maps);
    }
    for (const ps2r::app::RegionMap& m : ps2r::app::RegionMaps())
        if (m.serial == s.entry->serial && m.origin.empty()) s.maps.push_back("built in");
    if (!ps2r::app::HasRegionMap(s.entry->serial)) {
        s.refused = true;
        s.message = ps2r::app::RegionRefusal(*s.entry);
    }
    return s;
}

std::string Describe(const Selection& s) {
    if (!s.entry) {
        if (!s.identified) return "region: no disc identified -- loaded as the PAL layout";
        return "region: " + (s.executable.empty() ? std::string("no disc program named") : s.executable + " is no release the table knows") +
               " -- loaded as the PAL layout";
    }
    std::string out = std::string("region: ") + ps2r::app::RegionName(s.entry->region) + " " + s.entry->serial + ", ";
    if (s.native) out += "the decompilation's own layout" + (s.maps.empty() ? std::string() : " (address maps registered: " + std::to_string(s.maps.size()) + ")");
    else if (s.refused) out += "needs an address map (" + s.entry->map_file + "): none";
    else {
        out += "through an address map (";
        for (size_t i = 0; i < s.maps.size(); ++i) out += (i ? ", " : "") + s.maps[i];
        out += ")";
    }
    out += ", " + std::to_string(s.default_hz) + " Hz" + (s.offers_50 ? " (50 offered)" : " (50 not offered)");
    return out;
}

std::string PrepareDisc(const Selection& s) {
    if (!s.entry || s.native || s.refused) return {};
    std::vector<ps2r::app::RegionMap::Name> names;
    for (const ps2r::app::RegionMap& m : ps2r::app::RegionMaps())
        if (m.serial == s.entry->serial)
            for (const auto& n : m.names)
                if (std::none_of(names.begin(), names.end(), [&](const auto& x) { return x.pal == n.pal && x.other == n.other; })) names.push_back(n);
    if (names.empty()) return {};
    const std::string layer = "region:" + s.entry->serial;
    te::mods::SetRegionFiles(layer, {{"CRASH6/CRASH.BH", [names, layer](const std::vector<uint8_t>& below, std::vector<uint8_t>* out, std::string* why) {
        std::vector<std::string> renamed;
        if (!ps2r::app::RenameArchiveEntries(below, names, out, &renamed, why)) return false;
        for (const std::string& r : renamed)
            std::fprintf(stderr, "[twinsanityevolved] region: the archive entry %s (the name the European program asks for)\n", r.c_str());
        if (renamed.empty()) std::fprintf(stderr, "[twinsanityevolved] region: no archive entry to rename (%s)\n", layer.c_str());
        return true;
    }}});
    std::string list;
    for (const auto& n : names) list += (list.empty() ? "" : ", ") + n.other + " -> " + n.pal;
    return "region: the European program's file names over this disc's archive index: " + list;
}

bool LoadImage(w2c_env& env, const std::vector<uint8_t>& elf, const Selection& s, const RetailImage& image, std::string* error) {
    // a map made for exactly this executable (its sha256) lays it out, whatever the row -- a relocated PAL executable too;
    // with no map registered (every PAL disc today) nothing is hashed and the native path runs exactly as before
    const ps2r::app::RegionMap* map = nullptr;
    std::string sha;
    if (!ps2r::app::RegionMaps().empty()) {
        sha = ps2r::app::Sha256OfBytes(elf);
        map = ps2r::app::FindRegionMap(s.serial, sha);
    }
    if (!map && s.native) return te::LoadRetailImage(env, elf, image, error);
    if (!map) {
        if (error)
            *error = "This disc's program (" + s.executable + ", sha256 " + sha.substr(0, 16) + "...) is not the one the port's address map for " +
                     (s.entry ? s.entry->disc_words : std::string("this disc")) + " was made from: another version or a modified program needs its "
                     "own map (twinsanityevolved-modtool region-map --pal <the European disc> --disc <this disc>, put beside the game).";
        return false;
    }
    if (map->image_start != image.start || map->bss_start != image.bss_start || map->image_end != image.end) {
        if (error) *error = "the address map lays out another PAL image than this build's";
        return false;
    }
    ps2r::app::MapLoadReport r;
    if (!ps2r::app::LoadImageThroughMap(elf, *map, env.memory.data, env.memory.size, &r, error)) return false;
    std::fprintf(stderr, "[twinsanityevolved] region: %s laid out at the PAL addresses through %s: %u segments, %llu bytes, %u pointer words "
                         "(%u code, %u data, %u by range, %u unmapped)%s\n",
                 s.executable.c_str(), map->origin.empty() ? "the built-in map" : map->origin.c_str(), r.segments,
                 (unsigned long long)r.bytes_copied, r.pointers, r.by_code, r.by_data, r.by_range, r.unmapped,
                 map->fills.empty() ? "" : (", " + std::to_string(r.fills) + " PAL words kept").c_str());
    return true;
}

}  // namespace te::region
