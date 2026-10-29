/* engine/platform/ps2e/ps2e_draw.h -- ps2e_draw: the typed-draw surface a NON-recompiled producer (a decompiled
 * game's native platform arm over this engine) draws a frame through.
 *
 * The engine's owned graphics path consumes DrawIr records (engine/gs/hle/ir/draw_ir.h) produced by the recompiled
 * title's HLE producers over a GS VRAM model. A decomp has no VRAM, no GIF packets and no DMA chains: it knows a frame
 * of W x H pixels, textures as RGBA8 images, and draws as lists of vertices with a small render state. This header is
 * that surface, in C so a foreign toolchain (wasm2c output, a C runner) can call it without the engine's C++ types:
 *
 *   device  = ps2e_draw_create(&config)          one SDL window + the render backend the build's arm supplies + one frame
 *                                                target (PS2R_PS2E_DRAW_BACKEND: gl = a GL 4.1 context + GlDevice; null)
 *   tex     = ps2e_draw_texture_create(...)      RGBA8, top row first, PS2 alpha bytes (0x80 = 1.0)
 *   ps2e_draw_begin_frame(device, &clear)        the frame target cleared (colour and/or depth) or kept
 *   ps2e_draw_submit(device, &state, verts, n)   one draw: GS PRIM/TEST/ALPHA-valued state over frame-pixel vertices
 *   ps2e_draw_present(device)                    the frame target scanned out to the window and swapped
 *
 * Value contract: the state's numbers are the PS2's (the GS User's Manual's PRIM, TEST.ZTST/ATST/AFAIL, ALPHA.A/B/C/D/
 * FIX, TEX0.TFX/TCC, TEX1.MMAG, CLAMP.WMS/WMT) because the game's materials carry exactly those values and the backend
 * (engine/render/backend.h) implements their exact semantics; this header does NOT carry any VRAM address, packet
 * word or register bit layout. Vertex x/y are frame pixels (0,0 the frame's top-left corner; 1/16 fractions are kept),
 * z is the GS depth (0 .. 2^24-1 for a Z24 buffer; bigger values are clamped by the backend's mapping), u/v are
 * fractions of the texture (0..1, wrapping per the state; times the vertex's q when one is given -- the GS's S, T and Q,
 * interpolated linearly and divided per pixel: perspective-correct), colours are RGBA bytes with the PS2 alpha scale.
 * Every draw is a TRIANGLE LIST: the producer expands its strips and sprites (so the vertex order is the triangle
 * order the GS would have rasterised, and the backend's bounding-box and coverage rules apply unchanged).
 *
 * Threading: one thread owns the device from create to destroy (the GL arm's context is current on it). Headless devices
 * (config.headless) draw into a hidden window; presentation then only feeds the capture (ps2e_draw_read_presented),
 * which is how a judge measures a frame without a display.
 */
#ifndef PS2E_DRAW_H
#define PS2E_DRAW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2e_draw ps2e_draw;

typedef struct ps2e_draw_config {
    int frame_width;     /* the logical frame (render target) in pixels, e.g. 512 x 448 (NTSC) / 512 x 512 (PAL) */
    int frame_height;
    int window_width;    /* the window's client size; 0 = the frame's size */
    int window_height;
    int headless;        /* 1 = hidden window (judges, CI); presentation still renders for capture */
    const char* title;   /* window title; NULL = "ps2e_draw" */
    int stencil_diag;    /* 1 = log the stencil's histogram at the shadow bracket's clear and composite ([ps2e.stencil] lines) */
    int depth_bits;      /* the producer's GS Z range: 24 = Z in [0, 2^24) (a screen matrix mapping the clip volume onto
                          * 0..2^24), mapped onto the depth attachment exactly (one step per Z unit, as the GS's Z32 test
                          * compares; a Z beyond 2^24 - 1 held there); 0 / 32 = Z32 (window depth z / 2^32: 128- or
                          * 256-unit steps) */
} ps2e_draw_config;

/* Creates the window, the arm's backend (the GL arm: its GL 4.1 core context + device) and the frame target. NULL when
 * the arm cannot be made (the GL arm on a host without GL). */
ps2e_draw* ps2e_draw_create(const ps2e_draw_config* config);
void ps2e_draw_destroy(ps2e_draw* device);

/* Processes the window's events. Returns 1 when the user asked the window to close, else 0. The window's thread
 * (the one that created the device) calls this; it notes the drawable size the present uses. */
int ps2e_draw_pump(ps2e_draw* device);
/* Threads. The GL context is current on the creating thread after ps2e_draw_create. A producer that draws from another
 * thread (a render thread: submit + present off the game's thread) first releases it on the creating thread
 * (bind = 0) and then binds it on the drawing thread (bind = 1); every call but ps2e_draw_pump then belongs to the
 * drawing thread (texture create/upload/destroy, resize, submit, present, the presented-image reads). Returns 1 when
 * the context changed hands, 0 when the device has no context (headless without GL) or the call failed. */
int ps2e_draw_bind_thread(ps2e_draw* device, int bind);
#define PS2E_DRAW_HAS_BIND_THREAD 1   /* for consumers pinned across engine versions */
/* The frame target re-made at a new logical size (the title's display height may change: 448/512). */
int ps2e_draw_resize_frame(ps2e_draw* device, int frame_width, int frame_height);
void ps2e_draw_frame_size(const ps2e_draw* device, int* frame_width, int* frame_height);

/* ---- textures ------------------------------------------------------------------------------------------------ */
/* An RGBA8 texture, `width * height * 4` bytes, top row first, PS2 alpha bytes (0x80 = fully opaque when modulated;
 * the device applies the ONE GPU alpha scaling). NULL pixels = an uninitialised texture to be filled by
 * ps2e_draw_texture_upload. Returns 0 on failure. */
