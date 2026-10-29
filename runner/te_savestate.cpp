// te_savestate.cpp -- see te_savestate.h.
#include "te_savestate.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef Yield   // winbase.h's macro; te::game::Yield is a function
#else
#include <dlfcn.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#elif defined(__linux__)
#include <sys/auxv.h>
#include <sys/personality.h>
#include <unistd.h>
// the image's bounds, from the linker (C names at global scope: declared inside a namespace they would not resolve)
extern "C" char __executable_start[], _end[];
#endif

#include "engine/core/savestate/savestate_format.h"
#include "engine/platform/host/platform.h"
#include "savestate.h"
#include "te_game.h"
#include "te_run.h"
#include "te_services.h"

#if !defined(_WIN32)
extern char** environ;
// The stack protector's per-process random canary: every protected frame on the game stack holds it, so a loaded stack
// must carry the loading process's value (the saved value is replaced word for word; 64 random bits never collide).
#if defined(__linux__)
// glibc keeps it in the thread block, not in an exported variable: the value is the kernel's AT_RANDOM bytes with the
// low byte cleared (glibc's _dl_setup_stack_chk_guard), the same in every thread of the process.
namespace {
uintptr_t StackGuardNow() {
    uintptr_t v = 0;
    if (const void* random = reinterpret_cast<const void*>(getauxval(AT_RANDOM))) std::memcpy(&v, random, sizeof v);
    return v & ~uintptr_t(0xff);
}
}  // namespace
#else
extern "C" uintptr_t __stack_chk_guard;
#endif
#else
// Windows (/GS, clang-cl's default): a protected frame holds __security_cookie XOR its own rsp, never the raw cookie, so
// the words cannot be swapped -- the loading PROCESS adopts the saved cookie instead (the CRT's globals are writable;
// __security_init_cookie sets them at start-up the same way). That is exact only when the stack is at the saved address
// (RestoreHead refuses otherwise) and it covers every frame on the game stack, the engine's and the CRT's included.
extern "C" uintptr_t __security_cookie;
extern "C" uintptr_t __security_cookie_complement;
namespace { uintptr_t StackGuardNow() { return __security_cookie; } }
#endif
#if !defined(_WIN32) && !defined(__linux__)
namespace { uintptr_t StackGuardNow() { return __stack_chk_guard; } }
#endif

