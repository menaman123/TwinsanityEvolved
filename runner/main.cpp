// twinsanityevolved -- the host runner.
//
// Constructs the engine's MainMemory, binds it as the game module's linear memory, instantiates the wasm2c module,
// loads the retail data image from the PAL executable on the disc image at RUN time, patches retail code pointers to
// wasm table indices, and calls the module's `entry`. Seam imports without an owner body yet hit the generated weak
// stubs (seam_stubs.cpp), which log once; the module's only imports are env.memory and the seam.
//
//   twinsanityevolved --iso <disc image> [--elf-in-iso SLES_525.68] [--dry-run] [--verbose]
//   twinsanityevolved --elf <SLES_525.68 extracted> [--dry-run]
//
// --dry-run: instantiate, load, patch, report, exit 0 (the toolchain's own smoke test). Game data is read at run time
// only: nothing is extracted to disk.
//
// The game runs on its own fiber (te_game.h) and yields to this host loop at save-state checkpoints (the end of every
// frame boundary when a save is due), at Platform::System::Exit and when entry returns or traps. Save states:
// --save-state PATH@N writes PATH when boundary N has passed (repeatable); --load-state PATH restores before the first
// switch into the fiber, so the loaded game continues at its saved boundary. Both re-exec the runner without ASLR.
#include <csignal>
#include <filesystem>
#if defined(_WIN32) || defined(__vita__)
#include <chrono>
#include <thread>
#endif
#if !defined(_WIN32)
#include <unistd.h>   // _exit (the watchdog's second firing)
#endif
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/ee/mem/main_memory.h"
#include "engine/core/config/config_loader.h"
#include "engine/core/config/settings_file.h"
#include "engine/platform/ps2e/present_pacer.h"
#include "te_graphics_hooks.h"
#include "engine/platform/host/platform.h"
#include "padlog.h"
#include "savestate.h"
#include "te_audio.h"
#include "te_disc_log.h"
#include "te_region.h"
#include "te_env.h"
#include "te_game.h"
#include "te_harness.h"
#include "te_host.h"
#include "te_display.h"
#include "te_input.h"
#include "te_language.h"
#include "te_mods.h"
#include "te_autopilot.h"
#include "te_probe.h"
#include "te_profile.h"
#include "te_prof.h"
#include "te_savestate.h"
#include "te_savestate_keys.h"
#include "te_math_diag.h"
#include "te_startup.h"
#if defined(__vita__)
#include "te_vita.h"   // the Vita lane: baked arguments, the log file, the memory report
#include "te_tick_lock.h"   // the tick lock's exit line
#endif
#include "te_services.h"
#include "te_texts.h"
#include "te_run.h"
#if defined(TE_NATIVE)
#include "te_native.h"   // the Vita's native build of the game code: no wasm module (te_native.h)
#else
#include "wasm-rt.h"
#endif

extern "C" const uint32_t te_known_function_starts[];
extern "C" const uint32_t te_known_function_start_count;
#if !defined(TE_NATIVE)
extern w2c_env* te_diag_env;   // runner/te_env.cpp: the call_indirect adapter log reads the retail table through it
#endif

namespace {
w2c_env g_env;
#if !defined(TE_NATIVE)
w2c_game g_instance{};   // a global: its address is part of the game fiber's frames (save states)
#endif
int g_exit_status = 0;
volatile sig_atomic_t g_watchdog = 0;

// Pending --save-state requests: (path, boundary); taken at the first checkpoint at or after that boundary
struct SaveRequest { std::string path; uint64_t at; bool done; };
std::vector<SaveRequest> g_save_requests;
std::string g_save_now;   // the path the host writes when the fiber yields with Reason::Checkpoint
std::string g_load_now;   // the path the host restores when the fiber yields with Reason::LoadState

void Usage() {
    std::fprintf(stderr, "usage: twinsanityevolved (--iso <image> [--elf-in-iso NAME] [--region-map FILE] | --elf <file>) [--dry-run] [--verify-disc] [--verbose] [--run-seconds N] [--test-trap] [--trace-state]\n"
                         "       [--deterministic] [--hz 60|50] [--language <name>|0..7] [--text-language <name>] [--speech-language <name>] [--language-bank N] [--config ps2r.ini|none] [--aspect auto|4:3|16:9|16:10|21:9] [--frame-rate 30|60|120|144|240|uncapped] [--fps-counter on|off] [--mod ID ...] [--mc-dir DIR] [--mods-dir DIR] [--no-sdl-input] [--no-kbm] [--press 0xBUTTONS] [--no-audio] [--skip-movies] [--stream-instant]\n"
                         "       [--save-state PATH@BOUNDARY ...] [--load-state PATH] [--stop-at BOUNDARY] [--present-ct16]\n"
                         "       [--record-pad FILE] [--play-pad FILE] [--pad-script FILE] [--inject-key KEY@FROM-TO ...] [--close-at BOUNDARY]\n"
                         "       [--harness-out DIR]\n"
                         "       [--trace-player N] [--trace-bodies N] [--dump-collision DIR] [--autopilot PLAN]\n");
}

// The checkpoint (te::savestate::Checkpoint, the last statement of te::Clock::Boundary) on the game fiber: yield to
// the host when a save is due; the host writes it and resumes. Nothing else happens here.
void OnCheckpoint(uint64_t boundary) {
    for (SaveRequest& r : g_save_requests) {
        if (r.done || boundary < r.at) continue;
        r.done = true;
        g_save_now = r.path;
        te::game::Yield(te::game::Reason::Checkpoint, 0, boundary);
    }
    // The paths go straight into the globals: NO heap-owning local may be live across a Yield here -- the saved stack carries
    // this frame, and a later load (in the same process) resumes it with that local's heap pointer long freed (a
    // std::string local here aborted the third practice load with "pointer being freed was not allocated").
    if (te::savestate::TakeSaveRequest(g_save_now)) te::game::Yield(te::game::Reason::Checkpoint, 0, boundary);
    if (te::savestate::TakeLoadRequest(g_load_now)) te::game::Yield(te::game::Reason::LoadState, 0, boundary);   // restored in place
    if (g_watchdog) te::game::Yield(te::game::Reason::Exit, 0, boundary);
}
}  // namespace

namespace te {
#if !defined(__vita__)   // (the Vita: inline in te_host.h over the globals BindMainMemory sets)
uint8_t* GuestMemory() { return g_env.memory.data; }
std::size_t GuestMemorySize() { return g_env.memory.size; }
#endif
const char* GuestString(uint32_t address, std::size_t max) {
    const uint8_t* p = GuestPointer(address, 1);
    if (!p) return nullptr;
    const std::size_t room = std::min<std::size_t>(max, GuestMemorySize() - address);
    return std::memchr(p, 0, room) ? reinterpret_cast<const char*>(p) : nullptr;
}
[[noreturn]] void RequestExit(int status) {
    g_exit_status = status;
    te::game::ExitFromGame(status);   // returns only when not on the game fiber
    std::exit(status);
}
}  // namespace te

