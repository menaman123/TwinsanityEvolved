// platform/nativeee (host side) -- te_mods.h: the built-in mod framework. The registry, the switches (ps2r.ini
// mods.<id>.enabled through the engine's SettingsFile), the hook dispatch (the frame boundary, level entry / exit from the
// player's chunk, the menu build, the card's save / load) and the seam's Platform::System::Mod* imports the title's
// Options > MODS page is built from (ten: the count, label, switch, has-switch, the page's rows / label / row label / chosen
// row, the warp target). The manifests (mods/<id>/mod.json) are embedded by CMake (TE_GEN/mods/mods_manifests.cpp)
// and checked against the registrations.
#include "te_mods.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/core/config/settings_file.h"
#include "engine/platform/host/platform.h"
#include "mods_json.h"
#include "ps2e_seam.h"
#include "te_harness.h"
#include "te_host.h"
#include "te_mods_page.h"
#include "te_services.h"

// Generated: the manifests (id, json) and their count
struct te_mod_manifest { const char* id; const char* json; };
extern "C" const te_mod_manifest te_mod_manifests[];
extern "C" const unsigned te_mod_manifest_count;

namespace te::mods {

namespace {

struct Entry {
    Mod mod;
    bool enabled = false;
    bool forced = false;   // --mod <id>: on for this run, never persisted
    bool folder = false;   // a folder mod (ScanFolder): `info` is its manifest
    Info info;
};
std::vector<Entry>& Entries() { static std::vector<Entry> e; return e; }
bool g_installed = false;
FrameContext g_last;
std::string g_last_chunk;
bool g_have_player = false;
// the folder mods: the folder read, the overlay order asked for (mods.order), what the disc composed
std::string g_folder;
std::vector<std::string> g_order;
std::vector<std::string> g_applied;
std::vector<Info> g_packs;   // the folder's LANGUAGE PACKS (kind language, by id): not mods -- no registry entry
uint32_t g_overrides = 0;
std::function<uint64_t()> g_hits;

// The player's chunk, read as the authoring probe reads it (runner/te_probe.cpp; PAL retail globals, game/symbol_addrs.txt):
// G_UnkPlayableCharObjInstCxt -> Agent::instance -> ReferencedObject::chunk -> ChunkData::path (a game String)
constexpr uint32_t kPlayerCharacter = 0x003098FC;
constexpr uint32_t kAgentInstance = 0x00, kObjectChunk = 0xA0, kChunkPath = 0x138;

uint32_t Rd32(uint32_t address) {
    const uint8_t* p = GuestPointer(address, 4);
    if (!p) return 0;
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::string PlayerChunk(bool* have_player) {
    *have_player = false;
    const uint32_t agent = Rd32(te::RetailAddress(kPlayerCharacter));
    if (!agent) return "";
    const uint32_t instance = Rd32(agent + kAgentInstance);
    if (!instance || !GuestPointer(instance, 0x160)) return "";
    *have_player = true;
    const uint32_t chunk = Rd32(instance + kObjectChunk);
    if (!chunk || !GuestPointer(chunk, 0x1F0)) return "";
    const uint32_t text = Rd32(chunk + kChunkPath), length = Rd32(chunk + kChunkPath + 4);
    if (!text || length == 0 || length > 256) return "";
    const uint8_t* p = GuestPointer(text, length);
    if (!p) return "";
    return std::string(reinterpret_cast<const char*>(p), length);
}

void Persist(const Entry& e) {
    const std::string& path = GetSettings().settings_path;
    if (path.empty()) return;
    std::string err;
    const std::string key = "mods." + e.mod.id + ".enabled";
    if (!ps2r::SettingsFile::Set(path, key, e.enabled ? "on" : "off", &err))
        std::fprintf(stderr, "[twinsanityevolved] mods: %s not written to %s: %s\n", key.c_str(), path.c_str(), err.c_str());
    DropPackKeys(path);   // a language pack's stale switch / order place go at the file's next write
}

// A mod id: the ps2r.ini key mods.<id>.enabled accepts [a-z0-9_]+ only (engine ModsConfig::ParseKey)
bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// A whole host file through the engine's descriptors (every target, the Vita's included); false when it does not read or
// is over `limit` bytes
bool ReadHostFile(const std::string& path, size_t limit, std::string& out) {
    const int fd = ps2r::host::FdOpen(path.c_str(), ps2r::host::kOpenRead);
    if (fd < 0) return false;
    const int64_t size = ps2r::host::FdSize(fd);
    bool ok = size >= 0 && uint64_t(size) <= limit;
    if (ok) {
        out.assign(size_t(size), '\0');
        size_t done = 0;
        while (done < out.size()) {
            const ps2r::host::SSize got = ps2r::host::FdPread(fd, &out[done], out.size() - done, done);
            if (got <= 0) break;
            done += size_t(got);
        }
        ok = done == out.size();
    }
    ps2r::host::FdClose(fd);
    return ok;
}

// A name the host's file manager leaves in folders (never a mod)
bool HostClutter(const std::string& name) {
    std::string l = name;
    for (char& c : l) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return name.empty() || name[0] == '.' || l == "thumbs.db" || l == "desktop.ini";
}

Kind KindOf(const std::string& name, bool* known) {
    *known = true;
    if (name == "disc") return Kind::Disc;
    if (name == "language") return Kind::Language;
    if (name == "builtin") return Kind::Builtin;
    *known = false;
    return Kind::Builtin;
}

// The manifest's fields into an Info (schema 1 or 2; unknown members are ignored: a newer writer's additions)
void FillInfo(const json::Value& m, Info& info) {
    info.name = m.String("name", info.id);
    info.author = m.String("author");
    info.version = m.String("version");
    info.description = m.String("description");
    info.schema = uint32_t(m.Unsigned("schema", 1));
    if (const json::Value* s = m.Get("source"); s && s->IsObject()) {
        info.source.kind = s->String("kind");
        info.source.artefact = s->String("artefact");
        info.source.sha256 = s->String("sha256");
    }
    if (const json::Value* b = m.Get("base"); b && b->IsObject()) {
        info.base.region = b->String("region");
        info.base.executable = b->String("executable");
    }
    if (const json::Value* files = m.Get("files"); files && files->IsArray())
        for (const json::Value& f : files->items)
            if (f.IsObject()) {
                FileRow row;
                row.path = f.String("path");
                row.form = f.String("form");
                row.bytes = f.Unsigned("bytes");
                row.base_bytes = f.Unsigned("base_bytes");
                row.base_sha256 = f.String("base_sha256");
                row.base_sample_sha256 = f.String("base_sample_sha256");
                row.base_sample_windows = uint32_t(f.Unsigned("base_sample_windows"));
                row.base_sample_window_bytes = f.Unsigned("base_sample_window_bytes");
                info.files.push_back(row);
            }
    if (const json::Value* l = m.Get("language"); l && l->IsObject()) {
        info.language.id = l->String("id");
        info.language.label = l->String("label");
        info.language.text_bank = l->String("text_bank");
        info.language.font = l->String("font");
        info.language.direction = l->String("direction");
    }
    if (const json::Value* x = m.Get("executable"); x && x->IsObject()) {
        if (const json::Value* d = x->Get("data_changes"); d && d->IsObject())
            for (const auto& kv : d->members)
                if (kv.second.IsNumber()) info.data_changes.emplace_back(kv.first, uint64_t(kv.second.number < 0 ? 0 : kv.second.number));
        if (const json::Value* c = x->Get("code_changes"); c && c->IsArray())
            for (const json::Value& v : c->items)
                if (v.IsString()) info.code_changes.push_back(v.text);
    }
}

// A relative path inside a mod's folder ('/' or '\'; no "..", no drive / device, not absolute)
bool SafeRelative(const std::string& rel, std::string* clean) {
    if (rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find(':') != std::string::npos) return false;
    std::string out, part;
    for (size_t i = 0; i <= rel.size(); ++i) {
        const char c = i < rel.size() ? rel[i] : '/';
        if (c == '/' || c == '\\') {
            if (part == "..") return false;
            if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + part;
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    if (out.empty()) return false;
    *clean = out;
    return true;
}

}  // namespace

const char* HookName(Hook hook) {
    static const char* const names[] = {"frame", "level_enter", "level_exit", "menu_build", "save", "load"};
    return uint32_t(hook) < uint32_t(Hook::Count) ? names[uint32_t(hook)] : "?";
}

void Register(const Mod& mod) {
    for (const Entry& e : Entries())
        if (e.mod.id == mod.id) { std::fprintf(stderr, "[twinsanityevolved] mods: %s registered twice\n", mod.id.c_str()); return; }
    Entry e;
    e.mod = mod;
    Entries().push_back(e);
}

uint32_t Count() { return uint32_t(Entries().size()); }
const Mod* At(uint32_t index) { return index < Entries().size() ? &Entries()[index].mod : nullptr; }
const Mod* Find(const std::string& id) {
    for (const Entry& e : Entries()) if (e.mod.id == id) return &e.mod;
    return nullptr;
}

std::string Manifest(const std::string& id) {
    for (unsigned i = 0; i < te_mod_manifest_count; ++i)
        if (id == te_mod_manifests[i].id) return te_mod_manifests[i].json;
    return "";
}

void Configure(const std::vector<std::string>& enabled_ids) {
    for (Entry& e : Entries()) {
        if (!e.mod.has_switch) continue;   // no switch: nothing persisted applies; the session's click turns it on
        const bool on = std::find(enabled_ids.begin(), enabled_ids.end(), e.mod.id) != enabled_ids.end();
        if (on && !e.enabled) {
            e.enabled = true;
            if (e.mod.on_enable) e.mod.on_enable(true);
        }
    }
}

bool Enabled(uint32_t index) { return index < Entries().size() && Entries()[index].enabled; }
bool Enabled(const std::string& id) {
    for (const Entry& e : Entries()) if (e.mod.id == id) return e.enabled;
    return false;
}

void SetEnabled(uint32_t index, bool on, bool persist) {
    if (index >= Entries().size()) return;
    Entry& e = Entries()[index];
    if (!e.mod.has_switch) persist = false;   // a switchless mod is on for the session only
    if (!persist) e.forced = on;
    if (e.enabled == on) { if (persist) Persist(e); return; }
    e.enabled = on;
    if (e.mod.on_enable) e.mod.on_enable(on);
    if (persist) Persist(e);
    std::fprintf(stderr, "[twinsanityevolved] mods: %s %s (%s)\n", e.mod.id.c_str(), on ? "ON" : "off", Describe().c_str());
}

bool AnyEnabled() {
    for (const Entry& e : Entries()) if (e.enabled) return true;
    return false;
}

bool AppliesLive(uint32_t index) { return index < Entries().size() && !Entries()[index].folder; }

std::string Describe() {
    std::string s = "mods:";
    if (Entries().empty()) return s + " none";
    for (const Entry& e : Entries()) {
        s += " " + e.mod.id + "=" + (e.enabled ? "on" : "off");
        if (e.forced) s += e.mod.has_switch ? "(--mod)" : "(session)";
    }
    s += AnyEnabled() ? "; saves -> the -mods card" : "; saves -> the vanilla card";
    return s;
}

const FrameContext& LastFrame() { return g_last; }

void OnBoundary(uint64_t boundary) {
    if (Entries().empty()) return;
    FrameContext ctx;
    ctx.boundary = boundary;
    unsigned state = 0, last = 0, next = 0;
    if (harness::ReadGameState(&state, &last, &next)) ctx.state = state;
    ctx.paused = ctx.state == 15;                       // StatePaused
    ctx.playing = ctx.state == 12 || ctx.state == 13;   // StatePlaying, StateWatching
    bool have_player = false;
    ctx.chunk = PlayerChunk(&have_player);
    // A level entered / left: the player's chunk path changed (the chunk name the trace logs). Leaving play (no player,
    // the title) leaves the last chunk.
    if (ctx.chunk != g_last_chunk) {
        // the area line (bin's session_timeline.py --areas keys a device log's windows by it): one per change
        std::fprintf(stderr, "[twinsanityevolved] area: '%s' @ boundary %llu (was '%s')\n", ctx.chunk.c_str(),
                     (unsigned long long)boundary, g_last_chunk.c_str());
        if (!g_last_chunk.empty())
            for (Entry& e : Entries()) if (e.enabled && e.mod.on_level_exit) e.mod.on_level_exit(ctx, g_last_chunk);
        if (!ctx.chunk.empty())
            for (Entry& e : Entries()) if (e.enabled && e.mod.on_level_enter) e.mod.on_level_enter(ctx, ctx.chunk);
        g_last_chunk = ctx.chunk;
    }
    g_have_player = have_player;
    for (Entry& e : Entries()) if (e.enabled && e.mod.on_frame) e.mod.on_frame(ctx);
    g_last = ctx;
}

void OnMenuBuild() {
    for (Entry& e : Entries()) if (e.enabled && e.mod.on_menu_build) e.mod.on_menu_build();
}

void OnSave(const std::string& save, const std::string& file) {
    for (Entry& e : Entries()) if (e.enabled && e.mod.on_save) e.mod.on_save(save, file);
}

void OnLoad(const std::string& save, const std::string& file) {
    for (Entry& e : Entries()) if (e.enabled && e.mod.on_load) e.mod.on_load(save, file);
}

// The built-in mods register themselves here (one function per mod source file)
void RegisterLevelSelect();
void RegisterSpeedrun();

void RegisterBuiltins() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    RegisterLevelSelect();
    RegisterSpeedrun();
}

void InstallBuiltins() {
    if (g_installed) return;
    g_installed = true;
    RegisterBuiltins();
    // Every registration has a manifest that agrees with it (the built-ins; a folder mod IS its manifest)
    for (const Entry& e : Entries()) {
        if (e.folder) continue;
        const std::string text = Manifest(e.mod.id);
        if (text.empty()) { std::fprintf(stderr, "[twinsanityevolved] mods: %s has no mods/%s/mod.json\n", e.mod.id.c_str(), e.mod.id.c_str()); continue; }
        json::Value m;
        std::string error;
        if (!json::Parse(text, m, &error) || !m.IsObject()) {
            std::fprintf(stderr, "[twinsanityevolved] mods: mods/%s/mod.json is not a JSON object (%s)\n", e.mod.id.c_str(), error.c_str());
            continue;
        }
        const std::string id = m.String("id"), name = m.String("name"), version = m.String("version");
        if (id != e.mod.id || name != e.mod.name || version != e.mod.version)
            std::fprintf(stderr, "[twinsanityevolved] mods: mods/%s/mod.json (%s / %s / %s) disagrees with the built-in (%s / %s / %s)\n",
                         e.mod.id.c_str(), id.c_str(), name.c_str(), version.c_str(), e.mod.id.c_str(), e.mod.name.c_str(), e.mod.version.c_str());
    }
    Services::Get().clock().AddBoundaryHook("mods", OnBoundary);
}

// ---- folder mods --------------------------------------------------------------------------------------------------------

const char* KindName(Kind kind) {
    switch (kind) {
        case Kind::Disc: return "disc";
        case Kind::Language: return "language";
        case Kind::Builtin: break;
    }
    return "builtin";
}

uint32_t ScanFolder(const std::string& mods_dir) {
    // a second scan (a test) forgets the previous folder's mods
    std::vector<Entry>& entries = Entries();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& e) { return e.folder; }), entries.end());
    g_packs.clear();
    g_folder = mods_dir;
    g_applied.clear();
    g_overrides = 0;
    g_hits = nullptr;
    if (mods_dir.empty() || !ps2r::host::IsDirectory(mods_dir.c_str())) return 0;
    std::vector<ps2r::host::DirectoryEntry> listing;
    if (!ps2r::host::ListDirectory(mods_dir.c_str(), &listing)) {
        std::fprintf(stderr, "[twinsanityevolved] mods: the mods folder %s does not list\n", mods_dir.c_str());
        return 0;
    }
    std::sort(listing.begin(), listing.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    uint32_t registered = 0;
    for (const auto& d : listing) {
        if (!d.directory || HostClutter(d.name)) continue;
        const std::string dir = mods_dir + "/" + d.name;
        auto skip = [&](const std::string& why) {
            std::fprintf(stderr, "[twinsanityevolved] mods: %s skipped: %s\n", dir.c_str(), why.c_str());
        };
        if (!ValidId(d.name)) { skip("a mod's folder is named by its id: lower-case letters, digits and _ only"); continue; }
        std::string text;
        if (!ReadHostFile(dir + "/mod.json", 1u << 20, text)) { skip("no mod.json (or one over 1 MB)"); continue; }
        json::Value m;
        std::string error;
        if (!json::Parse(text, m, &error)) { skip("mod.json: " + error); continue; }
        if (!m.IsObject()) { skip("mod.json is not a JSON object"); continue; }
        const uint64_t schema = m.Unsigned("schema", 1);
        if (schema != 1 && schema != 2) { skip("mod.json schema " + std::to_string(schema) + ": this game reads schemas 1 and 2"); continue; }
        const std::string id = m.String("id");
        if (id != d.name) { skip("mod.json names id '" + id + "': it must be the folder's name, '" + d.name + "'"); continue; }
        bool known = false;
        const Kind kind = KindOf(m.String("kind", schema == 1 ? "builtin" : ""), &known);
        if (!known) { skip("mod.json kind '" + m.String("kind") + "': disc, language or builtin"); continue; }
        if (Find(id)) {
            // a built-in's id: its manifest (the repo's mods/<id>/mod.json copied here) or a clash -- the built-in stays
            skip(kind == Kind::Builtin ? "the built-in " + id + "'s manifest: the built-in is compiled into the game"
                                       : "the id " + id + " is a built-in mod's");
            continue;
        }
        if (kind == Kind::Builtin) { skip("kind builtin: this game has no built-in mod '" + id + "'"); continue; }
        if (kind == Kind::Language) {
            // a language pack: offered as a text language (te_language_mod.h), never a mod -- no switch, no order, no card
            Info pack;
            pack.id = id;
            pack.kind = kind;
            pack.folder = true;
            pack.dir = dir;
            pack.manifest = text;
            FillInfo(m, pack);
            g_packs.push_back(pack);
            continue;
        }
        Entry e;
        e.folder = true;
        e.info.id = id;
        e.info.kind = kind;
        e.info.folder = true;
        e.info.dir = dir;
        e.info.manifest = text;
        FillInfo(m, e.info);
        e.info.has_disc = ps2r::host::IsDirectory((dir + "/disc").c_str());
        e.mod.id = id;
        e.mod.name = e.info.name;
        e.mod.version = e.info.version;
        e.mod.has_switch = true;   // mods.<id>.enabled, read at start-up: the launcher switches it (never the in-game page)
        entries.push_back(e);
        ++registered;
    }
    return registered;
}

const std::string& Folder() { return g_folder; }

void SetOrder(const std::string& order) {
    g_order.clear();
    std::string id;
    for (size_t i = 0; i <= order.size(); ++i) {
        const char c = i < order.size() ? order[i] : ',';
        if (c == ',' || c == ' ' || c == '\t' || c == ';') {
            if (!id.empty()) {
                const Mod* m = Find(id);
                bool folder = false;
                for (const Entry& e : Entries()) folder |= e.folder && e.mod.id == id;
                const bool pack = std::any_of(g_packs.begin(), g_packs.end(), [&](const Info& p) { return p.id == id; });
                if (pack)
                    std::fprintf(stderr, "[twinsanityevolved] mods: mods.order names '%s', a language pack (not a mod: it has no place in the order): ignored\n", id.c_str());
                else if (!m || !folder)
                    std::fprintf(stderr, "[twinsanityevolved] mods: mods.order names '%s', which is not a mod of the mods folder: ignored\n", id.c_str());
                else if (std::find(g_order.begin(), g_order.end(), id) == g_order.end())
                    g_order.push_back(id);
                id.clear();
            }
        } else {
            id.push_back(c);
        }
    }
}

std::vector<std::string> Order() {
    std::vector<std::string> out = g_order;
    std::vector<std::string> rest;
    for (const Entry& e : Entries())
        if (e.folder && std::find(out.begin(), out.end(), e.mod.id) == out.end()) rest.push_back(e.mod.id);
    std::sort(rest.begin(), rest.end());
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

std::vector<DiscLayer> DiscLayers() {
    std::vector<DiscLayer> out;
    for (const std::string& id : Order())
        for (const Entry& e : Entries())
            if (e.folder && e.mod.id == id && e.enabled && e.info.has_disc) out.push_back(DiscLayer{id, e.info.dir});
    return out;
}

void NoteOverlay(const std::vector<std::string>& applied, uint32_t overrides, std::function<uint64_t()> hits) {
    g_applied = applied;
    g_overrides = overrides;
    g_hits = std::move(hits);
}

std::vector<Info> List() {
    std::vector<Info> out;
    for (const Entry& e : Entries()) {
        Info info;
        if (e.folder) {
            info = e.info;
        } else {
            info.id = e.mod.id;
            info.name = e.mod.name;
            info.version = e.mod.version;
            info.kind = Kind::Builtin;
            json::Value m;
            const std::string text = Manifest(e.mod.id);
            if (!text.empty() && json::Parse(text, m, nullptr) && m.IsObject()) {
                FillInfo(m, info);
                info.name = e.mod.name;   // the registration's (checked against the manifest at InstallBuiltins)
                info.version = e.mod.version;
                info.manifest = text;
            }
        }
        info.enabled = e.enabled;
        info.forced = e.forced;
        info.has_switch = e.mod.has_switch;
        const auto at = std::find(g_applied.begin(), g_applied.end(), e.mod.id);
        info.layer = at == g_applied.end() ? -1 : int32_t(at - g_applied.begin());
        out.push_back(std::move(info));
    }
    // built-ins in registration order, then the folder mods by id
    std::stable_sort(out.begin(), out.end(), [](const Info& a, const Info& b) {
        if (a.folder != b.folder) return !a.folder;
        return a.folder && a.id < b.id;
    });
    return out;
}

const Info* FindInfo(const std::string& id, Info* storage) {
    if (!storage) return nullptr;
    for (const Info& info : List())
        if (info.id == id) { *storage = info; return storage; }
    return nullptr;
}

std::vector<Info> LanguagePacks() { return g_packs; }   // by id (the scan's order)

void DropPackKeys(const std::string& settings_path) {
    if (settings_path.empty()) return;
    const std::vector<Info> packs = LanguagePacks();
    if (packs.empty()) return;
    std::string err;
    for (const Info& p : packs)
        if (!ps2r::SettingsFile::Remove(settings_path, "mods." + p.id + ".enabled", &err))
            std::fprintf(stderr, "[twinsanityevolved] mods: %s: mods.%s.enabled not removed: %s\n", settings_path.c_str(), p.id.c_str(), err.c_str());
    std::string text, order;
    if (!ReadHostFile(settings_path, 1u << 20, text) || !ps2r::SettingsFile::GetFromText(text, "mods.order", &order)) return;
    std::string kept, id;
    bool dropped = false;
    for (size_t i = 0; i <= order.size(); ++i) {
        const char c = i < order.size() ? order[i] : ',';
        if (c == ',' || c == ' ' || c == '\t' || c == ';') {
            if (!id.empty()) {
                const bool pack = std::any_of(packs.begin(), packs.end(), [&](const Info& p) { return p.id == id; });
                if (pack) dropped = true;
                else kept += (kept.empty() ? "" : ",") + id;
                id.clear();
            }
        } else {
            id.push_back(c);
        }
    }
    if (!dropped) return;
    const bool ok = kept.empty() ? ps2r::SettingsFile::Remove(settings_path, "mods.order", &err)
                                 : ps2r::SettingsFile::Set(settings_path, "mods.order", kept, &err);
    if (!ok) std::fprintf(stderr, "[twinsanityevolved] mods: %s: mods.order not rewritten: %s\n", settings_path.c_str(), err.c_str());
}

namespace {
std::string g_disc_region;
}  // namespace

void SetDiscRegion(const std::string& region) { g_disc_region = region; }
const std::string& DiscRegion() { return g_disc_region; }

std::string RegionDisc(const std::string& region) {
    std::string r;
    for (char c : region) r.push_back(c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c);
    if (r == "PAL") return "the European disc";
    if (r == "NTSC-U") return "the North American disc";
    if (r == "NTSC-J") return "the Japanese disc";
    return "";
}

bool OtherRegion(const std::string& mod_region) {
    const std::string mine = RegionDisc(mod_region), disc = RegionDisc(g_disc_region);
    return !mine.empty() && !disc.empty() && mine != disc;
}

std::string DescribeFolders() {
    std::vector<const Entry*> folders;
    for (const Entry& e : Entries()) if (e.folder) folders.push_back(&e);
    std::sort(folders.begin(), folders.end(), [](const Entry* a, const Entry* b) { return a->mod.id < b->mod.id; });
    std::string s = "mods: " + std::to_string(folders.size()) + (folders.size() == 1 ? " folder" : " folders");
    if (!folders.empty()) {
        s += " (";
        for (size_t i = 0; i < folders.size(); ++i)
            s += (i ? ", " : "") + folders[i]->mod.id + " " + KindName(folders[i]->info.kind) + " " + (folders[i]->enabled ? "on" : "off");
        s += ")";
    }
    std::string order;
    for (const DiscLayer& l : DiscLayers()) order += (order.empty() ? "" : ",") + l.id;
    s += ", order: " + (order.empty() ? std::string("none") : order);
    if (!g_packs.empty()) {
        s += "; language packs (not mods):";
        for (const Info& p : g_packs) s += " " + p.id;
    }
    return s;
}

std::string CensusRow() {
    const uint64_t hits = g_hits ? g_hits() : 0;
    return "mods=" + std::to_string(g_applied.size()) + ":" + std::to_string(g_overrides) + ":" + std::to_string(hits);
}

bool ReadModFile(const std::string& id, const std::string& relpath, std::vector<uint8_t>& out, std::string* error) {
    out.clear();
    std::string dir;
    for (const Entry& e : Entries()) if (e.folder && e.mod.id == id) dir = e.info.dir;
    for (const Info& p : g_packs) if (p.id == id) dir = p.dir;
    if (dir.empty()) { if (error) *error = "no folder mod or language pack '" + id + "'"; return false; }
    std::string clean;
    if (!SafeRelative(relpath, &clean)) { if (error) *error = "'" + relpath + "' is not a path inside the mod's folder"; return false; }
    std::string text;
    const std::string path = dir + "/" + clean;
    if (!ReadHostFile(path, size_t(512) << 20, text)) { if (error) *error = "cannot read " + path; return false; }
    out.assign(text.begin(), text.end());
    return true;
}

}  // namespace te::mods

// ---- the seam: Platform::System::Mod* (include/platform/system.h; the generator maps the header's u32 to ps2e_s32) ------
// The seam's mod index is the PAGE's row (te_mods_page.h: the mods that apply while the game runs -- the built-ins); the page
// answers the switch a row shows and takes the player's choice. A folder mod is never a row: it is switched in the
// launcher, and nothing here writes its key.
namespace {
void CopyLabel(const std::string& label, ps2e_u32 out, ps2e_s32 size) {
    if (size <= 0) return;
    uint8_t* p = te::GuestPointer(out, size_t(size));
    if (!p) return;
    const size_t n = std::min(label.size(), size_t(size) - 1);
    std::memcpy(p, label.data(), n);
    p[n] = 0;
}
const te::mods::Mod* ModAt(ps2e_s32 mod) { return mod >= 0 ? te::mods::At(te::mods::page::Entry(uint32_t(mod))) : nullptr; }
te::mods::WarpTarget g_warp;
}  // namespace

extern "C" {

ps2e_s32 ps2e_system_mod_count(void) {
    te::mods::OnMenuBuild();
    return ps2e_s32(te::mods::page::Count());
}

void ps2e_system_mod_label(ps2e_s32 mod, ps2e_u32 out, ps2e_s32 size) {
    CopyLabel(ModAt(mod) ? te::mods::page::Label(uint32_t(mod)) : "", out, size);
}

ps2e_s32 ps2e_system_mod_enabled(ps2e_s32 mod) { return mod >= 0 && te::mods::page::Shown(uint32_t(mod)) ? 1 : 0; }

ps2e_s32 ps2e_system_mod_has_switch(ps2e_s32 mod) {
    const te::mods::Mod* m = ModAt(mod);
    return !m || m->has_switch ? 1 : 0;
}

void ps2e_system_set_mod_enabled(ps2e_s32 mod, ps2e_s32 enabled) {
    if (mod >= 0) te::mods::page::Choose(uint32_t(mod), enabled != 0);
}

ps2e_s32 ps2e_system_mod_page_rows(ps2e_s32 mod) {
    const te::mods::Mod* m = ModAt(mod);
    return m && m->page_rows ? ps2e_s32(m->page_rows()) : 0;
}

void ps2e_system_mod_page_label(ps2e_s32 mod, ps2e_u32 out, ps2e_s32 size) {
    const te::mods::Mod* m = ModAt(mod);
    CopyLabel(m ? m->page_title : "", out, size);
}

void ps2e_system_mod_page_row_label(ps2e_s32 mod, ps2e_s32 row, ps2e_u32 out, ps2e_s32 size) {
    const te::mods::Mod* m = ModAt(mod);
    CopyLabel(m && m->page_row_label && row >= 0 ? m->page_row_label(uint32_t(row)) : "", out, size);
}

ps2e_s32 ps2e_system_mod_page_row_chosen(ps2e_s32 mod, ps2e_s32 row) {
    const te::mods::Mod* m = ModAt(mod);
    if (!m || !m->page_row_chosen || row < 0 || !te::mods::Enabled(te::mods::page::Entry(uint32_t(mod)))) return te::mods::ActionNone;
    g_warp = te::mods::WarpTarget{};
    const te::mods::Action action = m->page_row_chosen(uint32_t(row), g_warp);
    std::fprintf(stderr, "[twinsanityevolved] mods: %s row %d chosen -> action %u%s%s'\n", m->id.c_str(), row, unsigned(action),
                 action == te::mods::ActionWarp ? " warp to '" : " '", g_warp.chunk.c_str());
    return ps2e_s32(action);
}

void ps2e_system_mod_warp_target(ps2e_u32 chunk, ps2e_s32 size, ps2e_u32 character) {
    CopyLabel(g_warp.chunk, chunk, size);
    if (uint8_t* p = te::GuestPointer(character, 4)) std::memcpy(p, &g_warp.character, 4);
}

}  // extern "C"
