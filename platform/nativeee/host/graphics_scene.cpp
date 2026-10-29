// platform/nativeee (host side) -- the SCENE of Platform::Graphics over NativeEE's typed draws: the chunk culling (the
// engine's CullingMath / CullingPlanes / ClippingMath, the VU0 microprograms of the second set as values), the rigid and
// placed models and the scenery meshes (their submodel packets decoded ONCE from the VIF UNPACKs the wasm side hands over
// when a model is read and PREPARED once for the engine's MeshGeometry, then every instance only transformed,
// clipped and projected by its BuildPrepared -- the VU1 strip programs' transform, six-plane clipping and projection as
// value arithmetic, bit-identical to Build's), the lit models' vertex colours (vs_lit_tristrip's formula, into a scratch
// per instance), the chunk's sky (drawn around the camera, the depth left at the far plane) and the shadow volumes (dark
// translucent stamps in the shadows' bucket; the half-size alpha buffer is not drawn). A submodel's strips under one unblended
// state go to the frame as ONE triangle-list draw (the order kept; the GS rasterised them in that order too), blended
// ones one draw per strip as the engine built them (the backend's blend reads a destination copy per draw).
//
// Everything arrives as u32 guest addresses: the models in te_gfx_layout.h's layouts (the wasm side's
// graphics_models.cpp writes them), the game's RenderView / ChunkView / RenderTargetDescription as the decomp lays them out.
// The draws go through graphics.cpp's buckets (graphics_internal.h).
#include <algorithm>
#include <memory>
#include <array>
#include <atomic>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/math/clipping_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_planes.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_host.h"
#include "te_load_census.h"
#include "te_mesh_builder.h"
#include "te_prof.h"
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#endif
// The engine's skin path (SkinGeometry).
#include "games/slus_20909_crash_twinsanity/hle/graphics/skin_geometry.h"

namespace {

using te::gfx::Draw;
using te::gfx::MaterialView;
using Vector = title::CullingMath::Vector;
using Matrix = title::CullingMath::Matrix;
using Geometry = title::MeshGeometry;

// ---- the game's structs in guest memory ------------------------------------------------------------------------------------
struct RenderView {   // include/game/view.h, 0x170 bytes
    float projection[4][4], toCamera[4][4], toClip[4][4], toScreen[4][4], cameraToScreen[4][4];
    float clip[4], forward[4];
    uint32_t cameraObject;
    float inverseAspect;
    uint8_t unused168[8];
};
struct ChunkView {   // include/game/scenery.h ChunkView = renderer.h PlacedObject (0x108 bytes)
    float toClip[4][4];    // 0x00
    float clipped[4][4];   // 0x40 the last culling test's matrix to the clip space (toCamera)
    float toScreen[4][4];  // 0x80
    float clip[4];         // 0xC0
    float origin[4];       // 0xD0
    float camera[4];       // 0xE0
    int32_t distance;      // 0xF0
    uint32_t view;         // 0xF4 RenderView*
    uint32_t visibility;   // 0xF8 the draw mode: 1 wholly in view, 2 partly
    uint32_t lastVisibility;
    uint32_t matrices;     // 0x100
    uint32_t vtable;
};
struct ModelLights {   // Platform::Graphics::ModelLights
    uint32_t directions, colours, ambient;
};
// Retail globals (PAL SLES-52568, game/symbol_addrs.txt): the scene's view, the frustum planes, the shadows' matrices
constexpr uint32_t kRenderViewPointer = 0x00309BE0;    // G_RendRel
constexpr uint32_t kViewPlanes = 0x003B4900;           // D_003B4900 g_ViewPlanes[6]
constexpr uint32_t kFarViewPlanes = 0x003B4960;        // D_003B4960 g_FarViewPlanes[6]
constexpr uint32_t kShadowToCamera = 0x003B0770;       // D_003B0770 g_ShadowToCamera
constexpr uint32_t kShadowToScreen = 0x003B07B0;       // D_003B07B0 g_ShadowToScreen
constexpr uint32_t DrawUnclipped = 1;
constexpr uint32_t BucketSky = 0, BucketOpaque = 2, BucketShadows = 21;

template <typename T>
const T* Guest(uint32_t address) {
    return reinterpret_cast<const T*>(te::GuestPointer(address, sizeof(T)));
}
template <typename T>
T* GuestMutable(uint32_t address) {
    return reinterpret_cast<T*>(te::GuestPointer(address, sizeof(T)));
}

// ---- the game thread's seam costs (the device spends ~10 us of the game thread per placed model, ~4 us per box
// test) ----------------------------------------------------------------------------------------------------------------
// The guest memory read once per seam call: te::GuestPointer calls two out-of-line functions (te::GuestMemory /
// GuestMemorySize) for every pointer -- the same test (`address + size` within the memory) on the same two values
struct Mem {
    uint8_t* base;
    uint64_t size;
    Mem() : base(te::GuestMemory()), size(te::GuestMemorySize()) {}
#if defined(TE_NATIVE)
    // the Vita's native build (te_host.h): a guest address is the native pointer
    const uint8_t* Bytes(uint32_t address, size_t n) const { (void)n; return te::GuestPointer(address, n); }
#else
    const uint8_t* Bytes(uint32_t address, size_t n) const { return base && uint64_t(address) + n <= size ? base + address : nullptr; }
#endif
    template <typename T> const T* Get(uint32_t address) const { return reinterpret_cast<const T*>(Bytes(address, sizeof(T))); }
    const float* Floats(uint32_t address, size_t n) const { return reinterpret_cast<const float*>(Bytes(address, n * 4)); }
};
// std::isfinite on the stored bits: false exactly when the exponent is all ones (an infinity or a NaN). The Cortex-A9
// compiled each std::isfinite as VCMP + VMRS (a VFP-to-core flag transfer that waits for the VFP pipeline): 32 of them for
// every placed model's two matrices
bool FiniteBits(const float* p, size_t n) {
    uint32_t special = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t bits;
        std::memcpy(&bits, p + i, 4);
        special |= uint32_t((bits & 0x7F800000u) == 0x7F800000u);
    }
    return special == 0;
}
// The diagnosis switches the per-instance paths read, read once on the game thread (a function-local `static const bool x =
// getenv()` is a guard load + a DMB barrier on every call on the Vita: several per instance and per pass)
struct SceneOptions {
    bool read = false;
    bool noDraw = false;          // TE_GFX_NO_DRAW
    bool firstPassOnly = false;   // TE_GFX_NO_PASSES
    bool cullAllOut = false, cullAllIn = false, cullBoxesOut = false;   // TE_GFX_CULL_ALL_OUT / _ALL_IN / _BOXES_OUT
    std::string skipBuckets;      // TE_GFX_DIAG_SKIP_BUCKET=a,b -> ",a,b,"
    // TE_GAMECPU2=0: the game thread's seams in their earlier form (the in-run A/B: TE_AB=gamecpu2, graphics.cpp AbToggle)
    bool fast = true;
    // (the Vita) TE_VITA_LATE_SEAMS=0: the later seam forms off -- a blend skin's parts and a shadow mesh's parts
    // one build each again, the box tests through their copies -- the earlier forms, for the draw-list check (TE_GFX_DRAWHASH)
    bool lateSeams = true;
    // (the Vita) TE_GFX_SHADOW_SHARED: 1 (default) the shadow volumes through ShadowGeometry::BuildRunsShared (each
    // distinct position once, the strips wholly outside one clip plane skipped), 0 BuildRuns as before, 2 (diagnosis) both,
    // compared. TE_GFX_BUILD_CERT: the builds' per-vertex arithmetic without its clamps where it is provably the same
    // (the exponent budget: exponent_budget.h) -- 1 (default), 0 the clamped forms, 2 both compared, 3 MeshGeometry's two
    // passes on the clamped forms, 4 MeshGeometry's two passes compared with its one-pass form
    int shadowShared = 1;
    int buildCert = 1;
    // TE_GFX_BUILD_REPEAT_CENSUS=1 (diagnosis): the builds whose inputs repeat one of the picture before's (a hash of each
    // shadow / chunked build's inputs on the game thread: off by default -- the device's game thread is its bottleneck)
    bool repeatCensus = false;
    __attribute__((noinline, cold)) void Read() {
        read = true;
        noDraw = std::getenv("TE_GFX_NO_DRAW") != nullptr;
        firstPassOnly = std::getenv("TE_GFX_NO_PASSES") != nullptr;
        cullAllOut = std::getenv("TE_GFX_CULL_ALL_OUT") != nullptr;
        cullAllIn = std::getenv("TE_GFX_CULL_ALL_IN") != nullptr;
        cullBoxesOut = std::getenv("TE_GFX_CULL_BOXES_OUT") != nullptr;
        if (const char* v = std::getenv("TE_GFX_DIAG_SKIP_BUCKET")) skipBuckets = std::string(",") + v + ",";
        if (const char* v = std::getenv("TE_GAMECPU2")) fast = v[0] != '0';
        if (const char* v = std::getenv("TE_VITA_LATE_SEAMS")) lateSeams = v[0] != '0';
#if defined(__vita__)
        if (const char* v = std::getenv("TE_GFX_SHADOW_SHARED")) shadowShared = std::max(0, std::min(2, std::atoi(v)));
        if (const char* v = std::getenv("TE_GFX_BUILD_CERT")) buildCert = std::max(0, std::min(4, std::atoi(v)));
        repeatCensus = std::getenv("TE_GFX_BUILD_REPEAT_CENSUS") != nullptr;
        title::MeshGeometry::SetCertified(buildCert);
        title::ShadowGeometry::SetCertified(buildCert);
        std::fprintf(stderr, "[te.gfx] builds: shadow volumes %s; the per-vertex arithmetic %s (TE_GFX_BUILD_CERT=%d)\n",
                     shadowShared == 0 ? "through BuildRuns (TE_GFX_SHADOW_SHARED=0)"
                                       : shadowShared == 2 ? "shared AND BuildRuns, compared (TE_GFX_SHADOW_SHARED=2)"
                                                           : "by distinct position, the strips outside the clip volume skipped",
                     buildCert == 1   ? "clamp-free where the exponent budget and the audit allow it, else clamped"
                     : buildCert == 2 ? "clamp-free AND clamped, compared"
                     : buildCert == 3 ? "in two passes, clamped"
                     : buildCert == 4 ? "meshes: two passes vs one, their output compared (shadows clamp-free where allowed)"
                                      : "clamped (as before)",
                     buildCert);
#endif
    }
} g_opts;
inline const SceneOptions& Opts() {
    if (!g_opts.read) g_opts.Read();
    return g_opts;
}
inline bool FastSeams() { return Opts().fast; }
// The seams' census (calls per present in the device log: what each te.prof share costs a call), on the game thread
struct SeamCensus {
    uint64_t placed = 0, rigid = 0, nativeDraws = 0, boxTests = 0, boxVisible = 0, boxClipped = 0, screenMade = 0, clipMade = 0,
             modelLoads = 0, cellTests = 0, results = 0, matrixReads = 0, materialHits = 0, materialMisses = 0;
} g_seams;
void SeamCensusLine();
}  // namespace
namespace te::gfx {
uint32_t g_seamCalls[kCallCount] = {};
bool g_repeatCensus = false;
namespace {
struct RepeatTrack {
    std::vector<uint32_t> prev, cur, set;   // this picture's / the previous picture's input hashes, the previous ones as a set
    uint64_t calls = 0, samePlace = 0, sameAny = 0;
};
RepeatTrack g_repeat[kCallCount];
bool InSet(const std::vector<uint32_t>& set, uint32_t h) {
    if (set.empty()) return false;
    const size_t mask = set.size() - 1;
    for (size_t k = h & mask;; k = (k + 1) & mask) {
        if (set[k] == h) return true;
        if (!set[k]) return false;
    }
}
}  // namespace
void NoteRepeatWords(SeamCall c, const void* a, size_t aBytes, const void* b, size_t bBytes, const void* d, size_t dBytes) {
    uint32_t h = 2166136261u;
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        if (!q) { h = (h ^ 0xA5u) * 16777619u; return; }
        for (size_t i = 0; i < n; ++i) h = (h ^ q[i]) * 16777619u;
        h = (h ^ 0x5Au) * 16777619u;
    };
    mix(a, aBytes);
    if (b || bBytes) mix(b, bBytes);
    if (d || dBytes) mix(d, dBytes);
    h |= 1u;
    RepeatTrack& t = g_repeat[c];
    const size_t at = t.cur.size();
    t.cur.push_back(h);
    t.calls++;
    if (at < t.prev.size() && t.prev[at] == h) t.samePlace++;
    if (InSet(t.set, h)) t.sameAny++;
}
}  // namespace te::gfx
namespace {
// the repeat census's picture end (SeamCensusLine, every picture): this picture's hashes become the previous ones
void RepeatPictureEnd() {
    for (auto& t : te::gfx::g_repeat) {
        size_t cap = 16;
        while (cap < t.cur.size() * 2) cap <<= 1;
        t.set.assign(cap, 0u);
        for (uint32_t h : t.cur)
            for (size_t k = h & (cap - 1);; k = (k + 1) & (cap - 1)) {
                if (t.set[k] == h) break;
                if (!t.set[k]) { t.set[k] = h; break; }
            }
        t.prev.swap(t.cur);
        t.cur.clear();
    }
}
// TE_GAMECPU2_BENCH=P (diagnosis): present P's box tests captured (frustum, link, box, model) and timed afterwards in the
// same order through the previous TestBoxAt (TestBoxAtReference) and the present one's halves -- the per-call cost of
// each form on the same inputs, on this CPU ("[te.gfx.bench]")
struct CullBench {
    long at = -2;   // -2 unread, -1 off
    bool capturing = false, done = false;
    struct Case {
        title::CullingMath::Frustum frustum;
        title::CullingMath::Link link;
        title::CullingMath::Box box;
        Matrix model;
    };
    std::vector<Case> cases;
} g_cullBench;
void GameCpuBench();
// The interpolator's tags (graphics_internal.h SetDrawTag): a matrix flattened row-major, the tag of an object with its
// model->world and model->screen matrices as drawn, cleared after the object's draws
void Flat(const Matrix& m, float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = m[r][c];
}
void TagDiag(uint64_t id, const Matrix& world, const Matrix& screen);
// The world matrix the interpolator needs for a draw projected by `screen`: the one with world . toCamera . cameraToScreen
// == screen under the current view (a placed model's screen matrix carries its chunk's matrix; a rigid model's equals
// world . toScreen already). W = screen . (toCamera . cameraToScreen)^-1; nullopt when the view is missing or singular.
std::optional<Matrix> WorldOfScreen(const Matrix& screen);
// `family`: the model the instance shares with the others the producer names by draw order (OrdinalId), 0 for none -- the
// interpolator pairs an instance with a clearly nearer one of its family when its own previous instance is far
void TagRigid(uint64_t id, const Matrix& world, const Matrix& screen, uint32_t family = 0) {
    float w[16], sc[16];
    Flat(world, w);
    Flat(screen, sc);
    te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{id, family, ps2r::ps2e::InterpKind::Rigid}, w, sc);
    TagDiag(id, world, screen);
}
uint64_t TagId(uint32_t a, uint32_t b) { return (uint64_t(a) << 32) | uint64_t(b); }
// The k-th draw of `model` through `matrix` this frame: a producer that draws several instances of one model through ONE
// matrix address (a stack local: an OGI's jointed rigid pieces, the level's objects) names them by their order within the
// picture. The order shifts when an earlier instance is culled or comes into view, so a rigid instance also names its
// family (FamilyOf) and the interpolator pairs it with a clearly nearer instance of the family when its own previous one is
// far. A chunk's placements take no ordinal (one identity per model and chunk view: paired by nearness). Reset at Present
// (the picture's end).
std::unordered_map<uint64_t, uint32_t> g_drawOrdinals;
// The blended UV-scrolled passes as native meshes (te::gfx::SetNativeScrolled): ON by default; TE_GFX_NATIVE_SCROLLED=0
// (read by graphics.cpp ReadOptions: a namespace-scope getenv here would run before the Vita's args.txt has set the
// environment) builds them on the CPU every frame as before
std::atomic<bool> g_nativeScrolled{true};
uint64_t OrdinalId(uint64_t base) {
    const uint32_t k = g_drawOrdinals[base]++;
    return base ^ (uint64_t(k) * 0xD6E8FEB86659FD93ull);
}
// The family an ordinal-named instance belongs to (InterpTag::part of a rigid draw): never 0
uint32_t FamilyOf(uint64_t base) {
    const uint32_t f = uint32_t((base ^ (base >> 32)) * 0x9E3779B97F4A7C15ull >> 32);
    return f ? f : 1u;
}

std::optional<Matrix> ReadMatrix(uint32_t address) {
    const auto* m = reinterpret_cast<const float(*)[4]>(te::GuestPointer(address, 64));
    if (!m) return std::nullopt;
    Matrix out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
    return out;
}
Matrix ToMatrix(const float (*m)[4]) {
    Matrix out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
    return out;
}
std::optional<Vector> ReadVector(uint32_t address) {
    const float* v = reinterpret_cast<const float*>(te::GuestPointer(address, 16));
    if (!v) return std::nullopt;
    return Vector{v[0], v[1], v[2], v[3]};
}
void WriteMatrix(uint32_t address, const Matrix& m) {
    auto* out = reinterpret_cast<float(*)[4]>(te::GuestPointer(address, 64));
    if (!out) return;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
}
// Row-vector matrix product (the game's VuMultiplyMatrices: out = a * b, p' = p * a * b)
Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a[r][k] * b[k][c];
            out[r][c] = s;
        }
    return out;
}
Matrix Identity() { return Matrix{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}; }
Matrix Translation(const Vector& t) {
    Matrix m = Identity();
    m[3] = {t[0], t[1], t[2], 1.f};
    return m;
}
// The affine inverse (a rotation / scale with a translation in row 3, the PS2's row-vector convention): the 3x3 inverted by
// cofactors, the translation carried back; nullopt when singular
std::optional<Matrix> AffineInverse(const Matrix& m) {
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return std::nullopt;
    const float r = 1.f / det;
    Matrix o = Identity();
    o[0][0] = (e * i - f * h) * r; o[0][1] = (c * h - b * i) * r; o[0][2] = (b * f - c * e) * r;
    o[1][0] = (f * g - d * i) * r; o[1][1] = (a * i - c * g) * r; o[1][2] = (d * c - a * f) * r;
    o[2][0] = (d * h - e * g) * r; o[2][1] = (b * g - a * h) * r; o[2][2] = (a * e - b * d) * r;
    for (int k = 0; k < 3; ++k) {
        o[3][k] = -(m[3][0] * o[0][k] + m[3][1] * o[1][k] + m[3][2] * o[2][k]);
        o[k][3] = 0.f;
    }
    o[3][3] = 1.f;
    return o;
}
// The camera's position in a model's space (models.cpp EyeIn / CameraPosition x the instance's inverse): the point the
// view's toCamera sends to the origin, carried through the world matrix's inverse
std::optional<Vector> EyeIn(const Matrix& world, const float (*toCamera)[4]) {
    const auto invCam = AffineInverse(ToMatrix(toCamera));
    const auto invWorld = AffineInverse(world);
    if (!invCam || !invWorld) return std::nullopt;
    Vector eye{};
    for (int k = 0; k < 3; ++k) eye[k] = (*invCam)[3][k];
    eye[3] = 1.f;
    Vector o{};
    for (int k = 0; k < 3; ++k) o[k] = eye[0] * (*invWorld)[0][k] + eye[1] * (*invWorld)[1][k] + eye[2] * (*invWorld)[2][k] + (*invWorld)[3][k];
    o[3] = 1.f;
    for (int k = 0; k < 3; ++k) if (!std::isfinite(o[k])) return std::nullopt;
    return o;
}

bool Finite(const Matrix& m) {
    // (FiniteBits: std::isfinite on the stored bits, without a VFP flag transfer per element)
    return FiniteBits(m[0].data(), 4) && FiniteBits(m[1].data(), 4) && FiniteBits(m[2].data(), 4) && FiniteBits(m[3].data(), 4);
}

const RenderView* CurrentView() {
    const uint32_t* p = Guest<uint32_t>(te::RetailAddress(kRenderViewPointer));
    return p && *p ? Guest<RenderView>(*p) : nullptr;
}

std::optional<Matrix> WorldOfScreen(const Matrix& screen) {
    const RenderView* view = CurrentView();
    if (!view) return std::nullopt;
    // in double throughout: the camera->screen matrix is projective with entries ~1e6 beside ~1; a float product and
    // inverse leave a shear of a few parts per thousand in the world's basis, which a far object's re-projection shows
    double cam[16], c2s[16], ws[16], inv[16], sc[16], w[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            cam[r * 4 + c] = view->toCamera[r][c];
            c2s[r * 4 + c] = view->cameraToScreen[r][c];
            sc[r * 4 + c] = screen[r][c];
        }
    ps2r::ps2e::SceneInterpolator::Multiply4(cam, c2s, ws);
    if (!ps2r::ps2e::SceneInterpolator::Invert4(ws, inv)) return std::nullopt;
    ps2r::ps2e::SceneInterpolator::Multiply4(sc, inv, w);
    Matrix out;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r][c] = float(w[r * 4 + c]);
    if (!Finite(out)) return std::nullopt;
    return out;
}

// TE_GFX_INTERP_DIAG: the tag matrices' consistency -- the interpolator rebuilds model->screen as world . toCamera .
// cameraToScreen; a producer whose drawn matrix is not that product would be re-projected wrongly
void TagDiag(uint64_t id, const Matrix& world, const Matrix& screen) {
    static const bool diag = std::getenv("TE_GFX_INTERP_DIAG") != nullptr;
    if (!diag) return;
    static int said = 0;
    const RenderView* view = CurrentView();
    if (!view || said >= 12) return;
    const Matrix rebuilt = Multiply(Multiply(world, ToMatrix(view->toCamera)), ToMatrix(view->cameraToScreen));
    const Matrix viaScreen = Multiply(world, ToMatrix(view->toScreen));
    float worst = 0, worst2 = 0, scale = 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            worst = std::max(worst, std::fabs(rebuilt[r][c] - screen[r][c]));
            worst2 = std::max(worst2, std::fabs(viaScreen[r][c] - screen[r][c]));
            scale = std::max(scale, std::fabs(screen[r][c]));
        }
    std::fprintf(stderr, "[te.gfx.interp] tag %llx: |world.toCamera.cameraToScreen - drawn| %.4g, |world.toScreen - drawn| %.4g (drawn max %.4g)\n",
                 (unsigned long long)id, worst, worst2, scale);
    // the round trip the interpolator does at fraction 1: a model point projected, the drawn vertex taken back through the
    // inverse and forward again (K = S^-1 . world . toCamera . cameraToScreen = identity): the error in frame pixels
    double S[16], Sinv[16];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) S[r * 4 + c] = screen[r][c];
    if (ps2r::ps2e::SceneInterpolator::Invert4(S, Sinv)) {
        const double pt[4] = {0.3, -0.2, 0.5, 1.0};
        double h[4] = {0, 0, 0, 0};
        for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) h[c] += pt[k] * S[k * 4 + c];
        const double X = h[0] / h[3], Y = h[1] / h[3], Z = double(uint32_t(int64_t(h[2] / h[3])));
        double b[4] = {0, 0, 0, 0};
        for (int c = 0; c < 4; ++c) b[c] = X * Sinv[c] + Y * Sinv[4 + c] + Z * Sinv[8 + c] + Sinv[12 + c];
        std::fprintf(stderr, "[te.gfx.interp]   round trip: model (%.3f %.3f %.3f) -> screen (%.1f %.1f %.1f w %.4f) -> model (%.4f %.4f %.4f w %.4f) -> %.4f %.4f %.4f\n",
                     pt[0], pt[1], pt[2], X, Y, Z, h[3], b[0], b[1], b[2], b[3], b[0] / b[3], b[1] / b[3], b[2] / b[3]);
        std::fprintf(stderr, "[te.gfx.interp]   S rows: (%.3g %.3g %.3g %.3g) (%.3g %.3g %.3g %.3g) (%.3g %.3g %.3g %.3g) (%.3g %.3g %.3g %.3g)\n",
                     S[0], S[1], S[2], S[3], S[4], S[5], S[6], S[7], S[8], S[9], S[10], S[11], S[12], S[13], S[14], S[15]);
    }
    said++;
}

void Once(const char* what) {
    static std::unordered_map<std::string, bool> said;
    bool& s = said[what];
    if (!s) {
        s = true;
        std::fprintf(stderr, "[te.gfx] %s\n", what);
    }
}