uint32_t ps2e_draw_texture_create(ps2e_draw* device, int width, int height, const void* rgba8);
int ps2e_draw_texture_upload(ps2e_draw* device, uint32_t texture, int x, int y, int width, int height,
                             const void* rgba8, size_t stride_bytes);
/* The same with GPU alpha bytes already (0xFF opaque): what the engine's TextureDecoder produces. */
uint32_t ps2e_draw_texture_create_gpu_alpha(ps2e_draw* device, int width, int height, const void* rgba8);
void ps2e_draw_texture_destroy(ps2e_draw* device, uint32_t texture);
/* A texture's levels below the first, as the PS2 kept them (MIPTBP1/2): `count` images of halving size (at least 1x1),
 * each `max(width >> i, 1) * max(height >> i, 1) * 4` bytes with the device's GPU alpha (as
 * ps2e_draw_texture_create_gpu_alpha takes them). Replaces any levels the texture had. Returns 1 on success. */
int ps2e_draw_texture_set_mips(ps2e_draw* device, uint32_t texture, int count, const void* const* levels);

/* RESERVED TEXTURES (a producer that decodes its textures on worker threads and has them made later, between frames or at
 * their first draw): ps2e_draw_texture_reserve hands out, from ANY thread, the id the next ps2e_draw_texture_create would
 * have taken -- ids are taken in the order the calls are made, so a producer that reserves where it used to create gets the
 * very same ids. The texture itself is made on the drawing thread by ps2e_draw_texture_create_reserved_gpu_alpha (as
 * ps2e_draw_texture_create_gpu_alpha makes one; levels by ps2e_draw_texture_set_mips). A draw or a query that names an id
 * the device has no texture for calls the producer's resolver (ps2e_draw_set_texture_resolver) on the drawing thread first:
 * it returns 1 when it made the texture there and then (the draw then finds it), 0 when there is none to make (the draw is
 * refused, as for any texture id that does not exist). Destroying a reserved id that was never made is a no-op. */
#define PS2E_DRAW_HAS_TEXTURE_RESERVE 1
uint32_t ps2e_draw_texture_reserve(ps2e_draw* device);
int ps2e_draw_texture_create_reserved_gpu_alpha(ps2e_draw* device, uint32_t texture, int width, int height, const void* rgba8);
typedef int (*ps2e_draw_texture_resolver)(void* user, uint32_t texture);
void ps2e_draw_set_texture_resolver(ps2e_draw* device, ps2e_draw_texture_resolver resolve, void* user);

/* ---- movie pictures ------------------------------------------------------------------------------------------ */
/* A MOVIE texture (the Vita's movie path; a device without the backend's movie entries returns 0 and the caller keeps an
 * RGBA texture of ps2e_draw_texture_create): `slots` sets of storage for one decoded picture of width x height each --
 * PS2E_DRAW_MOVIE_YUV420: three 8-bit planes (Y width x height, U and V (width + 1) / 2 x (height + 1) / 2, limited-range
 * BT.601 as an MPEG-2 decoder leaves them), PS2E_DRAW_MOVIE_RGBA: one RGBA8 image (alpha 0xFF). ps2e_draw_movie_plane hands
 * out a plane's CPU address and row stride, so a decoder writes its picture straight into the GPU's storage (from any
 * thread: the caller keeps it from writing a slot while a frame the GPU has not finished samples it); the id is a texture
 * of ps2e_draw_state: a draw with it shows the slot ps2e_draw_movie_show made current, drawn by the device's own movie
 * program over the draw's rectangle (its vertices' box and u / v extent: the movie quad), no blending, no depth test --
 * the picture as it is, whatever else the state says (texture filter: the state's / the Display page's). Destroyed with
 * ps2e_draw_texture_destroy. Create, show and destroy belong to the drawing thread, as every call. */
#define PS2E_DRAW_MOVIE_RGBA 0
#define PS2E_DRAW_MOVIE_YUV420 1
#define PS2E_DRAW_HAS_MOVIE 1
uint32_t ps2e_draw_movie_create(ps2e_draw* device, int width, int height, int format, int slots);
/* plane 0..2 (Y, U, V; RGBA: 0 only) of `slot`: its storage's CPU address and row stride in bytes (NULL when out of range) */
void* ps2e_draw_movie_plane(ps2e_draw* device, uint32_t movie, int slot, int plane, size_t* stride);
/* The slot the movie texture's draws show from now on (-1: none -- a draw then draws nothing) */
void ps2e_draw_movie_show(ps2e_draw* device, uint32_t movie, int slot);
/* The movie texture's draws made so far, and those that drew nothing (no slot shown, or the device refused) */
void ps2e_draw_movie_counts(const ps2e_draw* device, uint64_t* drawn, uint64_t* refused);

/* ---- a frame ------------------------------------------------------------------------------------------------- */
typedef struct ps2e_draw_clear {
    uint8_t red, green, blue;   /* the clear colour (alpha written 0x80) */
    int clear_colour;           /* 1 = clear the colour buffer to the colour; 0 = keep the previous frame's pixels */
    int clear_depth;            /* 1 = clear the depth buffer to the GS far plane (0) */
} ps2e_draw_clear;
void ps2e_draw_begin_frame(ps2e_draw* device, const ps2e_draw_clear* clear);

