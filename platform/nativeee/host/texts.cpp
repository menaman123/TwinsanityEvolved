// platform/nativeee (host side) -- te_texts.h: the platform's texts over the frame through the game's text system.
//
// The fps counter: render fps = presents per wall second (the frames the host actually scanned out,
// te_graphics_presents), game Hz = frame boundaries per wall second (te::Clock), both over a half-second window, drawn
// top-right as two lines in the game's menu white at half the menu size -- "59.9 fps" over "60.0 hz". The figures are the
// host's; the glyphs, the layout and the quads are the game's (Font::Draw -> ps2e_gfx2_text_quads). Off by default
// (display.fps_counter); nothing crosses the seam while it is off, so a run with it off draws exactly what it drew before.
#include "te_texts.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ps2e_seam.h"
#include "te_display.h"
#include "te_gfx_layout.h"
#include "te_host.h"
#include "te_services.h"

extern "C" uint64_t te_graphics_presents();   // host/graphics.cpp: frames presented so far

namespace te::texts {

namespace {

struct Entry { int id; std::string name; Source source; };
std::vector<Entry>& Sources() { static std::vector<Entry> s; return s; }
int g_next_id = 1;
uint32_t g_last_count = 0;

// The rates: sampled once a frame (every Collect), the window closed every half second of wall time
struct RateState {
    bool primed = false;
    std::chrono::steady_clock::time_point since;
    uint64_t presents = 0, boundaries = 0;
    Rates rates;
};
RateState g_rate;

void SampleRates() {
    using Steady = std::chrono::steady_clock;
    const auto now = Steady::now();
    const uint64_t presents = te_graphics_presents();
    const uint64_t boundaries = Services::Get().clock().BoundaryCount();
    if (!g_rate.primed) {
        g_rate.primed = true;
        g_rate.since = now;
        g_rate.presents = presents;
        g_rate.boundaries = boundaries;
        return;
    }
    const double dt = std::chrono::duration<double>(now - g_rate.since).count();
    if (dt < 0.5) return;
    g_rate.rates.fps = double(presents - g_rate.presents) / dt;
    g_rate.rates.hz = double(boundaries - g_rate.boundaries) / dt;
    g_rate.since = now;
    g_rate.presents = presents;
    g_rate.boundaries = boundaries;
}

bool g_fps_installed = false;

}  // namespace

int Register(const std::string& name, Source source) {
    const int id = g_next_id++;
    Sources().push_back({id, name, std::move(source)});
    return id;
}

void Unregister(int id) {
    auto& s = Sources();
    s.erase(std::remove_if(s.begin(), s.end(), [id](const Entry& e) { return e.id == id; }), s.end());
}

void Collect(std::vector<Line>& out) {
    SampleRates();
    for (const Entry& e : Sources()) e.source(out);
    g_last_count = uint32_t(out.size());
}

uint32_t LastCount() { return g_last_count; }

Rates CurrentRates() { return g_rate.rates; }

void InstallFpsCounter() {
    if (g_fps_installed) return;
    g_fps_installed = true;
    Register("fps_counter", [](std::vector<Line>& out) {
        if (!display::FpsCounter()) return;
        const Rates r = CurrentRates();
        // One decimal (the game's font has digits and lower-case letters; the strings stay the same width for the same
        // figures), capped so an unpaced headless run cannot overflow the corner
        char text[64];
        std::snprintf(text, sizeof text, "%.1f fps~%.1f hz", std::min(r.fps, 999.0), std::min(r.hz, 999.0));
        Line l;
        l.x = 0.97f;
        l.y = 0.04f;
        l.scale = 0.5f;
        l.colour = 0x80C0C0C0u;   // the game's white (colour.h: ColourFull 192, AlphaFull 128)
        l.alignment = Top | Right;
        l.text = text;
        out.push_back(l);
    });
    // The frame-rate row's help line (the game's menus have no help text of their own): while the GRAPHIC OPTIONS page is
    // open and the frame rate is 120 / 144 / 240 with vsync on, one line at the bottom says the display's refresh is the cap
    Register("frame_rate_help", [](std::vector<Line>& out) {
        if (!display::GraphicsPageOpen()) return;
        const int rate = display::FrameRate();
        if (rate < 2 || rate > 4 || !display::EffectiveVSync()) return;
        Line l;
        l.x = 0.5f;
        l.y = 0.90f;
        l.scale = 0.5f;
        l.colour = 0x80C0C0C0u;
        l.alignment = Top | Middle;
        l.text = display::FrameRateHelp();
        out.push_back(l);
    });
    // The game-rate row's help line: a rate chosen on the page applies from the next start, and the page says so
    Register("game_rate_help", [](std::vector<Line>& out) {
        if (!display::GraphicsPageOpen() || !display::GameRatePending()) return;
        Line l;
        l.x = 0.5f;
        l.y = 0.94f;
        l.scale = 0.5f;
        l.colour = 0x80C0C0C0u;
        l.alignment = Top | Middle;
        l.text = display::GameRateHelp();
        out.push_back(l);
    });
#if defined(__vita__)
    // The resolution row's help line on the PS Vita: at 2x, the console's maximum, the page says what it costs -- two
    // lines in the band between the page's ring and its SELECT / BACK prompts (from 0.90 down)
    Register("scale_help", [](std::vector<Line>& out) {
        if (!display::GraphicsPageOpen() || !display::ScaleAtMaximum()) return;
        Line l;
        l.x = 0.5f;
        l.y = 0.80f;
        l.scale = 0.5f;
        l.colour = 0x80C0C0C0u;
        l.alignment = Top | Middle;
        l.text = display::ScaleHelp();
        out.push_back(l);
    });
#endif
}

}  // namespace te::texts

extern "C" {

// Platform::Graphics::QueuePlatformTexts (wasm side) -> the lines of this frame, as te_platform_text records in guest memory
ps2e_s32 ps2e_gfx2_platform_texts(ps2e_u32 out, ps2e_s32 max) {
    if (max <= 0) return 0;
    static std::vector<te::texts::Line> lines;
    lines.clear();
    te::texts::Collect(lines);
    if (lines.empty()) return 0;
    const int count = int(std::min<size_t>(lines.size(), size_t(max)));
    auto* p = reinterpret_cast<te_platform_text*>(te::GuestPointer(out, sizeof(te_platform_text) * size_t(count)));
    if (!p) return 0;
    for (int i = 0; i < count; ++i) {
        const te::texts::Line& l = lines[size_t(i)];
        te_platform_text& t = p[i];
        std::memset(&t, 0, sizeof t);
        t.x = l.x; t.y = l.y; t.scale = l.scale; t.colour = l.colour; t.alignment = l.alignment;
        const size_t n = std::min(l.text.size(), sizeof(t.text) - 1);
        std::memcpy(t.text, l.text.data(), n);
        t.text[n] = 0;
    }
    static const bool diag = std::getenv("TE_GFX_DIAG_TEXT") != nullptr;
    if (diag) {
        // Every line of this frame (the census: which platform strings the frame carries; the present they are drawn in)
        std::string all;
        for (int i = 0; i < count; ++i) all += std::string(i ? " | '" : "'") + p[i].text + "'";
        std::fprintf(stderr, "[te.gfx.diag] present %llu platform texts %d: %s\n", (unsigned long long)te_graphics_presents(), count, all.c_str());
    }
    return count;
}

}  // extern "C"
