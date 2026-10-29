// te_host.h -- what every host-side seam module (platform/nativeee/host/<header>.cpp) may use: guest memory access by
// u32 address (wasm address == guest address == MainMemory offset), the run-control hooks the runner provides, and the
// run settings the runner parsed (the disc image path is the manifest: every file the game reads is read out of it at
// RUN time).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace te {

// The 32 MB linear memory the module and the engine share. Null until the runner bound it.
#if defined(TE_NATIVE)
// THE VITA'S NATIVE BUILD (CMake TE_VITA_NATIVE, runner/te_native.h): the game's code is native ARM, so every pointer it
// hands the seam -- an argument, or one it stored in a structure the host reads -- is a native address: a "guest address"
// IS the pointer (32-bit on the Vita), and GuestPointer is the identity on a valid range (below). GuestMemory() is the guest block:
// the game's 32 MB RETAIL address space (retail address X at GuestMemory() + X: the retail image, the game's heap), which
// the diagnostics that hash or dump the 32 MB read; host code that names a retail address goes through RetailAddress.
extern uint8_t* g_guest_memory_base;
extern std::size_t g_guest_memory_size;
// Where a game pointer can point (runner/te_env.cpp BindGuestBlock, runner/te_game.cpp on the game thread): the eboot's
// data segment (data + .bss: the guest block -- the game's heap and the retail image -- inside it), the game thread's stack
// and the eboot's code segment (code + read-only data: the game's string literals), each as its base and span. A host
// read checks against them as the wasm lanes check against the 32 MB: the host's hooks walk pointers the game may have
// left stale (the player agent's fields during a level load) and must get null for them, not a fault.
// ONE struct (one address to make, a pair of loads per range) and 32-bit arithmetic: the seams make thousands of these
// a picture -- the first form (six separate globals, 64-bit ends) cost ~38 instructions a pointer against the wasm
// lane's ~13 (ps2e_vu0_init_identity_matrix: 55 against 28)
struct GuestRanges {
    uint32_t data_base = 0, data_span = 0;
    uint32_t stack_base = 0, stack_span = 0;
    uint32_t text_base = 0, text_span = 0;
};
extern GuestRanges g_guest_ranges;
inline uint8_t* GuestMemory() { return g_guest_memory_base; }
inline std::size_t GuestMemorySize() { return g_guest_memory_size; }
inline uint32_t RetailAddress(uint32_t retail) { return uint32_t(reinterpret_cast<uintptr_t>(g_guest_memory_base)) + retail; }
// [address, address + size) inside [base, base + span): `address - base` wraps past the span for an address below base
inline bool GuestInRange(uint32_t address, std::size_t size, uint32_t base, uint32_t span) {
    const uint32_t offset = address - base;
    return offset < span && size <= std::size_t(span - offset);
}
inline uint8_t* GuestPointer(uint32_t address, std::size_t size) {
    const GuestRanges& r = g_guest_ranges;
    if (GuestInRange(address, size, r.data_base, r.data_span) || GuestInRange(address, size, r.stack_base, r.stack_span) ||
        GuestInRange(address, size, r.text_base, r.text_span))
        return reinterpret_cast<uint8_t*>(uintptr_t(address));
    return nullptr;
}
#else
#if defined(__vita__)
// The Vita: the base and the size as plain globals the runner sets when it binds the memory (runner/te_env.cpp
// BindMainMemory; the memory never moves) -- every GuestPointer was two calls into the runner, and the seams make
// thousands of them a frame
extern uint8_t* g_guest_memory_base;
extern std::size_t g_guest_memory_size;
inline uint8_t* GuestMemory() { return g_guest_memory_base; }
inline std::size_t GuestMemorySize() { return g_guest_memory_size; }
#else
uint8_t* GuestMemory();
std::size_t GuestMemorySize();
#endif

