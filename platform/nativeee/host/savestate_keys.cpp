// savestate_keys.cpp -- see te_savestate_keys.h.
#include "te_savestate_keys.h"

#include <cstdio>
#include <vector>

#include "engine/platform/host/platform.h"
#include "savestate.h"
#include "te_host.h"
#include "te_input.h"
#include "te_services.h"
#include "te_texts.h"

namespace te::savestate_keys {

namespace {

const char* const kKeySave = "F5";
const char* const kKeyLoad = "F8";
// Where the notice sits: horizontally centred near the bottom of the title-safe area (the HUD's counters are at the top
// corners, the speedrun timer and the fps counter at the top), half the menu's glyph size, the game's menu white
constexpr float kNoticeX = 0.5f, kNoticeY = 0.9f, kNoticeScale = 0.5f;
constexpr uint32_t kNoticeColour = 0x80FFFFFFu;
constexpr uint32_t kNoticeSeconds = 2;

struct State {
    bool installed = false;
    std::string root;
    int slot = 0;
    bool held_save = false, held_load = false;
    std::string pending_save, pending_load;   // the paths the keys asked for (answered by Saved / Loaded)
    std::string notice;
    uint32_t frames_left = 0;
    int text_source = -1;
};
State g;

std::string Joined(const std::string& dir, const char* name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    return (last == '/' || last == '\\') ? dir + name : dir + "/" + name;
}

void Show(const std::string& text) {
    g.notice = text;
    const unsigned hz = GetSettings().field_hz ? GetSettings().field_hz : 60u;
    g.frames_left = kNoticeSeconds * hz;   // counted in frames the game draws (the notice's source is asked once a frame)
}

std::string SlotWords() { return "slot " + std::to_string(g.slot); }

bool FileExists(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// MakeDirectory for every missing level of <root>/savestates (the root itself normally exists: it holds ps2r.ini)
bool MakeDirectories(const std::string& dir) {
    if (dir.empty() || ps2r::host::IsDirectory(dir.c_str())) return true;
    const size_t slash = dir.find_last_of("/\\");
    if (slash != std::string::npos && slash > 0) {
        const std::string parent = dir.substr(0, slash);
        // a drive or device root ("C:", "ux0:") is not made
        if (!parent.empty() && parent.back() != ':' && !MakeDirectories(parent)) return false;
    }
    return ps2r::host::MakeDirectory(dir.c_str());
}

void OnBoundary(uint64_t boundary) {
    auto edge = [](const char* key, bool& held) {
        const bool now = input::KeyHeld(key);
        const bool pressed = now && !held;
        held = now;
        return pressed;
    };
    if (edge(kKeySave, g.held_save)) {
        const std::string dir = Directory(g.root);
        const std::string path = SlotPath(g.root, g.slot);
        if (!MakeDirectories(dir)) {
            std::fprintf(stderr, "[twinsanityevolved] savestate: %s -- the folder %s cannot be made; nothing saved\n", kKeySave, dir.c_str());
            Show("state not saved (its folder cannot be made)");
        } else {
            g.pending_save = path;
            savestate::RequestSave(path);
            std::fprintf(stderr, "[twinsanityevolved] savestate: %s -> %s (%s) @%llu\n", kKeySave, SlotWords().c_str(), path.c_str(),
                         (unsigned long long)boundary);
        }
    }
    if (edge(kKeyLoad, g.held_load)) {
        const std::string path = SlotPath(g.root, g.slot);
        if (!FileExists(path)) {
            std::fprintf(stderr, "[twinsanityevolved] savestate: %s -- no saved state in %s (%s): %s saves one\n", kKeyLoad, SlotWords().c_str(),
                         path.c_str(), kKeySave);
            Show("no saved state in " + SlotWords());
        } else {
            g.pending_load = path;
            savestate::RequestLoad(path);
            std::fprintf(stderr, "[twinsanityevolved] savestate: %s <- %s (%s) @%llu\n", kKeyLoad, SlotWords().c_str(), path.c_str(),
                         (unsigned long long)boundary);
        }
    }
}

void Texts(std::vector<texts::Line>& out) {
    if (g.frames_left == 0 || g.notice.empty()) return;
    --g.frames_left;
    texts::Line line;
    line.x = kNoticeX;
    line.y = kNoticeY;
    line.scale = kNoticeScale;
    line.colour = kNoticeColour;
    line.alignment = texts::Bottom | texts::Centre;
    line.text = g.notice;
    out.push_back(line);
}

}  // namespace

std::string Directory(const std::string& root) { return Joined(root, "savestates"); }

std::string SlotPath(const std::string& root, int slot) {
    return Joined(Directory(root), ("slot" + std::to_string(slot) + ".ps2st").c_str());
}

void Install(const std::string& root, int slot) {
    g.root = root;
    g.slot = slot < 0 ? 0 : slot;
    if (g.installed) return;
    g.installed = true;
    Services::Get().clock().AddBoundaryHook("savestate keys", OnBoundary);
    g.text_source = texts::Register("savestate", Texts);
    std::fprintf(stderr, "[twinsanityevolved] savestate: %s saves the game to %s (%s), %s loads it\n", kKeySave, SlotWords().c_str(),
                 SlotPath(g.root, g.slot).c_str(), kKeyLoad);
}

bool Installed() { return g.installed; }
const std::string& Root() { return g.root; }
int Slot() { return g.slot; }

bool IsPendingSave(const std::string& path) { return !g.pending_save.empty() && path == g.pending_save; }

void Saved(const std::string& path, bool ok, const std::string& error) {
    if (g.pending_save.empty() || path != g.pending_save) return;
    g.pending_save.clear();
    if (ok) Show("state saved to " + SlotWords());
    else {
        std::fprintf(stderr, "[twinsanityevolved] savestate: %s not saved: %s\n", path.c_str(), error.c_str());
        Show("state not saved (see the log)");
    }
}

void Loaded(const std::string& path, bool ok, const std::string& error) {
    if (g.pending_load.empty() || path != g.pending_load) return;
    g.pending_load.clear();
    if (ok) Show("state loaded from " + SlotWords());
    else Show("state not loaded (see the log)");
    (void)error;   // the runner logs it
}

std::string Notice() { return g.frames_left ? g.notice : std::string(); }
uint32_t NoticeFramesLeft() { return g.frames_left; }

}  // namespace te::savestate_keys
