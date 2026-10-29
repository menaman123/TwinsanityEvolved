// te_vita.cpp -- the PlayStation Vita runner's start-up (te_vita.h).
#include "te_vita.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// newlib's heap (sbrk): the default is far too small for the title's 32 MB linear memory, the 32 MB game stack and the
// engine's caches. Weakly referenced by the SDK's sbrk; this definition sizes the block it allocates at start-up.
#if defined(TE_NATIVE)
// The native build of the game code (TE_VITA_NATIVE, te_native.h): the game's 32 MB is the eboot's own .bss (the guest
// block), not a heap allocation -- the heap gives those 32 MB back, so the process' memory is what it was
extern "C" unsigned int _newlib_heap_size_user = 192u * 1024u * 1024u;
#else
extern "C" unsigned int _newlib_heap_size_user = 224u * 1024u * 1024u;
#endif
// Every thread the tree starts (std::thread: the render thread, the geometry builders, the audio feeder) takes the
// pthread default stack, 32 KB unless this says otherwise -- the builders' VU programs and strip buffers outgrow that
// and overwrote the memory beside the stack (mutexes included). Weakly referenced by the SDK's pthread_create.
extern "C" unsigned int _pthread_stack_default_user = 1024u * 1024u;

namespace te::vita {

namespace {

constexpr const char* kDataDir = "ux0:data/twinsanity";

std::vector<std::string> g_args;
std::vector<char*> g_argv;
SceUID g_log_fd = -1;
int g_synced_writes = 0;
SceUInt64 g_last_sync_us = 0;

// The start-up's lines are synced one by one (a crash before the first frame still leaves them on the card); after
// that a sync per line would stall the game thread on the memory card's flash whenever something logs mid-play, so the
// file is committed at most every few seconds -- by the logger thread
constexpr int kSyncedWrites = 400;
// THE SYNCED START-UP ENDS AT THE GAME'S FIRST PICTURE (LogRunning), not after 400 lines: on the device a
// synced line (written, then the file closed and reopened to commit its size) costs the logging thread 6-17 ms, and the
// pace census -- ~40 lines every 300 presents, printed by the DRAWING thread (graphics.cpp NotePresented -> NotePace) --
// then held the drawing thread 160-700 ms at every window end until line 400 (present ~2100 in device-run8.log: the
// title, its cutscene, the island pan): pictures replaced, the tick lock dropped 60 -> 30 at boundaries 1712, 2015, 2319
// (presents 1200, 1500, 1800), "drawing thread per picture ms max 438 / 674 / 273" -- the "60, then drops as if loading
// something in the background" a player saw. From the first picture on, every line goes through the logger thread.
std::atomic<bool> g_log_running{false};
// The logger thread COMMITS the file (closes and reopens it: on the device a sync alone leaves the directory entry's size
// behind the data) at most this often -- its own time, at the lowest priority; a crash loses at most this much of the log
constexpr SceUInt64 kSyncIntervalUs = 2000000;

std::string g_log_path;

// THE LOG IS WRITTEN BY MANY THREADS (the game, render, build, pacer, audio and FFmpeg threads all fprintf(stderr)), and
// the Vita's newlib stdio has NO locks: its sys/lock.h is built without _RETARGETABLE_LOCKING (every __lock_acquire is
// ((void)0)) and __sfp_lock_acquire / __sinit_lock_acquire are a bare `bx lr` in the eboot. Two threads in fprintf on the
// one line-buffered FILE raced on its buffer pointers: device-run3.log has lines overwritten ("12x512 (PAL), window,
// render thread on" -- the TTY copy in the core dump has "[te.gfx] renderer started: frame 512x512 (PAL) ...") and one
// written twice, and a line ("game state @ boundary 70") the TTY has and the file lost; the same race lets one thread's
// copy run past the end of the stdio buffer (heap corruption on three real cores). Hence: the FILE is UNBUFFERED (newlib
// then hands each chunk of a printf straight to LogWrite, touching no shared buffer), LogWrite gathers each thread's
// chunks into whole lines in a buffer of that thread's own, and complete lines go out one at a time under a kernel
// lightweight mutex (its work area in .bss, as the build pool's: no heap page under a kernel lock) together with the
// descriptor's close / reopen below -- which another thread could otherwise see half done.
SceKernelLwMutexWork g_log_lock;
bool g_log_lock_ok = false;
// TE_LOG_STAMP=1 (args.txt): each line prefixed with the process time, "[seconds.milliseconds] " -- the device has no
// other clock in its log (the start-up phases, the runtime shader compiles, a stall before a crash). Off by default:
// the lines are unchanged.
bool g_log_stamp = false;
thread_local char t_line[2048];
thread_local size_t t_line_len = 0;

// AFTER THE START-UP the lines go to the card from a logger thread ("te-log"): a calling thread (the game thread above
// all: the state trace, the census lines) copies its stamped line into a ring under the lock and signals the logger,
// which writes what the ring holds and commits the file every kSyncIntervalUs -- a sceIoWrite on the memory card, and the
// periodic commit (tens of ms), no longer land on whichever thread logged. The file's order is the ring's: the logger takes a batch
// and the file lock under the ring lock, and a writer that finds the ring full (or the logger not running) writes the ring
// and then its line itself, under both (the old synchronous path). The start-up lines stay synced one by one.
constexpr size_t kRingBytes = 128 * 1024;
char g_ring[kRingBytes];
char g_batch[kRingBytes];
size_t g_ring_head = 0, g_ring_tail = 0;   // bytes ever put / taken (the ring position is mod kRingBytes)
SceKernelLwMutexWork g_file_lock;          // the file and the TTY (taken after g_log_lock, never the other way)
bool g_file_lock_ok = false;
SceUID g_log_sema = -1;
bool g_log_async = false;                  // the logger thread runs (under g_log_lock)
uint64_t g_log_ring_lines = 0, g_log_sync_lines = 0;

// The TTY a line at a time (sceClibPrintf's own buffer is small)
void TtyBytes(const char* data, size_t len) {
    while (len > 0) {
        const char* nl = static_cast<const char*>(std::memchr(data, '\n', len));
        const size_t take = nl ? static_cast<size_t>(nl - data) + 1 : len;
        sceClibPrintf("%.*s", static_cast<int>(take), data);
        data += take;
        len -= take;
    }
}

// The file's writes (g_file_lock held): bytes, then the start-up's close/reopen or the periodic sync
void FileBytes(const char* data, size_t len, bool startup_line) {
    if (g_log_fd < 0 || len == 0) return;
    sceIoWrite(g_log_fd, data, static_cast<SceSize>(len));
    const SceUInt64 now = sceKernelGetProcessTimeWide();
    if (startup_line && g_synced_writes < kSyncedWrites && !g_log_running.load(std::memory_order_relaxed)) {
        // The start-up lines: the file is CLOSED and reopened for append after each one. On the device a sync alone
        // left the directory entry's size behind the data (a log read back after a crash ended on a 2 KB boundary, the
        // last lines lost); a close commits the size, and start-up logs only a few dozen lines.
        ++g_synced_writes;
        g_last_sync_us = now;
        sceIoClose(g_log_fd);
        g_log_fd = sceIoOpen(g_log_path.c_str(), SCE_O_WRONLY | SCE_O_APPEND, 0777);
    } else if (now - g_last_sync_us >= kSyncIntervalUs) {
        // the periodic commit (the logger thread's, or a writer's that found the ring full): closed and reopened, as the
        // start-up lines are -- the directory entry's size follows the data (a sync alone did not on the device)
        g_last_sync_us = now;
        sceIoClose(g_log_fd);
        g_log_fd = sceIoOpen(g_log_path.c_str(), SCE_O_WRONLY | SCE_O_APPEND, 0777);
    }
}

// g_log_lock held: the ring's bytes out into `out` (returns the count)
size_t TakeRing(char* out) {
    const size_t n = g_ring_head - g_ring_tail;
    for (size_t k = 0; k < n;) {
        const size_t at = (g_ring_tail + k) % kRingBytes;
        const size_t run = std::min(n - k, kRingBytes - at);
        std::memcpy(out + k, g_ring + at, run);
        k += run;
    }
    g_ring_tail = g_ring_head;
    return n;
}
void PutRing(const char* data, size_t len) {
    for (size_t k = 0; k < len;) {
        const size_t at = (g_ring_head + k) % kRingBytes;
        const size_t run = std::min(len - k, kRingBytes - at);
        std::memcpy(g_ring + at, data + k, run);
        k += run;
    }
    g_ring_head += len;
}

int LoggerMain(SceSize, void*) {
    bool uncommitted = false;   // bytes written since the last commit (FileBytes commits every kSyncIntervalUs)
    for (;;) {
        // a quiet card is committed too: the last lines before a long silence (or a crash) reach the directory entry
        SceUInt timeout = SceUInt(kSyncIntervalUs);
        const int woke = sceKernelWaitSema(g_log_sema, 1, uncommitted ? &timeout : nullptr);
        sceKernelLockLwMutex(&g_log_lock, 1, nullptr);
        if (g_ring_head == g_ring_tail) {
            sceKernelUnlockLwMutex(&g_log_lock, 1);
            if (woke < 0 && uncommitted) {   // the timeout: nothing new for a while -- commit what was written
                sceKernelLockLwMutex(&g_file_lock, 1, nullptr);
                if (g_log_fd >= 0) {
                    g_last_sync_us = sceKernelGetProcessTimeWide();
                    sceIoClose(g_log_fd);
                    g_log_fd = sceIoOpen(g_log_path.c_str(), SCE_O_WRONLY | SCE_O_APPEND, 0777);
                }
                sceKernelUnlockLwMutex(&g_file_lock, 1);
                uncommitted = false;
            }
            continue;
        }
        sceKernelLockLwMutex(&g_file_lock, 1, nullptr);
        const size_t n = TakeRing(g_batch);
        sceKernelUnlockLwMutex(&g_log_lock, 1);
        TtyBytes(g_batch, n);
        const SceUInt64 committed_at = g_last_sync_us;
        FileBytes(g_batch, n, false);
        uncommitted = g_last_sync_us == committed_at;   // (FileBytes committed when its interval was up)
        sceKernelUnlockLwMutex(&g_file_lock, 1);
    }
    return 0;
}

// g_log_lock held: the logger thread, once the start-up's synced lines are written (TE_LOG_ASYNC=0: never)
void StartLogger() {
    static bool tried = false;
    if (tried || !g_file_lock_ok) return;
    tried = true;
    const char* v = std::getenv("TE_LOG_ASYNC");
    if (v && v[0] == '0') return;
    g_log_sema = sceKernelCreateSema("te-log", 0, 0, 0x7fffffff, nullptr);
    if (g_log_sema < 0) return;
    // a small kernel thread at 160, below the builders (144): it waits on the card, and only it does (starved, the ring
    // fills and the writers fall back to writing themselves)
    const SceUID t = sceKernelCreateThread("te-log", LoggerMain, 160, 0x8000, 0, 0, nullptr);
    if (t < 0 || sceKernelStartThread(t, 0, nullptr) < 0) return;
    g_log_async = true;
}

// One whole line (or a piece of an overlong one) to the TTY and the file.
void EmitLine(const char* data, size_t len) {
    if (len == 0) return;
    char stamp[32] = {};
    int stamp_len = 0;
    if (g_log_stamp) {
        const SceUInt64 us = sceKernelGetProcessTimeWide();
        stamp_len = std::snprintf(stamp, sizeof stamp, "[%llu.%03llu] ", static_cast<unsigned long long>(us / 1000000u),
                                  static_cast<unsigned long long>(us / 1000u % 1000u));
        if (stamp_len < 0 || stamp_len >= static_cast<int>(sizeof stamp)) stamp_len = 0;
    }
    if (g_log_lock_ok) sceKernelLockLwMutex(&g_log_lock, 1, nullptr);
    const size_t need = size_t(stamp_len) + len;
    if (g_log_async && g_log_fd >= 0 && need <= kRingBytes - (g_ring_head - g_ring_tail)) {
        PutRing(stamp, size_t(stamp_len));
        PutRing(data, len);
        ++g_log_ring_lines;
        sceKernelUnlockLwMutex(&g_log_lock, 1);
        sceKernelSignalSema(g_log_sema, 1);
        return;
    }
    // On this thread: the ring's bytes first (the file keeps the lines' order), then the line. Both the file and the
    // kernel's TTY (sceClibPrintf): an emulator shows the TTY in its own log at once, while its host-side file buffer may
    // reach the disk only at close.
    if (g_file_lock_ok) sceKernelLockLwMutex(&g_file_lock, 1, nullptr);
    if (g_ring_head != g_ring_tail) {
        const size_t n = TakeRing(g_batch);
        TtyBytes(g_batch, n);
        FileBytes(g_batch, n, false);
    }
    sceClibPrintf("%.*s%.*s", stamp_len, stamp, static_cast<int>(len), data);
    if (g_log_fd >= 0) {
        if (stamp_len) sceIoWrite(g_log_fd, stamp, static_cast<SceSize>(stamp_len));
        FileBytes(data, len, true);
    }
    ++g_log_sync_lines;
    if (g_file_lock_ok) sceKernelUnlockLwMutex(&g_file_lock, 1);
    if (g_synced_writes >= kSyncedWrites || g_log_running.load(std::memory_order_relaxed)) StartLogger();
    if (g_log_lock_ok) sceKernelUnlockLwMutex(&g_log_lock, 1);
}

// newlib's writer for the unbuffered FILE: the calling thread's chunks gathered up to each '\n'. Always "writes" all n
// bytes (the TTY has them even when the file is gone), so the stream never enters its error state.
int LogWrite(void*, const char* buf, int n) {
    if (!buf || n <= 0) return 0;
    const char* p = buf;
    size_t left = static_cast<size_t>(n);
    while (left > 0) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', left));
        const size_t take = nl ? static_cast<size_t>(nl - p) + 1 : left;
        if (t_line_len + take > sizeof t_line) {   // longer than the buffer: what it holds goes out as it is
            EmitLine(t_line, t_line_len);
            t_line_len = 0;
        }
        if (take > sizeof t_line) {
            EmitLine(p, take);
        } else {
            std::memcpy(t_line + t_line_len, p, take);
            t_line_len += take;
            if (nl) {
                EmitLine(t_line, t_line_len);
                t_line_len = 0;
            }
        }
        p += take;
        left -= take;
    }
    return n;
}

// vitaGL compiles the engine's shaders with Sony's runtime compiler, libshacccg.suprx, which a Vita does not ship: the
// installed vitaGL (built NO_DEBUG) does not check that it loaded and the first glCompileShader dies without a word.
// Refused here instead, with the file's two accepted locations in the log.
bool ShaderCompilerPresent() {
    SceIoStat st{};
    return sceIoGetstat("ur0:data/libshacccg.suprx", &st) >= 0 || sceIoGetstat("ur0:data/external/libshacccg.suprx", &st) >= 0;
}

// The default arguments: the disc image, the settings and the memory cards under the data folder; the state trace on
// (the milestone's evidence); no keyboard. The movies play (the lane links its own MPEG-2 FFmpeg build:
// tools/vita/ffmpeg_vita.sh); --skip-movies in args.txt still skips them.
// THE GAME RATE IS THE PAL DISC'S OWN 50 Hz: SLES-52568 is the PAL Crash Twinsanity -- its developers' game loop
// ticks 50 times a second (frameTime 180 a tick), and the port's 60 Hz default ran that loop 1.2 times as often.
// The built-in arguments carry NO --hz: a command-line --hz wins over ps2r.ini's game.hz, so an earlier built-in
// "--hz 50" made the Options' GAME RATE row (game.hz = 60) a dead switch -- the device ran 50 Hz whatever the player chose.
// Now ps2r.ini's game.hz decides (the row writes it, "at the next start"), and a ps2r.ini without the key -- or none yet --
// runs the Vita's settings default, the PAL disc's 50 Hz (runner/main.cpp kSettingsDefaultHz). An args.txt replaces these
// arguments: its own --hz decides for its run, or game.hz / the default 50 when it names a ps2r.ini, or the engine's 60
// with --config none (the emulator's deterministic legs and every earlier script keep their 60 Hz).
std::vector<std::string> DefaultArgs(const std::string& disc) {
    return {"--iso", disc, "--trace-state", "--no-kbm",
            "--mc-dir", std::string(kDataDir) + "/memcards", "--config", std::string(kDataDir) + "/ps2r.ini"};
}

bool FileThere(const std::string& path) {
    SceIoStat st{};
    return sceIoGetstat(path.c_str(), &st) >= 0 && !SCE_S_ISDIR(st.st_mode);
}

// THE DISC CHOICE: ux0:data/twinsanity/disc-image.txt -- the SAME file the launcher writes in a computer's game
// folder: one line, the disc image's path -- names the disc the built-in arguments start
// with; without the file, game.iso. A Vita path (ux0:..., ux1:..., uma0:..., imc0:...: a device name, its number and a
// colon) is taken as it is; anything else is relative to ux0:data/twinsanity (a bare file name: the file beside game.iso);
// a computer's absolute path (/Users/..., C:\...: a game folder's own file copied as it is) is read as its FILE NAME in
// ux0:data/twinsanity. Two discs can live side by side (game.iso and ntsc.iso) and one line switches them -- the launcher's
// PS Vita folder writes it. A named file that is not there falls back to game.iso (when that is there); `said` is the
// start-up line's reason.
std::string ChooseDisc(std::string* said) {
    const std::string fallback = std::string(kDataDir) + "/game.iso";
    const std::string file = std::string(kDataDir) + "/disc-image.txt";
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        *said = "no disc-image.txt";
        return fallback;
    }
    std::string line;
    std::getline(f, line);
    if (line.size() >= 3 && uint8_t(line[0]) == 0xEF && uint8_t(line[1]) == 0xBB && uint8_t(line[2]) == 0xBF) line.erase(0, 3);
    auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!line.empty() && blank(line.back())) line.pop_back();
    size_t lead = 0;
    while (lead < line.size() && blank(line[lead])) ++lead;
    line.erase(0, lead);
    if (line.size() >= 2 && (line.front() == '"' || line.front() == '\'') && line.back() == line.front()) line = line.substr(1, line.size() - 2);
    std::replace(line.begin(), line.end(), '\\', '/');
    if (line.empty()) {
        *said = "disc-image.txt is empty";
        return fallback;
    }
    // a device: letters, digits, a colon (ux0:, uma0:, imc0:, ux1:) -- a computer's drive letter (C:) has no digit
    size_t k = 0;
    while (k < line.size() && std::isalpha(static_cast<unsigned char>(line[k]))) ++k;
    const size_t letters = k;
    while (k < line.size() && std::isdigit(static_cast<unsigned char>(line[k]))) ++k;
    const bool device = letters > 0 && k > letters && k < line.size() && line[k] == ':';
    std::string path, how;
    if (device) {
        path = line;
        how = "disc-image.txt";
    } else {
        const bool computer = line.front() == '/' || (line.size() >= 2 && std::isalpha(static_cast<unsigned char>(line[0])) && line[1] == ':');
        std::string rel = line;
        if (computer) {
            const size_t slash = rel.find_last_of("/:");
            rel = slash == std::string::npos ? rel : rel.substr(slash + 1);
            how = "disc-image.txt names a computer's path (" + line + "): its file name here";
        } else {
            while (rel.compare(0, 2, "./") == 0) rel.erase(0, 2);
            how = "disc-image.txt";
        }
        path = std::string(kDataDir) + "/" + rel;
    }
    if (!FileThere(path) && FileThere(fallback) && path != fallback) {
        *said = "disc-image.txt names " + path + ", which is not there: game.iso";
        return fallback;
    }
    *said = how;
    return path;
}

