// platform/nativeee (host side) -- the VU0 maths of Platform::Math (include/platform/math.h; PS2 bodies src/platform/ps2/math.cpp:
// VU0 microprograms of the standard set) and the collision half of the implicit second seam (include/game/{collision,physics}.h;
// PS2 bodies src/platform/ps2/collisionmaths.cpp), over the engine's value APIs: title::JointMath (the animations' slerp, Euler
// blend, turn, lerp, joint matrix and the parent product -- the microprograms 0x1C0, 0x818, 0xA70, 0x548/0x668/0x790 as the
// engine transcribed them), title::CollisionMath (the edge tests, vertex differences, point groups and hull supports) and
// title::MathLibrary (the triangle plane). The EE FPU's MAX.S/MIN.S compare signed magnitudes.
//
// PROVISIONAL (by meaning, plain IEEE, to be pinned against the oracle; microprograms the engine has
// not transcribed): the particles' view maths of the second set -- ViewDistance (0xCB0), ParticleBlockView (0xA08 / 0xB90)
// over the views LoadParticleView keeps. The fast ray casts (0xAB8: SetRay / StartRayTriangle / FinishRayTriangle) are the
// engine's title::RayMath (the microprogram as values, bit-identical to the retail micro on the engine's interpreter: a hit
// within 0.1 world units of the triangle's edges counts, the share |ds| / (|ds| + |de|)); against an engine pinned before it
// the body below follows the game's own slow path (collision.cpp CheckTriangleIntersection) by meaning, without the margin.
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/math/collision_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/joint_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"
// The engine's value transcription of the fast ray cast (title::RayMath, pinned to the retail 0xAB8 micro by the engine's
// Vu0RayTriangle test); the slow-path body below it stays for A/B (TE_MATH_RAY=legacy).
#include "games/slus_20909_crash_twinsanity/hle/math/ray_math.h"
// The engine's value transcription of the particle block view (title::ParticleViewMath, pinned to the culling set's 0xA08 /
// 0xB90 micro by the engine's Vu0ParticleView test).
#include "games/slus_20909_crash_twinsanity/hle/math/particle_view_math.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_host.h"
#include "te_unclamped.h"

namespace {

using Vector = title::MathLibrary::Vector;
using Matrix = title::MathLibrary::Matrix;

std::optional<Vector> ReadVector(uint32_t address) {
    const float* v = reinterpret_cast<const float*>(te::GuestPointer(address, 16));
    if (!v) return std::nullopt;
    return Vector{v[0], v[1], v[2], v[3]};
}
std::optional<Matrix> ReadMatrix(uint32_t address) {
    const float* m = reinterpret_cast<const float*>(te::GuestPointer(address, 64));
    if (!m) return std::nullopt;
    Matrix out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r * 4 + c];
    return out;
}
void WriteVector(uint32_t address, const Vector& v) {
    float* out = reinterpret_cast<float*>(te::GuestPointer(address, 16));
    if (out) std::memcpy(out, v.data(), 16);
}
void WriteMatrix(uint32_t address, const Matrix& m) {
    float* out = reinterpret_cast<float*>(te::GuestPointer(address, 64));
    if (!out) return;
    for (int r = 0; r < 4; ++r) std::memcpy(out + r * 4, m[r].data(), 16);
}
float* Floats(uint32_t address, size_t count) { return reinterpret_cast<float*>(te::GuestPointer(address, count * 4)); }
#if defined(__vita__)
// a guest vector / matrix READ IN PLACE: Vector is float[4], Matrix float[4][4] (std::array, no padding), the guest
// memory holds the game's floats as it wrote them -- the joint seams' ReadVector / ReadMatrix copies (an optional each) were
// the bulk of their memory traffic (~300 joint, ~370 parent and ~290 Euler calls a hub frame on the device)
const Vector* InPlaceVector(uint32_t address) { return reinterpret_cast<const Vector*>(te::GuestPointer(address, 16)); }
const Matrix* InPlaceMatrix(uint32_t address) { return reinterpret_cast<const Matrix*>(te::GuestPointer(address, 64)); }
static_assert(sizeof(Vector) == 16 && sizeof(Matrix) == 64, "the in-place forms read 4 / 16 consecutive floats");
#endif

// MAX.S / MIN.S order: sign-magnitude as a signed key (+0 above -0; no NaN on the EE FPU)
int64_t FpuKey(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const int64_t magnitude = int64_t(bits & 0x7FFFFFFFu);
    return (bits & 0x80000000u) ? -magnitude - 1 : magnitude;
}

// ---- the particles' views (LoadParticleView): the chunk's matrix (its camera in row 3) and its matrix to the screen -----------
struct ParticleView {
    Vector camera;
    Matrix toScreen;
    std::array<Vector, 6> planes;   // the view's frustum planes in the chunk's space (near, 4 sides, far)
    bool valid = false;
};
constexpr int kParticleViews = 64;
ParticleView g_particleViews[kParticleViews];
constexpr uint32_t kViewPlanes = 0x003B4900;   // g_ViewPlanes (the camera's space)

Vector TransformPlane(const Vector& plane, const Matrix& place) {
    // A plane (n, d) into the space of `place` (the game's TransformPlanes: n' = n * R, d' = d - n' . t)
    Vector out{};
    for (int c = 0; c < 3; ++c) out[c] = plane[0] * place[0][c] + plane[1] * place[1][c] + plane[2] * place[2][c];
    out[3] = plane[3] - (out[0] * place[3][0] + out[1] * place[3][1] + out[2] * place[3][2]);
    return out;
}

