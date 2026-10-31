#pragma once

#include <array>
#include <cstdint>

namespace title {

// The standard VU0 set's ray/triangle test (micro entry 0xAB8; TR include/platform/math.h SetRay / StartRayTriangle /
// FinishRayTriangle) as value arithmetic: the ray's ends kept (vf4, vf5), each triangle (vf1-vf3) tested against the
// nearest hit so far (vf6, its w the share along the ray), the program's flag word (VI2: kNearer when the triangle was
// hit nearer) and its hit (vf31: the point, w the share). Bit-identical to the retail microprogram on the interpreter
// (tests/vu/test_vu0_ray_triangle.cpp pins it over the private micro image).
class RayMath {
public:
    using Vector = std::array<float, 4>;
    using Triangle = std::array<Vector, 3>;
    static constexpr uint32_t kNearer = 0x900;

    struct Ray {
        Vector start, end;
    };
    struct Result {
        bool nearer;       // VI2 == kNearer
        uint32_t flags;    // VI2 as the program leaves it
        Vector hit;        // vf31 (meaningful when nearer)
    };
    static Ray SetRay(const Vector& start, const Vector& end) { return Ray{start, end}; }
    static Result Test(const Ray& ray, const Triangle& triangle, const Vector& nearest);
    // What the fast ray cast's seam reads of Test: whether the triangle is hit nearer (`nearer`) and then the hit, written to
    // `hit` only when it is. On the desktop Test itself; the Vita's form stops at the first test that rules the hit out --
    // the ray's ends on one side of the plane, the share not below the nearest, an edge's distance -- each value it does
    // compute made by the same expressions in the same order as the Vita's Test.
    static bool Nearer(const Ray& ray, const Triangle& triangle, const Vector& nearest, Vector& hit);
};

}  // namespace title
