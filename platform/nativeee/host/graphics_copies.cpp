// The screen copies coalesced (TE_GFX_COPY_COALESCE; every platform).
//
// THE COPY: a material whose shader is a screen-copy type (0x10 / 0x11 -- the cavern's crystals, Ice Climb's ice) began
// its packet on the PS2 by copying the frame as drawn so far into the buffer it then samples as its texture (TR shaders.cpp
// WriteScreenCopy). The port records each such part's first draw with `copy_frame` (graphics_scene.cpp Emitter::
// BeginScreenCopy) and SubmitBuckets copies the frame target into the device's copy texture right before it
// (ps2e_draw_copy_frame). The part's draws sample that copy (state.texture == PS2E_DRAW_TEXTURE_FRAME_COPY: u, v = the
// fractions of the frame, clamped), its later draws without a copy of their own included. On the desktop a copy is a
// glCopyTexSubImage2D of the frame. On the PS Vita it is the frame target's GXM scene ended (its tiles resolved, its depth /
// stencil stored), a scene of the copy's framebuffer (vitaGL's scene-start mask update + the full-target copy pass), and the
// frame's scene begun again (its depth / stencil loaded, the mask update, the restore pass: gl_device.cpp VitaCopyRect) --
// two scene ends and four full-target passes a copy. Ice Climb's cave on the device: 6-8 copies a picture, 13-17 scene ends,
// ~1 ms of GPU a copy (the device log's windows, least squares), the drawing thread blocked up to 12 ms a frame in
// sceGxmEndScene behind the GPU's queue of scenes.
//
// THE RULE: the copy texture holds the frame as it stood at the frame's last copy taken. A copy draw's copy may be
// skipped when, in every texel the draws sampling it read, the frame still is what it was then -- when no draw since that
// copy wrote a pixel of the region they sample. Then the texture already holds, in that region, exactly what the new copy
// would hold, so every sample -- and every pixel those draws write -- is the same value; and nothing else reads the
// texture (the next copy taken overwrites it whole; the Vita's restore pass reads it only within a copy taken). The rest
// of the texture is older than a new copy would have made it -- unread until the next copy taken replaces it.
//   * The draws sampling this copy: the copy draw and the draws after it in its bucket's drawing order up to the bucket's
//     next copy draw (a part's later draws -- graphics_scene.cpp's Emitter appends them in the copy draw's bucket, after it;
//     the draw order never moves a draw across a copy draw: graphics_zorder.cpp keeps the frame readers' runs, the batch
//     plan's barriers). A sampler first in its bucket (one whose copy draw lies in an earlier bucket) cannot be recorded --
//     counted ("orphan samplers": must stay 0) and the coalescing switched off for the run if one is seen.
//   * The region they sample: the box of their vertices' u, v (the device's STQ for these draws: q = 1, so u, v are the
//     sampled fractions themselves, affine over each triangle -- every sample lies in the vertices' hull), clamped to the
//     frame as the CLAMP wrap does, padded 2 frame pixels each way (the bilinear footprint, the copy's physical texels at
//     any internal scale: a frame pixel is >= 1 texel). A sampler that is a mesh (none is: the screen-copy passes are
//     always built on the CPU) or has a non-finite u, v: the whole frame.
//   * The pixels a draw since the copy may have written: a vertex draw's box (its vertices +-1 pixel: the rasteriser
//     covers only pixel centres inside its triangles), a native mesh's box (ps2e_draw_mesh_screen_box: its bounds' corners
//     through its matrix, padded 2; unknown -- a corner at or behind the eye, no bounds kept (not the Vita), a GPU skin --
//     = the whole frame), nothing for a draw that writes no colour (no_colour_write: FBMSK all set -- the shadow volumes'
//     counting draws, the stencil clears, depth clears); the Vita's sky layers drawn at a stop: the whole frame.
// The decision needs only the order SubmitBuckets draws in (each draw's box noted lazily, from the last copy taken to the
// copy draw deciding), so it holds for every path the loop takes (the plan's groups, the volume runs, the instancing runs:
// none of them crosses a copy draw).
// MODES: TE_GFX_COPY_COALESCE unset / 1 = on; 0 = every copy taken (as before); force = (diagnosis, NOT identical) every
// copy after the frame's first skipped -- the picture oracle's negative control.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "graphics_internal.h"