int main(int argc, char** argv) {
    te::startup::AttachParentConsole();
#if defined(__vita__)
    te::vita::Startup(&argc, &argv);
#endif
    te::profile::StartFromEnvironment();
    std::string iso, elf_path, elf_in_iso = "SLES_525.68";
    bool dry_run = false, verbose = false, test_trap = false, no_audio = false, trace_state = false, verify_disc = false;
    unsigned trace_player = 0;
    unsigned trace_bodies = 0;
    unsigned run_seconds = 0;
    te::Settings settings;   // the services' run settings (platform/nativeee/host/te_host.h)
    uint32_t press = 0;
    std::string load_state, record_pad, play_pad, pad_script, harness_out, autopilot, dump_collision, config_path, aspect_override, frame_rate_override;
    int fps_counter_override = -1;   // --fps-counter on|off for this run (the judge's arms); -1 = the file's / off
    bool hz_given = false;           // --hz on the command line wins over ps2r.ini's game.hz
    bool hz_from_settings = false;   // a settings file is in play: the GAME RATE row reads its (or the default) value back
    char hz_source[112] = "the default 60: no settings file";   // where the game rate came from (the Vita's start-up line)
    // game.hz's default when a settings file is in play and does not name it (or there is none yet): the
    // Vita's is the PAL disc's own 50 Hz (its developers' game loop), the desktop's the engine's 60. A run without a
    // settings file (--config none: the deterministic legs, the judges) keeps the engine's 60; --hz decides for one run.
#if defined(__vita__)
    constexpr int kSettingsDefaultHz = 50;
    constexpr const char* kSettingsDefaultName = "the Vita's default 50";
#else
    constexpr int kSettingsDefaultHz = 60;
    constexpr const char* kSettingsDefaultName = "the default 60";
#endif
    std::vector<std::string> mods_on, mods_from_file;   // --mod <id> (repeatable, on for this run, not persisted); ps2r.ini's mods.<id>.enabled
    uint64_t stop_at = 0, close_at = 0;
    // the memory cards (and ps2r.ini beside them): `userdata` in the game's own folder for a downloaded game, or an
    // earlier install's <home>/.twinsanityevolved (te_startup.h DataDirectory)
    {
        const std::string data = te::startup::DataDirectory();
        if (!data.empty()) settings.mc_dir = data + "/memcards";
    }
    std::string mods_dir;   // --mods-dir: the mods folder for this run (te_mods.h FOLDER MODS)
    std::string region_map_arg;     // --region-map FILE: an address map for another release's program (te_region.h)
    bool elf_in_iso_given = false;  // --elf-in-iso NAME, else the program SYSTEM.CNF boots
    std::string mc_dir_arg;   // --mc-dir as given ("" = not given: the game folder decides)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 >= argc) { Usage(); std::exit(2); } out = argv[++i]; };
        if (a == "--iso") next(iso);
        else if (a == "--elf") next(elf_path);
        else if (a == "--elf-in-iso") { next(elf_in_iso); elf_in_iso_given = true; }
        else if (a == "--region-map") next(region_map_arg);   // an address map (tools/discs/elf_region_map.py) for this run
        else if (a == "--dry-run") dry_run = true;
        else if (a == "--verify-disc") verify_disc = true;   // hash the whole image at boot: retail = a known dump (TE_DISC_VERIFY=1 too)
        else if (a == "--verbose") verbose = true;
        else if (a == "--test-trap") test_trap = true;   // self-test: a wasm trap must unwind to main, not crash
        else if (a == "--run-seconds") { std::string v; next(v); run_seconds = unsigned(std::atoi(v.c_str())); }   // watchdog: exit 0 after N s
        else if (a == "--deterministic") settings.deterministic = true;        // no pacer: one field per boundary, fixed RTC
        else if (a == "--hz") { std::string v; next(v); settings.field_hz = unsigned(std::atoi(v.c_str())); hz_given = true; }
        else if (a == "--language") {   // a language NAME (english, french, ...) for the text AND the speech (the legacy one name), or a raw console code 0..7
            std::string v; next(v);
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) settings.language = std::atoi(v.c_str());
            else settings.language_name = v;
        }
        else if (a == "--text-language") next(settings.text_language_name);       // the text's language for this run (english, portuguese, arabic ...)
        else if (a == "--speech-language") next(settings.speech_language_name);   // the speech's language for this run (english, french, spanish ...)
        else if (a == "--language-bank") { std::string v; next(v); settings.language_bank = std::atoi(v.c_str()); }   // a slot of the game's language table (diagnostic)
        else if (a == "--config") next(config_path);
        else if (a == "--fps-counter") { std::string v; next(v); fps_counter_override = (v == "on" || v == "1") ? 1 : 0; }   // render fps + game Hz through the game's text system
        else if (a == "--mod") { std::string v; next(v); mods_on.push_back(v); }   // a built-in mod (level_select, speedrun) on for this run
        else if (a == "--aspect") { std::string v; next(v); aspect_override = v; }
        else if (a == "--frame-rate") { std::string v; next(v); frame_rate_override = v; }   // the presenter's cadence for this run (30|60|120|144|240|uncapped)   // the display shape for this run (auto|4:3|16:9|16:10|21:9; the judge's arm)   // the settings file read (and written by the Options menu); default <home>/.twinsanityevolved/ps2r.ini, `none` = no file
        else if (a == "--mc-dir") { next(settings.mc_dir); mc_dir_arg = settings.mc_dir; }
        else if (a == "--mods-dir") next(mods_dir);   // the mods folder for this run (default: <the folder holding ps2r.ini>/mods)
        else if (a == "--no-sdl-input") settings.sdl_input = false;
        else if (a == "--no-kbm") settings.kbm = false;
        else if (a == "--press") { std::string v; next(v); press = uint32_t(std::strtoul(v.c_str(), nullptr, 0)); }   // hold the game's button word on pad 0
        else if (a == "--no-audio") no_audio = true;   // mix without opening the SDL audio device (headless / CI)
        else if (a == "--skip-movies") settings.skip_movies = true;   // FEATURES: every movie skipped (the stub's behaviour)
        else if (a == "--stream-instant") settings.stream_instant = true;   // A/B: the pre-drive-model stream façade (reads complete in the call)
        else if (a == "--present-ct16") settings.present_ct16 = true;   // the retail look -- the dithered 16-bit present (TE_GFX_PRESENT_CT16=1 too)
        else if (a == "--trace-state") trace_state = true;   // log GameController::State() changes at the boundaries (title judge)
        else if (a == "--trace-player") { std::string v; next(v); trace_player = unsigned(std::atoi(v.c_str())); if (!trace_player) trace_player = 1; }   // authoring probe: position/chunk/state every N boundaries
        else if (a == "--trace-bodies") { std::string v; next(v); trace_bodies = unsigned(std::atoi(v.c_str())); if (!trace_bodies) trace_bodies = 1; }   // the floating rigid bodies (the icebergs) every N boundaries
        else if (a == "--save-state") {   // PATH@N: write PATH at the first checkpoint at or after boundary N (N omitted: the first)
            std::string v; next(v);
            SaveRequest r{v, 0, false};
            const size_t at = v.rfind('@');
            if (at != std::string::npos) { r.path = v.substr(0, at); r.at = std::strtoull(v.c_str() + at + 1, nullptr, 0); }
            g_save_requests.push_back(r);
        }
        else if (a == "--load-state") next(load_state);
        else if (a == "--stop-at") { std::string v; next(v); stop_at = std::strtoull(v.c_str(), nullptr, 0); }   // exit 0 at boundary N
        else if (a == "--record-pad") next(record_pad);    // write pad 0's samples per boundary (fixtures/padlogs format)
        else if (a == "--play-pad") next(play_pad);        // replace the sampler with a recorded padlog
        else if (a == "--pad-script") next(pad_script);    // a padlog with `hold` lines (authoring: boundary ranges)
        else if (a == "--harness-out") next(harness_out);   // the harness leg (te_harness.h): frames.fhash / audio.ahash / run.meta / state.trace / frames.pdesc
        else if (a == "--autopilot") next(autopilot);
        else if (a == "--dump-collision") next(dump_collision);   // with --trace-player: each chunk's collision triangles as OBJ into DIR (authoring)      // assisted play: a plan driven from the probe (record it with --record-pad)
        else if (a == "--close-at") { std::string v; next(v); close_at = std::strtoull(v.c_str(), nullptr, 0); }   // post the window's close (SDL_QUIT) at boundary N
        else if (a == "--inject-key") {   // KEY@FROM-TO: the SDL key held through the keyboard backend at those boundaries (menus by keyboard)
            std::string v; next(v);
            const size_t at = v.rfind('@'), dash = v.rfind('-');
            if (at == std::string::npos || dash == std::string::npos || dash < at) { Usage(); return 2; }
            std::string error;
            if (!te::input::InjectKey(v.substr(0, at), std::strtoull(v.c_str() + at + 1, nullptr, 0), std::strtoull(v.c_str() + dash + 1, nullptr, 0), &error)) {
                std::fprintf(stderr, "[twinsanityevolved] --inject-key: %s\n", error.c_str());
                return 2;
            }
        }
        else { Usage(); return 2; }
    }
    // A play run (not --deterministic: the launcher's game, a player's terminal) runs without ASLR from its first line, so an
    // F5 state (te_savestate_keys.h) loads with F8 in a LATER session too: a state holds the image's addresses (macOS re-spawns
    // itself with ASLR off -- the waiting parent forwards the launcher's stop to it --, Linux re-execs under ADDR_NO_RANDOMIZE,
    // Windows links without a dynamic base). TE_NO_RESPAWN=1 opts out (states then load in the saving session only).
#if !defined(__vita__)   // (the Vita has no save-state mechanism: te_savestate_null.cpp)
    if (!settings.deterministic) {
        std::string warning;
        te::savestate_runner::EnsureNoAslr(argc, argv, &warning);   // re-execs (and never returns) when ASLR moved us
        if (!warning.empty()) std::fprintf(stderr, "[twinsanityevolved] savestate: %s\n", warning.c_str());
    }
