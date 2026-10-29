#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

// Feature macro for consumers pinned across engine versions: MeshGeometry::Prepare / BuildPrepared are present.
#define TITLE_MESH_GEOMETRY_PREPARED 1

namespace title {

// Shared authored position/colour/STQ geometry for source shader families 01 and A.
// Converted once when the model is loaded.
// A draw owns values; it has no packet, data-memory image, or microcode entry.
class MeshGeometry {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;
    struct Vertex {
        std::array<float, 3> position;
        std::array<float, 3> stq;
        std::array<uint8_t, 4> colour;
        bool restart = false;
    };
    struct Strip { std::vector<Vertex> vertices; };
    struct Mesh { std::vector<Strip> strips; };
    struct View {
        Matrix screen{};
        Matrix clip{};
        Vector guard{};
        std::array<float, 2> scroll{};
        bool clipped = true;
    };
    enum class Result { Ok, InvalidAsset, InvalidGeometry, UnsupportedState, LimitExceeded };
    enum class Layout { Sky, Unlit };
    static constexpr size_t kMaxVertices = 65536;
    static constexpr size_t kMaxOutputVertices = kMaxVertices * 24;

    // TR resources.cpp ModelRead's serialized submodel. These two shader families
    // have one fixed authored layout; this is not a general DMA/VIF interpreter.
    // The caller supplies its declared vertex count and bounds, at load time.
    // UV float mantissas also store colour bytes: both meanings are retained.
    static Result Decode(const uint8_t* bytes, size_t size, uint32_t vertices, Mesh& out,
                         Layout layout = Layout::Unlit);

    // The source shader's strip gate, six-plane clipping, interpolation and
    // screen projection. The full state comes from a source-owned material.
    // Each strip visits every instance in source block order.
    // Clipped fans precede the surviving strip, as the source shader does.
    // Failure leaves out unchanged; success can produce no visible triangles.
    static Result Build(const Mesh& mesh, const std::vector<View>& instances, const ps2r::gs::DrawIr& state,
                        std::vector<ps2r::gs::DrawIr>& out);

    // ---- prepared builds: the same arithmetic as Build, the view-independent work done once ----------------
    // A decomp's native platform arm draws every placed and rigid instance of a model every frame through a
    // fresh matrix. Build does all of its per-vertex validation, flushing, copying, hashing and bounding per
    // call; Prepare does the mesh's part of that ONCE (validated exactly as Build validates: the strip sizes,
    // kMaxVertices, finite values -- the same Result for the same mesh), and BuildPrepared then projects one
    // view and hands each draw Build would have published (a clipped triangle's fan, a strip's body; never an
    // empty one; in Build's order) to `sink` as that draw's triangle-list vertices. The vertices are
    // BIT-IDENTICAL to Build's for the same mesh, view and state (test_mesh_geometry_prepared): the
    // source programs' transform, guard-band test, six-plane clipping, projection and rounding are the same
    // functions, only reordered so that nothing a view does not depend on is recomputed. The colours may be
    // replaced per build (lit models, shadow stamps): `colours` is one RGBA per vertex in strip order
    // (Prepared::vertex_count of them) or null for the mesh's own. The state is checked as Build checks it.
    struct Prepared {
        struct StripRange { uint32_t first, count; };
        std::vector<StripRange> strips;
        std::vector<Vector> positions;              // x y z 1, flushed as the programs flush operands
        std::vector<Vector> stq;                    // s t q 0, flushed
        std::vector<std::array<uint8_t, 4>> colours;
        std::vector<uint8_t> restart;               // 1 = the strip restarts at this vertex (no triangle)
        size_t vertex_count = 0;
    };
    using Sink = void (*)(void* user, const ps2r::gs::VertexRec* vertices, size_t count, bool fan);
    static Result Prepare(const Mesh& mesh, Prepared& out);
    // The Vita's prepared builds through its Advanced SIMD unit (4 lanes per multiply-add; rounding to nearest where the
    // scalar path rounds toward zero: a lane may differ by a unit in the last place). Off by default; a no-op elsewhere.
    static void SetVectorTransform(bool on);
    static bool VectorTransform();
    static Result BuildPrepared(const Prepared& prepared, const View& view, const ps2r::gs::DrawIr& state,
                                const std::array<uint8_t, 4>* colours, Sink sink, void* user);
    // (the Vita's builders) BuildPrepared in two passes: every vertex's values first -- the clip transform, the guard
    // scaling and both outcodes when clipped, the projected vertex -- each a function of the vertex and the view only, then
    // the strips' assembly and fans on them as before (the same draws, the same order, bit for bit). The first pass's
    // arithmetic without its clamps where the exponent budget shows every clamp the identity and the stored S / T / Q pass
    // the audit (exponent_budget.h; modes: 1 the default, 0 the one-pass form, 2 both forms compared, 3 the two passes on
    // the clamped forms, 4 the two passes' output compared with the one-pass form's, the latter drawn); elsewhere, and with
    // SetVectorTransform on, the one-pass form. The census: builds clamp-free,
    // builds clamped, vertices compared that differed (mode 2).
    static void SetCertified(int mode);
    static void CertifiedCensus(uint64_t out[5]);   // + (mode 4) builds compared with the one-pass form, those differing
};

}  // namespace title
