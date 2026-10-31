// render/backends/gl/gl_census.h -- GlCensus: what the GL backend's draws cost in GL calls, counted (a diagnosis: the
// Vita's drawing thread pays vitaGL + the emulator's GXM work per glDrawArrays and per glUniform*). Plain counters written
// by the one GL thread; a reader on another thread sees a value at most a frame old (they only feed a log line).
#pragma once

#include <cstdint>
#include <string>

namespace ps2r {
namespace render {

struct GlCensus {
    uint64_t draws_converted = 0;   // GlDrawExecutor::Draw (vertices converted on the CPU, uploaded into the ring)
    uint64_t draws_raw = 0;         // DrawRaw (the producer's vertices uploaded as they are)
    uint64_t mesh_full = 0;         // DrawMesh (the whole state sequence)
    uint64_t mesh_again = 0;        // DrawMeshAgain (the last mesh draw's state, the transform only)
    uint64_t mesh_texture = 0;      // DrawMeshTexture (the last mesh draw's state with another texture)
    uint64_t gl_draws = 0;          // glDrawArrays calls of those draws (a long one is several)
    uint64_t state_calls = 0;       // GL state calls the state cache made (GlStateCache::Mutations)
    uint64_t uniform_calls = 0;     // uniform values changed through the uniform cache (GlUniformCache::Mutations)
    uint64_t uniform_sends = 0;     // glUniform* calls it made (GlUniformCache::Sent: the mirror's and the Vita's synced ones)
    uint64_t state_by_path[4] = {0, 0, 0, 0};   // the state calls made inside DrawMesh / DrawMeshAgain / DrawMeshTexture / DrawRaw
    uint64_t uniform_checked = 0;   // TE_GFX_UNIFORM_CHECK=1: draws whose program's uniforms were checked against the record
    uint64_t uniform_mismatch = 0;  // ... and the uniforms found not holding their value (0 = every draw had its values)
    uint64_t size_uniforms = 0;     // of the state calls: the Vita's te_size_* glUniform2f (ApplyVitaSamplerSizes)
    uint64_t size_skipped = 0;      // te_size_* uploads skipped because the new texture has the size already sent
    // The memory side (the Vita's vitaGL pools: every re-specification is an allocation there, every in-place refill of
    // a texture drawn in the last 4 frames a copy-on-write allocation + a full copy of the old texels)
    uint64_t ring_bytes = 0;        // vertex bytes uploaded into the streaming ring
    uint64_t ring_frame_peak = 0;   // the most one frame uploaded (the reader resets it per window)
    uint64_t ring_copy_bytes = 0, ring_copy_us = 0;   // (the Vita) the timed ring copies (1 in 32): bytes, microseconds
    // (the Vita) the full paths' sampled CPU split (gl_draw_executor.cpp SplitTimer): [0] DrawMesh, [1] DrawRaw (the
    // vertex draws); [k] state set-up, upload, program (+ the raw draw), the GL draw and the rest; the draws timed
    uint64_t split_us[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}}, split_n[2] = {0, 0};
    uint64_t ring_respecs = 0;      // ring storage re-specifications (glBufferData: a wrap / a frame over its slice)
    uint64_t ring_respec_bytes = 0;
    uint64_t tex_uploads = 0;       // level-0 uploads into an existing texture (GlDevice::Upload)
    uint64_t tex_upload_bytes = 0;
    uint64_t tex_respecified = 0;   // of them: whole images re-specified (glTexImage2D: no copy of the old texels)
    uint64_t buffer_subdata = 0;    // glBufferSubData calls (the scanout quad)
    uint64_t scanout_same = 0;      // presents whose scanout quad was already in its buffer (no upload)
    uint64_t meshes_alive = 0;      // native meshes (each a VBO + a VAO) in existence
    uint64_t meshes_created = 0;
    uint64_t mesh_bytes = 0;        // their vertex bytes created (GL_STATIC_DRAW buffers)
    uint64_t mesh_list_vertices = 0;     // the Vita: the native meshes' triangle-list vertices made (GlDevice::CreateMesh) ...
    uint64_t mesh_stored_vertices = 0;   // ... and the vertices their buffers hold (distinct ones when indexed)
    uint64_t mesh_index_us = 0;          // ... and the drawing thread's time deduplicating them (MeshIndices)
    uint64_t mesh_create_us = 0;         // ... and its time making them in all (GlDevice::CreateMesh: the dedup, the uploads)
    // the GPU skin meshes made (GlDevice::CreateSkinMesh): vertices by the most bones a mesh's vertices weigh
    // (1 / 2 / 3), of meshes with a negative colour lane, of blend parts
    uint64_t skin_mesh_vertices[3] = {0, 0, 0}, skin_mesh_negative_vertices = 0, skin_mesh_blend_vertices = 0;
    uint64_t present_draw_us = 0;   // GlDevice::Present before the swap (the frame target's scene ends, the display's begins)
    uint64_t swap_us = 0;           // the swap callback (the Vita: vglSwapBuffers)
    uint64_t swap_max_us = 0;       // the longest swap (the reader resets it per window)
    uint64_t presents = 0;
    // the Vita's PS2 program variants (GlShaderLibrary::Ps2FastProgram): the draws each took, and the fast program's A/B
    // (TE_GFX_VITA_PS2_FAST=abN): which arm is on, and per arm the presents, the present-to-present time, the swap
    uint64_t draws_ps2_fast = 0, draws_ps2_full = 0;
    uint64_t draws_ps2_lean = 0;
    uint64_t lean_uniform_calls = 0;        // glUniform* calls bringing a lean program's uniforms to the full program's
    // the Vita's raw mesh draws (GlShaderLibrary::SelectPs2 with a RawMesh: submitted to GXM directly, no vitaGL draw);
    // the mesh draws that could not be (by reason: off, not a mesh-path program, no twin, a uniform not known, too long)
    uint64_t draws_raw_mesh = 0;
    uint64_t raw_mesh_declined[6] = {0, 0, 0, 0, 0, 0};
    uint64_t ps2_fast_active = 0;           // the A/B arm on (1) / off (0): the fast program's, or the lean programs'
    uint64_t arm_presents[2] = {0, 0};      // [0] off, [1] on
    uint64_t arm_interval_us[2] = {0, 0};
    uint64_t arm_swap_us[2] = {0, 0};
    const char* ab_name = "PS2 fast program";   // what the A/B switches (TE_GFX_VITA_PS2_FAST / TE_GFX_VITA_PS2_LEAN)
    // the Vita's GPU-skinned draws (GlDrawExecutor::DrawSkinMesh): made, refused (no lean variant for the state), indices
    uint64_t skin_draws = 0, skin_refused = 0, skin_indices = 0;
    // the Vita's mesh draw PACKETS (GlShaderLibrary::DrawRawPacket, ps2e_draw's packet cache): mesh draws replayed from a
    // packet (nothing but their own transform computed), packets recorded by raw draws, and the replays declined by reason
    // (the caller then took its slow path): [0] packets off / raw or lean off, [1] no packet cached for the state (the
    // caller's count), [2] the run's GL context moved, [3] stale (the twins rebuilt / the slot reused), [4] the texture
    // record changed, [5] the blend state / the target's format changed, [6] the screen size changed, [7] length / buffer
    uint64_t mesh_packet = 0;
    uint64_t packet_across = 0;     // of them: drawn after GL calls since the run's last slow draw (the context found unchanged)
    uint64_t packets_recorded = 0;
    uint64_t packet_declined[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    // the arms each present's frame was drawn in (GlDevice::Present, before any A/B flip): the packets on, the native meshes
    // drawn indexed -- the census lines name their window's arms (an earlier device log was read one A/B window off)
    uint64_t presents_packets_on = 0, presents_meshes_indexed = 0, presents_counted = 0;
    // the Vita's GPU-skin PACKETS (GlShaderLibrary::DrawSkinPacket: a skin draw replayed raw from the packet its state's
    // last raw skin draw recorded) and the raw skin draws (RawSkinSubmit: the skin program's GXM twin, no vitaGL draw)
    uint64_t skin_packet = 0, skin_raw = 0, skin_packet_declined = 0, skin_raw_declined = 0;
    // the raw draws' unit-0 texture and stream-0 sets, and those skipped (the context held them already:
    // gl_shader_library.cpp RawSetTexture0 / RawSetStream0)
    uint64_t raw_texture_sets = 0, raw_texture_skipped = 0, raw_stream_sets = 0, raw_stream_skipped = 0;
    // the raw mesh draws' vertex default uniform buffers, and those re-used (their composed contents equal to the
    // buffer the context still held for their vertex program: no buffer reserved, none set -- RawSetVertexUniforms)
    uint64_t raw_vertex_uniform_sets = 0, raw_vertex_uniform_kept = 0;
    // the device's lookups that went past their direct-mapped slots to the hash map (GlDevice::FindMesh)
    uint64_t mesh_lookups = 0, mesh_lookup_misses = 0;
    // the INSTANCED packet draws (GlShaderLibrary::DrawRawPacketInstances): instanced draws made, the mesh
    // draws they drew (instances), and runs that were drawn one by one instead (by reason)
    uint64_t instanced_draws = 0, instanced_instances = 0, instanced_declined = 0;
    // the Vita's PER-PRESENT GPU A/B: the arm the frame being drawn is in (GlDevice::BeginFrame: -1 none, 0 / 1)
    // and what it switches -- vita_gpu_census tags every GXM scene the frame ends with it and attributes the GPU's busy
    // samples to the arm of the scene the GPU is on (the two arms alternate every present: the same scenes in both)
    int gpu_ab_arm = -1;
    const char* gpu_ab_name = "";
    const char* gpu_ab_arms[2] = {"off", "on"};
    // the Vita's screen copies (GlDevice::VitaCopyRect): copies drawn on the GPU, the frame's scene continued after them
    // (no scene-start mask update, nothing restored) / restored from the copy (the mask update + the restore pass), and
    // the pixels the copy passes covered (the whole target, or the region asked: ps2e_draw_copy_frame_region)
    uint64_t copies = 0, copies_continued = 0, copies_restored = 0, copy_pixels = 0, copy_whole = 0;
    uint64_t copies_restated = 0;   // quiet copies whose copy texture's vitaGL record was re-stated (the zero-area draw)
    // the Vita's sampled textures made (GlDevice::CreateTextureWithData), the textures vitaGL left without storage there
    // or at a whole image's re-specification (Upload; its allocator refused every pool and the newlib heap: the draw then
    // samples whatever the texture's GXM record held), the level chains it placed off GXM's 16-byte texture alignment (its
    // realloc of level 0 into the chain), and the textures given the levels the PS2 authored (FinishMipChain: written over
    // vitaGL's chain), their bytes and the drawing thread's time waiting for vitaGL's level transfers before the write
    // (only when the platform does not skip them: the transfers skipped are counted), and the authored levels past the
    // chain vitaGL holds (none expected: the PS2's chains stop where an axis reaches 1, as vitaGL's)
    uint64_t tex_made = 0, tex_no_storage = 0, tex_chains = 0, tex_chain_misaligned = 0;
    uint64_t tex_levels_authored = 0, tex_level_bytes = 0, tex_level_wait_us = 0, tex_levels_dropped = 0;
    uint64_t tex_level_transfers_skipped = 0, tex_levels_beyond_chain = 0;
};

// One census, a plain global (constant-initialised): a function-local static's guard was a load + a DMB barrier at every
// use on the Vita's GCC (thread-safe statics) -- ~10 uses per draw on the drawing thread
extern GlCensus g_gl_census;
inline GlCensus& Census() { return g_gl_census; }

// The Vita's GPU-skinned draws per frame by program over the window since the last call (GlShaderLibrary::SelectSkin),
// and the draws refused for want of a lean variant; "" when the skinning programs are not built. Ends in '\\n'.
std::string SkinProgramCensusLines(double frames);

// The Vita's PS2 programs per frame over the window since the last call (GlShaderLibrary::SelectPs2): each lean
// variant's draws and share of the drawn pixels (screen boxes), the fast / full fallbacks and why, the opaque share;
// with TE_GFX_VITA_PS2_CENSUS=1 also the most-drawn state keys. Lines ending in '\n'; "" when nothing was counted.
std::string Ps2ProgramCensusLines(double frames);

}  // namespace render
}  // namespace ps2r
