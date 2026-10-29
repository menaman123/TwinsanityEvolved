// runner/te_disc_log.cpp -- see te_disc_log.h.
#include "te_disc_log.h"

#include "app/host/disc_regions.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace te::discs {
extern const char* const kRetailLists[];   // CMakeLists.txt: launcher/discs/*.json as raw string literals
}

namespace te::disc {

void RegisterBuiltInTables() {
    static bool done = false;
    if (done) return;
    done = true;
    for (const char* const* list = te::discs::kRetailLists; *list; ++list) {
        const std::string kind = ps2r::app::DiscTableKind(*list);
        std::string error;
        if (kind == "region-map") {   // an address map for another release's program: read only for a disc of its serial
            if (!ps2r::app::AddRegionMapText(*list)) std::fprintf(stderr, "[twinsanityevolved] disc: a built-in region map has no serial\n");
            continue;
        }
        if (!ps2r::app::RegisterDiscTable(*list, &error))
            std::fprintf(stderr, "[twinsanityevolved] disc: a built-in disc table does not read: %s\n", error.c_str());
    }
}

bool VerifyRequested(bool verify_flag) {
    const char* v = std::getenv("TE_DISC_VERIFY");
    return verify_flag || (v && std::strcmp(v, "0") != 0 && *v);
}

bool LogIdentity(const std::string& iso, ps2r::app::DiscIdentity* out, bool verify) {
    RegisterBuiltInTables();
    ps2r::app::IdentifyOptions options;
    options.hash_below = kBootHashBytes + 1;
    options.hash_image = verify;
    ps2r::app::DiscIdentity id;
    std::string error;
    const bool accepted = ps2r::app::IdentifyDiscWith(iso, options, &id, &error);
    std::string tail;
    if (id.image_hashed) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "; image crc32 %08x sha1 %s (%.2f GB hashed in %.1f s)", id.image_crc32, id.image_sha1.c_str(),
                      double(id.image_bytes) / 1e9, id.image_seconds);
        tail = buf;
    }
    if (!accepted) tail += " -- " + error;
    else if (!id.warning.empty()) tail += " -- " + id.warning;
    std::fprintf(stderr, "[twinsanityevolved] disc: %s%s\n", ps2r::app::DescribeDisc(id).c_str(), tail.c_str());
    if (out) *out = id;
    return accepted;
}

}  // namespace te::disc
