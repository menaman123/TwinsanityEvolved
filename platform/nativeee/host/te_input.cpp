// platform/nativeee (host side) -- the pads' host sampler: the engine's SDL InputSystem (a game controller when one is
// connected, keyboard+mouse ALWAYS live as a pad, hot-plug both ways, the engine's default_bindings.ini) feeding pad 0,
// polled once per frame boundary on the game thread (SDL's "main thread" is the runner's, which runs the module).
//
// Event pump ownership: with no window (headless runs) this module pumps SDL's events itself at the boundary and
// forwards them to the InputSystem; the window owner (graphics) may take the pump -- te::input::SetPumpOwned(false)
// and te::input::HandleSdlEvent(e) for every event it polls -- so keyboard events reach the pad through one queue.
// Without SDL input (te::Settings::sdl_input false, or SDL refusing to start) the neutral connected pad stays.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "te_host.h"
#include "te_input.h"
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>   // sceKernelPowerTick
#include "../../../runner/te_vita.h"
#endif
#include "te_services.h"
#include "te_prof.h"

#if TE_HAVE_SDL_INPUT
#include <SDL.h>

#include "engine/platform/sdl/input/gamepad_backend.h"
#include "engine/platform/sdl/input/gamepad_convert.h"
#include "engine/platform/sdl/input/input_system.h"
#include "engine/platform/sdl/input/keyboard_mouse_backend.h"
#if defined(__vita__)
#include <psp2/ctrl.h>
#endif
#endif