typedef struct ps2e_draw_vertex {
    float x, y;                 /* frame pixels from the frame's top-left corner (fractions kept: the GS's 1/16) */
    float z;                    /* GS depth */
    float u, v;                 /* fractions of the texture, times q when q is given (the GS's S and T) */
    uint8_t r, g, b, a;         /* RGBA bytes, PS2 alpha scale */
    float q;                    /* the GS's Q (1/w): u and v are S = u'.q and T = v'.q; 0 = 1 (u and v as they are). The
                                   backend interpolates S, T and Q linearly over the screen and divides per pixel as the GS
                                   does, so a producer that gives its vertices' q gets perspective-correct texturing; one that
                                   leaves it 0 gets the affine mapping (every triangle's texture flat in screen space) */
} ps2e_draw_vertex;
/* The vertex carries q (perspective-correct texturing). A producer built against an older header leaves it 0 (affine). */
#define PS2E_DRAW_HAS_VERTEX_Q 1

/* GS numbering throughout (the game's shader settings ARE these values). */
enum ps2e_draw_depth_test { PS2E_DRAW_DEPTH_NEVER = 0, PS2E_DRAW_DEPTH_ALWAYS = 1, PS2E_DRAW_DEPTH_GEQUAL = 2,
                            PS2E_DRAW_DEPTH_GREATER = 3 };
enum ps2e_draw_alpha_test { PS2E_DRAW_ATST_NEVER = 0, PS2E_DRAW_ATST_ALWAYS = 1, PS2E_DRAW_ATST_LESS = 2,
                            PS2E_DRAW_ATST_LEQUAL = 3, PS2E_DRAW_ATST_EQUAL = 4, PS2E_DRAW_ATST_GEQUAL = 5,
                            PS2E_DRAW_ATST_GREATER = 6, PS2E_DRAW_ATST_NOTEQUAL = 7 };
enum ps2e_draw_wrap { PS2E_DRAW_WRAP_REPEAT = 0, PS2E_DRAW_WRAP_CLAMP = 1 };
enum ps2e_draw_tfx { PS2E_DRAW_TFX_MODULATE = 0, PS2E_DRAW_TFX_DECAL = 1, PS2E_DRAW_TFX_HIGHLIGHT = 2,
                     PS2E_DRAW_TFX_HIGHLIGHT2 = 3 };

typedef struct ps2e_draw_state {
    uint32_t texture;           /* 0 = untextured (vertex colours only) */
    int gouraud;                /* 1 = per-vertex colour interpolation (PRIM.IIP), 0 = flat (the last vertex's) */
    int depth_test;             /* ps2e_draw_depth_test (TEST.ZTST; ZTE on) */
    int depth_write;            /* 0 = ZBUF.ZMSK */
    int blend;                  /* PRIM.ABE */
    uint8_t blend_a, blend_b, blend_c, blend_d, blend_fix;   /* ALPHA: (A - B) * C + D over {Cs, Cd, 0} / {As, Ad, FIX} */
    int alpha_test;             /* TEST.ATE */
    int alpha_method;           /* ps2e_draw_alpha_test (TEST.ATST) */
    uint8_t alpha_reference;    /* TEST.AREF */
    int alpha_fail;             /* TEST.AFAIL: 0 KEEP, 1 FB_ONLY, 2 ZB_ONLY, 3 RGB_ONLY */
    int linear;                 /* TEX1.MMAG/MMIN linear (1) or nearest (0) */
    int wrap_u, wrap_v;         /* ps2e_draw_wrap (CLAMP.WMS/WMT) */
    int tfx;                    /* ps2e_draw_tfx (TEX0.TFX) */
    int tcc;                    /* TEX0.TCC: 1 = the texture's alpha is used */
    int scissor_x0, scissor_y0, scissor_x1, scissor_y1;   /* inclusive frame pixels; all zero = the whole frame */
    int no_colour_write;        /* 1 = FRAME.FBMSK all set: the draw writes depth only (a depth clear) */
    int stencil;                /* ps2e_draw_stencil: the shadow-volume counter (0 = off) */
    int fba;                    /* FRAME alpha forced: the written alpha's top bit set (an unblended draw; see below) */
    int dest_alpha_test;        /* on a COUNT_UP / COUNT_DOWN draw: TEST.DATE with DATM 0 against the frame's alpha at the clear */
    /* TEX1's mipmapping over the texture's levels (ps2e_draw_texture_set_mips); all zero = level 0 only */
    int mip_max_level;          /* TEX1.MXL: the deepest level the draw may use (0 = no mipmapping) */
    int mip_k;                  /* TEX1.K, signed 7.4 fixed point */
    int mip_l;                  /* TEX1.L */
    int mip_minify;             /* TEX1.MMIN with mipmaps: 2..5 (the GS numbering) */
} ps2e_draw_state;

/* The draw state carries TEX1's mipmapping and the textures their levels (ps2e_draw_texture_set_mips). */
#define PS2E_DRAW_HAS_MIPS 1

/* The casters' exclusion (shadows.cpp StartShadows). The GS shrank the frame's alpha into the half-size buffer
 * and counted the volume strips only where that alpha's top bit was 0 (DATE, DATM 0): the lit world writes alpha 0x7E
 * and receives the shadows, a skin writes 0x80 (or a material writes with FBA) and is never darkened -- a caster's own
 * volume would otherwise darken half his body. Here the counter's CLEAR also marks every frame pixel whose alpha is
 * the PS2 0x80 or more (the backend's GPU-scaled 0xFF) in a stencil bit above the counter, and a counting draw with
 * dest_alpha_test skips those pixels. fba is honoured on unblended draws (a blended draw keeps the fixed-function blend
 * and writes its source alpha). The frame's clear writes the top bit set (the undrawn background is never a receiver). */
#define PS2E_DRAW_HAS_FBA_DATE 1

