// mods_disc.cpp -- te_mods.h MakeDiscVolume: the volume te::Disc opens. No folder mod with a disc/ tree on: a plain IsoFs
// (the disc layer exactly as without mods -- no overlay object, nothing on the read path). Otherwise the image with the
// enabled mods' disc/ trees composed over it in mods.order (engine/cdvd/disc_overlay.h), which at its open logs what it
// composed (one line per layer refused, one per disc file), reports to te::mods::NoteOverlay (the census row
// mods=<layers>:<overrides>:<hits>) and prints that row every TE_GFX_SUMMARY_EVERY boundaries (default 600; 0 = never):
// the presenter's own summary line belongs to the renderer, so the row rides beside it on its own line.
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "engine/cdvd/disc_overlay.h"
#include "engine/platform/host/platform.h"
#include "te_mods.h"
#include "te_services.h"

namespace te::mods {

namespace {

std::string g_region_layer;
std::vector<std::pair<std::string, ComputeFile>> g_region_files;

bool IsRegionLayer(const std::string& id) { return !g_region_layer.empty() && id == g_region_layer; }

long SummaryEvery() {
    const char* v = std::getenv("TE_GFX_SUMMARY_EVERY");
    return v ? std::strtol(v, nullptr, 10) : 600;
}

class ModsVolume final : public DiscOverlay {
public:
    explicit ModsVolume(std::vector<Layer> layers) : DiscOverlay(std::move(layers)) {}
    ~ModsVolume() override { NoteOverlay({}, 0, nullptr); }   // the census forgets a volume that is gone (a test's reset)

    bool Open(const std::string& path, std::string* error) override {
        const uint64_t open_start = ps2r::host::MonotonicNowNs();
        const bool ok = DiscOverlay::Open(path, error);
        if (!ok) return false;
        const uint64_t open_ns = ps2r::host::MonotonicNowNs() - open_start;
        const Report& r = GetReport();
        for (const std::string& why : r.refused) {
            // "<id>: <the file>: <why>": a mod made for another region's disc says so first (the base check's own reason after)
            Info info;
            const size_t colon = why.find(": ");
            if (colon != std::string::npos && FindInfo(why.substr(0, colon), &info) && OtherRegion(info.base.region))
                std::fprintf(stderr, "[twinsanityevolved] mods: the disc overlay left out %s: it was made for %s, and this is %s (%s)\n",
                             why.substr(0, colon).c_str(), RegionDisc(info.base.region).c_str(), RegionDisc(DiscRegion()).c_str(),
                             why.substr(colon + 2).c_str());
            else
                std::fprintf(stderr, "[twinsanityevolved] mods: the disc overlay left out %s\n", why.c_str());
        }
        for (const std::string& line : r.files) std::fprintf(stderr, "[twinsanityevolved] mods: disc %s\n", line.c_str());
        std::string applied;
        for (const std::string& id : r.applied) applied += (applied.empty() ? "" : ",") + id;
        std::fprintf(stderr, "[twinsanityevolved] mods: disc overlay over %s: layers %s, %u files replaced or added (%u in the virtual region), "
                     "image %llu bytes (the disc's %llu); composed in %.1f ms (base checks: %u in %.1f ms)\n", path.c_str(),
                     applied.empty() ? "none" : applied.c_str(), r.overrides, r.relocated, (unsigned long long)ImageBytes(),
                     (unsigned long long)uint64_t(VolumeSectors()) * IsoFs::kSectorSize, double(open_ns) / 1e6, r.base_checks,
                     double(r.base_check_ns) / 1e6);
        const DiscOverlay* self = this;
        std::vector<std::string> mods_applied;   // the census counts the mods, not the region loader's layer
        for (const std::string& id : r.applied)
            if (!IsRegionLayer(id)) mods_applied.push_back(id);
        uint32_t overrides = r.overrides;
        if (!g_region_layer.empty()) {   // the files some mod touched (a line ends ", by <layer>+<layer>...")
            overrides = 0;
            for (const std::string& line : r.files) {
                const size_t by = line.rfind(", by ");
                if (by == std::string::npos) continue;
                std::string ids = line.substr(by + 5);
                bool mod = false;
                for (size_t at = 0; at <= ids.size();) {
                    size_t plus = ids.find('+', at);
                    if (plus == std::string::npos) plus = ids.size();
                    if (!IsRegionLayer(ids.substr(at, plus - at))) mod = true;
                    at = plus + 1;
                }
                if (mod) ++overrides;
            }
        }
        if (mods_applied.empty()) NoteOverlay({}, 0, nullptr);   // the region loader's layer alone: no mod
        else NoteOverlay(mods_applied, overrides, [self] { return self->Hits(); });
        const long every = SummaryEvery();
        if (every > 0 && !hooked_) {
            hooked_ = true;
            Services::Get().clock().AddBoundaryHook("mods census", [every](uint64_t boundary) {
                if (boundary > 0 && boundary % uint64_t(every) == 0)
                    std::fprintf(stderr, "[te.mods] %s (boundary %llu)\n", CensusRow().c_str(), (unsigned long long)boundary);
            });
        }
        return true;
    }

private:
    bool hooked_ = false;
};

}  // namespace

void SetRegionFiles(std::string layer_id, std::vector<std::pair<std::string, ComputeFile>> files) {
    g_region_layer = files.empty() ? std::string() : std::move(layer_id);
    g_region_files = std::move(files);
}

std::unique_ptr<IsoFs> MakeDiscVolume() {
    const std::vector<DiscLayer> layers = DiscLayers();
    if (layers.empty() && g_region_files.empty()) return std::unique_ptr<IsoFs>(new IsoFs);
    std::vector<DiscOverlay::Layer> composed;
    for (const DiscLayer& l : layers) {
        DiscOverlay::Layer layer{l.id, l.dir, {}};
        // what the layer's recipes were made against (mod.json files[].base_*): the overlay refuses the layer over another base
        Info info;
        if (FindInfo(l.id, &info))
            for (const FileRow& row : info.files)
                if (row.form == "ranges" && row.base_bytes) {
                    DiscOverlay::BaseCheck check;
                    check.path = row.path;
                    check.bytes = row.base_bytes;
                    check.sha256 = row.base_sha256;
                    check.sample_sha256 = row.base_sample_sha256;
                    check.sample_windows = row.base_sample_windows;
                    check.sample_window_bytes = row.base_sample_window_bytes;
                    layer.checks.push_back(check);
                }
        composed.push_back(std::move(layer));
    }
    if (!g_region_files.empty()) {   // last: over the disc as the mods made it
        DiscOverlay::Layer region{g_region_layer, std::string(), {}};
        for (const auto& f : g_region_files) region.computed.push_back({f.first, f.second});
        composed.push_back(std::move(region));
    }
    return std::unique_ptr<IsoFs>(new ModsVolume(std::move(composed)));
}

}  // namespace te::mods