// The DEVICE DIAGNOSIS set (the port runs on hardware and the log comes back from a run of the built-in
// arguments, no args.txt): every line time-stamped, the pace / heap / vitaGL census every 300 presents, the section
// profiler's shares of the game / render / build threads every 300 presents. Applied only when there is no args.txt
// (an args.txt is a deliberate run: its own TE_* tokens decide) and never over a variable already set. Off again by
// removing it here once the device numbers are in.
constexpr const char* kDiagnosisEnv[][2] = {
    {"TE_LOG_STAMP", "1"},
    {"TE_GFX_SUMMARY_EVERY", "300"},
    {"TE_PROF", "1"},
    {"TE_PROF_EVERY", "300"},
    // every tick over 50 ms (two and a half fields at 50 Hz: a hitch) logged with its load work -- textures, model parts,
    // native meshes, the waits, vitaGL's allocations -- and the game thread's sections sampled in it (te_load_census.h)
    {"TE_TICK_LOG_MS", "50"},
    // (the lean PS2 programs are always on -- their off arm would also turn the raw mesh draws off, which need a
    // lean mesh-path program -- so the log's PS2 program lines and GPU probes are the lean programs' throughout;
    // the game thread's fast seams (TE_GAMECPU2) are always on, no TE_AB; the Vita's tick lock runs its default
    // 'auto' (60 / 30 Hz), its "[te.vita.lock]" lines report the mode)
    // (no A/B arms any more. An earlier device log's packets A/B -- TE_GFX_VITA_PACKETS=ab300 -- drew every other
    // 300-present window WITHOUT the mesh draw packets: its "off" arm was the 20 fps title pan (te-render 32.5 ms a present,
    // the again / texture-swap paths at 15-29 us a draw against 10-11 us replayed). The packets replayed in every "on"
    // window in both mesh forms; the native meshes' A/B -- TE_GFX_VITA_MESH_INDEX=ab600 -- gave no comparable windows
    // (different scenes in each arm). Both now run their defaults: packets on, the meshes indexed (an earlier build's were the
    // triangle lists); every census line that an arm changes says which arm its window ran.)
    // the GPU's own fill census (engine/render/backends/gl/gl_fill_census.h): one frame in 32 every draw counts the samples
    // that pass its depth / stencil tests in a GXM visibility counter, with its true screen box and its vertices -- the
    // "[te.gfx.vita] fill per sampled frame" lines (2 GXM calls a draw on the sampled frames only)
    {"TE_GFX_VITA_FILL", "32"},
    // the profile's tail: 40 rows per thread down to 0.2 % of its samples (te_prof.cpp; defaults 16 / 0.5 %) --
    // the game thread's seams below the top 16 (each 0.2-0.8 % of a hub tick on the device) get their own rows
    {"TE_PROF_ROWS", "40"},
    {"TE_PROF_MIN", "0.2"},
    // NO A/B ARMS: the device run measures one build at the 50 Hz game rate. The earlier arms -- the native
    // meshes' forms alternating every present (TE_GFX_VITA_MESH_INDEX=ab1), the compact mesh records and the culling memo
    // against their off arms in 300-present windows (TE_GFX_COMPACT=ab300, TE_GFX_CULL_MEMO=ab300) -- are gone: every one
    // runs its default (the indexed meshes -- the GPU's cost was the same in both forms on the device, -0.4 / +0.0 / +0.2 ms
    // --, the compact records on, the memo on), so every window of the log is the shipped path. An args.txt token
    // (TE_GFX_VITA_MESH_INDEX=ab1, TE_GFX_COMPACT=ab300, ...) still runs an arm.
    // the controller is read straight from sceCtrl (platform/nativeee/host/te_input.cpp VitaPad: SDL's joystick
    // values and mapping replicated); every 60th boundary SDL's own read runs too and its 27 game-controller values are
    // compared with the replica's -- the player's real sticks and buttons check it on the device (a line every 100 checks:
    // "pad check ... N compared with SDL's own read, M differ"); ~0.4 ms once in 60 ticks, no effect on the pad's sample
    {"TE_VITA_PAD_CHECK", "60"},
};