/* The shadow volumes' counter (TR shadows.cpp StartShadows / the type-0B volume strips / ApplyShadows, as the GS did it in
 * a half-size accumulator: the counter byte starts at 15, every volume strip adds +1 (its +8 winding colour) or -1 (248)
 * where it passes the depth test against the scene, and the composite darkens where the count is 1..16). Here the frame's
 * stencil is the accumulator at the frame's resolution: CLEAR writes the origin (a submit with this value draws nothing),
 * COUNT_UP / COUNT_DOWN wrap the low 5 bits per fragment FAILING the depth test -- the GS tested the volume's negated z
 * against the scene depth's inverted copy, so a face counts where it lies at or behind the scene surface (a volume sets
 * depth_test GEQUAL, no_colour_write and depth_write 0; its z is the ordinary GS depth) -- and INSIDE draws only where the
 * count is 1..16 (the composite quad).
 * PS2E_DRAW_HAS_STENCIL lets a producer built against an older device keep its volumes as stamps. */
#define PS2E_DRAW_HAS_STENCIL 1
enum ps2e_draw_stencil { PS2E_DRAW_STENCIL_OFF = 0, PS2E_DRAW_STENCIL_COUNT_UP = 1, PS2E_DRAW_STENCIL_COUNT_DOWN = 2,
                         PS2E_DRAW_STENCIL_INSIDE = 3, PS2E_DRAW_STENCIL_CLEAR = 4 };

/* One draw of `count` vertices (a multiple of 3: a triangle list). Returns the number of triangles rasterised, or a
 * negative value when the state or the vertices were refused (nothing drawn). */
int ps2e_draw_submit(ps2e_draw* device, const ps2e_draw_state* state, const ps2e_draw_vertex* vertices, int count);

/* NATIVE MESHES: a model's triangles kept on the device and drawn through a matrix, the transform to the GS's 12.4 screen,
 * the divide and the lighting done per vertex by the device (the CPU builds nothing per frame). ps2e_draw_mesh_create
 * takes a triangle list and returns its handle, or 0 when the backend has no native transform (the producer then builds
 * the triangles itself and submits them). A mesh is destroyed only once no submitted frame still draws it. */
#define PS2E_DRAW_HAS_MESH 1
typedef struct ps2e_draw_mesh_vertex {
    float x, y, z;              /* the model's space */
    float s, t, q;              /* the vertex's STQ as the model has it (a draw's UV scroll is added before the divide) */
    uint8_t r, g, b, a;         /* the colour an unlit draw shows */
    uint8_t fog, pad[3];        /* 0 */
    float nx, ny, nz;           /* the normal in the model's space (0 without one: a lit draw then shows the ambient) */
    uint8_t lr, lg, lb, la;     /* the colour a lit draw lights */
} ps2e_draw_mesh_vertex;        /* 48 bytes */
typedef struct ps2e_draw_mesh_params {
    float screen[16];           /* model -> GS screen (x / y in 12.4 fixed pixels, depth), the title's row-vector
                                   convention: rows 0..3, position = row3 + x.row0 + y.row1 + z.row2 */
    float offset[2];            /* the GS origin taken off after the divide (frame pixels) */
    float scroll[2];            /* added to S and T before the divide */
    int lit;                    /* colour = lit colour x (ambient + sum colour_i max(0, normal . dir_i)), clamped */
    float light_dir[3][3];
    float light_colour[3][3];
    float ambient[3];
    float box[4];               /* the draw's screen box (frame pixels x0, y0, x1, y1): the area a blend reading the
                                   destination copies */
    int first, count;           /* a sub-range of the mesh's triangle-list vertices (whole triangles); count 0 = the
                                   whole mesh (the Vita's per-chunk draws of a clipped instance) */
} ps2e_draw_mesh_params;
uint32_t ps2e_draw_mesh_create(ps2e_draw* device, const ps2e_draw_mesh_vertex* vertices, int count);
void ps2e_draw_mesh_destroy(ps2e_draw* device, uint32_t mesh);
/* A PREPARED native mesh: everything ps2e_draw_mesh_create computes from the triangle list before it makes the mesh,
 * computed ahead -- from ANY thread, without a device (ps2e_draw_mesh_prepare) -- so that making it on the drawing thread
 * (ps2e_draw_mesh_create_prepared) is only the GPU's buffers: the distinct vertices and the 16-bit indices the Vita's device
 * draws an indexed mesh from (ps2e_draw_mesh_create's deduplication, the same vertices in the same order), and the bounds it
 * keeps. ps2e_draw_mesh_create_prepared(device, prepare(list)) makes the same mesh as ps2e_draw_mesh_create(device, list)
 * (the same buffers, the same bounds, the same id). The prepared form owns its storage (ps2e_draw_mesh_prepared_free).
 * Off the Vita, and when the device indexes no meshes, the prepared form keeps the list and creating from it is
 * ps2e_draw_mesh_create. */
#define PS2E_DRAW_HAS_MESH_PREPARE 1
typedef struct ps2e_draw_mesh_prepared ps2e_draw_mesh_prepared;
ps2e_draw_mesh_prepared* ps2e_draw_mesh_prepare(const ps2e_draw_mesh_vertex* vertices, int count);
uint32_t ps2e_draw_mesh_create_prepared(ps2e_draw* device, const ps2e_draw_mesh_prepared* prepared);
void ps2e_draw_mesh_prepared_free(ps2e_draw_mesh_prepared* prepared);
/* the triangle-list vertices it was made from (whole triangles), and the bytes it holds */
int ps2e_draw_mesh_prepared_count(const ps2e_draw_mesh_prepared* prepared);
size_t ps2e_draw_mesh_prepared_bytes(const ps2e_draw_mesh_prepared* prepared);
/* One draw of a mesh under `state` (its texture, blend, tests; never the stencil clear). Returns > 0 drawn, < 0 refused. */
int ps2e_draw_submit_mesh(ps2e_draw* device, const ps2e_draw_state* state, uint32_t mesh, const ps2e_draw_mesh_params* params);
/* `n` consecutive draws of one mesh under one `state` -- the same as ps2e_draw_submit_mesh(device, state, mesh,
 * params[i]) for i = 0..n-1 in order, every params[i] with the same first / count, the same offset / scroll and lit 0 (the
 * caller's run) -- which the Vita's device may draw as one instanced draw (the run's draws of one packet: their matrices on
 * an instance stream). Returns the number of them refused (0: all drawn). */
