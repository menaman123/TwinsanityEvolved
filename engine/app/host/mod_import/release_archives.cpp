// mod_internal.h KnownArchives / RegionDisc: the releases' archive indexes built into the importer (launcher/discs/archives/
// *.json, embedded by mod_import.cmake as string pieces: kArchiveTables), read once.
#include <mutex>

#include "app/host/mod_import/json.h"
#include "app/host/mod_import/mod_internal.h"

namespace te::modimport::detail {

// mod_import.cmake: one array of string pieces per table (each piece under any compiler's literal limit), nullptr-ended
extern const char* const* const kArchiveTables[];

const std::vector<ReleaseArchive>& KnownArchives() {
    static std::vector<ReleaseArchive> tables;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const char* const* const* t = kArchiveTables; *t; ++t) {
            std::string text;
            for (const char* const* piece = *t; *piece; ++piece) text += *piece;
            json::Value v;
            if (!json::Parse(text, &v, nullptr) || !v.IsObject() || v.String("kind") != "archive-index") continue;
            ReleaseArchive r;
            r.serial = v.String("serial");
            r.executable = v.String("executable");
            r.region = v.String("region");
            r.version = v.String("version");
            r.provenance = v.String("provenance");
            const json::Value* retail = v.Get("retail");
            r.retail = retail && retail->type == json::Value::Type::Bool && retail->boolean;
            if (const json::Value* e = v.Get("entries"); e && e->IsArray())
                for (const json::Value& row : e->items)
                    if (row.IsArray() && row.items.size() == 2 && row.items[0].IsString() && row.items[1].IsNumber())
                        r.entries[ArchiveKey(row.items[0].text)] = {row.items[0].text, uint32_t(row.items[1].number)};
            if (!r.serial.empty() && !r.entries.empty()) tables.push_back(std::move(r));
        }
    });
    return tables;
}

std::string RegionDisc(const std::string& region) {
    if (region == "PAL") return "the European disc";
    if (region == "NTSC-U") return "the North American disc";
    if (region == "NTSC-J") return "the Japanese disc";
    return "another disc";
}

}  // namespace te::modimport::detail
