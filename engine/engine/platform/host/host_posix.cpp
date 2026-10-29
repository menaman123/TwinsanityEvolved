// platform/host/host_posix.cpp -- the POSIX arm of engine/platform/host/platform.h
// (Linux + macOS + the PlayStation Vita's newlib). Compiled on every non-Windows host. Each body is
// the direct POSIX call the engine used to make inline; the Darwin-only pieces (QoS,
// phys_footprint) and the Vita's (the kernel's descriptors for 64-bit reads, its thread
// priorities and memory report) are the one place the host keeps an #if on the OS.
#include "engine/platform/host/platform.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <filesystem>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__vita__)
#include <malloc.h>
#include <mutex>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/qos.h>
#include <pthread/qos.h>
#endif
#if defined(__APPLE__) || defined(__GLIBC__)
#include <execinfo.h>
#include <dlfcn.h>
#define PS2R_HOST_HAVE_BACKTRACE 1
#else
#define PS2R_HOST_HAVE_BACKTRACE 0
#endif

#if defined(__vita__)
#include "engine/platform/vita/fiber_vita.h"
#else
#include "engine/platform/posix/fiber_posix.h"
#endif

namespace ps2r::host {

// ---- section 1: compiler shims (GNU builtins on this arm) ------------------------------
uint32_t Ctz64(uint64_t v) { return v ? static_cast<uint32_t>(__builtin_ctzll(v)) : 64u; }
uint32_t Clz64(uint64_t v) { return v ? static_cast<uint32_t>(__builtin_clzll(v)) : 64u; }
uint32_t Popcount64(uint64_t v) { return static_cast<uint32_t>(__builtin_popcountll(v)); }

void Mul64x64(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
#if defined(__SIZEOF_INT128__)
    const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
    *lo = static_cast<uint64_t>(p);
    *hi = static_cast<uint64_t>(p >> 64);
#else
    // a 32-bit target (the Vita): four 32x32 partial products
    const uint64_t a_lo = static_cast<uint32_t>(a), a_hi = a >> 32;
    const uint64_t b_lo = static_cast<uint32_t>(b), b_hi = b >> 32;
    const uint64_t ll = a_lo * b_lo, lh = a_lo * b_hi, hl = a_hi * b_lo, hh = a_hi * b_hi;
    const uint64_t mid = (ll >> 32) + static_cast<uint32_t>(lh) + static_cast<uint32_t>(hl);
    *lo = (mid << 32) | static_cast<uint32_t>(ll);
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
#endif
}

void* FrameAddress() { return __builtin_frame_address(0); }

void* AlignedAlloc(size_t alignment, size_t bytes) {
    void* p = nullptr;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
#if defined(__vita__)
    p = memalign(alignment, bytes ? bytes : 1);   // newlib's; its free() releases it
#else
    if (posix_memalign(&p, alignment, bytes ? bytes : 1) != 0) return nullptr;
#endif
    return p;
}
void AlignedFree(void* p) { std::free(p); }

// ---- section 2: host API ----------------------------------------------------------------
#if defined(__vita__)
// newlib's off_t is 32 bits on the Vita, so its descriptors cannot address a disc image past 2 GB. FdOpen opens
// through the kernel instead (sceIoOpen / sceIoPread / sceIoLseek take a 64-bit SceOff) and hands out the slot of a
// small table with kKernelFd set; every other descriptor (stdio's, fileno's) is newlib's and goes through it as before.
namespace {
constexpr int kKernelFd = 0x40000000;
constexpr int kKernelSlots = 64;
SceUID g_kernel_fds[kKernelSlots];
std::mutex g_kernel_fds_mutex;
bool IsKernelFd(int fd) { return fd >= 0 && (fd & kKernelFd) != 0 && (fd & ~kKernelFd) < kKernelSlots; }
SceUID KernelUid(int fd) {
    std::lock_guard<std::mutex> lock(g_kernel_fds_mutex);
    return g_kernel_fds[fd & ~kKernelFd];
}
}  // namespace

int FdOpen(const char* path, uint32_t flags) {
    int o = 0;
    const bool rd = flags & kOpenRead, wr = flags & kOpenWrite;
    o |= (rd && wr) ? SCE_O_RDWR : (wr ? SCE_O_WRONLY : SCE_O_RDONLY);
    if (flags & kOpenCreate) o |= SCE_O_CREAT;
    if (flags & kOpenTruncate) o |= SCE_O_TRUNC;
    if (flags & kOpenAppend) o |= SCE_O_APPEND;
    const SceUID uid = sceIoOpen(path, o, 0666);
    if (uid < 0) { errno = ENOENT; return -1; }
    std::lock_guard<std::mutex> lock(g_kernel_fds_mutex);
    for (int i = 0; i < kKernelSlots; ++i) {
        if (g_kernel_fds[i] == 0) { g_kernel_fds[i] = uid; return kKernelFd | i; }
    }
    sceIoClose(uid);
    errno = EMFILE;
    return -1;
}
SSize FdRead(int fd, void* buf, size_t n) { return IsKernelFd(fd) ? sceIoRead(KernelUid(fd), buf, n) : ::read(fd, buf, n); }
SSize FdWrite(int fd, const void* buf, size_t n) { return IsKernelFd(fd) ? sceIoWrite(KernelUid(fd), buf, n) : ::write(fd, buf, n); }
SSize FdPread(int fd, void* buf, size_t n, uint64_t offset) {
    if (IsKernelFd(fd)) return sceIoPread(KernelUid(fd), buf, n, static_cast<SceOff>(offset));
    return ::pread(fd, buf, n, static_cast<off_t>(offset));
}
int FdSync(int fd) { return IsKernelFd(fd) ? sceIoSyncByFd(KernelUid(fd), 0) : ::fsync(fd); }
int64_t FdSize(int fd) {
    if (IsKernelFd(fd)) {
        SceIoStat st{};
        return sceIoGetstatByFd(KernelUid(fd), &st) < 0 ? -1 : static_cast<int64_t>(st.st_size);
    }
    struct stat st{};
    return ::fstat(fd, &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
}
int FdClose(int fd) {
    if (!IsKernelFd(fd)) return ::close(fd);
    std::lock_guard<std::mutex> lock(g_kernel_fds_mutex);
    const SceUID uid = g_kernel_fds[fd & ~kKernelFd];
    g_kernel_fds[fd & ~kKernelFd] = 0;
    return uid > 0 && sceIoClose(uid) >= 0 ? 0 : -1;
}
int FdOfStream(std::FILE* f) { return ::fileno(f); }
int DirectorySync(const char* directory) {
    (void)directory;   // the kernel's renames are durable; there is no directory descriptor to sync
    return 0;
}
#else
int FdOpen(const char* path, uint32_t flags) {
    int o = 0;
    const bool rd = flags & kOpenRead, wr = flags & kOpenWrite;
    o |= (rd && wr) ? O_RDWR : (wr ? O_WRONLY : O_RDONLY);
    if (flags & kOpenCreate) o |= O_CREAT;
    if (flags & kOpenTruncate) o |= O_TRUNC;
    if (flags & kOpenAppend) o |= O_APPEND;
    return ::open(path, o, 0644);
}
SSize FdRead(int fd, void* buf, size_t n) { return ::read(fd, buf, n); }
SSize FdWrite(int fd, const void* buf, size_t n) { return ::write(fd, buf, n); }
SSize FdPread(int fd, void* buf, size_t n, uint64_t offset) {
    return ::pread(fd, buf, n, static_cast<off_t>(offset));
}
int FdSync(int fd) { return ::fsync(fd); }
int64_t FdSize(int fd) {
    struct stat st{};
    return ::fstat(fd, &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
}
int FdClose(int fd) { return ::close(fd); }
int FdOfStream(std::FILE* f) { return ::fileno(f); }
int DirectorySync(const char* directory) {
    const int fd = ::open(directory, O_RDONLY);
    if (fd < 0) return -1;
    const int r = ::fsync(fd);
    ::close(fd);
    return r;
}
#endif

int StrCaseCmp(const char* a, const char* b) { return ::strcasecmp(a, b); }
int StrNCaseCmp(const char* a, const char* b, size_t n) { return ::strncasecmp(a, b, n); }

bool MakeDirectory(const char* path) {
    if (::mkdir(path, 0755) == 0) return true;
    return errno == EEXIST && IsDirectory(path);
}
bool IsDirectory(const char* path) {
    struct stat st{};
    return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
bool ListDirectory(const char* path, std::vector<DirectoryEntry>* out) {
    if (out) out->clear();
    if (!path || !*path) return false;
    DIR* dir = ::opendir(path);
    if (!dir) return false;
    std::string base(path);
    if (base.back() != '/') base.push_back('/');
    while (const struct dirent* e = ::readdir(dir)) {
        const std::string name(e->d_name);
        if (name.empty() || name == "." || name == "..") continue;
        // whether it is a directory: stat (the Vita's newlib dirent has no d_type; a symlinked folder counts as one)
        struct stat st{};
        const bool directory = ::stat((base + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        if (out) out->push_back(DirectoryEntry{name, directory});
    }
    ::closedir(dir);
    return true;
}
int64_t FileSeek64(std::FILE* f, int64_t offset, int whence) { return ::fseeko(f, offset, whence); }
int64_t FileTell64(std::FILE* f) { return ::ftello(f); }

int ProcessId() { return static_cast<int>(::getpid()); }
int ExecReplaceProcess(const char* image, char* const argv[]) {
#if defined(__vita__)
    (void)image; (void)argv;   // one application image per process on the device: no relaunch
    errno = ENOSYS;
    return -1;
#else
    if (image && *image) ::execv(image, argv);
    return ::execvp(argv[0], argv);
#endif
}

#if defined(__vita__)
namespace {
// The application's grant as the kernel reports it free at the first sample (the newlib heap block is already taken by
// then and counts as in use); the footprint is how much of that grant is gone since.
uint64_t VitaFreeBytes() {
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof info;
    if (sceKernelGetFreeMemorySize(&info) < 0) return 0;
    return static_cast<uint64_t>(info.size_user) + static_cast<uint64_t>(info.size_cdram) + static_cast<uint64_t>(info.size_phycont);
}
uint64_t g_vita_peak = 0;
}  // namespace
#endif

uint64_t ProcessFootprintBytes() {
#if defined(__vita__)
    static const uint64_t grant = static_cast<uint64_t>(365) << 20;   // the extra-memory grant (ATTRIBUTE2=12)
    const uint64_t free_now = VitaFreeBytes();
    const uint64_t used = free_now && free_now < grant ? grant - free_now : 0;
    if (used > g_vita_peak) g_vita_peak = used;
    return used;
#elif defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    return static_cast<uint64_t>(info.phys_footprint);
#else
    // Linux: VmRSS from /proc/self/statm (resident pages).
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    const int got = std::fscanf(f, "%lu %lu", &size, &resident);
    std::fclose(f);
    if (got != 2) return 0;
    return static_cast<uint64_t>(resident) * static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}
uint64_t ProcessPeakFootprintBytes() {
#if defined(__vita__)
    ProcessFootprintBytes();
    return g_vita_peak;
#elif defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    return static_cast<uint64_t>(info.ledger_phys_footprint_peak);
#else
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    uint64_t kb = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, "VmHWM:", 6) == 0) { kb = std::strtoull(line + 6, nullptr, 10); break; }
    }
    std::fclose(f);
    return kb * 1024u;
#endif
}
std::string ExecutableImagePath(std::string* err) {
#if defined(__vita__)
    (void)err;
    return "app0:eboot.bin";
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size + 1, '\0');
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0) {
        if (err) *err = "_NSGetExecutablePath failed";
        return std::string();
    }
    buffer.resize(std::strlen(buffer.c_str()));
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(buffer, ec);
    return ec ? buffer : canonical.string();
#else
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        if (err) *err = "/proc/self/exe: " + ec.message();
        return std::string();
    }
    return target.string();
#endif
}

