#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace title {

// Source culling operations, with the view/link lifetime explicit. These are
// ordinary math data; installing NTSC entries also requires the owning renderer
// to replace LoadCullingView/CullLoadLink and the consumers of CullResult.
class CullingMath {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;

    struct View {
        Matrix to_clip;
        Matrix to_screen;
        Vector camera;
    };
    struct Link {
        Matrix to_clip;
        Matrix to_screen;
        Vector camera;
    };
    struct Model {
        Matrix to_screen;
        int32_t distance;
    };
    struct PlaneColumns {
        std::array<Vector, 3> absolute_normals;
        std::array<Vector, 3> normals;
        Vector offsets;
    };
    struct Frustum {
        PlaneColumns sides;
        PlaneColumns depth;
        PlaneColumns far_sides;
    };
    struct Box {
        Vector minimum;
        Vector maximum;
    };
    struct Classification {
        uint32_t outside;
        uint32_t clipped;
    };
    struct BoxView {
        Classification visibility;
        int32_t distance;
        std::optional<Matrix> to_screen;
        std::optional<Matrix> to_clip;
    };

    // TR renderer/culling.cpp:449. The camera is transformed by object, while
    // the two view matrices are composed with chunk. Camera.w is unused.
    static Link LoadLink(const View& view, const Matrix& chunk, const Matrix& object);
    // TR renderer/culling.cpp:465, chunkview.cpp:190,258. Distance is the
    // saturating integer squared xyz distance, not its square root.
    static Model LoadModel(const Link& link, const Matrix& model);
    // TR renderer/culling.cpp:478, chunkview.cpp:195. The loaded columns hold
    // four side planes and the near/far pair. Nonzero masks mean out/partial.
    static Classification TestBox(const Frustum& frustum, const Box& box);
    // Source renderer/culling.cpp:492. Rejects far-plane intersections;
    // visible objects produce screen data, with clip data only when needed.
    static BoxView TestBoxAt(
        const Frustum& frustum, const Link& link, const Box& box, const Matrix& model);
    // TestBoxAt in two halves, bit for bit: the visibility and the distance (to_screen / to_clip left empty), and the
    // matrices of a visible box -- the model's rows through the link's matrix to the screen, and to the clip space when
    // the box is clipped. A caller that may never read the matrices (an instance's test answers only in or out) computes
    // them when it reads them, from the same link and model.
    static BoxView TestBoxAtVisibility(
        const Frustum& frustum, const Link& link, const Box& box, const Matrix& model);
#if defined(__vita__)
    // TestBoxAtVisibility's two answers without its BoxView -- 150 bytes (two optional matrices) zeroed and handed
    // back through memory per call -- and without the caller's copies of the box and the model: `box` = minimum xyzw then
    // maximum xyzw, `model` = the model's 16 floats as they lie in guest memory. The same operations on the same operands
    // in the same order (the same bits in every rounding mode): the visibility returned, the distance through `distance`.
    static Classification TestBoxAtVisibilityOf(
        const Frustum& frustum, const Link& link, const float* box, const Matrix& model, int32_t* distance);
#endif
    static Matrix ScreenMatrix(const Link& link, const Matrix& model);
    static Matrix ClipMatrix(const Link& link, const Matrix& model);
    // TestBoxAt as it was composed before (Classify(TransformPlanes()) through a PlaneColumns, the matrices made at once):
    // the same answers, the previous work -- the platform's in-run A/B arm (TE_AB=gamecpu2)
    static BoxView TestBoxAtReference(
        const Frustum& frustum, const Link& link, const Box& box, const Matrix& model);

private:
    class Rounding;
    static float Finite(float value);
    static float Product(float a, float b);
    static float Add(float a, float b);
    static float Difference(float a, float b);
    static float MultiplyAdd(float a, float b, float sum);
    static Vector Transform(const Matrix& basis, const Vector& coordinates);
    // Transform written into `out` (no Vector returned through the stack: a matrix's four rows are four of these)
    static void TransformInto(const Matrix& basis, const Vector& coordinates, Vector& out);
    static int32_t Distance(const Vector& camera, const Vector& position);
    static Classification Classify(
        const PlaneColumns& planes, const Vector& center, const Vector& half);
    static PlaneColumns TransformPlanes(const PlaneColumns& planes, const Matrix& model);
    // Classify(TransformPlanes(planes, model), center, half), bit for bit: each plane's column made and tested in turn
    // (the Vita: the 28 transformed columns never go through memory)
    static Classification ClassifyAt(
        const PlaneColumns& planes, const Matrix& model, const Vector& center, const Vector& half);
};

}  // namespace title