namespace te::input {

namespace {
#if TE_HAVE_SDL_INPUT
ps2r::platform::sdl::InputSystem* g_input = nullptr;
bool g_events_inited = false;
#endif
#if TE_HAVE_SDL_INPUT && defined(__vita__)
// ---- THE VITA CONTROLLER READ STRAIGHT FROM sceCtrl ------------------------------------------------------------
// SDL's path per boundary was SDL_JoystickUpdate (its Vita driver's sceCtrlPeekBufferPositive2 -- port 1, then port 0 on a
// handheld, where port 1 fails --, a joystick event for every changed axis and button through SDL's event queue, the game
// controller layer's own events on top) and then 27 SDL_GameControllerGetAxis / GetButton calls, each taking the joysticks'
// lock and scanning the mapping: "pads: SDL joystick update (the controller)" 0.8 % + "pads: SDL event pump" 0.4 % + "pads: the
// controller read (InputSystem)" 0.2 % of the device's game thread (measured in the hub: ~0.45 ms a tick). Here: ONE read of the same
// ports, and the values SDL would hold -- its driver's analog map (calc_bezier_y, computed the same way on the same thread:
// the game thread's rounding), its "changed since the last read" tests, the joystick layer's axis state (an axis reports its
// first reading until it moves by more than 1/80 of its range: SDL_PrivateJoystickAxis's initial-value rules), the button
// states, and SDL's PSVita Controller mapping (SDL_gamecontrollerdb.h: a:b2 b:b1 x:b3 y:b0 back:b10 start:b11 leftshoulder:b4
// rightshoulder:b5 dpup:b8 dpdown:b6 dpleft:b7 dpright:b9 leftstick:b14 rightstick:b15 leftx..righty:a0..a3,
// lefttrigger:a4 righttrigger:a5 -- the triggers' full-range axis normalised to 0..32767 as SDL_GameControllerGetAxis does)
// -- handed to the engine's GamepadBackend as its raw snapshot (GamepadBackend::SetRawSource), which maps it as before
// (deadzone, sensitivity, [pad] remap, the InputMapper). SDL's own joystick updates stop (SDL_HINT_AUTO_UPDATE_JOYSTICKS 0:
// its pump every 4th boundary no longer reads the pad). TE_VITA_DIRECT_PAD=0: SDL's path as before; TE_VITA_PAD_CHECK=N:
// every Nth boundary SDL's own read too, its game controller's 27 values compared with the replica's (a census line).
struct VitaPad {
    // the driver's (SDL_sysjoystick.c, vita): the analog map, the previous reading's axes and buttons
    int analog_map[256] = {};
    unsigned char old_axis[6] = {};   // lx, ly, rx, ry, lt, rt (the driver's static old_* of joystick 0)
    unsigned old_buttons = 0;
    // the joystick's state (SDL_joystick.c SDL_JoystickAxisInfo / the buttons array)
    struct Axis {
        int16_t value = 0, initial = 0, zero = 0;
        bool has_initial = false, has_second = false, sent_initial = false, sending_initial = false;
    } axes[6];
    uint8_t buttons[16] = {};
    bool on = false;
    uint32_t check_every = 0;
    uint64_t reads = 0, checked = 0, differ = 0;
    int said = 0;
};
VitaPad g_vpad;

// SDL's calc_bezier_y with its four points (the "linear curve" of the Vita driver), the same expressions
struct BezierPoint { int x, y; };
void BezierLerp(BezierPoint* dest, const BezierPoint* first, const BezierPoint* second, float t) {
    dest->x = first->x + (second->x - first->x) * t;
    dest->y = first->y + (second->y - first->y) * t;
}
int BezierY(float t) {
    static const BezierPoint a = {0, 0}, b = {0, 0}, c = {128, 32767}, d = {128, 32767};
    BezierPoint ab, bc, cd, abbc, bccd, dest;
    BezierLerp(&ab, &a, &b, t);
    BezierLerp(&bc, &b, &c, t);
    BezierLerp(&cd, &c, &d, t);
    BezierLerp(&abbc, &ab, &bc, t);
    BezierLerp(&bccd, &bc, &cd, t);
    BezierLerp(&dest, &abbc, &bccd, t);
    return dest.y;
}

// SDL_PrivateJoystickAxis (SDL 2.32.8) on the replica's state: the value the joystick holds after it
void JoystickAxis(VitaPad::Axis& info, int16_t value) {
    if (!info.has_initial || (!info.has_second && (info.initial <= -32767 || info.initial == 32767) && std::abs(int(value)) < (32767 / 4))) {
        info.initial = value;
        info.value = value;
        info.zero = value;
        info.has_initial = true;
    } else if (value == info.value && !info.sending_initial) {
        return;
    } else {
        info.has_second = true;
    }
    if (!info.sent_initial) {
        const int kMaxAllowedJitter = 32767 / 80;   // SDL_JOYSTICK_AXIS_MAX / 80
        if (std::abs(int(value) - int(info.value)) <= kMaxAllowedJitter) return;
        info.sent_initial = true;
        info.sending_initial = true;
        JoystickAxis(info, info.initial);
        info.sending_initial = false;
    }
    // (SDL_PrivateJoystickShouldIgnoreEvent: never -- the engine's GamepadBackend sets SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS)
    info.value = value;
}

// The Vita driver's VITA_JoystickUpdate for joystick 0 on the replica's state
void ReadVitaPad(VitaPad& p) {
    static const unsigned kButtonMap[16] = {SCE_CTRL_TRIANGLE, SCE_CTRL_CIRCLE, SCE_CTRL_CROSS, SCE_CTRL_SQUARE, SCE_CTRL_L1, SCE_CTRL_R1,
                                            SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_UP, SCE_CTRL_RIGHT, SCE_CTRL_SELECT, SCE_CTRL_START,
                                            SCE_CTRL_L2, SCE_CTRL_R2, SCE_CTRL_L3, SCE_CTRL_R3};
    SceCtrlData pad{};
    if (sceCtrlPeekBufferPositive2(1, &pad, 1) < 0) sceCtrlPeekBufferPositive2(0, &pad, 1);   // port 1, a handheld's 0
    const unsigned char v[6] = {pad.lx, pad.ly, pad.rx, pad.ry, pad.lt, pad.rt};
    for (int a = 0; a < 6; ++a) {
        if (p.old_axis[a] != v[a]) {
            JoystickAxis(p.axes[a], int16_t(p.analog_map[v[a]]));
            p.old_axis[a] = v[a];
        }
    }
    const unsigned changed = p.old_buttons ^ pad.buttons;
    p.old_buttons = pad.buttons;
    if (changed)
        for (int i = 0; i < 16; ++i)
            if (changed & kButtonMap[i]) p.buttons[i] = (pad.buttons & kButtonMap[i]) ? 1 : 0;
    ++p.reads;
}

// SDL's PSVita Controller mapping applied to the joystick's state: what SDL_GameControllerGetAxis / GetButton answer
void MapVitaPad(const VitaPad& p, ps2r::platform::sdl::GamepadRaw& raw) {
    raw = ps2r::platform::sdl::GamepadRaw{};
    // the sticks: a full-range axis to a full-range output (no normalisation); 0 when zero (no other binding)
    raw.axis[SDL_CONTROLLER_AXIS_LEFTX] = p.axes[0].value;
    raw.axis[SDL_CONTROLLER_AXIS_LEFTY] = p.axes[1].value;
    raw.axis[SDL_CONTROLLER_AXIS_RIGHTX] = p.axes[2].value;
    raw.axis[SDL_CONTROLLER_AXIS_RIGHTY] = p.axes[3].value;
    // the triggers: the input range -32768..32767 to the output 0..32767, as SDL_GameControllerGetAxis computes it
    for (int k = 0; k < 2; ++k) {
        const int value = p.axes[4 + k].value;
        const float normalized = (float)(value - (-32768)) / (32767 - (-32768));
        const int out = 0 + (int)(normalized * (32767 - 0));
        raw.axis[SDL_CONTROLLER_AXIS_TRIGGERLEFT + k] = (out != 0 && out >= 0 && out <= 32767) ? int16_t(out) : int16_t(0);
    }
    raw.button[SDL_CONTROLLER_BUTTON_A] = p.buttons[2];
    raw.button[SDL_CONTROLLER_BUTTON_B] = p.buttons[1];
    raw.button[SDL_CONTROLLER_BUTTON_X] = p.buttons[3];
    raw.button[SDL_CONTROLLER_BUTTON_Y] = p.buttons[0];
    raw.button[SDL_CONTROLLER_BUTTON_BACK] = p.buttons[10];
    raw.button[SDL_CONTROLLER_BUTTON_START] = p.buttons[11];
    raw.button[SDL_CONTROLLER_BUTTON_LEFTSTICK] = p.buttons[14];
    raw.button[SDL_CONTROLLER_BUTTON_RIGHTSTICK] = p.buttons[15];
    raw.button[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = p.buttons[4];
    raw.button[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = p.buttons[5];
    raw.button[SDL_CONTROLLER_BUTTON_DPAD_UP] = p.buttons[8];
    raw.button[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = p.buttons[6];
    raw.button[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = p.buttons[7];
    raw.button[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = p.buttons[9];
}

// GamepadBackend's raw source: the snapshot of the boundary's read (the sampler reads before the InputSystem's Frame)
bool VitaRawSource(void*, ps2r::platform::sdl::GamepadRaw& raw) {
    MapVitaPad(g_vpad, raw);
    return true;
}

// The replica's start, as SDL's own went: the driver's analog map (VITA_JoystickInit -- here right after it ran inside the
// InputSystem's Init, on the same thread: the same rounding mode, which moves two of its entries), the joystick's first read
// (SDL_JoystickOpen ends with the driver's Update), then the game controller's mapping load setting the triggers' axes to
// their zero point (SDL_PrivateLoadButtonMapping: value = zero = the input range's minimum, -32768)
void StartVitaPad(VitaPad& p) {
    p = VitaPad{};
    for (int i = 0; i < 128; i++) {
        const float t = (float)i / 127.0f;
        p.analog_map[i + 128] = BezierY(t);
        p.analog_map[127 - i] = -1 * p.analog_map[i + 128];
    }
    ReadVitaPad(p);
    p.reads = 0;
    for (int a = 4; a < 6; ++a) p.axes[a].value = p.axes[a].zero = -32768;
}

// TE_VITA_PAD_CHECK: SDL's own read of this boundary, its game controller's answers against the replica's
void CheckVitaPad(VitaPad& p) {
    auto* gp = g_input ? g_input->Gamepad() : nullptr;
    if (!gp || gp->OpenPads() != 1) return;
    SDL_GameController* gc = static_cast<SDL_GameController*>(gp->PadController(0));
    if (!gc) return;
    SDL_JoystickUpdate();
    ps2r::platform::sdl::GamepadRaw sdl{}, mine;
    for (int a = 0; a < ps2r::platform::sdl::GamepadRaw::kAxes; ++a) sdl.axis[a] = SDL_GameControllerGetAxis(gc, SDL_GameControllerAxis(a));
    for (int b = 0; b < ps2r::platform::sdl::GamepadRaw::kButtons; ++b) sdl.button[b] = SDL_GameControllerGetButton(gc, SDL_GameControllerButton(b));
    // (its events would reach the next boundary's sample as the pending presses of SDL's path: dropped)
    SDL_FlushEvents(SDL_JOYAXISMOTION, SDL_JOYBUTTONUP);
    SDL_FlushEvents(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERBUTTONUP);
    MapVitaPad(p, mine);
    ++p.checked;
    bool same = true;
    for (int a = 0; a < ps2r::platform::sdl::GamepadRaw::kAxes; ++a) same &= sdl.axis[a] == mine.axis[a];
    for (int b = 0; b < ps2r::platform::sdl::GamepadRaw::kButtons; ++b) same &= (sdl.button[b] != 0) == (mine.button[b] != 0);
    if (!same) {
        ++p.differ;
        if (p.said++ < 8)
            std::fprintf(stderr, "[twinsanityevolved] input: pad check: SDL axes %d %d %d %d %d %d vs the direct read %d %d %d %d %d %d (buttons A %d/%d)\n",
                         sdl.axis[0], sdl.axis[1], sdl.axis[2], sdl.axis[3], sdl.axis[4], sdl.axis[5], mine.axis[0], mine.axis[1],
                         mine.axis[2], mine.axis[3], mine.axis[4], mine.axis[5], sdl.button[0], mine.button[0]);
    }
}
#endif
bool g_pump_owned = true;
bool g_installed = false;
struct Injection {
    int scancode;
    uint64_t from, to;
    bool delivered = false, spent = false;
    uint64_t last = 0;
};
std::vector<Injection> g_injections;

// Whether an injected key is held at `boundary`. A window plays ONCE: when the timeline goes back after it was delivered (a
// state loaded in place -- F8, a practice state) it is spent, so an injected F8 does not press itself again each time the
// restored game reaches its boundary.
bool Live(Injection& k, uint64_t boundary) {
    if (k.spent) return false;
    if (k.delivered && boundary < k.last) {
        k.spent = true;
        return false;
    }
    if (boundary < k.from || boundary > k.to) return false;
    k.delivered = true;
    k.last = boundary;
    return true;
}
}  // namespace

bool Install(Pads& pads, const Settings& settings) {
    if (g_installed) return true;
    if (!settings.sdl_input) return false;
#if TE_HAVE_SDL_INPUT
#if defined(__vita__)
    te::vita::Step("input: SDL events subsystem");
#endif
    if (!SDL_WasInit(SDL_INIT_EVENTS)) {
        if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0) {
            std::fprintf(stderr, "[twinsanityevolved] input: SDL events unavailable (%s); neutral pad\n", SDL_GetError());
            return false;
        }
        g_events_inited = true;
    }
    g_input = new ps2r::platform::sdl::InputSystem;
    ps2r::platform::sdl::InputSettings in;
    in.kbm_enabled = settings.kbm;
    in.gamepad_enabled = true;
#if defined(__vita__)
    te::vita::Step("input: the engine's InputSystem (gamepad subsystem, the Vita controller)");
#endif
    g_input->Init(in);
#if defined(__vita__)
    te::vita::Step("input: ready");
    {
        // the controller straight from sceCtrl (VitaPad above) when the one pad SDL opened is the Vita's controller
        const char* v = std::getenv("TE_VITA_DIRECT_PAD");
        ps2r::platform::sdl::GamepadBackend* gp = g_input->Gamepad();
        SDL_GameController* gc = gp && gp->OpenPads() == 1 ? static_cast<SDL_GameController*>(gp->PadController(0)) : nullptr;
        const char* name = gc ? SDL_GameControllerName(gc) : nullptr;
        if (!(v && v[0] == '0') && name && std::strcmp(name, "PSVita Controller") == 0) {
            StartVitaPad(g_vpad);
            gp->SetRawSource(&VitaRawSource, nullptr);
            SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");   // SDL's pump no longer reads the pad
            g_vpad.on = true;
            if (const char* c = std::getenv("TE_VITA_PAD_CHECK")) g_vpad.check_every = uint32_t(std::max(1L, std::atol(c)));
            std::fprintf(stderr, "[twinsanityevolved] input: the Vita controller read straight from sceCtrl every boundary (SDL's "
                                 "joystick values and its PSVita Controller mapping replicated; SDL's joystick update off; "
                                 "TE_VITA_DIRECT_PAD=0: SDL's path)%s\n",
                         g_vpad.check_every ? (" -- TE_VITA_PAD_CHECK: SDL's read compared every " + std::to_string(g_vpad.check_every) +
                                               " boundaries").c_str() : "");
        } else {
            std::fprintf(stderr, "[twinsanityevolved] input: the controller through SDL's joystick update (%s)\n",
                         v && v[0] == '0' ? "TE_VITA_DIRECT_PAD=0" : name ? name : "no single pad open");
        }
    }
#endif
    const uint32_t hz = settings.field_hz ? settings.field_hz : 60u;
    const float dt = 1.0f / static_cast<float>(hz);
    pads.SetSampler([dt](unsigned port, unsigned slot) -> Pads::Sample {
        Pads::Sample s;
        if (port != 0 || slot != 0) return s;   // one pad: the engine's InputSystem is pad 0
        // (the device's "hook pads" was 0.4-0.6 ms a tick: its two halves as their own sections, TE_PROF=1; the game
        // thread's slot, no thread_local read)
        static const unsigned short profPump = te::prof::Register("pads: SDL event pump");
        static const unsigned short profRead = te::prof::Register("pads: the controller read (InputSystem)");
        if (g_pump_owned) {
#if defined(__vita__)
            // The Vita: SDL's whole pump (its video system's keyboard / mouse / touch / IME polls -- none of them the game's:
            // no keyboard or mouse, the touch screens off -- and the joysticks) only every Nth boundary
            // (TE_VITA_SDL_PUMP_EVERY, default 4; 1 = every boundary as before); the controller read itself
            // (SDL_JoystickUpdate: sceCtrl) and the event queue -- the controller's events, the pads' pending presses --
            // every boundary, as before. ("pads: SDL event pump" 1.1-1.2 % of the device's game thread, 0.35-0.4 ms a tick)
            static const unsigned short profJoy = te::prof::Register("pads: SDL joystick update (the controller)");
            static const uint32_t pumpEvery = [] {
                const char* v = std::getenv("TE_VITA_SDL_PUMP_EVERY");
                const long n = v ? std::atol(v) : 4L;
                return uint32_t(n < 1 ? 1 : n > 60 ? 60 : n);
            }();
            static uint32_t boundary = 0;
            if (g_vpad.on) {
                // the controller read straight from sceCtrl every boundary; SDL's pump (its other polls, the
                // window's events) every Nth, its joystick update off (SDL_HINT_AUTO_UPDATE_JOYSTICKS)
                if (boundary++ % pumpEvery == 0) {
                    te::prof::Scope pumpScope(profPump, te::prof::kGame);
                    SDL_PumpEvents();
                }
                static const unsigned short profDirect = te::prof::Register("pads: the controller read (sceCtrl, direct)");
                te::prof::Scope directScope(profDirect, te::prof::kGame);
                ReadVitaPad(g_vpad);
            } else if (boundary++ % pumpEvery == 0) {
                te::prof::Scope pumpScope(profPump, te::prof::kGame);
                SDL_PumpEvents();
            } else {
                te::prof::Scope joyScope(profJoy, te::prof::kGame);
                SDL_JoystickUpdate();
            }
            {
                te::prof::Scope pumpScope(profPump, te::prof::kGame);
                SDL_Event e;
                while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {
                    if (e.type == SDL_QUIT || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE)) {
                        std::fprintf(stderr, "[te.gfx] window closed: exit\n");
                        te::RequestExit(0);
                    }
                    g_input->HandleEvent(e);
                }
            }
#else
            te::prof::Scope pumpScope(profPump, te::prof::kGame);
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                // This pump drains SDL's whole queue, so the window's close button arrives here as often as at the
                // window owner's pump (graphics PumpWindow): it ends the run the same way, at once.
                if (e.type == SDL_QUIT || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE)) {
                    std::fprintf(stderr, "[te.gfx] window closed: exit\n");
                    te::RequestExit(0);
                }
                g_input->HandleEvent(e);
            }
#endif
        }
        if (!g_injections.empty() && g_input->Kbm()) {
            const uint64_t boundary = te::Services::Get().clock().BoundaryCount();
            for (Injection& k : g_injections)
                if (Live(k, boundary)) g_input->Kbm()->InjectKey(k.scancode);
        }
        te::prof::Scope readScope(profRead, te::prof::kGame);
        const ps2r::platform::sdl::PadSample p = g_input->Frame(dt);
        s.connected = g_input->Connected() || te::GetSettings().pad_connected;
        s.buttons = p.buttons;
        s.lx = p.lx; s.ly = p.ly; s.rx = p.rx; s.ry = p.ry;
#if defined(__vita__)
        if (g_vpad.on && g_vpad.check_every && g_vpad.reads % g_vpad.check_every == 0) {
            CheckVitaPad(g_vpad);
            if (g_vpad.checked % 100 == 0)
                std::fprintf(stderr, "[twinsanityevolved] input: pad check (TE_VITA_PAD_CHECK): %llu boundaries read, %llu compared with SDL's "
                                     "own read, %llu differ\n", (unsigned long long)g_vpad.reads, (unsigned long long)g_vpad.checked,
                             (unsigned long long)g_vpad.differ);
        }
#endif
        return s;
    });
    g_installed = true;
    if (settings.verbose) std::fprintf(stderr, "[twinsanityevolved] input: SDL InputSystem on pad 0 (%s)\n", g_input->Sources().c_str());
    return true;
#else
    (void)pads;
    return false;
#endif
}

bool InjectKey(const std::string& key_name, uint64_t from, uint64_t to, std::string* error) {
#if TE_HAVE_SDL_INPUT
    const SDL_Scancode sc = SDL_GetScancodeFromName(key_name.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) { if (error) *error = "not an SDL key name: " + key_name; return false; }
    Injection k;
    k.scancode = static_cast<int>(sc);
    k.from = from;
    k.to = to;
    g_injections.push_back(k);
    return true;
#else
    (void)from; (void)to;
    if (error) *error = "no SDL input in this build: " + key_name;
    return false;
#endif
}

bool KeyHeld(const char* key_name) {
#if TE_HAVE_SDL_INPUT
    const SDL_Scancode sc = SDL_GetScancodeFromName(key_name);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    if (!g_injections.empty()) {
        const uint64_t boundary = te::Services::Get().clock().BoundaryCount();
        for (Injection& k : g_injections)
            if (k.scancode == int(sc) && Live(k, boundary)) return true;
    }
    if (!g_installed || !SDL_WasInit(SDL_INIT_EVENTS)) return false;
    int count = 0;
    const Uint8* state = SDL_GetKeyboardState(&count);
    return state && int(sc) < count && state[sc] != 0;
#else
    (void)key_name;
    return false;
#endif
}

bool RequestWindowClose() {
#if TE_HAVE_SDL_INPUT
    SDL_Event e{};
    e.type = SDL_QUIT;
    return SDL_PushEvent(&e) == 1;
#else
    return false;
#endif
}

void SetPumpOwned(bool owned) { g_pump_owned = owned; }
bool PumpOwned() { return g_pump_owned; }
bool Pumping() { return g_installed && g_pump_owned; }



#if TE_HAVE_SDL_INPUT
void HandleSdlEvent(const SDL_Event& event) { if (g_input) g_input->HandleEvent(event); }
#endif

std::string Sources() {
#if TE_HAVE_SDL_INPUT
    return g_input ? g_input->Sources() : std::string("none");
#else
    return "none";
#endif
}

void Shutdown() {
#if TE_HAVE_SDL_INPUT
    if (g_input) { g_input->Shutdown(); delete g_input; g_input = nullptr; }
    if (g_events_inited) { SDL_QuitSubSystem(SDL_INIT_EVENTS); g_events_inited = false; }
#endif
    g_installed = false;
}

}  // namespace te::input

extern "C" int te_input_vita_window_pump_skipped(void) {
#if defined(__vita__)
    static const bool pump = [] { const char* v = std::getenv("TE_VITA_PUMP"); return v && v[0] == '1'; }();
    static uint32_t blanks = 0;
    if (++blanks % 60 == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    return !pump && te::input::Pumping() ? 1 : 0;
#else
    return 0;
#endif
}
