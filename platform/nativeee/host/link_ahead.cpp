// platform/nativeee (host side) -- te_link_ahead.h: the archive's index, and the linked chunks read ahead. See the header.
#include "te_link_ahead.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "disc_reader.h"
#include "engine/cdvd/iso_fs.h"
#include "te_host.h"
#include "te_services.h"

namespace te::archive {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Index {
    const IsoFs* iso = nullptr;   // the volume it was read from (a services reset opens another)
    bool ok = false;
    std::map<std::string, Entry> by_name;   // lower case
    std::vector<Entry> by_offset;           // sorted by their first byte
};

// CRASH6\CRASH.BH: a 32-bit header word, then per entry its name's length, the name, its offset in CRASH.BD and its size
// (tools/discs/archive_index.py reads it the same way). Read once per volume, on the game thread.
const Index& Get() {
    static Index index;
    te::Disc& disc = te::Services::Get().disc();
    disc.Initialise();
    if (!disc.IsOpen()) return index;
    const IsoFs* iso = &disc.Iso();
    if (index.iso == iso) return index;
    index = Index{};
    index.iso = iso;
    IsoFs::Entry bh, bd;
    if (!iso->Find("CRASH6/CRASH.BH", bh) || !iso->Find("CRASH6/CRASH.BD", bd) || bh.directory || bd.directory || bh.size < 4 ||
        bh.size > (1u << 20))
        return index;
    std::vector<uint8_t> table(bh.size);
    if (iso->ReadBytes(uint64_t(bh.lba) * IsoFs::kSectorSize, bh.size, table.data()) != bh.size) return index;
    const uint64_t data = uint64_t(bd.lba) * IsoFs::kSectorSize;
    auto u32 = [&table](size_t at) { uint32_t v; std::memcpy(&v, table.data() + at, 4); return v; };
    for (size_t at = 4; at + 4 <= table.size();) {
        const uint32_t k = u32(at);
        if (k == 0 || k > 512 || at + 12 + k > table.size()) return index;   // not this game's index: none
        std::string name(reinterpret_cast<const char*>(table.data() + at + 4), k);
        while (!name.empty() && name.back() == '\0') name.pop_back();
        const uint32_t offset = u32(at + 4 + k), size = u32(at + 8 + k);
        if (uint64_t(offset) + size > bd.size) return index;
        const Entry e{data + offset, size};
        index.by_name[Lower(name)] = e;
        index.by_offset.push_back(e);
        at += 12 + k;
    }
    std::sort(index.by_offset.begin(), index.by_offset.end(), [](const Entry& a, const Entry& b) { return a.image < b.image; });
    index.ok = !index.by_offset.empty();
    return index;
}

}  // namespace

bool Find(const std::string& name, Entry& out) {
    const Index& index = Get();
    if (!index.ok) return false;
    const auto it = index.by_name.find(Lower(name));
    if (it == index.by_name.end()) return false;
    out = it->second;
    return true;
}

bool EntryAt(uint64_t image_byte, Entry& out) {
    const Index& index = Get();
    if (!index.ok) return false;
    auto it = std::upper_bound(index.by_offset.begin(), index.by_offset.end(), image_byte,
                               [](uint64_t b, const Entry& e) { return b < e.image; });
    if (it == index.by_offset.begin()) return false;
    --it;
    if (image_byte >= it->image + it->size) return false;
    out = *it;
    return true;
}

}  // namespace te::archive