// ---- the fast ray casts (0xAB8): the ray in VU0's registers, one triangle's edge tests pending until Finish ----------------
struct RayState {
    Vector start{}, end{};
    bool haveRay = false;
    std::optional<title::CollisionMath::EdgeSetup> pending;
    title::CollisionMath::Triangle triangle{};
    Vector nearest{};
} g_ray;
constexpr float kSideTolerance = 0.0001f;

// The collision edge tests of the second seam keep their pending products between Start and Finish (VU0 registers)
std::optional<title::CollisionMath::EdgeSetup> g_edges;
// The hull support loaded for TriangleHullSupportRange
std::optional<title::CollisionMath::HullSupport> g_support;

// ---- diagnosis: the retail 0xAB8 microprogram itself on the engine's VU interpreter --------------------------------------
// TE_MATH_RAY_MICRO=1 answers every fast ray cast from the retail standard-set micro (parsed from the retail image's DMA chain
// at g_StandardVu0Programs, D_002E6110: two MPG payloads at their micro addresses) instead of the provisional body;
// TE_MATH_RAY_COMPARE=1 runs both and counts their disagreements (printed at exit and every 1000 casts). Development only.
constexpr uint32_t kStandardVu0Programs = 0x002E6110;
// TE_MATH_RAY=retail|legacy: which body answers the fast ray casts. `retail` = the engine's title::RayMath (the 0xAB8
// micro as values: a hit within 0.1 units of the edges counts), `legacy` = the provisional slow-path form the first pad
// logs were authored against (no margin). The DEFAULT is retail since the fixtures were re-recorded from their plans under
// it (with the EE rounding and the drive model); `legacy` stays for A/B against the pre-flip recordings.
bool RetailRays() {
    // (read once on the game thread into a plain int: a function-local static's guard is a DMB per cast on the Vita)
    static int retail = -1;
    if (retail < 0) {
        const char* v = std::getenv("TE_MATH_RAY");
        retail = v && std::strcmp(v, "legacy") == 0 ? 0 : 1;
    }
    return retail != 0;
}
struct RayMicro {
    bool read = false, use = false, compare = false, ready = false;
    std::unique_ptr<VuUnit> vu;
    std::unique_ptr<VuInterpreterTier> tier;
    uint64_t casts = 0, agree = 0, portOnly = 0, microOnly = 0, hitDiffer = 0;
    void Read() {
        if (read) return;
        read = true;
        use = std::getenv("TE_MATH_RAY_MICRO") != nullptr;
        compare = std::getenv("TE_MATH_RAY_COMPARE") != nullptr;
        if (!use && !compare) return;
        std::array<uint8_t, 4096> image{};
        uint32_t at = te::RetailAddress(kStandardVu0Programs);
        for (int tags = 0; tags < 16; ++tags) {
            const uint32_t* tag = reinterpret_cast<const uint32_t*>(te::GuestPointer(at, 16));
            if (!tag) break;
            const uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7;
            const uint8_t* body = te::GuestPointer(at + 16, qwc * 16);
            if (!body) break;
            std::vector<uint8_t> stream(8 + size_t(qwc) * 16);
            std::memcpy(stream.data(), tag + 2, 8);
            std::memcpy(stream.data() + 8, body, size_t(qwc) * 16);
            for (size_t p = 0; p + 4 <= stream.size();) {
                uint32_t code;
                std::memcpy(&code, stream.data() + p, 4);
                p += 4;
                const uint32_t cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
                if (cmd == 0x4A) {   // MPG
                    const size_t n = num ? num : 256, a = size_t(imm) * 8;
                    if (a + n * 8 <= image.size() && p + n * 8 <= stream.size()) std::memcpy(image.data() + a, stream.data() + p, n * 8);
                    p += n * 8;
                }
            }
            at += 16 + qwc * 16;
            if (id == 6 || id == 7) break;
        }
        vu = std::make_unique<VuUnit>(0);
        tier = std::make_unique<VuInterpreterTier>();
        vu->WriteMicroMem(0, image.data(), image.size());
        ready = true;
        std::fprintf(stderr, "[te.math] ray casts: retail 0xAB8 micro on the interpreter (%s%s); the body is %s\n", use ? "answers" : "", compare ? " compared" : "", RetailRays() ? "title::RayMath (TE_MATH_RAY=retail)" : "the provisional slow-path form (TE_MATH_RAY=legacy)");
        std::atexit([] { Report(); });
    }
    static void Report();
    // vi2 and vf31 of one StartRayTriangle/FinishRayTriangle
    uint32_t Run(const Vector& start, const Vector& end, const title::CollisionMath::Triangle& t, const Vector& nearest, Vector& hit) {
        VUregs& r = vu->regs();
        std::memset(&r, 0, sizeof(r));
        r.Reset();
        auto set = [&](int reg, const Vector& v) { for (int k = 0; k < 4; ++k) std::memcpy(&r.VF[reg].I[k], &v[k], 4); };
        set(4, start); set(5, end); set(1, t[0]); set(2, t[1]); set(3, t[2]); set(6, nearest);
        tier->Run(*vu, 0xAB8);
        for (int k = 0; k < 4; ++k) std::memcpy(&hit[k], &r.VF[31].I[k], 4);
        return r.VI[2];
    }
} g_rayMicro;
void RayMicro::Report() {
    if (g_rayMicro.compare)
        std::fprintf(stderr, "[te.math] ray casts compared: %llu (agree %llu, port-only hits %llu, micro-only hits %llu, hits differing %llu)\n",
                     (unsigned long long)g_rayMicro.casts, (unsigned long long)g_rayMicro.agree, (unsigned long long)g_rayMicro.portOnly,
                     (unsigned long long)g_rayMicro.microOnly, (unsigned long long)g_rayMicro.hitDiffer);
}

