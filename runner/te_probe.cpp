// te_probe.cpp -- the pad-log authoring probe: where the played character is, in which chunk, what it stands
// on, where the camera looks, and the chunk's links (the load walls the walk must cross). Read-only over guest memory
// through the retail globals of the PAL image (game/symbol_addrs.txt); nothing here is reachable from the game.
//
// Used by the authoring loop (--autopilot, te_autopilot.cpp) and read by hand: run a script, read the trace, extend
// the script. Not a gate.
#include "te_probe.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "te_host.h"
#include "te_services.h"

#if defined(TE_NATIVE) && defined(__GNUC__)
// the native lane's GuestPointer is the identity on an integer address: GCC's -Warray-bounds takes the pointers it makes
// for objects of size 0 (every read below is behind InRange)
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif

namespace te::probe {

namespace {
// Retail globals (PAL SLES-52568, game/symbol_addrs.txt)
constexpr uint32_t kPlayerCharacter = 0x003098FC;   // G_UnkPlayableCharObjInstCxt: CharacterAgent*
constexpr uint32_t kGameController = 0x00309888;    // G_GameController
constexpr uint32_t kGlobalClock = 0x003238B0;       // G_GlobalClock_ (TimeClock: flags, time, advance)
constexpr uint32_t kGameClockController = 0x00309ACC;   // G_GameClockController: GameTimeController* (lastStamp, frameTime, ...)

// Offsets (game/include/game/*.h CHECK_OFFSETs)
constexpr uint32_t kAgentInstance = 0x00;      // Agent::instance
constexpr uint32_t kCharacterState = 0x70;     // CharacterAgent::state (CharacterState)
constexpr uint32_t kCreatureVelocity = 0x60;   // CreatureAgent::velocity
constexpr uint32_t kObjectPlace = 0x08;        // ReferencedObject::place
constexpr uint32_t kObjectChunk = 0xA0;        // ReferencedObject::chunk
constexpr uint32_t kPlacePosition = 0x40;      // ObjectPlace::position
constexpr uint32_t kPlaceMatrix = 0x00;        // ObjectPlace::matrix
constexpr uint32_t kChunkMatrix = 0x10;        // ChunkData::matrix (last row: the camera, in the world)
constexpr uint32_t kChunkFlags = 0x120;        // ChunkData::flags
constexpr uint32_t kChunkName = 0x12C;         // ChunkData::name (String)
constexpr uint32_t kChunkPath = 0x138;         // ChunkData::path (String)
constexpr uint32_t kChunkLinks = 0x150;        // ChunkData::links
constexpr uint32_t kLinkFlags = 0x00;          // ChunkLinkData::flags
constexpr uint32_t kLinkLinkedData = 0x04;     // ChunkLinkData::linkedData
constexpr uint32_t kLinkNext = 0x08;           // ChunkLinkData::next
constexpr uint32_t kLinkLoadWall = 0x90;       // ChunkLinkData::loadWall
constexpr uint32_t kWallCorners = 0x00;        // LoadWall::corners[4]
constexpr uint32_t kWallPlane = 0x40;          // LoadWall::plane
constexpr uint32_t kChunkCollision = 0x1BC;    // ChunkData::collision (CollisionData*)
constexpr uint32_t kChunkInstances = 0x15C;    // ChunkData::instances (ChunkInstances*)
constexpr uint32_t kInstancesNodes = 0x08;     // ChunkInstances::nodes[24] (GameNode* heads, one list per NodeKind)
constexpr uint32_t kNodeOwner = 0x00;          // GameNode::owner (InstanceContext*)
constexpr uint32_t kNodeNext = 0x10;           // GameNode::next
constexpr uint32_t kObjectNodeObject = 0x78;   // ObjectNodeBase::object (GameObject*)
constexpr uint32_t kGameObjectName = 0x14;     // GameObject::name (String)
constexpr uint32_t kObjectFlags = 0x04;        // ReferencedObject::flags
constexpr uint32_t kObjectBox = 0x10 + 0x30;   // ReferencedObject::collision.box (Box: min, max)
constexpr uint32_t kInstanceId = 0x154;        // InstanceContext::id
constexpr uint32_t kDiskManager = 0x0031B4B8;  // g_DiskManager (D_0031B4B8); handles[] at 0x58, DiskNode::memory at 4

// Guest memory through te::GuestPointer (the wasm lanes' bounds checks; the Vita's native build: the pointer itself), retail
// addresses through RetailAddress (te_host.h)
bool InRange(uint32_t address, uint32_t size) { return address != 0 && GuestPointer(address, size) != nullptr; }
const uint8_t* At(uint32_t address) { return GuestPointer(address, 0); }
uint32_t R(uint32_t retail) { return RetailAddress(retail); }
uint32_t Rd32(uint32_t address) {
    if (!InRange(address, 4)) return 0;
    uint32_t v;
    std::memcpy(&v, At(address), 4);
    return v;
}
float RdF32(uint32_t address) {
    if (!InRange(address, 4)) return 0.f;
    float v;
    std::memcpy(&v, At(address), 4);
    return v;
}
std::string RdString(uint32_t string) {   // game/string.h String { char* string; s32 length; s32 capacity; }
    const uint32_t text = Rd32(string), length = Rd32(string + 4);
    if (!text || length == 0 || length > 256 || !InRange(text, length)) return "";
    return std::string(reinterpret_cast<const char*>(At(text)), length);
}

unsigned g_every = 0;
std::string g_collision_dir;
uint64_t g_last_boundary = 0;
uint32_t g_last_chunk = 0;
std::string g_last_chunk_path;

// The chunk's collision triangles as a Wavefront OBJ (<dir>/<chunk leaf name>.obj), once per chunk: a walkable map for
// pad-log authoring. game/collision.h: CollisionData { version, nodeCount, groupCount, triangleCount,
// vertexCount, nodesHandle, groupsHandle, trianglesHandle, verticesHandle }, the arrays in the disk manager's nodes.
void DumpCollision(uint32_t chunk, const std::string& path) {
    if (g_collision_dir.empty() || !InRange(chunk, 0x1F0)) return;
    const uint32_t col = Rd32(chunk + kChunkCollision);
    if (!InRange(col, 0x28)) return;
    const uint32_t tri_count = Rd32(col + 0xC), vert_count = Rd32(col + 0x10);
    const int32_t tri_h = int32_t(Rd32(col + 0x1C)), vert_h = int32_t(Rd32(col + 0x20));
    if (tri_h < 0 || vert_h < 0 || tri_h >= 5500 || vert_h >= 5500 || tri_count == 0 || tri_count > 400000) return;
    const uint32_t tris = Rd32(Rd32(R(kDiskManager) + 0x58 + 4 * uint32_t(tri_h)) + 4);
    const uint32_t verts = Rd32(Rd32(R(kDiskManager) + 0x58 + 4 * uint32_t(vert_h)) + 4);
    if (!InRange(tris, tri_count * 8) || !InRange(verts, vert_count * 16)) return;
    std::string leaf = path;
    const size_t slash = leaf.find_last_of("\\/");
    if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
    const std::string file = g_collision_dir + "/" + leaf + ".obj";
    if (FILE* probe_exists = std::fopen(file.c_str(), "r")) { std::fclose(probe_exists); return; }
    FILE* f = std::fopen(file.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# TwinsanityEvolved collision dump: %s (%u triangles, %u vertices); faces carry the surface index as a group\n", path.c_str(), tri_count, vert_count);
    for (uint32_t v = 0; v < vert_count; ++v) std::fprintf(f, "v %.4f %.4f %.4f\n", RdF32(verts + v * 16), RdF32(verts + v * 16 + 4), RdF32(verts + v * 16 + 8));
    for (uint32_t t = 0; t < tri_count; ++t) {
        uint64_t w;
        std::memcpy(&w, At(tris + t * 8), 8);
        const uint32_t a = uint32_t(w & 0x3FFFF), b = uint32_t((w >> 18) & 0x3FFFF), c = uint32_t((w >> 36) & 0x3FFFF), surface = uint32_t((w >> 54) & 0x3FF);
        if (a >= vert_count || b >= vert_count || c >= vert_count) continue;
        std::fprintf(f, "f %u %u %u # s%u\n", a + 1, b + 1, c + 1, surface);
    }
    std::fclose(f);
    std::fprintf(stderr, "[probe] collision of %s written: %s (%u triangles)\n", path.c_str(), file.c_str(), tri_count);
}

}  // namespace

// The chunk's awake object instances (ChunkInstances::nodes[NodeObject], then the models' list for the rest): where each is, its
// collision box, its flags and its object's name. The scenery the static collision does not carry: crates, gates, bridges,
// creatures, triggers. An authoring listing (the autopilot's 'instances' step and every chunk change).
void DumpInstances(uint64_t boundary) {
    State s;
    if (!Read(s) || !InRange(s.chunk, 0x1F0)) return;
    const uint32_t insts = Rd32(s.chunk + kChunkInstances);
    if (!InRange(insts, 0x68)) return;
    std::fprintf(stderr, "[probe] @%llu instances of %s:\n", (unsigned long long)boundary, s.chunk_path.c_str());
    std::vector<uint32_t> seen;
    for (unsigned kind : {1u, 3u, 0u, 4u, 5u, 7u, 8u}) {
        unsigned n = 0;
        for (uint32_t node = Rd32(insts + kInstancesNodes + 4 * kind); InRange(node, 0x18) && n < 2000; node = Rd32(node + kNodeNext), ++n) {
            const uint32_t inst = Rd32(node + kNodeOwner);
            if (!InRange(inst, 0x160)) continue;
            bool dup = false;
            for (uint32_t v : seen) if (v == inst) { dup = true; break; }
            if (dup) continue;
            seen.push_back(inst);
            std::string name;
            if (kind == 1) {
                const uint32_t object = Rd32(node + kObjectNodeObject);
                if (InRange(object, 0x20)) name = RdString(object + kGameObjectName);
            } else {
                // the object node of this instance, if any (InstanceContext::nodes.nodes[NodeObject] at 0xD4 + 4 + 4)
                const uint32_t onode = Rd32(inst + 0xD4 + 4 + 4 * 1);
                if (InRange(onode, 0x80)) { const uint32_t object = Rd32(onode + kObjectNodeObject); if (InRange(object, 0x20)) name = RdString(object + kGameObjectName); }
            }
            const uint32_t place = Rd32(inst + kObjectPlace);
            float x = 0, y = 0, z = 0;
            if (InRange(place, 0x70)) {
                uint64_t bits; std::memcpy(&bits, At(place + 0x60), 8);
                const uint32_t at = (bits & 0x4) ? place + kPlaceMatrix + 0x30 : place + kPlacePosition;
                x = RdF32(at); y = RdF32(at + 4); z = RdF32(at + 8);
            }
            const uint32_t flags = Rd32(inst + kObjectFlags);
            std::fprintf(stderr, "[probe]   inst 0x%08X id %d k%u '%s' at (%.1f %.1f %.1f) box (%.1f %.1f %.1f)-(%.1f %.1f %.1f) flags 0x%05X%s%s%s\n", inst,
                         int32_t(Rd32(inst + kInstanceId)), kind, name.c_str(), x, y, z, RdF32(inst + kObjectBox), RdF32(inst + kObjectBox + 4), RdF32(inst + kObjectBox + 8),
                         RdF32(inst + kObjectBox + 16), RdF32(inst + kObjectBox + 20), RdF32(inst + kObjectBox + 24), flags,
                         (flags & 0x10) ? " solid" : "", (flags & 0x80000) ? " solidModel" : "", (flags & 0x40000) ? " dynScenery" : "");
        }
    }
    // the sleeping instances (InstanceContext::next chain from ChunkInstances::sleeping)
    unsigned asleep = 0;
    for (uint32_t inst = Rd32(insts); InRange(inst, 0x160) && asleep < 2000; inst = Rd32(inst + 0x144), ++asleep) {
        std::string name;
        const uint32_t onode = Rd32(inst + 0xD4 + 4 + 4 * 1);
        if (InRange(onode, 0x80)) { const uint32_t object = Rd32(onode + kObjectNodeObject); if (InRange(object, 0x20)) name = RdString(object + kGameObjectName); }
        const uint32_t place = Rd32(inst + kObjectPlace);
        float x = 0, y = 0, z = 0;
        if (InRange(place, 0x70)) {
            uint64_t bits; std::memcpy(&bits, At(place + 0x60), 8);
            const uint32_t at = (bits & 0x4) ? place + kPlaceMatrix + 0x30 : place + kPlacePosition;
            x = RdF32(at); y = RdF32(at + 4); z = RdF32(at + 8);
        }
        std::fprintf(stderr, "[probe]   asleep 0x%08X '%s' at (%.1f %.1f %.1f) box (%.1f %.1f %.1f)-(%.1f %.1f %.1f) flags 0x%05X nodes 0x%X\n", inst, name.c_str(), x, y, z,
                     RdF32(inst + kObjectBox), RdF32(inst + kObjectBox + 4), RdF32(inst + kObjectBox + 8), RdF32(inst + kObjectBox + 16), RdF32(inst + kObjectBox + 20),
                     RdF32(inst + kObjectBox + 24), Rd32(inst + kObjectFlags), Rd32(inst + 0xD4));
    }
    std::fprintf(stderr, "[probe]   %zu instances awake, %u asleep; list lengths by kind:", seen.size(), asleep);
    for (unsigned kind = 0; kind < 24; ++kind) {
        unsigned n = 0;
        for (uint32_t node = Rd32(insts + kInstancesNodes + 4 * kind); InRange(node, 0x18) && n < 5000; node = Rd32(node + kNodeNext)) ++n;
        if (n) std::fprintf(stderr, " k%u=%u", kind, n);
    }
    const uint32_t dyn = Rd32(s.chunk + 0x1E4);
    std::fprintf(stderr, "; dynamicScenery 0x%08X models %u\n", dyn, InRange(dyn, 0x14) ? Rd32(dyn + 0xC) & 0xFFFF : 0);
}

// The named instance's collision as the rigid bodies see it (SphereBody::TouchHulls): ObjectCollision (0x10 into the object:
// hull / givenHull / OGI, cells and cell, hullMatrix, bits, world box, own box, joint matrices) and every hull it would hand out
// (GameOGI::hulls + hullJoints: counts, surface, the vertices and planes of the blob). The attribution step for an instance a
// body passes through (the cryscave trapdoor): the same words are read out of the oracle's memory capture.
void DumpHulls(const std::string& name_in, uint64_t boundary) {
    State s;
    if (!Read(s) || !InRange(s.chunk, 0x1F0)) return;
    const uint32_t insts = Rd32(s.chunk + kChunkInstances);
    if (!InRange(insts, 0x68)) return;
    unsigned n = 0, found = 0;
    for (uint32_t node = Rd32(insts + kInstancesNodes + 4 * 1); InRange(node, 0x18) && n < 2000; node = Rd32(node + kNodeNext), ++n) {
        const uint32_t inst = Rd32(node + kNodeOwner);
        if (!InRange(inst, 0x160)) continue;
        const uint32_t object = Rd32(node + kObjectNodeObject);
        if (!InRange(object, 0x20)) continue;
        const std::string name = RdString(object + kGameObjectName);
        if (name.find(name_in) == std::string::npos) continue;
        ++found;
        const uint32_t col = inst + 0x10;
        const uint32_t hull = Rd32(col + 0x04), given = Rd32(col + 0x08), cells = Rd32(col + 0x10), hullMatrix = Rd32(col + 0x1C), ogi = Rd32(col + 0x70);
        const int32_t cell = int32_t(Rd32(col + 0x14));
        std::fprintf(stderr, "[probe] @%llu hulls of '%s' inst 0x%08X flags 0x%05X: hull 0x%08X given 0x%08X sceneryCell 0x%08X cells 0x%08X cell %d leftOut 0x%08X hullMatrix 0x%08X bits 0x%08X ogi 0x%08X jointMatrices 0x%08X count %d surfaces 0x%08X\n",
                     (unsigned long long)boundary, name.c_str(), inst, Rd32(inst + kObjectFlags), hull, given, Rd32(col + 0x0C), cells, cell, Rd32(col + 0x18), hullMatrix,
                     Rd32(col + 0x20), ogi, Rd32(col + 0x74), int32_t(Rd32(col + 0x78)), Rd32(col + 0x80));
        std::fprintf(stderr, "[probe]   box (%.3f %.3f %.3f)-(%.3f %.3f %.3f) ownBox (%.3f %.3f %.3f)-(%.3f %.3f %.3f)\n", RdF32(col + 0x30), RdF32(col + 0x34), RdF32(col + 0x38),
                     RdF32(col + 0x40), RdF32(col + 0x44), RdF32(col + 0x48), RdF32(col + 0x50), RdF32(col + 0x54), RdF32(col + 0x58), RdF32(col + 0x60), RdF32(col + 0x64), RdF32(col + 0x68));
        const uint32_t place = Rd32(inst + kObjectPlace);
        if (InRange(place, 0x70)) {
            for (unsigned r = 0; r < 4; ++r)
                std::fprintf(stderr, "[probe]   place.matrix[%u] (%.4f %.4f %.4f %.4f)%s\n", r, RdF32(place + 16 * r), RdF32(place + 16 * r + 4), RdF32(place + 16 * r + 8), RdF32(place + 16 * r + 12),
                             r == 3 ? "" : "");
            std::fprintf(stderr, "[probe]   place.position (%.4f %.4f %.4f) bits 0x%08X\n", RdF32(place + kPlacePosition), RdF32(place + kPlacePosition + 4), RdF32(place + kPlacePosition + 8), Rd32(place + 0x60));
        }
        // the hulls: one of its own / given, else the OGI's
        uint32_t count = 0, hulls = 0, joints = 0;
        if (hull || given) { count = 1; hulls = hull ? hull : given; }
        else if (InRange(ogi, 0x90)) { count = Rd32(ogi + 0x48) & 0xFF; hulls = Rd32(ogi + 0x84); joints = Rd32(ogi + 0x88); }
        if (InRange(ogi, 0x90))
            std::fprintf(stderr, "[probe]   ogi 0x%08X id 0x%08X '%s' joints %u hulls %u hullJoints 0x%08X bounds (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", ogi, Rd32(ogi + 4), RdString(ogi + 0x50).c_str(),
                         Rd32(ogi + 0x40) & 0xFF, count, joints, RdF32(ogi + 0x20), RdF32(ogi + 0x24), RdF32(ogi + 0x28), RdF32(ogi + 0x30), RdF32(ogi + 0x34), RdF32(ogi + 0x38));
        for (uint32_t h = 0; h < count && h < 16; ++h) {
            const uint32_t H = hulls + 0x20 * h;
            if (!InRange(H, 0x20)) break;
            const uint32_t vc = Rd32(H) & 0xFFFF, ec = Rd32(H) >> 16, pc = Rd32(H + 4) & 0xFFFF, planesOff = Rd32(H + 8) >> 16, blob = Rd32(H + 0x1C);
            std::fprintf(stderr, "[probe]   hull %u @0x%08X joint %d: vertices %u edges %u planes %u planesOffset 0x%X surface %d blobSize %u blob 0x%08X\n", h, H,
                         joints ? int(int8_t(Rd32(joints + (h & ~3u)) >> (8 * (h & 3)))) : -1, vc, ec, pc, planesOff, int16_t(Rd32(H + 0x14) >> 16), Rd32(H + 0x18), blob);
            for (uint32_t v = 0; v < vc && v < 12 && InRange(blob + 16 * v, 16); ++v)
                std::fprintf(stderr, "[probe]     v%u (%.3f %.3f %.3f %.3f)\n", v, RdF32(blob + 16 * v), RdF32(blob + 16 * v + 4), RdF32(blob + 16 * v + 8), RdF32(blob + 16 * v + 12));
            for (uint32_t p = 0; p < pc && p < 8 && InRange(blob + planesOff + 16 * p, 16); ++p)
                std::fprintf(stderr, "[probe]     plane%u (%.4f %.4f %.4f %.4f)\n", p, RdF32(blob + planesOff + 16 * p), RdF32(blob + planesOff + 16 * p + 4), RdF32(blob + planesOff + 16 * p + 8), RdF32(blob + planesOff + 16 * p + 12));
        }
        const uint32_t jm = Rd32(col + 0x74);
        const int32_t jmc = int32_t(Rd32(col + 0x78));
        for (int32_t j = 0; j < jmc && j < 8 && InRange(jm + 0x40 * j, 0x40); ++j)
            std::fprintf(stderr, "[probe]   jointMatrix %d rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f)\n", j, RdF32(jm + 0x40 * j), RdF32(jm + 0x40 * j + 4), RdF32(jm + 0x40 * j + 8),
                         RdF32(jm + 0x40 * j + 0x10), RdF32(jm + 0x40 * j + 0x14), RdF32(jm + 0x40 * j + 0x18), RdF32(jm + 0x40 * j + 0x20), RdF32(jm + 0x40 * j + 0x24), RdF32(jm + 0x40 * j + 0x28),
                         RdF32(jm + 0x40 * j + 0x30), RdF32(jm + 0x40 * j + 0x34), RdF32(jm + 0x40 * j + 0x38));
        // the live joint transforms a body's hull test uses (HullJointMatrix: the model node's animator first): the ModelNode is
        // InstanceContext::nodes.nodes[NodeModel]; its animator's joint matrices are the OGI joints' (OgiAnimator::jointMatrices)
        const uint32_t model = Rd32(inst + 0xD4 + 4 + 4 * 3);                    // ModelNode (GameNode 0x18, then bits, ogi, drawnOgi, animator)
        const uint32_t animator = InRange(model, 0x30) ? Rd32(model + 0x24) : 0;
        const uint32_t jointsArray = InRange(animator, 0x40) ? Rd32(animator + 0x18) : 0;   // OgiAnimator::joints (SizedArray<JointAnimation*>*)
        std::fprintf(stderr, "[probe]   modelNode 0x%08X animator 0x%08X joints 0x%08X\n", model, animator, jointsArray);
        if (InRange(jointsArray, 8)) {
            const uint32_t data = Rd32(jointsArray), size = Rd32(jointsArray + 4);
            for (uint32_t j = 0; j < size && j < 8; ++j) {
                const uint32_t ja = Rd32(data + 4 * j);
                if (!InRange(ja, 0x70)) { std::fprintf(stderr, "[probe]   live joint %u: none\n", j); continue; }
                std::fprintf(stderr, "[probe]   live joint %u transform rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f)\n", j, RdF32(ja), RdF32(ja + 4), RdF32(ja + 8),
                             RdF32(ja + 0x10), RdF32(ja + 0x14), RdF32(ja + 0x18), RdF32(ja + 0x20), RdF32(ja + 0x24), RdF32(ja + 0x28), RdF32(ja + 0x30), RdF32(ja + 0x34), RdF32(ja + 0x38));
            }
        }
    }
    if (!found) std::fprintf(stderr, "[probe] @%llu hulls: no awake instance named '%s'\n", (unsigned long long)boundary, name_in.c_str());
}

bool FindInstance(const std::string& name_in, Instance& out) {
    out = Instance{};
    State s;
    if (!Read(s) || !InRange(s.chunk, 0x1F0)) return false;
    const uint32_t insts = Rd32(s.chunk + kChunkInstances);
    if (!InRange(insts, 0x68)) return false;
    // NAME@ = of the instances whose name contains NAME, the one nearest the character (xz); else the first in list order
    std::string name = name_in;
    const bool nearest = !name.empty() && name.back() == '@';
    if (nearest) name.pop_back();
    float best = 1e30f;
    // the object nodes (kind 1) carry the names; the creatures and actors are all there
    unsigned n = 0;
    for (uint32_t node = Rd32(insts + kInstancesNodes + 4 * 1); InRange(node, 0x18) && n < 2000; node = Rd32(node + kNodeNext), ++n) {
        const uint32_t inst = Rd32(node + kNodeOwner);
        if (!InRange(inst, 0x160)) continue;
        const uint32_t object = Rd32(node + kObjectNodeObject);
        if (!InRange(object, 0x20) || RdString(object + kGameObjectName).find(name) == std::string::npos) continue;
        Instance i;
        const uint32_t place = Rd32(inst + kObjectPlace);
        if (InRange(place, 0x70)) {
            uint64_t bits; std::memcpy(&bits, At(place + 0x60), 8);
            const uint32_t at = (bits & 0x4) ? place + kPlaceMatrix + 0x30 : place + kPlacePosition;
            i.x = RdF32(at); i.y = RdF32(at + 4); i.z = RdF32(at + 8);
        }
        i.min_x = RdF32(inst + kObjectBox); i.min_y = RdF32(inst + kObjectBox + 4); i.min_z = RdF32(inst + kObjectBox + 8);
        i.max_x = RdF32(inst + kObjectBox + 16); i.max_y = RdF32(inst + kObjectBox + 20); i.max_z = RdF32(inst + kObjectBox + 24);
        i.flags = Rd32(inst + kObjectFlags);
        i.valid = true;
        const float dx = i.x - s.x, dz = i.z - s.z, d = dx * dx + dz * dz;
        if (!nearest) { out = i; return true; }
        if (d < best) { best = d; out = i; }
    }
    return out.valid;
}

// The graphics tables (game/graphicstables.h GraphicsTable: entries, count) for textures and materials, with each resource's
// references (ResourceHeader bits 0-15) and kept bit (17), and for every texture the te_gfx_shader records (platform/nativeee/
// te_gfx_layout.h: magic 'TESH', texture at +0x14) in guest memory that point at it, each with the material ('TEMT') owning it.
// A diagnostic for a resource held across a chunk change (the bossarea OLEG picture: a texture the table still had).
void DumpTextureTable(uint64_t boundary) {
    constexpr uint32_t kTextureTable = 0x003B05C0, kMaterialTable = 0x003B0588;
    auto dump = [&](const char* what, uint32_t table) {
        const uint32_t entries = Rd32(table);
        const uint32_t count = Rd32(table + 4) & 0xFFFF;
        std::fprintf(stderr, "[probe] @%llu %s table: %u entries\n", (unsigned long long)boundary, what, count);
        for (uint32_t i = 0; i < count && i < 512; ++i) {
            const uint32_t item = Rd32(entries + i * 8), id = Rd32(entries + i * 8 + 4);
            const uint32_t bits = Rd32(item);
            std::fprintf(stderr, "[probe]   %s id 0x%08X item 0x%08X refs %u kept %u\n", what, id, item, bits & 0xFFFF, (bits >> 17) & 1);
        }
    };
    dump("texture", R(kTextureTable));
    dump("material", R(kMaterialTable));
    // the shaders in guest memory and their textures (a linear scan of the heap region on 16-byte boundaries)
    const uint8_t* mem = GuestMemory();
    const uint32_t size = uint32_t(GuestMemorySize());
    unsigned shaders = 0;
    for (uint32_t a = 0x00400000; a + 0x40 <= size; a += 16) {
        uint32_t magic; std::memcpy(&magic, mem + a, 4);
        if (magic != 0x48534554u) continue;
        const uint32_t texture = Rd32(R(a) + 0x14), texture_id = Rd32(R(a) + 0x10);
        if (!texture && !texture_id) continue;
        ++shaders;
        // the material owning this shader
        uint32_t owner = 0;
        for (uint32_t m = 0x00400000; m + 0x40 <= size && !owner; m += 16) {
            uint32_t mm; std::memcpy(&mm, mem + m, 4);
            if (mm != 0x544D4554u) continue;
            for (unsigned k = 0; k < 4; ++k) if (Rd32(R(m) + 0x10 + 4 * k) == R(a)) { owner = m; break; }
        }
        std::fprintf(stderr, "[probe]   shader 0x%08X type %u texture id 0x%08X item 0x%08X (refs %u) owned by material 0x%08X (kind %u, bucket %u)\n", a, Rd32(R(a) + 4),
                     texture_id, texture, texture ? Rd32(texture) & 0xFFFF : 0, owner, owner ? Rd32(R(owner) + 4) : 0, owner ? Rd32(R(owner) + 8) : 0);
    }
    std::fprintf(stderr, "[probe]   %u textured shaders\n", shaders);
}

namespace {
void DumpLinks(uint32_t chunk) {
    unsigned n = 0;
    for (uint32_t link = Rd32(chunk + kChunkLinks); link && n < 32; link = Rd32(link + kLinkNext), ++n) {
        const uint32_t linked = Rd32(link + kLinkLinkedData);
        const uint32_t flags = Rd32(link + kLinkFlags);
        const uint32_t wall = Rd32(link + kLinkLoadWall);
        std::string path = linked ? RdString(linked + kChunkPath) : "";
        if (path.empty() && linked) path = RdString(linked + kChunkName);
        if (linked && !path.empty()) DumpCollision(linked, path);
        std::fprintf(stderr, "[probe]   link %u -> %s (flags 0x%08X, linked 0x%08X)", n, path.empty() ? "?" : path.c_str(), flags, linked);
        if (wall && InRange(wall, 0x90)) {
            float cx = 0, cy = 0, cz = 0;
            for (unsigned c = 0; c < 4; ++c) {
                cx += RdF32(wall + kWallCorners + c * 16 + 0);
                cy += RdF32(wall + kWallCorners + c * 16 + 4);
                cz += RdF32(wall + kWallCorners + c * 16 + 8);
            }
            std::fprintf(stderr, " wall centre (%.1f %.1f %.1f) plane (%.2f %.2f %.2f %.1f) corners", cx / 4, cy / 4, cz / 4,
                         RdF32(wall + kWallPlane), RdF32(wall + kWallPlane + 4), RdF32(wall + kWallPlane + 8), RdF32(wall + kWallPlane + 12));
            for (unsigned c = 0; c < 4; ++c)
                std::fprintf(stderr, " (%.1f %.1f %.1f)", RdF32(wall + kWallCorners + c * 16), RdF32(wall + kWallCorners + c * 16 + 4),
                             RdF32(wall + kWallCorners + c * 16 + 8));
        }
        std::fprintf(stderr, "\n");
    }
}
}  // namespace

bool Read(State& s) {
    s = State{};
    const uint32_t gc = Rd32(R(kGameController));
    if (InRange(gc, 16)) { uint64_t v; std::memcpy(&v, At(gc + 8), 8); s.game_state = unsigned((v >> 44) & 63); }
    s.clock = Rd32(R(kGlobalClock) + 4);
    const uint32_t tc = Rd32(R(kGameClockController));
    if (InRange(tc, 8)) s.frame_time = Rd32(tc + 4);
    const uint32_t agent = Rd32(R(kPlayerCharacter));
    if (!InRange(agent, 0x110)) return false;
    const uint32_t instance = Rd32(agent + kAgentInstance);
    if (!InRange(instance, 0x160)) return false;
    s.valid = true;
    const uint32_t place = Rd32(instance + kObjectPlace);
    s.chunk = Rd32(instance + kObjectChunk);
    s.chunk_path = s.chunk ? RdString(s.chunk + kChunkPath) : "";
    if (InRange(place, 0x70)) {
        uint64_t bits;
        std::memcpy(&bits, At(place + 0x60), 8);
        const uint32_t at = (bits & 0x4) ? place + kPlaceMatrix + 0x30 : place + kPlacePosition;   // place.h SyncPosition
        s.x = RdF32(at); s.y = RdF32(at + 4); s.z = RdF32(at + 8);
        s.face_x = RdF32(place + kPlaceMatrix + 0x20); s.face_z = RdF32(place + kPlaceMatrix + 0x28);
    }
    const uint32_t state = Rd32(agent + kCharacterState);
    s.mode = (state >> 4) & 15; s.standing = (state >> 8) & 15; s.ground = (state >> 12) & 1; s.dead = (state >> 14) & 1;
    s.vx = RdF32(agent + kCreatureVelocity); s.vy = RdF32(agent + kCreatureVelocity + 4); s.vz = RdF32(agent + kCreatureVelocity + 8);
    if (InRange(s.chunk, 0x1F0)) {
        s.cam_x = RdF32(s.chunk + kChunkMatrix + 0x30); s.cam_y = RdF32(s.chunk + kChunkMatrix + 0x34); s.cam_z = RdF32(s.chunk + kChunkMatrix + 0x38);
        s.look_x = RdF32(s.chunk + kChunkMatrix + 0x20); s.look_z = RdF32(s.chunk + kChunkMatrix + 0x28);
    }
    return true;
}

void Print(uint64_t boundary, const State& s) {
    std::fprintf(stderr, "[probe] @%llu pos (%.3f %.3f %.3f) vel (%.3f %.3f %.3f) face (%.2f %.2f) mode %u standing %u ground %u dead %u "
                         "cam (%.1f %.1f %.1f) look (%.2f %.2f) gstate %u chunk %s clock %u frame_time %u\n",
                 (unsigned long long)boundary, s.x, s.y, s.z, s.vx, s.vy, s.vz, s.face_x, s.face_z, s.mode, s.standing, s.ground, s.dead,
                 s.cam_x, s.cam_y, s.cam_z, s.look_x, s.look_z, s.game_state, s.chunk_path.empty() ? "-" : s.chunk_path.c_str(),
                 s.clock, s.frame_time);
}

namespace {
void Sample(uint64_t boundary, bool force) {
    State s;
    if (!Read(s)) {
        if (force) std::fprintf(stderr, "[probe] @%llu no player character (gstate %u)\n", (unsigned long long)boundary, s.game_state);
        return;
    }
    const bool chunk_changed = s.chunk != g_last_chunk || s.chunk_path != g_last_chunk_path;
    if (!force && !chunk_changed && (g_every == 0 || boundary - g_last_boundary < g_every)) return;
    g_last_boundary = boundary;
    Print(boundary, s);
    if (chunk_changed) {
        g_last_chunk = s.chunk;
        g_last_chunk_path = s.chunk_path;
        if (s.chunk) {
            const uint32_t flags = Rd32(s.chunk + kChunkFlags);
            std::fprintf(stderr, "[probe] @%llu chunk 0x%08X '%s' (name '%s', state %u) links:\n", (unsigned long long)boundary, s.chunk,
                         s.chunk_path.c_str(), RdString(s.chunk + kChunkName).c_str(), (flags >> 18) & 15);
            DumpCollision(s.chunk, s.chunk_path);
            DumpLinks(s.chunk);
            DumpInstances(boundary);
        }
    }
}
}  // namespace

void Install(unsigned every, const std::string& collision_dir) {
    g_every = every;
    g_collision_dir = collision_dir;
    Services::Get().clock().AddBoundaryHook("trace-player", [](uint64_t boundary) { Sample(boundary, false); });
    std::fprintf(stderr, "[probe] tracing the played character every %u boundaries (and at every chunk change)\n", every);
}

void SampleNow(uint64_t boundary) { Sample(boundary, true); }

// ---- the rigid bodies (--trace-bodies N): the physics world's bodies, their place, motion and what floats them ----------------
namespace {
// game/include/game/rigidbody.h (CHECK_OFFSETs): PhysicsWorld { bodies, capacity, freeSlots, freeCapacity, used }, the body
// (RigidBody 0x2E0, DynamicBody 0x380, HullBody / SphereBody after it)
constexpr uint32_t kPhysicsWorld = 0x00309AD4;   // g_PhysicsWorld (UnkStruct_0x14_HasSomeDataPTRs): PhysicsWorld*
constexpr uint32_t kHullBodyVTable = 0x00305440, kSphereBodyVTable = 0x003054D8;
constexpr uint32_t kNodeVTable = 0x14;           // GameNode's vtable
constexpr uint32_t kBodyFlags = 0x18, kBodyMass = 0x1C, kBodySubsteps = 0x64, kBodyMomentum = 0x70, kBodyRotation = 0x80;
constexpr uint32_t kBodyMatrix = 0xB0, kBodyAngularVelocity = 0x140, kBodyVelocity = 0x150, kBodyForce = 0x1C0;
constexpr uint32_t kBodyStepForce = 0x1E0, kBodyStepTorque = 0x1F0, kBodyWaterLevel = 0x210;
constexpr uint32_t kDynBits = 0x2E0, kDynInstance = 0x2E4, kDynDrag = 0x31C, kDynLengthDrag = 0x320, kDynBuoyancy = 0x324;
constexpr uint32_t kDynPushes = 0x350;
unsigned g_bodies_every = 0;
bool g_bodies_all = false;   // TE_TRACE_BODIES_ALL=1: every body, not only the floating ones (buoyancy > 0)
uint64_t g_bodies_last = 0;

uint32_t Bits(float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; }

void SampleBodies(uint64_t boundary) {
    if (g_bodies_every == 0 || (boundary - g_bodies_last < g_bodies_every && g_bodies_last != 0)) return;
    g_bodies_last = boundary;
    const uint32_t world = Rd32(R(kPhysicsWorld));
    if (!InRange(world, 0x14)) { std::fprintf(stderr, "[bodies] @%llu no physics world (0x%08X)\n", (unsigned long long)boundary, world); return; }
    const uint32_t bodies = Rd32(world);
    const int32_t capacity = int32_t(Rd32(world + 4));
    if (!InRange(bodies, 4 * 200) || capacity <= 0 || capacity > 200) {
        std::fprintf(stderr, "[bodies] @%llu physics world 0x%08X: bodies 0x%08X capacity %d used %d\n", (unsigned long long)boundary, world, bodies, capacity, int32_t(Rd32(world + 16)));
        return;
    }
    unsigned listed = 0;
    for (int32_t slot = 0; slot < capacity; ++slot) {
        const uint32_t body = Rd32(bodies + 4 * uint32_t(slot));
        if (!InRange(body, 0x3A0)) continue;
        ++listed;
        const float buoyancy = RdF32(body + kDynBuoyancy);
        if (!g_bodies_all && !(buoyancy > 0.f)) continue;
        const uint32_t vt = Rd32(body + kNodeVTable);
        const char* kind = vt == R(kHullBodyVTable) ? "hull" : vt == R(kSphereBodyVTable) ? "sphere" : "body";
        const uint32_t inst = Rd32(body + kDynInstance);
        std::string name;
        uint32_t iflags = 0;
        float box_min_y = 0, box_max_y = 0, place_y = 0;
        if (InRange(inst, 0x160)) {
            iflags = Rd32(inst + kObjectFlags);
            const uint32_t onode = Rd32(inst + 0xD4 + 4 + 4 * 1);
            if (InRange(onode, 0x80)) { const uint32_t object = Rd32(onode + kObjectNodeObject); if (InRange(object, 0x20)) name = RdString(object + kGameObjectName); }
            box_min_y = RdF32(inst + kObjectBox + 4);
            box_max_y = RdF32(inst + kObjectBox + 20);
            const uint32_t place = Rd32(inst + kObjectPlace);
            if (InRange(place, 0x70)) {
                uint64_t pbits; std::memcpy(&pbits, At(place + 0x60), 8);
                place_y = RdF32(((pbits & 0x4) ? place + kPlaceMatrix + 0x30 : place + kPlacePosition) + 4);
            }
        }
        const uint32_t p = body + kBodyMatrix + 0x30, v = body + kBodyVelocity, w = body + kBodyAngularVelocity;
        const uint32_t sf = body + kBodyStepForce, st = body + kBodyStepTorque;
        std::fprintf(stderr, "[bodies] @%llu slot %d 0x%08X %s '%s' inst 0x%08X%s pos (%.5f %.5f %.5f) y %08X vel (%.5f %.5f %.5f) spin (%.5f %.5f %.5f) "
                             "rot (%.5f %.5f %.5f %.5f) water %.5f buoy %.4f mass %.3f drag %.4f/%.4f sub %d step (%.4f %.4f %.4f | %.4f %.4f %.4f) force (%.3f %.3f %.3f) "
                             "box y %.4f..%.4f place y %.5f flags 0x%X bits 0x%X push (%.5f %.5f %.5f)\n",
                     (unsigned long long)boundary, slot, body, kind, name.c_str(), inst, (iflags & 1) ? " asleep" : "", RdF32(p), RdF32(p + 4), RdF32(p + 8), Bits(RdF32(p + 4)),
                     RdF32(v), RdF32(v + 4), RdF32(v + 8), RdF32(w), RdF32(w + 4), RdF32(w + 8), RdF32(body + kBodyRotation), RdF32(body + kBodyRotation + 4),
                     RdF32(body + kBodyRotation + 8), RdF32(body + kBodyRotation + 12), RdF32(body + kBodyWaterLevel), buoyancy, RdF32(body + kBodyMass),
                     RdF32(body + kDynDrag), RdF32(body + kDynLengthDrag), int32_t(Rd32(body + kBodySubsteps)), RdF32(sf), RdF32(sf + 4), RdF32(sf + 8), RdF32(st), RdF32(st + 4),
                     RdF32(st + 8), RdF32(body + kBodyForce), RdF32(body + kBodyForce + 4), RdF32(body + kBodyForce + 8), box_min_y, box_max_y, place_y, Rd32(body + kBodyFlags),
                     Rd32(body + kDynBits), RdF32(body + kDynPushes), RdF32(body + kDynPushes + 4), RdF32(body + kDynPushes + 8));
        (void)kBodyMomentum;
    }
    if (g_bodies_all) std::fprintf(stderr, "[bodies] @%llu world 0x%08X: %u bodies in %d slots (used %d)\n", (unsigned long long)boundary, world, listed, capacity, int32_t(Rd32(world + 16)));
    // TE_TRACE_INSTANCES=NAME: the current chunk's awake instances whose object name contains NAME (with or without a body):
    // where each is placed (position and rotation) and its collision box
    static const char* watch = std::getenv("TE_TRACE_INSTANCES");
    if (!watch || !*watch) return;
    State s;
    if (!Read(s) || !InRange(s.chunk, 0x1F0)) return;
    const uint32_t insts = Rd32(s.chunk + kChunkInstances);
    if (!InRange(insts, 0x68)) return;
    unsigned n = 0;
    for (uint32_t node = Rd32(insts + kInstancesNodes + 4 * 1); InRange(node, 0x18) && n < 2000; node = Rd32(node + kNodeNext), ++n) {
        const uint32_t inst = Rd32(node + kNodeOwner);
        const uint32_t object = Rd32(node + kObjectNodeObject);
        if (!InRange(inst, 0x160) || !InRange(object, 0x20)) continue;
        const std::string name = RdString(object + kGameObjectName);
        if (name.find(watch) == std::string::npos) continue;
        const uint32_t place = Rd32(inst + kObjectPlace);
        if (!InRange(place, 0x70)) continue;
        uint64_t pbits; std::memcpy(&pbits, At(place + 0x60), 8);
        const uint32_t at = (pbits & 0x4) ? place + kPlaceMatrix + 0x30 : place + kPlacePosition;
        // the instance's rigid body node (NodeRigidBody 5) and its physics body
        const uint32_t rnode = Rd32(inst + 0xD4 + 4 + 4 * 5);
        std::fprintf(stderr, "[inst] @%llu 0x%08X '%s' pos (%.5f %.5f %.5f) y %08X up (%.4f %.4f %.4f) box y %.4f..%.4f flags 0x%05X rigid 0x%08X\n",
                     (unsigned long long)boundary, inst, name.c_str(), RdF32(at), RdF32(at + 4), RdF32(at + 8), Bits(RdF32(at + 4)),
                     RdF32(place + kPlaceMatrix + 0x10), RdF32(place + kPlaceMatrix + 0x14), RdF32(place + kPlaceMatrix + 0x18),
                     RdF32(inst + kObjectBox + 4), RdF32(inst + kObjectBox + 20), Rd32(inst + kObjectFlags), rnode);
    }
}
}  // namespace

void InstallBodies(unsigned every) {
    g_bodies_every = every ? every : 1;
    const char* all = std::getenv("TE_TRACE_BODIES_ALL");
    g_bodies_all = all && all[0] == '1';
    Services::Get().clock().AddBoundaryHook("trace-bodies", [](uint64_t boundary) {
        SampleBodies(boundary);
        // TE_PROBE_DUMP=0xADDR:WORDS@BOUNDARY[,...]: guest words printed once at a boundary (inspection of tables)
        static const char* dump = std::getenv("TE_PROBE_DUMP");
        if (!dump) return;
        for (const char* p = dump; *p;) {
            char* end = nullptr;
            const uint32_t addr = uint32_t(std::strtoul(p, &end, 0));
            if (!end || *end != ':') break;
            const uint32_t words = uint32_t(std::strtoul(end + 1, &end, 0));
            if (!end || *end != '@') break;
            const uint64_t at = std::strtoull(end + 1, &end, 0);
            if (at == boundary)
                for (uint32_t w = 0; w < words; w += 4) {
                    std::fprintf(stderr, "[probe.dump] @%llu 0x%08X:", (unsigned long long)boundary, addr + 4 * w);
                    for (uint32_t k = w; k < w + 4 && k < words; ++k) std::fprintf(stderr, " %08X", Rd32(addr + 4 * k));
                    std::fprintf(stderr, "\n");
                }
            p = (end && *end == ',') ? end + 1 : end ? end : p + std::strlen(p);
            if (!*p) break;
        }
    });
    std::fprintf(stderr, "[probe] tracing the physics world's %s bodies every %u boundaries\n", g_bodies_all ? "" : "floating", g_bodies_every);
}

}  // namespace te::probe