int ps2e_draw_submit_mesh_instances(ps2e_draw* device, const ps2e_draw_state* state, uint32_t mesh,
                                    const ps2e_draw_mesh_params* const* params, int n);
/* 1 when the device draws such runs as instanced draws (the Vita's GL backend with its instanced twins and the
 * packets on), else 0: a producer then has no reason to look for runs. */
int ps2e_draw_mesh_instancing(const ps2e_draw* device);
/* The screen box (frame pixels x0, y0, x1, y1, padded for the rasteriser's rounding) a draw of `mesh` through `params`
 * can touch: its model-space bounds' eight corners through the matrix as the native vertex program takes them. Returns 0
 * when unknown (no bounds kept: not the Vita, or a corner at or behind the eye -- the box is then the whole plane). */
int ps2e_draw_mesh_screen_box(ps2e_draw* device, uint32_t mesh, const ps2e_draw_mesh_params* params, float box[4]);
/* (the Vita) The model-space box (lo / hi corners) of `mesh`'s vertices as kept at its creation (the bounds every
 * box and depth range above is made from). Returns 0 when none is kept or the mesh is a skin mesh (elsewhere 0). */
int ps2e_draw_mesh_bounds(ps2e_draw* device, uint32_t mesh, float lo[3], float hi[3]);
/* (the Vita) 1 when a mesh draw of `mesh` under `state` with `params` would be KEPT as one of the frame's shadow
 * casters were it submitted now (the casters' re-stamp keeps the frame's first casters only: an order-dependent record),
 * 0 when it would not be a caster (or none are recorded); elsewhere 0. Asks only: nothing recorded. */
int ps2e_draw_mesh_caster_candidate(ps2e_draw* device, const ps2e_draw_state* state, uint32_t mesh, const ps2e_draw_mesh_params* params);
/* (the Vita) The casters the frame may still keep (the re-stamp's room) */
int ps2e_draw_caster_room(const ps2e_draw* device);
/* (the Vita) 1 when a draw under `state` may record a shadow caster (its vertices' alpha not looked at) */
int ps2e_draw_state_caster(ps2e_draw* device, const ps2e_draw_state* state);
/* (the Vita) A lower bound of the GS z (z / w through params->screen) every vertex of a SKIN draw of `mesh` lands at with
 * `bones` (`joints` x 3 rows of 4: ps2e_draw_skin_mesh_draw's palette): the smallest over every joint's image of the mesh's
 * bind-pose box -- exact as a bound when the mesh's weights are convex (checked at its creation). 0 = not known (not a
 * skin mesh, weights not convex, a corner at or behind the eye); elsewhere 0. */
int ps2e_draw_skin_depth_far(ps2e_draw* device, uint32_t mesh, const ps2e_draw_mesh_params* params, const float* bones, int joints,
                             float* z_far);
/* The GS depth range (z_near = the largest GS z: nearest under GEQUAL / GREATER, z_far the smallest) of the same eight
 * corners (the Vita's draw order, platform/nativeee/host/graphics_zorder.cpp). Returns 0 when unknown (no bounds kept,
 * a skin mesh, or a corner at or behind the eye). */
int ps2e_draw_mesh_depth_range(ps2e_draw* device, uint32_t mesh, const ps2e_draw_mesh_params* params, float* z_near, float* z_far);
/* ps2e_draw_mesh_depth_range's answer to one question, cheaper: 1 when the mesh's bounds through `params` are known
 * and every corner's depth is at least z_min (z_far >= z_min), 0 when known and not, -1 when not known (as depth_range's 0).
 * The Vita only (-1 elsewhere). */
int ps2e_draw_mesh_depth_far_above(ps2e_draw* device, uint32_t mesh, const ps2e_draw_mesh_params* params, float z_min);
/* A lower bound of the PS2 alpha (0..255, before the alpha test) every fragment of a draw under `state` computes -- of
 * `mesh` (lit: its lit colours' alpha) or of the `count` vertices `v` -- from the smallest vertex alpha and the texture's
 * smallest texel alpha as the Vita's programs combine them (MODULATE; the untextured vertex colour); -1 when not known
 * (another TFX, a screen copy, a skin, a texture handed over in GPU alpha). The Vita's alpha-test fold
 * (platform/nativeee/host/graphics_zorder.cpp): a test that no such fragment can fail is no test. Elsewhere -1. */
int ps2e_draw_alpha_floor(ps2e_draw* device, const ps2e_draw_state* state, uint32_t mesh, int lit, const ps2e_draw_vertex* v, int count);
/* GPU-SKINNED MESHES (the Vita; elsewhere create returns 0 and submit refuses): a skinned model part's static vertices
 * and its triangles (16-bit indices into them, in the order the GS would rasterise them) kept on the device, and drawn
 * through a bone PALETTE: the device skins, transforms, divides and lights every vertex as the title's skinned lit strip
 * program does (position = sum of weight x (joint's 3x4 . bind position), the normal through the first joint's axes,
 * colour lane = uint8(int32(min(lit x lane, 255))), the alpha the vertex's own). `max_alpha`: the vertices' largest alpha
 * byte (the casters' re-stamp). A skin mesh is destroyed with ps2e_draw_mesh_destroy. */