void ApplyDiagnosisEnv() {
    std::string said;
    for (const auto& kv : kDiagnosisEnv) {
        if (std::getenv(kv[0])) continue;
        setenv(kv[0], kv[1], 0);
        said += std::string(" ") + kv[0] + "=" + kv[1];
    }
    if (!said.empty()) std::fprintf(stderr, "[twinsanityevolved] vita: diagnosis set (built in, no args.txt):%s\n", said.c_str());
}

// ux0:data/twinsanity/args.txt, when present, replaces the defaults: whitespace-separated tokens, # to end of line. A
// token NAME=value whose name starts with TE_ or PS2E_ sets that environment variable instead (the device has no shell to
// set the tree's diagnostic switches from).
bool ReadArgsFile(std::vector<std::string>* out) {
    std::ifstream f(std::string(kDataDir) + "/args.txt");
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            const size_t start = i;
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (i == start) continue;
            const std::string token = line.substr(start, i - start);
            const size_t eq = token.find('=');
            if (eq != std::string::npos && (token.rfind("TE_", 0) == 0 || token.rfind("PS2E_", 0) == 0)) {
                setenv(token.substr(0, eq).c_str(), token.substr(eq + 1).c_str(), 1);
                std::fprintf(stderr, "[twinsanityevolved] vita: %s from args.txt\n", token.c_str());
                continue;
            }
            out->push_back(token);
        }
    }
    return !out->empty();
}

}  // namespace

