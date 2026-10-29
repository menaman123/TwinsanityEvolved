// te_harness.cpp -- the port's harness leg over the engine's writers (te_harness.h).
#include "te_harness.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "engine/core/config/config.h"
#include "engine/core/verify/verify_host.h"
#include "engine/platform/ps2e/ps2e_draw.h"
#include "te_host.h"
#include "te_services.h"

namespace te::harness {
namespace {

struct Harness {
    bool armed = false;
    std::unique_ptr<ps2r::verify::VerifyHost> host;
    std::FILE* state_f = nullptr;
    std::FILE* pdesc_f = nullptr;
    std::vector<uint8_t> topdown, bottomup;
    uint64_t presents = 0, boundaries = 0, state_lines = 0;
    uint64_t last_state_key = ~0ull;
    std::string state_trace;   // "Name@boundary;..." for run.meta
    int w = 0, h = 0;
} g;

constexpr uint32_t kGameControllerPointer = 0x00309888;   // PAL G_GameController

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

}  // namespace

bool ReadGameState(unsigned* state, unsigned* last_state, unsigned* next_state) {
    // (te::GuestPointer: the bounds checks of the wasm lanes; on the Vita's native build the pointer is the address itself)
    const uint8_t* global = te::GuestPointer(te::RetailAddress(kGameControllerPointer), 4);
    if (!global) return false;
    const uint32_t gc = rd32(global);
    const uint8_t* controller = gc ? te::GuestPointer(gc, 16) : nullptr;
    if (!controller) return false;
    uint64_t v = 0;
    std::memcpy(&v, controller + 8, 8);
    if (state) *state = unsigned((v >> 44) & 63);
    if (last_state) *last_state = unsigned((v >> 38) & 63);
    if (next_state) *next_state = unsigned((v >> 50) & 63);
    return true;
}

const char* StateName(unsigned s) {
    static const char* const names[] = {"WaitingForLoader", "Starting", "WaitingForPad", "CheckingCard", "VivendiLogo",
        "TravellersTalesLogo", "LoadingTitle", "Title", "MainMenu", "LoadingLevel", "NewGame", "StartingPlay", "Playing",
        "Watching", "Movie", "Paused", "Gallery", "Waiting", "GameOver", "Credits", "RestartingTitle", "FadingOut",
        "Restarting", "?23", "NoState"};
    return s < sizeof names / sizeof names[0] ? names[s] : "?";
}

// ---- the perceptual descriptor (tools/oracle/pdesc.py is the bit-for-bit mirror; change both) ----------------------------
std::string Describe(const uint8_t* px, int w, int h, int channels) {
    if (!px || w <= 0 || h <= 0 || (channels != 3 && channels != 4)) return std::string();
    // The arena-judge bands as percentages of the frame: x 5..95 %, sky y 0..25 %, mid 30..55 %, ground 60..95 %.
    struct Band { int y0, y1; } bands[3] = {{0, 25}, {30, 55}, {60, 95}};
    const int x0 = w * 5 / 100, x1 = w * 95 / 100;
    double lum[3] = {0, 0, 0};
    unsigned long long rs = 0, gs = 0, bs = 0;
    std::vector<uint32_t> bins(4096, 0);
    // dHash: 9 x 8 cells of mean luminance (integer), bit = left < right
    unsigned long long cell_sum[8][9] = {};
    unsigned long long cell_n[8][9] = {};
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = px + size_t(y) * w * channels;
        const int cy = y * 8 / h;
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + size_t(x) * channels;
            const unsigned r = p[0], gg = p[1], b = p[2];
            const unsigned l1000 = 299u * r + 587u * gg + 114u * b;
            rs += r; gs += gg; bs += b;
            bins[((r >> 4) << 8) | ((gg >> 4) << 4) | (b >> 4)]++;
            const int cx = x * 9 / w;
            cell_sum[cy][cx] += l1000;
            cell_n[cy][cx]++;
        }
    }
    for (int bi = 0; bi < 3; ++bi) {
        const int y0 = h * bands[bi].y0 / 100, y1 = h * bands[bi].y1 / 100;
        unsigned long long sum = 0, n = 0;
        for (int y = y0; y < y1; ++y) {
            const uint8_t* row = px + size_t(y) * w * channels;
            for (int x = x0; x < x1; ++x) {
                const uint8_t* p = row + size_t(x) * channels;
                sum += 299u * p[0] + 587u * p[1] + 114u * p[2];
                ++n;
            }
        }
        lum[bi] = n ? double(sum) / (1000.0 * double(n)) : 0.0;
    }
    uint32_t top = 0;
    for (uint32_t c : bins) if (c > top) top = c;
    const double n = double(w) * double(h);
    uint64_t dhash = 0;
    for (int cy = 0; cy < 8; ++cy)
        for (int cx = 0; cx < 8; ++cx) {
            const unsigned long long a = cell_n[cy][cx] ? cell_sum[cy][cx] / cell_n[cy][cx] : 0;
            const unsigned long long b = cell_n[cy][cx + 1] ? cell_sum[cy][cx + 1] / cell_n[cy][cx + 1] : 0;
            if (a < b) dhash |= uint64_t(1) << (cy * 8 + cx);
        }
    char line[256];
    std::snprintf(line, sizeof line, "%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%016llx", w, h, lum[0], lum[1], lum[2],
                  double(rs) / n, double(gs) / n, double(bs) / n, double(top) / n, (unsigned long long)dhash);
    return line;
}