// ---- the decoded models ---------------------------------------------------------------------------------------------------
// A few consecutive triangles of one of a part's strips (the Vita's per-chunk draws of a CLIPPED instance -- ChunkedBuild):
// the strip's vertices [v0, v1) (its triangles are those closing at v0 + 2 .. v1 - 1), their place in the part's native
// mesh and their model-space box. A CPU build of chunks builds those vertices as strips of their own (the same vertices,
// the same triangles and the same clipping decisions per triangle as the whole strip's build: each depends on its own
// three vertices only; the winding, which the strip's earlier triangles set, colours no pixel differently)
struct NativeChunk {
    uint32_t strip = 0;                        // the strip (part.prepared.strips)
    uint16_t v0 = 0, v1 = 0;                   // its vertices in the strip
    uint32_t first = 0, count = 0;             // its triangles in the native mesh (list vertices)
    std::array<float, 6> box{};                // x0 y0 z0 x1 y1 z1 of its vertices
};
// A submodel: the strips the VIF packet UNPACKs for the VU1 strip programs (positions, STQ with the colour bytes in the low
// byte of each word, the lit models' normals and their separate colour bytes). Kept as the engine's Mesh (its Build consumes
// it) plus the normals and the static colours it needs when lit.
struct Part {
    Geometry::Mesh mesh;                                      // the decoded strips (the prepared form's source)
    te::gfx::MeshPrepared prepared;                           // prepared once per decode for every instance's build
    std::vector<std::vector<std::array<float, 3>>> normals;   // per strip, per vertex; empty when the packet has none
    std::vector<std::vector<std::array<uint8_t, 4>>> colours; // the V4-8 colour stream (lit models); empty when none
    bool decoded = false;
    uint32_t vertices = 0;
    size_t triangles = 0;
    uint32_t native = 0;            // the native mesh (te::gfx::NativeMeshes): 0 until made
    std::array<float, 6> nativeBox{};   // its model-space bounds (x0 y0 z0 x1 y1 z1): the draws' screen boxes
    mutable uint8_t meshState = 0;  // 0 not asked for, 1 asked for this picture, 2 made (or declined), 3 being made (a batch)
    // the Vita: the native mesh's chunks (made with the mesh, never changed after; empty for a small part, which is one
    // chunk), and whether they are there -- a build posted before they were made does not read them (it took `chunked`)
    std::vector<NativeChunk> chunks;
    bool chunked = false;
    uint32_t nativeCount = 0;       // the native mesh's vertices (a triangle list)
    // its native mesh prepared at its load, on a worker (PartPrep, TE_GFX_MESH_AT_LOAD): taken when the part asks for its
    // mesh, settled (cancelled / waited for) before the part goes (RetireMesh)
    std::shared_ptr<struct PartPrep> prep;
};
// Each submodel's packet BYTES, kept host-side: the game releases the packet's disk-manager memory after ReadModel
// (resources.cpp DeferRelease), so a save state cannot re-read it from RAM0; the state carries these and the restore
// decodes them again (te::gfx::ModelSources / ReplayModelSources), as graphics.cpp keeps the textures' packets
struct PartSource {
    int32_t vertices = 0;
    std::vector<uint8_t> packet;
};
struct Model {
    std::vector<Part> parts;
    std::vector<PartSource> sources;
};
// A part's native mesh PREPARED AT ITS LOAD (TE_GFX_MESH_AT_LOAD; the Vita's default): on a worker (the builder pool's
// background queue), what FlushNativeMeshes and the drawing thread's batch made from the part when it first asked for its
// mesh -- the triangle list (MeshVertices), its chunks (MakeChunks), its count and box, and the device's prepared form
// (ps2e_draw_mesh_prepare: the list deduplicated, the work that was 65 % of the drawing thread's mesh making). The part still
// asks at its first draw (CPU-built that picture, as before) and takes its handle at the next (ResolveMeshBatches): the same
// pictures; the batch only makes the GPU's buffers. The chunks, count and box go into the part with its handle (they are
// read only once `native` is set). Computed under the game thread's floating-point environment (FZ: the compares of the
// boxes), where FlushNativeMeshes computed them.
struct PrepCensus {
    std::atomic<uint64_t> queued{0}, ahead{0}, used{0}, unready{0};
};
PrepCensus g_prepCensus;
struct PartPrep : te::gfx::PreparedMesh {
    const Part* part = nullptr;     // (alive until Settle: RetireMesh settles it before the part goes)
    std::fenv_t env{};
    std::vector<NativeChunk> chunks;
    std::array<float, 6> box{1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    uint32_t count = 0;             // the triangle list's vertices
};
void InstallPrep(Part& part, PartPrep& prep);
std::unordered_map<uint32_t, Model> g_models;   // RigidModelData guest address -> its parts
// g_models' lookups through a direct-mapped cache (every instance's draw looked its model up: the bucket index is a
// division, a software routine on the Cortex-A9). An element's address is stable for its life in the map (rehashing moves
// no node); the cache forgets an address when its element is erased, everything when the map is cleared.
struct ModelSlot {
    uint32_t key = 0;
    Model* model = nullptr;
};
std::array<ModelSlot, 512> g_modelSlots;
ModelSlot& ModelSlotOf(uint32_t address) { return g_modelSlots[((address >> 4) ^ (address >> 13)) & (g_modelSlots.size() - 1)]; }
Model* FindModel(uint32_t address) {
    ModelSlot& slot = ModelSlotOf(address);
    if (address && slot.key == address) return slot.model;
    const auto it = g_models.find(address);
    if (it == g_models.end()) return nullptr;
    slot.key = address;
    slot.model = &it->second;
    return slot.model;
}
void ForgetModel(uint32_t address) {
    ModelSlot& slot = ModelSlotOf(address);
    if (slot.key == address) slot = ModelSlot{};
}
void ForgetModels() { g_modelSlots.fill(ModelSlot{}); }

// A part's material as ReadMaterial reads it and its first shader's draw state as StateOf makes it, cached by the material's
// address (every draw of every instance read and decoded both: ~370 instructions, three guest pointers and a guard barrier).
// An entry is used only when every byte the two functions read is what it was made from -- the material's first 32 bytes
// (magic, kind, bucket, shaderCount, shaders[]), its first shader's first 32 (magic, type, settings, texture, colour, page,
// lodK, lodL; the scroll after them is read per draw as before), that shader's texture's header bytes 0x14..0x1B (levels,
// formats, hasAlpha, function) and the texture's device handle -- so it answers what they would answer.
struct MaterialSlot {
    uint32_t address = 0;           // 0: empty
    uint32_t shader = 0, texture = 0, handle = 0;
    uint32_t pool = 0, poolGeneration = 0;   // the Vita's state pool entry of `state` (te::gfx::PoolState; 0 none)
    uint32_t materialWords[8], shaderWords[8], textureWords[2];
    MaterialView view;
    ps2e_draw_state state;
};
std::array<MaterialSlot, 512> g_materialSlots;
inline bool SameWords(const uint32_t* kept, const uint8_t* bytes, int words) {
    uint32_t differ = 0;
#if defined(__vita__)
#pragma GCC novector   // (core registers: a NEON XOR-OR ends in a NEON-to-core transfer, ~20 cycles on the Cortex-A9)
#endif
    for (int k = 0; k < words; ++k) {
        uint32_t w;
        std::memcpy(&w, bytes + 4 * k, 4);
        differ |= w ^ kept[k];
    }
    return differ == 0;
}
// Fills `view` for the material at `address` (as ReadMaterial would) and answers its first shader's state (as StateOf would):
// the cache slot's (valid until the next CachedMaterial call) or `scratch`, filled. `pool` (the Vita's compact records; null:
// not asked): the state pool's entry of the answered state's bytes, 0 when there is none
const ps2e_draw_state* CachedMaterial(const Mem& mem, uint32_t address, MaterialView& view, ps2e_draw_state& scratch,
                                      uint32_t* pool = nullptr) {
    const uint8_t* mat = address ? mem.Bytes(address, sizeof(te_gfx_material)) : nullptr;
    MaterialSlot& slot = g_materialSlots[((address >> 4) ^ (address >> 13)) & (g_materialSlots.size() - 1)];
    if (mat && slot.address == address && SameWords(slot.materialWords, mat, 8)) {
        const uint8_t* sh = slot.shader ? mem.Bytes(slot.shader, sizeof(te_gfx_shader)) : nullptr;
        const uint8_t* tx = slot.texture ? mem.Bytes(slot.texture, sizeof(te_gfx_texture)) : nullptr;
        if ((!slot.shader || (sh && SameWords(slot.shaderWords, sh, 8))) && (!slot.texture || (tx && SameWords(slot.textureWords, tx + 0x14, 2))) &&
            te::gfx::TextureHandleAt(slot.texture) == slot.handle) {
            view = slot.view;
            g_seams.materialHits++;
            if (pool) {
                // (the pool may have remade the entry for other bytes since: the generation says)
                if (!te::gfx::PoolCurrent(slot.pool, slot.poolGeneration)) slot.pool = te::gfx::PoolState(slot.state, &slot.poolGeneration);
                *pool = slot.pool;
            }
            return &slot.state;
        }
    }
    g_seams.materialMisses++;
    if (pool) *pool = 0;
    view = te::gfx::ReadMaterial(address);
    scratch = te::gfx::StateOf(view);
    ps2e_draw_state& state = scratch;
    if (!mat || !view.material) return &scratch;   // (answered, not kept: no material there)
    // the bytes ReadMaterial and StateOf read: the material's; the shader at shaders[0] when ReadMaterial looked there
    // (shaderCount and shaders[0] non-zero) -- whatever it found; the texture it then read when the shader was one
    uint32_t words[8];
    std::memcpy(words, mat, 32);
    const te_gfx_material* m = reinterpret_cast<const te_gfx_material*>(mat);
    const uint32_t shader = m->shaderCount && m->shaders[0] ? m->shaders[0] : 0;
    const uint8_t* sh = shader ? mem.Bytes(shader, sizeof(te_gfx_shader)) : nullptr;
    if (shader && !sh) return &scratch;   // (a shader address outside the memory: not kept)
    const uint32_t texture = view.shader ? view.shader->texture : 0;
    const uint8_t* tx = texture ? mem.Bytes(texture, sizeof(te_gfx_texture)) : nullptr;
    if (texture && !tx) return &scratch;
    slot.address = address;
    std::memcpy(slot.materialWords, words, 32);
    slot.shader = shader;
    if (sh) std::memcpy(slot.shaderWords, sh, 32);
    slot.texture = texture;
    if (tx) std::memcpy(slot.textureWords, tx + 0x14, 8);
    slot.handle = te::gfx::TextureHandleAt(texture);
    slot.view = view;
    std::memcpy(&slot.state, &state, sizeof state);   // (byte for byte: the pool keys on the bytes)
    slot.pool = 0;
    slot.poolGeneration = 0;
    if (pool) *pool = slot.pool = te::gfx::PoolState(slot.state, &slot.poolGeneration);
    return &scratch;
}
// The shadow meshes' geometry as the type-0B program saw it (title::ShadowGeometry: each strip emitted twice, the
// front-facing triangles +8 and the back-facing 248 by the screen-space winding, the clipped fans by the authored flag),
// decoded once per model from the kept packet bytes at the first shadow draw
// (the Vita) per decoded part: its strips' and vertices' counts and its distinct positions (by their bits) -- the
// builders' census
struct ShadowPartInfo {
    uint32_t strips = 0, vertices = 0, unique = 0;
};
struct ShadowModel {
    std::vector<title::ShadowGeometry::Mesh> parts;
    std::vector<bool> decoded;
    bool tried = false;
    std::vector<ShadowPartInfo> info;   // (the Vita: one per part, filled at the decode)
    // (the Vita) each decoded part's distinct positions (title::ShadowGeometry::Share; empty: the part builds through
    // BuildRuns as before)
    std::vector<title::ShadowGeometry::Shared> shared;
};
std::unordered_map<uint32_t, ShadowModel> g_shadowModels;
// A released model's storage (ps2e_gfx2_model_released): the two maps' nodes extracted whole -- the parts and the shadow
// parts stay at the addresses the posted builds hold -- freed with the frame slot it travels with (te::gfx::RetireStorage)
struct RetiredModel : te::gfx::RetiredStorage {
    std::unordered_map<uint32_t, Model>::node_type model;
    std::unordered_map<uint32_t, ShadowModel>::node_type shadow;
};
void LoadModelPart(uint32_t model, int32_t index, int32_t count, const uint8_t* bytes, int32_t size, int32_t vertices);

// The parts' meshes being made on the drawing thread (FlushNativeMeshes: posted at a picture's hand-over without waiting),
// oldest first. A part being made (meshState 3) takes its handle before anything reads it -- its next draw, its release --
// so every draw that reads a part's mesh reads the handle it read when the hand-over waited for the meshes.
struct MeshBatch {
    std::shared_ptr<te::gfx::MeshBatchJob> job;
    std::vector<Part*> parts;
    uint64_t ticket = 0;
};
std::vector<MeshBatch> g_meshBatches;
void ResolveMeshBatches() {
    if (g_meshBatches.empty()) return;
    for (MeshBatch& b : g_meshBatches) {   // made in order: waiting for each is waiting for the drawing thread once
        te::gfx::WaitMeshes(b.ticket, *b.job);
        for (size_t k = 0; k < b.parts.size(); ++k) {
            b.parts[k]->native = b.job->requests[k].handle;
            b.parts[k]->meshState = 2;
            if (b.job->requests[k].prepared) InstallPrep(*b.parts[k], static_cast<PartPrep&>(*b.job->requests[k].prepared));
        }
    }
    g_meshBatches.clear();
}

// A part's native mesh handed back (destroyed once no frame still draws it)
void RetireMesh(Part& part) {
    if (part.meshState == 3) ResolveMeshBatches();
    if (part.prep) {
        part.prep->Settle();   // (a worker's preparation reads the part: cancelled, or waited for)
        part.prep.reset();
    }
    te::gfx::RetireMesh(part.native);
    part.native = 0;
    part.meshState = 0;
}

// The part's triangles as a native mesh: the prepared strips' triangles in the build's order (MeshGeometry::
// BuildPrepared: a vertex flagged restart draws no triangle and restarts the winding; the others alternate it), each
// vertex with its position, STQ and colour as the build reads them, the normal and the colour LitColours lights
std::vector<ps2e_draw_mesh_vertex> MeshVertices(const Part& part, std::vector<uint32_t>* offsets = nullptr) {
    std::vector<ps2e_draw_mesh_vertex> out;
    const auto& prepared = part.prepared;
    std::vector<ps2e_draw_mesh_vertex> strip;
    if (offsets) offsets->assign(1, 0u);   // each strip's end in the list (offsets[s + 1]), from 0
    for (size_t si = 0; si < prepared.strips.size(); ++si) {
        const auto& range = prepared.strips[si];
        const bool normals = si < part.normals.size() && part.normals[si].size() == range.count;
        const bool colours = si < part.colours.size() && part.colours[si].size() == range.count;
        strip.assign(range.count, ps2e_draw_mesh_vertex{});
        for (uint32_t i = 0; i < range.count; ++i) {
            const size_t at = range.first + i;
            ps2e_draw_mesh_vertex& v = strip[i];
            v.x = prepared.positions[at][0];
            v.y = prepared.positions[at][1];
            v.z = prepared.positions[at][2];
            v.s = prepared.stq[at][0];
            v.t = prepared.stq[at][1];
            v.q = prepared.stq[at][2];
            const auto& c = prepared.colours[at];
            v.r = c[0]; v.g = c[1]; v.b = c[2]; v.a = c[3];
            if (normals) {
                v.nx = part.normals[si][i][0];
                v.ny = part.normals[si][i][1];
                v.nz = part.normals[si][i][2];
            }
            const auto& lc = colours ? part.colours[si][i] : c;
            v.lr = lc[0]; v.lg = lc[1]; v.lb = lc[2]; v.la = lc[3];
        }
        unsigned parity = 0;
        for (uint32_t i = 0; i < range.count; ++i) {
            if (prepared.restart[range.first + i]) { parity = 0; continue; }
            if (i < 2) continue;
            out.push_back(strip[i - 2 + (parity & 1)]);
            out.push_back(strip[i - 1 - (parity & 1)]);
            out.push_back(strip[i]);
            ++parity;
        }
        if (offsets) offsets->push_back(uint32_t(out.size()));
    }
    return out;
}



// The parts whose meshes this picture's draws asked for (model, submodel); made together before the hand-over
std::vector<std::pair<uint32_t, uint32_t>> g_meshRequests;
#if defined(__vita__)
void FlushSkinMeshes();   // the skins' section below: the GPU-skinned parts' meshes
#endif

// The VIF UNPACK's size in 32-bit words (the data follows the code word-aligned)
bool UnpackWords(uint32_t cmd, uint32_t num, size_t& words) {
    const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
    const uint32_t fields = vn + 1;
    if (num == 0) num = 256;
    switch (vl) {
        case 0: words = size_t(num) * fields; return true;                      // 32-bit
        case 1: words = (size_t(num) * fields * 2 + 3) / 4; return true;         // 16-bit
        case 2: words = (size_t(num) * fields + 3) / 4; return true;             // 8-bit
        case 3: if (vn != 3) return false; words = (size_t(num) * 2 + 3) / 4; return true;   // V4-5
    }
    return false;
}

// The packet: a DMA chain fragment (its RET/CNT tags carry two VIF codes each, TTE), the VIF stream in between. Strips are
// delimited by MSCAL; within a strip the UNPACKs are classified by their VU address: 3 = positions (V3-32), 4 = STQ + colour
// bytes (V4-32), 5 = normals (V3-32), 6 = colours (V4-8). Other layouts fall back to "first V3-32 = positions, the V4-32 = STQ".
bool DecodePacket(const uint8_t* bytes, size_t size, uint32_t vertices, Part& part, std::string* why) {
    auto word = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, bytes + at, 4);
        return v;
    };
    if (size < 16 || (size & 3)) { if (why) *why = "size"; return false; }
    size_t cursor = 0;
    std::vector<float> pos, stq, nrm;
    std::vector<uint32_t> stqBits, col8;
    bool havePos = false, haveStq = false;
    uint32_t total = 0;
    auto flushStrip = [&]() -> bool {
        if (!havePos) return true;   // a packet prologue with no vertices
        const size_t n = pos.size() / 3;
        if (n < 1) { havePos = haveStq = false; pos.clear(); stq.clear(); stqBits.clear(); nrm.clear(); col8.clear(); return true; }
        Geometry::Strip strip;
        strip.vertices.resize(n);
        std::vector<std::array<float, 3>> normals;
        std::vector<std::array<uint8_t, 4>> colours;
        for (size_t i = 0; i < n; ++i) {
            auto& v = strip.vertices[i];
            for (int k = 0; k < 3; ++k) {
                v.position[k] = pos[i * 3 + k];
                if (!std::isfinite(v.position[k])) v.position[k] = 0.f;
            }
            if (haveStq && stq.size() >= (i + 1) * 4) {
                for (int k = 0; k < 3; ++k) {
                    v.stq[k] = stq[i * 4 + k];
                    if (!std::isfinite(v.stq[k])) v.stq[k] = k == 2 ? 1.f : 0.f;
                }
                for (int k = 0; k < 4; ++k) v.colour[k] = uint8_t(stqBits[i * 4 + k]);
                v.restart = (stqBits[i * 4 + 3] & 0x8000) != 0;
            } else {
                v.stq = {0.f, 0.f, 1.f};
                v.colour = {0x80, 0x80, 0x80, 0x80};
            }
            if (nrm.size() >= (i + 1) * 3) normals.push_back({nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]});
            if (col8.size() > i) {
                const uint32_t c = col8[i];
                colours.push_back({uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16), uint8_t(c >> 24)});
            }
        }
        if (normals.size() == n) part.normals.push_back(std::move(normals)); else part.normals.push_back({});
        if (colours.size() == n) part.colours.push_back(std::move(colours)); else part.colours.push_back({});
        total += uint32_t(n);
        part.triangles += n >= 3 ? n - 2 : 0;
        part.mesh.strips.push_back(std::move(strip));
        havePos = haveStq = false;
        pos.clear(); stq.clear(); stqBits.clear(); nrm.clear(); col8.clear();
        return true;
    };
    // The VIF code stream between `from` and `to` (words)
    auto vif = [&](size_t from, size_t to) -> bool {
        size_t at = from;
        while (at + 4 <= to) {
            const uint32_t code = word(at);
            at += 4;
            const uint32_t cmd = (code >> 24) & 0x7F;
            const uint32_t num = (code >> 16) & 0xFF;
            const uint32_t imm = code & 0xFFFF;
            if (cmd >= 0x60) {
                size_t words;
                if (!UnpackWords(cmd, num, words)) { if (why) *why = "unpack format"; return false; }
                if (at + words * 4 > to) { if (why) *why = "unpack overruns"; return false; }
                const uint32_t addr = imm & 0x3FF;
                const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
                const uint32_t count = num ? num : 256;
                if (vl == 0 && vn == 2) {   // V3-32
                    if (!havePos || addr == 3) {
                        if (havePos && addr == 3) { if (!flushStrip()) return false; }
                        pos.resize(size_t(count) * 3);
                        std::memcpy(pos.data(), bytes + at, size_t(count) * 12);
                        havePos = true;
                    } else {   // the second V3-32 stream: normals
                        nrm.resize(size_t(count) * 3);
                        std::memcpy(nrm.data(), bytes + at, size_t(count) * 12);
                    }
                } else if (vl == 0 && vn == 3 && count > 1) {   // V4-32 of the vertices (the one-quadword headers are skipped)
                    stq.resize(size_t(count) * 4);
                    stqBits.resize(size_t(count) * 4);
                    std::memcpy(stq.data(), bytes + at, size_t(count) * 16);
                    std::memcpy(stqBits.data(), bytes + at, size_t(count) * 16);
                    haveStq = true;
                } else if (vl == 2 && vn == 3) {   // V4-8: colours
                    col8.resize(count);
                    std::memcpy(col8.data(), bytes + at, size_t(count) * 4);
                }
                at += words * 4;
                continue;
            }
            switch (cmd) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
                case 0x10: case 0x11: case 0x13: case 0x15: case 0x17:
                    break;
                case 0x14:   // MSCAL: the strip's program runs on what was unpacked
                    if (!flushStrip()) return false;
                    break;
                case 0x20: at += 4; break;              // STMASK
                case 0x30: case 0x31: at += 16; break;  // STROW / STCOL
                case 0x4A: at += size_t(num ? num : 256) * 8; break;   // MPG
                case 0x50: case 0x51: at += size_t(imm ? imm : 65536) * 16; break;   // DIRECT
                default:
                    if (why) *why = "vif code";
                    return false;
            }
        }
        return true;
    };
    // The DMA tags
    while (cursor + 16 <= size) {
        const uint32_t tag0 = word(cursor);
        const uint32_t qwc = tag0 & 0xFFFF;
        const uint32_t id = (tag0 >> 28) & 7;
        if (!vif(cursor + 8, cursor + 16)) return false;   // the tag's own two VIF codes
        cursor += 16;
        const size_t end = cursor + size_t(qwc) * 16;
        if (id == 3 || id == 4) {   // REF/REFS: the data is elsewhere (not in this packet); nothing follows inline
            continue;
        }
        if (end > size) { if (why) *why = "tag overruns"; return false; }
        if (!vif(cursor, end)) return false;
        cursor = end;
        if (id == 6 || id == 7) break;   // RET / END
        if (id == 2 || id == 5) break;   // NEXT / CALL: cannot follow an address
    }
    if (!flushStrip()) return false;
    part.vertices = total;
    part.decoded = !part.mesh.strips.empty();
    if (!part.decoded && why) *why = "no strips";
    (void)vertices;
    return part.decoded;
}

// ---- building an instance's draws -------------------------------------------------------------------------------------------
// vs_lit_tristrip: lit = L3 + L0*max(n.l0, 0) + L1*max(n.l1, 0) + L2*max(n.l2, 0); RGB = min(col * lit, 255) truncated
struct Lights {
    std::array<Vector, 3> directions;   // in the model's space
    std::array<Vector, 3> colours;
    Vector ambient;
};
// The instance's lit colours, one per vertex in strip order (the prepared mesh's order), into a scratch the build reads
void LitColours(const Part& part, const Lights& lights, std::vector<std::array<uint8_t, 4>>& out) {
    out.resize(part.prepared.vertex_count);
    size_t at = 0;
    for (size_t s = 0; s < part.mesh.strips.size(); ++s) {
        const auto& strip = part.mesh.strips[s];
        const bool normals = s < part.normals.size() && part.normals[s].size() == strip.vertices.size();
        const bool colours = s < part.colours.size() && part.colours[s].size() == strip.vertices.size();
        for (size_t i = 0; i < strip.vertices.size(); ++i, ++at) {
            const auto& v = strip.vertices[i];
            std::array<uint8_t, 4> col = colours ? part.colours[s][i] : v.colour;
            float lit[3] = {lights.ambient[0], lights.ambient[1], lights.ambient[2]};
            if (normals) {
                const auto& n = part.normals[s][i];
                for (int l = 0; l < 3; ++l) {
                    const auto& d = lights.directions[l];
                    const float dot = std::max(0.f, n[0] * d[0] + n[1] * d[1] + n[2] * d[2]);
                    for (int k = 0; k < 3; ++k) lit[k] += lights.colours[l][k] * dot;
                }
            }
            auto& o = out[at];
            for (int k = 0; k < 3; ++k) {
                const float value = float(col[k]) * lit[k];
                o[k] = uint8_t(std::min(255.f, std::max(0.f, value)));
            }
            o[3] = col[3];
        }
    }
}

#if defined(__vita__)
// ---- the Vita's per-chunk draws of clipped instances and the sky ------------------------------------------------------------
// A clipped instance whose part is not wholly in front (NativeInFront) was built on the CPU every frame -- the hills
// around the camera, the sky dome. Its part's native mesh is cut into chunks of consecutive strips (MakeChunks), and the
// build (on a worker) sorts each chunk by its box through the view: wholly outside one plane of the title's clip volume
// (every vertex outside it: the title's clip program drops every triangle there) -> nothing; wholly inside the GUARD
// band with w > 0 and its screen depth inside what the native vertex program takes (every vertex: the program draws the
// strips as they are -- no triangle is cut or dropped, which is exactly what the native mesh draws) -> the native mesh's
// range; else -> the CPU build of the chunk's strips alone (the same vertices the part's build gives for them). In chunk
// order: the triangles reach the frame in the build's order. TE_GFX_CHUNKED_NATIVE=0: off (the whole part CPU-built).
std::atomic<bool> g_chunkedNative{true};
constexpr uint32_t kChunkTriangles = 12;
// a native run shorter than this many triangles is built on the CPU instead (each native draw costs the drawing thread
// more than the CPU build of a few triangles saves: TE_GFX_CHUNK_MIN_NATIVE, read at the first chunked build)
uint32_t ChunkMinNative() {
    static const uint32_t v = std::getenv("TE_GFX_CHUNK_MIN_NATIVE") ? uint32_t(std::max(1, std::atoi(std::getenv("TE_GFX_CHUNK_MIN_NATIVE")))) : 96u;
    return v;
}
// the census (the workers': atomics): chunks / triangles drawn native, built, dropped; parts chunked; [0] models, [1] sky
struct ChunkCensus {
    std::atomic<uint64_t> parts{0}, native_tris{0}, cpu_tris{0}, skipped_tris{0}, native_draws{0}, cpu_runs{0}, whole_cpu{0};
};
ChunkCensus g_chunkCensus[2];