#define PS2E_DRAW_HAS_SKIN 1
typedef struct ps2e_draw_skin_vertex {
    float x, y, z;              /* the bind-pose position (the batch's ints times its scale) */
    float s, t, one;            /* S, T (scaled) and 1 */
    float r, g, b, a;           /* the colour's lanes as the program reads them (sign-extended ints), the alpha byte */
    float nx, ny, nz;           /* the bind-pose normal (scaled) */
    float w0, w1, w2;           /* the three bones' weights (0 for an unused one) */
    uint8_t j0, j1, j2, j3;     /* their joints in the draw's palette */
    float index;                /* a blend skin part's vertex: its place in the draw's displacements (0 in a plain skin) */
} ps2e_draw_skin_vertex;        /* 72 bytes */
uint32_t ps2e_draw_skin_mesh_create(ps2e_draw* device, const ps2e_draw_skin_vertex* vertices, int count,
                                    const uint16_t* indices, int index_count, int max_alpha);
/* One draw of a skin mesh under `state` through `params` (the skin's matrix to the screen, the GS origin, the UV scroll,
 * the lights: `lit` is read as 1) and `bones`: `joints` (at most 64) x 3 rows of 4 floats, each joint's 3x4 transposed
 * (row k = the k-th lanes of its three axes and its translation); a blend skin's part also `disp`: `disp_count` (at most
 * 64) x 4 floats, each vertex's displacement of its bind position (xyz; null / 0 for a plain skin). Both must stay valid
 * until the frame is presented (the casters' re-stamp draws the skin again at the shadows' composite). Returns > 0 drawn,
 * < 0 refused. */
int ps2e_draw_submit_skin(ps2e_draw* device, const ps2e_draw_state* state, uint32_t mesh, const ps2e_draw_mesh_params* params,
                          const float* bones, int joints, const float* disp, int disp_count);
/* A mesh draw right after another whose state differs in the texture only takes the device's short path (the texture's
 * state, the transform, the draw; the picture is the full draw's). 2 (the default): the alpha test and the mipmapping may
 * differ too (the short path also sets the register uniforms); 1 = the texture only; 0 = every such draw takes the full path. */
void ps2e_draw_set_mesh_texture_swap(ps2e_draw* device, int on);
/* The Vita's precomputed mesh draw PACKETS (TE_GFX_VITA_PACKETS=0: off): an again / texture-swap mesh draw is drawn from
 * the packet the backend recorded when its state last drew (render::Backend::DrawMeshPacket). This census, since create:
 * `missing` = such draws that found no packet kept for their state (they took the slow paths); `repeats` = packet draws
 * of the same packet and the same mesh range as the draw just before them (only their transform differs: what one
 * instanced draw could take without any reordering). Elsewhere 0. */
void ps2e_draw_mesh_packet_census(const ps2e_draw* device, uint64_t* missing, uint64_t* repeats);
/* The skin draws of the mesh run's context that had no skin packet kept for their state (the Vita; 0 elsewhere). */
void ps2e_draw_skin_packet_census(const ps2e_draw* device, uint64_t* missing);
/* The Vita's census of the mesh draws that took the full path, since create, by why (`reasons`, 5 entries): [0] no run (the
 * frame's first, after a screen copy / a present / a skin draw / a shadow composite), [1] another draw came between it and
 * the run's last mesh draw, [2] another context (a state field beyond the texture / alpha test / mipmapping), [3] the
 * run's state but for a texture swap the short path does not take (untextured on one side, the frame copy), [4] the device
 * declined the again / texture-swap path; `casters` = mesh draws replayed by the casters' re-stamp at a shadow composite
 * (each a full draw of its own). Elsewhere 0. */
void ps2e_draw_mesh_full_census(const ps2e_draw* device, uint64_t reasons[5], uint64_t* casters);
/* The lookups of the draws' records through their direct-mapped slots, since create: [0] mesh bounds looked up,
 * [1] of them past the slots (the map), [2] textures looked up, [3] of them past the slots; [4] mesh draws drawn from the
 * packet memo (the last packet draw's state again). The bounds and the memo are the Vita's (0 elsewhere). */
void ps2e_draw_lookup_census(const ps2e_draw* device, uint64_t out[5]);
/* A backend's own tuning switch by name (render::Backend::SetOption; a diagnosis A/B -- e.g. the Vita GL backend's
 * "draw_legacy"); an unknown name is ignored. On the drawing thread. */
void ps2e_draw_set_backend_option(ps2e_draw* device, const char* name, int value);
/* Shadow-volume counting draws submitted grouped (several volumes' triangles in one submit): the producer notes each
 * volume's own vertices for the casters' re-stamp (ps2e_draw_note_volume_box: their screen box, as a submit of them alone
 * would note it) and, while ps2e_draw_set_volume_boxes_given is on, a counting submit notes nothing itself. The Vita only;
 * elsewhere no-ops. */
void ps2e_draw_set_volume_boxes_given(ps2e_draw* device, int on);
void ps2e_draw_note_volume_box(ps2e_draw* device, const ps2e_draw_vertex* vertices, int count);
/* The same note from the volume's vertices' extent as the producer measured it in its own pass over them: minmax = the
 * smallest x, y and the largest x, y of its whole triangles' vertices (std::min / std::max in vertex order) -- what
 * ps2e_draw_note_volume_box computes from the vertices. The Vita only; elsewhere a no-op. */
void ps2e_draw_note_volume_rect(ps2e_draw* device, const float minmax[4]);
/* ps2e_draw_submit for vertices whose every Q the producer found finite (exponent not all ones) in its own pass over them:
 * the device skips that check (the raw draw's condition). Elsewhere = ps2e_draw_submit. */
