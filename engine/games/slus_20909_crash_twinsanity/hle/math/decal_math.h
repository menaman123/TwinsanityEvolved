#pragma once

#include <array>
#include <cstdint>

namespace title {

// The decal VU0 set's AgeDecal programs (micro entries 0x1C0 and 0x360; TR src/platform/ps2/renderer/particles.cpp
// AgeDecal over LoadDecalView / LoadDecalType) as value arithmetic. The VU0 data image the game keeps: the far frustum's
// four side planes in the chunk's space, transposed (quadwords 0..3: n.x, n.y, n.z, d of the four planes in the lanes),
// the near plane four times, transposed (4..7), the other chunk's matrix (8..11; 0x360 only) and each decal type's
// variants from quadword 0x10 (eight each: four colour keys, then four size keys whose quadword 5 carries the life rate
// in w). The decal: its place (w its life left), its frame's two half-word quadwords (the corners as 1.15 fixed point)
// and its variant's quadword address.
//
// What it answers (DecalLook): the decal's place (0x360: taken through the other chunk's matrix; its w 1 / the
// bounding radius), its colour and size interpolated between the variant's keys by the life spent (in thirds), the
// corners' two half-diagonal offsets in 4.12 and, in the w of the first, the OR of the MAC sign flags of (plane distance -
// radius) over the near plane and the four side planes: non-zero when the decal does not lie wholly inside the view.
// Pinned to the retail microprograms on the interpreter by tests/vu/test_vu0_decal_age.cpp (a private fixture).
class DecalMath {
public:
    using Vector = std::array<float, 4>;
    static constexpr uint32_t kSameChunk = 0x1C0;    // a decal in the camera's chunk
    static constexpr uint32_t kOtherChunk = 0x360;   // a decal placed in another chunk (its matrix at quadwords 8..11)
    static constexpr uint32_t kDataQuadwords = 256;  // VU0 data memory (4 KB)

    struct Decal {
        Vector place{};                   // vf01: xyz, w the life left (seconds)
        std::array<int32_t, 4> frame{};   // vf02: the corners' first half words
        std::array<int32_t, 4> more{};    // vf03: the second half words
        uint16_t variant = 0;             // vi01: the variant's quadword address in data memory
    };
    struct Look {
        std::array<uint32_t, 4> place{};    // vf28 (float bits)
        std::array<uint32_t, 4> colour{};   // vf29 (integers)
        std::array<uint32_t, 4> offsets{};  // vf31 (4.12 integers; w = the outside flags, sign-extended)
        std::array<uint32_t, 4> more{};     // vf30 (4.12 integers)
    };
    // `data` is the VU0 data image (kDataQuadwords quadwords); `program` kSameChunk or kOtherChunk. `dataFinite`: the
    // caller vouches that no word of the image is an infinity or a NaN -- the Vita then leaves out the VU's operand and
    // result clamps where none can act (round toward zero with flush-to-zero, the decal's place finite: the same values,
    // bit for bit); false keeps the clamped form everywhere
    // (`unclamped`, when given: whether the unclamped form answered -- the census)
    static Look Age(const Vector* data, uint32_t program, const Decal& decal, bool dataFinite = false, bool* unclamped = nullptr);
#if defined(DECAL_MATH_TEST_UNCLAMPED)
    static Look AgeUnclampedForTest(const Vector* data, uint32_t program, const Decal& decal);
#endif
};

}  // namespace title
