// render/backends/gl/gl_gpu_lifetime.cpp -- the GPU-lifetime audit (gl_gpu_lifetime.h).
#include "engine/render/backends/gl/gl_gpu_lifetime.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#if PS2R_HOST_VITA
// vitaGL r1488's records, read only: its texture records (shared.h struct texture, 56 bytes, last_frame at 0; the GL
// name is the index), its frame count (vglSwapBuffers' count; a draw stamps last_frame with it) and the scene fence it
// passes to every sceGxmEndScene (buffers.c query_fence: value = the scenes ended, *address = the scenes the GPU
// finished). A buffer's GL name is its record's address (struct vbo: ptr, size, alloc_func, last_frame at 12).
extern "C" {
extern uint8_t texture_slots[];
extern uint32_t vgl_framecount;
extern SceGxmNotification query_fence __attribute__((weak));
extern SceGxmShaderPatcher* gxm_shader_patcher __attribute__((weak));
}
#endif

namespace ps2r {
namespace render {
namespace gpu_lifetime {

bool g_on = false;

namespace {

// the event categories and the three answers an event gets
enum Cat { kTexFree, kTexRespec, kTexWrite, kBufFree, kBufRespec, kBufWrite, kRing, kUniforms, kCollector, kProgFree, kProgSet, kCats };
enum Class { kIdle, kProtected, kUnprotected, kClasses };
const char* const kCatNames[kCats] = {"texture freed", "texture re-specified", "texture written by the CPU",
                                      "buffer freed", "buffer re-specified", "buffer written by the CPU",
                                      "vertex ring slice rewritten", "uniform ring overwritten (1 KB blocks)", "collector frees",
                                      "shader program code freed", "shader program set"};
struct Counts {
    uint64_t n[kCats][kClasses] = {};
    uint64_t unprotected_bytes[kCats] = {};
    uint64_t uniform_bytes = 0;        // the uniform ring's reservations (the Vita)
    uint64_t uniform_frame_peak = 0;   // ... the most one frame reserved
    uint64_t uniform_wraps = 0;
    uint32_t lag_max = 0;              // elsewhere: the presents the GPU had not finished at an event, the most
    uint64_t frames = 0;               // the swaps
    uint64_t programs_created = 0, programs_refused = 0;   // the shader patcher's new programs / refusals (the Vita)
};
Counts g_window, g_total;
#if PS2R_HOST_VITA
uint64_t g_logged = 0;                 // UNPROTECTED lines written
uint64_t g_logged_protected[kCats] = {};
#endif

Cat CatOf(bool texture, Event e) {
    const int base = texture ? kTexFree : kBufFree;
    return Cat(base + (e == Event::Free ? 0 : e == Event::Respecify ? 1 : 2));
}

void Count(Cat c, Class k, uint64_t bytes) {
    g_window.n[c][k]++;
    g_total.n[c][k]++;
    if (k == kUnprotected) {
        g_window.unprotected_bytes[c] += bytes;
        g_total.unprotected_bytes[c] += bytes;
    }
}

#if PS2R_HOST_VITA
constexpr uint32_t kNotUsed = 0xFFFFFFFFu;     // vitaGL's OBJ_NOT_USED
constexpr uint32_t kPurgeFrames = 4;           // vitaGL's FRAME_PURGE_FREQ
constexpr size_t kTextureRecordBytes = 56;     // sizeof(texture) in r1488
constexpr uint32_t kTextureRecords = 16384;
// the scenes ended up to each frame's swap (the frame presented: vgl_framecount - 1 once the swap returned)
constexpr uint32_t kMarks = 64;
struct Mark { uint32_t frame = 0xFFFFFFFFu; uint32_t scenes = 0; };
Mark g_marks[kMarks];

uint32_t ScenesDone() { return *static_cast<volatile const uint32_t*>(query_fence.address); }

// whether the GPU may still read storage vitaGL's record says was last used in frame `last`
enum class Gpu { Idle, InFlight };
Gpu FrameState(uint32_t last) {
    if (last == kNotUsed) return Gpu::Idle;                   // not drawn since it was (re)made
    const uint32_t now = vgl_framecount;
    if (int32_t(last - now) >= 0) return Gpu::InFlight;      // drawn in the frame being built: its scenes may not be ended
    if (now - last > kMarks / 2) return Gpu::Idle;            // (long finished: the display queue holds 2 frames)
    if (!&query_fence || !query_fence.address) return Gpu::InFlight;
    const Mark& m = g_marks[last % kMarks];
    if (m.frame != last) return Gpu::InFlight;               // (no mark: assumed the worst)
    return int32_t(ScenesDone() - m.scenes) >= 0 ? Gpu::Idle : Gpu::InFlight;
}

uint32_t TextureLastFrame(uint32_t tex) {
    if (!tex || tex >= kTextureRecords) return kNotUsed;
    uint32_t last;
    std::memcpy(&last, texture_slots + size_t(tex) * kTextureRecordBytes, 4);
    return last;
}
uint32_t BufferLastFrame(uint32_t buf) {
    if (!buf) return kNotUsed;
    uint32_t last;
    std::memcpy(&last, reinterpret_cast<const uint8_t*>(uintptr_t(buf)) + 12, 4);
    return last;
}

// the answer for storage last used in `last`: idle, or in flight and then deferred by vitaGL (its collector / its
// copy-on-write: the record's use at most kPurgeFrames old) unless the path frees or writes it itself
Class Judge(uint32_t last, bool vitagl_protects) {
    if (FrameState(last) == Gpu::Idle) return kIdle;
    const bool deferred = last != kNotUsed && vgl_framecount - last <= kPurgeFrames;
    return deferred && vitagl_protects ? kProtected : kUnprotected;
}

void Report(Cat c, Class k, const char* what_kind, uint32_t name, const char* size, uint32_t last, const char* what) {
    if (k == kIdle) return;
    const uint32_t done = &query_fence && query_fence.address ? ScenesDone() : 0;
    const uint32_t issued = &query_fence ? query_fence.value : 0;
    if (k == kUnprotected) {
        const uint64_t n = ++g_logged;
        if (n > 32 && n % 256 != 0) return;
        std::fprintf(stderr, "[te.gfx.vita] GPU LIFETIME: %s %u (%s) %s at frame %u (%s) while the GPU may still read it: last used in "
                     "frame %u, scenes finished %u of %u -- vitaGL does not defer it: UNPROTECTED (%llu so far)\n", what_kind, name,
                     size, kCatNames[c], vgl_framecount, what, last, done, issued, (unsigned long long)n);
    } else if (g_logged_protected[c]++ < 4) {
        std::fprintf(stderr, "[te.gfx.vita] GPU lifetime: %s %u (%s) %s at frame %u (%s) while the GPU may still read it: last used in "
                     "frame %u, scenes finished %u of %u -- deferred by vitaGL (%s)\n", what_kind, name, size, kCatNames[c],
                     vgl_framecount, what, last, done, issued,
                     c == kTexWrite || c == kBufWrite ? "its copy-on-write" : "its garbage collector");
    }
}

// the uniform ring (utils/gxm_utils.c: UNIFORM_CIRCULAR_POOL_SIZE 2 MB, wrapped to its start when full): the frame that
// last wrote each block, and the pass it was in
constexpr uint32_t kUniformRing = 2u * 1024u * 1024u;
constexpr uint32_t kUniformBlock = 1024u;
constexpr uint32_t kUniformBlocks = kUniformRing / kUniformBlock;
const uint8_t* g_uni_base = nullptr;
const uint8_t* g_uni_last = nullptr;
uint32_t g_uni_pass = 0;
uint32_t g_block_frame[kUniformBlocks];
uint32_t g_block_pass[kUniformBlocks];
uint32_t g_uni_frame = 0xFFFFFFFFu;
uint64_t g_uni_frame_bytes = 0;

// the shader programs the patcher handed out: their references (the patcher returns the same program for the same
// patch, counted), the frame of their last set, their registered GXP
struct Program { uint32_t refs = 0; uint32_t last = kNotUsed; const void* id = nullptr; bool fragment = false; };
std::unordered_map<const void*, Program>& Programs() {
    static std::unordered_map<const void*, Program> programs;
    return programs;
}
void ProgramGone(const void* program, const Program& pr, const char* how) {
    const Class k = FrameState(pr.last) == Gpu::Idle ? kIdle : kUnprotected;
    Count(kProgFree, k, 0);
    if (k == kUnprotected) {
        const uint64_t n = ++g_logged;
        if (n <= 32 || n % 256 == 0)
            std::fprintf(stderr, "[te.gfx.vita] GPU LIFETIME: %s program %p's code freed (%s) at frame %u while the GPU may still run it: "
                         "last set in frame %u -- the patcher hands the space to the next program: UNPROTECTED (%llu so far)\n",
                         pr.fragment ? "fragment" : "vertex", program, how, vgl_framecount, pr.last, (unsigned long long)n);
    }
}
#else
// elsewhere: a fence per present; the presents the GPU has not finished = issued - done
constexpr uint32_t kFences = 64;
GLsync g_fences[kFences] = {};
uint64_t g_issued = 0, g_done = 0;
uint32_t Lag() {
    while (g_done < g_issued) {
        GLsync& f = g_fences[g_done % kFences];
        if (f) {
            const GLenum r = glClientWaitSync(f, 0, 0);
            if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) break;
            glDeleteSync(f);
            f = nullptr;
        }
        ++g_done;
    }
    return uint32_t(g_issued - g_done);
}
void NoteDesktop(Cat c) {
    // the GL driver keeps whatever the GPU may still read: every event is protected, its lag recorded
    const uint32_t lag = Lag();
    Count(c, lag ? kProtected : kIdle, 0);
    if (lag > g_window.lag_max) g_window.lag_max = lag;
    if (lag > g_total.lag_max) g_total.lag_max = lag;
}
#endif

void Line(char* out, size_t cap, const Counts& w, Cat c) {
    const uint64_t* n = w.n[c];
    std::snprintf(out, cap, "%s %llu (in flight %llu: deferred %llu, UNPROTECTED %llu)", kCatNames[c],
                  (unsigned long long)(n[kIdle] + n[kProtected] + n[kUnprotected]), (unsigned long long)(n[kProtected] + n[kUnprotected]),
                  (unsigned long long)n[kProtected], (unsigned long long)n[kUnprotected]);
}

}  // namespace

void Init() {
    static bool read = false;
    if (read) return;
    read = true;
    const char* e = std::getenv("TE_GFX_GPU_LIFETIME");
    g_on = e && e[0] == '1';
    if (!g_on) return;
#if PS2R_HOST_VITA
    std::fprintf(stderr, "[te.gfx.vita] GPU lifetime audit on (TE_GFX_GPU_LIFETIME=1): every free, re-specification and CPU write of "
                 "GPU storage checked against vitaGL's record of its last use and the scene fence (%s)\n",
                 &query_fence ? "query_fence" : "NO scene fence in this vitaGL: every storage drawn counts as in flight");
#else
    std::fprintf(stderr, "[te.gfx] GPU lifetime audit on (TE_GFX_GPU_LIFETIME=1): every free, re-specification and CPU write of GPU "
                 "storage counted with the presents the GPU had not finished (the GL driver keeps what the GPU still reads)\n");
#endif
}

void OnSwap() {
    if (!g_on) return;
    g_window.frames++;
    g_total.frames++;
#if PS2R_HOST_VITA
    // the frame just presented and the scenes ended up to its swap (its last scene included: vglSwapBuffers ended it)
    const uint32_t f = vgl_framecount - 1;
    g_marks[f % kMarks] = Mark{f, &query_fence ? query_fence.value : 0};
    // vitaGL's collector frees, at the end of this swap, the list of frame f - 3 (gxm.c garbage_collector: one list a
    // swap, FRAME_PURGE_FREQ lists): storage last used in that frame or before -- in flight if the GPU has not finished it
    if (f >= 3) {
        const Class k = FrameState(f - 3) == Gpu::Idle ? kIdle : kUnprotected;
        Count(kCollector, k, 0);
        if (k == kUnprotected) {
            const uint64_t n = ++g_logged;
            if (n <= 32 || n % 256 == 0)
                std::fprintf(stderr, "[te.gfx.vita] GPU LIFETIME: vitaGL's collector frees frame %u's list at the swap of frame %u while "
                             "the GPU has not finished frame %u: UNPROTECTED (%llu so far)\n", f - 3, f, f - 3, (unsigned long long)n);
        }
    }
#else
    Lag();
    if (g_issued - g_done >= kFences) {
        // (64 presents behind: the oldest waited for, so its slot is free)
        GLsync& oldest = g_fences[g_done % kFences];
        if (oldest) {
            glClientWaitSync(oldest, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glDeleteSync(oldest);
            oldest = nullptr;
        }
        ++g_done;
    }
    g_fences[g_issued % kFences] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    ++g_issued;
#endif
}

void Texture(uint32_t tex, uint32_t w, uint32_t h, Event e, const char* what, bool copy_on_write, bool at_once) {
    if (!g_on || !tex) return;
    const Cat c = CatOf(true, e);
#if PS2R_HOST_VITA
    const uint32_t last = TextureLastFrame(tex);
    const bool vitagl_protects = !at_once && (e != Event::CpuWrite || copy_on_write);
    const Class k = Judge(last, vitagl_protects);
    Count(c, k, uint64_t(w) * h * 4);
    char size[48];
    std::snprintf(size, sizeof size, "%ux%u, %u KB", w, h, w * h * 4 / 1024);
    Report(c, k, "texture", tex, size, last, what);
#else
    (void)w; (void)h; (void)what; (void)copy_on_write; (void)at_once;
    NoteDesktop(c);
#endif
}

void Buffer(uint32_t buf, uint64_t bytes, Event e, const char* what, bool copy_on_write) {
    if (!g_on || !buf) return;
    const Cat c = CatOf(false, e);
#if PS2R_HOST_VITA
    const uint32_t last = BufferLastFrame(buf);
    if (!bytes) {
        int32_t size;   // (vitaGL's record: the storage's size at 4)
        std::memcpy(&size, reinterpret_cast<const uint8_t*>(uintptr_t(buf)) + 4, sizeof size);
        bytes = size > 0 ? uint64_t(size) : 0;
    }
    const Class k = Judge(last, e != Event::CpuWrite || copy_on_write);
    Count(c, k, bytes);
    char size[32];
    std::snprintf(size, sizeof size, "%llu KB", (unsigned long long)(bytes / 1024));
    Report(c, k, "buffer", buf, size, last, what);
#else
    (void)bytes; (void)what; (void)copy_on_write;
    NoteDesktop(c);
#endif
}

void RingSlice(uint32_t frame, uint32_t prev, int slice, uint64_t slice_bytes) {
    if (!g_on) return;
#if PS2R_HOST_VITA
    // the slice is written in place (no copy, no collector): in flight = UNPROTECTED
    const Class k = FrameState(prev) == Gpu::Idle ? kIdle : kUnprotected;
    Count(kRing, k, slice_bytes);
    if (k == kUnprotected) {
        const uint64_t n = ++g_logged;
        if (n <= 32 || n % 256 == 0)
            std::fprintf(stderr, "[te.gfx.vita] GPU LIFETIME: vertex ring slice %d rewritten at frame %u while the GPU may still read "
                         "frame %u's vertices in it: UNPROTECTED (%llu so far)\n", slice, frame, prev, (unsigned long long)n);
    }
#else
    (void)frame; (void)prev; (void)slice; (void)slice_bytes;
    NoteDesktop(kRing);
#endif
}

void UniformsReserved(const void* p, uint32_t size) {
    if (!g_on || !p || !size) return;
#if PS2R_HOST_VITA
    const uint8_t* const q = static_cast<const uint8_t*>(p);
    if (!g_uni_base) {
        // the first reservation the audit sees is the ring's start (vitaGL's index starts at 0 and the audit is on from the
        // first draw); its blocks never written
        g_uni_base = q;
        for (uint32_t b = 0; b < kUniformBlocks; ++b) g_block_frame[b] = kNotUsed;
    }
    if (g_uni_last && q < g_uni_last) {
        ++g_uni_pass;   // vitaGL wrapped to the ring's start
        g_window.uniform_wraps++;
        g_total.uniform_wraps++;
    }
    g_uni_last = q;
    const uint32_t now = vgl_framecount;
    if (now != g_uni_frame) {
        g_uni_frame = now;
        g_uni_frame_bytes = 0;
    }
    g_uni_frame_bytes += size;
    g_window.uniform_bytes += size;
    g_total.uniform_bytes += size;
    if (g_uni_frame_bytes > g_window.uniform_frame_peak) g_window.uniform_frame_peak = g_uni_frame_bytes;
    if (g_uni_frame_bytes > g_total.uniform_frame_peak) g_total.uniform_frame_peak = g_uni_frame_bytes;
    if (q < g_uni_base || q + size > g_uni_base + kUniformRing) return;   // (not the ring: nothing to judge)
    const uint32_t b0 = uint32_t(q - g_uni_base) / kUniformBlock, b1 = uint32_t(q + size - 1 - g_uni_base) / kUniformBlock;
    for (uint32_t b = b0; b <= b1; ++b) {
        const uint32_t was = g_block_frame[b];
        // the ring is written forward: a block written in THIS pass holds this reservation's predecessors (no overwrite);
        // a block of an earlier pass held uniforms of frame `was` -- overwritten now, unread if the GPU has not finished
        // that frame (or if it is this frame: a frame larger than the ring)
        if (was != kNotUsed && g_block_pass[b] != g_uni_pass) {
            const bool own = was == now;
            const Class k = !own && FrameState(was) == Gpu::Idle ? kIdle : kUnprotected;
            Count(kUniforms, k, kUniformBlock);
            if (k == kUnprotected) {
                const uint64_t n = ++g_logged;
                if (n <= 32 || n % 256 == 0)
                    std::fprintf(stderr, "[te.gfx.vita] GPU LIFETIME: uniform ring at +%u (1 KB) overwritten at frame %u while the GPU may "
                                 "still read frame %u's uniforms there%s (the ring's pass %u; this frame %llu KB so far): UNPROTECTED "
                                 "(%llu so far)\n", b * kUniformBlock, now, was, own ? " -- this frame's own, one pass before" : "",
                                 g_uni_pass, (unsigned long long)(g_uni_frame_bytes / 1024), (unsigned long long)n);
            }
        }
        g_block_frame[b] = now;
        g_block_pass[b] = g_uni_pass;
    }
#else
    (void)p; (void)size;
#endif
}

void ProgramCreated(const void* program, const void* id, bool fragment, int r) {
    if (!g_on) return;
#if PS2R_HOST_VITA
    if (r < 0 || !program) {
        g_window.programs_refused++;
        g_total.programs_refused++;
        const uint64_t n = g_total.programs_refused;
        if (n <= 16 || n % 64 == 0)
            std::fprintf(stderr, "[te.gfx.vita] GPU lifetime: the shader patcher REFUSED a %s program (0x%08x) at frame %u -- its memory: "
                         "fragment code %u KB, vertex code %u KB, buffer %u KB allocated (%llu refused so far)\n",
                         fragment ? "fragment" : "vertex", unsigned(r), vgl_framecount,
                         &gxm_shader_patcher && gxm_shader_patcher ? sceGxmShaderPatcherGetFragmentUsseMemAllocated(gxm_shader_patcher) / 1024 : 0,
                         &gxm_shader_patcher && gxm_shader_patcher ? sceGxmShaderPatcherGetVertexUsseMemAllocated(gxm_shader_patcher) / 1024 : 0,
                         &gxm_shader_patcher && gxm_shader_patcher ? sceGxmShaderPatcherGetBufferMemAllocated(gxm_shader_patcher) / 1024 : 0,
                         (unsigned long long)n);
        return;
    }
    Program& pr = Programs()[program];
    if (!pr.refs) {
        g_window.programs_created++;
        g_total.programs_created++;
    }
    pr.refs++;
    pr.id = id;
    pr.fragment = fragment;
#else
    (void)program; (void)id; (void)fragment; (void)r;
#endif
}

void ProgramUsed(const void* program, bool fragment) {
    if (!g_on || !program) return;
#if PS2R_HOST_VITA
    auto& programs = Programs();
    auto it = programs.find(program);
    if (it == programs.end()) {
        // a program the patcher did not hand out since the audit began (made before it, or freed since)
        Program pr;
        pr.refs = 1;
        pr.fragment = fragment;
        it = programs.emplace(program, pr).first;
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr, "[te.gfx.vita] GPU lifetime: a %s program %p set that the patcher did not make since the audit began "
                         "(made before it, or its code already freed) -- tracked from here\n", fragment ? "fragment" : "vertex", program);
        }
    }
    it->second.last = vgl_framecount;
#else
    (void)program; (void)fragment;
#endif
}

void ProgramReleased(const void* program, bool fragment) {
    if (!g_on || !program) return;
#if PS2R_HOST_VITA
    auto& programs = Programs();
    auto it = programs.find(program);
    if (it == programs.end()) return;
    (void)fragment;
    if (it->second.refs > 1) {
        it->second.refs--;
        return;
    }
    ProgramGone(program, it->second, "its last reference released");
    programs.erase(it);
#else
    (void)program; (void)fragment;
#endif
}

void ProgramIdUnregistered(const void* id, bool force) {
    if (!g_on || !id) return;
#if PS2R_HOST_VITA
    if (!force) return;   // (a plain unregistration leaves the programs made from it alone)
    auto& programs = Programs();
    for (auto it = programs.begin(); it != programs.end();) {
        if (it->second.id == id) {
            ProgramGone(it->first, it->second, "its GXP force-unregistered");
            it = programs.erase(it);
        } else {
            ++it;
        }
    }
#else
    (void)id; (void)force;
#endif
}

std::string WindowLine() {
    if (!g_on) return std::string();
    char parts[kCats][160];
    for (int c = 0; c < kCats; ++c) Line(parts[c], sizeof parts[c], g_window, Cat(c));
    char out[2048];
#if PS2R_HOST_VITA
    uint64_t unprotected = 0, unprotected_total = 0;
    for (int c = 0; c < kCats; ++c) {
        unprotected += g_window.n[c][kUnprotected];
        unprotected_total += g_total.n[c][kUnprotected];
    }
    const bool sp = &gxm_shader_patcher && gxm_shader_patcher;
    std::snprintf(out, sizeof out, "[te.gfx.vita] GPU lifetime (TE_GFX_GPU_LIFETIME=1) in the window: %s; %s; %s; %s; %s; %s; %s; "
                  "uniform ring %.1f KB a frame (the most %.1f KB of its 2048), %llu wraps, %s; %s; shader programs made %llu, refused %llu "
                  "(%u alive; the patcher's code heaps: fragment %u KB, vertex %u KB of 1024 each), %s -- UNPROTECTED %llu (%llu so far)",
                  parts[kTexFree], parts[kTexRespec], parts[kTexWrite], parts[kBufFree], parts[kBufRespec], parts[kBufWrite], parts[kRing],
                  g_window.frames ? double(g_window.uniform_bytes) / 1024.0 / double(g_window.frames) : 0.0,
                  double(g_window.uniform_frame_peak) / 1024.0, (unsigned long long)g_window.uniform_wraps, parts[kUniforms],
                  parts[kCollector], (unsigned long long)g_window.programs_created, (unsigned long long)g_window.programs_refused,
                  unsigned(Programs().size()), sp ? sceGxmShaderPatcherGetFragmentUsseMemAllocated(gxm_shader_patcher) / 1024 : 0,
                  sp ? sceGxmShaderPatcherGetVertexUsseMemAllocated(gxm_shader_patcher) / 1024 : 0, parts[kProgFree],
                  (unsigned long long)unprotected, (unsigned long long)unprotected_total);
#else
    std::snprintf(out, sizeof out, "[te.gfx] GPU lifetime (TE_GFX_GPU_LIFETIME=1) in the window: %s; %s; %s; %s; %s; %s; %s -- the GPU "
                  "at most %u presents behind (the GL driver keeps what it still reads)", parts[kTexFree], parts[kTexRespec],
                  parts[kTexWrite], parts[kBufFree], parts[kBufRespec], parts[kBufWrite], parts[kRing], g_window.lag_max);
#endif
    g_window = Counts{};
    return out;
}

}  // namespace gpu_lifetime
}  // namespace render
}  // namespace ps2r
