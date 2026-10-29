#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

namespace title {

// Type-0B shadow volumes. All data is load-time geometry or source instance
// values; no register file, packet, program image or hardware owner is needed.
class ShadowGeometry {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;
    struct Vertex {
        std::array<float, 3> position{};
        bool restart = false;
        // Source corner's orientation flag (bit 5 of its authored normal.x).
        // The clipped fan uses this flag; the surviving strip uses vertex parity.
        bool reverse_fan = false;
    };
    struct Strip { std::vector<Vertex> vertices; };
    struct Mesh { std::vector<Strip> strips; };
    struct View {
        Matrix screen{}, clip{};
        Vector guard{};
        std::array<float, 2> offset{};
    };
    enum class Result { Ok, InvalidAsset, InvalidGeometry, LimitExceeded };
    static constexpr size_t kMaxVertices = 65536;
    static constexpr size_t kMaxOutputVertices = 2 * 24 * kMaxVertices;
    // ModelRead's authored part record, decoded once while its bytes are owned.
    // Type 0B requires the normal stream even though it does not light vertices:
    // normal.x bit 5 selects the clipped fan's orientation. Failure is atomic.
    static Result Decode(const uint8_t*, size_t, uint32_t vertices, Mesh&);
    // Whether Build fills each output draw's record snapshot (its vertex hash and screen box, the oracles' and the GS
    // path's census). On by default; a consumer that reads only the vertices turns it off (the Vita's builders: the
    // 64-bit hash per vertex was a large part of a shadow build there). Set before the first Build.
    static void SetSnapshots(bool on);
    // Each source strip visits the dark node's instances, then the light
    // node's instances. Fans precede that node/instance's surviving strip.
    // Keep output groups intact: the pass counts each strip/fan only once.
    static Result Build(const Mesh&, const std::vector<View>&,
                        std::vector<ps2r::gs::DrawIr>& out);
    // Build's groups for one view as runs of one vertex array (`ends`: each group's end), without a DrawIr, a record
    // snapshot or a vector per group: the same vertices in the same order (Build's consumers that read only the vertices).
    // On a failure both are left empty.
    static Result BuildRuns(const Mesh&, const View&, std::vector<ps2r::gs::VertexRec>& verts, std::vector<uint32_t>& ends);

    // ---- the shared-position build (the Vita's builders) -----------------------------------------------------------------
    // A mesh's strips share most of their positions (the authored volumes: about a third of a part's vertices are distinct).
    // Share indexes them once per mesh (per strip vertex: its distinct position, by the position's bits). BuildRunsShared
    // gives BuildRuns' groups bit for bit: every per-vertex value of Instance -- the clip transform, the guard scaling, both
    // outcodes, the screen projection and the quantized vertex -- is a function of the position's bits and the view, so it
    // is computed once per distinct position; a strip whose vertices all share an outcode bit of the clip test is skipped
    // (Instance drops each of its triangles and publishes nothing for it); each strip's assembly (the winding test, the
    // fans, the parity, the bodies) runs as Instance's on those values.
    struct Shared {
        std::vector<std::array<float, 3>> positions;   // the distinct positions
        std::vector<std::vector<uint16_t>> index;      // per strip, per vertex: its distinct position
        int exponent_lo = 0x7FFFFFFF, exponent_hi = -0x7FFFFFFF - 1;   // the positions' nonzero exponents (exponent_budget.h)
    };
    static Result Share(const Mesh&, Shared&);
    struct SharedCensus {
        uint64_t vertices = 0, positions = 0, projected = 0, strips = 0, strips_skipped = 0;
        uint64_t certified = 0, exact = 0, check_differ = 0;   // the per-position phases (see SetCertified)
    };
    static Result BuildRunsShared(const Mesh&, const Shared&, const View&, std::vector<ps2r::gs::VertexRec>& verts,
                                  std::vector<uint32_t>& ends, SharedCensus* census = nullptr);
    // The per-position arithmetic (the Vita): mode 1 (the default) computes it without the clamps where the exponent budget
    // (exponent_budget.h: the matrices' and the positions' exponents, checked per build) shows every clamp the identity,
    // the screen side's stored projections audited after (none denormal) -- the values are then the clamped forms' bit for
    // bit; otherwise the clamped forms. 0: the clamped forms always. 2 (diagnosis): both, compared
    // (SharedCensus::check_differ). Elsewhere the clamped forms.
    static void SetCertified(int mode);
    static int Certified();
};

}  // namespace title