namespace te::savestate_runner {

namespace {
using te::savestate::Blob;
using te::savestate::PayloadReader;

w2c_env* g_env = nullptr;
w2c_game* g_instance = nullptr;
uint32_t g_build_id = 0;
uint64_t g_loaded_boundary = 0;
std::vector<std::string> g_last_sections;   // "TAG:vN" of the state Save wrote last (WriteSidecar's sections=)

// The saved process's bases (from the head section) while a restore runs
struct Bases {
    uint64_t image_base = 0, image_size = 0, memory_base = 0, table_base = 0, table_bytes = 0, stack_lo = 0, stack_hi = 0,
             instance_addr = 0, env_addr = 0;
    uint64_t boundary = 0;
    uint32_t build_id = 0;
    uint64_t stack_guard = 0;
};
Bases g_saved;

constexpr uint32_t kHeadVersion = 1;

}  // namespace

// CRC32 of our own executable: the build identity a state is tied to (table indices, thunk pools, globals layout).
// The path is the engine's (platform/host: _NSGetExecutablePath / /proc/self/exe / GetModuleFileNameW) -- one arm per
// platform lives there, not here.
uint32_t BuildId() {
    if (g_build_id) return g_build_id;
    std::string err;
    const std::string path = ps2r::platform::host::ExecutablePath(&err);
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    g_build_id = bytes.empty() ? 1u : ps2r::savestate::Crc32(bytes.data(), bytes.size());
    return g_build_id;
}

namespace {

uint64_t StackTop() {
    const te::game::Fiber& f = te::game::Get();
    return (uint64_t(uintptr_t(f.stack)) + f.stack_size) & ~uint64_t(15);
}

uint64_t TableBytes() { return uint64_t(g_instance->w2c_0x5F_indirect_function_table.size) * sizeof(wasm_rt_funcref_t); }

// ---- the head section --------------------------------------------------------------------------------------------------
void SerializeHead(std::vector<uint8_t>& out) {
    const ImageInfo img = Image();
    const te::game::Fiber& f = te::game::Get();
    te::savestate::Put(out, kHeadVersion);
    te::savestate::Put(out, BuildId());
    te::savestate::Put(out, f.boundary);
    te::savestate::Put(out, img.base);
    te::savestate::Put(out, img.size);
    te::savestate::Put(out, uint64_t(uintptr_t(g_env->memory.data)));
    te::savestate::Put(out, uint64_t(uintptr_t(g_instance->w2c_0x5F_indirect_function_table.data)));
    te::savestate::Put(out, TableBytes());
    te::savestate::Put(out, uint64_t(uintptr_t(f.stack)));
    te::savestate::Put(out, StackTop());
    te::savestate::Put(out, uint64_t(uintptr_t(g_instance)));
    te::savestate::Put(out, uint64_t(uintptr_t(g_env)));
    te::savestate::PutString(out, te::GetSettings().iso_path);
    te::savestate::Put(out, uint64_t(StackGuardNow()));
}

bool RestoreHead(const Blob& blob, std::string* err) {
    PayloadReader in(blob.payload);
    uint32_t version = 0;
    std::string iso;
    auto fail = [&](const std::string& why) { if (err) *err = "head: " + why; return false; };
    if (!in.Get(&version) || version != kHeadVersion) return fail("version");
    if (!in.Get(&g_saved.build_id) || !in.Get(&g_saved.boundary) || !in.Get(&g_saved.image_base) || !in.Get(&g_saved.image_size) ||
        !in.Get(&g_saved.memory_base) || !in.Get(&g_saved.table_base) || !in.Get(&g_saved.table_bytes) ||
        !in.Get(&g_saved.stack_lo) || !in.Get(&g_saved.stack_hi) || !in.Get(&g_saved.instance_addr) ||
        !in.Get(&g_saved.env_addr) || !te::savestate::GetString(in, iso) || !in.Get(&g_saved.stack_guard))
        return fail("truncated");
    char hex[64];
    if (g_saved.build_id != BuildId()) {
        std::snprintf(hex, sizeof hex, "%08x, this executable is %08x", g_saved.build_id, BuildId());
        return fail(std::string("the state was saved by another build (") + hex + ")");
    }
    const ImageInfo img = Image();
    if (g_saved.image_base != img.base) {
        std::snprintf(hex, sizeof hex, "0x%llx, now 0x%llx", (unsigned long long)g_saved.image_base, (unsigned long long)img.base);
        return fail(std::string("the executable was loaded at another address (") + hex +
                    "): run with ASLR disabled (the runner does this itself for --save-state/--load-state; TE_NO_RESPAWN set?)");
    }
    if (g_saved.instance_addr != uint64_t(uintptr_t(g_instance)) || g_saved.env_addr != uint64_t(uintptr_t(g_env)))
        return fail("the module instance moved (not a global?)");
#if defined(_WIN32)
    // /GS frames hold cookie ^ rsp: exact only on the saved stack address (the POSIX arms relocate the raw guard word)
    if (g_saved.stack_lo != uint64_t(uintptr_t(te::game::Get().stack)))
        return fail("the game stack is not at its saved address (the fixed mapping was refused); a Windows state needs it");
    __security_cookie = uintptr_t(g_saved.stack_guard);
    __security_cookie_complement = ~uintptr_t(g_saved.stack_guard);
#endif
    if (!iso.empty() && iso != te::GetSettings().iso_path)
        std::fprintf(stderr, "[twinsanityevolved] savestate: saved with disc '%s', loading with '%s'\n", iso.c_str(),
                     te::GetSettings().iso_path.c_str());
    return true;
}

// ---- RAM0 --------------------------------------------------------------------------------------------------------------
void SerializeRam(std::vector<uint8_t>& out) {
    out.insert(out.end(), g_env->memory.data, g_env->memory.data + g_env->memory.size);
}
bool RestoreRam(const Blob& blob, std::string* err) {
    if (blob.payload.size() != g_env->memory.size) { if (err) *err = "RAM0: size differs from the 32 MB memory"; return false; }
    std::memcpy(g_env->memory.data, blob.payload.data(), blob.payload.size());
    return true;
}

// ---- the module: globals (the u32 fields of w2c_game in declaration order), table size, runtime TLS ------------------------
struct Globals {
    uint32_t stack_pointer, retail_image, retail_functions, retail_function_count, heap_base, vt_targets, vt_thunks,
        vt_thunk_count, vt_signature_count, data_end;
};
Globals ReadGlobals() {
    const w2c_game& i = *g_instance;
    return Globals{i.w2c_0x5F_stack_pointer, i.w2c_te_retail_image, i.w2c_te_retail_functions, i.w2c_te_retail_function_count,
                   i.w2c_0x5F_heap_base, i.w2c_te_vt_targets, i.w2c_te_vt_thunks, i.w2c_te_vt_thunk_count,
                   i.w2c_te_vt_signature_count, i.w2c_0x5F_data_end};
}
void WriteGlobals(const Globals& g) {
    w2c_game& i = *g_instance;
    i.w2c_0x5F_stack_pointer = g.stack_pointer; i.w2c_te_retail_image = g.retail_image; i.w2c_te_retail_functions = g.retail_functions;
    i.w2c_te_retail_function_count = g.retail_function_count; i.w2c_0x5F_heap_base = g.heap_base; i.w2c_te_vt_targets = g.vt_targets;
    i.w2c_te_vt_thunks = g.vt_thunks; i.w2c_te_vt_thunk_count = g.vt_thunk_count; i.w2c_te_vt_signature_count = g.vt_signature_count;
    i.w2c_0x5F_data_end = g.data_end;
}
void SerializeModule(std::vector<uint8_t>& out) {
    te::savestate::Put(out, uint32_t(1));
    const Globals g = ReadGlobals();
    te::savestate::Put(out, uint32_t(10));
    te::savestate::Put(out, g.stack_pointer); te::savestate::Put(out, g.retail_image); te::savestate::Put(out, g.retail_functions);
    te::savestate::Put(out, g.retail_function_count); te::savestate::Put(out, g.heap_base); te::savestate::Put(out, g.vt_targets);
    te::savestate::Put(out, g.vt_thunks); te::savestate::Put(out, g.vt_thunk_count); te::savestate::Put(out, g.vt_signature_count);
    te::savestate::Put(out, g.data_end);
    te::savestate::Put(out, uint32_t(g_instance->w2c_0x5F_indirect_function_table.size));
    te::savestate::Put(out, te_run_call_depth());
    std::vector<uint8_t> tls(te_run_tls_size());
    te_run_tls_save(tls.data());
    te::savestate::Put(out, uint32_t(tls.size()));
    te::savestate::PutBytes(out, tls.data(), tls.size());
}
bool RestoreModule(const Blob& blob, std::string* err) {
    PayloadReader in(blob.payload);
    uint32_t version = 0, count = 0, table = 0, depth = 0;
    Globals g{};
    std::vector<uint8_t> tls;
    auto fail = [&](const char* why) { if (err) *err = std::string("module: ") + why; return false; };
    if (!in.Get(&version) || version != 1 || !in.Get(&count) || count != 10) return fail("version");
    if (!in.Get(&g.stack_pointer) || !in.Get(&g.retail_image) || !in.Get(&g.retail_functions) || !in.Get(&g.retail_function_count) ||
        !in.Get(&g.heap_base) || !in.Get(&g.vt_targets) || !in.Get(&g.vt_thunks) || !in.Get(&g.vt_thunk_count) ||
        !in.Get(&g.vt_signature_count) || !in.Get(&g.data_end) || !in.Get(&table) || !in.Get(&depth) ||
        !te::savestate::GetVector(in, tls, 65536))
        return fail("truncated");
    if (table != g_instance->w2c_0x5F_indirect_function_table.size) return fail("the function table size differs (another build)");
    if (tls.size() != te_run_tls_size()) return fail("the runtime TLS size differs (another build/runtime)");
    const Globals now = ReadGlobals();
    if (g.retail_image != now.retail_image || g.heap_base != now.heap_base || g.data_end != now.data_end ||
        g.vt_thunk_count != now.vt_thunk_count)
        return fail("the module's layout differs (another build)");
    WriteGlobals(g);
    te_run_tls_restore(tls.data());
    (void)depth;   // carried inside the TLS bytes too
    return true;
}

// ---- the fiber: register file + stack bytes ------------------------------------------------------------------------------
uint64_t Relocate(uint64_t v, uint64_t& memory_hits, uint64_t& table_hits, uint64_t& stack_hits) {
#if !defined(_WIN32)
    if (g_saved.stack_guard && v == g_saved.stack_guard) return uint64_t(StackGuardNow());
#endif
    const uint64_t mem_now = uint64_t(uintptr_t(g_env->memory.data));
    const uint64_t tbl_now = uint64_t(uintptr_t(g_instance->w2c_0x5F_indirect_function_table.data));
    const te::game::Fiber& f = te::game::Get();
    const uint64_t stack_now = uint64_t(uintptr_t(f.stack));
    if (g_saved.memory_base && v >= g_saved.memory_base && v < g_saved.memory_base + g_env->memory.size) {
        if (g_saved.memory_base != mem_now) { ++memory_hits; return v - g_saved.memory_base + mem_now; }
        return v;
    }
    if (g_saved.table_base && v >= g_saved.table_base && v < g_saved.table_base + g_saved.table_bytes) {
        if (g_saved.table_base != tbl_now) { ++table_hits; return v - g_saved.table_base + tbl_now; }
        return v;
    }
    if (v >= g_saved.stack_lo && v < g_saved.stack_hi) {
        if (g_saved.stack_lo != stack_now) { ++stack_hits; return v - g_saved.stack_lo + stack_now; }
        return v;
    }
    return v;
}

void SerializeStack(std::vector<uint8_t>& out) {
    const te::game::Fiber& f = te::game::Get();
    te::savestate::Put(out, uint32_t(1));
    te::savestate::PutBytes(out, &f.game, sizeof f.game);
#if defined(__aarch64__)
    const uint64_t sp = f.game.sp;
#else
    const uint64_t sp = f.game.rsp;
#endif
    const uint64_t top = StackTop();
    const uint64_t used = top - sp;
    te::savestate::Put(out, sp);
    te::savestate::Put(out, used);
    te::savestate::PutBytes(out, reinterpret_cast<const void*>(uintptr_t(sp)), size_t(used));
    // Diagnostics: words that look like host pointers outside every known region (a heap pointer live across the
    // checkpoint would be one): counted to show the stack is clean.
    const ImageInfo img = Image();
    uint64_t foreign = 0, image = 0, memory = 0, table = 0, stack = 0;
    const uint64_t mem = uint64_t(uintptr_t(g_env->memory.data)), tbl = uint64_t(uintptr_t(g_instance->w2c_0x5F_indirect_function_table.data));
    for (uint64_t a = sp; a + 8 <= top; a += 8) {
        uint64_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(uintptr_t(a)), 8);
        if (v >= img.base && v < img.base + img.size) ++image;
        else if (v >= mem && v < mem + g_env->memory.size) ++memory;
        else if (v >= tbl && v < tbl + TableBytes()) ++table;
        else if (v >= uint64_t(uintptr_t(f.stack)) && v < top) ++stack;
        else if (v == uint64_t(StackGuardNow())) continue;   // the stack protector's canary (rewritten / adopted on load)
        else if (v >= 0x100000000ull && v < 0x800000000000ull) {   // a plausible 64-bit user pointer
            ++foreign;
            if (te::GetSettings().verbose && foreign <= 16)
                std::fprintf(stderr, "[twinsanityevolved] savestate:   foreign word at top-0x%llx: 0x%llx\n",
                             (unsigned long long)(top - a), (unsigned long long)v);
        }
    }
    std::fprintf(stderr, "[twinsanityevolved] savestate: stack %llu bytes (%llu words: %llu image, %llu memory, %llu table, %llu stack, %llu foreign-looking)\n",
                 (unsigned long long)used, (unsigned long long)(used / 8), (unsigned long long)image, (unsigned long long)memory,
                 (unsigned long long)table, (unsigned long long)stack, (unsigned long long)foreign);
    if (te::GetSettings().verbose) {
        // The frame chain (fp -> [saved fp, saved lr]) with symbol names: which functions are on the game stack
        uint64_t fp = te_fiber_frame_pointer(&f.game);
        for (int depth = 0; depth < 64 && fp >= sp && fp + 16 <= top; ++depth) {
            uint64_t prev = 0, lr = 0;
            std::memcpy(&prev, reinterpret_cast<const void*>(uintptr_t(fp)), 8);
            std::memcpy(&lr, reinterpret_cast<const void*>(uintptr_t(fp + 8)), 8);
#if defined(_WIN32)
            const char* name = "?";   // no dladdr; the frame offsets still show the depth
#else
            Dl_info info{};
            const char* name = dladdr(reinterpret_cast<void*>(uintptr_t(lr)), &info) && info.dli_sname ? info.dli_sname : "?";
#endif
            std::fprintf(stderr, "[twinsanityevolved] savestate:   frame top-0x%llx: %s\n", (unsigned long long)(top - fp), name);
            if (prev <= fp) break;
            fp = prev;
        }
    }
}

bool RestoreStack(const Blob& blob, std::string* err) {
    te::game::Fiber& f = te::game::Get();
    PayloadReader in(blob.payload);
    uint32_t version = 0;
    te_fiber_ctx ctx{};
    uint64_t sp = 0, used = 0;
    auto fail = [&](const char* why) { if (err) *err = std::string("stack: ") + why; return false; };
    if (!in.Get(&version) || version != 1 || !in.GetBytes(&ctx, sizeof ctx) || !in.Get(&sp) || !in.Get(&used)) return fail("truncated");
    if (used > f.stack_size || in.Remaining() < used) return fail("stack bytes out of range");
    const uint64_t top_now = StackTop();
    const uint64_t stack_now = uint64_t(uintptr_t(f.stack));
    if (g_saved.stack_hi - g_saved.stack_lo != top_now - stack_now) return fail("stack size differs");
    // Where the bytes go: the same offset from the top in our mapping (identical when the mapping took the hint)
    const uint64_t sp_now = top_now - used;
    std::vector<uint8_t> bytes(static_cast<size_t>(used), uint8_t{0});
    if (used && !in.GetBytes(bytes.data(), size_t(used))) return fail("truncated bytes");
    uint64_t memory_hits = 0, table_hits = 0, stack_hits = 0;
    for (size_t k = 0; k + 8 <= bytes.size(); k += 8) {
        uint64_t v = 0;
        std::memcpy(&v, &bytes[k], 8);
        const uint64_t r = Relocate(v, memory_hits, table_hits, stack_hits);
        if (r != v) std::memcpy(&bytes[k], &r, 8);
    }
    uint64_t* regs = reinterpret_cast<uint64_t*>(&ctx);
    for (size_t k = 0; k < sizeof ctx / 8; ++k) regs[k] = Relocate(regs[k], memory_hits, table_hits, stack_hits);
    if (memory_hits || table_hits || stack_hits)
        std::fprintf(stderr, "[twinsanityevolved] savestate: relocated %llu memory, %llu table, %llu stack words (the regions moved between processes)\n",
                     (unsigned long long)memory_hits, (unsigned long long)table_hits, (unsigned long long)stack_hits);
    if (used) std::memcpy(reinterpret_cast<void*>(uintptr_t(sp_now)), bytes.data(), bytes.size());
    f.game = ctx;
    f.started = true;
    return true;
}
}  // namespace