int ps2e_draw_submit_finite(ps2e_draw* device, const ps2e_draw_state* state, const ps2e_draw_vertex* vertices, int count);
/* Vertices written IN PLACE: room for `count` vertices in the device's own GPU-visible vertex storage (the Vita:
 * the vertex ring's slice of the frame being drawn) -- the caller writes every vertex there and submits that very array
 * (ps2e_draw_submit / ps2e_draw_submit_finite, `count` or fewer of them), which the device then draws where it lies instead
 * of copying it. The storage is write-only for the caller (GPU memory: reading it back is slow). Null: no room (or not
 * offered: elsewhere always) -- the caller uses its own storage. Valid until the frame is presented. */
ps2e_draw_vertex* ps2e_draw_reserve_vertices(ps2e_draw* device, int count);
/* The casters' re-stamp runs so far (the Vita; elsewhere zeros): out[0] runs (brackets re-stamped with the
 * stencil set once), out[1] mesh casters' full draws, out[2] mesh casters' packet replays, out[3] skin casters' full draws,
 * out[4] skin casters' packet replays, out[5] vertex casters' draws */
void ps2e_draw_restamp_census(const ps2e_draw* device, uint64_t out[6]);
/* The frame's casters as its opaque draws left them: called between the opaque and the translucent draws, the pixels whose
 * alpha is then a PS2 0x80 or more stay casters for the shadow bracket even where a later draw that writes no depth (the
 * water over a sinking Crash) replaced their alpha -- the retail picture shows no shadow on them. The Vita's re-stamp keeps
 * the same pixels already (a no-op there). Returns 1 when marked. */
int ps2e_draw_mark_casters_early(ps2e_draw* device);
/* 1 = the producer keeps every vertex array it hands to ps2e_draw_submit valid until the frame's ps2e_draw_present
 * (its frame's draw lists outlive their drawing): the device may then keep pointers to a frame's vertices instead of
 * copying them (the Vita's shadow casters, drawn again at the shadows' composite). 0 (the default) = copied. */
void ps2e_draw_set_retained_vertices(ps2e_draw* device, int on);
/* (the Vita) The draws submitted while on are the frame's far background drawn after the opaque scene -- the sky's
 * layers with a depth test that passes only at the far value the background's depth reset left (the producer's
 * TE_GFX_VITA_SKY_LAST): not the shadows' casters, as when they were drawn first without a depth test. */
void ps2e_draw_set_far_background(ps2e_draw* device, int on);

/* The SCREEN COPY (TwinsanityEvolved; TR shaders.cpp WriteScreenCopy / shadersettings.cpp MakeScreenCopySettings): the
 * screen-copy shader types (0x10 / 0x11, and 0x18's distortion) began their material's packet by copying the frame as
 * drawn so far into a buffer they then sampled as their TEXTURE (the whole frame across 0..1, clamped) -- the cavern's
 * crystals refract what lies behind them. ps2e_draw_copy_frame takes that copy of the frame target, ordered with the
 * submits around it (every draw submitted before it is in the copy, nothing after); a state whose `texture` is
 * PS2E_DRAW_TEXTURE_FRAME_COPY samples the latest copy of this frame (u, v = fractions of the frame, CLAMP; the copy's
 * alpha is the frame's as written). A frame-copy draw with no copy taken this frame is refused (nothing drawn). The
 * copy is the frame target's physical size and is re-made with it. */
#define PS2E_DRAW_HAS_FRAME_COPY 1
#define PS2E_DRAW_TEXTURE_FRAME_COPY 0xFFFFFFF0u
int ps2e_draw_copy_frame(ps2e_draw* device);
/* (TwinsanityEvolved) The screen copy of a REGION of the frame: `box` = frame pixels x0, y0, x1, y1 (the GS's orientation,
 * fractional values covered whole, padded a pixel), clipped to the frame; the copy texture keeps what it holds outside it.
 * A producer asks for a region only where the texture already equals the frame outside it (graphics_copies.cpp: every
 * pixel written since the frame's last copy lies inside). The frame's first copy, a null box, and a device that does not
 * copy regions (the Vita whenever the copy's scene cannot be begun without vitaGL's mask quad: gl_device.cpp
 * VitaCopyRect) copy the whole frame -- never less than asked. Returns 1 when the copy texture holds this frame there. */
#define PS2E_DRAW_HAS_COPY_REGION 1
int ps2e_draw_copy_frame_region(ps2e_draw* device, const float box[4]);
/* Announces, before the frame's ps2e_draw_begin_frame, how many screen copies (ps2e_draw_copy_frame) the frame about to
 * be drawn takes. A tile-based device (the Vita: vitaGL / GXM) ends its GPU scene at each copy and would start the
 * next scene with the depth / stencil at their background values; announced, it keeps the frame's depth and stencil
 * across those scene breaks for that frame only (they are stored and loaded again around each copy: memory traffic
 * worth paying in a frame with copies alone). A copy in a frame that was not announced still copies, and the device
 * keeps the depth from the next frame on. Elsewhere a no-op. */
#define PS2E_DRAW_HAS_EXPECT_COPIES 1
void ps2e_draw_expect_frame_copies(ps2e_draw* device, int copies);

/* Scans the frame target out to the window (letterboxed to the window's size) and swaps. */
void ps2e_draw_present(ps2e_draw* device);
uint64_t ps2e_draw_frames_presented(const ps2e_draw* device);
/* The frame RESOLVED as a present would resolve it -- the armed capture (ps2e_draw_read_presented, ps2e_draw_write_ppm)
 * holds it afterwards -- but never shown: no blit to the window, no swap, the presented count unchanged. A producer
 * that presents at its own cadence (every other picture at 30; interpolated frames between the pictures at 120 and
 * above) resolves the pictures it does not show this way, so a judge's picture hashes are the same at every rate.
 * Without an armed capture the call only ends the frame. */