// Diagnosis toggles: a group of bodies made stubs again (TE_MATH_STUB=collision,rays,joints,particles). Read once into plain
// flags on the game thread: a function-local `static const char* v = getenv()` cost every seam call a guard load + a DMB
// barrier on the Vita (thousands of joint / collision calls a frame)
struct MathStubs {
    bool read = false;
    bool collision = false, rays = false, joints = false, particles = false;
};
MathStubs g_mathStubs;
bool Stubbed(const char* group) {
    if (!g_mathStubs.read) {
        g_mathStubs.read = true;
        if (const char* v = std::getenv("TE_MATH_STUB")) {
            g_mathStubs.collision = std::strstr(v, "collision") != nullptr;
            g_mathStubs.rays = std::strstr(v, "rays") != nullptr;
            g_mathStubs.joints = std::strstr(v, "joints") != nullptr;
            g_mathStubs.particles = std::strstr(v, "particles") != nullptr;
        }
    }
    switch (group[0]) {
        case 'c': return g_mathStubs.collision;
        case 'r': return g_mathStubs.rays;
        case 'j': return g_mathStubs.joints;
        case 'p': return g_mathStubs.particles;
        default: {
            const char* v = std::getenv("TE_MATH_STUB");
            return v && std::strstr(v, group) != nullptr;
        }
    }
}

}  // namespace

