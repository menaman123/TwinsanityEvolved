// render/backends/gl/gl_fill_census.h -- the Vita's FILL census: where the GPU's fragment work goes, counted by the GPU.
//
// One frame in N (TE_GFX_VITA_FILL=N; unset / 0 = off) every draw of the frame gets its own GXM visibility counter
// (sceGxmSetFrontVisibilityTestIndex with the INCREMENT op: the samples that pass the draw's depth and stencil tests --
// on the device's deferred renderer the fragments that reach the tag buffer, i.e. what an opaque draw may be shaded for
// and what a punch-through (discarding) draw IS shaded for; in Vita3K the host's occlusion query: the samples kept) and
// its screen box (the true one when the producer hands it over: FillCensusBox; the draw's own otherwise). The counters
// are read back at the next sampled frame (N >= 8 frames later: the GPU is done with them) and folded per program --
// draws, box pixels, samples passed -- per sampled frame, in frames' worth of the frame target. Elsewhere: no-ops.
#pragma once

#include <string>

namespace ps2r {
namespace render {

// the program codes FillCensusDraw takes: the lean variants 0..6 (GlShaderLibrary's order), the GPU-skinning programs
// kFillSkin + variant, the fast and the full PS2 programs
constexpr int kFillLean = 0, kFillSkin = 8, kFillFast = 16, kFillFull = 17, kFillCodes = 18;

// The producer (graphics.cpp SubmitBuckets): the frame about to be drawn; true when it is sampled (its draws then pass
// their true boxes through FillCensusBox). `frame_w` x `frame_h`: the frame target (the boxes' clip and the unit).
bool FillCensusFrame(int frame_w, int frame_h);
// the next draw's true screen box (frame pixels x0, y0, x1, y1); a box with x1 < x0 = not known (the draw's own is taken)
void FillCensusBox(const float box[4]);
// whether the frame being drawn is sampled (a producer computes its boxes only then)
bool FillCensusSampling();
// The backend (GlDrawExecutor, right before it binds the draw's program): the vertices (or a skin's indices) the next
// draw submits -- the vertex program's work
void FillCensusVerts(unsigned count);
// The backend (GlShaderLibrary::SelectPs2 / SelectSkin): a draw with program `code` about to be submitted, its own box
// `px` pixels, over vertex path `path` (0 the generic vertex program, 1 mesh, 2 lit mesh, 3 vertices, 4 raw, 5 skin); on
// a sampled frame it gets the next counter
void FillCensusDraw(int code, double px, int path);
// GlDevice::Present: the sampled frame's last draw was made (the counters off before the scanout)
void FillCensusEnd();
// GlDevice::Present, right after the swap -- the one moment the GXM context is outside a scene (vitaGL begins the next
// scene at the next draw / clear): the visibility buffer is handed to GXM here (sceGxmSetVisibilityBuffer refuses within a
// scene; device-run8.log: "OFF (sceGxmSetVisibilityBuffer refused)" when it was set from the first frame's submission)
void FillCensusAfterSwap();
// The producer (graphics.cpp SubmitBuckets): the frame bucket the next draws belong to (0..31) -- the census folds per
// bucket too (the sky, the scenery, the sea, the effects, the HUD ...)
void FillCensusGroup(int group);
// the backend, right before a vitaGL call that draws a quad of its own inside the frame (a glClear: vitaGL
// clears by drawing a full-screen quad): its samples count in the census's "vitaGL's own" counter, not the last draw's
void FillCensusForeign();
// The producer: the next draw's rasterised area in frame pixels when it is known (a vertex draw: the sum of its
// triangles' areas, clipped to the frame -- the fragments it generates, overdraw included; a box overstates it)
void FillCensusArea(double px);
// The window's lines since the last call ("" when nothing was sampled); each ends in '\n'
std::string FillCensusLines();

}  // namespace render
}  // namespace ps2r