#endif
    // no image on the command line: the one named beside the executable (chosen on an earlier start, or written by the
    // launcher); none yet, or that file has moved: the player picks it, and the choice is remembered beside the game
    if (iso.empty() && elf_path.empty()) {
        iso = te::startup::DiscImageBesideExecutable();
        std::error_code ec;
        if (iso.empty() || !std::filesystem::exists(iso, ec)) {
            iso = te::startup::ChooseDiscImage();
            if (!iso.empty() && !te::startup::RememberDiscImage(iso))
                std::fprintf(stderr, "[twinsanityevolved] the game's folder is read-only: the disc image is asked for again next start\n");
        }
    }
    if (iso.empty() && elf_path.empty()) {
        te::startup::ShowError("Crash Twinsanity", "No disc image was chosen. Start the game again and choose your disc image (the "
                                                   "European SLES-52568 or the North American SLUS-20909), or start it with --iso <image>.");
        Usage();
        return 2;
    }
    if (!g_save_requests.empty() || !load_state.empty()) {
        std::string warning;
        te::savestate_runner::EnsureNoAslr(argc, argv, &warning);   // re-execs (and never returns) when ASLR moved us
        if (!warning.empty()) std::fprintf(stderr, "[twinsanityevolved] savestate: %s\n", warning.c_str());
    }
    settings.iso_path = iso;
    settings.verbose = verbose;
    // The game folder (engine/app/host/game_folder.h): ps2r.ini beside the executable (macOS: beside the .app bundle) wins when
    // there is one -- the launcher's portable install, everything in one folder; else the earlier lookups (userdata/ beside
    // the game or <home>/.twinsanityevolved: the settings file beside the memory cards). --config / --mc-dir / --mods-dir
    // override for the run; paths.memcards / paths.mods in the file are applied once it is read (below).
    ps2r::app::GameFolder::Inputs folder_inputs;
    ps2r::app::GameFolder::Resolved folders = te::startup::LocateFolders(config_path, mc_dir_arg, mods_dir, &folder_inputs);
    // The disc's identity, by content (runner/te_disc_log.h: a log line, the directories and the size; --verify-disc hashes
    // the image), and the region-aware loader's row for its program (te_region.h): the program SYSTEM.CNF boots is the one
    // loaded, the row decides the field rate's default and how the data image is laid out; a release that needs an address
    // map it does not have is refused here with the sentence a player reads.
    ps2r::app::DiscIdentity disc_identity;
    const bool disc_identified = !iso.empty() && te::disc::LogIdentity(iso, &disc_identity, te::disc::VerifyRequested(verify_disc));
    const te::region::Selection region = te::region::Select(disc_identity, disc_identified, region_map_arg, folders.folder);
    std::fprintf(stderr, "[twinsanityevolved] %s\n", te::region::Describe(region).c_str());
    te::mods::SetDiscRegion(disc_identified ? ps2r::app::RegionName(disc_identity.region) : "");   // a refused mod of another region says so
    {
        const std::string names = te::region::PrepareDisc(region);   // before the disc opens (te::Disc's volume)
        if (!names.empty()) std::fprintf(stderr, "[twinsanityevolved] %s\n", names.c_str());
    }
    if (region.refused) {
        te::startup::ShowError("Crash Twinsanity", region.message);
        return 3;
    }
    if (!elf_in_iso_given && region.identified && !region.executable.empty()) elf_in_iso = region.executable;
    if (!hz_given) settings.field_hz = region.default_hz;
    if (mc_dir_arg.empty()) settings.mc_dir = folders.memcards;
    // the settings file ps2r.ini (the engine's schema: ConfigLoader reads it, SettingsFile writes one key) beside
    // the memory cards, <home>/.twinsanityevolved/ps2r.ini, or --config PATH. The file supplies what the command line did not
    // (the two languages; the Display page's video.* keys). A deterministic run (the judges, CI) never writes the
    // file unless --config names one; a play run creates it with the defaults on first run (the "first-run page" of a port
    // without a launcher: the file the player edits, the Options menu writes).
    {
        const bool explicit_config = !config_path.empty() && config_path != "none";
        if (config_path.empty()) config_path = folders.settings;   // beside the game (portable), else beside the memory cards
        if (config_path == "none") config_path.clear();
        te::display::Settings display;   // the engine's video.* keys the port honours; the defaults when there is no file
        bool legacy_language_form = false;   // ps2r.ini names both languages in game.language's retired "<text>+<speech>" form
        if (!config_path.empty()) {
            ps2r::Config cfg;
            cfg.video.vsync = false;   // the port's default (its pacer times the presents); the file's video.vsync overrides
            // game.hz: 0 = "not in the file" -- the engine's default 60 must not read as the player's choice (the
            // Vita's default is 50: kSettingsDefaultHz)
            cfg.game.hz = 0;
            std::string err;
            bool existed = false;
            int file_hz = 0;   // the file's game.hz when it names 50 or 60; 0 = not there (or no file, or unreadable)
            // a key the port reads itself and the engine's schema may not know is taken out before the engine's strict parse,
            // which refuses a whole file for one unknown key (TakePortKeys keeps every key the schema knows: display.tick_lock
            // is the schema's); the game folder's keys (paths.*, mods.order) are the schema's too and stay
            std::string text;
            {
                std::ifstream in(config_path, std::ios::binary);
                existed = static_cast<bool>(in);
                if (existed) text.assign(std::istreambuf_iterator<char>(in), {});
            }
            const bool applied = !existed || ps2r::ConfigLoader::ApplyIniText(te::startup::TakePortKeys(text, nullptr, nullptr), cfg, &err);
            if (!applied) {
                err = config_path + ": " + err;
                std::fprintf(stderr, "[twinsanityevolved] settings: %s not applied: %s\n", config_path.c_str(), err.c_str());
            } else if (existed) {
                // the languages: each row's own key, else the legacy game.language for both -- unless the command
                // line named a language for both (--language, --language-bank); --text-language / --speech-language name one
                {
                    const bool run_names_both = !settings.language_name.empty() || settings.language >= 0 || settings.language_bank >= 0;
                    if (!run_names_both) {
                        if (settings.text_language_name.empty()) settings.text_language_name = cfg.game.text_language;
                        if (settings.speech_language_name.empty()) settings.speech_language_name = cfg.game.speech_language;
                        settings.language_name = cfg.game.language;
                        legacy_language_form = cfg.game.text_language.empty() && cfg.game.speech_language.empty() &&
                                               cfg.game.language.find('+') != std::string::npos;
                    }
                }
                // game.hz: 50 (the PAL console's rate) or 60; anything else (or no key) is the default -- THE GAME RATE below
                // (the region's rate; the platform's settings default only on a disc that offers 50)
                if (cfg.game.hz == 50 || cfg.game.hz == 60) file_hz = cfg.game.hz;
                if (cfg.game.hz == 50 && !region.offers_50)
                    std::fprintf(stderr, "[twinsanityevolved] settings: game.hz = 50 is the PAL console's rate: this disc runs at %u\n", region.default_hz);
                display.aspect = int(cfg.video.aspect);
                display.fit = int(cfg.video.fit_mode);
                display.scale = cfg.video.scale > 0 ? cfg.video.scale : 1;
                display.texture_filter = int(cfg.video.texture_filter);
                display.vsync = cfg.video.vsync;
                display.fullscreen = int(cfg.video.fullscreen);
                display.offset_x = cfg.video.custom_offset_x;
                display.offset_y = cfg.video.custom_offset_y;
                {   // display.fps_counter / video.frame_rate -- read by name so the port still builds against an engine without the keys
                    std::string v;
                    if (cfg.Get("display.fps_counter", &v)) display.fps_counter = v == "on";
                    ps2r::ps2e::PresentPacer::Rate rate;
#if defined(__vita__)
                    // the Vita: the FRAME RATE row is the tick lock -- display.tick_lock = auto (60/30) | 60 | 30
                    // (te_tick_lock.h); the presenter shows every picture (video.frame_rate is not read: 60)
                    if (cfg.Get("display.tick_lock", &v)) display.tick_lock = v == "60" ? 1 : v == "30" ? 2 : 0;
#else
                    if (cfg.Get("video.frame_rate", &v) && ps2r::ps2e::PresentPacer::Parse(v.c_str(), &rate)) display.frame_rate = int(rate);
                    if (cfg.Get("video.interp", &v)) display.interp = !(v == "off" || v == "0" || v == "false");
#endif
                    (void)rate;
                    // mods.<id>.enabled for every built-in (the ids the framework knows; read by name). Registration only:
                    // the services are constructed after te::SetSettings (InstallBuiltins below adds the frame hook)
                    te::mods::RegisterBuiltins();
                    for (uint32_t i = 0; i < te::mods::Count(); ++i) {
                        const te::mods::Mod* m = te::mods::At(i);
                        if (cfg.Get("mods." + m->id + ".enabled", &v) && v == "on") mods_from_file.push_back(m->id);
                    }
                }
                if (verbose)
                    std::fprintf(stderr, "[twinsanityevolved] settings: %s read (game.text_language = %s, game.speech_language = %s, game.language = %s)\n",
                                 config_path.c_str(), cfg.game.text_language.c_str(), cfg.game.speech_language.c_str(), cfg.game.language.c_str());
            } else if (!settings.deterministic || explicit_config) {
                // first run: the file with the defaults, so the player finds it -- the two languages (the run's, else english)
                const std::string legacy = settings.language_name.empty() ? "english" : settings.language_name;
                const std::string text = settings.text_language_name.empty() ? legacy : settings.text_language_name;
                const std::string speech = settings.speech_language_name.empty() ? legacy : settings.speech_language_name;
                if (ps2r::SettingsFile::Set(config_path, "game.text_language", text, &err) &&
                    ps2r::SettingsFile::Set(config_path, "game.speech_language", speech, &err))
                    std::fprintf(stderr, "[twinsanityevolved] settings: %s created (game.text_language = %s, game.speech_language = %s)\n",
                                 config_path.c_str(), text.c_str(), speech.c_str());
                else
                    std::fprintf(stderr, "[twinsanityevolved] settings: %s not created: %s\n", config_path.c_str(), err.c_str());
            }
            // (the 50 Hz fix + the region map) THE GAME RATE: the file's game.hz when it names one (the Options' GAME RATE row writes
            // it) -- its 50 only on a disc that offers 50 (te_region.h: the North American disc runs at 60 whatever the file
            // says; the sentence above); without the key, the platform's settings default on a disc that offers 50 (the Vita's
            // 50: the PAL disc's own loop; the desktop's 60), else the region's rate. The row reads this value back; an --hz
            // decides for one run (the row then keeps the saved value and says it applies at the next start).
            if (file_hz) display.game_hz = file_hz == 50 && region.offers_50 ? 50 : int(region.default_hz);
            else display.game_hz = region.offers_50 ? kSettingsDefaultHz : int(region.default_hz);
            hz_from_settings = true;
            if (!hz_given) {
                settings.field_hz = unsigned(display.game_hz);
                const std::string disc_words = region.entry ? region.entry->disc_words : std::string("this disc");
                if (file_hz && (file_hz != 50 || region.offers_50))
                    std::snprintf(hz_source, sizeof hz_source, "ps2r.ini game.hz = %d", file_hz);
                else if (!region.offers_50)
                    std::snprintf(hz_source, sizeof hz_source, "%s runs at %u: %s", disc_words.c_str(), region.default_hz,
                                  file_hz ? "ps2r.ini's game.hz = 50 is the PAL console's rate" : "the Vita's default 50 is the PAL disc's rate");
                else std::snprintf(hz_source, sizeof hz_source, "%s: %s", kSettingsDefaultName,
                                   !existed ? "no ps2r.ini yet" : err.empty() ? "ps2r.ini has no game.hz" : "ps2r.ini not readable");
            }
            if (!settings.deterministic || explicit_config) settings.settings_path = config_path;
            // the retired one-key form of two languages, game.language = <text>+<speech> ('s "arabic+french"): read once as
            // the two names and the file rewritten with the two keys
            if (legacy_language_form && !settings.settings_path.empty()) {
                const std::string both = settings.language_name;
                const size_t plus = both.find('+');
                if (settings.text_language_name.empty()) settings.text_language_name = both.substr(0, plus);
                if (settings.speech_language_name.empty()) settings.speech_language_name = both.substr(plus + 1);
                if (ps2r::SettingsFile::Set(config_path, "game.text_language", settings.text_language_name, &err) &&
                    ps2r::SettingsFile::Set(config_path, "game.speech_language", settings.speech_language_name, &err) &&
                    ps2r::SettingsFile::Remove(config_path, "game.language", &err))
                    std::fprintf(stderr, "[twinsanityevolved] settings: game.language = %s rewritten as game.text_language = %s, game.speech_language = %s\n",
                                 both.c_str(), settings.text_language_name.c_str(), settings.speech_language_name.c_str());
                else
                    std::fprintf(stderr, "[twinsanityevolved] settings: game.language = %s not rewritten: %s\n", both.c_str(), err.c_str());
            }
        }
        // The engine's vsync default is ON (a Windows release blocker); the port's pacer times its presents, so the port's
        // default without a file is OFF (the engine's VideoConfig default is read only when the file exists and names it).
        if (!aspect_override.empty()) {
            static const char* const names[] = {"auto", "4:3", "16:9", "16:10", "21:9"};
            bool found = false;
            for (int i = 0; i < 5; ++i) if (aspect_override == names[i]) { display.aspect = i; found = true; }
            if (!found) { std::fprintf(stderr, "[twinsanityevolved] --aspect: auto|4:3|16:9|16:10|21:9\n"); return 2; }
        }
        if (fps_counter_override >= 0) display.fps_counter = fps_counter_override == 1;
        display.running_hz = settings.field_hz == 50 ? 50 : 60;
        // the GAME RATE row: the saved value (above) when a settings file is in play; without one nothing is saved -- the row
        // shows this run's rate
        if (!hz_from_settings) display.game_hz = display.running_hz;
#if defined(__vita__)
        // the device log's first word on the game loop: its rate and where it came from (the built-in arguments
        // carry no --hz -- ps2r.ini's game.hz decides, the Options' GAME RATE row; without the key the Vita's default 50, the
        // PAL disc's own rate; an args.txt's --hz for its run; --config none: the engine's 60)
        {
            char saved[96] = "";
            if (hz_given && hz_from_settings)
                std::snprintf(saved, sizeof saved, "; the GAME RATE setting is %d Hz, from the next start without --hz", display.game_hz);
            std::fprintf(stderr, "[twinsanityevolved] vita: game rate %u Hz (%s%s)%s\n", settings.field_hz == 50 ? 50u : 60u,
                         hz_given ? "--hz on the command line" : hz_source, saved,
                         settings.field_hz == 50 ? " -- the PAL disc's own game loop: 50 ticks a second on the pacer's 20 ms timer, "
                                                   "each picture shown from the first 60 Hz refresh after it is ready" : "");
        }
#endif
        (void)hz_source;
        (void)kSettingsDefaultName;
        // --frame-rate for the run (the judges' arms), else TE_FRAME_RATE (a judge script that cannot add arguments)
        if (frame_rate_override.empty() && std::getenv("TE_FRAME_RATE")) frame_rate_override = std::getenv("TE_FRAME_RATE");
        if (!frame_rate_override.empty()) {
#if defined(__vita__)
            // the Vita: auto | 60 | 30 -- the tick lock (te_tick_lock.h); at the 50 Hz game rate its rates are 50 / 25 (the
            // same choices: the full rate and the half), so 50 and 25 name them too
            if (frame_rate_override == "auto") display.tick_lock = 0;
            else if (frame_rate_override == "60" || frame_rate_override == "50") display.tick_lock = 1;
            else if (frame_rate_override == "30" || frame_rate_override == "25") display.tick_lock = 2;
            else {
                std::fprintf(stderr, "[twinsanityevolved] --frame-rate: auto|60|30 (auto|50|25 at --hz 50) on the Vita\n");
                return 2;
            }
#else
            ps2r::ps2e::PresentPacer::Rate rate;
            if (!ps2r::ps2e::PresentPacer::Parse(frame_rate_override.c_str(), &rate)) {
                std::fprintf(stderr, "[twinsanityevolved] --frame-rate: 30|60|120|144|240|uncapped\n");
                return 2;
            }
            display.frame_rate = int(rate);
#endif
        }
        te::display::Configure(display);
        te::texts::InstallFpsCounter();
    }
    // the speedrun mod's practice states load in the SAME process (no re-exec), but a state saved in one play session must
    // load in the next: the runner runs without ASLR whenever the mod is on at start-up (as it does for --save-state / --load-state)
    {
        bool speedrun = false;
        for (const std::string& id : mods_on) speedrun |= id == "speedrun";
        for (const std::string& id : mods_from_file) speedrun |= id == "speedrun";
        if (speedrun && g_save_requests.empty() && load_state.empty()) {
            std::string warning;
            te::savestate_runner::EnsureNoAslr(argc, argv, &warning);
            if (!warning.empty()) std::fprintf(stderr, "[twinsanityevolved] savestate: %s\n", warning.c_str());
        }
    }
    // The mods folder (te_mods.h FOLDER MODS) and the memory cards as the settings file places them (paths.mods /
    // paths.memcards, relative to its folder) unless --mods-dir / --mc-dir named them: default <the settings file's
    // folder>/mods; a run without a settings file (--config none: the judges, CI) reads a mods folder only when --mods-dir
    // names one. The folder mods' switches (mods.<id>.enabled) and their disc overlay's order (mods.order) come from
    // ps2r.ini like the built-ins' switches.
    std::string mods_order;
    std::vector<std::string> folder_mods_on;
    std::string save_dir_from_file;   // ps2r.ini's paths.save_dir (F5 / F8's slots; "" = the game folder)
    int save_slot = 0;                // ps2r.ini's save.slot
    if (!config_path.empty()) {
        ps2r::Config cfg;
        std::string err;
        bool existed = false;
        if (ps2r::ConfigLoader::ApplyIniFile(config_path, cfg, &err, &existed) && existed) {
            ps2r::app::GameFolder::ApplySettings(folders, folder_inputs, cfg.paths.memcards, cfg.paths.mods);
            if (mc_dir_arg.empty()) settings.mc_dir = folders.memcards;
            mods_order = cfg.mods.order;
            save_dir_from_file = cfg.paths.save_dir;
            save_slot = cfg.save.slot > 0 ? cfg.save.slot : 0;
            for (const auto& kv : cfg.mods.enabled)
                if (kv.second) folder_mods_on.push_back(kv.first);
        }
    }
    mods_dir = folders.mods;
    std::fprintf(stderr, "[twinsanityevolved] %s\n", ps2r::app::GameFolder::Describe(folders).c_str());
    // another release's disc keeps its own memory card beside the European one's: the European C++ names the card's
    // directory by its own literals ("BE" + "SLES-52568") while the program's data names the save by this disc's product
    // code, so the two discs' saves would share one directory and each would overwrite the other's banks
    if (region.entry && !region.native && !region.refused && !settings.mc_dir.empty()) {
        settings.mc_dir += "-" + region.entry->serial;
        std::fprintf(stderr, "[twinsanityevolved] saves: %s keeps its own memory card: %s\n", region.entry->disc_words.c_str(), settings.mc_dir.c_str());
    }
    te::SetSettings(settings);
    // the built-in mods (all off unless ps2r.ini or --mod says so; the switches persist through the Options > Mods page),
    // then the mods folder's beside them
    te::mods::InstallBuiltins();
    // F5 / F8 (te_savestate_keys.h): the player's save state in the GAME FOLDER (the folder holding ps2r.ini: the launcher's
    // game folder, the Vita's ux0:data/twinsanity) unless ps2r.ini names paths.save_dir (relative to that folder); a run without
    // a settings file (--config none) keeps its slots beside its memory cards' folder, else in the data folder
    {
        std::string root = save_dir_from_file;
        std::error_code ec;
        if (!root.empty() && !std::filesystem::path(root).is_absolute() && !folders.folder.empty())
            root = (std::filesystem::path(folders.folder) / root).generic_string();
        if (root.empty()) root = folders.folder;
        if (root.empty() && !settings.mc_dir.empty()) {
            const std::filesystem::path parent = std::filesystem::path(settings.mc_dir).parent_path();
            root = parent.empty() ? std::string(".") : parent.generic_string();
        }
        if (root.empty()) root = te::startup::DataDirectory();
        te::savestate_keys::Install(root, save_slot);
    }
    te::mods::ScanFolder(mods_dir);
    te::mods::SetOrder(mods_order);
    mods_from_file.insert(mods_from_file.end(), folder_mods_on.begin(), folder_mods_on.end());
    te::mods::Configure(mods_from_file);
    // a language pack is not a mod: it has no switch -- an earlier version's mods.<id>.enabled and --mod <id> change
    // nothing (it is offered in GAME OPTIONS > TEXT LANGUAGE whenever it is in the mods folder)
    auto language_pack = [](const std::string& id) {
        for (const te::mods::Info& p : te::mods::LanguagePacks()) if (p.id == id) return true;
        return false;
    };
    for (const std::string& id : folder_mods_on)
        if (language_pack(id))
            std::fprintf(stderr, "[twinsanityevolved] mods: mods.%s.enabled ignored: %s is a language pack, not a mod (a text language of the game's options)\n",
                         id.c_str(), id.c_str());
    for (const std::string& id : mods_on) {
        bool known = false;
        for (uint32_t i = 0; i < te::mods::Count(); ++i) if (te::mods::At(i)->id == id) { te::mods::SetEnabled(i, true, false); known = true; }
        if (!known && language_pack(id)) {
            std::fprintf(stderr, "[twinsanityevolved] --mod %s: %s is a language pack, not a mod: it is offered as a text language "
                                 "(GAME OPTIONS > TEXT LANGUAGE, --text-language %s) -- nothing to switch on\n", id.c_str(), id.c_str(), id.c_str());
            continue;
        }
        if (!known) { std::fprintf(stderr, "[twinsanityevolved] --mod %s: no built-in mod and no mods folder mod of that id (%s)\n", id.c_str(), te::mods::Describe().c_str()); return 2; }
    }
    std::fprintf(stderr, "[twinsanityevolved] %s\n", te::mods::Describe().c_str());
    std::fprintf(stderr, "[twinsanityevolved] %s\n", te::mods::DescribeFolders().c_str());
    te::audio::Configure(!no_audio);
    if (!harness_out.empty()) {
        char build_id[64];
        std::snprintf(build_id, sizeof build_id, "twinsanityevolved %08x", te::savestate_runner::BuildId());
        std::string error;
        if (!te::harness::Arm(harness_out, build_id, &error)) { std::fprintf(stderr, "[twinsanityevolved] --harness-out: %s\n", error.c_str()); return 2; }
        te::harness::Set("disc", iso.empty() ? elf_path : iso);
        te::harness::Set("elf", elf_in_iso);
        te::harness::Set("field_hz", std::to_string(settings.field_hz));
        te::harness::Set("skip_movies", settings.skip_movies ? "on" : "off");
        te::harness::Set("stream_model", settings.stream_instant ? "instant" : "drive");
        te::harness::Set("language_request", settings.language_name.empty() ? std::to_string(settings.language) : settings.language_name);
        if (!settings.text_language_name.empty() || !settings.speech_language_name.empty())
            te::harness::Set("language_rows_request", "text=" + settings.text_language_name + ",speech=" + settings.speech_language_name);
        te::harness::Set("play_pad", play_pad);
        te::harness::Set("pad_script", pad_script);
        te::harness::Set("load_state", load_state);
        std::fprintf(stderr, "[twinsanityevolved] harness leg: %s\n", harness_out.c_str());
    }