namespace te::link_ahead {
namespace {

// The game's structures (game/include/game/chunkloading.h, chunkdata.h; the PAL executable's global, game/symbol_addrs.txt)
constexpr uint32_t kChunkLoadingManager = 0x00309AD8;   // G_ChunkLoadingManager_: ChunkLoadingManager*
constexpr uint32_t kManagerLoaders = 0x18, kManagerFocusLoader = 0x1C;
constexpr uint32_t kLoaderPath = 0x0C, kLoaderRm2 = 0x20, kLoaderSm2 = 0x24, kLoaderLinks = 0x28, kLoaderNext = 0x2C;
constexpr uint32_t kFileLoaderState = 0x10;   // ChunkLoaderBase::state
constexpr uint32_t kLinkPath = 0x04, kLinkLoader = 0x10, kLinkFlags = 0x20, kLinkNext = 0xC4;   // GameChunkLink (data at 0x20)
constexpr uint32_t kLinkKeepShift = 7, kLinkKeepMask = 0x7F;   // ChunkLinkFlags::keep: the link takes the chunk's RM2 along
enum FileState : uint32_t { kQueued = 1, kWaitingToLoad = 2, kLoading = 3, kLoaded = 4, kWaitingToUnload = 5, kUnloading = 6 };
constexpr int kMaxLinks = 32;
constexpr int kLogChunks = 40;

uint32_t Rd32(uint32_t address) {
    const uint8_t* p = te::GuestPointer(address, 4);
    if (!p) return 0;
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// A String {char* string; s32 length; s32 capacity} at `field`
std::string RdString(uint32_t field) {
    const uint32_t ptr = Rd32(field), length = Rd32(field + 4);
    if (!ptr || length == 0 || length > 255) return std::string();
    const uint8_t* p = te::GuestPointer(ptr, length);
    return p ? std::string(reinterpret_cast<const char*>(p), length) : std::string();
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int FileState(uint32_t file_loader) { return file_loader ? int(Rd32(file_loader + kFileLoaderState)) : -1; }
bool Has(int state) { return state == kLoaded || state == kWaitingToUnload || state == kUnloading; }

struct State {
    std::string last;           // the last boundary's wanted set (what was acted on)
    std::string focus;          // the focus chunk's path (the log lines')
    std::vector<std::string> keep;
    int logged = 0;
} g;

void OnBoundary(uint64_t boundary) {
    if (!te::io::Async() || te::io::AheadBudget() == 0) return;
    const uint32_t manager = Rd32(te::RetailAddress(kChunkLoadingManager));
    const uint32_t focus = manager ? Rd32(manager + kManagerFocusLoader) : 0;
    if (!focus) return;   // (a load between levels: what is read ahead stays until a focus says otherwise)
    struct Want {
        std::string path;
        bool rm2 = false;    // a keep link: the RM2 comes along
        bool wanted = false; // a link reaches it now (it is waiting its second)
    };
    std::vector<Want> wants;
    std::vector<std::string> keep;
    std::string signature;
    int n = 0;
    for (uint32_t link = Rd32(focus + kLoaderLinks); link && n < kMaxLinks; link = Rd32(link + kLinkNext), ++n) {
        const std::string path = Lower(RdString(link + kLinkPath));
        if (path.empty()) continue;
        const bool rm2 = ((Rd32(link + kLinkFlags) >> kLinkKeepShift) & kLinkKeepMask) != 0;
        const uint32_t reference = Rd32(link + kLinkLoader);
        uint32_t loader = reference ? Rd32(reference) : 0;   // LoaderReference::loader
        for (uint32_t l = Rd32(manager + kManagerLoaders), k = 0; !loader && l && k < 256; l = Rd32(l + kLoaderNext), ++k)
            if (Lower(RdString(l + kLoaderPath)) == path) loader = l;
        const int sm2_state = loader ? FileState(Rd32(loader + kLoaderSm2)) : -1;
        const int rm2_state = loader ? FileState(Rd32(loader + kLoaderRm2)) : -1;
        if (Has(sm2_state) && (!rm2 || Has(rm2_state))) continue;   // the game has it
        keep.push_back(path);
        if (sm2_state == kLoading || rm2_state == kLoading) continue;   // being read: its extent, if any, serves it
        Want w;
        w.path = path;
        w.rm2 = rm2;
        w.wanted = sm2_state == kWaitingToLoad || rm2_state == kWaitingToLoad;
        wants.push_back(w);
    }
    std::sort(keep.begin(), keep.end());
    for (const Want& w : wants) signature += w.path + (w.rm2 ? "+" : "-") + (w.wanted ? "!" : ".") + ";";
    signature += "|";
    for (const std::string& k : keep) signature += k + ";";
    if (signature == g.last) return;
    g.last = signature;
    const std::string focus_path = Lower(RdString(focus + kLoaderPath));
    // what no link leads to any more, or the game has, let go of
    if (keep != g.keep) {
        te::io::KeepAhead(te::io::Ahead::Links, keep);
        g.keep = keep;
    }
    // the wanted ones first, then the others
    std::stable_sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.wanted > b.wanted; });
    const IsoFs* iso = &te::Services::Get().disc().Iso();
    for (const Want& w : wants) {
        archive::Entry sm2, rm2;
        if (!archive::Find(w.path + ".sm2", sm2)) continue;
        uint64_t begin = sm2.image, end = sm2.image + sm2.size;
        const bool with_rm2 = w.rm2 && archive::Find(w.path + ".rm2", rm2) && rm2.image == end;   // (contiguous: the archive's order)
        if (with_rm2) end = rm2.image + rm2.size;
        if (te::io::AheadHas(iso, begin, end)) continue;
        const bool made = te::io::ReadAhead(iso, begin, end, te::io::Ahead::Links, w.path);
        if (g.logged < kLogChunks) {
            ++g.logged;
            std::fprintf(stderr, "[te.io] %s the linked chunk '%s' (%s, %.1f MB) at boundary %llu: '%s' links to it%s%s\n",
                         made ? "reading ahead" : "not reading ahead", w.path.c_str(), with_rm2 ? "SM2 + RM2" : "SM2", double(end - begin) / 1048576.0,
                         (unsigned long long)boundary, focus_path.c_str(), w.wanted ? " (wanted now)" : "",
                         made ? "" : " -- no room in the read-ahead budget");
        }
    }
}

// ---- TE_CHUNK_LOG=1: the chunk loaders' states, a line at each change ------------------------------------------------------
// Every loader of the manager's list: its SM2's and RM2's states (ChunkLoaderBase::state), the depths the links want it at
// (ChunkLoaderBits: depth, keepDepth), its chunk's state (ChunkFlags::state: hidden while its files are not all loaded),
// whether its scenery is in (ChunkData::scenery) and what the drawing made of it (game/src/game/chunkscenery.cpp DrawChunk:
// ChunkData::drawnFrame = g_RenderedFrames when the drawing reached it, ChunkFlags::drawn when something of its scenery was
// drawn) -- a rise said at once, a fall once it has held 30 boundaries (a chunk at the edge of the view flickers). What a
// picture shows of a linked chunk is what these say: a chunk seen through a doorway before its files are in has nothing to
// draw yet. Read-only, the game's own structures; keyed by the loader's address, so a boundary with no change allocates
// nothing. At most kChunkLogLines lines a run.
constexpr uint32_t kLoaderBits = 0x04;            // ChunkLoader::bits (depth : 4, keepDepth : 4)
constexpr uint32_t kSm2Data = 0x1C;               // Sm2Loader::data (ChunkDataReference*: its first word the ChunkData)
constexpr uint32_t kChunkDrawnFrame = 0x114, kChunkFlags = 0x120, kChunkScenery = 0x144;   // ChunkData's
constexpr uint32_t kFlagsStateShift = 18, kFlagsDrawnBit = 1u << 23;   // ChunkFlags::state (4 bits), ::drawn
constexpr uint32_t kRenderedFrames = 0x00309B68;  // g_RenderedFrames (D_00309B68)
constexpr int kChunkLogLines = 1000;
constexpr uint32_t kFallAfter = 30;
enum Drawing : int { kNotReached = 0, kNothingDrawn = 1, kDrawn = 2 };

struct LoaderRecord {
    uint32_t loader = 0, path = 0;   // the loader's address and its path's string pointer (another chunk at the address: new)
    int sm2 = -2, rm2 = -2;
    uint32_t depth = 0, keep = 0, state = 0;
    bool scenery = false;
    int drawing = kNotReached;       // as said
    uint32_t lower = 0;              // boundaries the drawing has been below what was said
    bool seen = false;               // in the list at this boundary
    bool said = false;               // logged (a loader whose files never left the queue is not: the links' walk makes and
                                     // drops those every frame for the chunks two links away)
    std::string name;                // its path as logged
};
struct ChunkLog {
    std::vector<LoaderRecord> loaders;
    uint32_t focus = 0;
    int lines = 0;
    bool capped = false;
} g_chunks;

const char* StateName(int state) {
    switch (state) {
        case -1: return "none";
        case 0: return "idle";
        case 1: return "queued";
        case 2: return "waiting to load";
        case 3: return "loading";
        case 4: return "loaded";
        case 5: return "waiting to unload";
        case 6: return "unloading";
        case 7: return "unloaded";
        default: return "?";
    }
}

bool ChunkLine() {
    if (g_chunks.lines < kChunkLogLines) {
        ++g_chunks.lines;
        return true;
    }
    if (!g_chunks.capped) {
        g_chunks.capped = true;
        std::fprintf(stderr, "[te.io] chunk loaders: %d lines logged, the rest not (TE_CHUNK_LOG)\n", kChunkLogLines);
    }
    return false;
}

void LogChunkStates(uint64_t boundary) {
    const uint32_t manager = Rd32(te::RetailAddress(kChunkLoadingManager));
    if (!manager) return;
    const uint32_t focus = Rd32(manager + kManagerFocusLoader);
    if (focus != g_chunks.focus) {
        g_chunks.focus = focus;
        if (ChunkLine())
            std::fprintf(stderr, "[te.io] chunk loaders @%llu: the focus is '%s'\n", (unsigned long long)boundary,
                         focus ? RdString(focus + kLoaderPath).c_str() : "(none)");
    }
    const uint32_t rendered = Rd32(te::RetailAddress(kRenderedFrames));
    for (LoaderRecord& r : g_chunks.loaders) r.seen = false;
    uint32_t k = 0;
    for (uint32_t l = Rd32(manager + kManagerLoaders); l && k < 256; l = Rd32(l + kLoaderNext), ++k) {
        LoaderRecord now;
        now.loader = l;
        now.path = Rd32(l + kLoaderPath);
        const uint32_t sm2 = Rd32(l + kLoaderSm2), rm2 = Rd32(l + kLoaderRm2);
        now.sm2 = FileState(sm2);
        now.rm2 = FileState(rm2);
        const uint32_t bits = Rd32(l + kLoaderBits);
        now.depth = bits & 0xF;
        now.keep = (bits >> 4) & 0xF;
        const uint32_t reference = sm2 ? Rd32(sm2 + kSm2Data) : 0;
        const uint32_t chunk = reference ? Rd32(reference) : 0;
        const uint32_t flags = chunk ? Rd32(chunk + kChunkFlags) : 0;
        now.state = (flags >> kFlagsStateShift) & 0xF;
        now.scenery = chunk && Rd32(chunk + kChunkScenery) != 0;
        const bool reached = now.scenery && rendered - Rd32(chunk + kChunkDrawnFrame) < 2;
        const int drawing = !reached ? kNotReached : (flags & kFlagsDrawnBit) ? kDrawn : kNothingDrawn;
        LoaderRecord* r = nullptr;
        for (LoaderRecord& e : g_chunks.loaders)
            if (e.loader == l && e.path == now.path) { r = &e; break; }
        const bool fresh = r == nullptr;
        if (fresh) {
            g_chunks.loaders.push_back(LoaderRecord{});
            r = &g_chunks.loaders.back();
            r->loader = l;
            r->path = now.path;
        }
        r->seen = true;
        r->lower = drawing < r->drawing ? r->lower + 1 : 0;
        now.drawing = drawing > r->drawing || r->lower >= kFallAfter ? drawing : r->drawing;
        if (!fresh && now.sm2 == r->sm2 && now.rm2 == r->rm2 && now.depth == r->depth && now.keep == r->keep &&
            now.state == r->state && now.scenery == r->scenery && now.drawing == r->drawing)
            continue;
        const bool moving = now.sm2 > int(kQueued) || now.rm2 > int(kQueued) || chunk != 0;
        r->sm2 = now.sm2;
        r->rm2 = now.rm2;
        r->depth = now.depth;
        r->keep = now.keep;
        r->state = now.state;
        r->scenery = now.scenery;
        r->drawing = now.drawing;
        r->lower = 0;
        if (!moving && !r->said) continue;
        if (!r->said) {
            r->said = true;
            r->name = RdString(l + kLoaderPath);
        }
        if (!ChunkLine()) continue;
        char depth[16], keep[16];
        if (now.depth == 0xF) std::snprintf(depth, sizeof depth, "-"); else std::snprintf(depth, sizeof depth, "%u", now.depth);
        if (now.keep == 0xF) std::snprintf(keep, sizeof keep, "-"); else std::snprintf(keep, sizeof keep, "%u", now.keep);
        static const char* const kStates[] = {"hidden", "shown", "releasing", "released"};
        static const char* const kDrawing[] = {"not reached by the drawing", "reached, nothing of it drawn", "drawn"};
        std::fprintf(stderr, "[te.io] chunk loaders @%llu: '%s' SM2 %s, RM2 %s; depth %s, keep depth %s; %s, scenery %s, %s\n",
                     (unsigned long long)boundary, r->name.c_str(), StateName(now.sm2), StateName(now.rm2), depth,
                     keep, chunk ? (now.state < 4 ? kStates[now.state] : "?") : "no chunk data", now.scenery ? "in" : "not in",
                     kDrawing[now.drawing]);
    }
    // the loaders gone from the list (destroyed: the chunk dropped)
    for (size_t i = 0; i < g_chunks.loaders.size();) {
        if (g_chunks.loaders[i].seen) { ++i; continue; }
        if (g_chunks.loaders[i].said && ChunkLine())
            std::fprintf(stderr, "[te.io] chunk loaders @%llu: '%s' gone (its loader destroyed)\n", (unsigned long long)boundary,
                         g_chunks.loaders[i].name.c_str());
        g_chunks.loaders.erase(g_chunks.loaders.begin() + long(i));
    }
}

}  // namespace

void Install() {
    g = State{};
    te::Services::Get().clock().AddBoundaryHook("linked chunks ahead", [](uint64_t boundary) { OnBoundary(boundary); });
    if (const char* v = std::getenv("TE_CHUNK_LOG"); v && v[0] == '1') {
        g_chunks = ChunkLog{};
        g_chunks.loaders.reserve(64);
        te::Services::Get().clock().AddBoundaryHook("chunk loaders' states", [](uint64_t boundary) { LogChunkStates(boundary); });
        std::fprintf(stderr, "[te.io] chunk loaders: a line at each change of a chunk's files, link depths, scenery and drawing "
                             "(TE_CHUNK_LOG)\n");
    }
}

}  // namespace te::link_ahead