// THE BUILDERS' CENSUS: the work the builds do per frame and the work they are spared, in counts the emulator
// reproduces (the device's builder threads cost 26 ms a picture in Totem Hokum's village) -- "[te.gfx.vita] builders per
// frame: ..." beside the chunks' census line. Workers' counters are atomics (relaxed adds); the game thread's plain.
struct BuildersCensus {
    // the shadow volumes: builds (one per mesh), parts, strips and vertices, the distinct positions among those vertices,
    // the vertices out, builds that emitted nothing
    std::atomic<uint64_t> shadow_builds{0}, shadow_parts{0}, shadow_strips{0}, shadow_vertices{0}, shadow_unique{0},
        shadow_out{0}, shadow_empty{0};
    // the shared builds (ShadowGeometry::BuildRunsShared): the positions transformed (the clip side), projected (the screen
    // side), the strips skipped by their outcodes, the builds whose per-position arithmetic went clamp-free (the exponent budget) / that
    // took the clamped forms; TE_GFX_SHADOW_SHARED=2: the parts compared with BuildRuns, those that differed
    std::atomic<uint64_t> shadow_positions{0}, shadow_projected{0}, shadow_skipped{0}, shadow_cert{0}, shadow_clamped{0},
        shadow_compared{0}, shadow_differ{0}, shadow_cert_differ{0};
    // the clipped parts' and the sky's chunked builds ([0] models, [1] sky): the CPU-built chunks' triangles by why they are
    // not native -- a corner at or behind the eye plane, in front but outside the guard band, in the band but outside the
    // native program's depth range, in a native run under the run rule's length, the sky's rule -- the CPU vertices built
    // and the vertices of the clipped fans among the output
    std::atomic<uint64_t> cpu_behind[2] = {}, cpu_guard[2] = {}, cpu_depth[2] = {}, cpu_run[2] = {}, cpu_sky[2] = {},
        cpu_vertices[2] = {}, fan_vertices[2] = {}, out_vertices[2] = {};
    // the skins built on the CPU: blend parts, their vertices, vertex x shape terms, listed shapes of weight 0; plain parts
    std::atomic<uint64_t> blend_parts{0}, blend_vertices{0}, blend_terms{0}, blend_zero{0}, skin_parts{0}, skin_vertices{0};
    // (the game thread's) builds posted and those whose inputs were those of one of the picture before's builds of the kind
    // (a reuse would answer them): [0] shadows, [1] clipped models, [2] sky
    uint64_t posted[3] = {}, repeat[3] = {};
};
BuildersCensus g_builders;
// The inputs' hashes of the picture's builds per kind (the game thread's), the previous picture's sorted
struct RepeatKeys {
    std::vector<uint64_t> cur, prev;
    bool Seen(uint64_t key) const { return std::binary_search(prev.begin(), prev.end(), key); }
    void EndPicture() {
        prev.swap(cur);
        cur.clear();
        std::sort(prev.begin(), prev.end());
    }
} g_repeatKeys[3];
uint64_t HashBytes(uint64_t h, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
void NoteBuildInputs(int kind, uint64_t key) {
    g_builders.posted[kind]++;
    if (g_repeatKeys[kind].Seen(key)) g_builders.repeat[kind]++;
    g_repeatKeys[kind].cur.push_back(key);
}
// a chunked build's inputs as one hash: the part, its mesh and chunks, the view, the lights, the GS origin
uint64_t ChunkKey(const void* part, bool chunked, uint32_t native, const Geometry::View& v, const Lights* lights, float ox, float oy,
                  bool textured) {
    uint64_t h = 1469598103934665603ull;
    const uintptr_t p = reinterpret_cast<uintptr_t>(part);
    const uint32_t flags = (chunked ? 1u : 0u) | (textured ? 2u : 0u) | (lights ? 4u : 0u) | (v.clipped ? 8u : 0u);
    h = HashBytes(h, &p, sizeof p);
    h = HashBytes(h, &flags, sizeof flags);
    h = HashBytes(h, &native, sizeof native);
    h = HashBytes(h, v.screen.data(), sizeof v.screen);
    h = HashBytes(h, v.clip.data(), sizeof v.clip);
    h = HashBytes(h, v.guard.data(), sizeof v.guard);
    h = HashBytes(h, v.scroll.data(), sizeof v.scroll);
    if (lights) {
        h = HashBytes(h, lights->directions.data(), sizeof lights->directions);
        h = HashBytes(h, lights->colours.data(), sizeof lights->colours);
        h = HashBytes(h, lights->ambient.data(), sizeof lights->ambient);
    }
    h = HashBytes(h, &ox, sizeof ox);
    return HashBytes(h, &oy, sizeof oy);
}

// The part's chunks (at most kChunkTriangles triangles of one strip each, the strip's triangles split evenly), from its
// native mesh's strip offsets (MeshVertices' order: a triangle per vertex from the third on that is not a restart)
void MakeChunks(const Part& part, const std::vector<uint32_t>& offsets, std::vector<NativeChunk>& chunks) {
    chunks.clear();
    const auto& prepared = part.prepared;
    const size_t strips = prepared.strips.size();
    if (offsets.size() != strips + 1) return;
    for (uint32_t si = 0; si < strips; ++si) {
        const auto& range = prepared.strips[si];
        // the list position of the triangle each vertex closes (UINT32_MAX: none)
        uint32_t at = offsets[si], tris = 0;
        uint32_t list[40];
        for (uint32_t i = 0; i < range.count && i < 40; ++i) {
            list[i] = UINT32_MAX;
            if (prepared.restart[range.first + i] || i < 2) continue;
            list[i] = at;
            at += 3;
            tris++;
        }
        if (at != offsets[si + 1] || !tris) {
            if (at != offsets[si + 1]) { chunks.clear(); return; }   // not the mesh's order: no chunks
            continue;
        }
        const uint32_t pieces = (tris + kChunkTriangles - 1) / kChunkTriangles;
        const uint32_t per = (tris + pieces - 1) / pieces;
        uint32_t i = 2;
        while (i < range.count) {
            // the next `per` triangles' closing vertices
            NativeChunk c;
            c.strip = si;
            uint32_t n = 0, j = i;
            uint32_t first = UINT32_MAX, last = 0;
            for (; j < range.count && n < per; ++j) {
                if (list[j] == UINT32_MAX) continue;
                if (first == UINT32_MAX) first = list[j];
                last = list[j] + 3;
                n++;
            }
            if (!n) break;
            c.v0 = uint16_t(i - 2);
            c.v1 = uint16_t(j);
            c.first = first;
            c.count = last - first;
            std::array<float, 6> box{1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
            for (uint32_t v = c.v0; v < c.v1; ++v) {
                const auto& pos = prepared.positions[range.first + v];
                for (int k = 0; k < 3; ++k) {
                    box[size_t(k)] = std::min(box[size_t(k)], pos[size_t(k)]);
                    box[size_t(k) + 3] = std::max(box[size_t(k) + 3], pos[size_t(k)]);
                }
            }
            c.box = box;
            chunks.push_back(c);
            i = j;
        }
    }
}

// The view's affine forms over the model space (value = a . p + b; the row-vector convention: lane k of M at p is
// M[3][k] + sum_i M[i][k] p_i) a chunk's box is sorted by
struct ChunkForms {
    struct Form { float a[3], b, l1; };   // l1 = |a_0| + |a_1| + |a_2| (the scale of the margin)
    Form inside[10];   // max < 0 for all of them: guard band (6), w > 0, screen w > 0, screen z >= 0, z < 2^31 w
    Form outside[12];  // a plane's two forms both min > 0: the box wholly outside that plane of the clip volume
    bool usable = false;
};
ChunkForms::Form MakeForm(const Matrix& m, const float (&coef)[4], float b_extra = 0.f) {
    ChunkForms::Form f{};
    for (int i = 0; i < 3; ++i) {
        float v = 0.f;
        for (int k = 0; k < 4; ++k) v += coef[k] * m[size_t(i)][size_t(k)];
        f.a[i] = v;
    }
    float b = b_extra;
    for (int k = 0; k < 4; ++k) b += coef[k] * m[3][size_t(k)];
    f.b = b;
    f.l1 = std::fabs(f.a[0]) + std::fabs(f.a[1]) + std::fabs(f.a[2]);
    return f;
}
ChunkForms FormsOf(const Geometry::View& view) {
    ChunkForms f;
    const auto& g = view.guard;
    if (!(g[3] > 0.f) || !(g[0] > 0.f) || !(g[1] > 0.f) || !(g[2] > 0.f)) return f;   // an unexpected guard: CPU
    // the guard band: |g_k c_k| <= g_w c_w for x, y, z (c the clip coordinates), and g_w c_w > 0
    int n = 0;
    for (int k = 0; k < 3; ++k)
        for (int sign = -1; sign <= 1; sign += 2) {
            float coef[4] = {0.f, 0.f, 0.f, -g[3]};
            coef[k] = float(sign) * g[size_t(k)];
            f.inside[n++] = MakeForm(view.clip, coef);
        }
    {
        const float coef[4] = {0.f, 0.f, 0.f, -1.f};
        f.inside[n++] = MakeForm(view.clip, coef);   // -c_w < 0
    }
    {
        // the native vertex program's depth: the screen's w > 0, z >= 0, z < 2^31 w
        const float cw[4] = {0.f, 0.f, 0.f, -1.f}, cz[4] = {0.f, 0.f, -1.f, 0.f}, ch[4] = {0.f, 0.f, 1.f, -2147483648.f};
        f.inside[n++] = MakeForm(view.screen, cw);
        f.inside[n++] = MakeForm(view.screen, cz);
        f.inside[n++] = MakeForm(view.screen, ch);
    }
    // the clip volume's planes, each as the title's test takes it: lane > |w| <=> lane - w > 0 and lane + w > 0;
    // -lane > |w| <=> -lane - w > 0 and -lane + w > 0
    n = 0;
    for (int k = 0; k < 3; ++k)
        for (int sign = 1; sign >= -1; sign -= 2)
            for (int ws = -1; ws <= 1; ws += 2) {
                float coef[4] = {0.f, 0.f, 0.f, float(ws)};
                coef[k] = float(sign);
                f.outside[n++] = MakeForm(view.clip, coef);
            }
    f.usable = true;
    return f;
}
enum ChunkClass { kChunkSkip, kChunkNative, kChunkCpu };
// diagnosis: TE_GFX_CHUNK_DIAG=N logs the first N chunked builds' chunks (their class, the first inside form that failed
// with its bound) -- read at the first use (the Vita sets the environment from args.txt after the statics)
// diagnosis: TE_GFX_CHUNK_CHECK=N -- for the first N chunks sorted as dropped, their strips built on the CPU anyway: a
// triangle out of one would be a wrong drop (logged with the box and the vertices' clip lanes)
int ChunkCheckLeft() {
    static std::atomic<int> left{std::getenv("TE_GFX_CHUNK_CHECK") ? std::atoi(std::getenv("TE_GFX_CHUNK_CHECK")) : 0};
    return left.load() > 0 ? left.fetch_sub(1) > 0 : 0;
}
int ChunkDiagLeft() {
    static std::atomic<int> left{std::getenv("TE_GFX_CHUNK_DIAG") ? std::atoi(std::getenv("TE_GFX_CHUNK_DIAG")) : 0};
    return left.fetch_sub(1) > 0 ? 1 : 0;
}
// a box (x0 y0 z0 x1 y1 z1) sorted: the forms bounded over it (centre +- the extents through |a|), with a margin of 1e-4
// of the terms' magnitude so that no rounding of the title's own arithmetic could put a vertex on the other side
ChunkClass Classify(const ChunkForms& f, const std::array<float, 6>& box, int* failed = nullptr, float* failed_hi = nullptr) {
    const float c[3] = {0.5f * (box[0] + box[3]), 0.5f * (box[1] + box[4]), 0.5f * (box[2] + box[5])};
    const float e[3] = {0.5f * (box[3] - box[0]), 0.5f * (box[4] - box[1]), 0.5f * (box[5] - box[2])};
    const float cm = std::max(std::fabs(c[0]) + e[0], std::max(std::fabs(c[1]) + e[1], std::fabs(c[2]) + e[2]));
    auto bound = [&](const ChunkForms::Form& fm, float* lo, float* hi) {
        const float mid = fm.a[0] * c[0] + fm.a[1] * c[1] + fm.a[2] * c[2] + fm.b;
        const float half = std::fabs(fm.a[0]) * e[0] + std::fabs(fm.a[1]) * e[1] + std::fabs(fm.a[2]) * e[2];
        const float margin = 1e-4f * (fm.l1 * cm + std::fabs(fm.b));
        *lo = mid - half - margin;
        *hi = mid + half + margin;
    };
    // wholly inside the guard band (a chunk off the screen but inside it is drawn too: the GPU clips it whole, and a
    // native run then need not break there)
    bool inside = true;
    for (int q = 0; q < 10; ++q) {
        float lo, hi;
        bound(f.inside[q], &lo, &hi);
        if (!(hi < 0.f)) {
            inside = false;
            if (failed) *failed = q;
            if (failed_hi) *failed_hi = hi;
            break;
        }
    }
    if (inside) return kChunkNative;
    for (int p = 0; p < 6; ++p) {
        float lo1, hi1, lo2, hi2;
        bound(f.outside[2 * p], &lo1, &hi1);
        bound(f.outside[2 * p + 1], &lo2, &hi2);
        if (lo1 > 0.f && lo2 > 0.f) return kChunkSkip;
    }
    return kChunkCpu;
}
// Classify's bounds of one form over a box (the same centre, extents, margin)
struct BoxBounds {
    float c[3], e[3], cm;
    explicit BoxBounds(const std::array<float, 6>& box) {
        for (int k = 0; k < 3; ++k) {
            c[k] = 0.5f * (box[size_t(k)] + box[size_t(k) + 3]);
            e[k] = 0.5f * (box[size_t(k) + 3] - box[size_t(k)]);
        }
        cm = std::max(std::fabs(c[0]) + e[0], std::max(std::fabs(c[1]) + e[1], std::fabs(c[2]) + e[2]));
    }
    void Of(const ChunkForms::Form& fm, float* lo, float* hi) const {
        const float mid = fm.a[0] * c[0] + fm.a[1] * c[1] + fm.a[2] * c[2] + fm.b;
        const float half = std::fabs(fm.a[0]) * e[0] + std::fabs(fm.a[1]) * e[1] + std::fabs(fm.a[2]) * e[2];
        const float margin = 1e-4f * (fm.l1 * cm + std::fabs(fm.b));
        *lo = mid - half - margin;
        *hi = mid + half + margin;
    }
};
// The census: why a chunk Classify did not find wholly inside is not native -- 0 a corner at or behind the eye plane (the
// clip w), 1 in front but outside the guard band, 2 in the band but outside the native program's depth range
int ChunkWhy(const ChunkForms& f, const std::array<float, 6>& box) {
    const BoxBounds b(box);
    float lo, hi;
    b.Of(f.inside[6], &lo, &hi);
    if (!(hi < 0.f)) return 0;
    for (int q = 0; q < 6; ++q) {
        b.Of(f.inside[q], &lo, &hi);
        if (!(hi < 0.f)) return 1;
    }
    return 2;
}

// LitColours' colour of one vertex (strip s, vertex i of it: the same arithmetic) -- a CPU-built chunk's
std::array<uint8_t, 4> LitColour(const Part& part, const Lights& lights, size_t s, size_t i) {
    const auto& strip = part.mesh.strips[s];
    const bool normals = s < part.normals.size() && part.normals[s].size() == strip.vertices.size();
    const bool colours = s < part.colours.size() && part.colours[s].size() == strip.vertices.size();
    const auto& v = strip.vertices[i];
    std::array<uint8_t, 4> col = colours ? part.colours[s][i] : v.colour;
    float lit[3] = {lights.ambient[0], lights.ambient[1], lights.ambient[2]};
    if (normals) {
        const auto& nrm = part.normals[s][i];
        for (int l = 0; l < 3; ++l) {
            const auto& d = lights.directions[l];
            const float dot = std::max(0.f, nrm[0] * d[0] + nrm[1] * d[1] + nrm[2] * d[2]);
            for (int k = 0; k < 3; ++k) lit[k] += lights.colours[l][k] * dot;
        }
    }
    std::array<uint8_t, 4> o;
    for (int k = 0; k < 3; ++k) {
        const float value = float(col[k]) * lit[k];
        o[k] = uint8_t(std::min(255.f, std::max(0.f, value)));
    }
    o[3] = col[3];
    return o;
}
#endif

// ---- the native meshes prepared at their parts' load (PartPrep) -------------------------------------------------------------
// TE_GFX_MESH_AT_LOAD: unset = the Vita's default (on there, off elsewhere), 0 off, 1 on
// (=2, a diagnosis: the preparation queued but never run by a worker -- every one made by the batch at its part's ask)
bool MeshPrepByBatchOnly() {
    static const bool on = [] {
        const char* v = std::getenv("TE_GFX_MESH_AT_LOAD");
        return v && v[0] == '2';
    }();
    return on;
}
bool MeshAtLoad() {
    static const bool on = [] {
        const char* v = std::getenv("TE_GFX_MESH_AT_LOAD");
#if defined(__vita__)
        bool x = true;
#else
        bool x = false;
#endif
        if (v && *v) x = v[0] != '0';
        std::fprintf(stderr, "[te.gfx] native meshes: %s (TE_GFX_MESH_AT_LOAD)\n",
                     x ? "prepared at their parts' load on the builders (the list, its chunks, its deduplication); made at the first ask"
                       : "made from their lists when their parts first ask");
        return x;
    }();
    return on;
}
// TE_GFX_MODEL_DRAIN: 1 = every model part's load waits for the posted builds and the mesh batches (as before; the default
// off the Vita), 0 = only a model the map already holds (the Vita's default)
bool DrainEveryModel() {
    static const bool on = [] {
        const char* v = std::getenv("TE_GFX_MODEL_DRAIN");
#if defined(__vita__)
        bool x = false;
#else
        bool x = true;
#endif
        if (v && *v) x = v[0] != '0';
        return x;
    }();
    return on;
}
// The vertices MeshVertices lists for the part (three a triangle: every strip vertex from the third on that is not a
// restart), without listing them
uint32_t ListVertices(const Part& part) {
    uint32_t n = 0;
    const auto& prepared = part.prepared;
    for (const auto& range : prepared.strips)
        for (uint32_t i = 2; i < range.count; ++i)
            if (!prepared.restart[range.first + i]) n += 3;
    return n;
}
// The preparation (whichever thread runs it: PreparedMesh::Ensure / TryMake), under the game thread's environment
void ComputePartPrep(te::gfx::PreparedMesh& base) {
    PartPrep& p = static_cast<PartPrep&>(base);
    const Part& part = *p.part;
    std::fenv_t own;
    std::fegetenv(&own);
    const bool swap_env = std::memcmp(&own, &p.env, sizeof own) != 0;
    if (swap_env) std::fesetenv(&p.env);
#if defined(__vita__)
    std::vector<uint32_t> offsets;
    const std::vector<ps2e_draw_mesh_vertex> list = MeshVertices(part, &offsets);
#else
    const std::vector<ps2e_draw_mesh_vertex> list = MeshVertices(part);
#endif
    p.count = uint32_t(list.size());
    if (list.size() >= 3) {
#if defined(__vita__)
        MakeChunks(part, offsets, p.chunks);
#endif
        std::array<float, 6> box{1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        for (const auto& v : list) {
            box[0] = std::min(box[0], v.x); box[1] = std::min(box[1], v.y); box[2] = std::min(box[2], v.z);
            box[3] = std::max(box[3], v.x); box[4] = std::max(box[4], v.y); box[5] = std::max(box[5], v.z);
        }
        p.box = box;
        p.mesh = ps2e_draw_mesh_prepare(list.data(), int(list.size()));
    }
    if (swap_env) std::fesetenv(&own);
}
// The part's mesh prepared now on a worker (its load: the game thread)
void QueuePartPrep(Part& part) {
    TE_PROF_SCOPE("model: its native mesh's preparation queued");
    auto prep = std::make_shared<PartPrep>();
    prep->part = &part;
    prep->compute = ComputePartPrep;
    std::fegetenv(&prep->env);
    part.prep = prep;
    g_prepCensus.queued.fetch_add(1, std::memory_order_relaxed);
    if (MeshPrepByBatchOnly()) return;
    te::gfx::PostBackground([prep] {
        TE_PROF_SCOPE("native mesh: prepared at its load (a worker)");
        if (prep->TryMake()) g_prepCensus.ahead.fetch_add(1, std::memory_order_relaxed);
    });
}
// What FlushNativeMeshes set at the ask, set with the handle (ResolveMeshBatches): read only once `native` is
void InstallPrep(Part& part, PartPrep& prep) {
#if defined(__vita__)
    part.nativeCount = prep.count / 3 * 3;
    part.chunks = std::move(prep.chunks);
    part.chunked = !part.chunks.empty();
#endif
    part.nativeBox = prep.box;
    if (part.prep.get() == &prep) part.prep.reset();   // (its prepared form is the GPU's now)
}

// The engine's Build wants a textured strip state; the draw's own state is applied afterwards
ps2r::gs::DrawIr BuildState() {
    ps2r::gs::DrawIr st{};
    st.prim_type = 4;
    st.tex_enabled = true;
    st.off_x = te::gfx::GsOriginX();
    st.off_y = te::gfx::GsOriginY();
    return st;
}

// ---- diagnosis: the draws of ONE present logged, each with its state and screen box --------------------------------
// TE_GFX_DIAG_PRESENT=N logs every model instance's submodel draw recorded for present N (the frame dumped as frame_N):
// kind, model, submodel, clipped or not, bucket, shader type, texture, blend/alpha state, triangles, the screen box and the
// mean vertex colour. TE_GFX_DIAG_FLAT=1 drops the textures and paints by clip mode (unclipped red, clipped green, the
// override state's blue) so a frame shows WHICH geometry covers a region. With the present's log: each submodel's
// model-space box and each placed instance's world matrix; TE_GFX_DIAG_MESH=<model> dumps that model's strips' vertices;
// TE_GFX_DIAG_SKIP_BUCKET=a,b leaves those buckets' model draws out. The depth test's own evidence, for the logged present
// (whose draws are all built here, none native): TE_GFX_DIAG_VERTS=1 logs every emitted vertex ([te.gfx.vert]: bucket,
// texture, screen x y, the GS Z, STQ, colour, the draw's depth test / write and blend), TE_GFX_DIAG_MARK_BUCKET=a,b paints
// those buckets' draws opaque magenta (no texture, blend or alpha test; the depth test and write kept) -- the pixels each
// bucket's draws win, against the exact GS test over the logged vertices (tools/judges/icetex_judge.py).
extern "C" uint64_t te_graphics_presents();
struct Diag {
    long present = -1;
    bool flat = false;
    bool verts = false;            // TE_GFX_DIAG_VERTS
    uint32_t marked = 0;           // TE_GFX_DIAG_MARK_BUCKET=a,b: one bit per bucket
    bool read = false;
    void Read() {
        if (read) return;
        read = true;
        if (const char* p = std::getenv("TE_GFX_DIAG_PRESENT")) present = std::atol(p);
        flat = std::getenv("TE_GFX_DIAG_FLAT") != nullptr;
        verts = std::getenv("TE_GFX_DIAG_VERTS") != nullptr;
        for (const char* v = std::getenv("TE_GFX_DIAG_MARK_BUCKET"); v && *v;) {
            char* end = nullptr;
            const long b = std::strtol(v, &end, 10);
            if (end == v) break;
            if (b >= 0 && b < 32) marked |= 1u << b;
            v = *end == ',' ? end + 1 : end;
        }
    }
    bool Marked(uint32_t bucket) const { return bucket < 32 && ((marked >> bucket) & 1u); }
    bool Logging() { Read(); return present >= 0 && te_graphics_presents() + 1 == uint64_t(present); }
} g_diag;
struct DiagBox {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    double r = 0, g = 0, b = 0, a = 0;
    size_t n = 0;
    void Add(const ps2e_draw_vertex& v) {
        x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
        r += v.r; g += v.g; b += v.b; a += v.a; n++;
    }
};

// The built vertices (frame pixels, GS depth, STQ) become triangle-list draws under the material's state, appended
// straight into the bucket's open draw (the bucket merges consecutive unblended draws of one state -- a submodel's
// strips and fans, and the instances of one material after it -- into ONE draw in their order; blended states stay one
// draw per published strip/fan, as the backend may read the destination before each draw). The bucket owns the vertex
// storage; nothing is allocated per build once the buckets have grown.
// One built vertex as the draw records it: the GS's STQ as the program left it on a textured state (the engine interpolates
// S, T and Q over the screen and divides per pixel, so the mapping is perspective-correct -- divided here per vertex it was
// affine per triangle: the seams across the intro's water tiles at grazing angles)
ps2e_draw_vertex DrawVertex(const ps2r::gs::VertexRec& v, bool textured) {
    ps2e_draw_vertex o{};
    o.x = v.x;
    o.y = v.y;
    o.z = v.z;
    if (textured && v.q != 0.f) {
        o.u = v.s;
        o.v = v.t;
        o.q = v.q;
    }
    o.r = v.r;
    o.g = v.g;
    o.b = v.b;
    o.a = v.a;
    return o;
}

struct Emitter {
    uint32_t bucket = 0;
    ps2e_draw_state state{};
    bool textured = false;
    size_t triangles = 0;
    DiagBox* box = nullptr;        // diagnosis: the emitted vertices' box and mean colour
    int flatTint = -1;             // diagnosis: 0 unclipped, 1 clipped, 2 override state (TE_GFX_DIAG_FLAT)
    // the screen-copy pass (shader types 0x10 / 0x11; the crystal program vs_crys_dual's 0x1720 sub): the texture is
    // the frame's copy and each vertex samples it at its own screen position (the fraction of the frame) displaced by the
    // built STQ -- the pass's mesh carries (n_clip.x, n_clip.y, 1) * cornerValue as its STQ, the normal through the clip
    // matrix's three rows (StMtx) -- clamped to the copy, plus the shader's scroll (P2); the first Append asks for the copy
    bool screenCopy = false;
    bool copyPending = false;
    std::array<float, 2> copyScroll{};
    void Begin(uint32_t b, const ps2e_draw_state& st) {
        bucket = b;
        state = st;
        g_diag.Read();
        if (g_diag.flat) { state.texture = 0; state.blend = 0; state.alpha_test = 0; }
        if (g_diag.Marked(b)) { state.texture = 0; state.blend = 0; state.alpha_test = 0; state.tfx = PS2E_DRAW_TFX_MODULATE; }
        textured = state.texture != 0;
        screenCopy = false;
        copyPending = false;
    }
    void BeginScreenCopy(uint32_t b, const ps2e_draw_state& st, std::array<float, 2> scroll) {
        Begin(b, st);
        screenCopy = textured;
        copyPending = screenCopy;
        copyScroll = scroll;
    }
    void Append(const ps2r::gs::VertexRec* verts, size_t count) {
        count = count / 3 * 3;
        if (!count) return;
        Draw& d = te::gfx::OpenDraw(bucket, state, copyPending);
        copyPending = false;
        const size_t base = d.verts.size();
        d.verts.resize(base + count);
        const float invW = 1.f / float(std::max(1, te::gfx::ScreenW())), invH = 1.f / float(std::max(1, te::gfx::ScreenH()));
        for (size_t i = 0; i < count; ++i) {
            const auto& v = verts[i];
            ps2e_draw_vertex& o = d.verts[base + i];
            o = DrawVertex(v, textured && !screenCopy);
            if (screenCopy) {
                const float ds = v.q != 0.f ? v.s / v.q : 0.f, dt = v.q != 0.f ? v.t / v.q : 0.f;
                o.u = std::min(1.f, std::max(0.f, v.x * invW + ds)) + copyScroll[0];
                o.v = std::min(1.f, std::max(0.f, v.y * invH + dt)) + copyScroll[1];
            }
            if (box) box->Add(o);
            if (box && g_diag.verts)
                std::fprintf(stderr, "[te.gfx.vert] b%u tex %u v %u xy (%.2f %.2f) z %.0f stq (%g %g %g) rgba (%u %u %u %u) zt %d zw %d bl %d\n",
                             unsigned(bucket), unsigned(state.texture), unsigned(i), o.x, o.y, o.z, o.u, o.v, o.q, unsigned(o.r), unsigned(o.g),
                             unsigned(o.b), unsigned(o.a), int(state.depth_test), int(state.depth_write), int(state.blend));
            if (g_diag.Marked(bucket)) { o.r = 0xFF; o.g = 0x00; o.b = 0xFF; o.a = 0x80; }
            if (g_diag.flat && flatTint >= 0) {
                o.r = flatTint == 0 ? 0xC0 : 0x20;
                o.g = flatTint == 1 ? 0xC0 : 0x20;
                o.b = flatTint == 2 ? 0xC0 : 0x20;
                o.a = 0x80;
            }
        }
        te::gfx::CloseDraw(bucket);
        triangles += count / 3;
    }
    static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool fan) {
        (void)fan;
        static_cast<Emitter*>(user)->Append(verts, count);
    }
    // One build of a prepared submodel under this emitter's state; the triangles recorded are returned
    size_t Build(const Part& part, const Geometry::View& view, const std::array<uint8_t, 4>* colours, Geometry::Result* result) {
        const size_t before = triangles;
        const Geometry::Result r = te::gfx::MeshBuild(part.prepared, view, BuildState(), colours, &Sink, this);
        if (result) *result = r;
        return triangles - before;
    }
};
std::vector<std::array<uint8_t, 4>> g_colourScratch;   // the lit colours / the shadows' stamp colour per vertex

// The Emitter's appends on a worker (te::gfx::DeferredBuild): the same vertices, each Append's triangles one segment

struct Collector {
    te::gfx::DeferredBuild* out = nullptr;
    bool textured = false;
    size_t triangles = 0;
    void Append(const ps2r::gs::VertexRec* verts, size_t count) {
        count = count / 3 * 3;
        if (!count) return;
        const size_t base = out->verts.size();
        out->verts.resize(base + count);
        for (size_t i = 0; i < count; ++i) out->verts[base + i] = DrawVertex(verts[i], textured);
        out->segments.push_back(uint32_t(count));
        triangles += count / 3;
    }
    static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool fan) {
        (void)fan;
        static_cast<Collector*>(user)->Append(verts, count);
    }
    // a native mesh range in this place (the Vita's chunked builds: ExpandBucket draws it as a mesh draw)
    void AppendMesh(uint32_t mesh, const ps2e_draw_mesh_params& mp) {
        out->segments.push_back(0u);
        out->meshes.push_back(te::gfx::DeferredBuild::MeshSegment{mesh, mp});
    }
};
#if defined(__vita__)
// the shadow builds' output is read for its vertices only (ShadowEmitter::Convert): no per-draw vertex hash / box
[[maybe_unused]] const bool g_shadowSnapshotsOff = (title::ShadowGeometry::SetSnapshots(false), true);
#endif
// The triangles the workers built, folded into the scene's census on the game thread
std::atomic<uint64_t> g_deferredModelTriangles{0}, g_deferredSkinTriangles{0};
// Whether this draw's build may run on a worker: workers running, nothing logged or tinted for diagnosis
bool Deferrable(bool logging) {
    return !logging && !g_diag.flat && te::gfx::DeferringBuilds();
}

#if defined(__vita__)
// A clipped instance's part (or a sky part) through its native mesh's chunks, on a worker (see MakeChunks): each chunk
// sorted by its box (Classify) -- dropped, drawn as a range of the native mesh (consecutive native chunks one range; a run
// under ChunkMinNative triangles built on the CPU with its neighbours instead), or built on the CPU -- in the chunks'
// order. `chunked` false: the part is one chunk (its whole mesh, its whole build). `kind`: the census row (0 models, 1 sky).
void ChunkedBuild(te::gfx::DeferredBuild& build, const Part& part, bool chunked, uint32_t native, const Geometry::View& view,
                  const std::optional<Lights>& lights, float ox, float oy, const ps2r::gs::DrawIr& state, bool textured, int kind) {
    static const unsigned short profChunked = te::prof::Register("chunked build (clipped / sky)");
    te::prof::Scope profScope(profChunked);
    // (the sky's chunks are dropped or CPU-built, never drawn natively: its native draws were visibly wrong in the title
    // intro -- picture 2530 of the title run: the purple backdrop missing where a native run of a sky part drew --
    // cause not found; the drops and the CPU chunks alone are verified there: the same run without native, thick 0)
    const ChunkForms forms = FormsOf(view);
    Collector collector{&build, textured};
    ps2e_draw_mesh_params mp{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) mp.screen[r * 4 + c] = view.screen[size_t(r)][size_t(c)];
    mp.offset[0] = ox;
    mp.offset[1] = oy;
    mp.scroll[0] = view.scroll[0];
    mp.scroll[1] = view.scroll[1];
    mp.box[0] = mp.box[1] = -1e9f;
    mp.box[2] = mp.box[3] = 1e9f;
    if (lights) {
        mp.lit = 1;
        for (int l = 0; l < 3; ++l)
            for (int k = 0; k < 3; ++k) {
                mp.light_dir[l][k] = lights->directions[size_t(l)][size_t(k)];
                mp.light_colour[l][k] = lights->colours[size_t(l)][size_t(k)];
            }
        for (int k = 0; k < 3; ++k) mp.ambient[k] = lights->ambient[size_t(k)];
    }
    ChunkCensus& census = g_chunkCensus[kind];
    census.parts.fetch_add(1, std::memory_order_relaxed);
    uint64_t native_tris = 0, cpu_tris = 0, skipped_tris = 0, draws = 0, runs = 0;
    // the builders' census: the CPU triangles by why, the CPU vertices, the fans' and all vertices out
    uint64_t why_tris[5] = {0, 0, 0, 0, 0}, cpu_vertices = 0;
    struct CensusSink {
        Collector* collector;
        uint64_t fans = 0, out = 0;
        static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool fan) {
            auto* s = static_cast<CensusSink*>(user);
            if (fan) s->fans += count;
            s->out += count;
            Collector::Sink(s->collector, verts, count, fan);
        }
    } sink{&collector};
    const bool diag = ChunkDiagLeft() != 0;
    if (!chunked) {
        // one chunk: the whole part (its whole build when it is not wholly inside the guard band)
        ChunkClass k = forms.usable ? Classify(forms, part.nativeBox) : kChunkCpu;
        const bool skyRule = kind == 1 && k == kChunkNative;
        if (kind == 1 && k == kChunkNative) k = kChunkCpu;   // the sky: never native (above)
        const uint64_t tris = part.nativeCount / 3;
        if (k == kChunkNative) {
            mp.first = 0;
            mp.count = int(part.nativeCount);
            collector.AppendMesh(native, mp);
            native_tris += tris;
            draws++;
        } else if (k == kChunkSkip) {
            skipped_tris += tris;
        } else {
            thread_local std::vector<std::array<uint8_t, 4>> lit;
            if (lights) LitColours(part, *lights, lit);
            te::gfx::MeshBuild(part.prepared, view, state, lights ? lit.data() : nullptr, &CensusSink::Sink, &sink);
            cpu_tris += tris;
            runs++;
            why_tris[skyRule ? 4 : forms.usable ? ChunkWhy(forms, part.nativeBox) : 1] += tris;
            cpu_vertices += part.prepared.vertex_count;
        }
    } else {
        // the chunks' classes, then the short native runs made CPU ones
        thread_local std::vector<uint8_t> cls;
        thread_local std::vector<uint8_t> pre;   // (the census) the class Classify gave, before the sky's and the run rules
        const size_t n = part.chunks.size();
        cls.resize(n);
        pre.resize(n);
        for (size_t c = 0; c < n; ++c) {
            cls[c] = uint8_t(forms.usable ? Classify(forms, part.chunks[c].box) : kChunkCpu);
            pre[c] = cls[c];
            if (kind == 1 && cls[c] == kChunkNative) cls[c] = kChunkCpu;   // the sky: never native (above)
        }
        std::string before;
        if (diag)
            for (size_t c = 0; c < n; ++c) before += cls[c] == kChunkSkip ? '-' : cls[c] == kChunkNative ? 'N' : 'c';
        for (size_t c = 0; c < n;) {
            if (cls[c] != kChunkNative) { ++c; continue; }
            size_t e = c;
            uint32_t tris = 0;
            while (e < n && cls[e] == kChunkNative && (e == c || part.chunks[e].first == part.chunks[e - 1].first + part.chunks[e - 1].count)) {
                tris += part.chunks[e].count / 3;
                ++e;
            }
            if (tris < ChunkMinNative())
                for (size_t k = c; k < e; ++k) cls[k] = kChunkCpu;
            c = e;
        }
        if (diag) {
            std::string line;
            for (size_t c = 0; c < n; ++c) line += cls[c] == kChunkSkip ? '-' : cls[c] == kChunkNative ? 'N' : 'c';
            std::fprintf(stderr, "[te.gfx.chunk] %s part (%u triangles, %u chunks): %s (classes before the run rule: %s; guard %g %g %g %g)\n",
                         kind ? "sky" : "model", unsigned(part.nativeCount / 3), unsigned(n), line.c_str(), before.c_str(),
                         double(view.guard[0]), double(view.guard[1]), double(view.guard[2]), double(view.guard[3]));
        }
        // the CPU chunks of a run as strips of their own in a scratch prepared form (their vertices copied from the part's)
        thread_local te::gfx::MeshPrepared scratch;
        thread_local std::vector<std::array<uint8_t, 4>> lit;
        auto cpu_flush = [&] {
            if (scratch.strips.empty()) return;
            scratch.vertex_count = scratch.positions.size();
            cpu_vertices += scratch.vertex_count;
            te::gfx::MeshBuild(scratch, view, state, lights ? lit.data() : nullptr, &CensusSink::Sink, &sink);
            scratch.strips.clear();
            scratch.positions.clear();
            scratch.stq.clear();
            scratch.colours.clear();
            scratch.restart.clear();
            lit.clear();
            runs++;
        };
        const auto& prepared = part.prepared;
        uint32_t run_first = 0, run_count = 0;
        auto native_flush = [&] {
            if (!run_count) return;
            mp.first = int(run_first);
            mp.count = int(run_count);
            collector.AppendMesh(native, mp);
            draws++;
            run_count = 0;
        };
        for (size_t c = 0; c < n; ++c) {
            const NativeChunk& ch = part.chunks[c];
            const uint64_t tris = ch.count / 3;
            if (cls[c] == kChunkSkip) {
                skipped_tris += tris;
                if (ChunkCheckLeft()) {
                    // the self-check: the chunk's own build must give nothing
                    te::gfx::MeshPrepared one;
                    const auto& range = prepared.strips[ch.strip];
                    one.strips.push_back({0u, uint32_t(ch.v1 - ch.v0)});
                    for (uint32_t v = ch.v0; v < ch.v1; ++v) {
                        const size_t at = range.first + v;
                        one.positions.push_back(prepared.positions[at]);
                        one.stq.push_back(prepared.stq[at]);
                        one.colours.push_back(prepared.colours[at]);
                        one.restart.push_back(prepared.restart[at]);
                    }
                    one.vertex_count = one.positions.size();
                    struct Count { size_t n = 0; static void Sink(void* u, const ps2r::gs::VertexRec*, size_t k, bool) { static_cast<Count*>(u)->n += k; } } cnt;
                    te::gfx::MeshBuild(one, view, state, nullptr, &Count::Sink, &cnt);
                    if (cnt.n) {
                        std::fprintf(stderr, "[te.gfx.chunk] WRONG DROP (%s): chunk %u+%u box (%g %g %g)-(%g %g %g) built %u vertices\n",
                                     kind ? "sky" : "model", ch.first, ch.count, double(ch.box[0]), double(ch.box[1]), double(ch.box[2]),
                                     double(ch.box[3]), double(ch.box[4]), double(ch.box[5]), unsigned(cnt.n));
                        for (uint32_t v = 0; v < uint32_t(one.positions.size()) && v < 6; ++v) {
                            const auto& q = one.positions[v];
                            float lane[4];
                            for (int k = 0; k < 4; ++k)
                                lane[k] = view.clip[3][size_t(k)] + view.clip[0][size_t(k)] * q[0] + view.clip[1][size_t(k)] * q[1] +
                                          view.clip[2][size_t(k)] * q[2];
                            std::fprintf(stderr, "[te.gfx.chunk]   v%u (%g %g %g) clip (%g %g %g %g)\n", v, double(q[0]), double(q[1]),
                                         double(q[2]), double(lane[0]), double(lane[1]), double(lane[2]), double(lane[3]));
                        }
                    }
                }
                continue;
            }
            if (cls[c] == kChunkNative) {
                cpu_flush();
                native_tris += tris;
                if (run_count && run_first + run_count == ch.first) run_count += ch.count;
                else {
                    native_flush();
                    run_first = ch.first;
                    run_count = ch.count;
                }
                continue;
            }
            native_flush();
            cpu_tris += tris;
            why_tris[pre[c] == kChunkNative ? (kind == 1 ? 4 : 3) : forms.usable ? ChunkWhy(forms, ch.box) : 1] += tris;
            const auto& range = prepared.strips[ch.strip];
            scratch.strips.push_back({uint32_t(scratch.positions.size()), uint32_t(ch.v1 - ch.v0)});
            for (uint32_t v = ch.v0; v < ch.v1; ++v) {
                const size_t at = range.first + v;
                scratch.positions.push_back(prepared.positions[at]);
                scratch.stq.push_back(prepared.stq[at]);
                scratch.colours.push_back(prepared.colours[at]);
                scratch.restart.push_back(prepared.restart[at]);
                if (lights) lit.push_back(LitColour(part, *lights, ch.strip, v));
            }
        }
        cpu_flush();
        native_flush();
    }
    census.native_tris.fetch_add(native_tris, std::memory_order_relaxed);
    census.cpu_tris.fetch_add(cpu_tris, std::memory_order_relaxed);
    census.skipped_tris.fetch_add(skipped_tris, std::memory_order_relaxed);
    census.native_draws.fetch_add(draws, std::memory_order_relaxed);
    census.cpu_runs.fetch_add(runs, std::memory_order_relaxed);
    BuildersCensus& bc = g_builders;
    bc.cpu_behind[kind].fetch_add(why_tris[0], std::memory_order_relaxed);
    bc.cpu_guard[kind].fetch_add(why_tris[1], std::memory_order_relaxed);
    bc.cpu_depth[kind].fetch_add(why_tris[2], std::memory_order_relaxed);
    bc.cpu_run[kind].fetch_add(why_tris[3], std::memory_order_relaxed);
    bc.cpu_sky[kind].fetch_add(why_tris[4], std::memory_order_relaxed);
    bc.cpu_vertices[kind].fetch_add(cpu_vertices, std::memory_order_relaxed);
    bc.fan_vertices[kind].fetch_add(sink.fans, std::memory_order_relaxed);
    bc.out_vertices[kind].fetch_add(sink.out, std::memory_order_relaxed);
    g_deferredModelTriangles += collector.triangles;
}
#endif

// Whether a clipped instance may still be drawn as a native mesh: every corner of the part's box in front of the eye and
// inside the depth range (the box's screen depth then is too: a ratio of two linear functions with a positive
// denominator is extreme at the corners), so the title's clipping would only cut the triangles at the screen's sides --
// the device's own viewport and scissor do that
bool NativeInFront(const Part& part, const Geometry::View& view) {
    if (!part.native) return false;
    const auto& b = part.nativeBox;
    for (int c = 0; c < 8; ++c) {
        const float px = b[(c & 1) ? 3 : 0], py = b[(c & 2) ? 4 : 1], pz = b[(c & 4) ? 5 : 2];
        float o[4];
        for (int k = 0; k < 4; ++k)
            o[k] = view.screen[3][size_t(k)] + view.screen[0][size_t(k)] * px + view.screen[1][size_t(k)] * py +
                   view.screen[2][size_t(k)] * pz;
        if (!(o[3] > 0.f)) return false;
        const float z = o[2] / o[3];
        if (!(z >= 0.f && z < 2147483648.f)) return false;
    }
    return true;
}


// One instance of a model: every submodel the model and its materials both have, through the view
struct InstanceState {
    const ps2e_draw_state* overrideState = nullptr;   // the shadows' stamp state (else each submodel's material)
    uint32_t overrideBucket = 0;
    const Lights* lights = nullptr;
    std::optional<Vector> eye;                        // the camera in the model's space (the env-map passes' half vector)
};
// the screen-copy pass's mesh -- the part's strips with the STQ replaced by the clip-space normal's x / y (the
// normal through the clip matrix's three rows, as the crystal program's StMtx) times the shader's cornerValue, q = 1;
// prepared per instance (a scratch: the crystals are a few thousand vertices)
struct ScreenCopyScratch {
    Geometry::Mesh mesh;
    te::gfx::MeshPrepared prepared;
} g_screenCopyScratch;
bool PrepareScreenCopyMesh(const Part& part, const Matrix& clip, float cornerValue) {
    auto& sc = g_screenCopyScratch;
    sc.mesh = part.mesh;
    for (size_t st = 0; st < sc.mesh.strips.size(); ++st) {
        auto& strip = sc.mesh.strips[st];
        const bool normals = st < part.normals.size() && part.normals[st].size() == strip.vertices.size();
        for (size_t i = 0; i < strip.vertices.size(); ++i) {
            auto& v = strip.vertices[i];
            float dx = 0.f, dy = 0.f;
            if (normals) {
                const auto& n = part.normals[st][i];
                dx = (n[0] * clip[0][0] + n[1] * clip[1][0] + n[2] * clip[2][0]) * cornerValue;
                dy = (n[0] * clip[0][1] + n[1] * clip[1][1] + n[2] * clip[2][1]) * cornerValue;
            }
            v.stq = {std::isfinite(dx) ? dx : 0.f, std::isfinite(dy) ? dy : 0.f, 1.f};
        }
    }
    return te::gfx::MeshPrepare(sc.mesh, sc.prepared) == Geometry::Result::Ok;
}

// the env-map pass's mesh (shader types 0x0C / 0x0F; the VU1 library's TexturedTristrip::RunEnvCall): per vertex
// the half vector h = normalize(eye - pos) + n in the model's space, env = (h / 2) through the clip matrix's three rows, the
// sphere map u = 0.5 * envn.x + 0.5, v = -0.5 * envn.y + 0.5 over the normalised env, clamped to [0, 1], q = 1 (the shader's
// scroll is the view's payload, added by the build as the micro added it after the clamp)
bool PrepareEnvMesh(const Part& part, const Matrix& clip, const Vector& eye, ScreenCopyScratch& sc = g_screenCopyScratch) {
    sc.mesh = part.mesh;
    for (size_t st = 0; st < sc.mesh.strips.size(); ++st) {
        auto& strip = sc.mesh.strips[st];
        const bool normals = st < part.normals.size() && part.normals[st].size() == strip.vertices.size();
        for (size_t i = 0; i < strip.vertices.size(); ++i) {
            auto& v = strip.vertices[i];
            float u = 0.5f, t = 0.5f;
            if (normals) {
                const auto& n = part.normals[st][i];
                float e[3], h2[3], env[3];
                float d2 = 0.f;
                for (int k = 0; k < 3; ++k) { e[k] = eye[k] - v.position[k]; d2 += e[k] * e[k]; }
                const float rsq = d2 > 0.f ? 1.f / std::sqrt(d2) : 0.f;
                for (int k = 0; k < 3; ++k) h2[k] = (e[k] * rsq + n[k]) * 0.5f;
                for (int k = 0; k < 3; ++k) env[k] = h2[0] * clip[0][k] + h2[1] * clip[1][k] + h2[2] * clip[2][k];
                const float d2v = env[0] * env[0] + env[1] * env[1] + env[2] * env[2];
                const float rsqv = d2v > 0.f ? 1.f / std::sqrt(d2v) : 0.f;
                u = 0.5f * env[0] * rsqv + 0.5f;
                t = -0.5f * env[1] * rsqv + 0.5f;
            }
            u = std::isfinite(u) ? std::min(1.f, std::max(0.f, u)) : 0.5f;
            t = std::isfinite(t) ? std::min(1.f, std::max(0.f, t)) : 0.5f;
            v.stq = {u, t, 1.f};
        }
    }
    return te::gfx::MeshPrepare(sc.mesh, sc.prepared) == Geometry::Result::Ok;
}
bool EnvType(uint32_t type) { return type == 0x0C || type == 0x0F; }

// The shader types a later pass of a material is drawn as (TR: a material's shaders are its passes over the same strips,
// the VU1 dispatcher walking them in order): the plain and lit strip families with the mesh's own STQ, and the screen
// copies. Types whose program derives other coordinates (0x0C / 0x0F env maps, 0x18 distortion, the cloth 0x17 / 0x1A...)
// are not drawn as later passes (the first shader is always drawn, as before)
bool PassDrawable(uint32_t type, bool haveEye) {
    switch (type) {
        case 0x00: case 0x01: case 0x02: case 0x04: case 0x0A: case 0x0D: case 0x0E: case 0x10: case 0x11: case 0x12:
        case 0x13: case 0x15: case 0x16: case 0x19: case 0x1B: case 0x1E: return true;
        case 0x0C: case 0x0F: return haveEye;   // the env-map seconds, when the instance's eye is known
        default: return false;
    }
}
bool ScreenCopyType(uint32_t type) { return type == 0x10 || type == 0x11; }