#if defined(TE_NATIVE)
    // 1-2. the Vita's native build (te_native.h): no module to instantiate -- the game's code is native ARM in this eboot and
    // its retail address space is the guest block (the retail image at block + its retail address, the heap above it)
    te::BindGuestBlock(g_env);
    std::fprintf(stderr, "[twinsanityevolved] module: the game's code is NATIVE ARM (TE_VITA_NATIVE); guest block %u MB at %p "
                         "(retail address X at block + X), heap base block + 0x003DB200, %u retail functions in the table\n",
                 unsigned(g_env.memory.size >> 20), static_cast<void*>(g_env.memory.data), te::native::RetailFunctionCount());
    te::vita::ReportModuleCode(g_env.memory.data);
#else
    // 1. the engine's memory IS the module's memory
    MainMemory ram;
    te::BindMainMemory(g_env, ram);

    // 2. the module
    wasm_rt_init();
    w2c_game& instance = g_instance;
    wasm2c_game_instantiate(&instance, &g_env);
    g_env.instance = &instance;
    te_diag_env = &g_env;
    std::fprintf(stderr, "[twinsanityevolved] module: memory %u MB bound to MainMemory, heap base 0x%08X, data end 0x%08X, retail image at 0x%08X, %u retail functions in the table\n",
                 unsigned(g_env.memory.size >> 20), *w2c_game_0x5F_heap_base(&instance), *w2c_game_0x5F_data_end(&instance),
                 *w2c_game_te_retail_image(&instance),
                 *reinterpret_cast<const uint32_t*>(g_env.memory.data + *w2c_game_te_retail_function_count(&instance)));