namespace te::gfx {
namespace {

struct Box {
    float x0, y0, x1, y1;
};
bool Overlap(const Box& a, const Box& b) { return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1; }

// why a copy was taken (the census)
enum Why : int { kFirst = 0, kOverlap, kUnknown, kSampler, kOff, kWhyCount };
const char* const kWhyNames[kWhyCount] = {"the frame's first", "a draw since the last copy over the sampled region",
                                          "a draw of unknown extent since the last copy", "a sampler of unknown region",
                                          "off"};
constexpr size_t kMaxBoxes = 2048;   // past these, the region since the copy is the whole frame (never reached in a frame)

struct Census {
    uint64_t frames = 0, frames_with = 0, asked = 0, taken = 0, skipped = 0;
    uint64_t why[kWhyCount] = {};
    uint64_t noted = 0, tested = 0, samplers = 0, orphans = 0, us = 0;
    uint64_t clean = 0, regions = 0, region_px = 0;   // skipped as nothing was written since; region copies; pixels asked
};

struct State {
    int mode = -1;               // -1 unread, 0 off, 1 on, 2 force (diagnosis)
    bool region = true;          // TE_GFX_COPY_REGION (default on): a later copy asks for the draws' box since the last only
    ps2e_draw* device = nullptr;
    float w = 512.f, h = 512.f;  // the frame (pixels: the vertices' units)
    bool valid = false;          // a copy of this frame was taken: the texture holds the frame as it stood then
    bool all = false;            // since it: a draw of unknown extent (the region written is the whole frame)
    std::vector<Box> dirty;      // since it: the boxes of the draws that may have written colour
    size_t cursor = 0;           // the bucket's first draw not noted yet
    int left = 0;                // copy draws still to come this frame
    bool orphan_seen = false;    // a sampler first in its bucket: coalescing off from then on
    Census now, last;
} g_cs;

void ReadMode() {
    const char* v = std::getenv("TE_GFX_COPY_COALESCE");
    g_cs.mode = !v || !v[0] || v[0] == '1' ? 1 : std::strcmp(v, "force") == 0 ? 2 : 0;
    const char* r = std::getenv("TE_GFX_COPY_REGION");
    g_cs.region = !(r && r[0] == '0');
    std::fprintf(stderr, "[te.gfx] screen copies (TE_GFX_COPY_COALESCE=%s): %s\n", v ? v : "",
                 g_cs.mode == 1 ? "coalesced -- a copy draw's copy is skipped when no draw since the frame's last copy touched "
                                  "the region its draws sample (the same texels, by construction)"
                 : g_cs.mode == 2 ? "FORCED off after the frame's first copy (diagnosis: NOT picture-identical)"
                                  : "every copy draw takes its copy (as before)");
    std::fprintf(stderr, "[te.gfx] screen copies (TE_GFX_COPY_REGION=%s): %s\n", r ? r : "",
                 g_cs.region ? "a frame's later copies ask for the box of the draws since its last copy only (the texture equals "
                               "the frame outside it)" : "every copy asks for the whole frame");
    g_cs.dirty.reserve(256);
}

void DirtyAll() {
    g_cs.all = true;
    g_cs.dirty.clear();
}

// the pixels draw `d` may write (its colour / alpha): into the region since the last copy
void Note(const Draw& d) {
    const ps2e_draw_state& s = d.state;
    if (s.no_colour_write) return;   // FBMSK all set: depth / stencil only
    ++g_cs.now.noted;
    Box b;
    if (d.mesh) {
        float bx[4];
        if (d.skin_joints || !ps2e_draw_mesh_screen_box(g_cs.device, d.mesh, &d.mesh_params, bx)) {
            DirtyAll();
            return;
        }
        b = Box{bx[0], bx[1], bx[2], bx[3]};
    } else {
        const size_t n = d.VertexCount();
        if (n < 3) return;   // (the device draws whole triangles only)
        const ps2e_draw_vertex* v = d.Vertices();
        float x0 = v[0].x, y0 = v[0].y, x1 = v[0].x, y1 = v[0].y;
        bool finite = true;
        for (size_t k = 0; k < n; ++k) {
            const float x = v[k].x, y = v[k].y;
            finite = finite && std::isfinite(x) && std::isfinite(y);
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
        if (!finite) {
            DirtyAll();
            return;
        }
        b = Box{x0 - 1.f, y0 - 1.f, x1 + 1.f, y1 + 1.f};
    }
    if (b.x1 < 0.f || b.y1 < 0.f || b.x0 > g_cs.w || b.y0 > g_cs.h) return;   // wholly off the frame: writes nothing
    if (g_cs.dirty.size() >= kMaxBoxes) {
        DirtyAll();
        return;
    }
    g_cs.dirty.push_back(b);
}

void NoteRange(Draw* const* list, size_t from, size_t to) {
    for (size_t k = from; k < to && !g_cs.all; ++k) Note(*list[k]);
}

uint64_t NowUs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

}  // namespace

void CopiesBeginFrame(ps2e_draw* device, int copy_draws, int screen_w, int screen_h) {
    if (g_cs.mode < 0) ReadMode();
    g_cs.device = device;
    g_cs.w = float(std::max(1, screen_w));
    g_cs.h = float(std::max(1, screen_h));
    g_cs.valid = false;
    g_cs.all = false;
    g_cs.dirty.clear();
    g_cs.cursor = 0;
    g_cs.left = std::max(0, copy_draws);
    g_cs.now.frames++;
    if (copy_draws > 0) g_cs.now.frames_with++;
}

bool CopyWanted(Draw* const* list, size_t count, size_t i, float region[4]) {
    const uint64_t t0 = NowUs();
    Census& c = g_cs.now;
    c.asked++;
    if (g_cs.left > 0) g_cs.left--;
    int why = -1;
    if (g_cs.mode == 0 || g_cs.orphan_seen) {
        why = kOff;
    } else {
        NoteRange(list, g_cs.cursor, i);
        if (!g_cs.valid) why = kFirst;
        else if (g_cs.all) why = kUnknown;
    }
    g_cs.cursor = i;   // the copy draw itself is drawn after this: noted at the next decision
    // nothing written since the last copy: the texture holds the frame wherever it is sampled
    const bool clean = why < 0 && g_cs.dirty.empty();
    if (clean) c.clean++;
    if (why < 0 && !clean) {
        // the region the draws sampling this copy read: this draw's and the bucket's next ones' up to its next copy draw
        Box s{1e30f, 1e30f, -1e30f, -1e30f};
        bool known = true;
        for (size_t k = i; k < count && known; ++k) {
            const Draw& e = *list[k];
            if (k > i && e.copy_frame) break;
            if (e.state.texture != PS2E_DRAW_TEXTURE_FRAME_COPY) continue;
            c.samplers++;
            if (e.mesh) {
                known = false;
                break;
            }
            const ps2e_draw_vertex* v = e.Vertices();
            for (size_t n = e.VertexCount() / 3 * 3, j = 0; j < n; ++j) {
                const float u = v[j].u, t = v[j].v;
                if (!std::isfinite(u) || !std::isfinite(t)) {
                    known = false;
                    break;
                }
                s.x0 = std::min(s.x0, u);
                s.y0 = std::min(s.y0, t);
                s.x1 = std::max(s.x1, u);
                s.y1 = std::max(s.y1, t);
            }
        }
        if (!known) {
            why = kSampler;
        } else if (s.x1 >= s.x0) {
            // the CLAMP wrap: the fractions sampled lie in [0, 1]; in frame pixels, padded 2 (the bilinear footprint)
            const auto px = [](float f, float size) { return std::min(1.f, std::max(0.f, f)) * size; };
            const Box sp{px(s.x0, g_cs.w) - 2.f, px(s.y0, g_cs.h) - 2.f, px(s.x1, g_cs.w) + 2.f, px(s.y1, g_cs.h) + 2.f};
            for (const Box& b : g_cs.dirty) {
                c.tested++;
                if (Overlap(b, sp)) {
                    why = kOverlap;
                    break;
                }
            }
        }
        // (no sampler drawing anything: the copy is read by nobody -- skipped like a clean one)
    }
    if (g_cs.mode == 2 && g_cs.valid) why = -1;   // diagnosis: every copy after the frame's first skipped
    const bool take = why >= 0;
    // THE COPY ASKED (TE_GFX_COPY_REGION, default on): the frame's first copy whole; a later one only the box of the
    // draws since the last copy -- the texture equals the frame outside them (it did at that copy, and nothing else was
    // written since), so refreshing them makes it equal to the frame everywhere again: the same texels as a whole copy
    region[0] = region[1] = 1.f;
    region[2] = region[3] = 0.f;   // (x0 > x1: the whole frame)
    if (take) {
        if (why != kFirst && why != kOff && !g_cs.all && !g_cs.dirty.empty() && g_cs.region) {
            Box u = g_cs.dirty[0];
            for (const Box& b : g_cs.dirty) u = Box{std::min(u.x0, b.x0), std::min(u.y0, b.y0), std::max(u.x1, b.x1), std::max(u.y1, b.y1)};
            region[0] = std::max(0.f, u.x0);
            region[1] = std::max(0.f, u.y0);
            region[2] = std::min(g_cs.w, u.x1);
            region[3] = std::min(g_cs.h, u.y1);
            c.region_px += uint64_t(std::max(0.f, region[2] - region[0]) * std::max(0.f, region[3] - region[1]));
            c.regions++;
        } else {
            c.region_px += uint64_t(g_cs.w * g_cs.h);
        }
        c.taken++;
        c.why[why]++;
        g_cs.valid = true;
        g_cs.all = false;
        g_cs.dirty.clear();
    } else {
        c.skipped++;
    }
    c.us += NowUs() - t0;
    return take;
}

void CopiesBucketEnd(Draw* const* list, size_t count) {
    // the orphan check (a tripwire: the Emitter records a part's samplers after its copy draw, in its bucket): a frame-copy
    // sampler before its bucket's first copy draw would sample an earlier bucket's copy -- looked for once the frame holds a
    // copy (before that a sampler is refused by the device, as always)
    if (!g_cs.orphan_seen && g_cs.mode != 0 && g_cs.valid) {
        for (size_t k = 0; k < count; ++k) {
            const Draw& e = *list[k];
            if (e.copy_frame) break;
            if (e.state.texture == PS2E_DRAW_TEXTURE_FRAME_COPY) {
                g_cs.now.orphans++;
                g_cs.orphan_seen = true;
                std::fprintf(stderr, "[te.gfx] screen copies: a frame-copy sampler is the first of its bucket (its copy is an "
                             "earlier bucket's): the coalescing is OFF from now on\n");
                break;
            }
        }
    }
    if (g_cs.mode != 0 && g_cs.valid && g_cs.left > 0 && !g_cs.all) {
        const uint64_t t0 = NowUs();
        NoteRange(list, g_cs.cursor, count);
        g_cs.now.us += NowUs() - t0;
    }
    g_cs.cursor = 0;
}

void CopiesDirtyAll() {
    if (g_cs.valid) DirtyAll();
}

std::string CopyCensusLine(uint64_t first_present, uint64_t last_present) {
    Census& n = g_cs.now;
    Census& l = g_cs.last;
    const double frames = double(n.frames - l.frames);
    std::string out;
    if (n.asked != l.asked && frames > 0) {
        char buf[1024];
        const double f = frames;
        std::snprintf(buf, sizeof buf,
                      "[te.gfx] screen copies per frame (presents %llu-%llu; TE_GFX_COPY_COALESCE %s): copy draws %.2f, copies "
                      "TAKEN %.2f (%s %.2f, %s %.2f, %s %.2f, %s %.2f, %s %.2f), coalesced %.2f (%.2f with nothing written since "
                      "the last copy); asked as the draws' box since the last copy %.2f, pixels asked %.1f K a frame; %.0f%% of "
                      "the frames with copy draws; draws noted %.1f, boxes tested %.1f, samplers %.2f, orphan samplers %llu; "
                      "%.1f us a frame\n",
                      (unsigned long long)first_present, (unsigned long long)last_present,
                      g_cs.mode == 1 ? (g_cs.orphan_seen ? "on (OFF: an orphan sampler)" : "on") : g_cs.mode == 2 ? "force" : "off",
                      double(n.asked - l.asked) / f, double(n.taken - l.taken) / f, kWhyNames[kFirst],
                      double(n.why[kFirst] - l.why[kFirst]) / f, kWhyNames[kOverlap], double(n.why[kOverlap] - l.why[kOverlap]) / f,
                      kWhyNames[kUnknown], double(n.why[kUnknown] - l.why[kUnknown]) / f, kWhyNames[kSampler],
                      double(n.why[kSampler] - l.why[kSampler]) / f, kWhyNames[kOff], double(n.why[kOff] - l.why[kOff]) / f,
                      double(n.skipped - l.skipped) / f, double(n.clean - l.clean) / f, double(n.regions - l.regions) / f,
                      double(n.region_px - l.region_px) / f / 1000.0, 100.0 * double(n.frames_with - l.frames_with) / f,
                      double(n.noted - l.noted) / f, double(n.tested - l.tested) / f, double(n.samplers - l.samplers) / f,
                      (unsigned long long)n.orphans, double(n.us - l.us) / f);
        out = buf;
    }
    l = n;
    return out;
}

}  // namespace te::gfx