ImageInfo Image() {
    ImageInfo info;
#if defined(__APPLE__)
    const struct mach_header_64* mh = reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(0));
    info.slide = _dyld_get_image_vmaddr_slide(0);
    // Every segment slides together: the image spans [lowest segment, highest segment end)
    uint64_t lo = ~0ull, hi = 0;
    const struct load_command* lc = reinterpret_cast<const struct load_command*>(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; ++i) {
        if (lc->cmd == LC_SEGMENT_64) {
            const auto* seg = reinterpret_cast<const struct segment_command_64*>(lc);
            if (seg->vmsize && std::strcmp(seg->segname, "__PAGEZERO") != 0) {
                lo = std::min<uint64_t>(lo, seg->vmaddr + uint64_t(info.slide));
                hi = std::max<uint64_t>(hi, seg->vmaddr + seg->vmsize + uint64_t(info.slide));
            }
        }
        lc = reinterpret_cast<const struct load_command*>(reinterpret_cast<const uint8_t*>(lc) + lc->cmdsize);
    }
    info.base = lo == ~0ull ? 0 : lo;
    info.size = hi > lo ? hi - lo : 0;
#elif defined(__linux__)
    info.base = uint64_t(uintptr_t(__executable_start));
    info.size = uint64_t(uintptr_t(_end)) - info.base;
    info.slide = int64_t(info.base);   // PIE: nonzero unless loaded at 0
