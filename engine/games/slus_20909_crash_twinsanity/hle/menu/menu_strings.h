#pragma once
// MenuStrings -- the strings the owned menu extension adds to the title's text
// table, one per language bank the title knows. The title's menus take text
// INDICES (MenuItemBits.text, MenuPageFlags.title, ChoiceItem::texts) into the
// current language's line table (TR include/game/language.h GameText); the
// extension appends these strings to every LOADED language bank at the title's
// Options page construction and refers to them by index, so a language change
// re-labels the added pages exactly as it re-labels the title's own. The
// strings travel in the binary (title data, never the memory card); the table
// is keyed by the title's language NAME (the folder/file name of
// Language\Code\<name>.txt: the US disc carries `American`; the PAL names are
// English, French, German, Spanish, Italian). An unknown name falls back to
// English. The title's own menu texts are lower-case ("graphic options",
// "widescreen"): the added ones follow.
#include <cstdint>
#include <string>

namespace title {

class MenuStrings {
public:
    enum class Id : uint8_t {
        Display = 0,        // the Display page title and its Options link
        Controls,           // the Controls page title and its Options link
        PerfOverlay,        // "fps overlay"
        Fullscreen,         // "fullscreen"
        KeyboardMouse,      // "keyboard and mouse"
        // The Display page's A2/A3 rows: labels and choice names.
        Scale,              // "resolution" (video.scale)
        Scale1x, Scale2x, Scale3x, Scale4x, ScaleNative,   // "1x".."4x", "window size"
        VSync,              // "vsync" (video.vsync)
        DisplayMode,        // "display mode" (video.fullscreen)
        Windowed, Borderless,                              // + Fullscreen above
        Aspect,             // "aspect ratio" (video.aspect)
        AspectAuto, AspectNormal, AspectWide, AspectWideTen, AspectUltraWide,
        RenderRate,         // "render rate" (video.render_rate)
        RateSixty, RateDisplay,
        TextureFilter,      // "texture filter" (video.texture_filter)
        FilterTitle,        // "original" -- the title's own TEX1 choice
        FilterPoint,        // "point"
        FilterBilinear,     // "bilinear"
        FilterTrilinear,    // "trilinear"
        Anisotropy,         // "anisotropic" (video.anisotropy)
        Aniso1,             // "off"
        Aniso2,             // "2x"
        Aniso4,             // "4x"
        Aniso8,             // "8x"
        Aniso16,            // "16x"
        // The FRAME RATE row (video.frame_rate) of a producer that paces its own presents: the label, the "uncapped" value,
        // the vsync row's value while uncapped ("off (uncapped)") and the help line for 120 / 144 / 240 with vsync on.
        FrameRate,          // "frame rate"
        Uncapped,           // "uncapped"
        OffUncapped,        // "off (uncapped)"
        VSyncCapsRate,      // "vsync on: the display refresh caps the frame rate"
        // The GAME RATE row (game.hz): the game's own frame rate, read when the game starts.
        GameRate,           // "game rate"
        GameRatePal,        // "50 hz (pal)"
        GameRateSixty,      // "60 hz"
        GameRateNextStart,  // "the game rate changes the next time the game starts"
        // The PS Vita's RESOLUTION row at its maximum, 2x: the help line says what it costs.
        ScaleVitaMaximum,   // "2x is the ps vita's maximum:~half the frame rate or less" (two lines)
        Count,
    };
    static constexpr uint32_t kCount = static_cast<uint32_t>(Id::Count);

    // The string of `id` in the language bank `language_name` (English when the
    // name is unknown). Never null.
    static const char* Text(Id id, const std::string& language_name);
    // The language names the table carries (for the handoff/selftest).
    static const char* const* Languages(uint32_t* count);
};

}  // namespace title
