// te_probe.h -- the pad-log authoring probe (--trace-player N): the played character's position, chunk, state, the
// camera and the chunk's links, one stderr line every N boundaries and at every chunk change. See te_probe.cpp.
#pragma once
#include <cstdint>
#include <string>

namespace te::probe {

// What the probe reads at a boundary (guest memory, PAL retail globals)
struct State {
    bool valid = false;            // a played character exists
    unsigned game_state = 0;       // GameController state (12 = Playing)
    float x = 0, y = 0, z = 0;     // the character's place
    float vx = 0, vy = 0, vz = 0;  // CreatureAgent::velocity (units per second)
    float face_x = 0, face_z = 0;  // its matrix's z row (where it faces)
    unsigned mode = 0, standing = 0, ground = 0, dead = 0;   // CharacterState
    float cam_x = 0, cam_y = 0, cam_z = 0;   // the chunk matrix's last row: the camera
    float look_x = 0, look_z = 0;            // its z row: where the camera looks (xz)
    uint32_t chunk = 0;
    std::string chunk_path;
    uint32_t clock = 0;            // g_GlobalClock.time (1/9000 s units; +149 a frame at 60 Hz)
    uint32_t frame_time = 0;       // GameTimeController::frameTime (clock units the last frame took: 150 at 60 Hz)
};

bool Read(State& s);                              // false when there is no played character
void Print(uint64_t boundary, const State& s);    // one [probe] line
void Install(unsigned every, const std::string& collision_dir);   // --trace-player N [--dump-collision DIR]
void SampleNow(uint64_t boundary);
void DumpInstances(uint64_t boundary);   // the current chunk's awake instances (names, places, boxes)

// One awake instance of the current chunk whose object name contains NAME (the first in list order): its place and
// collision box this boundary. The hazards' boxes move with them (the swinging log's bob, the skunk, the worms).
struct Instance {
    bool valid = false;
    float x = 0, y = 0, z = 0;
    float min_x = 0, min_y = 0, min_z = 0, max_x = 0, max_y = 0, max_z = 0;
    uint32_t flags = 0;
};
bool FindInstance(const std::string& name, Instance& out);
void DumpTextureTable(uint64_t boundary);
void DumpHulls(const std::string& name, uint64_t boundary);   // the named instance's ObjectCollision and the hulls a rigid body would test (attribution)   // the texture/material tables: ids, references, and which shaders/materials hold each texture
// --trace-bodies N: every N boundaries one [bodies] line per floating body of the physics world (buoyancy > 0; every body with
// TE_TRACE_BODIES_ALL=1): its place, motion, rotation, water level, forces and its instance's box (the icebergs' depth)
void InstallBodies(unsigned every);

}  // namespace te::probe