namespace {
using StartFn = void (*)(void*);
struct ThreadStart { StartFn fn; void* arg; };
void* ThreadTrampoline(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    s.fn(s.arg);
    return nullptr;
}
}  // namespace

bool ThreadCreate(NativeThread* out, void (*fn)(void*), void* arg, size_t stack_bytes) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_bytes) pthread_attr_setstacksize(&attr, stack_bytes);
    pthread_t tid{};
    auto* s = new ThreadStart{fn, arg};
    const int rc = pthread_create(&tid, &attr, &ThreadTrampoline, s);
    pthread_attr_destroy(&attr);
    if (rc != 0) { delete s; out->handle = nullptr; return false; }
    out->handle = reinterpret_cast<void*>(tid);
    return true;
}
void ThreadJoin(NativeThread t) {
    if (t.handle) pthread_join(reinterpret_cast<pthread_t>(t.handle), nullptr);
}
void ThreadDetach(NativeThread t) {
    if (t.handle) pthread_detach(reinterpret_cast<pthread_t>(t.handle));
}
void SetCurrentThreadName(const char* name) {
    if (!name || !*name) return;
#if defined(__vita__)
    (void)name;   // the kernel names a thread at creation only
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    char buf[16];
    std::strncpy(buf, name, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    pthread_setname_np(pthread_self(), buf);
#endif
}
void PinCurrentThreadInteractive(ThreadRole role) {
#if defined(__vita__)
    // the kernel's priorities (a lower number runs first): the pacer above the game above the renderer
    int priority = 160;
    switch (role) {
        case ThreadRole::Pacer: priority = 72; break;
        case ThreadRole::Game: priority = 96; break;
        case ThreadRole::Graphics: priority = 128; break;
        case ThreadRole::Worker: priority = 144; break;
        default: priority = 160; break;
    }
    sceKernelChangeThreadPriority(0, priority);
#elif defined(__APPLE__)
    (void)role;   // every role is the interactive class
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
    (void)role;   // the roles rank threads on Win32 and the Vita; here every role is the interactive class
    // Best effort: a lower nice value needs CAP_SYS_NICE; failure is not an error
    // (glibc marks nice() warn_unused_result, which a void cast does not satisfy
    // under GCC -Werror, so the result is tested).
    if (::nice(-5) == -1) {
        // No capability: the thread keeps the default priority.
    }
#endif
}

uint64_t MonotonicNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t ThreadCpuTimeNs() {
#if defined(__vita__)
    SceKernelThreadInfo info{};
    info.size = sizeof info;
    if (sceKernelGetThreadInfo(0, &info) < 0) return 0;
    return static_cast<uint64_t>(info.runClocks) * 1000ull;   // microseconds
#else
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

void RaiseTimerResolution() {
    // Nothing to raise: the POSIX kernels sleep to the microsecond (hrtimers / mach).
}

void SleepForNs(uint64_t ns) {
    timespec req{};
    req.tv_sec = static_cast<time_t>(ns / 1000000000ull);
    req.tv_nsec = static_cast<long>(ns % 1000000000ull);
    // nanosleep returns the remainder on EINTR: resume it so the span is never cut short.
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
    }
}

bool CpuDetect(bool* sse4_2, bool* avx2, bool* fma3, bool* neon, const char** arch) {
    *sse4_2 = *avx2 = *fma3 = *neon = false;
#if defined(__aarch64__) || defined(__arm64__)
    *neon = true;
    *arch = "arm64";
    return true;
#elif defined(__ARM_NEON)
    *neon = true;   // armv7-a with NEON (the Vita's Cortex-A9)
    *arch = "armv7";
    return true;
#elif defined(__x86_64__)
    *arch = "x86_64";
    unsigned eax, ebx, ecx, edx;
    // Leaf 1: SSE4.2 (ECX bit 20), FMA (ECX bit 12), OSXSAVE (ECX bit 27), AVX (ECX bit 28).
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    *sse4_2 = (ecx & (1u << 20)) != 0;
    const bool fma = (ecx & (1u << 12)) != 0;
    const bool osxsave = (ecx & (1u << 27)) != 0;
    const bool avx = (ecx & (1u << 28)) != 0;
    bool ymm_enabled = false;
    if (osxsave) {
        unsigned xcr0_lo, xcr0_hi;
        __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
        ymm_enabled = (xcr0_lo & 0x6u) == 0x6u;   // XMM + YMM state saved by the OS
    }
    // Leaf 7 sub-leaf 0: AVX2 (EBX bit 5).
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    const bool avx2_bit = (ebx & (1u << 5)) != 0;
    *avx2 = avx && avx2_bit && ymm_enabled;
    *fma3 = fma && *avx2;
    return true;
#else
    *arch = "other";
    return false;
#endif
}

bool GmTimeUtc(int64_t unix_seconds, struct tm* out) {
    const time_t t = static_cast<time_t>(unix_seconds);
    return ::gmtime_r(&t, out) != nullptr;
}

int Backtrace(void** frames, int max_frames) {
#if PS2R_HOST_HAVE_BACKTRACE
    return ::backtrace(frames, max_frames);
#else
    (void)frames; (void)max_frames;
    return 0;
#endif
}
void BacktraceSymbolsFd(void* const* frames, int count, int fd) {
#if PS2R_HOST_HAVE_BACKTRACE
    ::backtrace_symbols_fd(const_cast<void**>(frames), count, fd);
#else
    char line[32];
    for (int i = 0; i < count; ++i) {
        const int n = std::snprintf(line, sizeof line, "%p\n", frames[i]);
        if (n > 0) (void)!::write(fd, line, static_cast<size_t>(n));
    }
#endif
}
std::string SymbolForAddress(const void* pc) {
#if PS2R_HOST_HAVE_BACKTRACE
    Dl_info info{};
    if (!dladdr(pc, &info) || !info.dli_sname) return std::string();
    return std::string(info.dli_sname);
#else
    (void)pc;
    return std::string();
#endif
}

#if defined(__vita__)
using HostFiber = platform::FiberVita;
#else
using HostFiber = platform::FiberPosix;
#endif
bool CoroutineCreate(Coroutine* out, void (*fn)(void*), void* arg, size_t stack_bytes) {
    const platform::Fiber f = HostFiber::Create(fn, arg, stack_bytes);
    out->handle = f.handle;
    return f.handle != nullptr;
}
Coroutine CoroutineCurrent() { return Coroutine{HostFiber::Current().handle}; }
void CoroutineSwitch(Coroutine to) { HostFiber::Switch(platform::Fiber{to.handle}); }
void CoroutineDestroy(Coroutine c) { HostFiber::Destroy(platform::Fiber{c.handle}); }

const char* HostArmName() { return "posix"; }

}  // namespace ps2r::host