// ---- the leg ---------------------------------------------------------------------------------------------------------------
bool Arm(const std::string& out_dir, const std::string& build_id, std::string* err) {
    if (g.armed) return true;
    ps2r::Config cfg;
    cfg.harness.out_dir = out_dir;
    cfg.meta.profile = te::GetSettings().deterministic ? "deterministic" : "play";
    g.host = std::make_unique<ps2r::verify::VerifyHost>();
    std::vector<ps2r::verify::VerifyHost::FingerprintEntry> extra;
    extra.push_back({"port", "twinsanityevolved"});
    if (!g.host->Arm(cfg, build_id, "slus_20909_crash_twinsanity", err, extra)) { g.host.reset(); return false; }
    g.host->SetMemLane(false);   // the game's memory is the module's: not the oracle's lane; frames + audio only
    g.state_f = std::fopen((out_dir + "/state.trace").c_str(), "w");
    g.pdesc_f = std::fopen((out_dir + "/frames.pdesc").c_str(), "w");
    if (g.pdesc_f) std::fprintf(g.pdesc_f, "# key,w,h,lum_sky,lum_mid,lum_ground,mean_r,mean_g,mean_b,clear_share,dhash\n");
    g.armed = true;
    te::Services::Get().clock().AddBoundaryHook("harness", [](uint64_t boundary) { OnBoundary(boundary); });
    Set("harness", "port");
    return true;
}

bool Armed() { return g.armed; }
const std::string& LegDir() { static const std::string none; return g.host ? g.host->LegDir() : none; }

void OnPresentSkipped(uint64_t key) {
    if (!g.armed) return;
    g.host->OnPresentHashed(uint32_t(key), 0, "none");   // not read back (a keyed leg): hash 0, the line kept for the index
    if (g.pdesc_f) std::fprintf(g.pdesc_f, "%llu,0,0,0,0,0,0,0,0,0,0\n", (unsigned long long)key);
}

void OnPresent(ps2e_draw* dev, uint64_t key) {
    if (!g.armed) return;
    int w = 0, h = 0;
    if (dev) ps2e_draw_read_presented(dev, nullptr, 0, &w, &h);
    if (!dev || w <= 0 || h <= 0) {
        g.host->OnPresentHashed(uint32_t(key), 0, "none");   // no readable scanout: hash 0, the line kept (index alignment)
        if (g.pdesc_f) std::fprintf(g.pdesc_f, "%llu,0,0,0,0,0,0,0,0,0,0\n", (unsigned long long)key);
        return;
    }
    g.topdown.resize(size_t(w) * h * 4);
    if (!ps2e_draw_read_presented(dev, g.topdown.data(), g.topdown.size(), &w, &h)) {
        g.host->OnPresentHashed(uint32_t(key), 0, "none");
        return;
    }
    // The engine's hasher reads GL order (row 0 = the bottom row); ps2e_draw hands the image top row first.
    g.bottomup.resize(g.topdown.size());
    for (int y = 0; y < h; ++y)
        std::memcpy(g.bottomup.data() + size_t(y) * w * 4, g.topdown.data() + size_t(h - 1 - y) * w * 4, size_t(w) * 4);
    g.host->OnPresent(uint32_t(key), g.bottomup.data(), w, h, 1, "ps2e_draw/gl");
    if (g.pdesc_f) {
        std::fprintf(g.pdesc_f, "%llu,%s\n", (unsigned long long)key, Describe(g.topdown.data(), w, h, 4).c_str());
        std::fflush(g.pdesc_f);
    }
    g.presents++;
    g.w = w; g.h = h;
}

void OnAudioTick(const float* interleaved, int frames) {
    if (!g.armed) return;
    static const std::vector<ps2r::verify::MemorySpan> no_spans;
    g.host->OnBoundary(interleaved, frames, no_spans);
}

void OnBoundary(uint64_t boundary) {
    if (!g.armed) return;
    g.boundaries = boundary;
    unsigned state = 0, last = 0, next = 0;
    if (!ReadGameState(&state, &last, &next)) return;
    const uint64_t key = (uint64_t(last) << 12) | (uint64_t(state) << 6) | next;
    if (key == g.last_state_key) return;
    const bool state_changed = g.last_state_key == ~0ull || ((g.last_state_key >> 6) & 63) != state;
    g.last_state_key = key;
    if (!state_changed) return;   // only the STATE word's changes are anchors (lastState/nextState churn is not)
    if (g.state_f) {
        std::fprintf(g.state_f, "%llu,%u,%s\n", (unsigned long long)boundary, state, StateName(state));
        std::fflush(g.state_f);
    }
    g.state_lines++;
    if (!g.state_trace.empty()) g.state_trace += ';';
    g.state_trace += std::string(StateName(state)) + "@" + std::to_string(boundary);
}

void Set(const std::string& key, const std::string& value) {
    if (g.armed && g.host->Meta()) g.host->Meta()->Set(key, value);
}

void Finalize() {
    if (!g.armed) return;
    Set("boundaries", std::to_string(g.boundaries));
    Set("presents", std::to_string(g.presents));
    Set("frame_size", std::to_string(g.w) + "x" + std::to_string(g.h));
    Set("state_trace", g.state_trace);
    Set("state_trace_lines", std::to_string(g.state_lines));
    g.host->Finalize();
    if (g.state_f) { std::fclose(g.state_f); g.state_f = nullptr; }
    if (g.pdesc_f) { std::fclose(g.pdesc_f); g.pdesc_f = nullptr; }
}

}  // namespace te::harness
