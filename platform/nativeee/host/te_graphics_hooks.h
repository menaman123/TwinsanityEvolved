// te_graphics_hooks.h -- the graphics owner's hooks for the OTHER host modules (graphics.cpp defines them): the
// ps2e_draw device (created on demand) and a picture quad into the movies bucket, for the movie player.
#pragma once
#include <cstddef>
#include <cstdint>

typedef struct ps2e_draw ps2e_draw;

extern "C" {
// The frame's ps2e_draw device, created if needed (nullptr when the host has no window / GL context).
ps2e_draw* te_graphics_device(void);
// Every ps2e_draw call on that device other than from the frame itself goes through here: fn(user) runs on the
// graphics side's render thread when there is one (the GL context lives there) and waits for it, else inline.
void te_graphics_on_render(void (*fn)(void* user), void* user);
// The same, posted and NOT waited for: fn(user) runs on the render thread after the work posted before it and before
// the next frame handed to it (inline when there is no render thread); fn owns user (frees it).
void te_graphics_post_render(void (*fn)(void* user), void* user);
// A textured quad over the frame pixels [x0,x1) x [y0,y1) (u/v fractions of the texture) into the movies bucket, drawn
// at the next Present as the picture is: the texture's colours as they are, no blending, in front of everything.
void te_graphics_record_movie_quad(uint32_t texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                                   float v1);
// the retail present -- the frame shown as the GS's dithered 16-bit copy (CT24 -> CT16, DTHE + Sony's DIMX) --
// on or off from the next present (the display options page's toggle; te::Settings::present_ct16 / TE_GFX_PRESENT_CT16=1
// at start). Returns 1 when the engine linked has the CT16 scanout, 0 when it has not (the request is kept for the log).
int te_graphics_set_present_ct16(int on);
int te_graphics_present_ct16(void);
// The presenter's census row for the runner's exit lines and the harness:
//   present=<rate>:<shown fps>:<interpolated frames>:<snaps>:<latency ms>:<late presents> (pictures N, presents M, ...)
// shown fps = presents per wall second since the first present; snaps = frames shown as the newest picture because of a
// camera cut; latency = present instant - the shown content's time on the pacer's clock, averaged; late = slots served
// more than one slot period after their time.
void te_graphics_present_census(char* out, size_t size);
}