void DrawInstance(const te_gfx_rigid_model* rigid, const Geometry::View& view, const InstanceState& inst, uint64_t& counter) {
    auto& scene = te::gfx::Scene();
    const SceneOptions& opts = Opts();
    // (the fast arm folds the workers' triangles once a picture, FlushNativeMeshes: a 64-bit exclusive exchange per instance)
    if (!opts.fast) scene.model_triangles += g_deferredModelTriangles.exchange(0, std::memory_order_relaxed);
    if (opts.noDraw) return;   // diagnosis: culling without the draws
    // Diagnosis: TE_GFX_NO_PASSES=1 draws the first shader only (the single-pass picture: the judge's reference arm)
    const bool firstPassOnly = opts.firstPassOnly;
    const Mem mem;
    const te_gfx_model* data = rigid->model ? mem.Get<te_gfx_model>(rigid->model) : nullptr;
    if (!data) return;
    Model* found = nullptr;
    if (opts.fast) {
        found = FindModel(rigid->model);
    } else if (auto it = g_models.find(rigid->model); it != g_models.end()) {
        found = &it->second;
    }
    if (!found) {
        scene.submodels_missing++;
        return;
    }
    const Model& entry = *found;
    const uint32_t count = std::min<uint32_t>(rigid->count, data->subModelCount);
    const uint32_t* materials = count ? reinterpret_cast<const uint32_t*>(mem.Bytes(rigid->materials, 4 * size_t(count))) : nullptr;
    // the GS origin the native draws take off (the frame's: it does not change within an instance)
    const float originX = te::gfx::GsOriginX(), originY = te::gfx::GsOriginY();
    // the Vita's compact mesh records (te::gfx::RecordMeshCompact): decided once per instance (the tag, the presenter)
    const bool compact = opts.fast && !inst.overrideState && te::gfx::CompactMeshes();
    const bool compactCheck = compact && te::gfx::CompactCheck();
    static Emitter emitter;
    for (uint32_t i = 0; i < count && i < entry.parts.size(); ++i) {
        const Part& part = entry.parts[i];
        if (!part.decoded) continue;
        if (part.meshState == 3) ResolveMeshBatches();   // its mesh, made since the last hand-over: the handle first
        const uint32_t resource = materials ? materials[i] : 0;
        const uint32_t* materialAddress = (!inst.overrideState && resource) ? mem.Get<uint32_t>(resource + 8) : nullptr;   // MaterialResource::material
        // the material and its first shader's state: cached by address in the fast arm (CachedMaterial: the same answers)
        MaterialView material;
        ps2e_draw_state firstScratch;   // (filled when the answer is not a cache slot's)
        const ps2e_draw_state* firstState = &firstScratch;
        uint32_t firstPool = 0;         // the first shader's state's pool entry (compact records)
        if (opts.fast && !inst.overrideState) {
            firstState = CachedMaterial(mem, materialAddress ? *materialAddress : 0, material, firstScratch, compact ? &firstPool : nullptr);
        } else {
            material = te::gfx::ReadMaterial(materialAddress ? *materialAddress : 0);
            if (!inst.overrideState) firstScratch = te::gfx::StateOf(material);
            else firstScratch = ps2e_draw_state{};   // (never read: the override state is the pass's)
        }
        // the passes: the override state is one pass; else the material's shaders in order (the first always, the later
        // ones when their type is drawable as a pass -- the crystals' [0x10 screen copy] [0x02 lit] pairs)
        const uint32_t passes = inst.overrideState || firstPassOnly || !material.material
                                    ? 1u : std::max<uint32_t>(1u, std::min<uint32_t>(material.material->shaderCount, TE_GFX_MAX_SHADERS));
        for (uint32_t pass = 0; pass < passes; ++pass) {
            // the pass's state: the override's, the first shader's (the cache slot's: no copy), or this pass's own copy
            // (a later shader's, or the screen copy's changes)
            ps2e_draw_state own;
            const ps2e_draw_state* sp = nullptr;
            uint32_t bucket;
            bool lit = false, screenCopy = false, envMap = false;
            MaterialView m = material;
            // the submodel's view is the instance's with the material's UV scroll (StepAnimations) as its payload (the screen
            // copies keep none): made below for the builds only -- the native draws read the screen matrix and the scroll
            std::array<float, 2> subScroll = view.scroll;
            std::array<float, 2> scroll{0.f, 0.f};
            if (inst.overrideState) {
                sp = inst.overrideState;
                bucket = inst.overrideBucket;
            } else {
                if (pass) {
                    m = te::gfx::ShaderView(material, pass);
                    if (!m.shader || !PassDrawable(m.shader->type, inst.eye.has_value())) continue;
                    own = te::gfx::StateOf(m);
                    sp = &own;
                } else {
                    sp = firstState;
                }
                bucket = m.material ? m.bucket : BucketOpaque;
                // The lit shader families: 02 (lit tristrip), 04 (skinned lit), 0C/0F (env map seconds), 10 (crystal), 16
                lit = inst.lights && m.shader && (m.shader->type == 0x02 || m.shader->type == 0x10 || m.shader->type == 0x16 ||
                                                  (pass && EnvType(m.shader->type)));
                envMap = pass && m.shader && EnvType(m.shader->type) && inst.eye.has_value();
                if (m.shader && FiniteBits(m.shader->scroll, 2)) scroll = {m.shader->scroll[0], m.shader->scroll[1]};
                screenCopy = m.shader && ScreenCopyType(m.shader->type);
                if (screenCopy) {
                    // the frame as the texture (MakeScreenCopySettings: the copy, clamped, MODULATE, its alpha taken)
                    if (sp != &own) {
                        own = *sp;
                        sp = &own;
                    }
                    own.texture = PS2E_DRAW_TEXTURE_FRAME_COPY;
                    own.tfx = PS2E_DRAW_TFX_MODULATE;
                    own.tcc = 1;
                    own.wrap_u = own.wrap_v = PS2E_DRAW_WRAP_CLAMP;
                } else {
                    subScroll = scroll;
                }
            }
            const ps2e_draw_state& state = *sp;
            // diagnosis: TE_GFX_DIAG_SKIP_BUCKET=6,8 leaves those buckets' model draws out (which layer of a composite is which)
            if (!opts.skipBuckets.empty() && opts.skipBuckets.find("," + std::to_string(bucket) + ",") != std::string::npos) continue;
            // an unclipped instance's plain pass (or a clipped one wholly in front: NativeInFront): the part's native mesh
            // through the screen matrix, nothing built here (the first picture that draws a part asks for its mesh and
            // builds on the CPU meanwhile). The blended passes with a UV scroll -- the sea, the hub's additive light shafts,
            // mist; 70 a picture in the Mech arena -- too (te::gfx::SetNativeScrolled; TE_GFX_NATIVE_SCROLLED=0 builds them on
            // the CPU): they were once kept CPU-built because drawn natively they "sampled their texture elsewhere", which
            // is what a recycled Draw's stale native-mesh handle did (an old mesh drawn under another draw's texture;
            // ExpandBucket resets `mesh` since) -- the shader's (S + scroll) / w is MeshGeometry's, and stamped recordings
            // of the hub and the arena show the two paths' pictures alike within the recordings' noise
            const bool logging = g_diag.Logging();
            const bool scrolled = subScroll[0] != 0.f || subScroll[1] != 0.f;
            const bool scrolledBlendCpu = state.blend && scrolled && !g_nativeScrolled.load(std::memory_order_relaxed);
            if ((!view.clipped || NativeInFront(part, view)) && !screenCopy && !envMap && !logging && !g_diag.flat &&
                !scrolledBlendCpu && te::gfx::NativeMeshes()) {
                if (part.meshState == 0) {
                    part.meshState = 1;
                    g_meshRequests.emplace_back(rigid->model, i);
                }
                if (part.native) {
                    // the record's parameters written in place (every field a mesh draw reads: the screen matrix, the GS
                    // origin, the scroll, the lights when lit -- an unlit draw's light rows are never read --, the box, the
                    // whole mesh), no zeroed stack struct copied into it. The Vita: a COMPACT record when the state is the
                    // first shader's pooled one (te::gfx::RecordMeshCompact): the header and these parameters only; the
                    // drawing thread copies the state, the box and the whole-mesh range in when it expands the bucket
                    const uint32_t pool = sp == firstState ? firstPool : 0;
                    if (compact && !pool) te::gfx::NoteWholeMeshRecord();
                    ps2e_draw_mesh_params& mp = pool ? te::gfx::RecordMeshCompact(bucket, pool, state, part.native)
                                                     : te::gfx::RecordMeshInPlace(bucket, state, part.native);
                    std::memcpy(mp.screen, view.screen.data(), sizeof mp.screen);   // (std::array rows: 16 floats, row-major)
                    mp.offset[0] = originX;
                    mp.offset[1] = originY;
                    mp.scroll[0] = subScroll[0];
                    mp.scroll[1] = subScroll[1];
                    if (!pool || compactCheck) {
                        // the screen box: the whole frame. Its one reader is the device's destination copy (the rectangle a
                        // blend reading the destination copies), which no ps2e_draw state takes (its textures are decoded
                        // ones, FBA only unblended, colour clamp on, a 32-bit frame) -- the eight corners through the
                        // matrix per part were game-thread time spent on nothing
                        mp.box[0] = mp.box[1] = -1e9f;
                        mp.box[2] = mp.box[3] = 1e9f;
                        mp.first = 0;
                        mp.count = 0;
                    }
                    if (lit) {
                        mp.lit = 1;
                        const Lights& L = *inst.lights;
                        for (int l = 0; l < 3; ++l)
                            for (int k = 0; k < 3; ++k) {
                                mp.light_dir[l][k] = L.directions[size_t(l)][size_t(k)];
                                mp.light_colour[l][k] = L.colours[size_t(l)][size_t(k)];
                            }
                        for (int k = 0; k < 3; ++k) mp.ambient[k] = L.ambient[size_t(k)];
                    } else {
                        mp.lit = 0;
                    }
                    g_seams.nativeDraws++;
                    if (pass) scene.material_passes++;
                    continue;
                }
            }
            Geometry::View sub = view;   // the submodel's view (the builds')
            sub.scroll = subScroll;
            // a plain build runs on a worker (the placeholder takes its place in the bucket), its lighting with it
            const bool deferred = !screenCopy && Deferrable(logging);
            // the Vita: a clipped part not wholly in front -- its native mesh's chunks sorted by the build (ChunkedBuild);
            // the mesh is asked for at the part's first such draw (the CPU builds the whole part meanwhile)
            uint32_t chunkNative = 0;
            bool chunkChunked = false;
#if defined(__vita__)
            if (deferred && view.clipped && !envMap && !logging && !g_diag.flat && !scrolledBlendCpu && te::gfx::NativeMeshes() &&
                g_chunkedNative.load(std::memory_order_relaxed)) {
                if (part.meshState == 0) {
                    part.meshState = 1;
                    g_meshRequests.emplace_back(rigid->model, i);
                }
                chunkNative = part.native;
                chunkChunked = part.native && part.chunked;
                if (!part.native) g_chunkCensus[0].whole_cpu.fetch_add(1, std::memory_order_relaxed);
            }
#endif
            const std::array<uint8_t, 4>* colours = nullptr;
            if (lit && !deferred) {
                LitColours(part, *inst.lights, g_colourScratch);
                colours = g_colourScratch.data();
            }
            if (screenCopy) {
                if (!PrepareScreenCopyMesh(part, view.clip, m.shader->extra[0])) continue;
                emitter.BeginScreenCopy(bucket, state, scroll);
            } else {
                if (envMap && !deferred && !PrepareEnvMesh(part, view.clip, *inst.eye)) continue;
                emitter.Begin(bucket, state);
            }
            emitter.flatTint = inst.overrideState ? 2 : view.clipped ? 1 : 0;
            DiagBox box;
            emitter.box = logging ? &box : nullptr;
            if (deferred) {
                te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, emitter.state);
                std::optional<Lights> lights;
                if (lit) lights = *inst.lights;
                // an env-map pass: its reflection STQ prepared on the worker too, into the worker's own scratch
                std::optional<Vector> envEye;
                if (envMap) envEye = *inst.eye;
#if defined(__vita__)
                if (chunkNative && opts.repeatCensus)
                    NoteBuildInputs(1, ChunkKey(&part, chunkChunked, chunkNative, sub, lights ? &*lights : nullptr, te::gfx::GsOriginX(),
                                                te::gfx::GsOriginY(), emitter.textured));
#endif
                te::gfx::PostBuild(build, [&build, mesh = &part, sub, lights, envEye, clip = view.clip, state = BuildState(),
                                           textured = emitter.textured, model = rigid->model, i, pass, chunkNative, chunkChunked,
                                           ox = te::gfx::GsOriginX(), oy = te::gfx::GsOriginY()] {
#if defined(__vita__)
                    if (chunkNative) {
                        ChunkedBuild(build, *mesh, chunkChunked, chunkNative, sub, lights, ox, oy, state, textured, 0);
                        return;
                    }
#else
                    (void)chunkNative; (void)chunkChunked; (void)ox; (void)oy;
#endif
                    thread_local std::vector<std::array<uint8_t, 4>> litScratch;
                    thread_local ScreenCopyScratch envScratch;
                    if (envEye) {
                        TE_PROF_SCOPE("rigid env-map prepare");
                        if (!PrepareEnvMesh(*mesh, clip, *envEye, envScratch)) return;
                    }
                    if (lights) {
                        TE_PROF_SCOPE("rigid LitColours");
                        LitColours(*mesh, *lights, litScratch);
                    }
                    Collector collector{&build, textured};
                    static const unsigned short profClipped = te::prof::Register("rigid MeshBuild (clipped)"), profPlain = te::prof::Register("rigid MeshBuild");
                    te::prof::Scope profScope(sub.clipped ? profClipped : profPlain);
                    const Geometry::Result r = te::gfx::MeshBuild(envEye ? envScratch.prepared : mesh->prepared, sub, state,
                                                                  lights ? litScratch.data() : nullptr, &Collector::Sink, &collector);
                    if (r != Geometry::Result::Ok) {
                        static std::atomic<int> said{0};
                        if (said++ < 8) std::fprintf(stderr, "[te.gfx] model %#x part %u pass %u: Build refused (%d)\n", model, i, pass, int(r));
                    }
                    g_deferredModelTriangles += collector.triangles;
                });
                emitter.box = nullptr;
                if (pass) scene.material_passes++;
                continue;
            }
            Geometry::Result result;
            size_t triangles;
            if (screenCopy || envMap) {
                const size_t before = emitter.triangles;
                result = te::gfx::MeshBuild(g_screenCopyScratch.prepared, sub, BuildState(), colours, &Emitter::Sink, &emitter);
                triangles = emitter.triangles - before;
            } else {
                triangles = emitter.Build(part, sub, colours, &result);
            }
            emitter.box = nullptr;
            if (result != Geometry::Result::Ok) {
                static int said = 0;
                if (said++ < 8) std::fprintf(stderr, "[te.gfx] model %#x part %u pass %u: Build refused (%d)\n", rigid->model, i, pass, int(result));
                continue;
            }
            if (logging) {
                std::fprintf(stderr,
                             "[te.gfx.diag] present %ld %s model %#x part %u/%u pass %u %s bucket %u shader %#x tex %u tfx %d tcc %d blend %d(%u %u %u %u fix %u) atest %d(m%d ref %u fail %d) ztest %d zwrite %d tris %zu box (%.0f %.0f)-(%.0f %.0f) rgba (%.0f %.0f %.0f %.0f) n %zu scroll (%g %g)%s\n",
                             g_diag.present, inst.overrideState ? "shadow" : (&counter == &scene.rigid_models ? "rigid" : "placed"), rigid->model, i, count, pass,
                             view.clipped ? "clipped" : "UNCLIPPED", bucket, m.shader ? unsigned(m.shader->type) : 0u, state.texture, state.tfx, state.tcc, state.blend,
                             state.blend_a, state.blend_b, state.blend_c, state.blend_d, state.blend_fix, state.alpha_test, state.alpha_method, state.alpha_reference,
                             state.alpha_fail, state.depth_test, state.depth_write, triangles, box.x0, box.y0, box.x1, box.y1,
                             box.n ? box.r / box.n : 0.0, box.n ? box.g / box.n : 0.0, box.n ? box.b / box.n : 0.0, box.n ? box.a / box.n : 0.0, box.n,
                             scroll[0], scroll[1], screenCopy ? " SCREEN-COPY" : envMap ? " ENV-MAP" : "");
                if (pass == 0) {
                    // the submodel's streams and the material's shaders (the first is the draw's state)
                    const bool normals = !part.normals.empty() && !part.normals[0].empty(), colours8 = !part.colours.empty() && !part.colours[0].empty();
                    std::string shaders;
                    if (m.material) {
                        for (uint32_t k = 0; k < m.material->shaderCount && k < TE_GFX_MAX_SHADERS; ++k) {
                            const auto* sh = Guest<te_gfx_shader>(m.material->shaders[k]);
                            char buf[160];
                            if (sh) std::snprintf(buf, sizeof buf, " [type %#x settings %#llx tex %u @%#x colour %u %u %u %u]", unsigned(sh->type), (unsigned long long)sh->settings, sh->textureId, sh->texture, sh->colour[0], sh->colour[1], sh->colour[2], sh->colour[3]);
                            else std::snprintf(buf, sizeof buf, " [null]");
                            shaders += buf;
                        }
                    }
                    // the colour histogram of the whole submodel (authored colours: are they dark everywhere or a gradient?)
                    size_t dark = 0, mid = 0, bright = 0, total = 0;
                    for (const auto& strip : part.mesh.strips)
                        for (const auto& v : strip.vertices) {
                            const unsigned lum = (unsigned(v.colour[0]) + unsigned(v.colour[1]) + unsigned(v.colour[2])) / 3;
                            (lum < 16 ? dark : lum < 64 ? mid : bright)++;
                            total++;
                        }
                    std::fprintf(stderr, "[te.gfx.diag]   colours of %zu vertices: dark(<16) %zu mid(<64) %zu bright %zu\n", total, dark, mid, bright);
                    const auto& v0 = part.mesh.strips[0].vertices;
                    std::fprintf(stderr, "[te.gfx.diag]   streams: normals %d colours8 %d, strips %zu, strip0 v0 colour %u %u %u %u stq %g %g %g; material kind %u bucket %u shaders %u:%s\n",
                                 int(normals), int(colours8), part.mesh.strips.size(), v0[0].colour[0], v0[0].colour[1], v0[0].colour[2], v0[0].colour[3], v0[0].stq[0], v0[0].stq[1], v0[0].stq[2],
                                 m.material ? m.material->kind : 0u, m.material ? m.material->bucket : 0u, m.material ? m.material->shaderCount : 0u, shaders.c_str());
                    // the submodel's model-space box and, for TE_GFX_DIAG_MESH=<model>, every strip's vertices (position, STQ)
                    static const char* diagMesh = std::getenv("TE_GFX_DIAG_MESH");
                    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                    for (const auto& strip : part.mesh.strips)
                        for (const auto& v : strip.vertices)
                            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v.position[k]); hi[k] = std::max(hi[k], v.position[k]); }
                    std::fprintf(stderr, "[te.gfx.diag]   model box (%g %g %g)-(%g %g %g)\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
                    if (diagMesh && std::strtoul(diagMesh, nullptr, 0) == rigid->model) {
                        for (size_t s = 0; s < part.mesh.strips.size(); ++s) {
                            const auto& strip = part.mesh.strips[s];
                            for (size_t k = 0; k < strip.vertices.size(); ++k) {
                                const auto& v = strip.vertices[k];
                                std::fprintf(stderr, "[te.gfx.mesh] part %u strip %zu v %zu%s pos (%g %g %g) stq (%g %g %g) rgba %u %u %u %u\n", i, s, k, v.restart ? " RESTART" : "",
                                             v.position[0], v.position[1], v.position[2], v.stq[0], v.stq[1], v.stq[2], v.colour[0], v.colour[1], v.colour[2], v.colour[3]);
                            }
                        }
                    }
                }
            }
            scene.model_triangles += triangles;
            if (pass) scene.material_passes++;
        }
    }
    counter++;
}

// ---- culling state (the VU0 registers' roles) -------------------------------------------------------------------------------
// The last CullTestBoxAt's answer: its outcome masks and its distance (vf15)
struct BoxResult {
    title::CullingMath::Classification visibility;
    int32_t distance;
};
struct Culling {
    std::optional<title::CullingMath::View> view;
    std::optional<title::CullingMath::Link> link;
    std::optional<title::CullingMath::Frustum> planes;
    std::optional<title::CullingMath::Model> model;      // the last CullLoadModel: distance and matrix to the screen (vf15, vf11-14)
    std::optional<title::CullingMath::Classification> cell;   // the last CullTestBox
    std::optional<BoxResult> box;                        // the last CullTestBoxAt
    Matrix toScreen = Identity(), toClip = Identity();   // vf11-14 / vf7-10 as the programs leave them
    // A visible CullTestBoxAt's matrices (TestBoxAt's to_screen / to_clip) made when something reads them: the box's model
    // through the link it was tested with. The instances' tests (scenery.cpp DrawCellInstancesCulled -> InstanceResult) read
    // only in / out and the distance, so their 64 / 128 multiply-adds were never used. Bit for bit what TestBoxAt computes
    // (title::CullingMath::ScreenMatrix / ClipMatrix: the same rows through the same link); the link the pending matrices
    // need is the current one -- whatever replaces it (CullLoadLink, LoadCullingView) makes them first, as does every reader
    // (CullMatrices) and the TE_GAMECPU2=0 arm.
    bool screenPending = false, clipPending = false;
    Matrix screenModel{}, clipModel{};
    uint32_t screenAddress = 0;   // the pending screen matrix's model (its guest address: the Vita's culling memo place)
} g_cull;

#if defined(__vita__)
bool MemoScreenInto(uint32_t address, const Matrix& model, Matrix& out);   // the culling memo (below)
#endif
void MakeCullMatrices() {
    if (g_cull.screenPending) {
#if defined(__vita__)
        if (!MemoScreenInto(g_cull.screenAddress, g_cull.screenModel, g_cull.toScreen))
#endif
            g_cull.toScreen = title::CullingMath::ScreenMatrix(*g_cull.link, g_cull.screenModel);
        g_cull.screenPending = false;
        g_seams.screenMade++;
    }
    if (g_cull.clipPending) {
        g_cull.toClip = title::CullingMath::ClipMatrix(*g_cull.link, g_cull.clipModel);
        g_cull.clipPending = false;
        g_seams.clipMade++;
    }
}

std::optional<title::CullingPlanes::Planes> ReadPlanes(uint32_t address) {
    const float* p = reinterpret_cast<const float*>(te::GuestPointer(address, 6 * 16));
    if (!p) return std::nullopt;
    title::CullingPlanes::Planes planes;
    for (int i = 0; i < 6; ++i) planes[i] = {p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]};
    return planes;
}

#if defined(__vita__)
// ---- the culling memo (the Vita): an unmoved camera's culls answered as the previous pictures answered them ---------------
// The culling seams compute pure functions of their inputs: a box test (CullTestBoxAt) of the frustum, the link (its
// camera), the box and the model matrix; a model load (CullLoadModel) and a visible box's matrix to the screen of the link
// and the model; a cell test (CullTestBox) of the frustum and the box. The frustum and the link are interned by their
// CONTENT when loaded (an id per distinct content seen; ids are never reused), and an answer is kept per instance (its box
// / model address, direct-mapped) together with the exact input words it was made from: it is given back only for the same
// ids and the same words -- the answer the function gives, by construction. The device: ~550 box tests, ~570 model loads
// and ~290 cell tests a tick (3.4 / 1.6 / 2.0 us each; ~3.8 ms a tick with the matrices). Used only while the frustum and
// the link were already there in an earlier picture (the camera unmoved: the menus over a scene, a standing player, the
// pause): a moving camera makes new ones every picture, and the memo then neither looks nor keeps (no cost there).
// TE_GFX_CULL_MEMO=0: off.
struct CullMemo {
    using CM = title::CullingMath;
    template <typename T>
    static uint32_t HashOf(const T& v) {
        static_assert(sizeof(T) % 4 == 0, "words");
        uint32_t w[sizeof(T) / 4];
        std::memcpy(w, &v, sizeof w);
        uint32_t h = 2166136261u;
        for (uint32_t x : w) h = (h ^ x) * 16777619u;
        return h | 1u;
    }
    template <typename T>
    struct Interned {
        uint32_t id = 0, hash = 0;
        uint64_t picture = 0;   // the picture it was first loaded in
        T value{};
    };
    template <typename T, size_t N>
    struct Table {
        std::array<Interned<T>, N> slots{};
        // the id of `value` and whether an earlier picture had it (the camera unmoved since)
        uint32_t Intern(const T& value, uint64_t picture, uint32_t& nextId, bool& stable) {
            const uint32_t h = HashOf(value);
            Interned<T>& s = slots[(h ^ (h >> 16)) & (N - 1)];
            if (s.id && s.hash == h && std::memcmp(&s.value, &value, sizeof value) == 0) {
                stable = s.picture < picture;
                return s.id;
            }
            s.id = nextId++;
            s.hash = h;
            s.picture = picture;
            std::memcpy(&s.value, &value, sizeof value);
            stable = false;
            return s.id;
        }
    };
    Table<CM::Frustum, 64> frustums;
    Table<CM::Link, 64> links;
    uint32_t nextId = 1;
    uint32_t fid = 0, lid = 0;          // the current frustum's / link's (g_cull.planes / g_cull.link), 0 none
    bool fstable = false, lstable = false;
    struct BoxEntry {                   // a CullTestBoxAt's visibility and distance
        uint32_t fid = 0, lid = 0, fpctl = 0;
        uint32_t box[8], model[16];
        CM::Classification visibility;
        int32_t distance;
    };
    struct ScreenEntry {                // LoadModel's matrix to the screen and distance (ScreenMatrix = its matrix)
        uint32_t lid = 0, fpctl = 0;
        uint32_t model[16];
        CM::Matrix screen;
        int32_t distance;
        bool distanced;                 // the distance was made (a model load's; a box matrix's entry has none)
    };
    struct CellEntry {                  // a CullTestBox's classification
        uint32_t fid = 0, fpctl = 0;
        uint32_t box[8];
        CM::Classification cell;
    };
    // (allocated at the first use when on: 4096 / 4096 / 2048 entries, ~1.2 MB -- an unmoved island scene loads ~770 models
    // and tests ~670 boxes a picture: 1024 direct-mapped slots lost half of them to collisions)
    std::vector<BoxEntry> boxes;
    std::vector<ScreenEntry> screens;
    std::vector<CellEntry> cells;
    bool read = false, on = true, check = false;
    long ab = 0;   // TE_GFX_CULL_MEMO=abN: on / off by N presents (a device A/B)
    // the census: calls the memo looked at (the camera unmoved), answers it gave
    uint64_t boxLooked = 0, boxHits = 0, loadLooked = 0, loadHits = 0, cellLooked = 0, cellHits = 0, screenLooked = 0, screenHits = 0;
    uint64_t frustumsLoaded = 0, frustumsStable = 0, linksLoaded = 0, linksStable = 0;
    uint64_t checked = 0, differ = 0;   // TE_GFX_CULL_MEMO_CHECK=1: every answer given also made and compared
    uint64_t dupLooked = 0, dupHits = 0;   // TE_GFX_REPEAT_CENSUS: box tests repeated within one picture (a moving camera)
    bool On() {
        if (!read) {
            read = true;
            if (const char* v = std::getenv("TE_GFX_CULL_MEMO")) {
                if (v[0] == 'a' && v[1] == 'b') ab = std::max(1L, std::atol(v + 2));
                else on = v[0] != '0';
            }
            check = std::getenv("TE_GFX_CULL_MEMO_CHECK") != nullptr;
            if (on) {
                boxes.assign(4096, BoxEntry{});
                screens.assign(4096, ScreenEntry{});
                cells.assign(2048, CellEntry{});
            }
        }
        return on && (ab <= 0 || ((te_graphics_presents() / uint64_t(ab)) & 1u) == 0);
    }
    static uint32_t SlotOf(uint32_t a, uint32_t b, uint32_t mask) {
        uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
        return (h ^ (h >> 15)) & mask;
    }
    static bool Same(const uint32_t* kept, const float* now, int n) {
        uint32_t differ = 0;
#pragma GCC novector   // (core registers: see SameWords)
        for (int k = 0; k < n; ++k) {
            uint32_t w;
            std::memcpy(&w, now + k, 4);
            differ |= w ^ kept[k];
        }
        return differ == 0;
    }
    // The thread's rounding mode, flush-to-zero and default-NaN bits (FPSCR 22..25): an answer is the function's under the
    // mode it was made in -- kept with it, compared at every look (the culling runs in the thread's own mode on the Vita)
    static uint32_t FpControl() {
        uint32_t fpscr;
        asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
        return fpscr & 0x03C00000u;
    }
} g_cullMemo;
uint64_t CullPicture() { return te_graphics_presents(); }
void NoteFrustum() {
    CullMemo& m = g_cullMemo;
    if (!m.On() || !g_cull.planes) { m.fid = 0; m.fstable = false; return; }
    m.fid = m.frustums.Intern(*g_cull.planes, CullPicture(), m.nextId, m.fstable);
    m.frustumsLoaded++;
    m.frustumsStable += m.fstable ? 1 : 0;
}
void NoteLink() {
    CullMemo& m = g_cullMemo;
    if (!m.On() || !g_cull.link) { m.lid = 0; m.lstable = false; return; }
    m.lid = m.links.Intern(*g_cull.link, CullPicture(), m.nextId, m.lstable);
    m.linksLoaded++;
    m.linksStable += m.lstable ? 1 : 0;
}
// LoadModel / ScreenMatrix of the current link through the memo (`address`: the model's guest address, the entry's place)
const CullMemo::ScreenEntry* MemoScreen(uint32_t address, const float* modelWords, bool withDistance) {
    CullMemo& m = g_cullMemo;
    if (!m.lid || !m.lstable) return nullptr;
    (withDistance ? m.loadLooked : m.screenLooked)++;
    const uint32_t fpctl = CullMemo::FpControl();
    CullMemo::ScreenEntry& e = m.screens[CullMemo::SlotOf(address, 0x5C3EEDu, uint32_t(m.screens.size() - 1))];
    if (e.lid == m.lid && e.fpctl == fpctl && (!withDistance || e.distanced) && CullMemo::Same(e.model, modelWords, 16)) {
        (withDistance ? m.loadHits : m.screenHits)++;
        if (m.check) {
            const title::CullingMath::Matrix model = ToMatrix(reinterpret_cast<const float(*)[4]>(modelWords));
            const auto loaded = title::CullingMath::LoadModel(*g_cull.link, model);
            const auto screen = title::CullingMath::ScreenMatrix(*g_cull.link, model);
            m.checked++;
            if (std::memcmp(&loaded.to_screen, &e.screen, sizeof e.screen) != 0 || std::memcmp(&screen, &e.screen, sizeof e.screen) != 0 ||
                (withDistance && loaded.distance != e.distance))
                m.differ++;
        }
        return &e;
    }
    std::memcpy(e.model, modelWords, sizeof e.model);
    e.fpctl = fpctl;
    const title::CullingMath::Matrix model = ToMatrix(reinterpret_cast<const float(*)[4]>(modelWords));
    if (withDistance) {
        const auto loaded = title::CullingMath::LoadModel(*g_cull.link, model);
        e.screen = loaded.to_screen;
        e.distance = loaded.distance;
        e.distanced = true;
    } else {
        e.screen = title::CullingMath::ScreenMatrix(*g_cull.link, model);
        e.distanced = false;
    }
    e.lid = m.lid;
    return &e;
}
bool MemoScreenInto(uint32_t address, const Matrix& model, Matrix& out) {
    const CullMemo::ScreenEntry* e = MemoScreen(address, model[0].data(), false);
    if (!e) return false;
    out = e->screen;
    return true;
}
#endif

}  // namespace

extern "C" {

// ---- the models' geometry (wasm side ReadModel -> the packet bytes) -------------------------------------------------------
void ps2e_gfx2_model_loaded(ps2e_u32 model, ps2e_s32 index, ps2e_s32 count, ps2e_u32 packet, ps2e_s32 size, ps2e_s32 vertices) {
    const uint8_t* bytes = size > 0 ? te::GuestPointer(packet, size_t(size)) : nullptr;
    LoadModelPart(model, index, count, bytes, size, vertices);
}

void ps2e_gfx2_model_released(ps2e_u32 model) {
    TE_PROF_SCOPE("model: released (the builds drained, its meshes retired)");
    te::load::Timed timed(te::load::kModelRelease);
    if (!DrainEveryModel()) {
        // (the Vita's default) its storage kept until no picture handed over can be built from it -- RetiredModel -- instead
        // of the builds drained here (a chunk's unload: the game thread ran every queued build of the picture itself, the
        // first release of the burst); its native meshes retired as before (a part in a batch: the batch taken first)
        if (auto it = g_models.find(model); it != g_models.end())
            for (Part& part : it->second.parts) RetireMesh(part);
        auto retired = std::make_unique<RetiredModel>();
        retired->model = g_models.extract(model);
        retired->shadow = g_shadowModels.extract(model);
        ForgetModel(model);
        if (retired->model || retired->shadow) te::gfx::RetireStorage(std::move(retired));
        return;
    }
    te::gfx::DrainBuilds();   // a posted build may still read its parts
    ResolveMeshBatches();     // a batch being made may still name its parts
    if (auto it = g_models.find(model); it != g_models.end())
        for (Part& part : it->second.parts) RetireMesh(part);
    g_models.erase(model);
    ForgetModel(model);
    g_shadowModels.erase(model);
}

}  // extern "C"