#if defined(__vita__)
    te::vita::ReportModuleCode(g_env.memory.data);
#endif
#endif

    // 3. the retail data image, from the disc at run time: the program SYSTEM.CNF boots (the region row's), laid out as the
    // row says -- the PAL layout as it is, another release's through its address map (te_region.h)
    std::vector<uint8_t> elf;
    std::string error;
    if (!iso.empty()) {
        // through the services' disc: the image, or the mods' overlay composed over it (a mod may carry its own executable)
        te::Disc& disc = te::Services::Get().disc();
        disc.Initialise();
        if (!disc.IsOpen() || !te::ReadElfFromDisc(disc.Iso(), elf_in_iso, elf, &error)) {
            if (!disc.IsOpen()) error = disc.Error();
            te::startup::ShowError("Crash Twinsanity", error + " -- a disc image of the game is needed (the European SLES-52568 or the North American SLUS-20909).");
            return 3;
        }
    } else {
        std::ifstream f(elf_path, std::ios::binary);
        if (!f) { std::fprintf(stderr, "[twinsanityevolved] cannot read %s\n", elf_path.c_str()); return 3; }
        elf.assign(std::istreambuf_iterator<char>(f), {});
    }
    te::RetailImage image;
    if (!te::region::LoadImage(g_env, elf, region, image, &error)) {
        std::fprintf(stderr, "[twinsanityevolved] retail image: %s\n", error.c_str());
        if (!region.native) te::startup::ShowError("Crash Twinsanity", error);   // another release's program the map is not for
        return 4;
    }
    // diagnosis (te_math_diag.h, TE_MATH_TRANSFORM_COMPARE / _MICRO): the retail VuTransformPoint (MultiplyMatrixByVector) and
    // VuRotateVector (TransformVector_) words of the PAL executable, for the engine's VU interpreter
    if (te::math::MacroDiagRequested()) {
        std::vector<uint32_t> point_words, rotate_words;
        if (region.native && te::ReadRetailWords(elf, 0x0018EC80u, 16, point_words) && te::ReadRetailWords(elf, 0x0018EC48u, 14, rotate_words))
            te::math::SetRetailMacroWords(point_words.data(), point_words.size(), rotate_words.data(), rotate_words.size());
        else
            std::fprintf(stderr, "[te.math] the retail VU0 functions are not available (the PAL executable's words at 0x18EC48 / 0x18EC80)\n");
    }

