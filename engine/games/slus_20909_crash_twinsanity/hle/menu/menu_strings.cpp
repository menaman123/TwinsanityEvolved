#include "games/slus_20909_crash_twinsanity/hle/menu/menu_strings.h"

#include <cstring>

namespace title {
namespace {

// Column order = kLanguages order. The title's font is the Crash font page set
// (plain ASCII; the text files use `~` for a line break and `@`/`\\`/`^` for
// glyph escapes) -- the added strings keep to the glyphs the title's own menu
// texts use.
constexpr const char* kLanguages[] = {"American", "English", "French", "German", "Spanish", "Italian"};
constexpr uint32_t kLanguageCount = sizeof(kLanguages) / sizeof(kLanguages[0]);

constexpr const char* kTable[MenuStrings::kCount][kLanguageCount] = {
    // Display
    {"display", "display", "affichage", "anzeige", "pantalla", "schermo"},
    // Controls
    {"controls", "controls", "commandes", "steuerung", "controles", "comandi"},
    // PerfOverlay
    {"fps overlay", "fps overlay", "affichage fps", "fps-anzeige", "indicador fps", "indicatore fps"},
    // Fullscreen
    {"fullscreen", "fullscreen", "plein ecran", "vollbild", "pantalla completa", "schermo intero"},
    // KeyboardMouse
    {"keyboard and mouse", "keyboard and mouse", "clavier et souris", "tastatur und maus", "teclado y raton",
     "tastiera e mouse"},
    // Scale
    {"resolution", "resolution", "resolution", "aufloesung", "resolucion", "risoluzione"},
    // Scale1x .. Scale4x, ScaleNative (the title's ValueItems print digits: the font has them)
    {"1x", "1x", "1x", "1x", "1x", "1x"},
    {"2x", "2x", "2x", "2x", "2x", "2x"},
    {"3x", "3x", "3x", "3x", "3x", "3x"},
    {"4x", "4x", "4x", "4x", "4x", "4x"},
    {"window size", "window size", "taille fenetre", "fenstergroesse", "tamano ventana", "dimensione finestra"},
    // VSync
    {"vsync", "vsync", "vsync", "vsync", "vsync", "vsync"},
    // DisplayMode
    {"display mode", "display mode", "mode affichage", "anzeigemodus", "modo pantalla", "modo schermo"},
    // Windowed, Borderless
    {"windowed", "windowed", "fenetre", "fenster", "ventana", "finestra"},
    {"borderless", "borderless", "sans bordure", "randlos", "sin bordes", "senza bordi"},
    // Aspect
    {"aspect ratio", "aspect ratio", "format image", "seitenverhaeltnis", "relacion aspecto", "formato immagine"},
    // AspectAuto, AspectNormal (4:3), AspectWide (16:9), AspectWideTen (16:10), AspectUltraWide (21:9)
    {"auto", "auto", "auto", "auto", "auto", "auto"},
    {"normal", "normal", "normal", "normal", "normal", "normale"},
    {"wide", "wide", "large", "breit", "ancho", "largo"},
    {"wide ten", "wide ten", "large dix", "breit zehn", "ancho diez", "largo dieci"},
    {"ultra wide", "ultra wide", "ultra large", "ultrabreit", "ultra ancho", "ultra largo"},
    // RenderRate
    {"render rate", "render rate", "cadence rendu", "bildrate", "tasa de cuadros", "frequenza render"},
    // RateSixty, RateDisplay
    {"sixty", "sixty", "soixante", "sechzig", "sesenta", "sessanta"},
    {"display", "display", "ecran", "bildschirm", "pantalla", "schermo"},
    // TextureFilter
    {"texture filter", "texture filter", "filtre de texture", "texturfilter", "filtro de textura", "filtro texture"},
    // FilterTitle (the title's own choice)
    {"original", "original", "original", "original", "original", "originale"},
    // FilterPoint
    {"point", "point", "point", "punkt", "punto", "punto"},
    // FilterBilinear
    {"bilinear", "bilinear", "bilineaire", "bilinear", "bilineal", "bilineare"},
    // FilterTrilinear
    {"trilinear", "trilinear", "trilineaire", "trilinear", "trilineal", "trilineare"},
    // Anisotropy
    {"anisotropic", "anisotropic", "anisotrope", "anisotrop", "anisotropico", "anisotropico"},
    // Aniso1
    {"off", "off", "non", "aus", "no", "no"},
    // Aniso2
    {"2x", "2x", "2x", "2x", "2x", "2x"},
    // Aniso4
    {"4x", "4x", "4x", "4x", "4x", "4x"},
    // Aniso8
    {"8x", "8x", "8x", "8x", "8x", "8x"},
    // Aniso16
    {"16x", "16x", "16x", "16x", "16x", "16x"},
    // FrameRate
    {"frame rate", "frame rate", "cadence images", "bildrate", "tasa de cuadros", "frequenza fotogrammi"},
    // Uncapped
    {"uncapped", "uncapped", "illimitee", "unbegrenzt", "ilimitada", "illimitata"},
    // OffUncapped
    {"off (uncapped)", "off (uncapped)", "non (illimitee)", "aus (unbegrenzt)", "no (ilimitada)", "no (illimitata)"},
    // VSyncCapsRate
    {"vsync on: the display refresh caps the frame rate", "vsync on: the display refresh caps the frame rate",
     "vsync actif: la frequence de l'ecran limite la cadence", "vsync an: die bildwiederholrate begrenzt die bildrate",
     "vsync activado: el refresco de pantalla limita la tasa", "vsync attivo: il refresh dello schermo limita la frequenza"},
    // GameRate
    {"game rate", "game rate", "vitesse du jeu", "spieltempo", "velocidad del juego", "velocita del gioco"},
    // GameRatePal, GameRateSixty
    {"50 hz (pal)", "50 hz (pal)", "50 hz (pal)", "50 hz (pal)", "50 hz (pal)", "50 hz (pal)"},
    {"60 hz", "60 hz", "60 hz", "60 hz", "60 hz", "60 hz"},
    // GameRateNextStart
    {"the game rate changes the next time the game starts", "the game rate changes the next time the game starts",
     "la vitesse du jeu change au prochain lancement", "das spieltempo aendert sich beim naechsten start",
     "la velocidad cambia la proxima vez que inicie el juego", "la velocita cambia al prossimo avvio del gioco"},
    // ScaleVitaMaximum (two lines: `~` is the title's line break)
    {"2x is the ps vita's maximum:~half the frame rate or less", "2x is the ps vita's maximum:~half the frame rate or less",
     "2x est le maximum de la ps vita:~moitie des images ou moins", "2x ist das maximum der ps vita:~halbe bildrate oder weniger",
     "2x es el maximo de la ps vita:~la mitad de cuadros o menos", "2x e il massimo della ps vita:~meta dei fotogrammi o meno"},
};

}  // namespace

const char* MenuStrings::Text(Id id, const std::string& language_name) {
    const uint32_t row = static_cast<uint32_t>(id);
    if (row >= kCount) return "";
    uint32_t column = 1;   // English
    for (uint32_t i = 0; i < kLanguageCount; ++i)
        if (language_name == kLanguages[i]) { column = i; break; }
    return kTable[row][column];
}

const char* const* MenuStrings::Languages(uint32_t* count) {
    if (count) *count = kLanguageCount;
    return kLanguages;
}

}  // namespace title