namespace {
void LoadModelPart(uint32_t model, int32_t index, int32_t count, const uint8_t* bytes, int32_t size, int32_t vertices) {
    auto& scene = te::gfx::Scene();
    if (index < 0 || count <= 0 || index >= count || count > 4096) return;
    {
        te::load::Timed waited(te::load::kModelWait, 0);
        // a posted build may still read the parts this replaces, a batch being made may still name them -- but only a
        // DECODED part is ever drawn (DrawInstance, the sky and FlushNativeMeshes skip the others), and the map's nodes do not
        // move: a chunk's new model (not in the map) and the submodels after its first (their parts made empty at the first)
        // replace nothing a build or a batch can name: no wait (the Vita's default; TE_GFX_MODEL_DRAIN=1 always waits, as
        // before -- the default elsewhere)
        bool replacesDrawn = false;
        if (const auto mit = g_models.find(model); mit != g_models.end()) {
            const Model& old = mit->second;
            if (old.parts.size() != size_t(count)) {
                for (const Part& p : old.parts) replacesDrawn = replacesDrawn || p.decoded;
            } else {
                replacesDrawn = old.parts[size_t(index)].decoded;
            }
        }
        if (DrainEveryModel() || replacesDrawn) {
            te::gfx::DrainBuilds();
            ResolveMeshBatches();
        }
    }
    TE_PROF_SCOPE("model: decode + prepare (the packet -> strips)");
    te::load::Timed decoding(te::load::kModelDecode, 0);
    Model& m = g_models[model];
    if (m.parts.size() != size_t(count)) {
        for (Part& old : m.parts) RetireMesh(old);
        m.parts.assign(size_t(count), Part{});
        m.sources.assign(size_t(count), PartSource{});
    }
    PartSource& source = m.sources[size_t(index)];
    source.vertices = vertices;
    if (bytes && size > 0 && source.packet.data() != bytes) source.packet.assign(bytes, bytes + size);
    Part& part = m.parts[size_t(index)];
    RetireMesh(part);
    part = Part{};
    std::string why;
    if (!bytes || !DecodePacket(bytes, size_t(size), uint32_t(vertices), part, &why)) {
        scene.models_refused++;
        static int said = 0;
        if (said++ < 16)
            std::fprintf(stderr, "[te.gfx] model %#x submodel %d/%d: packet not decoded (%s; %d bytes, %d vertices)\n", model, index, count,
                         why.c_str(), size, vertices);
        return;
    }
    // prepared once here; every instance's build only transforms, clips and projects it
    const Geometry::Result prepared = te::gfx::MeshPrepare(part.mesh, part.prepared);
    if (prepared != Geometry::Result::Ok) {
        part.decoded = false;
        scene.models_refused++;
        static int said = 0;
        if (said++ < 16)
            std::fprintf(stderr, "[te.gfx] model %#x submodel %d/%d: geometry refused by the engine (%d; %zu strips, %u vertices)\n", model, index,
                         count, int(prepared), part.mesh.strips.size(), part.vertices);
        return;
    }
    scene.models_decoded++;
    decoding.count = 1;
    if (MeshAtLoad() && te::gfx::NativeMeshes()) QueuePartPrep(part);   // its native mesh prepared now, ahead of its first draw
    if (te::gfx::LogEnabled() && scene.models_decoded <= 64)
        std::fprintf(stderr, "[te.gfx] model %#x submodel %d/%d: %zu strips, %u vertices (declared %d), %zu triangles, normals %s, colours %s\n",
                     model, index, count, part.mesh.strips.size(), part.vertices, vertices, part.triangles,
                     !part.normals.empty() && !part.normals[0].empty() ? "yes" : "no",
                     !part.colours.empty() && !part.colours[0].empty() ? "yes" : "no");
}

// TE_GAMECPU2_BENCH (CullBench): called once a picture (FlushNativeMeshes, before the present). Arms the capture for picture
// P, and after it times the captured tests: R repetitions of the whole list per form (the inputs replayed in order)
void GameCpuBench() {
    CullBench& b = g_cullBench;
    if (b.at == -2) {
        const char* v = std::getenv("TE_GAMECPU2_BENCH");
        b.at = v ? std::max(0L, std::atol(v)) : -1;
    }
    if (b.at < 0 || b.done) return;
    const uint64_t presents = te_graphics_presents() + 1;   // the picture this call precedes the present of
    if (!b.capturing) {
        if (presents + 1 == uint64_t(b.at)) b.capturing = true;   // the next picture's tests
        return;
    }
    b.capturing = false;
    b.done = true;
    if (b.cases.empty()) return;
    auto now = [] {
#if defined(__vita__)
        return int64_t(sceKernelGetProcessTimeWide());
#else
        return int64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    };
    auto Bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
    constexpr int kReps = 20;
    volatile uint32_t sink = 0;
    size_t visible = 0, clipped = 0;
    for (const auto& c : b.cases) {
        const auto r = title::CullingMath::TestBoxAtReference(c.frustum, c.link, c.box, c.model);
        visible += r.to_screen ? 1 : 0;
        clipped += r.to_clip ? 1 : 0;
    }
    int64_t t0 = now();
    for (int rep = 0; rep < kReps; ++rep)
        for (const auto& c : b.cases) {
            const auto r = title::CullingMath::TestBoxAtReference(c.frustum, c.link, c.box, c.model);
            sink += r.visibility.outside + uint32_t(r.distance) + (r.to_screen ? Bits((*r.to_screen)[3][0]) : 0u);
        }
    int64_t t1 = now();
    for (int rep = 0; rep < kReps; ++rep)
        for (const auto& c : b.cases) {
            const auto r = title::CullingMath::TestBoxAtVisibility(c.frustum, c.link, c.box, c.model);
            sink += r.visibility.outside + uint32_t(r.distance);
        }
    int64_t t2 = now();
    for (int rep = 0; rep < kReps; ++rep)
        for (const auto& c : b.cases) {
            const auto r = title::CullingMath::TestBoxAtVisibility(c.frustum, c.link, c.box, c.model);
            sink += r.visibility.outside + uint32_t(r.distance);
            if (!r.visibility.outside) {
                sink += Bits(title::CullingMath::ScreenMatrix(c.link, c.model)[3][0]);
                if (r.visibility.clipped) sink += Bits(title::CullingMath::ClipMatrix(c.link, c.model)[3][0]);
            }
        }
    int64_t t3 = now();
    const double n = double(kReps) * double(b.cases.size());
    // (%u with casts: the Vita's newlib printf has no %zu)
    std::fprintf(stderr, "[te.gfx.bench] present %ld: %u box tests (%u visible, %u clipped), %d repetitions: the previous TestBoxAt "
                 "%.2f us a test; the visibility half %.2f us; the halves with every matrix made %.2f us (%u)\n",
                 b.at, unsigned(b.cases.size()), unsigned(visible), unsigned(clipped), kReps, double(t1 - t0) / n, double(t2 - t1) / n,
                 double(t3 - t2) / n, unsigned(sink & 1));
    b.cases.clear();
    b.cases.shrink_to_fit();
}

// "[te.gfx.seams] presents A-B per present: ..." with every TE_GFX_SUMMARY_EVERY-th present (default 600): the calls the
// te.prof shares of the game thread's scene seams are made of (a share / the calls = what one call costs on the device)
void SeamCensusLine() {
    GameCpuBench();
    static bool repeatRead = false;
    if (!repeatRead) {
        repeatRead = true;
        te::gfx::g_repeatCensus = std::getenv("TE_GFX_REPEAT_CENSUS") != nullptr;
    }
    if (te::gfx::g_repeatCensus) RepeatPictureEnd();
    static long every = -1;
    static uint64_t from = 0;
    static SeamCensus last{};
    static uint64_t lastDecals = 0, lastDecalsUnclamped = 0;
    if (every < 0) {
        const char* v = std::getenv("TE_GFX_SUMMARY_EVERY");
        every = v ? std::max(0L, std::atol(v)) : 600L;
    }
    if (every <= 0) return;
    const uint64_t presents = te_graphics_presents() + 1;   // this picture's (FlushNativeMeshes runs before its present)
    if (presents % uint64_t(every) != 0 || presents <= from) return;
    const double n = double(presents - from);
    const SeamCensus& s = g_seams;
    auto per = [&](uint64_t now, uint64_t before) { return double(now - before) / n; };
    std::fprintf(stderr, "[te.gfx.seams] presents %llu-%llu per present (%s): placed %.1f, rigid %.1f, native draws %.1f; box tests %.1f "
                 "(visible %.1f, clipped %.1f; matrices made %.1f to the screen + %.1f to the clip space, read %.1f times), model loads "
                 "%.1f, cell tests %.1f, results %.1f; decals aged %.1f (unclamped %.1f); materials cached %.1f, read %.1f\n",
                 (unsigned long long)(from + 1), (unsigned long long)presents, FastSeams() ? "TE_GAMECPU2 on" : "TE_GAMECPU2=0",
                 per(s.placed, last.placed), per(s.rigid, last.rigid), per(s.nativeDraws, last.nativeDraws), per(s.boxTests, last.boxTests),
                 per(s.boxVisible, last.boxVisible), per(s.boxClipped, last.boxClipped), per(s.screenMade, last.screenMade),
                 per(s.clipMade, last.clipMade), per(s.matrixReads, last.matrixReads), per(s.modelLoads, last.modelLoads),
                 per(s.cellTests, last.cellTests), per(s.results, last.results), per(te::gfx::Scene().decals_aged, lastDecals),
                 per(te::gfx::DecalsUnclamped(), lastDecalsUnclamped), per(s.materialHits, last.materialHits),
                 per(s.materialMisses, last.materialMisses));
    // the other costly seams' calls (graphics_internal.h SeamCall)
    static uint32_t lastCalls[te::gfx::kCallCount] = {};
    const uint32_t* c = te::gfx::g_seamCalls;
    auto calls = [&](te::gfx::SeamCall k) { return double(c[k] - lastCalls[k]) / n; };
    std::fprintf(stderr, "[te.gfx.calls] presents %llu-%llu per present: get_bbox %.1f (points %.1f), triangle_bounds %.1f, joint_matrix "
                 "%.1f, euler_rotation %.1f, multiply_by_parent %.1f, lerp %.1f, multiply_matrices %.1f, start_ray_triangle %.1f, "
                 "finish_ray_triangle %.1f, particle_block_view %.1f, draw_skin %.1f, draw_blend_skin %.1f, draw_shadow_mesh %.1f\n",
                 (unsigned long long)(from + 1), (unsigned long long)presents, calls(te::gfx::kCallGetBbox), calls(te::gfx::kCallBboxPoints),
                 calls(te::gfx::kCallTriangleBounds), calls(te::gfx::kCallJointMatrix), calls(te::gfx::kCallEuler),
                 calls(te::gfx::kCallMultiplyByParent), calls(te::gfx::kCallLerp), calls(te::gfx::kCallMultiplyMatrices),
                 calls(te::gfx::kCallStartRay), calls(te::gfx::kCallFinishRay), calls(te::gfx::kCallParticleView),
                 calls(te::gfx::kCallSkin), calls(te::gfx::kCallBlendSkin), calls(te::gfx::kCallShadowMesh));
    std::memcpy(lastCalls, c, sizeof lastCalls);
    if (te::gfx::g_repeatCensus) {
        static const char* const kNames[te::gfx::kCallCount] = {"get_bbox", "(points)", "triangle_bounds", "joint_matrix", "euler_rotation",
                                                                "multiply_by_parent", "lerp", "multiply_matrices", "start_ray_triangle",
                                                                "finish_ray_triangle", "particle_block_view", "draw_skin",
                                                                "draw_blend_skin", "draw_shadow_mesh"};
        std::string line;
        for (uint32_t k = 0; k < uint32_t(te::gfx::kCallCount); ++k) {
            auto& t = te::gfx::g_repeat[k];
            if (!t.calls) continue;
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s%s %llu calls: %.1f%% as the previous picture's at the same place, %.1f%% as one of them",
                          line.empty() ? "" : "; ", kNames[k], (unsigned long long)t.calls, 100.0 * double(t.samePlace) / double(t.calls),
                          100.0 * double(t.sameAny) / double(t.calls));
            line += buf;
            t.calls = t.samePlace = t.sameAny = 0;
        }
        std::fprintf(stderr, "[te.gfx.repeat] presents %llu-%llu (TE_GFX_REPEAT_CENSUS): %s\n", (unsigned long long)(from + 1),
                     (unsigned long long)presents, line.c_str());
    }
    {
        const std::string compactLine = te::gfx::CompactCensusLine(n);
        if (!compactLine.empty())
            std::fprintf(stderr, "[te.gfx.vita] presents %llu-%llu %s\n", (unsigned long long)(from + 1), (unsigned long long)presents,
                         compactLine.c_str());
    }
#if defined(__vita__)
    {
        // the culling memo (CullMemo): per present, the calls it looked at (the frustum / link unmoved) and answered
        struct Counts {
            uint64_t frustumsLoaded = 0, frustumsStable = 0, linksLoaded = 0, linksStable = 0, boxLooked = 0, boxHits = 0,
                     loadLooked = 0, loadHits = 0, screenLooked = 0, screenHits = 0, cellLooked = 0, cellHits = 0;
        };
        static Counts last;
        const CullMemo& m = g_cullMemo;
        auto d = [&](uint64_t now, uint64_t before) { return double(now - before) / n; };
        std::fprintf(stderr, "[te.gfx.vita] presents %llu-%llu culling memo%s per present: frustums loaded %.1f (%.1f unmoved), links %.1f "
                     "(%.1f unmoved); box tests looked at %.1f, answered %.1f; model loads %.1f / %.1f; box matrices %.1f / %.1f; "
                     "cell tests %.1f / %.1f\n",
                     (unsigned long long)(from + 1), (unsigned long long)presents,
                     !g_cullMemo.on ? " (OFF: TE_GFX_CULL_MEMO=0)" : g_cullMemo.ab > 0 ? " (A/B by TE_GFX_CULL_MEMO=abN)" : "",
                     d(m.frustumsLoaded, last.frustumsLoaded), d(m.frustumsStable, last.frustumsStable), d(m.linksLoaded, last.linksLoaded),
                     d(m.linksStable, last.linksStable), d(m.boxLooked, last.boxLooked), d(m.boxHits, last.boxHits),
                     d(m.loadLooked, last.loadLooked), d(m.loadHits, last.loadHits), d(m.screenLooked, last.screenLooked),
                     d(m.screenHits, last.screenHits), d(m.cellLooked, last.cellLooked), d(m.cellHits, last.cellHits));
        if (m.check)
            std::fprintf(stderr, "[te.gfx.vita] presents %llu-%llu culling memo CHECK (TE_GFX_CULL_MEMO_CHECK): %llu answers made again "
                         "and compared, %llu differ\n", (unsigned long long)(from + 1), (unsigned long long)presents,
                         (unsigned long long)m.checked, (unsigned long long)m.differ);
        if (te::gfx::g_repeatCensus) {
            static uint64_t lastDupLooked = 0, lastDupHits = 0;
            std::fprintf(stderr, "[te.gfx.repeat] presents %llu-%llu box tests of a moved camera per present: %.1f, of them the same box, "
                         "model, frustum and link as earlier in the picture %.1f\n", (unsigned long long)(from + 1),
                         (unsigned long long)presents, d(m.dupLooked, lastDupLooked), d(m.dupHits, lastDupHits));
            lastDupLooked = m.dupLooked;
            lastDupHits = m.dupHits;
        }
        last.frustumsLoaded = m.frustumsLoaded; last.frustumsStable = m.frustumsStable;
        last.linksLoaded = m.linksLoaded; last.linksStable = m.linksStable;
        last.boxLooked = m.boxLooked; last.boxHits = m.boxHits; last.loadLooked = m.loadLooked; last.loadHits = m.loadHits;
        last.screenLooked = m.screenLooked; last.screenHits = m.screenHits; last.cellLooked = m.cellLooked; last.cellHits = m.cellHits;
    }
    {
        // the builders' work per present (the shadow volumes, the chunked builds, the CPU skins) and the pool's
        std::string line = te::gfx::BuildersCensusLine(n);
        if (!line.empty()) {
            char head[64];
            std::snprintf(head, sizeof head, "presents %llu-%llu ", (unsigned long long)(from + 1), (unsigned long long)presents);
            for (size_t at = 0; (at = line.find("] ", at)) != std::string::npos; at += 2 + std::strlen(head)) line.insert(at + 2, head);
            std::fputs(line.c_str(), stderr);
        }
    }
#endif
    last = s;
    lastDecals = te::gfx::Scene().decals_aged;
    lastDecalsUnclamped = te::gfx::DecalsUnclamped();
    from = presents;
}
}  // namespace

