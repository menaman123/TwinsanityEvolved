#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

namespace title {

// The skinned (type-04 material) submodel geometry: TR skins.cpp's SetSkinDMA
// hands VU1 a precompiled packet per submodel (a RET tag + a VIF stream of
// batches: GIFtag, count, int->float scale, then per vertex the int16 position
// + normal.x, the int16 ST + normal.yz, the float weights with the palette
// index in the low bits and the influence count/ADC in .w, the int8 colour)
// and the resident program skins, lights and projects it (vs_skinned_lit_strip:
// the 0xcb0 scale+skin node, the 0xb30 clip mark when drawn clipped, the
// 0x1720 lit-strip emitter). Decode converts the packet once at load; Build
// lays the same quadwords out in a host-side VU data image together with the
// joints and the skin's data rows and runs the library's own program bodies
// over it, so the vertices are the lowered program's bit for bit, with no
// VIF/VU1/GIF transport. A draw owns values; nothing is read back from guest
// memory at delivery.
class SkinGeometry {
public:
    using Qw = std::array<uint32_t, 4>;
    struct Batch {
        Qw tag{};        // the strip GIFtag (NLOOP = vertices, EOP, PRE PRIM 4, NREG 3 ST/RGBAQ/XYZ2)
        Qw count{};      // .x = vertices * 4 (the VU quadword count), .y = the tag's low word
        Qw scale{};      // .xy = the int->float scales
        std::vector<Qw> vertices;  // 4 quadwords per vertex, exactly as the VIF unpacks leave them
        uint32_t Vertices() const { return uint32_t(vertices.size() / 4); }
    };
    struct Part { std::vector<Batch> batches; uint32_t vertices = 0; };
    // The skin's per-draw inputs (SetSkinDMA): the joints' matrices (4 QW each,
    // as WriteJoints unpacks them), the skin's data (WriteSkinData: the matrix
    // to the screen, the lights' directions in the skin's space (a row each),
    // their colours and the ambient), the clipped mode's matrix to camera space
    // and clip vector (WriteClipData), and the material's UV payload.
    struct Inputs {
        std::vector<Qw> joints;        // 4 * joint count quadwords
        std::array<Qw, 4> to_screen{};
        std::array<Qw, 3> lights{};    // rows; transposed into the data like WriteSkinData
        std::array<Qw, 3> colours{};
        Qw ambient{};
        bool clipped = false;
        std::array<Qw, 4> to_camera{};
        Qw clip{};
        std::array<float, 2> scroll{};
    };
    enum class Result { Ok, InvalidAsset, InvalidGeometry, UnsupportedState, LimitExceeded };
    static constexpr uint32_t kMaxBatchVertices = 64;
    static constexpr uint32_t kMaxJoints = 100;
    static constexpr size_t kMaxVertices = 65536;

    // TR resources.cpp SkinRead's packet (ReadPacket of `size` bytes): the RET
    // tag then the VIF stream. Only the fixed authored shape is accepted (NOP /
    // STCYCL / STMOD / STROW / MSCAL 0 / the four UNPACKs); anything else is an
    // invalid asset. `vertices` is the skin's declared count for the submodel.
    static Result Decode(const uint8_t* bytes, size_t size, uint32_t vertices, Part& out);

    // One batch through the program: the strip's vertices under `state` (the
    // material's DrawIr state; off_x/off_y, prim 4) assembled exactly as the GS
    // state machine assembles a triangle strip (VertexKick::AssembleStrip).
    // `clipped` selects the 0xb30 clip-mark node. Output vertices are appended
    // as one DrawIr per batch with a non-empty triangle list.
    static Result Build(const Part& part, const Inputs& inputs, const ps2r::gs::DrawIr& state,
                        std::vector<ps2r::gs::DrawIr>& out);
    // Build's batches as runs of one vertex array (`ends`: each batch's end; batches without a triangle add none) -- the
    // same vertices without a DrawIr and a vector per batch. Only where Build takes the Vita's direct skinning (a plain
    // skin on the Vita; a clipped one while the fast paths are on): UnsupportedState otherwise (and for an unsupported
    // state), the caller then Builds.
    static Result BuildRuns(const Part& part, const Inputs& inputs, const ps2r::gs::DrawIr& state,
                            std::vector<ps2r::gs::VertexRec>& verts, std::vector<uint32_t>& ends);
    // The Vita's direct skinning for the CLIPPED plain skins and the BLEND skins (skin_geometry.cpp FastSkinRuns: plain
    // floats, the guard-band marks of the clip program; on by default). false = their VU programs, as before. Elsewhere
    // a no-op (the desktop always runs the programs).
    static void SetFastPaths(bool on);

    // ---- blend skins (TR skins.cpp SetBlendSkinDMA / WriteBlendPart) --------------------------------------------------
    // A blend skin's part is ONE batch whose packet (BlendPart::packet, a chain fragment CALLed from the bucket) carries
    // the VIF stream of the skin layout WITHOUT the MSCAL: the programs follow in the chain -- without shapes the scale
    // pass (0x12a0), with shapes the shared-prefix displacement passes (0x1400 for the first shape over the int vertices,
    // 0x15a0 for each further shape over the floats; each adds ITOF0(row) * factors.xyz * weight to the position), then
    // the material's dispatcher (the float-input skin 0x1040, the clip mark when clipped, the lit emitter 0x1720).
    // DecodePart accepts the unterminated batch; `vertices` 0 takes the batch's own count.
    static Result DecodePart(const uint8_t* bytes, size_t size, uint32_t vertices, Part& out);
    struct Shape {
        float weight = 0.f;
        const uint8_t* rows = nullptr;   // the shape's int8 xyzw displacement per vertex (BlendPart::shapeAddresses[shape])
        uint32_t count = 0;              // rows (>= the part's vertices)
    };
    struct BlendInputs {
        std::array<float, 3> factors{};  // BlendPart::shapeFactors of the submodel's first part
        std::vector<Shape> shapes;       // in the game's order (the DrawBlendSkin `shapes` list with its `weights`)
    };
    static Result BuildBlend(const Part& part, const Inputs& inputs, const BlendInputs& blend,
                             const ps2r::gs::DrawIr& state, std::vector<ps2r::gs::DrawIr>& out);
    // BuildBlend's batches as runs of one vertex array (as BuildRuns): the Vita's direct path only (the fast paths on);
    // UnsupportedState otherwise, the caller then BuildBlends.
    static Result BuildBlendRuns(const Part& part, const Inputs& inputs, const BlendInputs& blend,
                                 const ps2r::gs::DrawIr& state, std::vector<ps2r::gs::VertexRec>& verts,
                                 std::vector<uint32_t>& ends);
};

}  // namespace title