void Step(const char* what) {
    std::fprintf(stderr, "[twinsanityevolved] vita: start-up: %s\n", what);
    std::fflush(stderr);
}

void LogRunning() {
    if (g_log_running.exchange(true)) return;
    if (g_log_lock_ok) sceKernelLockLwMutex(&g_log_lock, 1, nullptr);
    StartLogger();
    const bool async = g_log_async;
    if (g_log_lock_ok) sceKernelUnlockLwMutex(&g_log_lock, 1);
    std::fprintf(stderr, "[twinsanityevolved] vita: the game's first picture: the log goes to the card through the logger thread "
                         "from here (%s; %d start-up lines were committed one by one)\n",
                 async ? "no thread waits on the card" : "TE_LOG_ASYNC=0: the logging threads write it themselves", g_synced_writes);
}

void Startup(int* argc, char*** argv) {
    // The highest clocks an application may ask for (the system starts it at 333 MHz ARM / 111 MHz GPU): the game's
    // frame needs every cycle the four cores and the GPU have
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(kDataDir, 0777);
    sceIoMkdir((std::string(kDataDir) + "/memcards").c_str(), 0777);
    // The log: a kernel descriptor written and SYNCED per line (the emulator's host file buffer is otherwise flushed
    // only at close, which a crashed run never reaches), behind newlib's stdio through funopen so every fprintf(stderr)
    // / printf in the tree lands there unchanged. (Vita3K does not implement sceIoSyncByFd: there the file grows per
    // 4 KB of its host buffer, and CloseLog() after the exit census flushes the rest.)
    g_log_path = std::string(kDataDir) + "/twinsanityevolved.log";
    g_log_lock_ok = sceKernelCreateLwMutex(&g_log_lock, "te-log", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, nullptr) >= 0;
    g_file_lock_ok = g_log_lock_ok && sceKernelCreateLwMutex(&g_file_lock, "te-log-file", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, nullptr) >= 0;
    g_log_fd = sceIoOpen(g_log_path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (g_log_fd >= 0) {
        if (FILE* f = funopen(nullptr, nullptr, &LogWrite, nullptr, nullptr)) {
            // unbuffered: no stdio buffer shared by the threads (see LogWrite); LogWrite makes the lines
            std::setvbuf(f, nullptr, _IONBF, 0);
            stderr = f;
            stdout = f;
        }
    }

    SceKernelFreeMemorySizeInfo mem{};
    mem.size = sizeof mem;
    if (sceKernelGetFreeMemorySize(&mem) >= 0)
        std::fprintf(stderr, "[twinsanityevolved] vita: free memory at start-up: user %d KB, cdram %d KB, phycont %d KB; newlib heap %u MB\n",
                     mem.size_user / 1024, mem.size_cdram / 1024, mem.size_phycont / 1024, _newlib_heap_size_user >> 20);
    std::fprintf(stderr, "[twinsanityevolved] vita: clocks ARM %d MHz, bus %d MHz, GPU %d MHz, crossbar %d MHz\n",
                 scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
                 scePowerGetGpuXbarClockFrequency());
    if (!ShaderCompilerPresent()) {
        std::fprintf(stderr, "[twinsanityevolved] vita: the runtime shader compiler is MISSING: install libshacccg.suprx as "
                             "ur0:data/libshacccg.suprx (or ur0:data/external/libshacccg.suprx) -- the game cannot draw without it\n");
        std::fflush(stderr);
        sceKernelExitProcess(0);
    }
    std::fprintf(stderr, "[twinsanityevolved] vita: runtime shader compiler found (libshacccg.suprx)\n");
    // An SDL assertion on the device would otherwise try a message box (sceCommonDialog) and abort without a word in
    // this log: logged here instead, and the start-up continues (the first device runs stopped right after the FPU
    // line with SDL_ReportAssertion on the crashed thread's stack).
    SDL_SetAssertionHandler([](const SDL_AssertData* d, void*) -> SDL_AssertState {
        std::fprintf(stderr, "[twinsanityevolved] vita: SDL assertion failed: '%s' at %s:%d in %s -- ignored, continuing\n",
                     d->condition ? d->condition : "?", d->filename ? d->filename : "?", d->linenum, d->function ? d->function : "?");
        std::fflush(stderr);
        return SDL_ASSERTION_IGNORE;
    }, nullptr);

    g_args.clear();
    const bool from_file = ReadArgsFile(&g_args);
    if (!from_file) {
        std::string said;
        const std::string disc = ChooseDisc(&said);
        g_args = DefaultArgs(disc);
        std::fprintf(stderr, "[twinsanityevolved] vita: disc image %s (%s)\n", disc.c_str(), said.c_str());
        ApplyDiagnosisEnv();
    }
    // SDL's per-pump work on the Vita (SDL_vitavideo.c VITA_PumpEvents): the game reads no touch -- SDL's front and back
    // panel polls (two sceTouchPeek per pump) are left out; and the power tick that keeps the device awake is the
    // platform's own, once a second (graphics.cpp PumpWindow), not SDL's at every pump (SDL_VIDEO_ALLOW_SCREENSAVER=1).
    // Set only when not set already (args.txt TE_ tokens cannot name them: a run that wants SDL's reverts it here).
    setenv("VITA_DISABLE_TOUCH_FRONT", "1", 0);
    setenv("VITA_DISABLE_TOUCH_BACK", "1", 0);
    setenv("SDL_VIDEO_ALLOW_SCREENSAVER", "1", 0);
    // the compiled shaders' cache (engine/render/backends/gl/gl_shader_library.cpp): Sony's compiler runs only when a
    // shader changed (TE_GL_SHADER_CACHE=0 in args.txt: compile everything, store nothing)
    setenv("TE_GL_SHADER_CACHE", (std::string(kDataDir) + "/shaders").c_str(), 0);
    if (const char* stamp = std::getenv("TE_LOG_STAMP")) g_log_stamp = stamp[0] == '1';
    g_argv.clear();
    g_argv.push_back((*argv && (*argv)[0]) ? (*argv)[0] : const_cast<char*>("twinsanityevolved"));
    for (std::string& a : g_args) g_argv.push_back(a.data());
    g_argv.push_back(nullptr);
    *argc = static_cast<int>(g_argv.size()) - 1;
    *argv = g_argv.data();
    std::fprintf(stderr, "[twinsanityevolved] vita: arguments (%s):", from_file ? "ux0:data/twinsanity/args.txt" : "built in");
    for (const std::string& a : g_args) std::fprintf(stderr, " %s", a.c_str());
    std::fprintf(stderr, "\n");
}

void CloseLog() {
    std::fflush(stderr);
    std::fflush(stdout);
    if (t_line_len) {   // the closing thread's unterminated last line
        EmitLine(t_line, t_line_len);
        t_line_len = 0;
    }
    if (g_log_lock_ok) sceKernelLockLwMutex(&g_log_lock, 1, nullptr);
    if (g_file_lock_ok) sceKernelLockLwMutex(&g_file_lock, 1, nullptr);
    if (g_ring_head != g_ring_tail) {   // what the logger thread has not written yet
        const size_t n = TakeRing(g_batch);
        TtyBytes(g_batch, n);
        FileBytes(g_batch, n, false);
    }
    const SceUID fd = g_log_fd;
    g_log_fd = -1;   // EmitLine: the TTY only from here
    if (fd >= 0) {
        sceIoSyncByFd(fd, 0);
        sceIoClose(fd);
    }
    if (g_file_lock_ok) sceKernelUnlockLwMutex(&g_file_lock, 1);
    if (g_log_lock_ok) sceKernelUnlockLwMutex(&g_log_lock, 1);
}

namespace {
// VABS (VFP) on the given bits under FPSCR's mode bits (22..25: RMode, FZ, DN) replaced by `mode`; the result's bits.
// The instruction itself, not whatever the compiler would emit for fabs: TE_W2C_VFP_ABS relies on VABS.
uint32_t VabsBitsUnder(uint32_t bits, uint32_t mode) {
    uint32_t saved, out;
    asm volatile("vmrs %0, fpscr" : "=r"(saved));
    const uint32_t set = (saved & ~0x03C00000u) | mode;
    asm volatile("vmsr fpscr, %0" : : "r"(set) : "memory");
    asm volatile("vmov s15, %1\n\tvabs.f32 s15, s15\n\tvmov %0, s15" : "=r"(out) : "r"(bits) : "s15");
    asm volatile("vmsr fpscr, %0" : : "r"(saved) : "memory");
    return out;
}
uint64_t VabsBitsUnder64(uint64_t bits, uint32_t mode) {
    uint32_t saved, lo, hi;
    asm volatile("vmrs %0, fpscr" : "=r"(saved));
    const uint32_t set = (saved & ~0x03C00000u) | mode;
    asm volatile("vmsr fpscr, %0" : : "r"(set) : "memory");
    asm volatile("vmov d7, %2, %3\n\tvabs.f64 d7, d7\n\tvmov %0, %1, d7"
                 : "=r"(lo), "=r"(hi)
                 : "r"(uint32_t(bits)), "r"(uint32_t(bits >> 32))
                 : "d7");
    asm volatile("vmsr fpscr, %0" : : "r"(saved) : "memory");
    return uint64_t(hi) << 32 | lo;
}
}  // namespace

bool ReportModuleCode(const void* guest_memory) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(guest_memory);
#if defined(TE_NATIVE)
    std::fprintf(stderr, "[twinsanityevolved] vita: game code: NATIVE ARM (TE_VITA_NATIVE: the decomp compiled by the Vita's GCC, "
                         "no wasm2c module); the 32 MB guest block at 0x%08X (%s)\n",
                 unsigned(base), (base & 4095u) == 0 ? "page-aligned" : "NOT page-aligned");
    return true;
#endif
#if defined(TE_W2C_UNCHECKED_MEMORY)
    const char* memory = "unchecked (TE_VITA_UNCHECKED_MEMORY)";
#else
    const char* memory = "CHECKED (TE_VITA_UNCHECKED_MEMORY=OFF: every access tested, a stray one traps)";
#endif
#if defined(TE_W2C_VFP_ABS) && defined(TE_W2C_UNCHECKED_TRUNC) && defined(TE_W2C_FIXED_TABLE)
    const char* lean = "abs = VABS, float->int = VCVT, call_indirect against the fixed table size (TE_VITA_W2C_LEAN)";
#elif defined(TE_W2C_VFP_ABS) || defined(TE_W2C_UNCHECKED_TRUNC) || defined(TE_W2C_FIXED_TABLE)
    const char* lean = "part of TE_VITA_W2C_LEAN";
#else
    const char* lean = "wasm2c's guards (TE_VITA_W2C_LEAN=OFF)";
#endif
    std::fprintf(stderr, "[twinsanityevolved] vita: game module code: memory %s; %s; the 32 MB guest memory at 0x%08X "
                         "(%s)\n",
                 memory, lean, unsigned(base), (base & 4095u) == 0 ? "page-aligned" : "NOT page-aligned");
    // The inputs where a C fabs could differ from a sign clear: quiet and signalling NaNs with payloads and signs,
    // infinities, denormals (FZ would flush them in an arithmetic operation), zeros
    static const uint32_t kIn32[] = {0x7fc00000u, 0xffc00000u, 0x7fa00001u, 0xffa00001u, 0x7f800001u, 0xff800001u,
                                     0xffbfffffu, 0x7f800000u, 0xff800000u, 0x80000001u, 0x807fffffu, 0x00000001u,
                                     0x80000000u, 0x00000000u, 0xbf800000u, 0x3f800000u, 0xff7fffffu};
    static const uint64_t kIn64[] = {0x7ff8000000000000ull, 0xfff8000000000001ull, 0x7ff0000000000001ull,
                                     0xfff4000000000000ull, 0xfff0000000000000ull, 0x8000000000000001ull,
                                     0x800fffffffffffffull, 0x8000000000000000ull, 0xbff0000000000000ull};
    // the game thread's mode on the device (RZ | FZ | DN), the default (RN), RZ alone, FZ | DN alone
    static const uint32_t kModes[] = {0x03C00000u, 0x00000000u, 0x00C00000u, 0x03000000u};
    unsigned checked = 0, wrong = 0;
    uint32_t first_in = 0, first_out = 0, first_mode = 0;
    for (uint32_t mode : kModes) {
        for (uint32_t in : kIn32) {
            const uint32_t out = VabsBitsUnder(in, mode);
            ++checked;
            if (out != (in & 0x7fffffffu) && wrong++ == 0) { first_in = in; first_out = out; first_mode = mode; }
        }
        for (uint64_t in : kIn64) {
            const uint64_t out = VabsBitsUnder64(in, mode);
            ++checked;
            if (out != (in & 0x7fffffffffffffffull) && wrong++ == 0) {
                first_in = uint32_t(in >> 32); first_out = uint32_t(out >> 32); first_mode = mode;
            }
        }
    }
    if (wrong == 0) {
        std::fprintf(stderr, "[twinsanityevolved] vita: VABS is the sign bit cleared and nothing else: %u of %u inputs (NaN "
                             "payloads, signalling NaNs, denormals, infinities; FPSCR RZ|FZ|DN, RN, RZ, FZ|DN)\n", checked, checked);
        return true;
    }
    std::fprintf(stderr, "[twinsanityevolved] vita: WARNING: VABS is NOT a plain sign clear here: %u of %u inputs differ (first "
                         "0x%08X -> 0x%08X under FPSCR mode 0x%08X)%s\n",
                 wrong, checked, first_in, first_out, first_mode,
#if defined(TE_W2C_VFP_ABS)
                 " -- this build's wasm abs (TE_VITA_W2C_LEAN) may differ from the checked form on those values"
#else
                 ""
#endif
    );
    return false;
}

}  // namespace te::vita