// ---- save state (graphics.cpp's kTagGraphics section carries these): the decoded models are rebuilt from their kept
// packet bytes
namespace te::gfx {
// TE_GAMECPU2 (graphics_internal.h): the game thread's fast seam paths on / off (the in-run A/B flips it)
bool GameCpuFast() { return FastSeams(); }
void SetGameCpuFast(bool on) {
    Opts();
    if (g_opts.fast == on) return;
    if (!on) MakeCullMatrices();   // (the slow arm computes the box matrices at the test: none left pending)
    g_opts.fast = on;
}
void ResetDrawOrdinals() { g_drawOrdinals.clear(); }
// The frame's camera for the interpolator: the game's current RenderView -- world->camera, camera->screen (the GS pixel
// space, the viewport included: what toScreen = toCamera . cameraToScreen builds every rigid draw from) and its camera object
bool CurrentCamera(ps2r::ps2e::InterpCamera& out, uint32_t* camera_object) {
    const RenderView* view = CurrentView();
    if (!view) return false;
    const Matrix cam = ToMatrix(view->toCamera), c2s = ToMatrix(view->cameraToScreen);
    if (!Finite(cam) || !Finite(c2s)) return false;
    Flat(cam, out.to_camera);
    Flat(c2s, out.camera_to_screen);
    out.valid = true;
    if (camera_object) *camera_object = view->cameraObject;
    return true;
}
std::vector<ModelSource> ModelSources() {
    std::vector<ModelSource> out;
    for (const auto& [address, m] : g_models)
        for (size_t i = 0; i < m.sources.size(); ++i)
            if (!m.sources[i].packet.empty())
                out.push_back(ModelSource{address, int32_t(i), int32_t(m.sources.size()), m.sources[i].vertices, m.sources[i].packet});
    return out;
}
void FlushNativeMeshes() {
    // the workers' triangles into the census once a picture (DrawInstance's TE_GAMECPU2=0 arm folds them per instance)
    Scene().model_triangles += g_deferredModelTriangles.exchange(0, std::memory_order_relaxed);
    SeamCensusLine();
#if defined(__vita__)
    FlushSkinMeshes();   // the skin parts' meshes asked for this picture (graphics_scene.cpp's skins section)
    for (RepeatKeys& keys : g_repeatKeys) keys.EndPicture();   // the builders' census: this picture's inputs
#endif
    if (g_meshRequests.empty()) return;
    // the load census: the parts that asked for their mesh this picture, the meshes composed and posted (and their time)
    te::load::Note(te::load::kMeshAsk, 0.0, uint32_t(g_meshRequests.size()));
    te::load::Timed posted(te::load::kMeshPost, 0);
    // made on the drawing thread ahead of the picture handed over next, without this thread waiting there (the drawing
    // thread was usually still drawing or presenting the previous picture: the wait was most of a frame); the parts take
    // their handles before anything reads them (ResolveMeshBatches)
    MeshBatch batch;
    batch.job = std::make_shared<MeshBatchJob>();
    for (const auto& [model, index] : g_meshRequests) {
        auto it = g_models.find(model);
        if (it == g_models.end() || index >= it->second.parts.size()) continue;
        Part& part = it->second.parts[index];
        if (part.meshState != 1 || !part.decoded) continue;
        part.meshState = 2;
        MeshRequest r;
        if (part.prep) {
            // prepared at its load (PartPrep): the batch makes the mesh from that form; its chunks, count and box go into
            // the part with its handle (ResolveMeshBatches). The list's length decides as before whether there is a mesh.
            const uint32_t n = ListVertices(part);
            if (n < 3) continue;
            g_prepCensus.used.fetch_add(1, std::memory_order_relaxed);
            if (part.prep->state.load(std::memory_order_acquire) != te::gfx::PreparedMesh::kMade)
                g_prepCensus.unready.fetch_add(1, std::memory_order_relaxed);
            r.prepared = part.prep;
            part.meshState = 3;
            posted.count++;
            posted.bytes += n;
            batch.job->requests.push_back(std::move(r));
            batch.parts.push_back(&part);
            continue;
        }
#if defined(__vita__)
        std::vector<uint32_t> offsets;
        r.vertices = MeshVertices(part, &offsets);
        if (r.vertices.size() < 3) continue;
        // the chunks a clipped instance's build sorts (made once, with the mesh; a build posted before reads none)
        part.nativeCount = uint32_t(r.vertices.size() / 3 * 3);
        MakeChunks(part, offsets, part.chunks);
        part.chunked = !part.chunks.empty();
#else
        r.vertices = MeshVertices(part);
        if (r.vertices.size() < 3) continue;
#endif
        std::array<float, 6> box{1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        for (const auto& v : r.vertices) {
            box[0] = std::min(box[0], v.x); box[1] = std::min(box[1], v.y); box[2] = std::min(box[2], v.z);
            box[3] = std::max(box[3], v.x); box[4] = std::max(box[4], v.y); box[5] = std::max(box[5], v.z);
        }
        part.nativeBox = box;
        part.meshState = 3;
        posted.count++;
        posted.bytes += r.vertices.size();
        batch.job->requests.push_back(std::move(r));
        batch.parts.push_back(&part);
    }
    g_meshRequests.clear();
    if (batch.parts.empty()) return;
    batch.ticket = CreateMeshesAsync(batch.job);
    g_meshBatches.push_back(std::move(batch));
    if (!g_meshBatches.back().ticket) ResolveMeshBatches();   // made here and now (no drawing thread): taken at once, as before
}
// The census of the native meshes prepared at their load (the pace window; empty when nothing happened)
std::string PrepCensusLine() {
    auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
    const uint64_t queued = take(g_prepCensus.queued), ahead = take(g_prepCensus.ahead), used = take(g_prepCensus.used),
                   unready = take(g_prepCensus.unready);
    if (!queued && !ahead && !used) return std::string();
    char b[400];
    std::snprintf(b, sizeof b, "[te.load] native meshes prepared at their load (TE_GFX_MESH_AT_LOAD): %llu queued, %llu prepared on the "
                  "workers; %llu taken at their part's first ask (%llu of them not prepared yet then: made by the batch)\n",
                  (unsigned long long)queued, (unsigned long long)ahead, (unsigned long long)used, (unsigned long long)unready);
    return b;
}
void ReplayModelSources(const std::vector<ModelSource>& sources) {
    DrainBuilds();
    ResolveMeshBatches();
    for (auto& [address, m] : g_models)
        for (Part& part : m.parts) RetireMesh(part);
    g_models.clear();
    ForgetModels();
    g_shadowModels.clear();
    auto& scene = Scene();
    const uint64_t decoded = scene.models_decoded, refused = scene.models_refused;
    for (const ModelSource& s : sources)
        LoadModelPart(s.model, s.index, s.count, s.packet.data(), int32_t(s.packet.size()), s.vertices);
    // the counters are the run's census, not the restore's
    scene.models_decoded = decoded;
    scene.models_refused = refused;
}
}  // namespace te::gfx

extern "C" {

// ---- the scene's models ------------------------------------------------------------------------------------------------------
// DrawRigidModel (models.cpp SetRigidModelRenderDMA): the instance's matrix to the screen = matrix * view.toScreen, to the clip
// space = matrix * view.toClip, the lights' directions turned into the model's space (the rotation's transpose), drawn as the
// view test found it (mode 1 wholly in view: no clipping; 2: clipped)
void ps2e_graphics_draw_rigid_model(ps2e_u32 model, ps2e_u32 matrix, ps2e_u32 lights, ps2e_s32 mode) {
    const Mem mem;
    const auto* rigid = mem.Get<te_gfx_rigid_model>(model);
    const auto world = ReadMatrix(matrix);
    const RenderView* view = CurrentView();
    if (!rigid || !world || !view || !te::gfx::EnsureDevice()) return;
    g_seams.rigid++;
    Geometry::View v;
    v.screen = Multiply(*world, ToMatrix(view->toScreen));
    v.clip = Multiply(*world, ToMatrix(view->toClip));
    v.guard = {view->clip[0], view->clip[1], view->clip[2], view->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = uint32_t(mode) != DrawUnclipped;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    Lights l{};
    bool haveLights = false;
    if (const auto* ml = mem.Get<ModelLights>(lights)) {
        const float* dirs = mem.Floats(ml->directions, 12);
        const float* cols = mem.Floats(ml->colours, 12);
        const float* amb = mem.Floats(ml->ambient, 4);
        if (dirs && cols && amb) {
            haveLights = true;
            for (int i = 0; i < 3; ++i) {
                // RotateVector(transpose(matrix), direction): l_k = sum_r matrix[k][r] * d[r]
                for (int k = 0; k < 3; ++k) {
                    float s = 0.f;
                    for (int r = 0; r < 3; ++r) s += (*world)[k][r] * dirs[i * 4 + r];
                    l.directions[i][k] = s;
                }
                l.directions[i][3] = 0.f;
                l.colours[i] = {cols[i * 4], cols[i * 4 + 1], cols[i * 4 + 2], cols[i * 4 + 3]};
            }
            l.ambient = {amb[0], amb[1], amb[2], amb[3]};
        }
    }
    InstanceState inst;
    inst.lights = haveLights ? &l : nullptr;
    if (rigid->needsEye) inst.eye = EyeIn(*world, view->toCamera);   // the env-map passes
    // the interpolator's identity: this model at this instance's matrix (a stable address while the instance lives)
    if (te::gfx::Interpolating())
        if (const auto w = WorldOfScreen(v.screen)) TagRigid(OrdinalId(TagId(model, 0)), *w, v.screen, FamilyOf(TagId(model, 0)));   // (the matrix is a stack local: the order names the instance)
    if (g_diag.Logging()) {
        const auto* t = te::gfx::CurrentTarget();
        std::fprintf(stderr, "[te.gfx.diag] present %ld rigid view: toScreen row3 (%.1f %.1f %.1f %.1f) proj row0 (%.3f %.3f %.3f %.3f) clip (%.2f %.2f %.2f %.2f) world row3 (%.2f %.2f %.2f) target %s off (%d %d) size (%d %d) display (%d %d) xy (%d %d) c2s row3 (%.1f %.1f %.1f %.1f)\n",
                     g_diag.present, view->toScreen[3][0], view->toScreen[3][1], view->toScreen[3][2], view->toScreen[3][3],
                     view->projection[0][0], view->projection[0][1], view->projection[0][2], view->projection[0][3],
                     view->clip[0], view->clip[1], view->clip[2], view->clip[3], (*world)[3][0], (*world)[3][1], (*world)[3][2],
                     t ? "yes" : "none", t ? t->offsetX : 0, t ? t->offsetY : 0, t ? t->width : 0, t ? t->height : 0, t ? t->displayWidth : 0, t ? t->displayHeight : 0,
                     t ? t->x : 0, t ? t->y : 0, t ? t->clipToScreen[3][0] : 0.f, t ? t->clipToScreen[3][1] : 0.f, t ? t->clipToScreen[3][2] : 0.f, t ? t->clipToScreen[3][3] : 0.f);
    }
    DrawInstance(rigid, v, inst, te::gfx::Scene().rigid_models);
    te::gfx::ClearDrawTag();
}

// DrawPlacedModel (models.cpp SetPlacedModelDMA): the chunk view's matrices of its last culling test (to the screen, to the clip
// space), its clip vector and how the test found it. Billboards (shader type 1B) are drawn as placed for now.
void ps2e_graphics_draw_placed_model(ps2e_u32 model, ps2e_u32 viewAddress, ps2e_u32 world) {
    const Mem mem;
    const auto* rigid = mem.Get<te_gfx_rigid_model>(model);
    const auto* cv = mem.Get<ChunkView>(viewAddress);
    if (!rigid || !cv || !te::gfx::EnsureDevice()) return;
    g_seams.placed++;
    Geometry::View v;
    v.screen = ToMatrix(cv->toScreen);
    v.clip = ToMatrix(cv->clipped);
    v.guard = {cv->clip[0], cv->clip[1], cv->clip[2], cv->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = cv->visibility != DrawUnclipped;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    if (rigid->billboard) {
        // (once: Once() builds a std::string key -- a heap allocation for this 70-character text -- on every billboard draw)
        static bool said = false;
        if (!said) {
            said = true;
            Once("DrawPlacedModel: a billboard model drawn without its turn to the camera");
        }
    }
    if (g_diag.Logging()) {
        const auto w = ReadMatrix(world);
        if (w) std::fprintf(stderr, "[te.gfx.diag] present %ld placed view: model %#x world row0 (%.4f %.4f %.4f) row1 (%.4f %.4f %.4f) row2 (%.4f %.4f %.4f) row3 (%.4f %.4f %.4f) visibility %u clip (%.2f %.2f %.2f %.2f)\n",
                               g_diag.present, model, (*w)[0][0], (*w)[0][1], (*w)[0][2], (*w)[1][0], (*w)[1][1], (*w)[1][2], (*w)[2][0], (*w)[2][1], (*w)[2][2],
                               (*w)[3][0], (*w)[3][1], (*w)[3][2], unsigned(cv->visibility), cv->clip[0], cv->clip[1], cv->clip[2], cv->clip[3]);
    }
    InstanceState inst;
    if (rigid->needsEye) {   // the env-map passes (models.cpp: the camera through the placed instance's inverse)
        const auto w = ReadMatrix(world);
        const RenderView* view = CurrentView();
        if (w && view) inst.eye = EyeIn(*w, view->toCamera);
    }
    // the interpolator's identity: this model at this placement (the placed instance's own matrix address: stable while the
    // chunk lives, one per placement); the instance's world matrix is the one the chunk view's screen matrix stands for under
    // the current camera (the view carries the chunk's matrix; the game's `world` is chunk-local)
    // (the matrices live in the frame arena, recycled per frame: the same offsets recur across chunks, so the chunk view is
    // part of the identity; within a chunk the array's order is the placement's)
    if (te::gfx::Interpolating())
        if (const auto w = WorldOfScreen(v.screen)) TagRigid(TagId(model, viewAddress), *w, v.screen);   // (the chunk view's placements of the model one identity: paired by nearness, not draw order)
    DrawInstance(rigid, v, inst, te::gfx::Scene().placed_models);
    te::gfx::ClearDrawTag();
}

// ---- the sky (sky.cpp DrawSky): the sky's models around the camera, first in the frame, the depth left at the far plane ----
void ps2e_gfx2_draw_sky(ps2e_u32 skyAddress, ps2e_u32 viewAddress, ps2e_u32 cameraAddress) {
    const auto* sky = Guest<te_gfx_sky>(skyAddress);
    const auto* view = Guest<RenderView>(viewAddress);
    const auto camera = ReadVector(cameraAddress);
    const auto* target = te::gfx::CurrentTarget();
    if (!sky || !view || !camera || !te::gfx::EnsureDevice()) return;
    if (sky->count == 0 || sky->count > 64) return;
    const uint32_t* models = reinterpret_cast<const uint32_t*>(te::GuestPointer(sky->models, 4 * size_t(sky->count)));
    if (!models) return;
    // The PS2 drew into a half-size buffer with the view's matrix to it; here the frame's own clip-to-screen matrix
    Matrix clipToScreen = target ? ToMatrix(target->clipToScreen) : Identity();
    if (!target) {
        const float w = float(te::gfx::ScreenW()), h = float(te::gfx::ScreenH());
        clipToScreen = Matrix{{{w * 8.f, 0, 0, 0}, {0, -h * 8.f, 0, 0}, {0, 0, -8388607.f, 0}, {2048.f * 16.f, 2048.f * 16.f, 8388607.f, 1.f}}};
    }
    const Matrix translation = Translation(*camera);
    const Matrix toClip = Multiply(translation, ToMatrix(view->toClip));
    const Matrix toScreen = Multiply(toClip, clipToScreen);
    Geometry::View v;
    v.screen = toScreen;
    v.clip = toClip;
    v.guard = {view->clip[0], view->clip[1], view->clip[2], view->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = true;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    auto& scene = te::gfx::Scene();
    for (uint32_t i = 0; i < sky->count; ++i) {
        const auto* rigid = Guest<te_gfx_rigid_model>(models[i]);
        if (!rigid) continue;
        // the interpolator's identity: the sky's i-th model, its world the camera's place (it turns with the camera, no parallax)
        if (te::gfx::Interpolating())
            if (const auto w = WorldOfScreen(toScreen)) TagRigid(TagId(skyAddress, models[i]), *w, toScreen);
        // Drawn in the sky bucket under the materials' own blend state, the depth test ALWAYS and the depth not written
        const te_gfx_model* data = rigid->model ? Guest<te_gfx_model>(rigid->model) : nullptr;
        auto it = g_models.find(rigid->model);
        if (!data || it == g_models.end()) { scene.submodels_missing++; continue; }
        const uint32_t count = std::min<uint32_t>(rigid->count, data->subModelCount);
        const uint32_t* materials = count ? reinterpret_cast<const uint32_t*>(te::GuestPointer(rigid->materials, 4 * size_t(count))) : nullptr;
        static Emitter emitter;
        for (uint32_t p = 0; p < count && p < it->second.parts.size(); ++p) {
            const Part& part = it->second.parts[p];
            if (!part.decoded) continue;
            const uint32_t resource = materials ? materials[p] : 0;
            const uint32_t* materialAddress = resource ? Guest<uint32_t>(resource + 8) : nullptr;
            const MaterialView m = te::gfx::ReadMaterial(materialAddress ? *materialAddress : 0);
            ps2e_draw_state state = te::gfx::StateOf(m);
            state.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
            state.depth_write = 0;
            emitter.Begin(BucketSky, state);
            Geometry::View sub = v;
            if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) sub.scroll = {m.shader->scroll[0], m.shader->scroll[1]};
            if (Deferrable(false)) {
                // built on a worker as the scene's models are (the placeholder keeps its place in the sky bucket); the Vita:
                // through the part's native mesh's chunks (ChunkedBuild) once the mesh is made -- asked for here
                uint32_t chunkNative = 0;
                bool chunkChunked = false;
#if defined(__vita__)
                if (te::gfx::NativeMeshes() && !g_diag.flat && g_chunkedNative.load(std::memory_order_relaxed)) {
                    if (part.meshState == 3) ResolveMeshBatches();
                    if (part.meshState == 0) {
                        part.meshState = 1;
                        g_meshRequests.emplace_back(rigid->model, p);
                    }
                    chunkNative = part.native;
                    chunkChunked = part.native && part.chunked;
                    if (!part.native) g_chunkCensus[1].whole_cpu.fetch_add(1, std::memory_order_relaxed);
                }
#endif
                te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(BucketSky, emitter.state);
#if defined(__vita__)
                if (chunkNative && Opts().repeatCensus)
                    NoteBuildInputs(2, ChunkKey(&part, chunkChunked, chunkNative, sub, nullptr, te::gfx::GsOriginX(), te::gfx::GsOriginY(),
                                                emitter.textured));
#endif
                te::gfx::PostBuild(build, [&build, mesh = &part, sub, state = BuildState(), textured = emitter.textured, chunkNative,
                                           chunkChunked, ox = te::gfx::GsOriginX(), oy = te::gfx::GsOriginY()] {
#if defined(__vita__)
                    if (chunkNative) {
                        ChunkedBuild(build, *mesh, chunkChunked, chunkNative, sub, std::nullopt, ox, oy, state, textured, 1);
                        return;
                    }
#else
                    (void)chunkNative; (void)chunkChunked; (void)ox; (void)oy;
#endif
                    TE_PROF_SCOPE("sky MeshBuild");
                    Collector collector{&build, textured};
                    if (te::gfx::MeshBuild(mesh->prepared, sub, state, nullptr, &Collector::Sink, &collector) == Geometry::Result::Ok)
                        g_deferredModelTriangles += collector.triangles;
                });
                continue;
            }
            Geometry::Result result;
            const size_t triangles = emitter.Build(part, sub, nullptr, &result);
            if (result != Geometry::Result::Ok) continue;
            scene.model_triangles += triangles;
        }
        scene.sky_models++;
    }
    te::gfx::ClearDrawTag();
    // FinishSky drew the half-size buffer over the screen with the depth written at the far plane: the depth cleared here
    ps2e_draw_state clear{};
    clear.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    clear.depth_write = 1;
    clear.no_colour_write = 1;
    const float w = float(te::gfx::ScreenW()), h = float(te::gfx::ScreenH());
    auto V = [](float x, float y) {
        ps2e_draw_vertex o{};
        o.x = x; o.y = y; o.z = 0.f; o.a = 0x80;
        return o;
    };
    Draw d;
    d.state = clear;
    d.verts = {V(0, 0), V(w, 0), V(0, h), V(w, 0), V(w, h), V(0, h)};
    te::gfx::Record(BucketSky, std::move(d));
}

}  // extern "C"

namespace {
// ---- the shadows (shadows.cpp StartShadows / DrawShadowMesh / ApplyShadows) -----------------------------------------------------
// The PS2 counted the shadow volumes in a half-size accumulator: the byte at 120, every type-0B volume strip adding its
// winding colour (+8 / 248 = -8) where it passed the depth test against the scene, then the frame darkened by a quarter of
// 0x84 (33 levels) where the count was 1..16 (the engine's ShadowDraw is the step list). Here the frame's
// stencil is that counter through ps2e_draw: StartShadows clears it, each strip or clipped fan counts up or down by its
// authored colour (colour and depth not written, depth GEQUAL against the scene), ApplyShadows draws the full-frame
// composite where the count is inside -- so the volume's walls above the ground and the caster himself are NOT darkened (the
// front and back faces cancel there), only the surfaces inside the volume.
constexpr uint8_t kWindingUp = 8, kWindingDown = 248;
constexpr uint8_t kShadowMask = 0x84;      // ShadowDraw::QuantizedByte: the composite's source where the count is inside
constexpr uint8_t kShadowFix = 0x20;       // ALPHA {2, 0, 2, 1, 0x20}: Cd - Cs * 0x20 / 0x80 = Cd - 33

void SetStencil(ps2e_draw_state& st, int value) {
    st.stencil = value;
}

// The volume strips: each published strip / fan counts in the direction its authored colour says
struct ShadowEmitter {
    ps2e_draw_state up{}, down{};
    size_t triangles = 0, odd = 0;
    DiagBox* box = nullptr;
    void Begin() {
        up = ps2e_draw_state{};
        up.gouraud = 0;
        up.depth_test = PS2E_DRAW_DEPTH_GEQUAL;
        up.depth_write = 0;
        up.no_colour_write = 1;
        up.tcc = 1;
        // StartShadows' UndrawnAndNearer -- DATE with DATM 0: a strip counts only where the frame's alpha top bit
        // is 0 (the receivers: materials without FBA); the casters (Crash's skin, FBA on) are left alone. Without it the
        // caster's own body was darkened wherever his volume's front face lay in front of his skin (the reported blotches).
        // TE_GFX_SHADOW_NO_DATE=1 keeps the old counting for A/B.
        static const bool noDate = std::getenv("TE_GFX_SHADOW_NO_DATE") != nullptr;
        up.dest_alpha_test = noDate ? 0 : 1;
        down = up;
        SetStencil(up, PS2E_DRAW_STENCIL_COUNT_UP);
        SetStencil(down, PS2E_DRAW_STENCIL_COUNT_DOWN);
    }
    // One volume strip's vertices as the counting draw's: ShadowGeometry emits the type-0B program's NEGATED depth
    // (2^32 - z, for the GS's inverted depth copy); the frame's depth is the ordinary z, the count taken where the
    // ordinary test fails
    static void Convert(const ps2r::gs::VertexRec* verts, size_t count, uint8_t colour, ps2e_draw_vertex* out) {
        for (size_t i = 0; i < count; ++i) {
            const auto& v = verts[i];
            ps2e_draw_vertex& o = out[i];
            o = ps2e_draw_vertex{};
            o.x = v.x;
            o.y = v.y;
            o.z = v.z > 0.f ? 4294967296.f - v.z : 0.f;
            o.r = o.g = o.b = colour;
            o.a = 0x80;
        }
    }
    void Append(const ps2r::gs::VertexRec* verts, size_t count) {
        count = count / 3 * 3;
        if (!count) return;
        const uint8_t colour = verts[0].r;
        if (colour != kWindingUp && colour != kWindingDown) odd++;
        const ps2e_draw_state& state = colour == kWindingDown ? down : up;
        Draw& d = te::gfx::OpenDraw(BucketShadows, state);
        const size_t base = d.verts.size();
        d.verts.resize(base + count);
        Convert(verts, count, colour, d.verts.data() + base);
        if (box)
            for (size_t i = 0; i < count; ++i) box->Add(d.verts[base + i]);
        te::gfx::CloseDraw(BucketShadows);
        triangles += count / 3;
    }
    static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool fan) {
        (void)fan;
        static_cast<ShadowEmitter*>(user)->Append(verts, count);
    }
};
ShadowEmitter g_shadowEmitter;
bool g_shadowsOpen = false;
// The cost of a body on the game thread, added to a counter on scope exit
struct Timed {
    double& sink;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit Timed(double& s) : sink(s) {}
    ~Timed() { sink += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
};
ShadowModel& ShadowModelOf(uint32_t model, const Model& m) {
    ShadowModel& sm = g_shadowModels[model];
    if (sm.tried) return sm;
    sm.tried = true;
    sm.parts.resize(m.sources.size());
    sm.decoded.assign(m.sources.size(), false);
    for (size_t i = 0; i < m.sources.size(); ++i) {
        const PartSource& src = m.sources[i];
        if (src.packet.empty()) continue;
        const auto r = title::ShadowGeometry::Decode(src.packet.data(), src.packet.size(), uint32_t(src.vertices), sm.parts[i]);
        sm.decoded[i] = r == title::ShadowGeometry::Result::Ok;
        if (!sm.decoded[i]) {
            static int said = 0;
            if (said++ < 8) std::fprintf(stderr, "[te.gfx] shadow model %#x part %zu: ShadowGeometry refused the packet (%d)\n", model, i, int(r));
        }
    }
#if defined(__vita__)
    // the parts' counts and distinct positions (the builders' census), and their shared indexes (BuildRunsShared)
    sm.info.assign(sm.parts.size(), ShadowPartInfo{});
    for (size_t i = 0; i < sm.parts.size(); ++i) {
        if (!sm.decoded[i]) continue;
        ShadowPartInfo& info = sm.info[i];
        std::vector<std::array<uint32_t, 3>> positions;
        for (const auto& strip : sm.parts[i].strips) {
            for (const auto& v : strip.vertices) {
                std::array<uint32_t, 3> bits;
                std::memcpy(bits.data(), v.position.data(), 12);
                positions.push_back(bits);
            }
            info.vertices += uint32_t(strip.vertices.size());
        }
        info.strips = uint32_t(sm.parts[i].strips.size());
        std::sort(positions.begin(), positions.end());
        info.unique = uint32_t(std::unique(positions.begin(), positions.end()) - positions.begin());
    }
    sm.shared.assign(sm.parts.size(), title::ShadowGeometry::Shared{});
    for (size_t i = 0; i < sm.parts.size(); ++i)
        if (sm.decoded[i] && title::ShadowGeometry::Share(sm.parts[i], sm.shared[i]) != title::ShadowGeometry::Result::Ok)
            sm.shared[i] = title::ShadowGeometry::Shared{};
#endif
    return sm;
}
size_t g_shadowVolumes = 0;   // strips counted since StartShadows (no composite for an empty bracket)
// TE_GFX_SHADOWS_OFF=1: no volume built, no counter cleared, nothing composited (the game's shadow calls still run and
// the shadows' matrices are still written) -- the reference the shadows' host cost is measured against. Read at the first
// call (the Vita sets the TE_ variables from its arguments file in main, after static initialisation).
bool ShadowsOff() {
    static const bool off = std::getenv("TE_GFX_SHADOWS_OFF") != nullptr;
    return off;
}

// A full-frame quad in the shadows' bucket under `state`
void ShadowQuad(const ps2e_draw_state& state, uint8_t colour, uint8_t alpha) {
    const float w = float(te::gfx::ScreenW()), h = float(te::gfx::ScreenH());
    auto V = [&](float x, float y) {
        ps2e_draw_vertex o{};
        o.x = x; o.y = y; o.z = 0.f;
        o.r = o.g = o.b = colour; o.a = alpha;
        return o;
    };
    Draw d;
    d.state = state;
    d.verts = {V(0, 0), V(w, 0), V(0, h), V(w, 0), V(w, h), V(0, h)};
    te::gfx::Record(BucketShadows, std::move(d));
}
}  // namespace

extern "C" {

void ps2e_graphics_set_up_shadow_pass(void) { Once("SetUpShadowPass: the shadows' palette slot has no meaning here (the stencil counts the volumes)"); }

void ps2e_graphics_begin_shadows(void) {
    const RenderView* view = CurrentView();
    const auto* target = te::gfx::CurrentTarget();
    if (!view) return;
    const Matrix toCamera = ToMatrix(view->toClip);
    const Matrix toScreen = Multiply(toCamera, target ? ToMatrix(target->clipToScreen) : Identity());
    WriteMatrix(te::RetailAddress(kShadowToCamera), toCamera);
    WriteMatrix(te::RetailAddress(kShadowToScreen), toScreen);
    g_shadowsOpen = true;
    g_shadowVolumes = 0;
    g_shadowEmitter.Begin();
    if (!ShadowsOff() && te::gfx::EnsureDevice()) {
        // StartShadows: the counter's origin over the whole frame (a submit that draws nothing)
        ps2e_draw_state clear{};
        clear.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
        clear.depth_write = 0;
        clear.no_colour_write = 1;
        SetStencil(clear, PS2E_DRAW_STENCIL_CLEAR);
        ShadowQuad(clear, 0, 0);
    }
}

void ps2e_graphics_end_shadows(void) {
    if (!g_shadowsOpen) return;
    g_shadowsOpen = false;
    if (!g_shadowVolumes || !te::gfx::EnsureDevice()) return;
    // TE_GFX_NO_SHADOWS=1: the volumes are counted but never composited (the shadow_caster_judge's reference frame)
    static const bool off = std::getenv("TE_GFX_NO_SHADOWS") != nullptr;
    if (off) return;
    // ApplyShadows: Cd - 0x84 * 0x20 / 0x80 where the count is 1..16; the depth untouched
    ps2e_draw_state composite{};
    composite.gouraud = 0;
    composite.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    composite.depth_write = 0;
    composite.blend = 1;
    composite.blend_a = 2; composite.blend_b = 0; composite.blend_c = 2; composite.blend_d = 1;
    composite.blend_fix = kShadowFix;
    composite.tcc = 1;
    SetStencil(composite, PS2E_DRAW_STENCIL_INSIDE);
    // diagnosis: TE_GFX_SHADOW_DIAG=1 composites without the stencil test (the whole frame darkens: the blend path alone)
    static const bool diag = std::getenv("TE_GFX_SHADOW_DIAG") != nullptr;
    if (diag) SetStencil(composite, PS2E_DRAW_STENCIL_OFF);
    ShadowQuad(composite, kShadowMask, 0x80);
    te::gfx::Scene().shadow_passes++;
}

void ps2e_graphics_draw_shadow_mesh(ps2e_u32 mesh, ps2e_u32 toScreen, ps2e_u32 toCamera, ps2e_u32 world) {
    te::gfx::CountCall(te::gfx::kCallShadowMesh);
    if (ShadowsOff()) return;
    Timed timed(te::gfx::Scene().shadow_ms);
    const auto* rigid = Guest<te_gfx_rigid_model>(mesh);
    const auto screen = ReadMatrix(toScreen);
    const auto camera = ReadMatrix(toCamera);
    const RenderView* view = CurrentView();
    if (!rigid || !screen || !camera || !view || !te::gfx::EnsureDevice()) return;
#if defined(__vita__)
    if (Opts().noDraw) return;   // (read once: a function-local static's guard is a DMB per call)
#else
    static const bool noDraw = std::getenv("TE_GFX_NO_DRAW") != nullptr;
    if (noDraw) return;
#endif
    Geometry::View v;
    v.screen = *screen;
    v.clip = *camera;
    v.guard = {view->clip[0], view->clip[1], view->clip[2], view->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = true;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    auto& scene = te::gfx::Scene();
    const te_gfx_model* data = rigid->model ? Guest<te_gfx_model>(rigid->model) : nullptr;
#if defined(__vita__)
    Model* found = data ? FindModel(rigid->model) : nullptr;   // (the models' direct-mapped slot, as DrawInstance's)
    if (!data || !found) { scene.submodels_missing++; return; }
    const Model& modelEntry = *found;
#else
    auto it = g_models.find(rigid->model);
    if (!data || it == g_models.end()) { scene.submodels_missing++; return; }
    const Model& modelEntry = it->second;
#endif
    if (!g_shadowsOpen) g_shadowEmitter.Begin();   // a mesh outside a bracket: counted into the frame's last clear
    const uint32_t count = std::min<uint32_t>(rigid->count ? rigid->count : data->subModelCount, data->subModelCount);
    const bool logging = g_diag.Logging();
    DiagBox box;
    g_shadowEmitter.box = logging ? &box : nullptr;
    const size_t before = g_shadowEmitter.triangles, oddBefore = g_shadowEmitter.odd;
    ShadowModel& sm = ShadowModelOf(rigid->model, modelEntry);
    title::ShadowGeometry::View sv;
    sv.screen = v.screen;
    sv.clip = v.clip;
    sv.guard = v.guard;
    sv.offset = {te::gfx::GsOriginX(), te::gfx::GsOriginY()};
    // the interpolator's identity: this volume mesh at this instance's world matrix (a rigid object, re-projected with it)
    if (te::gfx::Interpolating())
        if (const auto w = WorldOfScreen(*screen)) TagRigid(OrdinalId(TagId(mesh, 0)), *w, *screen, FamilyOf(TagId(mesh, 0)));
    if (Deferrable(logging)) {
        // the volumes built on a worker: each part's strips in their order, each its counting-up or counting-down draw
        bool posted = false;
#if defined(__vita__)
        // THE MESH'S PARTS IN ONE BUILD (as the blend skins' parts): a placeholder, a deferred build and a posted
        // job (its 150-byte view copied in) per part were most of the 19 us a shadow mesh cost the device's game thread (measured
        // hub: 17 meshes a frame, 1.0 %); one job builds the parts in order into one build -- the same segments, each with its
        // up / down state, in the same order, replayed through the same joining rule by the bucket's expansion. (The shadow
        // model's parts and their decode flags are fixed once decoded; the model is released only after the builds drain.)
        const uint32_t parts = std::min<uint32_t>(count, uint32_t(sm.parts.size()));
        if (Opts().lateSeams) {
        for (uint32_t p = 0; p < parts && !posted; ++p) posted = sm.decoded[p];
        if (posted) {
            te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(BucketShadows, g_shadowEmitter.up);
            build.alt_state = g_shadowEmitter.down;
            if (Opts().repeatCensus) {
                // the census: a build whose inputs were one of the picture before's
                uint64_t key = HashBytes(1469598103934665603ull, &rigid->model, sizeof rigid->model);
                key = HashBytes(key, &parts, sizeof parts);
                key = HashBytes(key, sv.screen.data(), sizeof sv.screen);
                key = HashBytes(key, sv.clip.data(), sizeof sv.clip);
                key = HashBytes(key, sv.guard.data(), sizeof sv.guard);
                key = HashBytes(key, sv.offset.data(), sizeof sv.offset);
                NoteBuildInputs(0, key);
            }
            te::gfx::PostBuild(build, [&build, model = &sm, parts, sv, mesh, sharedMode = Opts().shadowShared] {
                TE_PROF_SCOPE("shadow build");
                thread_local std::vector<ps2r::gs::VertexRec> runVerts;
                thread_local std::vector<uint32_t> runEnds;
                size_t triangles = 0, odd = 0;
                // the builders' census: the work in, the shared builds' work, the vertices out
                uint64_t cParts = 0, cStrips = 0, cVertices = 0, cUnique = 0, cCompared = 0, cDiffer = 0;
                title::ShadowGeometry::SharedCensus sc;
                const size_t out0 = build.verts.size();
                for (uint32_t p = 0; p < parts; ++p) {
                    if (!model->decoded[p]) continue;
                    if (p < model->info.size()) {
                        const ShadowPartInfo& info = model->info[p];
                        cParts++;
                        cStrips += info.strips;
                        cVertices += info.vertices;
                        cUnique += info.unique;
                    }
                    // the part's distinct positions once, the strips outside the clip volume skipped (BuildRunsShared: the
                    // groups BuildRuns gives, bit for bit); TE_GFX_SHADOW_SHARED=0 BuildRuns, =2 both, compared
                    const bool shared = sharedMode && p < model->shared.size() && !model->shared[p].index.empty();
                    const auto r = shared ? title::ShadowGeometry::BuildRunsShared(model->parts[p], model->shared[p], sv, runVerts, runEnds, &sc)
                                          : title::ShadowGeometry::BuildRuns(model->parts[p], sv, runVerts, runEnds);
                    if (shared && sharedMode == 2) {
                        thread_local std::vector<ps2r::gs::VertexRec> checkVerts;
                        thread_local std::vector<uint32_t> checkEnds;
                        const auto r2 = title::ShadowGeometry::BuildRuns(model->parts[p], sv, checkVerts, checkEnds);
                        cCompared++;
                        const bool same = r2 == r && checkEnds == runEnds && checkVerts.size() == runVerts.size() &&
                                          (runVerts.empty() || std::memcmp(checkVerts.data(), runVerts.data(),
                                                                           runVerts.size() * sizeof(ps2r::gs::VertexRec)) == 0);
                        if (!same) {
                            cDiffer++;
                            static std::atomic<int> said{0};
                            if (said++ < 8)
                                std::fprintf(stderr, "[te.gfx] SHADOW SHARED CHECK: mesh %#x part %u differs (results %d / %d, vertices %zu / %zu, "
                                             "groups %zu / %zu)\n", mesh, p, int(r), int(r2), runVerts.size(), checkVerts.size(),
                                             runEnds.size(), checkEnds.size());
                        }
                    }
                    if (r != title::ShadowGeometry::Result::Ok) continue;
                    size_t at = 0;
                    for (const uint32_t end : runEnds) {
                        const ps2r::gs::VertexRec* d = runVerts.data() + at;
                        const size_t n = (end - at) / 3 * 3;
                        at = end;
                        if (!n) continue;
                        const uint8_t colour = d[0].r;
                        if (colour != kWindingUp && colour != kWindingDown) odd++;
                        const size_t base = build.verts.size();
                        build.verts.resize(base + n);
                        ShadowEmitter::Convert(d, n, colour, build.verts.data() + base);
                        build.segments.push_back(uint32_t(n));
                        build.alternate.push_back(colour == kWindingDown ? 1 : 0);
                        triangles += n / 3;
                    }
                }
                g_deferredModelTriangles += triangles;
                {
                    BuildersCensus& bc = g_builders;
                    const uint64_t out = build.verts.size() - out0;
                    bc.shadow_builds.fetch_add(1, std::memory_order_relaxed);
                    bc.shadow_parts.fetch_add(cParts, std::memory_order_relaxed);
                    bc.shadow_strips.fetch_add(cStrips, std::memory_order_relaxed);
                    bc.shadow_vertices.fetch_add(cVertices, std::memory_order_relaxed);
                    bc.shadow_unique.fetch_add(cUnique, std::memory_order_relaxed);
                    bc.shadow_positions.fetch_add(sc.positions, std::memory_order_relaxed);
                    bc.shadow_projected.fetch_add(sc.projected, std::memory_order_relaxed);
                    bc.shadow_skipped.fetch_add(sc.strips_skipped, std::memory_order_relaxed);
                    bc.shadow_cert.fetch_add(sc.certified, std::memory_order_relaxed);
                    bc.shadow_clamped.fetch_add(sc.exact, std::memory_order_relaxed);
                    bc.shadow_cert_differ.fetch_add(sc.check_differ, std::memory_order_relaxed);
                    bc.shadow_compared.fetch_add(cCompared, std::memory_order_relaxed);
                    bc.shadow_differ.fetch_add(cDiffer, std::memory_order_relaxed);
                    bc.shadow_out.fetch_add(out, std::memory_order_relaxed);
                    if (!out) bc.shadow_empty.fetch_add(1, std::memory_order_relaxed);
                }
                if (odd) {
                    static std::atomic<int> said{0};
                    if (said++ < 4) std::fprintf(stderr, "[te.gfx] shadow mesh %#x: %zu strips without a +8/-8 winding colour\n", mesh, odd);
                }
            });
        }
        } else
#endif
        for (uint32_t p = 0; p < count && p < sm.parts.size(); ++p) {
            if (!sm.decoded[p]) continue;
            te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(BucketShadows, g_shadowEmitter.up);
            build.alt_state = g_shadowEmitter.down;
            te::gfx::PostBuild(build, [&build, part = &sm.parts[p], sv, mesh] {
                TE_PROF_SCOPE("shadow build");
                // Build's groups as runs of one per-thread array (no DrawIr, record snapshot or vector per group)
                thread_local std::vector<ps2r::gs::VertexRec> runVerts;
                thread_local std::vector<uint32_t> runEnds;
                const auto r = title::ShadowGeometry::BuildRuns(*part, sv, runVerts, runEnds);
                if (r == title::ShadowGeometry::Result::Ok) {
                    size_t triangles = 0, odd = 0, at = 0;
                    for (const uint32_t end : runEnds) {
                        const ps2r::gs::VertexRec* d = runVerts.data() + at;
                        const size_t n = (end - at) / 3 * 3;
                        at = end;
                        if (!n) continue;
                        const uint8_t colour = d[0].r;
                        if (colour != kWindingUp && colour != kWindingDown) odd++;
                        const size_t base = build.verts.size();
                        build.verts.resize(base + n);
                        ShadowEmitter::Convert(d, n, colour, build.verts.data() + base);
                        build.segments.push_back(uint32_t(n));
                        build.alternate.push_back(colour == kWindingDown ? 1 : 0);
                        triangles += n / 3;
                    }
                    g_deferredModelTriangles += triangles;
                    if (odd) {
                        static std::atomic<int> said{0};
                        if (said++ < 4) std::fprintf(stderr, "[te.gfx] shadow mesh %#x: %zu strips without a +8/-8 winding colour\n", mesh, odd);
                    }
                }
            });
            posted = true;
        }
        te::gfx::ClearDrawTag();
        g_shadowEmitter.box = nullptr;
        if (posted) g_shadowVolumes++;   // the composite runs; with no volume drawn its stencil test passes nowhere
        scene.shadow_meshes++;
        return;
    }
    static std::vector<ps2r::gs::DrawIr> volumes;
    for (uint32_t p = 0; p < count && p < sm.parts.size(); ++p) {
        if (!sm.decoded[p]) continue;
        volumes.clear();
        const auto r = title::ShadowGeometry::Build(sm.parts[p], {sv}, volumes);
        if (r != title::ShadowGeometry::Result::Ok) continue;
        for (const auto& d : volumes) g_shadowEmitter.Append(d.verts.data(), d.verts.size());
    }
    te::gfx::ClearDrawTag();
    g_shadowEmitter.box = nullptr;
    const size_t triangles = g_shadowEmitter.triangles - before;
    if (g_shadowEmitter.odd != oddBefore) {
        scene.shadow_odd_colours += g_shadowEmitter.odd - oddBefore;
        static int said = 0;
        if (said++ < 4) std::fprintf(stderr, "[te.gfx] shadow mesh %#x: %zu strips without a +8/-8 winding colour\n", mesh, g_shadowEmitter.odd - oddBefore);
    }
    if (logging)
        std::fprintf(stderr, "[te.gfx.diag] present %ld shadow model %#x parts %u tris %zu box (%.0f %.0f)-(%.0f %.0f)\n", g_diag.present,
                     rigid->model, count, triangles, box.x0, box.y0, box.x1, box.y1);
    scene.model_triangles += triangles;
    if (triangles) g_shadowVolumes++;
    scene.shadow_meshes++;
}

// The chunk's planes from the camera's place in it: the view's planes and the far set (the retail arrays SetViewFrustum fills)
void ps2e_graphics_load_chunk_planes(ps2e_u32 place) {
    const auto matrix = ReadMatrix(place);
    const auto view = ReadPlanes(te::RetailAddress(kViewPlanes)), far = ReadPlanes(te::RetailAddress(kFarViewPlanes));
    if (!matrix || !view || !far) return;
    g_cull.planes = title::CullingPlanes::InChunk(*view, *far, *matrix);
#if defined(__vita__)
    NoteFrustum();   // (the culling memo: the frustum's id by its content)
#endif
    static int said = 0;
    if (te::gfx::LogEnabled() && said++ < 3)
        std::fprintf(stderr, "[te.gfx] LoadChunkPlanes: near (%g %g %g %g) right (%g %g %g %g) far (%g %g %g %g); far-set right (%g %g %g %g)\n",
                     (*view)[0][0], (*view)[0][1], (*view)[0][2], (*view)[0][3], (*view)[1][0], (*view)[1][1], (*view)[1][2], (*view)[1][3],
                     (*view)[5][0], (*view)[5][1], (*view)[5][2], (*view)[5][3], (*far)[1][0], (*far)[1][1], (*far)[1][2], (*far)[1][3]);
    g_cull.cell.reset();
    g_cull.box.reset();
}
void ps2e_graphics_load_portal_planes(ps2e_u32 place, ps2e_u32 portal) {
    const auto matrix = ReadMatrix(place);
    const auto view = ReadPlanes(te::RetailAddress(kViewPlanes)), far = ReadPlanes(te::RetailAddress(kFarViewPlanes)), sides = ReadPlanes(portal);
    if (!matrix || !view || !far || !sides) return;
    g_cull.planes = title::CullingPlanes::ThroughPortal(*view, *far, *matrix, *sides);
#if defined(__vita__)
    NoteFrustum();
#endif
    g_cull.cell.reset();
    g_cull.box.reset();
}
// The view's matrix, the renderer view's matrix to the screen and the camera's place; the link made the identity
void ps2e_graphics_load_culling_view(ps2e_u32 toClip, ps2e_u32 toScreen, ps2e_u32 camera) {
    const auto clip = ReadMatrix(toClip), screen = ReadMatrix(toScreen);
    const auto eye = ReadVector(camera);
    if (!clip || !screen || !eye) return;
    MakeCullMatrices();   // the pending matrices need the link this replaces
    g_cull.view = title::CullingMath::View{*clip, *screen, *eye};
    g_cull.link = title::CullingMath::LoadLink(*g_cull.view, Identity(), Identity());
#if defined(__vita__)
    NoteLink();   // (the culling memo: the link's id by its content)
#endif
    g_cull.model.reset();
    g_cull.cell.reset();
    g_cull.box.reset();
}
void ps2e_graphics_cull_load_link(ps2e_u32 chunk, ps2e_u32 object) {
    const auto c = ReadMatrix(chunk), o = ReadMatrix(object);
    if (!c || !o || !g_cull.view) return;
    MakeCullMatrices();   // the pending matrices need the link this replaces
    g_cull.link = title::CullingMath::LoadLink(*g_cull.view, *c, *o);
#if defined(__vita__)
    NoteLink();
#endif
}
void ps2e_graphics_cull_load_model(ps2e_u32 model) {
    const Mem mem;
    const float* m = mem.Floats(model, 16);
    if (!m || !g_cull.link) return;
#if defined(__vita__)
    // (the culling memo: the same link and model words -> the load's answer as made then)
    if (const CullMemo::ScreenEntry* e = MemoScreen(model, m, true)) g_cull.model = title::CullingMath::Model{e->screen, e->distance};
    else
#endif
        g_cull.model = title::CullingMath::LoadModel(*g_cull.link, ToMatrix(reinterpret_cast<const float(*)[4]>(m)));
    g_cull.toScreen = g_cull.model->to_screen;
    g_cull.screenPending = false;   // (a pending box matrix to the screen is replaced unread; one to the clip space stays)
    g_cull.box.reset();
    g_cull.cell.reset();
    te::gfx::Scene().culls++;
    g_seams.modelLoads++;
}
void ps2e_graphics_cull_test_box(ps2e_u32 corners) {
    const float* c = reinterpret_cast<const float*>(te::GuestPointer(corners, 32));
    if (!c || !g_cull.planes) return;
    title::CullingMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
#if defined(__vita__)
    CullMemo& memo = g_cullMemo;
    if (memo.fid && memo.fstable) {
        // (the culling memo: the same frustum and box words -> the classification as made then)
        memo.cellLooked++;
        const uint32_t fpctl = CullMemo::FpControl();
        CullMemo::CellEntry& e = memo.cells[CullMemo::SlotOf(corners, 0xCE11u, uint32_t(memo.cells.size() - 1))];
        if (e.fid == memo.fid && e.fpctl == fpctl && CullMemo::Same(e.box, c, 8)) {
            memo.cellHits++;
            g_cull.cell = e.cell;
            if (memo.check) {
                const auto made = title::CullingMath::TestBox(*g_cull.planes, box);
                memo.checked++;
                if (made.outside != e.cell.outside || made.clipped != e.cell.clipped) memo.differ++;
            }
        } else {
            g_cull.cell = title::CullingMath::TestBox(*g_cull.planes, box);
            std::memcpy(e.box, c, sizeof e.box);
            e.cell = *g_cull.cell;
            e.fpctl = fpctl;
            e.fid = memo.fid;
        }
    } else
#endif
        g_cull.cell = title::CullingMath::TestBox(*g_cull.planes, box);
    g_cull.box.reset();
    g_seams.cellTests++;
}
void ps2e_graphics_cull_test_box_at(ps2e_u32 boxAddress, ps2e_u32 model) {
    const Mem mem;
    const float* c = mem.Floats(boxAddress, 8);
    const float* mf = mem.Floats(model, 16);
    if (!c || !mf || !g_cull.planes || !g_cull.link) return;
#if defined(__vita__)
    // the box and the model read where they lie (TestBoxAtVisibilityOf: the same operations on the same floats),
    // the model copied for the pending screen / clip matrices only when the box is visible -- the copies of the box (32 B) and
    // the model (64 B) and TestBoxAtVisibility's zeroed 150-byte BoxView were a few hundred cycles of every one of the hub's
    // ~490 box tests a frame (3.4 us each on the device)
    if (g_cullBench.capturing || !FastSeams() || !Opts().lateSeams) {
#endif
    const title::CullingMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    const Matrix m = ToMatrix(reinterpret_cast<const float(*)[4]>(mf));
    g_cull.cell.reset();
    te::gfx::Scene().culls++;
    g_seams.boxTests++;
    if (g_cullBench.capturing) g_cullBench.cases.push_back({*g_cull.planes, *g_cull.link, box, m});
    if (FastSeams()) {
        // the visibility now, the matrices when they are read (MakeCullMatrices)
#if defined(__vita__)
        title::CullingMath::Classification visibility;
        int32_t distance;
        CullMemo& memo = g_cullMemo;
        if (memo.fid && memo.fstable && memo.lid && memo.lstable) {
            // (the culling memo: the same frustum, link and box / model words -> the visibility and distance as made then)
            memo.boxLooked++;
            const uint32_t fpctl = CullMemo::FpControl();
            CullMemo::BoxEntry& e = memo.boxes[CullMemo::SlotOf(boxAddress, model, uint32_t(memo.boxes.size() - 1))];
            if (e.fid == memo.fid && e.lid == memo.lid && e.fpctl == fpctl && CullMemo::Same(e.box, c, 8) && CullMemo::Same(e.model, mf, 16)) {
                memo.boxHits++;
                visibility = e.visibility;
                distance = e.distance;
                if (memo.check) {
                    const auto r = title::CullingMath::TestBoxAtVisibility(*g_cull.planes, *g_cull.link, box, m);
                    memo.checked++;
                    if (r.visibility.outside != visibility.outside || r.visibility.clipped != visibility.clipped || r.distance != distance)
                        memo.differ++;
                }
            } else {
                const auto r = title::CullingMath::TestBoxAtVisibility(*g_cull.planes, *g_cull.link, box, m);
                visibility = r.visibility;
                distance = r.distance;
                std::memcpy(e.box, c, sizeof e.box);
                std::memcpy(e.model, mf, sizeof e.model);
                e.visibility = visibility;
                e.distance = distance;
                e.fpctl = fpctl;
                e.fid = memo.fid;
                e.lid = memo.lid;
            }
        } else {
            const auto r = title::CullingMath::TestBoxAtVisibility(*g_cull.planes, *g_cull.link, box, m);
            visibility = r.visibility;
            distance = r.distance;
            if (te::gfx::g_repeatCensus && memo.fid && memo.lid) {
                // (the census: the same box, model, frustum and link tested earlier in this very picture -- a duplicate)
                CullMemo::BoxEntry& e = memo.boxes[CullMemo::SlotOf(boxAddress, model, uint32_t(memo.boxes.size() - 1))];
                const uint32_t fpctl = CullMemo::FpControl();
                memo.dupLooked++;
                if (e.fid == memo.fid && e.lid == memo.lid && e.fpctl == fpctl && CullMemo::Same(e.box, c, 8) && CullMemo::Same(e.model, mf, 16))
                    memo.dupHits++;
                std::memcpy(e.box, c, sizeof e.box);
                std::memcpy(e.model, mf, sizeof e.model);
                e.visibility = visibility;
                e.distance = distance;
                e.fpctl = fpctl;
                e.fid = memo.fid;
                e.lid = memo.lid;
            }
        }
        g_cull.box = BoxResult{visibility, distance};
        if (visibility.outside) return;
        g_seams.boxVisible++;
        g_cull.screenPending = true;
        g_cull.screenModel = m;
        g_cull.screenAddress = model;
        if (visibility.clipped) {
            g_seams.boxClipped++;
            g_cull.clipPending = true;
            g_cull.clipModel = m;
        }
        return;
#else
        const auto r = title::CullingMath::TestBoxAtVisibility(*g_cull.planes, *g_cull.link, box, m);
        g_cull.box = BoxResult{r.visibility, r.distance};
        if (r.visibility.outside) return;
        g_seams.boxVisible++;
        g_cull.screenPending = true;
        g_cull.screenModel = m;
        if (r.visibility.clipped) {
            g_seams.boxClipped++;
            g_cull.clipPending = true;
            g_cull.clipModel = m;
        }
        return;
#endif
    }
    MakeCullMatrices();
    const auto r = title::CullingMath::TestBoxAtReference(*g_cull.planes, *g_cull.link, box, m);
    g_cull.box = BoxResult{r.visibility, r.distance};
    if (r.to_screen) g_cull.toScreen = *r.to_screen;
    if (r.to_clip) g_cull.toClip = *r.to_clip;
#if defined(__vita__)
        return;
    }
    // the model's 16 floats in place (Matrix is float[4][4]; the guest memory holds the game's floats as written)
    const Matrix& m = *reinterpret_cast<const Matrix*>(mf);
    g_cull.cell.reset();
    te::gfx::Scene().culls++;
    g_seams.boxTests++;
    title::CullingMath::Classification visibility;
    int32_t distance;
    CullMemo& memo = g_cullMemo;
    if (memo.fid && memo.fstable && memo.lid && memo.lstable) {
        // (the culling memo: the same frustum, link and box / model words -> the visibility and distance as made then)
        memo.boxLooked++;
        const uint32_t fpctl = CullMemo::FpControl();
        CullMemo::BoxEntry& e = memo.boxes[CullMemo::SlotOf(boxAddress, model, uint32_t(memo.boxes.size() - 1))];
        if (e.fid == memo.fid && e.lid == memo.lid && e.fpctl == fpctl && CullMemo::Same(e.box, c, 8) && CullMemo::Same(e.model, mf, 16)) {
            memo.boxHits++;
            visibility = e.visibility;
            distance = e.distance;
            if (memo.check) {
                int32_t made_distance = 0;
                const auto made = title::CullingMath::TestBoxAtVisibilityOf(*g_cull.planes, *g_cull.link, c, m, &made_distance);
                memo.checked++;
                if (made.outside != visibility.outside || made.clipped != visibility.clipped || made_distance != distance) memo.differ++;
            }
        } else {
            visibility = title::CullingMath::TestBoxAtVisibilityOf(*g_cull.planes, *g_cull.link, c, m, &distance);
            std::memcpy(e.box, c, sizeof e.box);
            std::memcpy(e.model, mf, sizeof e.model);
            e.visibility = visibility;
            e.distance = distance;
            e.fpctl = fpctl;
            e.fid = memo.fid;
            e.lid = memo.lid;
        }
    } else {
        visibility = title::CullingMath::TestBoxAtVisibilityOf(*g_cull.planes, *g_cull.link, c, m, &distance);
        if (te::gfx::g_repeatCensus && memo.fid && memo.lid) {
            // (the census: the same box, model, frustum and link tested earlier in this very picture -- a duplicate)
            CullMemo::BoxEntry& e = memo.boxes[CullMemo::SlotOf(boxAddress, model, uint32_t(memo.boxes.size() - 1))];
            const uint32_t fpctl = CullMemo::FpControl();
            memo.dupLooked++;
            if (e.fid == memo.fid && e.lid == memo.lid && e.fpctl == fpctl && CullMemo::Same(e.box, c, 8) && CullMemo::Same(e.model, mf, 16))
                memo.dupHits++;
            std::memcpy(e.box, c, sizeof e.box);
            std::memcpy(e.model, mf, sizeof e.model);
            e.visibility = visibility;
            e.distance = distance;
            e.fpctl = fpctl;
            e.fid = memo.fid;
            e.lid = memo.lid;
        }
    }
    g_cull.box = BoxResult{visibility, distance};
    if (visibility.outside) return;
    g_seams.boxVisible++;
    g_cull.screenPending = true;
    g_cull.screenModel = m;   // (the model as it was at the test: the pending matrices are made from it when read)
    g_cull.screenAddress = model;
    if (visibility.clipped) {
        g_seams.boxClipped++;
        g_cull.clipPending = true;
        g_cull.clipModel = m;
    }
#endif
}
// CullOutcome {outside, clipped, distance}: the last test's masks (nonzero = out / partly out), the distance of the last
// model or box test (vf15 keeps it across a cell test)
void ps2e_graphics_cull_result(ps2e_u32 out) {
    uint32_t* o = reinterpret_cast<uint32_t*>(te::GuestPointer(out, 12));
    if (!o) return;
    g_seams.results++;
    const SceneOptions& opts = Opts();
    if (opts.cullAllOut) { o[0] = 1; o[1] = 0; o[2] = 0; return; }   // diagnosis: the empty answer (nothing in view)
    // diagnosis: everything wholly in view
    if (opts.cullAllIn) { o[0] = 0; o[1] = 0; o[2] = g_cull.box ? uint32_t(g_cull.box->distance) : g_cull.model ? uint32_t(g_cull.model->distance) : 0; return; }
    uint32_t outside = 0, clipped = 0;
    int32_t distance = g_cull.model ? g_cull.model->distance : 0;
    // diagnosis: CullTestBoxAt (the instances) out
    if (g_cull.box && opts.cullBoxesOut) { o[0] = 1; o[1] = 0; o[2] = uint32_t(g_cull.box->distance); return; }
    if (g_cull.box) {
        outside = g_cull.box->visibility.outside;
        clipped = g_cull.box->visibility.clipped;
        distance = g_cull.box->distance;
    } else if (g_cull.cell) {
        outside = g_cull.cell->outside;
        clipped = g_cull.cell->clipped;
    } else if (!g_cull.view || !g_cull.planes) {
        outside = 1;   // nothing loaded: nothing in view
    }
    o[0] = outside;
    o[1] = clipped;
    o[2] = uint32_t(distance);
}
void ps2e_graphics_cull_matrices(ps2e_u32 clipped, ps2e_u32 toScreen) {
    MakeCullMatrices();
    g_seams.matrixReads++;
    if (clipped) WriteMatrix(clipped, g_cull.toClip);
    if (toScreen) WriteMatrix(toScreen, g_cull.toScreen);
}

// ---- ClipBox / ClipBoxAt (culling.cpp macro mode; the engine's ClippingMath): the corners through the view's matrix (and a
// model's), scaled by the row after the matrix; flags[0] = every corner's bits together, flags[1] = their common bits -----------
void ps2e_graphics_clip_box(ps2e_u32 view, ps2e_u32 boxAddress, ps2e_u32 flags) {
    const auto matrix = ReadMatrix(view);
    const auto guard = ReadVector(view + 0x40);
    const float* c = reinterpret_cast<const float*>(te::GuestPointer(boxAddress, 32));
    uint32_t* f = reinterpret_cast<uint32_t*>(te::GuestPointer(flags, 8));
    if (!f) return;
    if (!matrix || !guard || !c) { f[0] = 0x3F; f[1] = 0x3F; return; }
    title::BoundsMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    const auto result = title::ClippingMath::ClipBox(box, *matrix, *guard);
    f[0] = result.any;
    f[1] = result.common;
}
void ps2e_graphics_clip_box_at(ps2e_u32 view, ps2e_u32 boxAddress, ps2e_u32 flags, ps2e_u32 model) {
    const auto matrix = ReadMatrix(view);
    const auto guard = ReadVector(view + 0x40);
    const auto m = ReadMatrix(model);
    const float* c = reinterpret_cast<const float*>(te::GuestPointer(boxAddress, 32));
    uint32_t* f = reinterpret_cast<uint32_t*>(te::GuestPointer(flags, 8));
    if (!f) return;
    if (!matrix || !guard || !c || !m) { f[0] = 0x3F; f[1] = 0x3F; return; }
    title::BoundsMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    const auto result = title::ClippingMath::ClipBox(box, *matrix, *guard, &*m);
    f[0] = result.any;
    f[1] = result.common;
}

}  // extern "C"

// ---- skins and blend skins: the skinned characters through the engine's SkinGeometry -------------------------------
// skins.cpp DrawSkin / DrawBlendSkin: per submodel its material's state, the skin's matrix times the view's (to the screen;
// to the clip space when drawn clipped), the lights turned into the skin's space, the joints' matrices, and the submodel's
// precompiled VU1 packet -- the engine runs the skinned lit strip program over them as values (SkinGeometry::Build /
// BuildBlend) and the strips come back as triangle lists in frame pixels, recorded into the material's bucket like a rigid
// model's. A skin's packets cross from the wasm side once when read (ps2e_gfx2_skin_loaded: the disk manager may move the
// block) and are kept host-side (the save state carries them); a blend skin's parts' packets live on the game's heap for the
// skin's life and are decoded at the first draw by their address.
namespace {
using SkinGeo = title::SkinGeometry;
// A skin part's GPU-skinned form (the Vita's GPU skinning): its batches' static vertices and triangles made once into a
// skin mesh on the device (the first picture that draws the part asks for it; the CPU builds the part meanwhile), the
// palette's joints (local index -> the skin's joint), and what the clipped draws' test needs
struct SkinGpu {
    uint8_t state = 0;              // 0 not asked for, 1 asked for this picture, 2 made (or not usable), 3 being made
    uint32_t handle = 0;            // the skin mesh (0: none -- not made yet, declined, or the part does not fit)
    std::vector<uint16_t> joints;   // the palette: local joint -> the skin's joint, in the order of first use
    std::vector<float> radius;      // per local joint: the largest bind-pose |position| among the vertices it moves
    bool convex = false;            // every vertex's weights >= 0, summing to 1 (a blend of the joints' balls is inside their hull)
    uint32_t joint_limit = 0;       // the draw's joint count must exceed the largest joint referenced (else the CPU)
    uint32_t triangles = 0;
    bool unfit = false;             // the part's data does not fit the GPU path (SkinMeshOf): always CPU-built
};
struct SkinPart {
    SkinGeo::Part part;
    bool decoded = false;
    int32_t vertices = 0;
    std::vector<uint8_t> packet;   // kept for the save state
    SkinGpu gpu;
};
struct SkinModel { std::vector<SkinPart> parts; };
std::unordered_map<uint32_t, SkinModel> g_skins;   // Skin guest address -> its submodels
struct BlendPartGeo { SkinGeo::Part part; bool decoded = false; bool tried = false; SkinGpu gpu; };
std::unordered_map<uint32_t, std::unordered_map<uint32_t, BlendPartGeo>> g_blendSkins;   // BlendSkin -> part packet address -> geometry

#if defined(__vita__)
// ---- the Vita's GPU skinning ------------------------------------------------------------------------------------------------
// TE_GFX_GPU_SKIN (te::gfx::SetGpuSkins; default on): a plain skin's part is drawn from its skin mesh through the frame's
// bone palette, the GPU doing what FastSkinRuns does per vertex on a builder (graphics.cpp / GlShaderLibrary::SelectSkin);
// a part the GPU path cannot take is built on the CPU as before (the census line says how many and why)
std::atomic<bool> g_gpuSkins{true};
// The blend skins' parts on the GPU too (te::gfx::SetGpuBlend, TE_GFX_GPU_BLEND=1): OFF by default -- a blend skin's
// parts are small (one batch each, at most 64 vertices: the heads' 4 parts) and each would be a skinned draw of its own,
// where their CPU builds join into one draw per state: the Mech fight's 85 parts a frame became 166 draws a frame (with
// the casters' re-stamps), the drawing thread's cost on the device. (One mesh per blend sub-model would make it one draw.)
std::atomic<bool> g_gpuBlend{false};
enum SkinPath { kSkinGpu, kSkinGpuBlend, kSkinBlendCpu, kSkinOff, kSkinWaiting, kSkinUnfit, kSkinJoints, kSkinClipped, kSkinDiag,
                kSkinPaths };
const char* const kSkinPathNames[kSkinPaths] = {"GPU", "GPU blend", "blend part on the CPU (TE_GFX_GPU_BLEND=0)", "off",
                                                "mesh being made", "part does not fit", "joint count",
                                                "clipped, not wholly in front", "diagnosis"};
uint64_t g_skinParts[kSkinPaths] = {};
uint64_t g_skinGpuTriangles = 0;
// the parts asked for this picture (skin, submodel) and the batches being made on the drawing thread (oldest first)
std::vector<std::pair<uint32_t, uint32_t>> g_skinMeshRequests;
std::vector<std::pair<uint32_t, uint32_t>> g_blendMeshRequests;   // (blend skin, part packet): a blend part's mesh
struct SkinMeshBatch {
    std::shared_ptr<te::gfx::MeshBatchJob> job;
    std::vector<SkinGpu*> parts;
    uint64_t ticket = 0;
};
std::vector<SkinMeshBatch> g_skinMeshBatches;
void ResolveSkinBatches() {
    if (g_skinMeshBatches.empty()) return;
    for (SkinMeshBatch& b : g_skinMeshBatches) {
        te::gfx::WaitMeshes(b.ticket, *b.job);
        for (size_t k = 0; k < b.parts.size(); ++k) {
            b.parts[k]->handle = b.job->requests[k].handle;
            b.parts[k]->state = 2;
            if (!b.job->requests[k].handle && g_gpuSkins.exchange(false)) {
                // the device made none (no skinning programs): the skins stay on the CPU from here on
                std::fprintf(stderr, "[te.gfx] GPU skinning: the device declined a skin mesh -- the skins are built on the CPU\n");
            }
        }
    }
    g_skinMeshBatches.clear();
}
// A part's skin mesh handed back (destroyed once no frame still draws it) and its GPU form forgotten
void RetireSkinGpu(SkinGpu& gpu) {
    if (gpu.state == 3) ResolveSkinBatches();
    te::gfx::RetireMesh(gpu.handle);
    gpu = SkinGpu{};
}
void RetireSkinMesh(SkinPart& part) { RetireSkinGpu(part.gpu); }
void RetireSkinMeshes(SkinModel& m) {
    for (SkinPart& part : m.parts) RetireSkinMesh(part);
}

// The part's static vertices and triangles for its skin mesh, as FastSkinRuns reads the batches: per vertex the bind
// position and the normal (QW0 / QW1's ints times the batch's scales), S and T, the bones the flags use (each weight
// word's float value; its low 9 bits the bone's quadword in the palette: a joint's four quadwords), the colour's lanes
// (QW3: the V4-8 unpack's sign-extended ints) and the alpha byte uint8(min(lane, 255)); per batch the strip's triangles
// as Assemble emits them (a vertex with the ADC flag draws none and restarts the winding; the others alternate it; a
// triangle the CPU drops as degenerate on the screen covers no pixel on the GPU either). false when the part does not
// fit: a bone not at a joint's first quadword, more than 64 joints, more than 65536 vertices.
bool SkinMeshOf(const SkinGeo::Part& part, SkinGpu& gpu, te::gfx::MeshRequest& r, bool blend = false) {
    const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
    std::vector<int> local(SkinGeo::kMaxJoints, -1);
    gpu.joints.clear();
    gpu.radius.clear();
    gpu.convex = true;
    gpu.joint_limit = 0;
    size_t vertices = 0;
    for (const auto& batch : part.batches) vertices += batch.Vertices();
    if (vertices < 3 || vertices > 65536) return false;
    // a blend part: one batch, each vertex its place in the draw's displacements (u_disp: at most 64)
    if (blend && (part.batches.size() != 1 || vertices > 64)) return false;
    r.skin_vertices.clear();
    r.skin_vertices.reserve(vertices);
    r.skin_indices.clear();
    r.skin_max_alpha = 0;
    for (const auto& batch : part.batches) {
        const uint32_t count = batch.Vertices();
        if (count < 3 || count > SkinGeo::kMaxBatchVertices || batch.vertices.size() != 4 * size_t(count)) return false;
        const float sx = F(batch.scale[0]), sy = F(batch.scale[1]);
        const size_t base = r.skin_vertices.size();
        unsigned parity = 0;
        for (uint32_t j = 0; j < count; ++j) {
            const auto& q0 = batch.vertices[4 * j];
            const auto& q1 = batch.vertices[4 * j + 1];
            const auto& q2 = batch.vertices[4 * j + 2];
            const auto& q3 = batch.vertices[4 * j + 3];
            ps2e_draw_skin_vertex v{};
            v.x = float(int32_t(q0[0])) * sx;
            v.y = float(int32_t(q0[1])) * sx;
            v.z = float(int32_t(q0[2])) * sx;
            v.nx = float(int32_t(q0[3])) * sx;
            v.ny = float(int32_t(q1[2])) * sy;
            v.nz = float(int32_t(q1[3])) * sy;
            v.s = float(int32_t(q1[0])) * sy;
            v.t = float(int32_t(q1[1])) * sy;
            v.one = 1.f;
            v.r = float(int32_t(q3[0]));
            v.g = float(int32_t(q3[1]));
            v.b = float(int32_t(q3[2]));
            const int alpha = int(uint8_t(std::min<int32_t>(int32_t(q3[3]), 255)));
            v.a = float(alpha);
            r.skin_max_alpha = std::max(r.skin_max_alpha, alpha);
            const uint32_t flags = q2[3] & 0xFFFFu;
            const uint32_t bones = flags & 3u;
            const int used = bones == 1 ? 1 : bones == 2 ? 2 : 3;
            float w[3] = {0.f, 0.f, 0.f};
            uint8_t jl[3] = {0, 0, 0};
            double sum = 0.0;
            const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            for (int b = 0; b < used; ++b) {
                const uint32_t at = q2[b] & 0x1FFu;
                if (at & 3u) return false;   // not a joint's first quadword: the CPU reads across two joints
                const uint32_t joint = at / 4;
                if (joint >= SkinGeo::kMaxJoints) return false;
                if (local[joint] < 0) {
                    if (gpu.joints.size() >= 64) return false;
                    local[joint] = int(gpu.joints.size());
                    gpu.joints.push_back(uint16_t(joint));
                    gpu.radius.push_back(0.f);
                }
                gpu.joint_limit = std::max(gpu.joint_limit, joint + 1);
                w[b] = F(q2[b]);
                jl[b] = uint8_t(local[joint]);
                gpu.radius[size_t(local[joint])] = std::max(gpu.radius[size_t(local[joint])], len);
                if (!(w[b] >= 0.f) || !std::isfinite(w[b])) gpu.convex = false;
                sum += double(w[b]);
            }
            if (std::fabs(sum - 1.0) > 1e-3) gpu.convex = false;
            v.w0 = w[0]; v.w1 = w[1]; v.w2 = w[2];
            v.j0 = jl[0]; v.j1 = jl[1]; v.j2 = jl[2]; v.j3 = 0;
            v.index = blend ? float(j) : 0.f;
            for (float f : {v.x, v.y, v.z, v.nx, v.ny, v.nz, v.s, v.t})
                if (!std::isfinite(f)) return false;
            r.skin_vertices.push_back(v);
            // Assemble: an ADC vertex resets the winding and draws nothing; from the third on the others draw
            if (flags & 0x8000u) {
                parity = 0;
                continue;
            }
            if (j < 2) continue;
            const uint16_t a = uint16_t(base + j - 2), b = uint16_t(base + j - 1), c = uint16_t(base + j);
            if (parity % 2 == 0) {
                r.skin_indices.push_back(a);
                r.skin_indices.push_back(b);
            } else {
                r.skin_indices.push_back(b);
                r.skin_indices.push_back(a);
            }
            r.skin_indices.push_back(c);
            ++parity;
        }
    }
    gpu.triangles = uint32_t(r.skin_indices.size() / 3);
    return gpu.triangles > 0 && !gpu.joints.empty();
}

// A clipped skin drawn on the GPU only when it is wholly in front of the eye and inside the depth range the native
// vertex program takes (as NativeInFront for a rigid part's box): every vertex is a blend (weights >= 0, summing to 1)
// of its joints' images of its bind position, each inside the ball around the joint's translation of radius |3x3|_F x
// the joint's largest bind radius -- so the skin lies in their hull, and an affine function (the screen matrix's w, its
// z, z - 2^31 w) is bounded over it by its bounds over the balls. Doubles, strict.
bool SkinInFront(const SkinGpu& gpu, const SkinGeo::Inputs& in, float slack = 0.f) {
    if (!gpu.convex) return false;
    const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return double(f); };
    double S[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) S[r][c] = F(in.to_screen[size_t(r)][size_t(c)]);
    const double aw[3] = {S[0][3], S[1][3], S[2][3]}, az[3] = {S[0][2], S[1][2], S[2][2]};
    const double ah[3] = {az[0] - 2147483648.0 * aw[0], az[1] - 2147483648.0 * aw[1], az[2] - 2147483648.0 * aw[2]};
    const double bw = S[3][3], bz = S[3][2], bh = S[3][2] - 2147483648.0 * S[3][3];
    const auto norm = [](const double* a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); };
    const double nw = norm(aw), nz = norm(az), nh = norm(ah);
    for (size_t j = 0; j < gpu.joints.size(); ++j) {
        const size_t at = size_t(gpu.joints[j]) * 4;
        if (at + 3 >= in.joints.size()) return false;
        double frob = 0.0, t[3];
        for (int axis = 0; axis < 3; ++axis)
            for (int k = 0; k < 3; ++k) {
                const double m = F(in.joints[at + size_t(axis)][size_t(k)]);
                frob += m * m;
            }
        for (int k = 0; k < 3; ++k) t[k] = F(in.joints[at + 3][size_t(k)]);
        const double rho = std::sqrt(frob) * (double(gpu.radius[j]) + double(slack));
        if (!std::isfinite(rho)) return false;
        const double w = aw[0] * t[0] + aw[1] * t[1] + aw[2] * t[2] + bw;
        const double z = az[0] * t[0] + az[1] * t[1] + az[2] * t[2] + bz;
        const double h = ah[0] * t[0] + ah[1] * t[1] + ah[2] * t[2] + bh;
        if (!(w - nw * rho > 0.0) || !(z - nz * rho >= 0.0) || !(h + nh * rho < 0.0)) return false;
    }
    return true;
}

// The palette of a draw: per local joint its 3x4 transposed (row k = the k-th lanes of its three axes and its translation)
void SkinPalette(const SkinGpu& gpu, const SkinGeo::Inputs& in, std::vector<float>& out) {
    const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
    out.resize(gpu.joints.size() * 12);
    float* o = out.data();
    for (size_t j = 0; j < gpu.joints.size(); ++j) {
        const size_t at = size_t(gpu.joints[j]) * 4;
        const auto& c0 = in.joints[at];
        const auto& c1 = in.joints[at + 1];
        const auto& c2 = in.joints[at + 2];
        const auto& tr = in.joints[at + 3];
        for (int k = 0; k < 3; ++k) {
            *o++ = F(c0[size_t(k)]);
            *o++ = F(c1[size_t(k)]);
            *o++ = F(c2[size_t(k)]);
            *o++ = F(tr[size_t(k)]);
        }
    }
}

// The skin parts (and blend parts) this picture asked for, made on the drawing thread ahead of the next picture
// (FlushNativeMeshes)
void FlushSkinMeshes() {
    if (g_skinMeshRequests.empty() && g_blendMeshRequests.empty()) return;
    SkinMeshBatch batch;
    batch.job = std::make_shared<te::gfx::MeshBatchJob>();
    auto add = [&](const SkinGeo::Part& geometry, SkinGpu& gpu, bool blend, uint32_t skin, uint32_t index) {
        if (gpu.state != 1) return;
        gpu.state = 2;
        te::gfx::MeshRequest r;
        if (!SkinMeshOf(geometry, gpu, r, blend)) {
            gpu.unfit = true;
            static int said = 0;
            if (said++ < 16)
                std::fprintf(stderr, "[te.gfx] GPU skinning: %s %#x part %#x does not fit (CPU-built)\n", blend ? "blend skin" : "skin",
                             skin, index);
            return;
        }
        gpu.state = 3;
        batch.job->requests.push_back(std::move(r));
        batch.parts.push_back(&gpu);
    };
    for (const auto& [skin, index] : g_skinMeshRequests) {
        auto it = g_skins.find(skin);
        if (it == g_skins.end() || index >= it->second.parts.size()) continue;
        SkinPart& part = it->second.parts[index];
        if (part.decoded) add(part.part, part.gpu, false, skin, index);
    }
    g_skinMeshRequests.clear();
    for (const auto& [skin, packet] : g_blendMeshRequests) {
        auto it = g_blendSkins.find(skin);
        if (it == g_blendSkins.end()) continue;
        auto pt = it->second.find(packet);
        if (pt == it->second.end() || !pt->second.decoded) continue;
        add(pt->second.part, pt->second.gpu, true, skin, packet);
    }
    g_blendMeshRequests.clear();
    if (batch.parts.empty()) return;
    batch.ticket = te::gfx::CreateMeshesAsync(batch.job);
    g_skinMeshBatches.push_back(std::move(batch));
    if (!g_skinMeshBatches.back().ticket) ResolveSkinBatches();   // made here and now (no drawing thread)
}
#endif

uint32_t Bits(float value) { uint32_t b; std::memcpy(&b, &value, 4); return b; }
SkinGeo::Qw RowQw(const Matrix& m, int r) { return {Bits(m[r][0]), Bits(m[r][1]), Bits(m[r][2]), Bits(m[r][3])}; }

// The per-draw inputs (SetSkinDMA): false when the view, the matrix or the joints cannot be read
bool SkinInputs(uint32_t jointsAddress, int32_t jointCount, uint32_t matrixAddress, uint32_t lightsAddress, int32_t mode,
                SkinGeo::Inputs& in) {
    const RenderView* view = CurrentView();
    const auto world = ReadMatrix(matrixAddress);
    if (!view || !world || jointCount <= 0 || jointCount > int32_t(SkinGeo::kMaxJoints)) return false;
    const auto* joints = reinterpret_cast<const uint32_t*>(te::GuestPointer(jointsAddress, 64 * size_t(jointCount)));
    if (!joints) return false;
    in.joints.resize(4 * size_t(jointCount));
    for (size_t q = 0; q < in.joints.size(); ++q)
        for (int k = 0; k < 4; ++k) in.joints[q][k] = joints[q * 4 + size_t(k)];
    const Matrix toScreen = Multiply(*world, ToMatrix(view->toScreen));
    if (!Finite(toScreen)) return false;
    for (int r = 0; r < 4; ++r) in.to_screen[r] = RowQw(toScreen, r);
    in.clipped = uint32_t(mode) != DrawUnclipped;
    if (in.clipped) {
        const Matrix toCamera = Multiply(*world, ToMatrix(view->toClip));
        if (!Finite(toCamera)) return false;
        for (int r = 0; r < 4; ++r) in.to_camera[r] = RowQw(toCamera, r);
        in.clip = {Bits(view->clip[0]), Bits(view->clip[1]), Bits(view->clip[2]), Bits(view->clip[3])};
    } else {
        // (what a fresh Inputs holds: the Vita's draw_skin reuses one across the calls)
        in.to_camera = {};
        in.clip = {};
    }
    // MakeSkinSpace: the lights' directions turned into the skin's space (the rotation's transpose), a row each; the
    // fourth words the PS2 left as stack leftovers are zero here (the program never reads them)
    bool lit = false;
    if (const auto* ml = Guest<ModelLights>(lightsAddress)) {
        const float* dirs = reinterpret_cast<const float*>(te::GuestPointer(ml->directions, 48));
        const float* cols = reinterpret_cast<const float*>(te::GuestPointer(ml->colours, 48));
        const float* amb = reinterpret_cast<const float*>(te::GuestPointer(ml->ambient, 16));
        if (dirs && cols && amb) {
            lit = true;
            for (int i = 0; i < 3; ++i) {
                float d[3];
                for (int k = 0; k < 3; ++k) {
                    float sum = 0.f;
                    for (int r = 0; r < 3; ++r) sum += (*world)[k][r] * dirs[i * 4 + r];
                    d[k] = sum;
                }
                in.lights[i] = {Bits(d[0]), Bits(d[1]), Bits(d[2]), 0};
                in.colours[i] = {Bits(cols[i * 4]), Bits(cols[i * 4 + 1]), Bits(cols[i * 4 + 2]), Bits(cols[i * 4 + 3])};
            }
            in.ambient = {Bits(amb[0]), Bits(amb[1]), Bits(amb[2]), Bits(amb[3])};
        }
    }
    if (!lit) {
        for (int i = 0; i < 3; ++i) { in.lights[i] = {0, 0, 0, 0}; in.colours[i] = {0, 0, 0, 0}; }
        in.ambient = {Bits(1.f), Bits(1.f), Bits(1.f), Bits(1.f)};
    }
    in.scroll = {0.f, 0.f};
    return true;
}

// The material of a skin's submodel (MaterialResource -> material) as the draw's state and bucket
[[maybe_unused]] void SkinMaterial(uint32_t materialsAddress, uint32_t count, uint32_t index, ps2e_draw_state& state, uint32_t& bucket, MaterialView* out) {
    const uint32_t* materials = count ? reinterpret_cast<const uint32_t*>(te::GuestPointer(materialsAddress, 4 * size_t(count))) : nullptr;
    const uint32_t resource = materials ? materials[index] : 0;
    const uint32_t* materialAddress = resource ? Guest<uint32_t>(resource + 8) : nullptr;   // MaterialResource::material
    const MaterialView m = te::gfx::ReadMaterial(materialAddress ? *materialAddress : 0);
    state = te::gfx::StateOf(m);
    bucket = m.material ? m.bucket : BucketOpaque;
    if (out) *out = m;
}

// The built draws into the bucket (the Emitter's triangle-list form; a skin's strips are published one DrawIr per batch)
size_t EmitSkinDraws(Emitter& emitter, const std::vector<ps2r::gs::DrawIr>& draws) {
    const size_t before = emitter.triangles;
    for (const auto& d : draws) emitter.Append(d.verts.data(), d.verts.size());
    return emitter.triangles - before;
}

void SkinDiag(const char* kind, uint32_t skin, uint32_t index, uint32_t count, const SkinGeo::Inputs& in, uint32_t bucket,
              const MaterialView& m, const ps2e_draw_state& state, size_t triangles, const DiagBox& box, int result) {
    std::fprintf(stderr,
                 "[te.gfx.diag] present %ld %s %#x part %u/%u %s joints %zu bucket %u shader %#x tex %u tfx %d blend %d atest %d ztest %d zwrite %d result %d tris %zu box (%.0f %.0f)-(%.0f %.0f) rgba (%.0f %.0f %.0f %.0f) n %zu\n",
                 g_diag.present, kind, skin, index, count, in.clipped ? "clipped" : "UNCLIPPED", in.joints.size() / 4, bucket,
                 m.shader ? unsigned(m.shader->type) : 0u, state.texture, state.tfx, state.blend, state.alpha_test, state.depth_test, state.depth_write,
                 result, triangles, box.x0, box.y0, box.x1, box.y1, box.n ? box.r / box.n : 0.0, box.n ? box.g / box.n : 0.0,
                 box.n ? box.b / box.n : 0.0, box.n ? box.a / box.n : 0.0, box.n);
}

void LoadSkinPart(uint32_t skin, int32_t index, int32_t count, const uint8_t* bytes, int32_t size, int32_t vertices) {
    auto& scene = te::gfx::Scene();
    if (index < 0 || count <= 0 || index >= count || count > 4096) return;
    te::gfx::DrainBuilds();   // a posted build may still read the parts this replaces
    SkinModel& m = g_skins[skin];
#if defined(__vita__)
    ResolveSkinBatches();     // a batch being made may still name them
    if (m.parts.size() != size_t(count)) RetireSkinMeshes(m);
    else RetireSkinMesh(m.parts[size_t(index)]);
#endif
    if (m.parts.size() != size_t(count)) m.parts.assign(size_t(count), SkinPart{});
    SkinPart& part = m.parts[size_t(index)];
    part = SkinPart{};
    part.vertices = vertices;
    if (bytes && size > 0 && part.packet.data() != bytes) part.packet.assign(bytes, bytes + size);
    const SkinGeo::Result r = bytes ? SkinGeo::Decode(part.packet.data(), part.packet.size(), uint32_t(vertices), part.part)
                                    : SkinGeo::Result::InvalidAsset;
    if (r != SkinGeo::Result::Ok) {
        scene.skins_refused++;
        static int said = 0;
        if (said++ < 16)
            std::fprintf(stderr, "[te.gfx] skin %#x submodel %d/%d: packet not decoded (%d; %d bytes, %d vertices)\n", skin, index, count,
                         int(r), size, vertices);
        // diagnosis: TE_GFX_SKIN_DUMP=DIR writes every refused packet's bytes there (skin_<address>_<index>.bin)
        if (const char* dir = std::getenv("TE_GFX_SKIN_DUMP")) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/skin_%08x_%d.bin", dir, skin, index);
            if (FILE* f = std::fopen(path, "wb")) { std::fwrite(part.packet.data(), 1, part.packet.size(), f); std::fclose(f); }
        }
        return;
    }
    part.decoded = true;
    scene.skins_decoded++;
    if (te::gfx::LogEnabled() && scene.skins_decoded <= 64)
        std::fprintf(stderr, "[te.gfx] skin %#x submodel %d/%d: %zu batches, %u vertices (declared %d)\n", skin, index, count,
                     part.part.batches.size(), part.part.vertices, vertices);
}
}  // namespace

namespace te::gfx {
void SetNativeScrolled(bool on) { g_nativeScrolled.store(on, std::memory_order_relaxed); }
void SetGpuSkins(bool on) {
#if defined(__vita__)
    g_gpuSkins.store(on, std::memory_order_relaxed);
#else
    (void)on;
#endif
}
void SetGpuBlend(bool on) {
#if defined(__vita__)
    g_gpuBlend.store(on, std::memory_order_relaxed);
#else
    (void)on;
#endif
}
void SetChunkedNative(bool on) {
#if defined(__vita__)
    g_chunkedNative.store(on, std::memory_order_relaxed);
#else
    (void)on;
#endif
}
std::string ChunkCensusLine(double frames) {
#if defined(__vita__)
    if (frames <= 0) return std::string();
    std::string out = "[te.gfx.vita] clipped parts per frame through their native mesh's chunks:";
    static const char* const kNames[2] = {"models", "sky"};
    char b[320];
    for (int k = 0; k < 2; ++k) {
        ChunkCensus& c = g_chunkCensus[k];
        const double parts = double(c.parts.exchange(0)), whole = double(c.whole_cpu.exchange(0));
        const double nt = double(c.native_tris.exchange(0)), ct = double(c.cpu_tris.exchange(0)), st = double(c.skipped_tris.exchange(0));
        const double nd = double(c.native_draws.exchange(0)), cr = double(c.cpu_runs.exchange(0));
        std::snprintf(b, sizeof b, "%s %s %.1f parts (+%.1f CPU-built whole: no mesh yet), triangles native %.0f / CPU %.0f / dropped %.0f "
                      "(%.0f%% native), %.1f native draws + %.1f CPU runs", k ? ";" : "", kNames[k], parts / frames, whole / frames,
                      nt / frames, ct / frames, st / frames, nt + ct > 0 ? 100.0 * nt / (nt + ct) : 0.0, nd / frames, cr / frames);
        out += b;
    }
    return out + "\n";
#else
    (void)frames;
    return std::string();
#endif
}
// the builders' census line: the work of the builds per frame and the work they were spared
std::string BuildersCensusLine(double frames) {
#if defined(__vita__)
    if (frames <= 0) return std::string();
    BuildersCensus& c = g_builders;
    const auto take = [frames](std::atomic<uint64_t>& a) { return double(a.exchange(0, std::memory_order_relaxed)) / frames; };
    const auto takePlain = [frames](uint64_t& a) { const double v = double(a) / frames; a = 0; return v; };
    char b[2200];
    const double sb = take(c.shadow_builds), sp = take(c.shadow_parts), ss = take(c.shadow_strips), sv = take(c.shadow_vertices),
                 su = take(c.shadow_unique), sout = take(c.shadow_out), se = take(c.shadow_empty), spo = take(c.shadow_positions),
                 spr = take(c.shadow_projected), ssk = take(c.shadow_skipped), sce = take(c.shadow_cert), scl = take(c.shadow_clamped),
                 scd = take(c.shadow_cert_differ), scm = take(c.shadow_compared), sdf = take(c.shadow_differ);
    uint64_t mesh[5];
    title::MeshGeometry::CertifiedCensus(mesh);
    const double rp0 = takePlain(c.posted[0]), rr0 = takePlain(c.repeat[0]), rp1 = takePlain(c.posted[1]), rr1 = takePlain(c.repeat[1]),
                 rp2 = takePlain(c.posted[2]), rr2 = takePlain(c.repeat[2]);
    double why[2][5], cv[2], fv[2], ov[2];
    for (int k = 0; k < 2; ++k) {
        why[k][0] = take(c.cpu_behind[k]);
        why[k][1] = take(c.cpu_guard[k]);
        why[k][2] = take(c.cpu_depth[k]);
        why[k][3] = take(c.cpu_run[k]);
        why[k][4] = take(c.cpu_sky[k]);
        cv[k] = take(c.cpu_vertices[k]);
        fv[k] = take(c.fan_vertices[k]);
        ov[k] = take(c.out_vertices[k]);
    }
    const double bp = take(c.blend_parts), bv = take(c.blend_vertices), bt = take(c.blend_terms), bz = take(c.blend_zero),
                 kp = take(c.skin_parts), kv = take(c.skin_vertices);
    std::snprintf(b, sizeof b,
                  "[te.gfx.vita] builders per present: SHADOW builds %.1f (inputs a build of the picture before had: %.1f of %.1f "
                  "posted), parts %.1f, strips %.1f (skipped: every vertex outside one clip plane %.1f), vertices %.1f (distinct "
                  "positions %.1f: transformed %.1f, projected %.1f; the arithmetic certified in %.1f part builds, clamped in %.1f, "
                  "compared and differing %.1f), vertices out %.1f, builds with nothing out %.1f (shared vs BuildRuns: %.1f parts "
                  "compared, %.1f differed); MESH builds (BuildPrepared, two passes): clamp-free %.1f, clamped %.1f (vertices "
                  "compared and differing %.1f; builds compared with the one-pass form %.1f, differing %.1f); CHUNKED models: "
                  "inputs repeated %.1f of "
                  "%.1f, CPU triangles by why: behind the eye plane %.1f, outside the guard band %.1f, depth range %.1f, short "
                  "native run %.1f; CPU vertices %.1f, out %.1f (fans %.1f); sky: inputs repeated %.1f of %.1f, CPU triangles: "
                  "behind %.1f, guard band %.1f, depth %.1f, the sky's rule %.1f; CPU vertices %.1f, out %.1f (fans %.1f); SKINS "
                  "on the CPU: blend parts %.1f (%.1f vertices, %.1f vertex x shape terms, %.1f shapes of weight 0), plain parts "
                  "%.1f (%.1f vertices)\n",
                  sb, rr0, rp0, sp, ss, ssk, sv, su, spo, spr, sce, scl, scd, sout, se, scm, sdf, double(mesh[0]) / frames,
                  double(mesh[1]) / frames, double(mesh[2]) / frames, double(mesh[3]) / frames, double(mesh[4]) / frames, rr1, rp1, why[0][0], why[0][1], why[0][2], why[0][3], cv[0], ov[0],
                  fv[0], rr2, rp2, why[1][0], why[1][1], why[1][2], why[1][4], cv[1], ov[1], fv[1], bp, bv, bt, bz, kp, kv);
    return std::string(b) + PoolCensusLine(frames);
#else
    (void)frames;
    return std::string();
#endif
}
std::string SkinPathCensusLine(double frames) {
#if defined(__vita__)
    if (frames <= 0) return "\n";
    std::string out = "skin parts by path:";
    char b[96];
    for (int k = 0; k < kSkinPaths; ++k) {
        if (!g_skinParts[k]) continue;
        std::snprintf(b, sizeof b, " %s %.1f", kSkinPathNames[k], double(g_skinParts[k]) / frames);
        out += b;
        g_skinParts[k] = 0;
    }
    std::snprintf(b, sizeof b, "; GPU-skinned triangles %.0f\n", double(g_skinGpuTriangles) / frames);
    g_skinGpuTriangles = 0;
    return out + b;
#else
    (void)frames;
    return "\n";
#endif
}
std::vector<ModelSource> SkinSources() {
    std::vector<ModelSource> out;
    for (const auto& [address, m] : g_skins)
        for (size_t i = 0; i < m.parts.size(); ++i)
            if (!m.parts[i].packet.empty())
                out.push_back(ModelSource{address, int32_t(i), int32_t(m.parts.size()), m.parts[i].vertices, m.parts[i].packet});
    return out;
}
void ReplaySkinSources(const std::vector<ModelSource>& sources) {
    DrainBuilds();
#if defined(__vita__)
    ResolveSkinBatches();
    for (auto& [address, m] : g_skins) RetireSkinMeshes(m);
    for (auto& [address, parts] : g_blendSkins)
        for (auto& [packet, geo] : parts) RetireSkinGpu(geo.gpu);
#endif
    g_skins.clear();
    g_blendSkins.clear();
    auto& scene = Scene();
    const uint64_t decoded = scene.skins_decoded, refused = scene.skins_refused;
    for (const ModelSource& s : sources)
        LoadSkinPart(s.model, s.index, s.count, s.packet.data(), int32_t(s.packet.size()), s.vertices);
    scene.skins_decoded = decoded;
    scene.skins_refused = refused;
}
}  // namespace te::gfx

extern "C" {

void ps2e_gfx2_skin_loaded(ps2e_u32 skin, ps2e_s32 index, ps2e_s32 count, ps2e_u32 packet, ps2e_s32 size, ps2e_s32 vertices) {
    const uint8_t* bytes = size > 0 ? te::GuestPointer(packet, size_t(size)) : nullptr;
    LoadSkinPart(skin, index, count, bytes, size, vertices);
}
void ps2e_gfx2_skin_released(ps2e_u32 skin) {
    te::gfx::DrainBuilds();   // a posted build may still read its parts
#if defined(__vita__)
    ResolveSkinBatches();     // a batch being made may still name its parts
    if (auto it = g_skins.find(skin); it != g_skins.end()) RetireSkinMeshes(it->second);
#endif
    g_skins.erase(skin);
}
void ps2e_gfx2_blend_skin_released(ps2e_u32 skin) {
    te::gfx::DrainBuilds();   // a posted blend may still read its parts
#if defined(__vita__)
    ResolveSkinBatches();     // a batch being made may still name its parts
    if (auto it = g_blendSkins.find(skin); it != g_blendSkins.end())
        for (auto& [packet, geo] : it->second) RetireSkinGpu(geo.gpu);
#endif
    g_blendSkins.erase(skin);
}

// DrawSkin (skins.cpp SetSkinDMA): every submodel under its material through the skinned lit strip program
void ps2e_graphics_draw_skin(ps2e_u32 skinAddress, ps2e_u32 joints, ps2e_s32 jointCount, ps2e_u32 matrix, ps2e_u32 lights, ps2e_s32 mode) {
    te::gfx::CountCall(te::gfx::kCallSkin);
#if defined(__vita__)
    if (Opts().noDraw) return;   // (TE_GFX_NO_DRAW, read once: a function-local static's guard is a DMB per call)
#else
    static const bool noDraw = std::getenv("TE_GFX_NO_DRAW") != nullptr;
    if (noDraw) return;
#endif
    Timed timed(te::gfx::Scene().skin_ms);
    const auto* skin = Guest<te_gfx_skin>(skinAddress);
    if (!skin || !te::gfx::EnsureDevice()) return;
    auto it = g_skins.find(skinAddress);
    if (it == g_skins.end()) { te::gfx::Scene().submodels_missing++; return; }
#if defined(__vita__)
    // the inputs kept across the calls (the game thread's; SkinInputs sets every field a fresh one would hold): a fresh
    // Inputs was the joints' vector allocated and freed per call -- two newlib heap calls (kernel locks) on the Vita
    static SkinGeo::Inputs in;
#else
    SkinGeo::Inputs in;
#endif
    if (!SkinInputs(joints, jointCount, matrix, lights, mode, in)) return;
    auto& scene = te::gfx::Scene();
    scene.skin_triangles += g_deferredSkinTriangles.exchange(0, std::memory_order_relaxed);
    static Emitter emitter;
    static std::vector<ps2r::gs::DrawIr> draws;
    const uint32_t count = std::min<uint32_t>(skin->count, uint32_t(it->second.parts.size()));
#if defined(__vita__)
    // the k-th draw of this skin this frame: the interpolator's identity, read by nothing else -- the ordinals' map (a
    // node allocated per skin per frame, all freed at the present) only while the presenter interpolates
    const uint64_t skinId = te::gfx::Interpolating() ? OrdinalId(TagId(skinAddress, 0)) : TagId(skinAddress, 0);
    const Mem mem;
#else
    const uint64_t skinId = OrdinalId(TagId(skinAddress, 0));   // the k-th draw of this skin this frame
#endif
    // the submodels' builds share one copy of the inputs (the joints: kilobytes) per scroll, not a copy each
    std::shared_ptr<const SkinGeo::Inputs> shared;
    for (uint32_t i = 0; i < count; ++i) {
        SkinPart& part = it->second.parts[i];
        if (!part.decoded) continue;
        ps2e_draw_state state;
        uint32_t bucket;
        MaterialView m;
#if defined(__vita__)
        {
            // the material and its first shader's state through the rigid models' cache (CachedMaterial: what ReadMaterial
            // and StateOf answer, kept while every byte they read is unchanged); SkinMaterial's reads otherwise
            const uint32_t* materials = skin->count ? reinterpret_cast<const uint32_t*>(mem.Bytes(skin->materials, 4 * size_t(skin->count))) : nullptr;
            const uint32_t resource = materials ? materials[i] : 0;
            const uint32_t* materialAddress = resource ? mem.Get<uint32_t>(resource + 8) : nullptr;   // MaterialResource::material
            ps2e_draw_state scratch;
            state = *CachedMaterial(mem, materialAddress ? *materialAddress : 0, m, scratch);
            bucket = m.material ? m.bucket : BucketOpaque;
        }
#else
        SkinMaterial(skin->materials, skin->count, i, state, bucket, &m);
#endif
        if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) in.scroll = {m.shader->scroll[0], m.shader->scroll[1]};
        else in.scroll = {0.f, 0.f};
        // the interpolator's identity: this skin at this instance matrix, submodel i -- a vertex run paired when its count holds
        te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{skinId, i, ps2r::ps2e::InterpKind::Vertices});
#if defined(__vita__)
        {
            // the GPU path: the part's skin mesh through this draw's palette (asked for at its first draw; until it is made,
            // and for whatever the GPU path cannot take, the CPU build below)
            SkinPath path = kSkinGpu;
            SkinGpu& gpu = part.gpu;
            if (!g_gpuSkins.load(std::memory_order_relaxed) || !te::gfx::NativeMeshes()) path = kSkinOff;
            else if (g_diag.flat || g_diag.Logging()) path = kSkinDiag;
            else {
                if (gpu.state == 3) ResolveSkinBatches();
                if (gpu.state == 0) {
                    gpu.state = 1;
                    g_skinMeshRequests.emplace_back(skinAddress, i);
                }
                if (gpu.unfit) path = kSkinUnfit;
                else if (!gpu.handle) path = kSkinWaiting;
                else if (gpu.joint_limit > in.joints.size() / 4) path = kSkinJoints;
                else if (in.clipped && !SkinInFront(gpu, in)) path = kSkinClipped;
            }
            g_skinParts[path]++;
            if (path == kSkinGpu) {
                thread_local std::vector<float> palette;
                SkinPalette(gpu, in, palette);
                const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
                ps2e_draw_mesh_params mp{};
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) mp.screen[r * 4 + c] = F(in.to_screen[size_t(r)][size_t(c)]);
                mp.offset[0] = te::gfx::GsOriginX();
                mp.offset[1] = te::gfx::GsOriginY();
                mp.scroll[0] = in.scroll[0];
                mp.scroll[1] = in.scroll[1];
                mp.lit = 1;
                for (int l = 0; l < 3; ++l)
                    for (int k = 0; k < 3; ++k) {
                        mp.light_dir[l][k] = F(in.lights[size_t(l)][size_t(k)]);
                        mp.light_colour[l][k] = F(in.colours[size_t(l)][size_t(k)]);
                    }
                for (int k = 0; k < 3; ++k) mp.ambient[k] = F(in.ambient[size_t(k)]);
                mp.box[0] = mp.box[1] = -1e9f;   // the whole frame (as a native mesh's: its one reader takes no ps2e_draw state)
                mp.box[2] = mp.box[3] = 1e9f;
                te::gfx::RecordSkinMesh(bucket, state, gpu.handle, mp, palette.data(), uint32_t(gpu.joints.size()));
                scene.skin_triangles += gpu.triangles;
                g_skinGpuTriangles += gpu.triangles;
                continue;
            }
        }
#endif
        if (Deferrable(g_diag.Logging())) {
            // the build on a worker: the placeholder takes this submodel's place in the bucket
            te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, state);
            if (!shared || shared->scroll != in.scroll) shared = std::make_shared<const SkinGeo::Inputs>(in);
            te::gfx::PostBuild(build, [&build, geometry = &part.part, inputs = shared, buildState = BuildState(), textured = state.texture != 0, skinAddress, i] {
                static const unsigned short profClipped = te::prof::Register("skin build (clipped)"), profPlain = te::prof::Register("skin build");
                te::prof::Scope profScope(inputs->clipped ? profClipped : profPlain);
#if defined(__vita__)
                {
                    uint64_t v = 0;
                    for (const auto& batch : geometry->batches) v += batch.Vertices();
                    g_builders.skin_parts.fetch_add(1, std::memory_order_relaxed);
                    g_builders.skin_vertices.fetch_add(v, std::memory_order_relaxed);
                }
#endif
                {
                    // the Vita's direct skinning: the batches' runs into one per-thread array, no DrawIr per batch
                    thread_local std::vector<ps2r::gs::VertexRec> runVerts;
                    thread_local std::vector<uint32_t> runEnds;
                    if (SkinGeo::BuildRuns(*geometry, *inputs, buildState, runVerts, runEnds) == SkinGeo::Result::Ok) {
                        Collector collector{&build, textured};
                        size_t at = 0;
                        for (const uint32_t end : runEnds) {
                            collector.Append(runVerts.data() + at, end - at);
                            at = end;
                        }
                        g_deferredSkinTriangles += collector.triangles;
                        return;
                    }
                }
                std::vector<ps2r::gs::DrawIr> built;
                const SkinGeo::Result r = SkinGeo::Build(*geometry, *inputs, buildState, built);
                if (r != SkinGeo::Result::Ok) {
                    static std::atomic<int> said{0};
                    if (said++ < 8) std::fprintf(stderr, "[te.gfx] skin %#x part %u: Build refused (%d)\n", skinAddress, i, int(r));
                    return;
                }
                Collector collector{&build, textured};
                for (const auto& d : built) collector.Append(d.verts.data(), d.verts.size());
                g_deferredSkinTriangles += collector.triangles;
            });
            continue;
        }
        draws.clear();
        const SkinGeo::Result r = SkinGeo::Build(part.part, in, BuildState(), draws);
        if (r != SkinGeo::Result::Ok) {
            static int said = 0;
            if (said++ < 8) std::fprintf(stderr, "[te.gfx] skin %#x part %u: Build refused (%d)\n", skinAddress, i, int(r));
            continue;
        }
        emitter.Begin(bucket, state);
        emitter.flatTint = in.clipped ? 1 : 0;
        DiagBox box;
        const bool logging = g_diag.Logging();
        emitter.box = logging ? &box : nullptr;
        const size_t triangles = EmitSkinDraws(emitter, draws);
        emitter.box = nullptr;
        if (logging) SkinDiag("skin", skinAddress, i, count, in, bucket, m, state, triangles, box, int(r));
        scene.skin_triangles += triangles;
    }
    te::gfx::ClearDrawTag();
    scene.skins++;
}