#if defined(TE_NATIVE)
    // 3a. the retail image's data pointers made native: block + the retail address (the reviewed list of words)
    {
        const te::native::DataRelocationReport dr = te::native::RelocateRetailDataPointers(g_env);
        std::fprintf(stderr, "[twinsanityevolved] retail data pointers: %u relocated to the guest block, %u listed words hold no image "
                             "address (left as they are), %u outside the pointer sections, %u unlisted words with an image-range value "
                             "(text and packed halves; left as they are)\n",
                     dr.relocated, dr.not_an_address, dr.out_of_range, dr.unlisted_candidates);
        for (uint32_t a : dr.examples) std::fprintf(stderr, "  listed word 0x%08X holds no image address\n", a);
    }
#endif
    // 4. retail code pointers -> table indices (natively: the functions' addresses)
    std::vector<uint32_t> starts(te_known_function_starts, te_known_function_starts + te_known_function_start_count);
    te::PatchReport rep = te::PatchRetailCodePointers(g_env, starts);
    std::fprintf(stderr, "[twinsanityevolved] retail image loaded (%lu-byte ELF); code pointers: %u words scanned, %u patched, %u unported bodies, %u stray in code range\n",
                 (unsigned long)elf.size(), rep.words_scanned, rep.patched, rep.unported_hits, rep.stray_code_range);   // (no %zu: the Vita's newlib printf has none)
    if (verbose) {
        for (auto& [where, fn] : rep.unported) std::fprintf(stderr, "  unported: word 0x%08X -> retail function 0x%08X\n", where, fn);
    }
    // 4a. The language setting: the disc's text banks, their contents and the selection (the retail image names the slots, the disc the banks)
    std::fprintf(stderr, "[twinsanityevolved] %s\n", te::language::Describe().c_str());
#if defined(TE_NATIVE)
    // 4b. no vtable thunks natively: every body takes a prefix of its slot's arguments (AAPCS-VFP, te_native.h)
    std::fprintf(stderr, "[twinsanityevolved] vtable thunks: none (native: a body reads the prefix of the arguments it declares)\n");
    if (dry_run) {
        std::fprintf(stderr, "[twinsanityevolved] --dry-run: not entering the game\n");
        return 0;
    }
    w2c_game* const game_instance = nullptr;
#else
    // 4b. vtable slots whose body's type differs from the slot's declared type -> thunks of the declared type
    te::VtableThunkReport vt = te::PatchVtableThunks(g_env, verbose);
    std::fprintf(stderr, "[twinsanityevolved] vtable thunks: %u slots checked, %u matched, %u thunked, %u pure-virtual, %u beyond vtable end, %u unported, %u pool exhausted, %u duplicate rows\n",
                 vt.slots_checked, vt.matched, vt.thunked, vt.pure, vt.beyond_vtable, vt.unported, vt.pool_exhausted, vt.already);
    for (const std::string& s : vt.pool_exhausted_signatures) std::fprintf(stderr, "  POOL EXHAUSTED for %s -- raise TE_VT_POOL (CMake) and rebuild\n", s.c_str());
    if (dry_run) {
        std::fprintf(stderr, "[twinsanityevolved] --dry-run: not entering the game\n");
        wasm2c_game_free(&instance);
        return 0;
    }
    w2c_game* const game_instance = &instance;
#endif

    // 4c. the game fiber + the save-state sections + the checkpoint handler
    std::string fiber_error;
    if (!te::game::Prepare(game_instance, test_trap, &fiber_error)) { std::fprintf(stderr, "[twinsanityevolved] %s\n", fiber_error.c_str()); return 5; }
    if (verbose || !g_save_requests.empty() || !load_state.empty()) {
        const te::game::Fiber& f = te::game::Get();
        const te::savestate_runner::ImageInfo img = te::savestate_runner::Image();
        std::fprintf(stderr, "[twinsanityevolved] fiber: game stack %p (%zu MB, %s), image 0x%llx+0x%llx (slide 0x%llx), memory %p, instance %p\n",
                     static_cast<void*>(f.stack), f.stack_size >> 20, f.at_hint ? "at the fixed address" : "NOT at the fixed address",
                     (unsigned long long)img.base, (unsigned long long)img.size, (unsigned long long)img.slide,
                     static_cast<void*>(g_env.memory.data), static_cast<void*>(game_instance));
#if !defined(TE_NATIVE)
        if (verbose)
            std::fprintf(stderr, "[twinsanityevolved] fiber: env %p, env.memory %p, instance.memory %p, table data %p (%u entries)\n",
                         static_cast<void*>(&g_env), static_cast<void*>(&g_env.memory), static_cast<void*>(instance.w2c_env_memory),
                         static_cast<void*>(instance.w2c_0x5F_indirect_function_table.data), instance.w2c_0x5F_indirect_function_table.size);
#endif
    }
#if !defined(TE_NATIVE)
    te::savestate_runner::Register(g_env, instance);   // (the Vita's runner refuses every state: te_savestate_null.cpp)
#endif
    te::savestate::SetCheckpointHandler(OnCheckpoint);

    if (run_seconds) {
        // The watchdog: the game yields at its next checkpoint (OnCheckpoint sees the flag); a game that never reaches
        // a boundary again is left by the alarm's second firing.
#if defined(_WIN32) || defined(__vita__)
        // No SIGALRM (Win32; newlib on the Vita): a detached thread plays the alarm (the same two firings, 2 s apart).
        std::thread([run_seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
            g_watchdog = 1;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            _exit(0);
        }).detach();
#else
        std::signal(SIGALRM, [](int) {
            if (g_watchdog) _exit(0);
            g_watchdog = 1;
            alarm(2);
        });
        alarm(run_seconds);
#endif
    }
    if (press) te::Services::Get().pads().HoldTestButtons(press);
    if (stop_at) {
        te::Services::Get().clock().AddBoundaryHook("stop-at", [stop_at](uint64_t boundary) {
            if (boundary >= stop_at) { std::fprintf(stderr, "[twinsanityevolved] --stop-at %llu reached\n", (unsigned long long)boundary); g_watchdog = 1; }
        });
    }
#if defined(TE_W2C_COUNT)
    {
        // A diagnosis build (CMake TE_VITA_W2C_COUNT): what the module executed per boundary, every TE_W2C_COUNT_EVERY
        // boundaries (default 150) -- the dynamic side of the static instruction census
        const char* ev = std::getenv("TE_W2C_COUNT_EVERY");
        const uint64_t every = ev && std::atoi(ev) > 0 ? uint64_t(std::atoi(ev)) : 150;
        te::Services::Get().clock().AddBoundaryHook("w2c count", [every](uint64_t boundary) {
            static uint32_t last[8] = {};
            static uint64_t from = 0;
            if (boundary < from + every) return;
            uint32_t now[8];
            te_w2c_count_read(now);
            const double n = double(boundary - from);
            std::fprintf(stderr, "[te.w2c.count] boundaries %llu-%llu per boundary: calls %.0f, loads+stores %.0f (f32 %.0f), abs %.1f, "
                                 "float->int %.1f, call_indirect %.1f, sqrt %.1f\n",
                         (unsigned long long)from, (unsigned long long)boundary, (now[0] - last[0]) / n, (now[1] - last[1]) / n,
                         (now[2] - last[2]) / n, (now[3] - last[3]) / n, (now[4] - last[4]) / n, (now[5] - last[5]) / n,
                         (now[6] - last[6]) / n);
            std::memcpy(last, now, sizeof last);
            from = boundary;
        });
        std::fprintf(stderr, "[twinsanityevolved] TE_W2C_COUNT: the module's operations counted (a diagnosis build: slower)\n");
    }