#elif defined(_WIN32)
    // The PE image: its load address is the module handle; its extent and preferred base are the optional header's.
    const HMODULE self = GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
    info.base = uint64_t(uintptr_t(self));
    info.size = nt->OptionalHeader.SizeOfImage;
    info.slide = int64_t(info.base) - int64_t(nt->OptionalHeader.ImageBase);   // 0 when linked /DYNAMICBASE:NO and not relocated
#endif
    return info;
}

#if defined(__APPLE__)
namespace { volatile pid_t g_respawned_child = 0; }   // the game a respawning parent waits for (EnsureNoAslr)
#endif

void EnsureNoAslr(int argc, char** argv, std::string* warning) {
    if (std::getenv("TE_NO_RESPAWN")) return;
#if defined(__APPLE__)
    if (_dyld_get_image_vmaddr_slide(0) == 0) return;
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = 0;
    posix_spawnattr_getflags(&attr, &flags);
    flags |= 0x0100;   // _POSIX_SPAWN_DISABLE_ASLR
    posix_spawnattr_setflags(&attr, flags);
    setenv("TE_NO_RESPAWN", "1", 1);
    std::vector<char*> args(argv, argv + argc);
    args.push_back(nullptr);
    char path[4096];
    uint32_t n = sizeof path;
    const char* exe = _NSGetExecutablePath(path, &n) == 0 ? path : argv[0];
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe, nullptr, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) { if (warning) *warning = "posix_spawn without ASLR failed; save states will be tied to this process's addresses"; return; }
    // This process only waits now: what would stop it stops the game (the launcher's Stop is a SIGTERM to the process it
    // started, a terminal's Ctrl-C a SIGINT), so those are forwarded to the child instead of leaving it running alone
    g_respawned_child = pid;
    struct sigaction forward {};
    forward.sa_handler = [](int sig) { if (g_respawned_child > 0) kill(g_respawned_child, sig); };
    sigemptyset(&forward.sa_mask);
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT}) sigaction(sig, &forward, nullptr);
    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
    std::fflush(stderr);
    _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
