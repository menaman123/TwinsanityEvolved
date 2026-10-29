// platform/nativeee (host side) -- system.h: Platform::System. The boot: the services constructed in ServiceStartup
// order (te_services.h), the console's language, the local time off the emulated RTC, the kernel TTY and
// Exit (the runner leaves the module). The PS2 arm's Initialise reset the graphics devices, restarted the IOP with the
// disc's IOPRP image and started the disc twice; natively the same sequence constructs the clock, the disc and the
// files, and StartServices -- the PS2 arm's eight driver loads, MemoryCard/Pads initialise, IOP heap, sound remote --
// constructs the pads, the saves, the IOP heap and asks the sound seam to come up.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "engine/cdvd/iso_fs.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_storage.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/files.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "te_input.h"
#include "te_language.h"
#include "te_services.h"

namespace te {

namespace {
Settings g_settings;
Services* g_services = nullptr;
}  // namespace

const Settings& GetSettings() { return g_settings; }
void SetSettings(const Settings& settings) { g_settings = settings; }

Services& Services::Get() {
    if (!g_services) g_services = new Services;
    return *g_services;
}

void Services::ResetForTest() {
    delete g_services;
    g_services = nullptr;
}

Clock& Services::clock() {
    if (!clock_) clock_.reset(new Clock(g_settings));
    return *clock_;
}
Disc& Services::disc() {
    clock();
    if (!disc_) disc_.reset(new Disc(g_settings));
    return *disc_;
}
Files& Services::files() {
    disc();
    if (!files_) files_.reset(new Files(*disc_));
    return *files_;
}
Pads& Services::pads() {
    files();
    if (!pads_) pads_.reset(new Pads(*clock_, g_settings));
    return *pads_;
}
Saves& Services::saves() {
    pads();
    if (!saves_) saves_.reset(new Saves(*clock_, g_settings));
    return *saves_;
}
IoHeap& Services::io() {
    saves();
    if (!io_) {
        io_.reset(new IoHeap);
        savestate::Register(savestate::kTagIoHeap, "te::IoHeap", false,
            [](std::vector<uint8_t>& out) { Services::Get().io().Serialize(out); },
            [](const savestate::Blob& blob, std::string* err) { return Services::Get().io().Restore(blob, err); });
    }
    return *io_;
}

// Which services exist (ServiceStartup progress): a loaded process constructs the same set before the sections restore.
void Services::RegisterStateSection() {
    savestate::Register(savestate::kTagSystem, "te::Services", true,
        [](std::vector<uint8_t>& out) {
            Services& s = Services::Get();
            savestate::Put(out, uint32_t(1));
            savestate::Put(out, uint8_t(s.initialised_ ? 1 : 0));
            savestate::Put(out, uint8_t(s.started_ ? 1 : 0));
        },
        [](const savestate::Blob& blob, std::string* err) {
            savestate::PayloadReader in(blob.payload);
            uint32_t version = 0;
            uint8_t initialised = 0, started = 0;
            if (!in.Get(&version) || version != 1 || !in.Get(&initialised) || !in.Get(&started)) {
                if (err) *err = "te::Services: payload";
                return false;
            }
            Services& s = Services::Get();
            if (initialised) s.Initialise();
            if (started) s.StartServices();
            return true;
        });
}

void Services::PrepareForRestore(const std::vector<savestate::Blob>& sections) {
    // The services the file carries must exist (and so be registered) before the registry restores: construct them
    // in ServiceStartup order from the system section's flags (a service's own section then finds its owner).
    RegisterStateSection();
    for (const savestate::Blob& b : sections) {
        if (b.tag != savestate::kTagSystem) continue;
        savestate::PayloadReader in(b.payload);
        uint32_t version = 0;
        uint8_t initialised = 0, started = 0;
        if (in.Get(&version) && in.Get(&initialised) && in.Get(&started)) {
            if (initialised) Initialise();
            if (started) StartServices();
        }
    }
    io();   // the IOP heap and everything before it exist in any case
}

void Services::Initialise() {
    if (initialised_) return;
    RegisterStateSection();
    // 1 the clock, 2 the disc (sceCdInit / Mmode(Dvd) / DiskReady), 3 the files (fioExit: forget the descriptors)
    clock();
    Disc& d = disc();
    d.Initialise();
    d.SetMedia(2);
    files().Reset();
    const int32_t ready = d.WaitReady();
    if (g_settings.verbose || ready != 2)
        std::fprintf(stderr, "[twinsanityevolved] system: disc %s (%s)\n", ready == 2 ? "ready" : "NOT READY",
                     d.IsOpen() ? d.Iso().VolumeId().c_str() : d.Error().c_str());
    initialised_ = true;
}

void Services::StartServices() {
    if (started_) return;
    Initialise();
    // The PS2 arm loaded SIO2MAN PADMAN MCMAN MCSERV LIBSD SDRDRV STREAM CDVDSTM here: each has its native owner
    static const char* const kDrivers[] = {"SIO2MAN.IRX", "PADMAN.IRX", "MCMAN.IRX", "MCSERV.IRX",
                                           "LIBSD.IRX",   "SDRDRV.IRX", "STREAM.IRX", "CDVDSTM.IRX"};
    for (const char* driver : kDrivers) io().LoadDriver(driver);
    // 4 the pads (padInit), 5 the saves' storage (mcInit), 6 the IOP heap (SifInitIopHeap), 7 the sound (sound.h)
    pads().Initialise();
    input::Install(pads(), g_settings);   // the SDL sampler (gamepad + kbm) on pad 0, or the neutral pad
    saves();
    io().Reset();
    ps2e_sound_initialise_remote();
    started_ = true;
    if (g_settings.verbose) {
        const Clock::Census c = clock().GetCensus();
        std::fprintf(stderr, "[twinsanityevolved] system: services started (clock %s %u Hz, disc %s, mc_dir '%s', language %d)\n",
                     c.play ? "play" : "deterministic", c.field_hz, disc().IsOpen() ? "open" : "absent",
                     g_settings.mc_dir.c_str(), language::ConsoleLanguage());
    }
}

}  // namespace te

