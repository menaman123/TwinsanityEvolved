// platform/nativeee (host side) -- the seam epoch: a count of the game's host entries that may change what a placement
// reads.
//
// Every host entry from the game moves it (the module lane's generated thunks, seam_host_thunks.c; the native lane's
// generated forwarders) except the culling's and the placement's own imports (tools/gen_seam.py EPOCH_TRANSPARENT: the
// only seams scenery.cpp's loops call between two DrawPlacedModel calls), and the module's resource code moves it before
// it changes a material, a shader, a texture or a model (ps2e_gfx2_resources_changed, platform/nativeee/wasm/*.cpp). The
// game's own code never writes the platform's structures (they are opaque to it: game/include/platform/graphics.h).
//
// So while the epoch stands, no byte of a material, a shader, a texture or a model has changed, and none of the state the
// placement takes for a stretch of instances (the frame's GS origin, the record kind, the device): an answer verified
// against those bytes in this epoch is still the answer (graphics_scene.cpp: the material slots, the placement context).
// Anything else a placement reads -- the chunk view's matrices, the world matrix, the texture map's and the state pool's
// own counters -- is read or checked per placement as before.
#pragma once

extern "C" {
extern unsigned te_seam_epoch;   // (unsigned on both sides: the C thunks declare it so)
}