#elif defined(__linux__)
    (void)argc;
    const int persona = personality(0xffffffff);
    if (persona != -1 && (persona & ADDR_NO_RANDOMIZE)) return;
    if (personality(persona | ADDR_NO_RANDOMIZE) == -1) { if (warning) *warning = "personality(ADDR_NO_RANDOMIZE) failed"; return; }
    setenv("TE_NO_RESPAWN", "1", 1);
    execv("/proc/self/exe", argv);
    if (warning) *warning = "execv after ADDR_NO_RANDOMIZE failed";
#elif defined(_WIN32)
    // No per-process switch to re-exec under: the runner is LINKED without a dynamic base (/DYNAMICBASE:NO,
    // CMakeLists.txt), so the loader places it at the preferred base unless that range is taken. Report when it was moved.
    (void)argc; (void)argv;
    const ImageInfo img = Image();
    if (img.slide != 0 && warning)
        *warning = "the image was relocated from its preferred base (the link should carry /DYNAMICBASE:NO); save states are tied to this process's addresses";
#else
    (void)argc; (void)argv;
    if (warning) *warning = "ASLR cannot be disabled on this platform; save states are tied to this process's addresses";
#endif
}

void Register(w2c_env& env, w2c_game& instance) {
    g_env = &env;
    g_instance = &instance;
    te::savestate::Register(te::savestate::kTagHead, "runner/head", true, SerializeHead, RestoreHead);
    te::savestate::Register(ps2r::savestate::TAG_RAM, "runner/RAM0", true, SerializeRam, RestoreRam);
    te::savestate::Register(te::savestate::kTagModule, "runner/module", true, SerializeModule, RestoreModule);
    te::savestate::Register(te::savestate::kTagStack, "runner/stack", true, SerializeStack, RestoreStack);
}