extern "C" {

// ---- Platform::Math -----------------------------------------------------------------------------------------------------------
ps2e_f32 ps2e_math_max(ps2e_f32 first, ps2e_f32 second) { return FpuKey(first) >= FpuKey(second) ? first : second; }
ps2e_f32 ps2e_math_min(ps2e_f32 first, ps2e_f32 second) { return FpuKey(first) <= FpuKey(second) ? first : second; }

void ps2e_math_slerp_rotations(ps2e_u32 from, ps2e_u32 to, ps2e_f32 share, ps2e_u32 out) {
    if (Stubbed("joints")) return;
    const auto f = ReadVector(from), t = ReadVector(to);
    if (!f || !t) return;
    WriteVector(out, title::JointMath::Interpolate(*f, *t, share).rotation);
}

void ps2e_math_euler_rotation(ps2e_u32 anglesNow, ps2e_u32 anglesNext, ps2e_u32 moveNow, ps2e_u32 moveNext, ps2e_u32 rotation,
                              ps2e_u32 move) {
    te::gfx::CountCall(te::gfx::kCallEuler);
    if (Stubbed("joints")) return;
#if defined(__vita__)
    // the four vectors read where they lie (InPlaceVector: the guest's floats as written) -- no optional copies;
    // EulerRotation's result is made before either output is written, as before
    const Vector* an = InPlaceVector(anglesNow);
    const Vector* ax = InPlaceVector(anglesNext);
    const Vector* mn = InPlaceVector(moveNow);
    const Vector* mx = InPlaceVector(moveNext);
#else
    const auto an = ReadVector(anglesNow), ax = ReadVector(anglesNext), mn = ReadVector(moveNow), mx = ReadVector(moveNext);
#endif
    if (!an || !ax || !mn || !mx) return;
    if (te::gfx::g_repeatCensus) {
        const std::array<Vector, 4> in{*an, *ax, *mn, *mx};
        te::gfx::NoteRepeat(te::gfx::kCallEuler, in.data(), sizeof in);
    }
    const auto result = title::JointMath::EulerRotation(*an, *ax, *mn, *mx);
    WriteVector(rotation, result.rotation);
    WriteVector(move, result.move);
}

void ps2e_math_turn_rotation(ps2e_u32 by, ps2e_u32 rotation) {
    if (Stubbed("joints")) return;
    const auto b = ReadVector(by), r = ReadVector(rotation);
    if (!b || !r) return;
    WriteVector(rotation, title::JointMath::Turn(*b, *r));
}

void ps2e_math_lerp(ps2e_u32 from, ps2e_u32 to, ps2e_f32 share, ps2e_u32 out) {
    te::gfx::CountCall(te::gfx::kCallLerp);
    if (Stubbed("joints")) return;
    const auto f = ReadVector(from), t = ReadVector(to);
    if (!f || !t) return;
    if (te::gfx::g_repeatCensus) te::gfx::NoteRepeat(te::gfx::kCallLerp, f->data(), 16, t->data(), 16, &share, 4);
    WriteVector(out, title::JointMath::Lerp(*f, *t, share));
}

// JointMatrix (math.cpp): the rotation's basis (the identity without one), divided by the parent's scale and scaled by its own
// when they are given, the translation (none: 0 0 0 1) as the last row, times the parent when there is one
void ps2e_math_joint_matrix(ps2e_u32 rotation, ps2e_u32 parentScale, ps2e_u32 scale, ps2e_u32 translation, ps2e_u32 parent,
                            ps2e_u32 out) {
    te::gfx::CountCall(te::gfx::kCallJointMatrix);
    if (Stubbed("joints")) return;
    if (te::gfx::g_repeatCensus) {
        // the words the body reads (each operand or its absence)
        float in[4 * 4 + 16 + 5] = {};
        const uint32_t addrs[4] = {rotation, parentScale, scale, translation};
        for (int k = 0; k < 4; ++k)
            if (addrs[k]) {
                if (const float* v = reinterpret_cast<const float*>(te::GuestPointer(addrs[k], 16))) std::memcpy(in + 4 * k, v, 16);
                in[32 + k] = 1.f;
            }
        if (parent) {
            if (const float* p = reinterpret_cast<const float*>(te::GuestPointer(parent, 64))) std::memcpy(in + 16, p, 64);
            in[36] = 1.f;
        }
        te::gfx::NoteRepeat(te::gfx::kCallJointMatrix, in, sizeof in);
    }
    Matrix matrix;
#if defined(__vita__)
    // the operands read in place (InPlaceVector / InPlaceMatrix): the same values into the same operations; the
    // result is assembled in `matrix` and written last, as before (an output that is also an input is read first)
    if (rotation) {
        const Vector* r = InPlaceVector(rotation);
        if (!r) return;
        static const Vector kUnit{1, 1, 1, 1};
        const Vector* psv = parentScale ? InPlaceVector(parentScale) : nullptr;
        const Vector* own = scale ? InPlaceVector(scale) : nullptr;
        const auto basis = title::JointMath::RotationBasis(*r, psv ? *psv : kUnit, own);
        for (int row = 0; row < 3; ++row) matrix[row] = basis[row];
    } else {
        matrix[0] = {1, 0, 0, 0};
        matrix[1] = {0, 1, 0, 0};
        matrix[2] = {0, 0, 1, 0};
    }
    matrix[3] = {0, 0, 0, 1};
    if (translation) {
        if (const Vector* t = InPlaceVector(translation)) matrix[3] = *t;
    }
    if (parent) {
        if (const Matrix* p = InPlaceMatrix(parent)) matrix = title::JointMath::MultiplyByParent(matrix, *p);
    }
    WriteMatrix(out, matrix);
    return;
#endif
    if (rotation) {
        const auto r = ReadVector(rotation);
        if (!r) return;
        Vector ps{1, 1, 1, 1};
        if (parentScale) {
            if (const auto v = ReadVector(parentScale)) ps = *v;
        }
        std::optional<Vector> own;
        if (scale) own = ReadVector(scale);
        const auto basis = title::JointMath::RotationBasis(*r, ps, own ? &*own : nullptr);
        for (int row = 0; row < 3; ++row) matrix[row] = basis[row];
    } else {
        matrix[0] = {1, 0, 0, 0};
        matrix[1] = {0, 1, 0, 0};
        matrix[2] = {0, 0, 1, 0};
    }
    matrix[3] = {0, 0, 0, 1};
    if (translation) {
        if (const auto t = ReadVector(translation)) matrix[3] = *t;
    }
    if (parent) {
        if (const auto p = ReadMatrix(parent)) matrix = title::JointMath::MultiplyByParent(matrix, *p);
    }
    WriteMatrix(out, matrix);
}

void ps2e_math_multiply_by_parent(ps2e_u32 matrix, ps2e_u32 parent, ps2e_u32 out) {
    te::gfx::CountCall(te::gfx::kCallMultiplyByParent);
    if (Stubbed("joints")) return;
#if defined(__vita__)
    // both operands read in place; the product is made before `out` is written (out may be the matrix itself)
    const Matrix* m = InPlaceMatrix(matrix);
    const Matrix* p = InPlaceMatrix(parent);
#else
    const auto m = ReadMatrix(matrix), p = ReadMatrix(parent);
#endif
    if (!m || !p) return;
    if (te::gfx::g_repeatCensus) te::gfx::NoteRepeat(te::gfx::kCallMultiplyByParent, m->data(), 64, p->data(), 64);
    WriteMatrix(out, title::JointMath::MultiplyByParent(*m, *p));
}

// The view for the frame's particles (graphics.h LoadParticleView): the chunk's matrix (its camera's place in row 3) and its
// matrix to the screen, kept by index; the frustum's side planes taken into the chunk's space for the block tests
ps2e_s32 ps2e_graphics_load_particle_view(ps2e_u32 matrices, ps2e_s32 index) {
    if (index < 0 || index >= kParticleViews) return index;
    const auto chunk = ReadMatrix(matrices), toScreen = ReadMatrix(matrices + 0x40);
    if (!chunk || !toScreen) return index;
    ParticleView& v = g_particleViews[index];
    v.camera = (*chunk)[3];
    v.toScreen = *toScreen;
    const float* planes = Floats(te::RetailAddress(kViewPlanes), 24);
    for (int i = 0; i < 6; ++i) {
        Vector plane = planes ? Vector{planes[i * 4], planes[i * 4 + 1], planes[i * 4 + 2], planes[i * 4 + 3]} : Vector{0, 0, 0, 0};
        v.planes[i] = TransformPlane(plane, *chunk);
    }
    v.valid = true;
    if (std::getenv("TE_PARTICLE_DIAG")) {
        static int said = 0;
        if (said++ < 64) {
            std::fprintf(stderr, "[te.math.diag] particle view %d chunk matrix rows (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f)\n", index,
                         (*chunk)[0][0], (*chunk)[0][1], (*chunk)[0][2], (*chunk)[0][3], (*chunk)[1][0], (*chunk)[1][1], (*chunk)[1][2], (*chunk)[1][3],
                         (*chunk)[2][0], (*chunk)[2][1], (*chunk)[2][2], (*chunk)[2][3], (*chunk)[3][0], (*chunk)[3][1], (*chunk)[3][2], (*chunk)[3][3]);
            for (int i = 0; i < 6; ++i)
                std::fprintf(stderr, "[te.math.diag]   g_ViewPlanes[%d] (%.3f %.3f %.3f %.2f) -> chunk (%.3f %.3f %.3f %.2f)\n", i, planes ? planes[i * 4] : 0.f, planes ? planes[i * 4 + 1] : 0.f,
                             planes ? planes[i * 4 + 2] : 0.f, planes ? planes[i * 4 + 3] : 0.f, v.planes[i][0], v.planes[i][1], v.planes[i][2], v.planes[i][3]);
        }
    }
    // The PS2 kept the view's VU0 address in the word 0x100 bytes past the matrices
    if (int32_t* kept = reinterpret_cast<int32_t*>(te::GuestPointer(matrices + 0x100, 4))) *kept = index;
    return index;
}

// The distance between a point and the camera's place in a view (0xCB0)
ps2e_f32 ps2e_math_view_distance(ps2e_s32 view, ps2e_u32 point) {
    const float* p = Floats(point, 3);
    if (!p || view < 0 || view >= kParticleViews || !g_particleViews[view].valid) return 0x1.312D00p+23f;
    const Vector& c = g_particleViews[view].camera;
    const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A particle block's matrix for drawing in a view (the culling VU0 set's 0xA08 / 0xB90, TR ParticleBlockView):
// rows 0..2 = the gravity matrix's rows times the view's matrix to the screen (0xB90, the emissions keeping their world
// translation: the view's matrix rows themselves, the gravity ignored); row 3 = the block's place moved TOWARDS THE CAMERA BY
// `scale` WORLD UNITS -- place - unit(place - camera) * scale -- times the view's matrix (0xB90: only that pull, w 1, the
// particles already hold world positions); the distance from the camera; and whether a box of the extents around the place
// lies wholly past one of the view's planes. Returns whether it is out. The earlier provisional body LERPED the place
// towards the camera BY THE FRACTION `scale` (scale 0 put every such emitter AT the camera, w 0: its quads blew up over the
// whole frame -- the cryscave dark flicker).
ps2e_s32 ps2e_math_particle_block_view(ps2e_s32 view, ps2e_s32 keepsTranslation, ps2e_u32 extents, ps2e_u32 gravity, ps2e_u32 place,
                                       ps2e_f32 scale, ps2e_u32 matrix, ps2e_u32 distance) {
    te::gfx::CountCall(te::gfx::kCallParticleView);
    const float* e = Floats(extents, 3);
    const auto g = ReadMatrix(gravity);
    const float* p = Floats(place, 3);
    float* d = Floats(distance, 1);
    if (!e || !g || !p || view < 0 || view >= kParticleViews || !g_particleViews[view].valid) {
        if (d) *d = 0x1.312D00p+23f;
        return 1;
    }
    const ParticleView& v = g_particleViews[view];
    const Vector c = v.camera;
    const Vector box{e[0], e[1], e[2], 1.f};
    const Vector at{p[0], p[1], p[2], 1.f};
    // (the switches read once into plain values: a function-local static's guard is a DMB per call on the Vita)
    static int switchesRead = 0;
    static bool noCull = false, legacy = false;
    static long diag = 0;
    if (!switchesRead) {
        switchesRead = 1;
        noCull = std::getenv("TE_PARTICLE_NOCULL") != nullptr;
        // TE_PARTICLE_VIEW=legacy: the earlier provisional body (the lerp towards the camera) for A/B against the recordings
        const char* pv = std::getenv("TE_PARTICLE_VIEW");
        legacy = pv && std::strcmp(pv, "legacy") == 0;
        const char* pd = std::getenv("TE_PARTICLE_DIAG");   // =N: the first N calls
        diag = pd ? std::max(1L, std::atol(pd) > 1 ? std::atol(pd) : 400L) : 0L;
    }
    Matrix out{};
    float far = 0.f;
    bool outside = false;
    if (legacy) {
        const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
        far = std::sqrt(dx * dx + dy * dy + dz * dz);
        for (int i = 0; i < 6 && !outside; ++i) {
            const Vector& plane = v.planes[i];
            const float centre = plane[0] * p[0] + plane[1] * p[1] + plane[2] * p[2] + plane[3];
            const float radius = std::fabs(plane[0]) * e[0] + std::fabs(plane[1]) * e[1] + std::fabs(plane[2]) * e[2];
            if (centre + radius < 0.f) outside = true;
        }
        Matrix block = *g;
        const float pull = 1.f - scale;
        Vector lerped{p[0] * scale + c[0] * pull, p[1] * scale + c[1] * pull, p[2] * scale + c[2] * pull, 1.f};
        if (keepsTranslation) lerped = {lerped[0] + (*g)[3][0], lerped[1] + (*g)[3][1], lerped[2] + (*g)[3][2], 1.f};
        block[3] = lerped;
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 4; ++col) {
                float s = 0.f;
                for (int k = 0; k < 4; ++k) s += block[r][k] * v.toScreen[k][col];
                out[r][col] = s;
            }
    } else {
    // The view record as the game laid it out for the micro: the planes in two groups of four lanes (|n| xyz, n xyz, d), the
    // camera's place, the matrix to the screen
    title::ParticleViewMath::View record{};
    for (int grp = 0; grp < 2; ++grp) {
        const int base = grp * 7;
        for (int lane = 0; lane < 4; ++lane) {
            const int pi = grp * 4 + lane;
            const Vector plane = pi < 6 ? v.planes[pi] : Vector{0.f, 0.f, 0.f, 0.f};
            for (int k = 0; k < 3; ++k) {
                record.qw[base + k][lane] = std::fabs(plane[k]);
                record.qw[base + 3 + k][lane] = plane[k];
            }
            record.qw[base + 6][lane] = plane[3];
        }
    }
    record.qw[14] = {c[0], c[1], c[2], 1.f};
    for (int r = 0; r < 4; ++r) record.qw[15 + r] = v.toScreen[r];
    const auto answer = title::ParticleViewMath::BlockView(record, box, *g, at, scale, keepsTranslation != 0);
    out = answer.matrix;
    far = answer.distance;
    outside = answer.flags != 0;
    }
    if (noCull) outside = false;
    if (d) *d = far;
    WriteMatrix(matrix, out);
    if (diag) {
        static long said = 0;
        if (said++ < diag) {
            const float w = out[3][3] != 0.f ? out[3][3] : 1.f;
            std::fprintf(stderr, "[te.math.diag] particle block view %d keep %d place (%.1f %.1f %.1f) extents (%.1f %.1f %.1f) scale %g far %.1f cam (%.1f %.1f %.1f) centre -> (%.1f %.1f %.1f w %.1f) planes0 (%.2f %.2f %.2f %.1f) -> %s\n",
                         view, keepsTranslation, p[0], p[1], p[2], e[0], e[1], e[2], scale, far, c[0], c[1], c[2],
                         out[3][0] / w, out[3][1] / w, out[3][2] / w, out[3][3], v.planes[0][0], v.planes[0][1], v.planes[0][2], v.planes[0][3], outside ? "OUT" : "in");
            if (outside) {
                std::fprintf(stderr, "[te.math.diag]   planes (centre / radius):");
                for (int i = 0; i < 6; ++i) {
                    const Vector& plane = v.planes[i];
                    const float centre = plane[0] * p[0] + plane[1] * p[1] + plane[2] * p[2] + plane[3];
                    const float radius = std::fabs(plane[0]) * e[0] + std::fabs(plane[1]) * e[1] + std::fabs(plane[2]) * e[2];
                    std::fprintf(stderr, " %d: %.1f / %.1f%s", i, centre, radius, centre + radius < 0.f ? " OUT" : "");
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    return outside ? 1 : 0;
}

// The fast ray casts (0xAB8), by the game's own slow path: the ray's ends on the plane's two sides, both ends not on the same
// side of any edge, then the plane's intersection; the hit (its w the share along the ray) when nearer than the nearest so far
void ps2e_math_set_ray(ps2e_u32 start, ps2e_u32 end) {
    if (Stubbed("rays")) return;
    const auto s = ReadVector(start), e = ReadVector(end);
    if (!s || !e) return;
    g_ray.start = *s;
    g_ray.end = *e;
    g_ray.haveRay = true;
    g_ray.pending.reset();
}

void ps2e_math_start_ray_triangle(ps2e_u32 vertices, ps2e_u32 nearest) {
    te::gfx::CountCall(te::gfx::kCallStartRay);
    if (Stubbed("rays")) return;
    const float* v = Floats(vertices, 12);
    const auto n = ReadVector(nearest);
    if (!v || !n || !g_ray.haveRay) { g_ray.pending.reset(); return; }
    for (int i = 0; i < 3; ++i) g_ray.triangle[i] = {v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3]};
    g_ray.nearest = *n;
    // the edges' set-up only feeds the provisional body; the retail one (the default) tests the triangle itself
    g_rayMicro.Read();
    g_ray.pending = RetailRays() && !g_rayMicro.ready ? title::CollisionMath::EdgeSetup{}
                                                      : title::CollisionMath::EvaluateStartEdges(g_ray.start, g_ray.end, g_ray.triangle);
}

namespace {
// The provisional body's answer: whether the triangle is hit nearer, and the hit
bool PortRayTest(const title::CollisionMath::EdgeSetup& setup, Vector& out) {
    if (-kSideTolerance < setup.plane_values[0] * setup.plane_values[1]) return false;
    const auto edges = title::CollisionMath::EvaluateFinishEdges(setup);
    for (int edge = 0; edge < 6; edge += 2) {
        const float a = edges.values[edge], b = edges.values[edge + 1];
        if ((kSideTolerance < a && kSideTolerance < b) || (a < -kSideTolerance && b < -kSideTolerance)) return false;
    }
    const auto plane = title::MathLibrary::EvaluateTrianglePlane(g_ray.triangle[0], g_ray.triangle[1], g_ray.triangle[2]);
    const Vector& pl = plane.plane;
    const Vector& s = g_ray.start;
    const Vector& e = g_ray.end;
    const float dx = e[0] - s[0], dy = e[1] - s[1], dz = e[2] - s[2];
    const float denominator = pl[0] * dx + pl[1] * dy + pl[2] * dz;
    const float share = -(pl[0] * s[0] + pl[1] * s[1] + pl[2] * s[2] + pl[3]) / denominator;
    if (!(share < g_ray.nearest[3])) return false;
    out = {s[0] + dx * share, s[1] + dy * share, s[2] + dz * share, share};
    return true;
}
}  // namespace

ps2e_s32 ps2e_math_finish_ray_triangle(ps2e_u32 hit) {
    te::gfx::CountCall(te::gfx::kCallFinishRay);
    if (Stubbed("rays")) return 0;
    if (!g_ray.pending) return 0;
    const auto setup = *g_ray.pending;
    g_ray.pending.reset();
    Vector portHit{};
    bool port;
    if (RetailRays()) {
        const title::RayMath::Result r = title::RayMath::Test(title::RayMath::Ray{g_ray.start, g_ray.end}, {g_ray.triangle[0], g_ray.triangle[1], g_ray.triangle[2]}, g_ray.nearest);
        port = r.nearer;
        if (port) portHit = r.hit;
    } else
    {
        port = PortRayTest(setup, portHit);
    }
    g_rayMicro.Read();
    if (g_rayMicro.ready) {
        Vector microHit{};
        const bool micro = g_rayMicro.Run(g_ray.start, g_ray.end, g_ray.triangle, g_ray.nearest, microHit) == 0x900;
        if (g_rayMicro.compare) {
            g_rayMicro.casts++;
            if (port == micro) {
                g_rayMicro.agree++;
                if (micro && microHit[3] != portHit[3]) g_rayMicro.hitDiffer++;
            } else if (port) g_rayMicro.portOnly++;
            else g_rayMicro.microOnly++;
            if (port != micro && g_rayMicro.portOnly + g_rayMicro.microOnly <= 40)
                std::fprintf(stderr, "[te.math] ray cast disagrees: port %d micro %d; ray (%.3f %.3f %.3f)->(%.3f %.3f %.3f) tri (%.2f %.2f %.2f)(%.2f %.2f %.2f)(%.2f %.2f %.2f) nearest %g micro w %g\n",
                             int(port), int(micro), g_ray.start[0], g_ray.start[1], g_ray.start[2], g_ray.end[0], g_ray.end[1], g_ray.end[2],
                             g_ray.triangle[0][0], g_ray.triangle[0][1], g_ray.triangle[0][2], g_ray.triangle[1][0], g_ray.triangle[1][1], g_ray.triangle[1][2],
                             g_ray.triangle[2][0], g_ray.triangle[2][1], g_ray.triangle[2][2], g_ray.nearest[3], microHit[3]);
            if (g_rayMicro.casts % 1000 == 0) RayMicro::Report();
        }
        if (g_rayMicro.use) {
            if (!micro) return 0;
            WriteVector(hit, microHit);
            return 1;
        }
    }
    if (!port) return 0;
    WriteVector(hit, portHit);
    return 1;
}

#if defined(__vita__)
// te_unclamped.h's census: calls taken on the unclamped path / left to the engine's clamped one (counted on the game
// thread, read by the pace bookkeeping on the drawing thread: relaxed 32-bit atomics, wrapping deltas)
std::atomic<uint32_t> g_unclamped_fast{0}, g_unclamped_clamped{0};
#endif

// ---- the second seam's collision maths (collisionmaths.cpp) over title::CollisionMath ---------------------------------------
void ps2e_vu0_start_triangle_edge_tests(ps2e_u32 start, ps2e_u32 end, ps2e_u32 triangle, ps2e_u32 values) {
    if (Stubbed("collision")) return;
    const auto s = ReadVector(start), e = ReadVector(end);
    const float* t = Floats(triangle, 12);   // CollisionHit: its three vertices first
    float* out = Floats(values, 2);
    if (!s || !e || !t || !out) return;
    title::CollisionMath::Triangle tri;
    for (int i = 0; i < 3; ++i) tri[i] = {t[i * 4], t[i * 4 + 1], t[i * 4 + 2], t[i * 4 + 3]};
#if defined(__vita__)
    // the same arithmetic without the PS2 FPU clamps when they cannot act (te_unclamped.h): bit-identical
    if (te::unclamped::StartEdgesApply(*s, *e, tri)) {
        g_edges = te::unclamped::StartEdges(*s, *e, tri);
        g_unclamped_fast.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_edges = title::CollisionMath::EvaluateStartEdges(*s, *e, tri);
        g_unclamped_clamped.fetch_add(1, std::memory_order_relaxed);
    }
#else
    g_edges = title::CollisionMath::EvaluateStartEdges(*s, *e, tri);
#endif
    out[0] = g_edges->plane_values[0];
    out[1] = g_edges->plane_values[1];
}

void ps2e_vu0_finish_triangle_edge_tests(ps2e_u32 values) {
    if (Stubbed("collision")) return;
    float* out = Floats(values, 6);
    if (!out || !g_edges) return;
#if defined(__vita__)
    const bool fast = te::unclamped::FinishEdgesApply(*g_edges);
    (fast ? g_unclamped_fast : g_unclamped_clamped).fetch_add(1, std::memory_order_relaxed);
    const auto result = fast ? te::unclamped::FinishEdges(*g_edges) : title::CollisionMath::EvaluateFinishEdges(*g_edges);
#else
    const auto result = title::CollisionMath::EvaluateFinishEdges(*g_edges);
#endif
    for (int i = 0; i < 6; ++i) out[i] = result.values[i];
}

// MakeVertexDifferences (physics.h VertexDifferences {matrix, vertices, count, otherMatrix, otherVertices, otherCount,
// differences, moved}): every vertex of both hulls through its matrix into moved (the first's, then the other's), then every
// vertex of the first less every vertex of the other into differences
void ps2e_vu0_make_vertex_differences(ps2e_u32 request) {
    if (Stubbed("collision")) return;
    const uint32_t* r = reinterpret_cast<const uint32_t*>(te::GuestPointer(request, 32));
    if (!r) return;
    const auto matrix = ReadMatrix(r[0]), otherMatrix = ReadMatrix(r[3]);
    const uint32_t count = r[2], otherCount = r[5];
    if (!matrix || !otherMatrix || count == 0 || otherCount == 0 || count > 1024 || otherCount > 1024) return;
    const float* vertices = Floats(r[1], size_t(count) * 4);
    const float* otherVertices = Floats(r[4], size_t(otherCount) * 4);
    float* differences = Floats(r[6], size_t(count) * otherCount * 4);
    float* moved = Floats(r[7], (size_t(count) + otherCount) * 4);
    if (!vertices || !otherVertices || !differences || !moved) return;
    for (uint32_t i = 0; i < count; ++i) {
        const Vector point{vertices[i * 4], vertices[i * 4 + 1], vertices[i * 4 + 2], vertices[i * 4 + 3]};
        const auto out = title::MathLibrary::EvaluateTransformVector(*matrix, point).output;
        std::memcpy(moved + i * 4, out.data(), 16);
    }
    for (uint32_t j = 0; j < otherCount; ++j) {
        const Vector point{otherVertices[j * 4], otherVertices[j * 4 + 1], otherVertices[j * 4 + 2], otherVertices[j * 4 + 3]};
        const auto out = title::MathLibrary::EvaluateTransformVector(*otherMatrix, point).output;
        std::memcpy(moved + (count + j) * 4, out.data(), 16);
    }
    const float w = (*matrix)[2][3];
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < otherCount; ++j) {
            float* d = differences + (size_t(i) * otherCount + j) * 4;
            for (int k = 0; k < 3; ++k) d[k] = moved[i * 4 + k] - moved[(count + j) * 4 + k];
            d[3] = w;
        }
}

// GroupPoints: each group of four points (four quadwords) packed as four x, four y, four z (THREE quadwords), in place: the
// output is denser than the input (48 bytes a group against 64), so every group is read before the first is written
void ps2e_vu0_group_points(ps2e_u32 points, ps2e_s32 groups) {
    if (Stubbed("collision")) return;
    if (groups <= 0 || groups > 1024) return;
    float* p = Floats(points, size_t(groups) * 16);
    if (!p) return;
    std::vector<title::CollisionMath::PointGroup> packed; packed.resize(size_t(groups));
    for (int32_t g = 0; g < groups; ++g) {
        std::array<Vector, 4> in;
        for (int i = 0; i < 4; ++i) in[i] = {p[g * 16 + i * 4], p[g * 16 + i * 4 + 1], p[g * 16 + i * 4 + 2], p[g * 16 + i * 4 + 3]};
        packed[size_t(g)] = title::CollisionMath::PackPoints(in);
    }
    for (int32_t g = 0; g < groups; ++g)
        for (int i = 0; i < 3; ++i) std::memcpy(p + g * 12 + i * 4, packed[size_t(g)][i].data(), 16);
}

// The range (least, greatest) of the groups' points along an axis
void ps2e_vu0_groups_range(ps2e_u32 groups, ps2e_s32 count, ps2e_u32 axis, ps2e_u32 range) {
    if (Stubbed("collision")) return;
    if (count <= 0 || count > 1024) return;
    const float* g = Floats(groups, size_t(count) * 12);
    const auto a = ReadVector(axis);
    float* out = Floats(range, 2);
    if (!g || !a || !out) return;
#if defined(__vita__)
    // The same products and sums without the PS2 FPU clamps when they cannot act (te_unclamped.h): bit-identical.
    if (te::unclamped::Applies() && te::unclamped::FiniteWords(g, size_t(count) * 12) && te::unclamped::FiniteWords(a->data(), 3)) {
        te::unclamped::GroupsRange(g, count, a->data(), out);
        g_unclamped_fast.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_unclamped_clamped.fetch_add(1, std::memory_order_relaxed);
#endif
    float least = INFINITY, greatest = -INFINITY;
    for (int32_t i = 0; i < count; ++i) {
        title::CollisionMath::PointGroup group;
        for (int k = 0; k < 3; ++k) group[k] = {g[i * 12 + k * 4], g[i * 12 + k * 4 + 1], g[i * 12 + k * 4 + 2], g[i * 12 + k * 4 + 3]};
        const auto projected = title::CollisionMath::ProjectGroup(group, *a);
        for (float v : projected.values) {
            least = std::min(least, v);
            greatest = std::max(greatest, v);
        }
    }
    out[0] = least;
    out[1] = greatest;
}

void ps2e_vu0_sixteen_groups_range(ps2e_u32 groups, ps2e_u32 axis, ps2e_u32 range) { ps2e_vu0_groups_range(groups, 16, axis, range); }

#if defined(__vita__)
// te_prof.cpp's CPU census reads the unclamped paths' counts
void te_unclamped_counts(uint32_t* fast, uint32_t* clamped) {
    *fast = g_unclamped_fast.load(std::memory_order_relaxed);
    *clamped = g_unclamped_clamped.load(std::memory_order_relaxed);
}
#endif

// LoadTriangleHullSupport: a triangle's three vertices against eight hull vertices, kept for the range tests
void ps2e_vu0_load_triangle_hull_support(ps2e_u32 triangle, ps2e_u32 hullVertices) {
    if (Stubbed("collision")) return;
    const float* t = Floats(triangle, 12);
    const float* h = Floats(hullVertices, 32);
    if (!t || !h) return;
    title::CollisionMath::Triangle tri;
    for (int i = 0; i < 3; ++i) tri[i] = {t[i * 4], t[i * 4 + 1], t[i * 4 + 2], t[i * 4 + 3]};
    std::array<Vector, 8> hull;
    for (int i = 0; i < 8; ++i) hull[i] = {h[i * 4], h[i * 4 + 1], h[i * 4 + 2], h[i * 4 + 3]};
    g_support = title::CollisionMath::BuildHullSupport(tri, hull);
}

void ps2e_vu0_triangle_hull_support_range(ps2e_u32 axis, ps2e_u32 range) {
    if (Stubbed("collision")) return;
    const auto a = ReadVector(axis);
    float* out = Floats(range, 2);
    if (!a || !out || !g_support) return;
    const auto projected = title::CollisionMath::ProjectHullSupport(*g_support, *a);
    out[0] = projected.range[0];
    out[1] = projected.range[1];
}

}  // extern "C"
