// engine/ipu/hle/decoded_picture.h -- DecodedPicture: one decoded video picture,
// RGBA8 row-major (4 bytes per pixel, `width * 4` bytes per row), the unit the
// video decoder produces and the frame writer consumes. A method-less POD.
#pragma once

#include <cstdint>
#include <vector>

namespace ps2r::ipu {

struct DecodedPicture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    // FfmpegVideoDecoder::SetPlanarOutput (the Vita's movie textures): the picture's 4:2:0 planes as decoded, no RGBA
    // conversion -- `planes` holds Y (width x height), then U, then V (each (width + 1) / 2 x (height + 1) / 2), packed
    // one after the other with no row padding, and `rgba` stays empty. planar = 0: RGBA in `rgba` (the default).
    int planar = 0;
    std::vector<uint8_t> planes;
};

}  // namespace ps2r::ipu