bool Save(const std::string& path, uint64_t boundary, std::string* err) {
    te::game::Fiber& f = te::game::Get();
    if (f.on_game) { if (err) *err = "Save must run on the host fiber"; return false; }
    f.boundary = boundary;
    std::vector<Blob> sections = te::savestate::Registry().Capture();
    ps2r::savestate::FileMeta meta;
    meta.vsync_frame = uint32_t(boundary);
    meta.file_version = ps2r::savestate::SectionRegistry::FileVersionFor(sections);
    uint64_t bytes = 0;
    for (const Blob& b : sections) bytes += b.payload.size();
    if (!ps2r::savestate::WriteStateFile(path, meta, sections, err)) return false;
    g_last_sections.clear();
    for (const Blob& b : sections) g_last_sections.push_back(ps2r::savestate::TagName(b.tag) + ":v" + std::to_string(b.version));
    std::fprintf(stderr, "[twinsanityevolved] savestate: saved %s at boundary %llu (%zu sections, %.1f MB)\n", path.c_str(),
                 (unsigned long long)boundary, sections.size(), double(bytes) / (1024.0 * 1024.0));
    return true;
}

bool WriteSidecar(const std::string& path, uint64_t boundary, const std::string& detail, std::string* err) {
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char build[48];
    std::snprintf(build, sizeof build, "twinsanityevolved %08x", BuildId());
    std::string text = "frame=" + std::to_string(boundary) + "\nwall_ms=" + std::to_string(ms) + "\nbuild_id=" + build +
                       "\ncomposition=twinsanityevolved\nsections=";
    for (size_t i = 0; i < g_last_sections.size(); ++i) text += (i ? "," : "") + g_last_sections[i];
    text += "\n" + detail;
    const std::string meta = path + ".meta", temporary = meta + ".tmp";
    std::FILE* f = std::fopen(temporary.c_str(), "wb");
    if (!f) { if (err) *err = "cannot create " + temporary; return false; }
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed || std::rename(temporary.c_str(), meta.c_str()) != 0) {
        std::remove(temporary.c_str());
        if (err) *err = "cannot write " + meta;
        return false;
    }
    return true;
}