// A host pointer to `size` guest bytes at `address`, or nullptr when the range is outside the 32 MB.
inline uint8_t* GuestPointer(uint32_t address, std::size_t size) {
    uint8_t* mem = GuestMemory();
    if (!mem || uint64_t(address) + size > GuestMemorySize()) return nullptr;
    return mem + address;
}
// The guest address of a RETAIL address (an address of the PAL executable's data image or of the game's heap): itself on
// the wasm lanes (wasm address == retail address), the guest block's base added on the native one (above)
inline uint32_t RetailAddress(uint32_t retail) { return retail; }
#endif
// A NUL-terminated guest string of at most `max` bytes, or nullptr. Not copied: valid until the game writes there.
const char* GuestString(uint32_t address, std::size_t max);

// Platform::System::Exit arrives here: the runner leaves the module (longjmp out of wasm2c) and ends with `status`.
[[noreturn]] void RequestExit(int status);

}  // namespace te

// A `void()` callback the game handed a host body as a function pointer (a wasm TABLE INDEX): called back into the
// module. Returns 1 when it was called, 0 when the index names no `() -> nil` function (logged) or there is no module.
// The runner defines it (runner/te_env.cpp); the tests' harness defines a counting one; a host executable without
// either (the graphics self-test) gets the weak default in movie_player.cpp, which returns 0.
extern "C" int te_call_void_callback(uint32_t index);

namespace te {

// What the runner (or a test) decided before the module is entered. Read by the services at construction
// (te::Services, host/te_services.h); never read from the environment or a file by the services themselves.
struct Settings {
    std::string iso_path;        // the disc image: the manifest. Empty = no disc (tests; Disc::Initialise fails)
    std::string mc_dir;          // the memory cards' host directory (<mc_dir>/mc0, mc1). Empty = saves see no card
    // te_language.h: the game's two languages. `text_language_name` / `speech_language_name` are the names asked for each
    // (ps2r.ini game.text_language / game.speech_language, --text-language / --speech-language; "" = not given), the text
    // resolved against the disc's text banks by CONTENT (and the language packs), the speech against its speech banks;
    // `language_name` is the LEGACY one name for both (ps2r.ini game.language / --language <name>), read for a row whose own
    // name is not given; `language_bank` (>= 0) names a slot of the game's language table directly (--language-bank N: a
    // diagnostic, the judge's raw arm); `language` (>= 0) is a raw Platform::System::ConsoleLanguage code (--language 0..7, the
    // raw-code form; what Language() answers when the disc's banks cannot be enumerated -- the tests' fake disc). -1 / "" =
    // not given.
    std::string text_language_name;
    std::string speech_language_name;
    std::string language_name;
    int32_t language_bank = -1;
    int32_t language = -1;
    std::string settings_path;   // the settings file (ps2r.ini) the Options menu persists to; "" = nothing is written
    bool deterministic = false;  // TickSource Deterministic: no pacer thread, one field per boundary, fixed RTC
    uint32_t field_hz = 60;      // the pacer's field rate (game_hz 60 by default; 50 reproduces a PAL console)
    bool kbm = true;             // keyboard+mouse feed pad 0 (engine InputSystem) when SDL input is available
    bool sdl_input = true;       // false: no SDL at all, pad 0 is the neutral connected pad (headless runs, tests)
    bool pad_connected = true;   // the neutral pad reports connected (the game then completes its DualShock 2 setup)
    bool skip_movies = false;    // FEATURES "skip movies": every movie ends at its first Step (the stub's behaviour)
    bool stream_instant = false; // the stream façade's INSTANT model (reads complete inside the call) instead of the
                                 // engine's drive-time model (ReadTiming: seek + transfer, poll/boundary/wait visibility)
    bool present_ct16 = false;   // the retail present -- the frame shown as the GS's dithered 16-bit copy (CT24 -> CT16)
    bool verbose = false;
};
const Settings& GetSettings();
void SetSettings(const Settings& settings);

}  // namespace te
