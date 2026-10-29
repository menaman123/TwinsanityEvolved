#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"

#include <cfenv>
#include <cmath>
#include <cstring>

#include "engine/vu/vu_float.h"
#include <limits>

namespace title {

// The Vita: the operations as the CPU does them, unclamped and in its own rounding -- the exact form (operands and
// results clamped through memory, the VU's rounding) cost the game thread milliseconds a frame there; elsewhere exact.
#if defined(__vita__)
class CullingMath::Rounding { public: Rounding() {} };
#else
class CullingMath::Rounding {
public:
    Rounding() : previous_(std::fegetround()) {
        if (previous_ != FE_TOWARDZERO) std::fesetround(FE_TOWARDZERO);
    }
    ~Rounding() {
        if (previous_ != FE_TOWARDZERO) std::fesetround(previous_);
    }
private:
    int previous_;
};
#endif

float CullingMath::Finite(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t magnitude = bits & 0x7fffffff;
    if (magnitude >= 0x7f800000) bits = (bits & 0x80000000) | 0x7f7fffff;
    else if (magnitude < 0x00800000) bits &= 0x80000000;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

#if defined(__vita__)
float CullingMath::Product(float a, float b) { return a * b; }
float CullingMath::Add(float a, float b) { return a + b; }
float CullingMath::Difference(float a, float b) { return a - b; }
float CullingMath::MultiplyAdd(float a, float b, float sum) { return a * b + sum; }
#else
float CullingMath::Product(float a, float b) {
    volatile float left = Finite(a), right = Finite(b);
    volatile float result = left * right;
    return Finite(result);
}

float CullingMath::Add(float a, float b) {
    volatile float left = Finite(a), right = Finite(b);
    volatile float result = left + right;
    return Finite(result);
}

float CullingMath::Difference(float a, float b) {
    volatile float left = Finite(a), right = Finite(b);
    volatile float result = left - right;
    return Finite(result);
}

float CullingMath::MultiplyAdd(float a, float b, float sum) {
    volatile float left = Finite(a), right = Finite(b), accumulated = Finite(sum);
    // VuFloat::Fma: std::fma's result (the Vita: its product and sum rounded apart; newlib's fmaf is software)
    return Finite(::VuFloat::Fma(left, right, accumulated));
}
#endif

CullingMath::Vector CullingMath::Transform(const Matrix& basis, const Vector& coordinates) {
    Vector result;
    for (size_t lane = 0; lane < result.size(); ++lane) {
        float sum = Product(basis[0][lane], coordinates[0]);
        sum = MultiplyAdd(basis[1][lane], coordinates[1], sum);
        sum = MultiplyAdd(basis[2][lane], coordinates[2], sum);
        result[lane] = MultiplyAdd(basis[3][lane], coordinates[3], sum);
    }
    return result;
}

void CullingMath::TransformInto(const Matrix& basis, const Vector& coordinates, Vector& out) {
    for (size_t lane = 0; lane < out.size(); ++lane) {
        float sum = Product(basis[0][lane], coordinates[0]);
        sum = MultiplyAdd(basis[1][lane], coordinates[1], sum);
        sum = MultiplyAdd(basis[2][lane], coordinates[2], sum);
        out[lane] = MultiplyAdd(basis[3][lane], coordinates[3], sum);
    }
}

CullingMath::Link CullingMath::LoadLink(
        const View& view, const Matrix& chunk, const Matrix& object) {
    Rounding rounding;
    Link link;
    for (size_t row = 0; row < chunk.size(); ++row) {
        link.to_clip[row] = Transform(view.to_clip, chunk[row]);
        link.to_screen[row] = Transform(view.to_screen, chunk[row]);
    }
    // Source camera composition begins with translation, unlike Transform.
    for (size_t lane = 0; lane < link.camera.size(); ++lane) {
        float sum = Product(object[3][lane], 1.0f);
        sum = MultiplyAdd(object[0][lane], view.camera[0], sum);
        sum = MultiplyAdd(object[1][lane], view.camera[1], sum);
        link.camera[lane] = MultiplyAdd(object[2][lane], view.camera[2], sum);
    }
    return link;
}

int32_t CullingMath::Distance(const Vector& camera, const Vector& position) {
    Vector squared;
    for (size_t lane = 0; lane < 3; ++lane) {
        const float delta = Difference(camera[lane], position[lane]);
        squared[lane] = Product(delta, delta);
    }
    const float sum = Add(Add(squared[0], squared[1]), squared[2]);
#if defined(__vita__)
    // sum >= 2^31 on the bits (a VFP compare is a VMRS flag transfer that waits for the VFP pipeline on the Cortex-A9):
    // non-negative (top bit clear), at least 0x4f000000 (2^31) and not a NaN (at most 0x7f800000, +infinity)
    uint32_t bits;
    std::memcpy(&bits, &sum, sizeof(bits));
    if (int32_t(bits) >= 0x4f000000 && bits <= 0x7f800000u) return std::numeric_limits<int32_t>::max();
#else
    if (sum >= 2147483648.0f) return std::numeric_limits<int32_t>::max();
#endif
    return static_cast<int32_t>(sum);
}

CullingMath::Model CullingMath::LoadModel(const Link& link, const Matrix& model) {
    Rounding rounding;
    Model result;
    for (size_t row = 0; row < model.size(); ++row)
        TransformInto(link.to_screen, model[row], result.to_screen[row]);
    result.distance = Distance(link.camera, model[3]);
    return result;
}

CullingMath::Classification CullingMath::Classify(
        const PlaneColumns& planes, const Vector& center, const Vector& half) {
    Classification result{};
    for (size_t plane = 0; plane < 4; ++plane) {
        float radius = Product(planes.absolute_normals[0][plane], half[0]);
        radius = MultiplyAdd(planes.absolute_normals[1][plane], half[1], radius);
        radius = MultiplyAdd(planes.absolute_normals[2][plane], half[2], radius);
        float distance = Product(planes.normals[0][plane], center[0]);
        distance = MultiplyAdd(planes.normals[1][plane], center[1], distance);
        distance = MultiplyAdd(planes.normals[2][plane], center[2], distance);
        distance = MultiplyAdd(planes.offsets[plane], 1.0f, distance);
        const bool high_negative = std::signbit(MultiplyAdd(radius, 1.0f, distance));
        const bool low_negative = std::signbit(MultiplyAdd(-Finite(radius), 1.0f, distance));
        // Source CullResult exposes these nonzero plane masks. Sign, including
        // negative zero, is the criterion; replacing it with < 0 loses a case.
        const uint32_t mask = 0x80u >> plane;
        if (high_negative && low_negative) result.outside |= mask;
        if (high_negative || low_negative) result.clipped |= mask;
    }
    return result;
}

CullingMath::Classification CullingMath::TestBox(const Frustum& frustum, const Box& box) {
    Rounding rounding;
    Vector center{}, half{};
    for (size_t axis = 0; axis < 3; ++axis) {
        half[axis] = Product(Difference(box.maximum[axis], box.minimum[axis]), 0.5f);
        center[axis] = MultiplyAdd(box.maximum[axis], 0.5f, Product(box.minimum[axis], 0.5f));
    }
    const auto sides = Classify(frustum.sides, center, half);
    const auto depth = Classify(frustum.depth, center, half);
    return {sides.outside | depth.outside, sides.clipped | depth.clipped};
}

CullingMath::PlaneColumns CullingMath::TransformPlanes(
        const PlaneColumns& planes, const Matrix& model) {
    PlaneColumns result;
    for (size_t plane = 0; plane < 4; ++plane) {
        for (size_t axis = 0; axis < 3; ++axis) {
            float sum = Product(planes.normals[0][plane], model[axis][0]);
            sum = MultiplyAdd(planes.normals[1][plane], model[axis][1], sum);
            sum = MultiplyAdd(planes.normals[2][plane], model[axis][2], sum);
            result.normals[axis][plane] = sum;
            result.absolute_normals[axis][plane] = std::fabs(sum);
        }
        float sum = Product(planes.normals[0][plane], model[3][0]);
        sum = MultiplyAdd(planes.normals[1][plane], model[3][1], sum);
        sum = MultiplyAdd(planes.normals[2][plane], model[3][2], sum);
        result.offsets[plane] = MultiplyAdd(planes.offsets[plane], 1.0f, sum);
    }
    return result;
}

CullingMath::Classification CullingMath::ClassifyAt(
        const PlaneColumns& planes, const Matrix& model, const Vector& center, const Vector& half) {
#if defined(__vita__)
    // TransformPlanes' column `plane` then Classify's test of it -- the same Product / MultiplyAdd calls on the same
    // operands in the same order as the two functions, the transformed column kept in locals instead of a PlaneColumns
    // written and read back (the Vita: ~1000 instructions of the three Classify(TransformPlanes()) pairs a box test made
    // were the struct's stores and loads, the calls and the branches).
    // The four planes' values first, the sign tests after, and no branch in either (Finite's clamp and the masks on the
    // bits): one basic block, so the compiler interleaves the planes' chains. The Cortex-A9's VFP issues one instruction a
    // cycle but a dependent VMLA waits 8 cycles for its accumulator and a dependent VMUL 5: each plane alone is a chain of
    // ~10 dependent operations, and a branch after each plane (the clamp's, the masks') kept the next plane's work behind it.
    // The same operations on the same operands: the same bits in every rounding and flush mode.
    float high[4], low[4];
    for (size_t plane = 0; plane < 4; ++plane) {
        float normal[3], absolute[3];
        for (size_t axis = 0; axis < 3; ++axis) {
            float sum = Product(planes.normals[0][plane], model[axis][0]);
            sum = MultiplyAdd(planes.normals[1][plane], model[axis][1], sum);
            sum = MultiplyAdd(planes.normals[2][plane], model[axis][2], sum);
            normal[axis] = sum;
            absolute[axis] = std::fabs(sum);
        }
        float offset = Product(planes.normals[0][plane], model[3][0]);
        offset = MultiplyAdd(planes.normals[1][plane], model[3][1], offset);
        offset = MultiplyAdd(planes.normals[2][plane], model[3][2], offset);
        offset = MultiplyAdd(planes.offsets[plane], 1.0f, offset);
        float radius = Product(absolute[0], half[0]);
        radius = MultiplyAdd(absolute[1], half[1], radius);
        radius = MultiplyAdd(absolute[2], half[2], radius);
        float distance = Product(normal[0], center[0]);
        distance = MultiplyAdd(normal[1], center[1], distance);
        distance = MultiplyAdd(normal[2], center[2], distance);
        distance = MultiplyAdd(offset, 1.0f, distance);
        high[plane] = MultiplyAdd(radius, 1.0f, distance);
        // Finite(radius) on its bits without a branch: +-inf / NaN -> +-FLT_MAX, a denormal -> its signed zero
        uint32_t bits;
        std::memcpy(&bits, &radius, sizeof(bits));
        const uint32_t sign = bits & 0x80000000u, magnitude = bits & 0x7fffffffu;
        uint32_t finite = magnitude < 0x00800000u ? sign : bits;
        finite = magnitude >= 0x7f800000u ? (sign | 0x7f7fffffu) : finite;
        float clamped;
        std::memcpy(&clamped, &finite, sizeof(clamped));
        low[plane] = MultiplyAdd(-clamped, 1.0f, distance);
    }
    uint32_t outside = 0, clipped = 0;
    for (size_t plane = 0; plane < 4; ++plane) {
        // std::signbit as the bits' top bit; the plane's mask 0x80 >> plane
        uint32_t h, l;
        std::memcpy(&h, &high[plane], sizeof(h));
        std::memcpy(&l, &low[plane], sizeof(l));
        h >>= 31;
        l >>= 31;
        outside |= (h & l) << (7 - plane);
        clipped |= (h | l) << (7 - plane);
    }
    return {outside, clipped};
#else
    return Classify(TransformPlanes(planes, model), center, half);
#endif
}

CullingMath::BoxView CullingMath::TestBoxAt(
        const Frustum& frustum, const Link& link, const Box& box, const Matrix& model) {
    BoxView result = TestBoxAtVisibility(frustum, link, box, model);
    if (result.visibility.outside) return result;
    result.to_screen = ScreenMatrix(link, model);
    if (result.visibility.clipped) result.to_clip = ClipMatrix(link, model);
    return result;
}

CullingMath::BoxView CullingMath::TestBoxAtVisibility(
        const Frustum& frustum, const Link& link, const Box& box, const Matrix& model) {
    Rounding rounding;
    Vector center{}, half{};
    for (size_t axis = 0; axis < 3; ++axis) {
        half[axis] = Product(Difference(box.maximum[axis], box.minimum[axis]), 0.5f);
        center[axis] = MultiplyAdd(box.maximum[axis], 0.5f, Product(box.minimum[axis], 0.5f));
    }
    const auto depth = ClassifyAt(frustum.depth, model, center, half);
    const auto sides = ClassifyAt(frustum.sides, model, center, half);
    BoxView result{};
    result.distance = Distance(link.camera, model[3]);
    result.visibility.clipped = sides.outside | (depth.clipped & 0x40u);
    result.visibility.outside = depth.outside | result.visibility.clipped;
    if (result.visibility.outside) return result;

    const auto far = ClassifyAt(frustum.far_sides, model, center, half);
    result.visibility.clipped = far.clipped | depth.clipped;
    return result;
}

#if defined(__vita__)
CullingMath::Classification CullingMath::TestBoxAtVisibilityOf(
        const Frustum& frustum, const Link& link, const float* box, const Matrix& model, int32_t* distance) {
    // TestBoxAtVisibility, statement for statement: box.minimum = box[0..3], box.maximum = box[4..7]
    Rounding rounding;
    Vector center{}, half{};
    for (size_t axis = 0; axis < 3; ++axis) {
        half[axis] = Product(Difference(box[4 + axis], box[axis]), 0.5f);
        center[axis] = MultiplyAdd(box[4 + axis], 0.5f, Product(box[axis], 0.5f));
    }
    const auto depth = ClassifyAt(frustum.depth, model, center, half);
    const auto sides = ClassifyAt(frustum.sides, model, center, half);
    Classification visibility;
    *distance = Distance(link.camera, model[3]);
    visibility.clipped = sides.outside | (depth.clipped & 0x40u);
    visibility.outside = depth.outside | visibility.clipped;
    if (visibility.outside) return visibility;

    const auto far = ClassifyAt(frustum.far_sides, model, center, half);
    visibility.clipped = far.clipped | depth.clipped;
    return visibility;
}
#endif

CullingMath::Matrix CullingMath::ScreenMatrix(const Link& link, const Matrix& model) {
    Rounding rounding;
    Matrix screen;
    for (size_t row = 0; row < 4; ++row) TransformInto(link.to_screen, model[row], screen[row]);
    return screen;
}

CullingMath::Matrix CullingMath::ClipMatrix(const Link& link, const Matrix& model) {
    Rounding rounding;
    Matrix clip;
    for (size_t row = 0; row < 4; ++row) TransformInto(link.to_clip, model[row], clip[row]);
    return clip;
}

CullingMath::BoxView CullingMath::TestBoxAtReference(
        const Frustum& frustum, const Link& link, const Box& box, const Matrix& model) {
    Rounding rounding;
    Vector center{}, half{};
    for (size_t axis = 0; axis < 3; ++axis) {
        half[axis] = Product(Difference(box.maximum[axis], box.minimum[axis]), 0.5f);
        center[axis] = MultiplyAdd(box.maximum[axis], 0.5f, Product(box.minimum[axis], 0.5f));
    }
    const auto depth = Classify(TransformPlanes(frustum.depth, model), center, half);
    const auto sides = Classify(TransformPlanes(frustum.sides, model), center, half);
    BoxView result{};
    result.distance = Distance(link.camera, model[3]);
    result.visibility.clipped = sides.outside | (depth.clipped & 0x40u);
    result.visibility.outside = depth.outside | result.visibility.clipped;
    if (result.visibility.outside) return result;

    const auto far = Classify(TransformPlanes(frustum.far_sides, model), center, half);
    result.visibility.clipped = far.clipped | depth.clipped;
    Matrix screen;
    for (size_t row = 0; row < 4; ++row) screen[row] = Transform(link.to_screen, model[row]);
    result.to_screen = screen;
    if (result.visibility.clipped) {
        Matrix clip;
        for (size_t row = 0; row < 4; ++row) clip[row] = Transform(link.to_clip, model[row]);
        result.to_clip = clip;
    }
    return result;
}

}  // namespace title
