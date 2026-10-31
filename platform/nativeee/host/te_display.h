// te_display.h -- the port's DISPLAY settings -- the engine's Display page (aspect, resolution,
// texture filter, vsync, window mode) and the title's own MoveDisplay (the console's "centre screen" option) -- over the
// engine's ps2e_draw presentation, persisted in ps2r.ini (the engine's video.* keys), shown as rows of the title's GRAPHIC
// OPTIONS page through the seam (Platform::Graphics::DisplaySetting*).
#pragma once
#include <cstdint>
#include <string>

namespace te::display {

// What the runner read from ps2r.ini (the engine's VideoConfig, the keys the port honours) before the device exists.
struct Settings {
    int aspect = 0;          // video.aspect: 0 auto (the TV's 4:3 in a window; the frame's own pixels headless -- the frozen
                             // judges' captures), 1 4:3, 2 16:9, 3 16:10, 4 21:9
    int fit = 1;             // video.fit_mode: 0 stretch, 1 fit (letterbox), 2 fill, 3 integer, 4 custom (= fit here)
    int scale = 1;           // video.scale: the frame target's internal scale 1..4 (0 = 1; the PS Vita 1..2: display.cpp)
    int texture_filter = 0;  // video.texture_filter: 0 title, 1 point, 2 bilinear, 3 trilinear (bilinear here: ps2e_draw has no mip chain)
    bool vsync = false;      // video.vsync (the port's default: off -- its pacer times the presents; a windowed run may turn it on)
    int fullscreen = 0;      // video.fullscreen: 0 windowed, 1 fullscreen, 2 borderless
    int offset_x = 0;        // video.custom_offset_x/y: the title's MoveDisplay, in FRAME pixels (the PS2's +-64 across, +-32 down)
    int offset_y = 0;
    bool fps_counter = false;   // display.fps_counter: render fps + game Hz top-right through the GAME's text system
    int frame_rate = 1;         // video.frame_rate: 0 = 30, 1 = 60 (default), 2 = 120, 3 = 144, 4 = 240, 5 = uncapped -- the
                                // presenter's cadence (the engine's PresentPacer rates); `60` shows every picture the game makes,
                                // so at a game rate of 50 Hz it is 50 frames a second
    int tick_lock = 0;          // the Vita: display.tick_lock = auto | 60 | 30 -- the FRAME RATE row is the TICK LOCK there
                                // (te_tick_lock.h): 0 auto (60/30, the default), 1 60, 2 30; the presenter stays at 60 (every
                                // picture shown) and frame_rate is 1
    bool interp = false;        // video.interp: at 120 / 144 / 240 / uncapped, the frames between two pictures interpolated (on)
                                // or the newest picture shown until the next (off, the default: every frame a picture as the
                                // game drew it -- an interpolated frame re-projects the newest picture's geometry, cut to its
                                // camera, and a fast camera turn shows its gaps); TE_GFX_INTERP overrides it
    int game_hz = 60;           // game.hz as chosen: 50 (the PAL console's rate) or 60; takes effect at the next start
    int running_hz = 60;        // the game rate this run started with (te::Settings::field_hz)
};

// The runner's settings (before the device; applied when it is created, re-applied on every change).
void Configure(const Settings& settings);
const Settings& Current();
// The title's MoveDisplay (Platform::Graphics::MoveDisplay: an offset between -1 and 1 each way): the picture moved by
// up to 64 frame pixels across and 32 down, persisted as video.custom_offset_x/y.
void MoveDisplay(float x, float y);
// Push the current settings to the device (graphics.cpp calls it when the device exists; no device = remembered).
void Apply();
// One line for the log.
std::string Describe();

// The seam's rows (Platform::Graphics::DisplaySetting*): the settings the page shows, in this order.
enum Row : uint32_t { RowAspect = 0, RowScale, RowTextureFilter, RowVSync, RowFrameRate, RowDisplayMode, RowFpsCounter, RowGameRate,
                     RowCount };
uint32_t Choices(uint32_t row);
std::string RowLabel(uint32_t row);
std::string ChoiceLabel(uint32_t row, uint32_t choice);
uint32_t Value(uint32_t row);
// The player's choice: applied at once and written to ps2r.ini (when the settings name a file).
void Set(uint32_t row, uint32_t choice);
// the fps counter -- on or off (the runner's --fps-counter / ps2r.ini display.fps_counter / the Display row).
bool FpsCounter();
void SetFpsCounter(bool on);
// The frame rate (0..5 as Settings::frame_rate; the names are the engine's PresentPacer's: "30" .. "240", "uncapped") and
// the swap interval in force: uncapped forces vsync off for the session (the vsync row reads "off (uncapped)" and refuses
// a change while uncapped is selected).
int FrameRate();
const char* FrameRateName();
bool EffectiveVSync();
// The GRAPHIC OPTIONS page is open (the game polled a row's value within the last frame): the help line's cue, and the
// line itself in the menu's language ("vsync on: the display refresh caps the frame rate").
bool GraphicsPageOpen();
std::string FrameRateHelp();
// The game rate chosen in the menu differs from the one this run started with: the help line says it applies next start.
bool GameRatePending();
std::string GameRateHelp();
#if defined(__vita__)
// The PS Vita's RESOLUTION row at its maximum (2x) and the help line the page shows then, in the menu's language
// ("2x is the ps vita's maximum:~half the frame rate or less", two lines).
bool ScaleAtMaximum();
std::string ScaleHelp();
#endif

}  // namespace te::display
