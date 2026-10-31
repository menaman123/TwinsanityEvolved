// render/backend.h -- Backend: the contract between the GS HLE layers and a
// graphics API.
//
// Everything above this line speaks in DrawIr, TextureSource and opaque SurfaceIds;
// everything below it speaks GL / Vulkan / software. No PS2 VRAM address, no PSM
// layout arithmetic and no page / epoch bookkeeping crosses this boundary: the
// target cache owns surfaces by SurfaceId, the texture-source resolver names a
// surface (or decoded bytes) in a TextureSource, and the class renderers lower to
// Draw calls. DrawParams carries the per-draw decisions made above the interface
// (which surfaces, the logical target size, the alt16 route, the per-frame clear);
// DrawResult returns what the coherence tail above needs (the fused bounding boxes,
// the blend route taken, the 5551 write mask, the Cd copy performed).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/render/surface_format.h"

namespace ps2r {
namespace gs {
struct DrawIr;
struct TextureSource;
}  // namespace gs

namespace render {

struct Caps {
    int max_texture_size = 0;
    bool staging_readback = false;   // BeginReadback / PollReadback are asynchronous
    bool reverse_subtract = false;   // GL_FUNC_REVERSE_SUBTRACT-class blend equation
    bool depth_sample = false;       // depth attachment sampleable as a texture
    bool texture_barrier = false;
    bool compute = false;
};

struct SurfaceId {
    uint32_t id = 0;                 // 0 = none
};

struct FenceId {
    uint64_t id = 0;                 // 0 = none
};

struct Rect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};

struct SurfaceDesc {
    uint32_t width = 0;              // physical texels
    uint32_t height = 0;
    SurfaceFormat format = SurfaceFormat::Rgba8;
    bool renderable = false;         // colour attachment with a framebuffer object
    uint8_t scale = 1;               // physical / logical factor (1 in the harness profile)
    bool cpu_writable = false;       // written in place through Backend::SurfaceMemory: fresh storage of its own (a
                                     // backend's texture pool never hands it out or takes it back)
};

// Clear selection for Backend::Clear.
enum ClearFlags : uint32_t { kClearColor = 1u, kClearDepth = 2u };

// Stencil test + update for one draw (API-agnostic; the GL backend maps them to
// glStencilFunc / glStencilMask / glStencilOp). The stencil-fail op is always KEEP;
// `pass_op` applies to fragments that pass both tests, `depth_fail_op` to fragments
// that pass the stencil test and fail the depth test (a shadow volume counts where it
// lies at or behind the scene surface; KEEP for every other use).
struct StencilState {
    enum Func : uint8_t { kAlways = 0, kEqual = 1 };
    enum Op : uint8_t { kKeep = 0, kInvert = 1, kIncrWrap = 2, kDecrWrap = 3, kReplace = 4 };
    bool enabled = false;
    Func func = kAlways;
    uint8_t ref = 0;
    uint8_t cmp_mask = 0xFF;
    uint8_t write_mask = 0xFF;
    Op pass_op = kKeep;
    Op depth_fail_op = kKeep;
};

// Which program a draw runs: the PS2 register program (None) or one of the
// stencil-composite specialisations -- StripCount draws the geometry into a
// stencil-only attachment (DATE + colour-authored Z discards in the shader, no
// colour output; the caller's StencilState counts the survivors), Invert is the
// folded byte-NOT self-read of a 32-bit target through its Cd copy.
enum class StencilProgram : uint8_t { None = 0, StripCount = 1, Invert = 2 };

// Register-program uniforms a draw LEAVES AS THE PREVIOUS DRAW WROTE THEM. The
// reference's class-native generic-program arms (skinned_native.inc
// DrawSkinnedLitNative, sprite_hud_native.inc DrawSpriteHudNative,
// particles_native.inc DrawParticlesNative, fmv_blit_native.inc DrawFmvBlitNative --
// each replicates DrawBatch's SetU* sites minus these) never write them, so a
// class-routed draw observes the values of the last full-set draw (a 16-bit frame's
// 5-bit quantisation carried onto the next CT32 accumulator draws, opengl_backend.cpp
// :10817-10823 vs skinned_native.inc:1145-1170). Bits compose per class shape; 0 = the
// full register program (DrawBatch, opengl_backend.cpp:8288-14398).
enum InheritedUniform : uint32_t {
    kInheritCt16Quant = 1u << 0,   // u_ct16q
    kInheritDither = 1u << 1,      // u_dthe, u_dimx_lo, u_dimx_hi
    kInheritDrawScale = 1u << 2,   // u_draw_scale
    kInheritUvNative = 1u << 3,    // u_uv_native
    kInheritBlendAbcd = 1u << 4,   // u_blend_a..d, u_fix_alpha
    kInheritZsrcMap = 1u << 5,     // u_zsrc_num, u_zsrc_den
};

// Cd-copy tracker state a colour target owns (the shader-side blend reads the
// destination from a copy of the attachment; the copy is refreshed lazily over
// the union of the rectangles written since it was last taken). Half-open device
// rectangles; last_route records which attachment (main 0 / alt16 companion 1)
// the copy mirrors.
struct CdCopyState {
    bool valid = false;
    int32_t dirty_x0 = 0, dirty_y0 = 0, dirty_x1 = 0, dirty_y1 = 0;
    int8_t last_route = -1;
};

struct DrawParams {
    SurfaceId color;                 // the target colour attachment (always set)
    SurfaceId depth;                 // its depth attachment (may be none)
    SurfaceId rt_copy;               // the target's Cd copy texture (none => no sw blend copy)
    SurfaceId alt16;                 // 16-bit companion attachment (bound when alt16_route)
    SurfaceId clut;                  // 256x2 CLUT texture for the indexed serve (unit 2)
    SurfaceId idx_partner;           // fmt=5 partner target (unit 3)
    SurfaceId zsrc;                  // colour-authored Z source for the in-shader compare (unit 4)
    int32_t render_w = 0;            // logical target size this draw rasterises into
    int32_t render_h = 0;
    int32_t target_w = 0;            // logical allocation size of the target (grow-only)
    int32_t target_h = 0;
    uint8_t scale = 1;               // target scale (physical = logical * scale)
    bool has_32bit_content = false;
    bool alt16_route = false;        // draw into the companion instead of the main attachment
    bool rt_copy_available = false;  // rt_copy names a live texture
    CdCopyState* cd_copy = nullptr;  // the target's tracker (mutated by the draw)
    bool zsrc_armed = false;         // the S9 colour-authored Z compare route
    uint8_t zsrc_scale = 1;          // scale of the zsrc surface
    bool do_frame_clear = false;     // per-game-frame clear before this draw
    bool clear_color = false;        // the clear also clears colour (scanout target)
    StencilProgram program = StencilProgram::None;
    SurfaceId stencil;               // the DepthStencil24_8 attachment (StripCount draws into it alone)
    // The stencil test and update around this draw. A StripCount draw applies it over the stencil
    // attachment; a register-program draw (None) applies it when `enabled` over the depth attachment,
    // which must then be a DepthStencil24_8 surface (the typed-draw surface's shadow-volume counter).
    StencilState stencil_state;
    uint32_t inherited_uniforms = 0; // InheritedUniform bits this draw does not write (0 = full set)
};

struct DrawResult {
    bool drew = false;               // false when the guards rejected the draw
    bool bbox_valid = false;
    float bbox_tri[4] = {0, 0, 0, 0};   // x0 y0 x1 y1 over the triangle-complete vertices
    float bbox_all[4] = {0, 0, 0, 0};   // extended over the <= 2 tail vertices
    bool alt16_route = false;
    bool sw_blend_active = false;    // shader-side blend engaged (rt_copy bound)
    bool needs_rt_copy = false;
    bool route_rt_blend = false;     // blended RT read mapped to exact fixed-function
    bool wrap_route = false;         // COLCLAMP=0 wrap routed to the shader blend
    bool fbmsk16_exact = false;      // 5551 mask not channel-aligned
    bool fbmsk16_engaged = false;    // the exact merge ran in the shader
    uint32_t fbmsk16 = 0;            // FRAME.FBMSK folded to the 5551 layout
    bool cd_copy_performed = false;
    Rect cd_copy_rect;               // logical rect copied (w == 0 => whole target)
    Rect write_rect;                 // bbox_all ∩ scissor ∩ target, logical, half-open
};

// Surface preserves retained callers' sampling. Owned presentation declares
// its window filter explicitly; this never changes a draw texture's sampler.
enum class PresentationFilter : uint8_t { Surface, Nearest, Linear };

// The GS's default dither matrix (DIMX: 16 cells of a 3-bit signed value -4..3, a nibble each, row-major from the lowest)
constexpr uint64_t kDefaultDimx = 0x6071243571603524ull;

struct ScanoutDesc {
    SurfaceId source;
    Rect src;                        // physical texels; all-zero means the complete source
    Rect dst;                        // window pixels
    bool flip_y = true;              // targets store PS2 top at GL bottom
    PresentationFilter filter = PresentationFilter::Surface;
    // The CT16 present: the source shown as the GS's copy into a 16-bit buffer with dithering would have left it --
    // each channel's 8 bits plus the DIMX cell of the frame pixel (-4..3), the top five kept, expanded to full range
    bool dither16 = false;
    uint64_t dimx = kDefaultDimx;    // GS DIMX
    // false = the scan-out is RESOLVED (the armed capture reads it: a judge's hash of a picture) but never reaches the
    // window: no blit to the default framebuffer, no swap. A producer that presents at its own cadence (a scene
    // interpolator between game pictures) resolves every picture it does not show this way. Default: shown.
    bool to_window = true;
};


// A native mesh's vertex (Backend::CreateMesh): the model's position and STQ as decoded, the colour an unlit draw shows,
// the normal and the colour a lit draw lights. The first 32 bytes are GpuVertex's layout (position, STQ, colour, fog).
struct MeshVertex {
    float x, y, z;
    float s, t, q;
    uint8_t r, g, b, a;
    uint8_t fog, pad[3];
    float nx, ny, nz;
    uint8_t lr, lg, lb, la;
};
static_assert(sizeof(MeshVertex) == 48, "MeshVertex is the 48-byte attribute layout");

// One mesh draw's transform (Backend::DrawMesh): model -> GS screen (12.4 fixed x / y, depth; the title's row-vector
// convention, rows 0..3), the GS origin taken off after the divide, the UV scroll added to S / T before it, and the
// lighting: colour = lit colour x (ambient + sum colour_i max(0, n . dir_i)), clamped.
struct MeshDrawParams {
    float screen[16] = {};
    float offset[2] = {};
    float scroll[2] = {};
    bool lit = false;
    float light_dir[3][3] = {};
    float light_colour[3][3] = {};
    float ambient[3] = {};
    // the draw's screen box (frame pixels: x0, y0, x1, y1), for the destination copy a shader-side blend reads
    float box[4] = {};
    // a sub-range of the mesh's vertices (whole triangles; count 0 = the whole mesh)
    uint32_t first = 0, count = 0;
};

// One instance of an instanced packet draw (Backend::DrawMeshPacketInstances): its MeshDrawParams' transform
// fields -- screen (16 floats), offset (2), scroll (2) -- and its box (4: the census's share of the frame)
struct MeshInstance {
    const float* screen;
    const float* offset;
    const float* scroll;
    const float* box;
};

// A GPU-skinned mesh's vertex (Backend::CreateSkinMesh; the Vita): the skin's static data as its batch unpacked it --
// the bind-pose position and normal (the int lanes times the batch's scales), S and T (scaled) and 1, the colour's lanes
// as the program reads them (sign-extended ints, as floats) with the vertex's final alpha byte in the fourth, the three
// bone weights (the words' float values, palette bits included) and their joints in the draw's palette (local indices).
struct SkinVertex {
    float x, y, z;
    float s, t, one;
    float r, g, b, a;
    float nx, ny, nz;
    float w0, w1, w2;
    uint8_t j0, j1, j2, j3;
    float index;   // the vertex's place in a blend skin's part (its displacement in the draw's), 0 in a plain skin's
};
static_assert(sizeof(SkinVertex) == 72, "SkinVertex is the 72-byte attribute layout");

// A vertex as the producer gives it (Backend::DrawRaw): frame pixels, depth, S and T, the colour, Q (0 = 1)
struct RawVertex {
    float x, y, z;
    float s, t;
    uint8_t r, g, b, a;
    float q;
};
static_assert(sizeof(RawVertex) == 28, "RawVertex is the 28-byte producer layout");

class Backend {
public:
    virtual ~Backend() = default;
    virtual Caps QueryCaps() const = 0;
    virtual SurfaceId CreateSurface(const SurfaceDesc& desc) = 0;
    virtual void DestroySurface(SurfaceId id) = 0;
    virtual SurfaceDesc DescribeSurface(SurfaceId id) const = 0;
    virtual void Clear(SurfaceId color, SurfaceId depth, uint32_t rgba, float z,
                       uint32_t flags = kClearColor | kClearDepth) = 0;
    // Attachment row order, stride in bytes (zero means tightly packed):
    // RGBA8/R8 bytes or Depth32f floats, matching DescribeSurface's format.
    virtual void Upload(SurfaceId id, const Rect& rect, const void* bytes, size_t stride) = 0;
    // A sampled RGBA8 texture made WITH its whole first level (`bytes`, tightly packed): CreateSurface then Upload of the
    // whole image -- the default -- or one step where the backend can (the Vita's GL backend: the storage allocated once,
    // with its texels, instead of made empty and then replaced)
    virtual SurfaceId CreateTextureWithData(const SurfaceDesc& desc, const void* bytes) {
        const SurfaceId id = CreateSurface(desc);
        if (id.id && bytes) Upload(id, Rect{0, 0, int32_t(desc.width), int32_t(desc.height)}, bytes, 0);
        return id;
    }
    // Mip levels of a texture surface (level 0 is the surface itself).
    virtual void UploadLevel(SurfaceId id, int level, int w, int h, const void* rgba) = 0;
    virtual void FinishMipChain(SurfaceId id, int authored_levels, bool synthesize_tail) = 0;
    virtual DrawResult Draw(const gs::DrawIr& ir, const gs::TextureSource& tex,
                            const DrawParams& params) = 0;
    // Native meshes: a model's triangles kept on the device (MeshVertex, a triangle list) and drawn through a matrix --
    // the transform, the divide to the GS's 12.4 screen and the lighting done per vertex by the device instead of on
    // the CPU. CreateMesh returns 0 on a backend without them (the caller then builds the triangles itself); DrawMesh
    // takes `ir` for the draw's state only (its vertices are not read).
    virtual uint32_t CreateMesh(const MeshVertex*, size_t) { return 0; }
    // The same mesh from its indexed form computed ahead (engine/render/mesh_index.h MeshIndices over the list: `distinct`
    // vertices, `count` 16-bit indices -- the list's vertices): 0 when the backend does not make its meshes from that form
    // (the caller then makes it from the list with CreateMesh)
    virtual uint32_t CreateMeshIndexed(const MeshVertex*, size_t, const uint16_t*, size_t) { return 0; }
    virtual void DestroyMesh(uint32_t) {}
    virtual DrawResult DrawMesh(const gs::DrawIr&, const gs::TextureSource&, const DrawParams&, uint32_t,
                                const MeshDrawParams&) { return DrawResult{}; }
    // Another mesh under the state of the mesh draw just made (nothing drawn or bound since): its transform and its
    // draw only. drew is false when the device cannot vouch for that state (the caller then makes the full DrawMesh).
    virtual DrawResult DrawMeshAgain(uint32_t, const MeshDrawParams&) { return DrawResult{}; }
    // Another mesh under the state of the mesh draw just made but for its texture (`ir` / `tex`: the new record, which
    // differs from the run's in the texture only; nothing drawn or bound since): the texture's part of the state, the
    // transform and the draw. drew is false when the device cannot vouch for that (the caller makes the full DrawMesh).
    virtual DrawResult DrawMeshTexture(const gs::DrawIr&, const gs::TextureSource&, uint32_t, const MeshDrawParams&) {
        return DrawResult{};
    }
    // Precomputed mesh draw PACKETS (the Vita's GL backend; elsewhere 0 / false): TakeMeshPacket hands back the packet the
    // mesh draw just made (DrawMesh / DrawMeshAgain / DrawMeshTexture) recorded -- what the device handed its GPU for that
    // draw apart from the draw's own transform -- or 0 (none: not a draw the device can replay). DrawMeshPacket draws `mesh`
    // through `mp` with such a packet, recorded for the same state (and the same `lit`), when the caller vouches that the
    // device's context is the one the state's draw would set up -- the draw is one DrawMeshAgain / DrawMeshTexture could
    // make (the same state as the run's draw, or the run's state with another texture / alpha test / mipmapping; nothing
    // drawn or bound since) -- and the device finds its own state unmoved since; false: nothing drawn (the caller then
    // makes the draw as before).
    virtual uint32_t TakeMeshPacket() { return 0; }
    virtual bool DrawMeshPacket(uint32_t, uint32_t, const MeshDrawParams&) { return false; }
    // INSTANCED packet draws (the Vita's GL backend; elsewhere false): `n` consecutive DrawMeshPacket draws of one
    // packet and one mesh range (`first` / `count` as MeshDrawParams' -- the same for all) differing in their matrix only,
    // each instance's MeshDrawParams::screen / offset / scroll (and box, the census's) given, offset / scroll the same for
    // all -- drawn as ONE instanced draw, in the instances' order, under DrawMeshPacket's conditions; false: nothing drawn
    // (the caller then draws them one by one). An unlit mesh's draws only (a lit draw's lights are per draw).
    virtual bool DrawMeshPacketInstances(uint32_t, uint32_t, uint32_t, uint32_t, const MeshInstance*, int) { return false; }
    virtual bool InstancedPacketDraws() const { return false; }   // whether DrawMeshPacketInstances can draw at all
    // GPU-skinned meshes (the Vita): a skin part's static vertices and its indexed triangle list kept on the device, drawn
    // through a bone palette (`bones`: `joints` x 3 rows of 4 floats, each joint's 3x4 transposed -- row k = the k-th
    // lanes of its three axes and its translation) then the mesh transform and the lighting as DrawMesh's lit path, the
    // colour formed as the skin program forms it. CreateSkinMesh returns 0 on a backend without them (the caller builds on
    // the CPU); a skin mesh is destroyed with DestroyMesh.
    virtual uint32_t CreateSkinMesh(const SkinVertex*, size_t, const uint16_t*, size_t) { return 0; }
    // `disp`: a blend skin's displacement per vertex (`disp_count` x 4 floats; null / 0 for a plain skin)
    virtual DrawResult DrawSkinMesh(const gs::DrawIr&, const gs::TextureSource&, const DrawParams&, uint32_t,
                                    const MeshDrawParams&, const float*, int, const float*, int) {
        return DrawResult{};
    }
    // GPU-skin draw PACKETS (the Vita's GL backend; elsewhere 0 / false), as the mesh draw packets: TakeSkinPacket hands
    // back the packet the skin draw just made (DrawSkinMesh) recorded, DrawSkinPacket draws `mesh` through `mp` and the
    // palette with one -- recorded for the same state -- when the caller vouches that the device's context is the one the
    // state's draw would set up (a skin of the mesh run's context: the run's state but for its texture / alpha test /
    // mipmapping) and the device finds its own state unmoved since; false: nothing drawn.
    virtual uint32_t TakeSkinPacket() { return 0; }
    virtual bool DrawSkinPacket(uint32_t, uint32_t, const MeshDrawParams&, const float*, int, const float*, int) { return false; }
    // Raw draws: the producer's vertices uploaded as they are (no records, no conversion pass: the device reads Q and
    // scales S / T itself) for a draw whose texture needs no per-vertex rebase. DrawsRaw says whether this draw may;
    // DrawRaw takes `ir` for the state only and `box` as the vertices' screen box (x0, y0, x1, y1).
    virtual bool DrawsRaw(const gs::DrawIr&, const gs::TextureSource&, const DrawParams&) const { return false; }
    virtual DrawResult DrawRaw(const gs::DrawIr&, const gs::TextureSource&, const DrawParams&, const RawVertex*, size_t,
                               const float*) { return DrawResult{}; }
    // Raw vertices written in place (the Vita's GlDevice; elsewhere null): room for `n` RawVertex records in the
    // device's own vertex storage (GPU-visible: the vertex ring's slice of this frame) -- the caller writes them there, and
    // a DrawRaw handed exactly that storage draws them where they lie, copying nothing. Null: no room (the caller keeps its
    // own storage, DrawRaw copies it as before). Valid for the frame being drawn.
    virtual RawVertex* ReserveRaw(size_t) { return nullptr; }
    // A run of draws under ONE stencil state set around the run instead of around each draw (the Vita's casters'
    // re-stamp; their DrawParams then carry none): ApplyStencilState before the run's first draw, ResetStencilState after
    // its last -- each draw sees the state every draw's own set / reset gave it
    virtual void ApplyStencilState(const StencilState&) {}
    virtual void ResetStencilState() {}
    // Movie pictures (ps2e_draw_movie_*; the Vita's GlDevice, every other backend: not offered). SurfaceMemory: the CPU
    // address of a level-0 texture surface's storage and its row stride in bytes, for a writer that fills the texture in
    // place (a movie decoder writing its picture straight into the GPU's memory; the caller keeps it from writing while a
    // frame the GPU has not finished samples it) -- nullptr when the backend cannot hand it out. DrawMoviePicture: a
    // picture drawn from such surfaces over a rectangle of the target with the backend's own movie program (YUV planes
    // converted, or an RGBA image as it is), no blending, no depth test or write; false = not drawn (no movie program).
    struct MoviePicture {
        SurfaceId color, depth;            // the target (DrawParams'), its logical size and internal scale
        int target_w = 0, target_h = 0, scale = 1;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;   // the quad, logical target pixels from the top-left corner
        float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;   // its texture extent (fractions; v0 = the picture's top row)
        bool yuv = false;                  // planes[0..2] = the Y, U, V R8 surfaces (limited range); else planes[0] RGBA8
        SurfaceId planes[3];
        bool linear = true;                // bilinear sampling (else nearest)
    };
    virtual void* SurfaceMemory(SurfaceId, size_t*) { return nullptr; }
    virtual bool DrawMoviePicture(const MoviePicture&) { return false; }
    // Stencil-composite primitives. ClearStencil writes `value` to every stencil
    // texel of a DepthStencil24_8 surface. DrawConstQuad rasterises a constant
    // colour over `scissor` (logical texels of the stencil surface) under the
    // stencil test -- into `color` with `stencil` attached when `color` is set
    // (alpha written only when `write_alpha`), or into the stencil attachment
    // alone when `color` is none (colour writes off; the pass_op updates the
    // stencil). Blend off, depth test ALWAYS without writes.
    virtual void ClearStencil(SurfaceId stencil, uint8_t value) = 0;
    virtual void DrawConstQuad(SurfaceId color, SurfaceId stencil, const Rect& scissor,
                               uint32_t rgba, bool write_alpha, const StencilState& st) = 0;
    // Sets `bits` in the stencil of every texel of `stencil` whose texel of `color` has a fully
    // opaque alpha byte (0xFF: the GPU's scaling of a PS2 alpha of 0x80 or more); every other
    // stencil bit and every colour texel are left as they are. A backend without a stencil
    // attachment returns false and marks nothing.
    virtual bool MarkStencilWhereOpaque(SurfaceId, SurfaceId, uint8_t) { return false; }
    // The same two over `n` rectangles of the target only (physical texels; the shadow bracket's region: ps2e_draw
    // ps2e_draw_shadow_region). A backend that cannot restrict them clears / marks the whole target -- the same picture.
    virtual void ClearStencilIn(SurfaceId stencil, uint8_t value, const Rect*, int) { ClearStencil(stencil, value); }
    virtual bool MarkStencilWhereOpaqueIn(SurfaceId color, SurfaceId stencil, uint8_t bits, const Rect*, int) {
        return MarkStencilWhereOpaque(color, stencil, bits);
    }
    // Diagnostics only: the stencil bytes of `rect` (physical texels, rows bottom-up as the
    // attachment stores them, one byte per texel) of a DepthStencil24_8 surface. A backend
    // that cannot read its stencil returns false and writes nothing.
    virtual bool ReadbackStencil(SurfaceId, const Rect&, uint8_t*) { return false; }
    virtual void CopyRect(SurfaceId src, const Rect& src_rect, SurfaceId dst,
                          const Rect& dst_rect, uint32_t transform) = 0;
    // The frame about to begin on the render target `color` takes screen copies (CopyRect of `color` mid-frame): `keep`
    // asks the backend to keep the target's depth / stencil across the copies. A tile-based backend (vitaGL / GXM on the
    // Vita) ends the GPU scene at a copy -- the frame's colour reaches memory only at a scene's end -- and starts the next
    // scene with its depth / stencil at their background values unless the surface is force-stored and force-loaded,
    // which costs memory traffic only worth paying in such a frame. Called before the frame's first draw; a backend whose
    // attachments survive a copy (desktop GL) ignores it.
    virtual void KeepDepthAcrossCopies(SurfaceId, bool) {}
    // Synchronous readback of `rect` (physical texels), rows bottom-up as the
    // attachment stores them, RGBA8 (colour) or float (depth).
    virtual void Readback(SurfaceId id, const Rect& rect, void* out, size_t stride) = 0;
    virtual FenceId BeginReadback(SurfaceId id, const Rect& rect) = 0;
    virtual bool PollReadback(FenceId fence, void* out, size_t stride) = 0;
    // A missing nonzero source or invalid explicit source rectangle refuses
    // before clear/swap; source 0 retains the cold no-image presentation.
    virtual void Present(const ScanoutDesc& desc) = 0;
    // The presenter's OUTPUT for a frame dump: while armed, Present keeps the
    // window image it drew (the default framebuffer after the display quad,
    // before the swap) and ReadPresented hands over that RGBA8 image (rows
    // bottom-up, the destination rectangle's size). Diagnostics only: never a
    // draw, scanout or mirror input. A backend without a window reads false.
    virtual void ArmPresentCapture(bool) {}
    virtual bool ReadPresented(std::vector<uint8_t>*, int*, int*) { return false; }
    // The window's drawable size in pixels: what Present scans into when ScanoutDesc.dst is
    // empty and the size of the image ReadPresented hands over. A backend without a window
    // ignores it.
    virtual void SetWindowSize(int, int) {}
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    // Run brackets inside a frame: a run is a sequence of Draw calls the backend may batch
    // (vertex storage mapped once); any other work on the surfaces a run draws into -- a
    // clear, a copy, a readback, a present -- ends the run first. BeginFrame / EndFrame open
    // and close a run themselves; a caller that interleaves its own surface work calls these.
    // A backend that draws as it goes leaves them empty.
    virtual void BeginDrawRun() {}
    virtual void EndDrawRun() {}
    // A backend's own tuning switch by name (a diagnosis A/B; an unknown name is ignored). The GL backend on the Vita:
    // "draw_legacy" 1 = its per-draw call sequence as before the drawing thread's rework (both PS2 programs
    // bound per draw, every uniform sent to both at once, the masks reset after every draw), 0 = the current one.
    // "gpu_timers" (the desktop GL backend): the GPU phase timer's mode (render/backends/gl/gl_gpu_timers.h; 0 off).
    virtual void SetOption(const char*, int) {}
    // The GPU phase timer (a diagnosis; a backend without one ignores these): the phase the next work belongs to (the
    // timer's phase numbers: ps2e_draw.h ps2e_draw_phase), the area a phase rasterises where the caller knows it (physical
    // pixels) with the passes / draws it made, and the census window's lines ("" when off or nothing was read).
    virtual void MarkGpuPhase(int) {}
    virtual void NoteGpuFill(int, double, uint32_t) {}
    virtual std::string GpuTimerLines(double) { return std::string(); }
    virtual bool GpuTimersOn() const { return false; }
    // The GPU and driver the backend draws with ("" when not known): the start-up line names it.
    virtual std::string DeviceName() const { return std::string(); }
};

}  // namespace render
}  // namespace ps2r