bool Load(const std::string& path, std::string* err) {
    te::game::Fiber& f = te::game::Get();
    if (f.on_game) { if (err) *err = "Load must run on the host fiber"; return false; }
    ps2r::savestate::FileMeta meta;
    std::vector<Blob> sections;
    if (!ps2r::savestate::ReadStateFile(path, &meta, &sections, err)) return false;
    // Every service the file knows must exist before the registry restores (their sections find their owners)
    // Registration order is restore order: the stream's files before the audio (its music windows are re-read
    // through them), the graphics' textures last; the services' system section constructs the rest.
    te::savestate::PrepareOwners();
    te::Services::Get().PrepareForRestore(sections);
    std::vector<uint32_t> known = te::savestate::Registry().KnownTags();
    for (const Blob& b : sections) {
        bool owned = false;
        for (uint32_t t : known) owned |= t == b.tag;
        if (!owned) std::fprintf(stderr, "[twinsanityevolved] savestate: section %s has no owner in this build -- skipped\n",
                                 ps2r::savestate::TagName(b.tag).c_str());
    }
    if (!te::savestate::Registry().Restore(sections, err)) return false;
    g_loaded_boundary = g_saved.boundary;
    std::fprintf(stderr, "[twinsanityevolved] savestate: loaded %s (boundary %llu, %zu sections)\n", path.c_str(),
                 (unsigned long long)g_saved.boundary, sections.size());
    return true;
}

uint64_t LoadedBoundary() { return g_loaded_boundary; }

}  // namespace te::savestate_runner