// A host string into the guest buffer `out` (`size` bytes, NUL-terminated)
namespace {
void CopyText(const std::string& text, ps2e_u32 out, ps2e_s32 size) {
    if (size <= 0) return;
    uint8_t* p = te::GuestPointer(out, static_cast<size_t>(size));
    if (!p) return;
    const size_t n = std::min(text.size(), static_cast<size_t>(size) - 1);
    std::memcpy(p, text.data(), n);
    p[n] = 0;
}
}  // namespace

extern "C" {

void ps2e_system_initialise(void) { te::Services::Get().Initialise(); }

void ps2e_system_start_services(void) { te::Services::Get().StartServices(); }

void ps2e_system_kputs(ps2e_u32 text) {
    const char* s = te::GuestString(text, 1024);
    if (s) std::fputs(s, stderr);
}

void ps2e_system_exit(ps2e_s32 status) {
    std::fprintf(stderr, "[twinsanityevolved] Platform::System::Exit(%d)\n", status);
    te::RequestExit(status);
}

// Platform::System::ConsoleLanguage: the OSD setting (libscf's sceScfGetLanguage); natively the language SETTING
// (te_language.h): the console code of the selected language's VOICE slot -- the RETAIL INDEX the game runs on (its
// GameLanguageOf maps the code back to the slot): the speech bank, the FMV sound track, a level's voice table, the title-card
// pictures and the credits follow it exactly as retail did. The two TEXT files follow LanguageTextBank() instead.
ps2e_s32 ps2e_system_language(void) { return te::language::ConsoleLanguage(); }

// Platform::System::LanguageTextBank: the slot whose Code/AgentLab text files are in the TEXT language in force -- the same
// slot Language() names on a retail disc; another one on a disc whose text banks were moved (the development image's Portuguese patch:
// English text in the `Spanish` slot). -1 = none selected: the game reads the slot Language() names.
ps2e_s32 ps2e_system_language_text_bank(void) { return te::language::TextBank(); }

// Platform::System::TextLanguage* / SpeechLanguage* / LanguageRowTitle: the Options menu's two language rows -- the
// TEXT languages (one per text content the disc carries, then the language packs) and the SPEECH languages (one per speech
// bank), the one in force (-1 = NoLanguage: a raw slot whose language the row has not), a choice's word, the player's choice
// persisted (ps2r.ini game.text_language / game.speech_language) and answered by Language() / LanguageTextBank() from then
// on, and the rows' titles in the text language in force.
ps2e_s32 ps2e_system_text_language_count(void) { return static_cast<ps2e_s32>(te::language::TextCount()); }

ps2e_s32 ps2e_system_text_language_selected(void) { return te::language::TextSelected(); }

void ps2e_system_text_language_label(ps2e_s32 language, ps2e_u32 out, ps2e_s32 size) {
    std::string label = language >= 0 ? te::language::TextLabel(static_cast<uint32_t>(language)) : "";
    if (label.empty()) label = "language " + std::to_string(language);
    CopyText(label, out, size);
}

void ps2e_system_set_text_language(ps2e_s32 language) {
    if (language >= 0) te::language::SelectText(static_cast<uint32_t>(language));
}

ps2e_s32 ps2e_system_speech_language_count(void) { return static_cast<ps2e_s32>(te::language::SpeechCount()); }

ps2e_s32 ps2e_system_speech_language_selected(void) { return te::language::SpeechSelected(); }

void ps2e_system_speech_language_label(ps2e_s32 language, ps2e_u32 out, ps2e_s32 size) {
    std::string label = language >= 0 ? te::language::SpeechLabel(static_cast<uint32_t>(language)) : "";
    if (label.empty()) label = "language " + std::to_string(language);
    CopyText(label, out, size);
}

void ps2e_system_set_speech_language(ps2e_s32 language) {
    if (language >= 0) te::language::SelectSpeech(static_cast<uint32_t>(language));
}

void ps2e_system_language_row_title(ps2e_s32 row, ps2e_u32 out, ps2e_s32 size) {
    CopyText(row >= 0 ? te::language::RowTitle(static_cast<uint32_t>(row)) : "", out, size);
}

// Platform::System::LocalTime -> DateTime{u16 year; u8 month, day, hour, minute, second} (8 bytes, padded) at `out`
void ps2e_system_local_time(ps2e_u32 out) {
    const te::Clock::Civil t = te::Services::Get().clock().LocalTime();
    uint8_t* p = te::GuestPointer(out, 8);
    if (!p) return;
    const uint16_t year = static_cast<uint16_t>(t.year);
    std::memcpy(p, &year, 2);
    p[2] = static_cast<uint8_t>(t.month);
    p[3] = static_cast<uint8_t>(t.day);
    p[4] = static_cast<uint8_t>(t.hour);
    p[5] = static_cast<uint8_t>(t.minute);
    p[6] = static_cast<uint8_t>(t.second);
    p[7] = 0;
}

}  // extern "C"