#endif
    if (const char* mh = std::getenv("TE_DIAG_MEMHASH")) {
        // TE_DIAG_MEMHASH=FROM-TO[@KB][:START+LEN] (diagnosis): at every boundary in [FROM, TO] one line with a 32-bit hash of
        // each KB-kilobyte block of the guest memory (default 1024: 32 blocks; START / LEN in hex: only that range) -- two
        // platforms' runs of one deterministic route diffed line by line give the first boundary and block where their game
        // states part
        unsigned long long from = 0, to = 0, kb = 1024, start = 0, len = 0;
        std::sscanf(mh, "%llu-%llu@%llu:%llx+%llx", &from, &to, &kb, &start, &len);
        if (kb < 1) kb = 1;
        te::Services::Get().clock().AddBoundaryHook("diag memhash", [from, to, kb, start, len](uint64_t boundary) {
            if (boundary < from || boundary > to) return;
            const uint8_t* mem = te::GuestMemory();
            const size_t size = te::GuestMemorySize(), block = size_t(kb) * 1024;
            if (!mem || !size) return;
            const size_t lo = std::min<size_t>(size, size_t(start)), hi = len ? std::min<size_t>(size, size_t(start + len)) : size;
            std::string line = "[te.memhash] b " + std::to_string(boundary) + ":";
            for (size_t at = lo; at < hi; at += block) {
                uint32_t h = 2166136261u;
                const size_t end = std::min(hi, at + block);
                for (size_t i = at; i + 4 <= end; i += 4) {
                    uint32_t w;
                    std::memcpy(&w, mem + i, 4);
                    h = (h ^ w) * 16777619u;
                }
                char hex[12];
                std::snprintf(hex, sizeof hex, " %08x", h);
                line += hex;
            }
            std::fprintf(stderr, "%s\n", line.c_str());
        });
    }
    if (const char* md = std::getenv("TE_DIAG_MEMDUMP")) {
        // TE_DIAG_MEMDUMP=B:START+LEN (diagnosis, hex START / LEN): at boundary B the guest words of that range, 16 a line
        // ("[te.memdump] b B 0xADDR: w0 .. w15") -- two platforms' dumps diffed find the words a memhash block names
        unsigned long long at = 0, start = 0, len = 0;
        std::sscanf(md, "%llu:%llx+%llx", &at, &start, &len);
        te::Services::Get().clock().AddBoundaryHook("diag memdump", [at, start, len](uint64_t boundary) {
            if (boundary != at) return;
            const uint8_t* mem = te::GuestMemory();
            const size_t size = te::GuestMemorySize();
            if (!mem) return;
            for (size_t a = size_t(start) & ~size_t(63); a < size_t(start + len) && a + 64 <= size; a += 64) {
                char line[256];
                int n = std::snprintf(line, sizeof line, "[te.memdump] b %llu 0x%08zx:", (unsigned long long)boundary, a);
                for (int k = 0; k < 16 && n > 0 && n < int(sizeof line) - 10; ++k) {
                    uint32_t w;
                    std::memcpy(&w, mem + a + size_t(k) * 4, 4);
                    n += std::snprintf(line + n, sizeof line - size_t(n), " %08x", w);
                }
                std::fprintf(stderr, "%s\n", line);
            }
        });
    }
    if (close_at) {
        te::Services::Get().clock().AddBoundaryHook("close-at", [close_at](uint64_t boundary) {
            static bool posted = false;
            if (posted || boundary < close_at) return;
            posted = true;
            std::fprintf(stderr, "[twinsanityevolved] --close-at %llu: window close posted (%s)\n", (unsigned long long)boundary,
                         te::input::RequestWindowClose() ? "SDL_QUIT queued" : "no SDL event queue");
        });
    }
    // The pad log (fixtures/padlogs): record pad 0's samples, play a recording back, or run an authoring script
    std::string pad_error;
    if (!play_pad.empty() && !te::padlog::Play(play_pad, &pad_error)) { std::fprintf(stderr, "[twinsanityevolved] padlog: %s\n", pad_error.c_str()); return 2; }
    if (!pad_script.empty() && !te::padlog::Script(pad_script, &pad_error)) { std::fprintf(stderr, "[twinsanityevolved] padlog: %s\n", pad_error.c_str()); return 2; }
    if (!autopilot.empty() && !te::autopilot::Load(autopilot, &pad_error)) { std::fprintf(stderr, "[twinsanityevolved] autopilot: %s\n", pad_error.c_str()); return 2; }
    if (!record_pad.empty() && !te::padlog::Record(record_pad, &pad_error)) { std::fprintf(stderr, "[twinsanityevolved] padlog: %s\n", pad_error.c_str()); return 2; }
    if (trace_player) te::probe::Install(trace_player, dump_collision);
    if (trace_bodies) te::probe::InstallBodies(trace_bodies);
    if (trace_state) {
        // The game controller's state word (te::harness::ReadGameState: include/game/gamecontroller.h GameControllerStates
        // through the retail global G_GameController) at every boundary: one line per change, so a run log records when
        // StateTitle (7) was reached and for how many boundaries it held. (A harness leg writes the same to state.trace.)
        te::Services::Get().clock().AddBoundaryHook("trace-state", [](uint64_t boundary) {
            static uint64_t last = ~0ull;
            unsigned state = 0, lastState = 0, nextState = 0;
            if (!te::harness::ReadGameState(&state, &lastState, &nextState)) return;
            const uint64_t key = (uint64_t(lastState) << 12) | (uint64_t(state) << 6) | nextState;
            if (key == last) return;
            last = key;
            std::fprintf(stderr, "[twinsanityevolved] game state @ boundary %llu: %s (%u) <- %s (%u), next %s (%u)\n",
                         (unsigned long long)boundary, te::harness::StateName(state), state, te::harness::StateName(lastState), lastState,
                         te::harness::StateName(nextState), nextState);
        });
    }
    // 4d. a loaded state replaces memory, module globals, the services' state and the fiber's stack + registers
    if (!load_state.empty()) {
        std::string error;
        if (!te::savestate_runner::Load(load_state, &error)) { std::fprintf(stderr, "[twinsanityevolved] --load-state: %s\n", error.c_str()); return 6; }
    }
    // 5. run: the fiber yields at checkpoints (a save is due), at Exit, or when entry returns/traps.
    // The game's thread (this one: the fiber runs on it) at the interactive quality of service: a paced frame has
    // ~8 ms of work in a 16.7 ms field and a shared, loaded host preempts a default-QoS thread for longer than that
    // (ticks of 17-18 ms seen under load 5+ with nothing of the frame's own in them); interactive keeps it on the
    // performance cores and first in line. TE_NO_QOS=1 leaves the default (comparison runs).
    // One arm per platform in the engine (platform/host PinCurrentThreadInteractive: macOS QOS_CLASS_USER_INTERACTIVE,
    // Windows SetThreadPriority, Linux the name): the port names the thread and asks for the class.
    if (!std::getenv("TE_NO_QOS")) ps2r::platform::host::PinCurrentThreadInteractive("te-game", ps2r::platform::host::ThreadRole::Game);
    // The EE's FPU rounds every result toward zero (no IEEE rounding modes); the game's f32 arithmetic compiled to wasm
    // runs under the host's round-to-nearest unless the game thread's FPU is set to round toward zero -- the DEFAULT
    // since the clock-rate fix (TimeClockAdvanceBy's 150 vs retail's 149 clock units a frame -- the game plays at retail speed;
    // the pad logs are recorded under it). TE_EE_ROUNDING=0 keeps the host's round-to-nearest (A/B, the pre-flip logs).
    const char* ee = std::getenv("TE_EE_ROUNDING");
    if (!(ee && *ee == '0')) {
        if (std::fesetround(FE_TOWARDZERO) != 0) std::fprintf(stderr, "[twinsanityevolved] fesetround(FE_TOWARDZERO) refused\n");
        else std::fprintf(stderr, "[twinsanityevolved] fpu: rounding toward zero on the game thread (the EE's mode; TE_EE_ROUNDING=0 opts out)\n");
    } else {
        std::fprintf(stderr, "[twinsanityevolved] fpu: the host's round-to-nearest on the game thread (TE_EE_ROUNDING=0)\n");
    }
    te::prof::StartFromEnv();   // TE_PROF=1: the sampling section profiler (te_prof.h); TE_PROF_BENCH=1: primitive timings
#if defined(__vita__)
    te::vita::Step("entering the game fiber (module instantiated; the game's main runs next)");