// DrawBlendSkin (skins.cpp SetBlendSkinDMA / WriteBlendSubModel / WriteBlendPart): every submodel's parts, each part's
// packet through the blend programs with the shapes the game lists (weights[i] on shapes[i]) over the first part's factors
void ps2e_graphics_draw_blend_skin(ps2e_u32 skinAddress, ps2e_u32 joints, ps2e_s32 jointCount, ps2e_u32 matrix, ps2e_u32 lights,
                                   ps2e_u32 weightsAddress, ps2e_u32 shapesAddress, ps2e_s32 shapeCount, ps2e_s32 mode) {
    te::gfx::CountCall(te::gfx::kCallBlendSkin);
#if defined(__vita__)
    if (Opts().noDraw) return;   // (read once: a function-local static's guard is a DMB per call)
#else
    static const bool noDraw = std::getenv("TE_GFX_NO_DRAW") != nullptr;
    if (noDraw) return;
#endif
    Timed timed(te::gfx::Scene().skin_ms);
    const auto* skin = Guest<te_gfx_blend_skin>(skinAddress);
    if (!skin || !te::gfx::EnsureDevice()) return;
#if defined(__vita__)
    static SkinGeo::Inputs in;   // (as draw_skin's: SkinInputs sets every field; a fresh one was the joints' vector allocated per call)
#else
    SkinGeo::Inputs in;
#endif
    if (!SkinInputs(joints, jointCount, matrix, lights, mode, in)) return;
    if (shapeCount < 0 || shapeCount > 64) return;
    const float* weights = shapeCount ? reinterpret_cast<const float*>(te::GuestPointer(weightsAddress, 4 * size_t(shapeCount))) : nullptr;
    const int32_t* shapes = shapeCount ? reinterpret_cast<const int32_t*>(te::GuestPointer(shapesAddress, 4 * size_t(shapeCount))) : nullptr;
    if (shapeCount && (!weights || !shapes)) return;
    const uint32_t* subModels = skin->count ? reinterpret_cast<const uint32_t*>(te::GuestPointer(skin->subModels, 4 * size_t(skin->count))) : nullptr;
    if (!subModels || skin->count > 4096) return;
    auto& scene = te::gfx::Scene();
    auto& cache = g_blendSkins[skinAddress];
    static Emitter emitter;
    static std::vector<ps2r::gs::DrawIr> draws;
#if defined(__vita__)
    // the k-th draw of this blend skin this frame: the interpolator's identity -- the ordinals' map only while it interpolates
    const uint64_t blendId = te::gfx::Interpolating() ? OrdinalId(TagId(skinAddress, 1)) : TagId(skinAddress, 1);
    const Mem mem;
    // A SUBMODEL'S PARTS IN ONE BUILD: the device's hub draws one blend skin of 46 parts a frame, every part a
    // placeholder record, a deferred build, a posted job and a copy of its shapes' list -- 433 us of the game thread for the
    // one call (measured: 1.3 % of the hub's samples). The consecutive parts that build on the CPU are gathered here and built by
    // one job, in order, into one build: its segments are the parts' segments in the same order, under the same state and
    // tag, and the bucket's expansion replays them through the same joining rule -- the same draws, the same vertices (a
    // part on another path, the GPU or an immediate build, first closes the batch: the draws keep their order)
    struct BatchPart { const SkinGeo::Part* geometry; uint32_t first, count, part; };
    static std::vector<BatchPart> batchParts;
    static std::vector<SkinGeo::Shape> batchShapes;
#else
    const uint64_t blendId = OrdinalId(TagId(skinAddress, 1));
#endif
    std::shared_ptr<const SkinGeo::Inputs> shared;   // one copy of the inputs for the parts' builds (per scroll)
    for (uint32_t i = 0; i < skin->count; ++i) {
        const auto* subModel = Guest<te_gfx_blend_sub_model>(subModels[i]);
        if (!subModel || !subModel->count || subModel->count > 4096) continue;
        const uint32_t* parts = reinterpret_cast<const uint32_t*>(te::GuestPointer(subModel->parts, 4 * size_t(subModel->count)));
        if (!parts) continue;
        ps2e_draw_state state;
        uint32_t bucket;
        MaterialView m;
#if defined(__vita__)
        {
            // the material and its first shader's state through the rigid models' cache (as draw_skin's: what ReadMaterial and
            // StateOf answer, kept while every byte they read is unchanged)
            const uint32_t* materials = skin->count ? reinterpret_cast<const uint32_t*>(mem.Bytes(skin->materials, 4 * size_t(skin->count))) : nullptr;
            const uint32_t resource = materials ? materials[i] : 0;
            const uint32_t* materialAddress = resource ? mem.Get<uint32_t>(resource + 8) : nullptr;   // MaterialResource::material
            ps2e_draw_state scratch;
            state = *CachedMaterial(mem, materialAddress ? *materialAddress : 0, m, scratch);
            bucket = m.material ? m.bucket : BucketOpaque;
        }
#else
        SkinMaterial(skin->materials, skin->count, i, state, bucket, &m);
#endif
        if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) in.scroll = {m.shader->scroll[0], m.shader->scroll[1]};
        else in.scroll = {0.f, 0.f};
        const auto* first = Guest<te_gfx_blend_part>(parts[0]);
        if (!first) continue;
#if defined(__vita__)
        static SkinGeo::BlendInputs blend;   // (its shapes' storage kept across the calls)
        blend.shapes.clear();
#else
        SkinGeo::BlendInputs blend;
#endif
        blend.factors = {first->shapeFactors[0], first->shapeFactors[1], first->shapeFactors[2]};
        te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{blendId, i, ps2r::ps2e::InterpKind::Vertices});
        emitter.Begin(bucket, state);
        emitter.flatTint = in.clipped ? 1 : 0;
        DiagBox box;
        const bool logging = g_diag.Logging();
        emitter.box = logging ? &box : nullptr;
        size_t triangles = 0;
        int lastResult = 0;
