// engine/render/mesh_index.h -- a native mesh's triangle list deduplicated by value: its DISTINCT vertices (all 48 bytes
// equal) in the order of their first use, and its 16-bit indices into them in the list's order. The Vita's GL backend draws
// a mesh indexed this way (GlDevice::CreateMesh: TE_GFX_VITA_MESH_INDEX); the producer may compute the same form ahead, on
// any thread (ps2e_draw_mesh_prepare), and hand it over (Backend::CreateMeshIndexed) -- one function for both, so the two
// give the same vertices and indices by construction. Header-only, no GL, no device: a pure function of the list.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "engine/render/backend.h"

namespace ps2r::render {

// The list's distinct vertices and its indices into them, in its order; `identity`: every list vertex kept (indices 0, 1,
// 2, ...: the index path alone, a diagnosis). False when it does not pay or does not fit (16-bit indices).
inline bool MeshIndices(const MeshVertex* verts, size_t count, bool identity, std::vector<MeshVertex>& distinct,
                        std::vector<uint16_t>& indices) {
    distinct.clear();
    indices.resize(count);
    if (identity) {
        if (count > 0x10000u) return false;
        distinct.assign(verts, verts + count);
        for (size_t i = 0; i < count; ++i) indices[i] = uint16_t(i);
        return true;
    }
    thread_local std::vector<int32_t> table;
    size_t cap = 64;
    while (cap < count * 2) cap <<= 1;
    table.assign(cap, -1);
    distinct.reserve(count);
    // (the drawing thread made a mesh's table at its creation: the title's first 3D frame ~500 meshes, ~150k vertices --
    // per vertex a hash of 4 words (the position and S) and an equality test of all 12 words, the same equality as a
    // 48-byte memcmp; newlib's memcmp is a call and a byte loop on the A9)
    static_assert(sizeof(MeshVertex) == 48, "a mesh vertex is 12 words");
    auto word = [](const MeshVertex* v, int k) {
        uint32_t x;
        std::memcpy(&x, reinterpret_cast<const unsigned char*>(v) + 4 * k, 4);
        return x;
    };
    auto same = [&word](const MeshVertex* a, const MeshVertex* b) {
        for (int j = 0; j < 12; ++j)
            if (word(a, j) != word(b, j)) return false;
        return true;
    };
    // (a 4-entry ring of the last distinct vertices tried before the hash was measured ~10 % slower)
    for (size_t i = 0; i < count; ++i) {
        const MeshVertex* const v = verts + i;
        uint32_t h = 2166136261u;
        h = (h ^ word(v, 0)) * 16777619u;
        h = (h ^ word(v, 1)) * 16777619u;
        h = (h ^ word(v, 2)) * 16777619u;
        h = (h ^ word(v, 3)) * 16777619u;
        size_t at = (h ^ (h >> 15)) & (cap - 1);
        int32_t k;
        for (;;) {
            k = table[at];
            if (k < 0 || same(distinct.data() + k, v)) break;
            at = (at + 1) & (cap - 1);
        }
        if (k < 0) {
            if (distinct.size() >= 0x10000u) return false;
            k = int32_t(distinct.size());
            table[at] = k;
            distinct.push_back(*v);
        }
        indices[i] = uint16_t(k);
    }
    return distinct.size() < count;
}

}  // namespace ps2r::render