void ps2e_draw_capture_frame(ps2e_draw* device);
#define PS2E_DRAW_HAS_CAPTURE_FRAME 1
/* The CT16 present (a title's retail look: the frame drawn in 24-bit colour and copied into the 16-bit buffer the CRTC
 * showed, with the GS's dithering -- DTHE and the DIMX matrix). enable 1 = every present quantises each channel to its
 * top five bits after adding the frame pixel's DIMX cell (-4..3), shown expanded to the full range; 0 = the frame's
 * colours as drawn (the default). `dimx` is the GS DIMX register (16 nibbles, a 3-bit two's-complement cell each, row-
 * major from the lowest); 0 = the GS's default matrix. Takes effect from the next present. */
void ps2e_draw_set_present_ct16(ps2e_draw* device, int enable, uint64_t dimx);
int ps2e_draw_present_ct16(const ps2e_draw* device);
#define PS2E_DRAW_HAS_PRESENT_CT16 1

/* ---- presentation (the title's Display page; TwinsanityEvolved) ------------------------------------------*/
/* How the frame is shown: the display SHAPE (the PS2's frame is anamorphic -- 512 wide for a 4:3 TV whether it is 448
 * or 512 lines tall -- so the frame's own pixel aspect is never what a TV showed), the fit, a move of the picture
 * (the console's "centre screen" option: DISPLAY DX/DY), the scan-out filter, the swap interval, the window mode, the
 * frame target's INTERNAL scale (physical = logical * scale: the same geometry rasterised at 2x..4x) and the windowed
 * size. Set from any thread; the window changes (mode, size) are applied by ps2e_draw_pump on the window's thread, the
 * internal scale and the texture filter at the next ps2e_draw_begin_frame, the rest by the next present on the
 * drawing thread. Every default (all zero) reproduces the plain picture exactly -- the frame's own pixels
 * letterboxed, linear, no swap interval, windowed, scale 1 -- so a consumer that never sets a presentation (and the
 * frozen judges' captures) is unchanged; a title's Display page chooses the TV shape (4:3) for play. */
typedef enum ps2e_draw_aspect {
    PS2E_DRAW_ASPECT_PIXEL = 0,  /* the frame's own pixels (512x512 PAL shows square): the default*/
    PS2E_DRAW_ASPECT_4_3 = 1,    /* the console's TV */
    PS2E_DRAW_ASPECT_16_9 = 2,
    PS2E_DRAW_ASPECT_16_10 = 3,
    PS2E_DRAW_ASPECT_21_9 = 4,
    PS2E_DRAW_ASPECT_WINDOW = 5  /* the window's shape (stretch) */
} ps2e_draw_aspect;
typedef enum ps2e_draw_fit { PS2E_DRAW_FIT_LETTERBOX = 0, PS2E_DRAW_FIT_FILL = 1, PS2E_DRAW_FIT_INTEGER = 2 } ps2e_draw_fit;
typedef enum ps2e_draw_window_mode { PS2E_DRAW_WINDOWED = 0, PS2E_DRAW_FULLSCREEN = 1, PS2E_DRAW_BORDERLESS = 2 } ps2e_draw_window_mode;
typedef struct ps2e_draw_presentation {
    int aspect;          /* ps2e_draw_aspect */
    int fit;             /* ps2e_draw_fit */
    int offset_x;        /* the picture moved, in FRAME pixels (scaled with it): the title's MoveDisplay -- the PS2 moved */
    int offset_y;        /*   up to 256 VCK (64 frame pixels) across and 32 lines down */
    int nearest;         /* 1 = nearest scan-out, 0 = linear (the default) */
    int vsync;           /* the swap interval: 1 = on, 0 = off (the default: the producer paces itself) */
    int window_mode;     /* ps2e_draw_window_mode */
    int scale;           /* the frame target's internal scale 1..4 (0 or 1 = the default; the capture stays the logical size) */
    int window_scale;    /* windowed: the window resized to frame * window_scale on the next pump (0 = leave it) */
    int texture_filter;  /* the draws' texture sampling: 0 = the title's own per-draw choice (TEX1), 1 = point, 2 = bilinear */
} ps2e_draw_presentation;
#define PS2E_DRAW_HAS_PRESENTATION 1   /* for consumers pinned across engine versions */
void ps2e_draw_set_presentation(ps2e_draw* device, const ps2e_draw_presentation* presentation);
void ps2e_draw_get_presentation(const ps2e_draw* device, ps2e_draw_presentation* out);
/* The window rectangle the last present drew the frame into (window pixels; the capture's coordinates when headless). */
void ps2e_draw_last_output_rect(const ps2e_draw* device, int* x, int* y, int* w, int* h);

/* ---- the presented image (judges) ---------------------------------------------------------------------------- */
/* While armed, every present keeps the window image it drew. ps2e_draw_read_presented copies it as RGBA8, top row
 * first, into `rgba8` (capacity `capacity` bytes) and reports its size; returns 1 when an image was copied, 0 when
 * none is held or the buffer is too small (the size is still reported so the caller can retry). */
void ps2e_draw_arm_capture(ps2e_draw* device, int armed);
int ps2e_draw_read_presented(ps2e_draw* device, uint8_t* rgba8, size_t capacity, int* width, int* height);
/* The held presented image written as a binary PPM (P6). Returns 1 on success. */
int ps2e_draw_write_ppm(ps2e_draw* device, const char* path);

/* Counters a producer may report: draws submitted and refused since create, triangles this frame. */
typedef struct ps2e_draw_counters {
    uint64_t draws;
    uint64_t refused;
    uint64_t triangles;
    uint64_t textures_alive;
} ps2e_draw_counters;
void ps2e_draw_counts(const ps2e_draw* device, ps2e_draw_counters* out);

#ifdef __cplusplus
}
#endif
#endif /* PS2E_DRAW_H */