#endif
    for (;;) {
        const te::game::Reason reason = te::game::Resume();
        const te::game::Fiber& f = te::game::Get();
        if (reason == te::game::Reason::Checkpoint) {
            std::string error;
            const bool saved = te::savestate_runner::Save(g_save_now, f.boundary, &error);
            if (!saved) std::fprintf(stderr, "[twinsanityevolved] savestate: %s not saved: %s\n", g_save_now.c_str(), error.c_str());
            if (saved && te::savestate_keys::IsPendingSave(g_save_now)) {
                // F5's slot: its sidecar names the build, the boundary and where the player is (a state attached to a report)
                std::string detail, meta_error;
                te::probe::State where;
                if (te::probe::Read(where)) {
                    char place[96];
                    std::snprintf(place, sizeof place, "place=%.3f %.3f %.3f\n", where.x, where.y, where.z);
                    detail = "level=" + where.chunk_path + "\n" + place + "game_state=" + std::to_string(where.game_state) + "\n";
                }
                if (te::savestate_runner::WriteSidecar(g_save_now, f.boundary, detail, &meta_error))
                    std::fprintf(stderr, "[twinsanityevolved] savestate: %s.meta written (%s)\n", g_save_now.c_str(),
                                 where.chunk_path.empty() ? "no level loaded" : where.chunk_path.c_str());
                else
                    std::fprintf(stderr, "[twinsanityevolved] savestate: %s.meta not written: %s\n", g_save_now.c_str(), meta_error.c_str());
            }
            te::savestate_keys::Saved(g_save_now, saved, error);   // F5's own answer on screen (the others' are the log's)
            continue;
        }
        if (reason == te::game::Reason::LoadState) {   // F8 and the speedrun mod's practice ring -- restored in place, same process
            std::string error;
            const bool loaded = te::savestate_runner::Load(g_load_now, &error);
            if (!loaded)
                std::fprintf(stderr, "[twinsanityevolved] savestate: %s not loaded: %s (the game continues)\n", g_load_now.c_str(), error.c_str());
            else
                std::fprintf(stderr, "[twinsanityevolved] savestate: %s loaded in place\n", g_load_now.c_str());
            te::savestate_keys::Loaded(g_load_now, loaded, error);
            continue;
        }
        if (reason == te::game::Reason::Exit) {
            if (g_watchdog) std::fprintf(stderr, "[twinsanityevolved] %s: exit 0\n", stop_at ? "--stop-at" : "--run-seconds elapsed");
            g_exit_status = g_watchdog ? 0 : f.code;
        } else if (reason == te::game::Reason::Returned) {
            std::fprintf(stderr, "[twinsanityevolved] entry returned (unexpected)\n");
            g_exit_status = 1;
        } else {
            std::fprintf(stderr, "[twinsanityevolved] wasm trap: %s\n", te_trap_name(f.code));
            g_exit_status = 70;
        }
        break;
    }
    te::padlog::Finish();
    {
        const te::Clock::Census c = te::Services::Get().clock().GetCensus();
        std::fprintf(stderr, "[twinsanityevolved] clock: %llu boundaries, %llu fields, %llu parks, %llu late, %llu timeouts (%s %u Hz); catch-up %llu boundaries, %llu fields dropped\n",
                     (unsigned long long)c.boundaries, (unsigned long long)c.fields, (unsigned long long)c.parks,
                     (unsigned long long)c.late, (unsigned long long)c.timeouts, c.play ? "play" : "deterministic", c.field_hz,
                     (unsigned long long)c.catchup_boundaries, (unsigned long long)c.dropped_fields);
        std::fprintf(stderr, "[twinsanityevolved] ticks: %llu measured, %llu over the field (%.1f ms), max %.1f ms, mean %.2f ms, cpu mean %.2f ms\n",
                     (unsigned long long)c.ticks_measured, (unsigned long long)c.ticks_over_field,
                     1000.0 / double(c.field_hz ? c.field_hz : 60u), c.tick_max_ms,
                     c.ticks_measured ? c.tick_sum_ms / double(c.ticks_measured) : 0.0,
                     c.ticks_measured ? c.tick_cpu_sum_ms / double(c.ticks_measured) : 0.0);
        // the game's own clock against the wall: Platform::Time's T0 (576000 a second of game time) since its
        // Initialise, over the wall seconds to the last boundary (play) -- 100 % when the game runs in real time
        if (c.play && c.game_wall_s > 0)
            std::fprintf(stderr, "[twinsanityevolved] game time: %.3f s on the game's clock (T0 %llu counts, %llu a field at %u Hz) in %.3f s of wall (%.2f %%)\n",
                         double(c.game_counts) / 576000.0, (unsigned long long)c.game_counts, (unsigned long long)c.counts_per_field,
                         c.field_hz, c.game_wall_s, 100.0 * double(c.game_counts) / 576000.0 / c.game_wall_s);
        else
            std::fprintf(stderr, "[twinsanityevolved] game time: %.3f s on the game's clock (T0 %llu counts, %llu a field at %u Hz) over %llu boundaries (%s)\n",
                         double(c.game_counts) / 576000.0, (unsigned long long)c.game_counts, (unsigned long long)c.counts_per_field,
                         c.field_hz, (unsigned long long)c.boundaries, c.play ? "play" : "deterministic");
#if defined(__vita__)
        std::fprintf(stderr, "[twinsanityevolved] %s\n", te::tick_lock::ExitLine().c_str());   // te_tick_lock.h
#endif
        te::audio::Report();   // the audio/stream/movie census (dropped=0/underruns=0)
        // the presenter's row: present=rate:shown_fps:interp_frames:snaps:latency_ms:late_presents (the frame-rate judge reads it)
        char present_row[2048] = {};
        te_graphics_present_census(present_row, sizeof present_row);
        std::fprintf(stderr, "[twinsanityevolved] %s\n", present_row);
        if (te::harness::Armed()) {
            te::harness::Set("present", std::string(present_row));
            te::harness::Set("clock", "boundaries=" + std::to_string(c.boundaries) + ",fields=" + std::to_string(c.fields) + ",parks=" +
                                          std::to_string(c.parks) + ",late=" + std::to_string(c.late) + ",timeouts=" + std::to_string(c.timeouts) +
                                          "," + (c.play ? "play" : "deterministic") + "," + std::to_string(c.field_hz) + "hz");
            const te::audio::RingHealth h = te::audio::Health();
            te::harness::Set("audio_ring", "pushed=" + std::to_string(h.pushed) + ",pulled=" + std::to_string(h.pulled) + ",dropped=" +
                                               std::to_string(h.dropped) + ",underruns=" + std::to_string(h.underruns) + ",overruns=" +
                                               std::to_string(h.overruns) + ",padded=" + std::to_string(h.padded) + ",device=" + (h.device ? "1" : "0"));
            te::harness::Set("exit_status", std::to_string(g_exit_status));
            te::harness::Set("watchdog", g_watchdog ? "1" : "0");
        }
        // the language the game ENDED in -- its current Code text table's line 9 (the main menu's "new game",
        // game/include/game/language.h g_Texts, PAL D_00309B10) beside the selection; the frozen judge reads this line.
        {
            const uint32_t kTexts = te::RetailAddress(0x00309B10);   // g_Texts[CodeTexts]
            const uint32_t lines = te::GuestPointer(kTexts, 4) ? te::rd32(te::GuestPointer(kTexts, 4)) : 0;
            const char* text9 = nullptr;
            if (lines && te::GuestPointer(lines + 9 * 4, 4)) text9 = te::GuestString(te::rd32(te::GuestPointer(lines + 9 * 4, 4)), 128);
            std::fprintf(stderr, "[twinsanityevolved] %s; game text 9 = '%s'\n", te::language::Describe().c_str(), text9 ? text9 : "(no texts loaded)");
            if (te::harness::Armed()) te::harness::Set("language", te::language::Describe() + (text9 ? std::string("; text9=") + text9 : ""));
        }
        // the mods' census row (te_mods.h CensusRow): the disc overlay's layers, the disc files they replace or add, the reads
        // it served -- mods=0:0:0 when no folder mod with a disc/ tree is on
        {
            const std::string row = te::mods::CensusRow();
            std::fprintf(stderr, "[twinsanityevolved] %s\n", row.c_str());
            if (te::harness::Armed()) te::harness::Set("mods", row);
        }
#if !defined(TE_NATIVE)
        const te::AdaptReport ad = te::AdaptedCalls();
        std::fprintf(stderr, "[twinsanityevolved] call_indirect adapted: %llu calls at %u distinct sites (adapted at the call site; 0 traps)\n",
                     (unsigned long long)ad.calls, ad.sites);
#endif
        // The game's heap (game/src/game/memory.cpp HeapManager g_HeapManager, PAL D_0031B338): the big-block part and the
        // small allocations' pages. A frame leak shows here as pages used == pages (the game then gets NULL from its
        // allocator and corrupts low memory): the frozen judge tools/judges/padlog_judge.py reads this line.
        {
            const uint8_t* mem = te::GuestPointer(te::RetailAddress(0x0031B338), 0x68);   // g_HeapManager
            const uint32_t kHeapManager = 0;
            const uint32_t ready = mem ? te::rd32(mem + kHeapManager + 0x30) : 0;
            if (ready == 1) {
                const uint32_t pool_size = te::rd32(mem + kHeapManager + 0x20), free_bytes = te::rd32(mem + kHeapManager + 0x14);
                const uint32_t used_bytes = te::rd32(mem + kHeapManager + 0x18), peak = te::rd32(mem + kHeapManager + 0x2C);
                const uint32_t pages = te::rd32(mem + kHeapManager + 0x40), pages_free = te::rd32(mem + kHeapManager + 0x64);
                const uint32_t pool_start = te::rd32(mem + kHeapManager + 0x1C) - te::RetailAddress(0);   // HeapManager::poolStart (fixed per build; its retail address)
                std::fprintf(stderr, "[twinsanityevolved] heap: pool %u bytes at 0x%08X, used %u, peak %u, free %u; small pages used %u of %u (free %u)\n",
                             pool_size, pool_start, used_bytes, peak, free_bytes, pages - pages_free, pages, pages_free);
                te::harness::Set("heap", "pool=" + std::to_string(pool_size) + ",used=" + std::to_string(used_bytes) + ",peak=" + std::to_string(peak) +
                                             ",free=" + std::to_string(free_bytes) + ",pages_used=" + std::to_string(pages - pages_free) + ",pages=" + std::to_string(pages));
            } else {
                std::fprintf(stderr, "[twinsanityevolved] heap: not started\n");
            }
        }
    }
    te::harness::Finalize();   // the leg's tails + run.meta (after the census keys)
#if !defined(TE_NATIVE)
    wasm2c_game_free(&instance);
#endif
#if defined(__vita__)
    te::vita::CloseLog();   // the census on the card before the process' teardown (te_vita.h)
#endif
    return g_exit_status;
}