#if defined(__vita__)
        // the gathered parts' one build (BatchPart above): its placeholder where the per-part ones would have stood
        batchParts.clear();
        batchShapes.clear();
        const auto flushBatch = [&] {
            if (batchParts.empty()) return;
            te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, emitter.state);
            if (!shared || shared->scroll != in.scroll) shared = std::make_shared<const SkinGeo::Inputs>(in);
            te::gfx::PostBuild(build, [&build, parts = batchParts, shapes = batchShapes, factors = blend.factors, inputs = shared,
                                       buildState = BuildState(), textured = emitter.textured, skinAddress, i] {
                TE_PROF_SCOPE("blend skin build");
                thread_local std::vector<ps2r::gs::VertexRec> runVerts;
                thread_local std::vector<uint32_t> runEnds;
                thread_local std::vector<ps2r::gs::DrawIr> built;
                thread_local SkinGeo::BlendInputs b;
                Collector collector{&build, textured};
                uint64_t cParts = 0, cVertices = 0, cTerms = 0, cZero = 0;   // the builders' census
                for (const BatchPart& bp : parts) {
                    // the part's own job of before, in order: its blend inputs, its runs (else BuildBlend's draws)
                    b.factors = factors;
                    b.shapes.assign(shapes.begin() + std::ptrdiff_t(bp.first), shapes.begin() + std::ptrdiff_t(bp.first + bp.count));
                    {
                        uint64_t v = 0;
                        for (const auto& batch : bp.geometry->batches) v += batch.Vertices();
                        cParts++;
                        cVertices += v;
                        cTerms += v * bp.count;
                        for (const auto& sh : b.shapes) cZero += sh.weight == 0.f ? 1 : 0;
                    }
                    if (SkinGeo::BuildBlendRuns(*bp.geometry, *inputs, b, buildState, runVerts, runEnds) == SkinGeo::Result::Ok) {
                        size_t at = 0;
                        for (const uint32_t end : runEnds) {
                            collector.Append(runVerts.data() + at, end - at);
                            at = end;
                        }
                        continue;
                    }
                    built.clear();
                    const SkinGeo::Result r = SkinGeo::BuildBlend(*bp.geometry, *inputs, b, buildState, built);
                    if (r != SkinGeo::Result::Ok) {
                        static std::atomic<int> said{0};
                        if (said++ < 8)
                            std::fprintf(stderr, "[te.gfx] blend skin %#x submodel %u part %u: BuildBlend refused (%d)\n", skinAddress, i, bp.part, int(r));
                        continue;
                    }
                    for (const auto& d : built) collector.Append(d.verts.data(), d.verts.size());
                }
                g_deferredSkinTriangles += collector.triangles;
                g_builders.blend_parts.fetch_add(cParts, std::memory_order_relaxed);
                g_builders.blend_vertices.fetch_add(cVertices, std::memory_order_relaxed);
                g_builders.blend_terms.fetch_add(cTerms, std::memory_order_relaxed);
                g_builders.blend_zero.fetch_add(cZero, std::memory_order_relaxed);
            });
            batchParts.clear();
            batchShapes.clear();
        };
#endif
        for (uint32_t p = 0; p < subModel->count; ++p) {
            const auto* part = Guest<te_gfx_blend_part>(parts[p]);
            if (!part || !part->packet || !part->packetSize) continue;
            BlendPartGeo& geo = cache[part->packet];
            if (!geo.tried) {
                geo.tried = true;
                const uint8_t* bytes = te::GuestPointer(part->packet, part->packetSize);
                const SkinGeo::Result r = bytes ? SkinGeo::DecodePart(bytes, part->packetSize, 0, geo.part) : SkinGeo::Result::InvalidAsset;
                if (r == SkinGeo::Result::Ok) {
                    geo.decoded = true;
                    scene.skins_decoded++;
                } else {
                    scene.skins_refused++;
                    static int said = 0;
                    if (said++ < 16)
                        std::fprintf(stderr, "[te.gfx] blend skin %#x submodel %u part %u: packet not decoded (%d; %u bytes)\n", skinAddress, i, p,
                                     int(r), part->packetSize);
                    if (const char* dir = std::getenv("TE_GFX_SKIN_DUMP")) {
                        char path[512];
                        std::snprintf(path, sizeof path, "%s/blend_%08x_%u_%u.bin", dir, skinAddress, i, p);
                        if (FILE* f = std::fopen(path, "wb")) { if (bytes) std::fwrite(bytes, 1, part->packetSize, f); std::fclose(f); }
                    }
                }
            }
            if (!geo.decoded) continue;
            // The shapes' displacement rows (V4-8 per vertex: shapeCounts[shape] rows of 4 bytes within shapeSizes[shape] quadwords)
            blend.shapes.clear();
            const uint32_t* addresses = part->shapeCount ? reinterpret_cast<const uint32_t*>(te::GuestPointer(part->shapeAddresses, 4 * size_t(part->shapeCount))) : nullptr;
            const uint32_t* counts = part->shapeCount ? reinterpret_cast<const uint32_t*>(te::GuestPointer(part->shapeCounts, 4 * size_t(part->shapeCount))) : nullptr;
            bool ok = true;
            for (int32_t k = 0; k < shapeCount && ok; ++k) {
                const int32_t shape = shapes[k];
                if (shape < 0 || shape >= int32_t(part->shapeCount) || !addresses || !counts) { ok = false; break; }
                SkinGeo::Shape sh;
                sh.weight = weights[k];
                sh.count = counts[shape];
                sh.rows = sh.count ? te::GuestPointer(addresses[shape], 4 * size_t(sh.count)) : nullptr;
                if (!sh.rows) { ok = false; break; }
                blend.shapes.push_back(sh);
            }
            if (!ok) continue;
#if defined(__vita__)
            {
                // the GPU path (as a plain skin's part, ps2e_graphics_draw_skin): the part's skin mesh through the palette,
                // each vertex's displacement -- its shapes' int8 rows x the factors x the weights, in the game's order --
                // summed here and added to its bind position by the GPU
                SkinPath path = kSkinGpuBlend;
                SkinGpu& gpu = geo.gpu;
                if (!g_gpuSkins.load(std::memory_order_relaxed) || !te::gfx::NativeMeshes()) path = kSkinOff;
                else if (!g_gpuBlend.load(std::memory_order_relaxed)) path = kSkinBlendCpu;
                else if (g_diag.flat || logging) path = kSkinDiag;
                else {
                    if (gpu.state == 3) ResolveSkinBatches();
                    if (gpu.state == 0) {
                        gpu.state = 1;
                        g_blendMeshRequests.emplace_back(skinAddress, part->packet);
                    }
                    if (gpu.unfit) path = kSkinUnfit;
                    else if (!gpu.handle) path = kSkinWaiting;
                    else if (gpu.joint_limit > in.joints.size() / 4) path = kSkinJoints;
                }
                thread_local std::vector<float> disp;
                float slack = 0.f;
                if (path == kSkinGpuBlend) {
                    const uint32_t count = geo.part.batches[0].Vertices();
                    disp.assign(size_t(count) * 4, 0.f);
                    for (const auto& sh : blend.shapes)
                        if (!sh.rows || sh.count < count) path = kSkinUnfit;   // FastSkinRuns refuses it too: CPU (as before)
                    if (path == kSkinGpuBlend) {
                        for (uint32_t j = 0; j < count; ++j) {
                            float d[3] = {0.f, 0.f, 0.f};
                            for (const auto& sh : blend.shapes) {
                                const uint8_t* row = sh.rows + size_t(j) * 4;   // V4-8: sign-extended
                                for (int k = 0; k < 3; ++k) d[k] += float(int32_t(int8_t(row[k]))) * blend.factors[size_t(k)] * sh.weight;
                            }
                            for (int k = 0; k < 3; ++k) disp[size_t(j) * 4 + size_t(k)] = d[k];
                            slack = std::max(slack, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
                        }
                        if (!std::isfinite(slack)) path = kSkinUnfit;
                        else if (in.clipped && !SkinInFront(gpu, in, slack)) path = kSkinClipped;
                    }
                }
                g_skinParts[path]++;
                if (path == kSkinGpuBlend) {
                    thread_local std::vector<float> palette;
                    SkinPalette(gpu, in, palette);
                    const auto F = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
                    ps2e_draw_mesh_params mp{};
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c) mp.screen[r * 4 + c] = F(in.to_screen[size_t(r)][size_t(c)]);
                    mp.offset[0] = te::gfx::GsOriginX();
                    mp.offset[1] = te::gfx::GsOriginY();
                    mp.scroll[0] = in.scroll[0];
                    mp.scroll[1] = in.scroll[1];
                    mp.lit = 1;
                    for (int l = 0; l < 3; ++l)
                        for (int k = 0; k < 3; ++k) {
                            mp.light_dir[l][k] = F(in.lights[size_t(l)][size_t(k)]);
                            mp.light_colour[l][k] = F(in.colours[size_t(l)][size_t(k)]);
                        }
                    for (int k = 0; k < 3; ++k) mp.ambient[k] = F(in.ambient[size_t(k)]);
                    mp.box[0] = mp.box[1] = -1e9f;
                    mp.box[2] = mp.box[3] = 1e9f;
                    flushBatch();   // (the CPU parts before it first: the draws keep their order)
                    te::gfx::RecordSkinMesh(bucket, state, gpu.handle, mp, palette.data(), uint32_t(gpu.joints.size()), disp.data(),
                                            uint32_t(disp.size() / 4));
                    triangles += gpu.triangles;
                    g_skinGpuTriangles += gpu.triangles;
                    continue;
                }
            }
#endif
            if (Deferrable(logging)) {
#if defined(__vita__)
                if (Opts().lateSeams) {
                    // gathered: the submodel's consecutive CPU parts are built by one job (flushBatch)
                    batchParts.push_back(BatchPart{&geo.part, uint32_t(batchShapes.size()), uint32_t(blend.shapes.size()), p});
                    batchShapes.insert(batchShapes.end(), blend.shapes.begin(), blend.shapes.end());
                    continue;
                }
#endif
                // the blend on a worker, as the plain skins' builds (the shapes' rows are the model's, unchanged while it is
                // loaded; the part's decode lives until the skin is released, which drains the builds first)
                te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, emitter.state);
                if (!shared || shared->scroll != in.scroll) shared = std::make_shared<const SkinGeo::Inputs>(in);
                te::gfx::PostBuild(build, [&build, geometry = &geo.part, inputs = shared, blend, buildState = BuildState(), textured = emitter.textured, skinAddress, i, p] {
                    TE_PROF_SCOPE("blend skin build");
                    {
                        // the Vita's direct path (SkinGeometry::SetFastPaths): the batches' runs into one per-thread array
                        thread_local std::vector<ps2r::gs::VertexRec> runVerts;
                        thread_local std::vector<uint32_t> runEnds;
                        if (SkinGeo::BuildBlendRuns(*geometry, *inputs, blend, buildState, runVerts, runEnds) == SkinGeo::Result::Ok) {
                            Collector collector{&build, textured};
                            size_t at = 0;
                            for (const uint32_t end : runEnds) {
                                collector.Append(runVerts.data() + at, end - at);
                                at = end;
                            }
                            g_deferredSkinTriangles += collector.triangles;
                            return;
                        }
                    }
                    thread_local std::vector<ps2r::gs::DrawIr> built;
                    built.clear();
                    const SkinGeo::Result r = SkinGeo::BuildBlend(*geometry, *inputs, blend, buildState, built);
                    if (r != SkinGeo::Result::Ok) {
                        static std::atomic<int> said{0};
                        if (said++ < 8) std::fprintf(stderr, "[te.gfx] blend skin %#x submodel %u part %u: BuildBlend refused (%d)\n", skinAddress, i, p, int(r));
                        return;
                    }
                    Collector collector{&build, textured};
                    for (const auto& d : built) collector.Append(d.verts.data(), d.verts.size());
                    g_deferredSkinTriangles += collector.triangles;
                });
                continue;
            }
#if defined(__vita__)
            flushBatch();   // (an immediate build after gathered parts: theirs first)
#endif
            draws.clear();
            const SkinGeo::Result r = SkinGeo::BuildBlend(geo.part, in, blend, BuildState(), draws);
            lastResult = int(r);
            if (r != SkinGeo::Result::Ok) {
                static int said = 0;
                if (said++ < 8) std::fprintf(stderr, "[te.gfx] blend skin %#x submodel %u part %u: BuildBlend refused (%d)\n", skinAddress, i, p, int(r));
                continue;
            }
            triangles += EmitSkinDraws(emitter, draws);
        }
#if defined(__vita__)
        flushBatch();
#endif
        emitter.box = nullptr;
        if (logging) SkinDiag("blendskin", skinAddress, i, skin->count, in, bucket, m, state, triangles, box, lastResult);
        scene.skin_triangles += triangles;
    }
    te::gfx::ClearDrawTag();
    scene.blend_skins++;
}

}  // extern "C"
