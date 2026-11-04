// te_autopilot.cpp -- assisted play for pad-log authoring. A plan of steps (goto X Z, jump, hold,
// wait, ...) is driven CLOSED-LOOP from the probe: every boundary the pad sample is computed from where the played
// character is and where the camera looks, so the stick points at the target in the world. With --record-pad the
// samples the game saw are written as a plain pad log (inputs only, fixtures/padlogs format) that replays
// deterministically with --play-pad: the delivered fixture is the recording, never the plan.
//
// Plan grammar (one step per line, '#' comments):
//   goto X Z [RADIUS] [MAX_FRAMES]     steer to (X, Z) in the CURRENT CHUNK's frame until within RADIUS (default 1.5); a stuck
//                                      detector jumps; a chunk change ends the step (each chunk has its own coordinate frame)
//   gotoj X Z [RADIUS] [MAX_FRAMES]    the same, jumping continuously (gaps, slopes)
//   dash X Z N [J1 R J2 D|@EX,EZ [PRE]] steer at (X, Z) for N frames with a double jump after D frames of running (0) or, with @EX,EZ, at
//                                      the frame the character reaches the edge point (EX, EZ) (within 0.6, or past it): CROSS J1 frames (8),
//                                      released R (6), CROSS J2 (8) -- the pit crossings; after the jump the stick stays on the target;
//                                      PRE = buttons held over the 8 frames before the first jump (CIRCLE: a slide jump)
//   jump                               CROSS held 10 frames
//   spin                               SQUARE held 6 frames
//   hold BUTTONS N [LX LY RX RY]       a raw hold of N frames (buttons as in a pad log: CROSS+START, hex, NONE)
//   look DX DZ [TOL] [N] [MAX_FRAMES]  turn the camera (the right stick) until it looks along the world direction (DX, DZ) within TOL
//                                      degrees (default 4), then keep it there N frames (default 0), the stick pushed back whenever the
//                                      camera drifts out (its own recentering): a held view that replays alike where the camera's
//                                      response differs a little (the Vita)
//   wait N                             the neutral pad N frames (of PLAY: paused outside Playing like every movement step)
//   idle N                             the neutral pad N frames in ANY state (the menus: a dialog's settle time)
//   at N                               the neutral pad until boundary N (absolute: the menu prefix of a fixture)
//   until-chunk NAME [MAX_FRAMES]      the neutral pad until the character is in a chunk whose path contains NAME
//   until-gstate N [MAX_FRAMES]        the neutral pad until GameController::State() == N
//   instances                          list the current chunk's instances (probe) and continue
//   tables                             list the texture/material tables and the shaders holding each texture (probe)
//   until-inst NAME FIELD OP VALUE [MAX_FRAMES]  the neutral pad until the awake instance whose name contains NAME has FIELD OP VALUE:
//                                      FIELD = x y z (its place) xmin xmax ymin ymax zmin zmax (its collision box, which moves with
//                                      the hazards: the swinging log's bob, the skunk, the worms) dist (to the character, xz);
//                                      OP = < or >. The hazard-aware wait: 'until-inst SWINGING_LOG xmin < -36' = the log is west.
//                                      NAME@ = of the instances whose name contains NAME, the one nearest the character (the worms)
//   until-air [MAX_FRAMES]             the neutral pad until the character rises (vy > 5: a worm's launch, a spring)
//   until-flag NAME MASK 0|1 [MAX_FRAMES]  the neutral pad until the awake instance whose name contains NAME has (flags & MASK) clear (0)
//                                      or set (1) -- the state bits of a hazard (a trapdoor's 0x200 = open)
//   watch NAME                         log the named instance's place + box with every probe line from here on (learn its motion)
//   hulls NAME                         dump the named instance's collision (ObjectCollision, OGI hulls, joint matrices) once
//   until-graph NAME GRAPH [STATE] [MAX_FRAMES]  the neutral pad until the awake instance named NAME (an object's whole name or the
//                                      part after its last '|') runs the behaviour graph GRAPH at a level, at its state STATE (-1:
//                                      any): a boss in the phase a test's message is for
// TEST REQUESTS (platform/nativeee/host/te_test_requests.h: what a test asks of the game, carried out by the game's own code at
// the start of the next frame of play; logged "[te.test] b N: ...", which tools/cutscenes/reach.py turns into the fixture's
// TE_TEST_ACTIONS -- the recording's pad log carries the inputs, never these):
//   teleport X Y Z [NAME]              the player (or NAME's instance) warped to (X, Y, Z) in its chunk's frame, as PositionWarp warps
//   message NAME ID [FROM]             the user message ID sent to NAME's instance from FROM's, as SendUserMessage sends one
//   counter N V [NAME]                 the game counter N set to V, as SetCounter sets one (NAME: that instance's own counter N)
//   stop                               end the plan (the runner's --stop-at ends the run)
#include "te_autopilot.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "padlog.h"
#include "te_cutscenes.h"
#include "te_probe.h"
#include "te_services.h"
#include "te_test_requests.h"

namespace te::autopilot {

namespace {
enum class Kind { Goto, GotoJump, Dash, Jump, Spin, Hold, Look, Wait, Idle, At, UntilChunk, UntilGState, Instances, Tables, UntilInst, UntilAir, UntilFlag, Watch, Hulls, UntilGraph, Test, Stop };

struct Step {
    Kind kind;
    float x = 0, z = 0, radius = 1.5f;
    uint64_t max_frames = 0;   // 0 = no limit
    uint16_t buttons = 0;
    uint32_t mask = 0;   // until-flag's mask
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    unsigned frames = 0;
    unsigned j1 = 8, r = 6, j2 = 8, d = 0;   // dash's double jump
    uint16_t pre = 0;                         // dash's buttons before the jump
    bool edge = false; float ex = 0, ez = 0;  // dash's jump trigger: the edge point
    uint64_t jump_at = 0;                     // when the dash's jump began (0: not yet)
    std::string name;   // until-chunk's chunk name; until-inst's / watch's instance name
    std::string field;  // until-inst's field
    bool less = true;   // until-inst's OP
    float value = 0;    // until-inst's VALUE
    int graph = 0, graph_state = -1;   // until-graph's GRAPH and STATE
    te::test_requests::Request request;   // a test request's
    std::string text;   // the plan line (logged)
    unsigned line = 0;
};

std::vector<Step> g_plan;
size_t g_step = 0;
uint64_t g_step_start = 0;
bool g_started = false;
// the stuck detector of goto: the best distance seen and when
float g_best = 1e9f;
uint64_t g_best_at = 0;
uint64_t g_jump_until = 0;
uint64_t g_detour_until = 0;
uint32_t g_step_chunk = 0;
unsigned g_stuck_jumps = 0;
bool g_done = false;
bool g_paused = false;
uint64_t g_look_in = 0;   // look: the boundary the camera first came within its tolerance (0: not yet)
std::string g_watch;   // the instance logged with the probe lines
uint64_t g_watch_last = 0;

float InstField(const probe::Instance& i, const probe::State& s, const std::string& field) {
    if (field == "x") return i.x; if (field == "y") return i.y; if (field == "z") return i.z;
    if (field == "xmin") return i.min_x; if (field == "xmax") return i.max_x;
    if (field == "ymin") return i.min_y; if (field == "ymax") return i.max_y;
    if (field == "zmin") return i.min_z; if (field == "zmax") return i.max_z;
    const float dx = i.x - s.x, dz = i.z - s.z;   // dist
    return std::sqrt(dx * dx + dz * dz);
}

void WatchTick(uint64_t boundary, const probe::State& s) {
    if (g_watch.empty() || boundary - g_watch_last < 10) return;
    g_watch_last = boundary;
    probe::Instance i;
    if (!probe::FindInstance(g_watch, i)) return;
    std::fprintf(stderr, "[autopilot] @%llu watch '%s' at (%.2f %.2f %.2f) box (%.2f %.2f %.2f)-(%.2f %.2f %.2f) flags 0x%05X dist %.1f\n",
                 (unsigned long long)boundary, g_watch.c_str(), i.x, i.y, i.z, i.min_x, i.min_y, i.min_z, i.max_x, i.max_y, i.max_z, i.flags,
                 InstField(i, s, "dist"));
}

constexpr uint16_t kCross = 1u << 14, kSquare = 1u << 15;

void Begin(uint64_t boundary) {
    g_step_start = boundary;
    g_best = 1e9f;
    g_best_at = boundary;
    g_jump_until = 0;
    g_detour_until = 0;
    g_stuck_jumps = 0;
    g_look_in = 0;
    if (g_step < g_plan.size()) {
        g_plan[g_step].jump_at = 0;
        const Step& st = g_plan[g_step];
        probe::State s;
        probe::Read(s);
        g_step_chunk = s.chunk;
        // (no %zu: the Vita's newlib printf has none -- it printed "zu" and the %s read the next argument as a pointer)
        std::fprintf(stderr, "[autopilot] @%llu step %u (line %u): %s", (unsigned long long)boundary, unsigned(g_step), st.line, st.text.c_str());
        if (s.valid) std::fprintf(stderr, "  [at (%.2f %.2f %.2f) chunk %s]", s.x, s.y, s.z, s.chunk_path.c_str());
        std::fprintf(stderr, "\n");
    }
}

void Next(uint64_t boundary, const char* why) {
    probe::State s;
    probe::Read(s);
    std::fprintf(stderr, "[autopilot] @%llu step %u done (%s) after %llu frames at (%.2f %.2f %.2f)\n", (unsigned long long)boundary, unsigned(g_step), why,
                 (unsigned long long)(boundary - g_step_start), s.x, s.y, s.z);
    ++g_step;
    Begin(boundary);
    if (g_step >= g_plan.size()) {
        g_done = true;
        std::fprintf(stderr, "[autopilot] @%llu plan complete\n", (unsigned long long)boundary);
    }
}

// The stick for a world direction (dx, dz) given where the camera looks: forward = look, right = (look.z, -look.x)
// (measured: LY=0 walks along the look, LX=255 along (look.z, -look.x)). Full deflection.
void Steer(const probe::State& s, float dx, float dz, Pads::Sample& out) {
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-4f) return;
    dx /= len; dz /= len;
    float fx = s.look_x, fz = s.look_z;
    const float flen = std::sqrt(fx * fx + fz * fz);
    if (flen < 1e-4f) { fx = 0; fz = 1; } else { fx /= flen; fz /= flen; }
    const float rx = fz, rz = -fx;
    float forward = dx * fx + dz * fz;   // -1..1
    float right = dx * rx + dz * rz;
    // The game runs only at a (near) full deflection and reads a partly pushed stick as a walk (measured: (128,40) and
    // (100,10) walk at 2.5 u/s, (128,0) and (66,1) run at 9 u/s), so the direction is pushed out to the square's edge.
    const float m = std::fmax(std::fabs(forward), std::fabs(right));
    if (m > 1e-4f) { forward /= m; right /= m; }
    auto to_axis = [](float v) { return uint8_t(std::lround(std::fmin(255.f, std::fmax(0.f, 128.f + v * 127.f)))); };
    out.lx = to_axis(right);
    out.ly = to_axis(-forward);
}

Pads::Sample Compute(uint64_t boundary) {
    Pads::Sample out;
    out.connected = true;
    if (!g_started) { g_started = true; Begin(boundary); if (g_plan.empty()) g_done = true; }
    if (g_done || g_step >= g_plan.size()) return out;
    const Step& st = g_plan[g_step];
    probe::State s;
    const bool have = probe::Read(s);
    if (have) WatchTick(boundary, s);
    // Movement steps run only while the game is Playing (12): a cutscene (Watching), a load or a death pause them --
    // the step's clocks are shifted so the frames outside play do not count (and nothing is pressed: CROSS would skip
    // the cutscene, which the fixture must let play).
    const bool movement = st.kind == Kind::Goto || st.kind == Kind::GotoJump || st.kind == Kind::Dash || st.kind == Kind::Jump || st.kind == Kind::Spin ||
                          st.kind == Kind::Wait || st.kind == Kind::Look || st.kind == Kind::UntilChunk || st.kind == Kind::UntilInst || st.kind == Kind::UntilAir || st.kind == Kind::UntilFlag ||
                          st.kind == Kind::UntilGraph || st.kind == Kind::Test;
    if (movement && s.game_state != 12) {
        // A modal message (Paused, 15: "auto-save failed", a hint) is dismissed with CROSS (3 frames every 40); the authoring
        // never presses START, so 15 is always a message here. Other states (Watching, loads) are let play.
        if (s.game_state == 15 && (boundary % 40) < 3) out.buttons = kCross;
        ++g_step_start;
        ++g_best_at;
        if (g_jump_until) ++g_jump_until;
        if (g_detour_until) ++g_detour_until;
        if (!g_paused) { g_paused = true; std::fprintf(stderr, "[autopilot] @%llu paused (gstate %u)\n", (unsigned long long)boundary, s.game_state); }
        return out;
    }
    if (g_paused) { g_paused = false; std::fprintf(stderr, "[autopilot] @%llu resumed\n", (unsigned long long)boundary); }
    const uint64_t elapsed = boundary - g_step_start;
    if (st.kind != Kind::At && st.max_frames && elapsed >= st.max_frames) { Next(boundary, "max frames"); return Compute(boundary); }
    switch (st.kind) {
        case Kind::Instances: probe::DumpInstances(boundary); Next(boundary, "listed"); return Compute(boundary);
        case Kind::UntilAir:
            if (have && s.vy > 5.f) { std::fprintf(stderr, "[autopilot] @%llu until-air: vy %.2f at (%.2f %.2f %.2f)\n", (unsigned long long)boundary, s.vy, s.x, s.y, s.z); Next(boundary, "airborne"); return Compute(boundary); }
            return out;
        case Kind::Tables: probe::DumpTextureTable(boundary); Next(boundary, "listed"); return Compute(boundary);
        case Kind::UntilFlag: {
            probe::Instance i;
            if (have && probe::FindInstance(st.name, i) && (((i.flags & st.mask) != 0) == st.less)) {
                std::fprintf(stderr, "[autopilot] @%llu until-flag: '%s' flags 0x%05X (mask 0x%X %s)\n", (unsigned long long)boundary, st.name.c_str(), i.flags, st.mask, st.less ? "set" : "clear");
                Next(boundary, "flag condition"); return Compute(boundary);
            }
            return out;
        }
        case Kind::Hulls: probe::DumpHulls(st.name, boundary); Next(boundary, "dumped"); return Compute(boundary);
        case Kind::UntilGraph:
            if (cutscenes::RunsGraph(st.name, st.graph, st.graph_state)) {
                std::fprintf(stderr, "[autopilot] @%llu until-graph: '%s' runs %d%s\n", (unsigned long long)boundary, st.name.c_str(), st.graph,
                             st.graph_state >= 0 ? (" at s" + std::to_string(st.graph_state)).c_str() : "");
                Next(boundary, "graph running"); return Compute(boundary);
            }
            return out;
        case Kind::Test: test_requests::Ask(st.request, boundary); Next(boundary, "asked"); return Compute(boundary);
        case Kind::Watch: g_watch = st.name; g_watch_last = 0; Next(boundary, "watching"); return Compute(boundary);
        case Kind::UntilInst: {
            probe::Instance i;
            if (have && probe::FindInstance(st.name, i)) {
                const float v = InstField(i, s, st.field);
                if (st.less ? v < st.value : v > st.value) {
                    std::fprintf(stderr, "[autopilot] @%llu until-inst: '%s' %s = %.2f (box (%.2f %.2f %.2f)-(%.2f %.2f %.2f))\n", (unsigned long long)boundary,
                                 st.name.c_str(), st.field.c_str(), v, i.min_x, i.min_y, i.min_z, i.max_x, i.max_y, i.max_z);
                    Next(boundary, "instance condition"); return Compute(boundary);
                }
            }
            return out;
        }
        case Kind::Stop: g_done = true; std::fprintf(stderr, "[autopilot] @%llu stop\n", (unsigned long long)boundary); return out;
        case Kind::Wait:
        case Kind::Idle:
            if (elapsed >= st.frames) { Next(boundary, "waited"); return Compute(boundary); }
            return out;
        case Kind::At:
            if (boundary >= st.max_frames) { Next(boundary, "boundary reached"); return Compute(boundary); }
            return out;
        case Kind::Jump:
            if (elapsed >= 10) { Next(boundary, "jumped"); return Compute(boundary); }
            out.buttons = kCross;
            return out;
        case Kind::Spin:
            if (elapsed >= 6) { Next(boundary, "spun"); return Compute(boundary); }
            out.buttons = kSquare;
            return out;
        case Kind::Hold:
            if (elapsed >= st.frames) { Next(boundary, "held"); return Compute(boundary); }
            out.buttons = st.buttons; out.lx = st.lx; out.ly = st.ly; out.rx = st.rx; out.ry = st.ry;
            return out;
        case Kind::Look: {
            if (!have) return out;
            // the error from the camera's look (xz) to the wanted direction, wrapped to (-pi, pi]; RX 255 turns the look from
            // +x towards +z (atan2(z, x) grows), so its sign picks the stick's side, its size the throw (at least half)
            constexpr float kPi = 3.14159265f;
            float e = std::atan2(st.z, st.x) - std::atan2(s.look_z, s.look_x);
            while (e > kPi) e -= 2.f * kPi;
            while (e <= -kPi) e += 2.f * kPi;
            const bool within = std::fabs(e) <= st.radius * kPi / 180.f;
            if (within && !g_look_in) {
                g_look_in = boundary;
                std::fprintf(stderr, "[autopilot] @%llu look: within %.1f deg of (%.2f, %.2f), the camera looks (%.2f %.2f) from (%.1f %.1f %.1f)\n",
                             (unsigned long long)boundary, st.radius, st.x, st.z, s.look_x, s.look_z, s.cam_x, s.cam_y, s.cam_z);
            }
            if (g_look_in && boundary - g_look_in >= st.frames) { Next(boundary, "looking"); return Compute(boundary); }
            if (!within) {
                const float m = std::fmin(1.f, std::fmax(0.5f, std::fabs(e) / (30.f * kPi / 180.f)));
                out.rx = uint8_t(std::lround(128.f + (e > 0.f ? 127.f : -127.f) * m));
            }
            return out;
        }
        case Kind::UntilChunk:
            if (have && s.chunk_path.find(st.name) != std::string::npos) { Next(boundary, "chunk reached"); return Compute(boundary); }
            return out;
        case Kind::UntilGState:
            if (s.game_state == st.frames) { Next(boundary, "state reached"); return Compute(boundary); }
            return out;
        case Kind::Dash: {
            if (!have) return out;
            if (elapsed >= st.frames) { Next(boundary, "dashed"); return Compute(boundary); }
            Steer(s, st.x - s.x, st.z - s.z, out);
            uint64_t began = st.d;   // the jump's first frame (relative to the step)
            if (st.edge) {
                Step& mut = g_plan[g_step];
                if (!mut.jump_at) {
                    const float ex = st.ex - s.x, ez = st.ez - s.z;        // to the edge
                    const float tx = st.x - st.ex, tz = st.z - st.ez;      // the edge's forward (towards the target)
                    if (std::sqrt(ex * ex + ez * ez) <= 0.6f || ex * tx + ez * tz < 0) {
                        mut.jump_at = boundary;
                        std::fprintf(stderr, "[autopilot] @%llu dash: jump at the edge (%.1f, %.1f), character at (%.2f %.2f %.2f) vel (%.2f %.2f %.2f)\n",
                                     (unsigned long long)boundary, st.ex, st.ez, s.x, s.y, s.z, s.vx, s.vy, s.vz);
                    }
                }
                if (!mut.jump_at) return out;
                began = mut.jump_at - g_step_start;
            }
            if (elapsed >= began) { const uint64_t t = elapsed - began; if (t < st.j1 || (t >= st.j1 + st.r && t < st.j1 + st.r + st.j2)) out.buttons |= kCross; }
            if (st.pre && elapsed + 8 >= began && elapsed < began + 3) out.buttons |= st.pre;
            return out;
        }
        case Kind::Goto:
        case Kind::GotoJump: {
            if (!have) return out;
            // Positions are CHUNK-LOCAL (every chunk has its own frame; crossing a load wall re-bases the character), so a
            // goto's target is meaningless once the chunk changed: the step ends and the plan continues in the new frame.
            if (s.chunk != g_step_chunk && g_step_chunk != 0) { Next(boundary, "chunk changed"); return Compute(boundary); }
            const float dx = st.x - s.x, dz = st.z - s.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist <= st.radius) { Next(boundary, "arrived"); return Compute(boundary); }
            if (dist < g_best - 0.25f) { g_best = dist; g_best_at = boundary; }
            // Stuck (no progress for 45 frames): detour k alternates a sidestep left/right of the target direction (perpendicular,
            // 20 + 10k frames) with a jump at its start -- a tree, a rock, a ledge -- then the target is steered at again.
            if (boundary < g_detour_until) {
                const float side = (g_stuck_jumps % 2) ? 1.f : -1.f;
                Steer(s, side * dz, -side * dx, out);
                if (boundary < g_jump_until) out.buttons |= kCross;
                return out;
            }
            Steer(s, dx, dz, out);
            if (st.kind == Kind::GotoJump) {
                if (((boundary - g_step_start) % 24) < 10) out.buttons |= kCross;
            } else if (boundary < g_jump_until) {
                out.buttons |= kCross;
            } else if (boundary - g_best_at > 45) {
                ++g_stuck_jumps;
                g_jump_until = boundary + 10;
                g_detour_until = (g_stuck_jumps % 3 == 1) ? boundary : boundary + 20 + 10 * g_stuck_jumps;   // 1st: a plain jump
                g_best_at = boundary;
                std::fprintf(stderr, "[autopilot] @%llu stuck at (%.2f %.2f %.2f) %.1f from (%.1f, %.1f): jump %u%s\n", (unsigned long long)boundary,
                             s.x, s.y, s.z, dist, st.x, st.z, g_stuck_jumps, g_detour_until > boundary ? " + detour" : "");
                out.buttons |= kCross;
            }
            return out;
        }
    }
    return out;
}
}  // namespace

bool Load(const std::string& path, std::string* error) {
    std::ifstream f(path);
    if (!f) { if (error) *error = "cannot open " + path; return false; }
    std::string line;
    unsigned n = 0;
    g_plan.clear();
    while (std::getline(f, line)) {
        ++n;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream in(line);
        std::string op;
        if (!(in >> op)) continue;
        Step st;
        st.line = n;
        st.text = line.substr(line.find_first_not_of(" \t"));
        while (!st.text.empty() && (st.text.back() == ' ' || st.text.back() == '\t')) st.text.pop_back();
        auto fail = [&](const char* what) { if (error) *error = path + ":" + std::to_string(n) + ": " + what; return false; };
        if (op == "goto" || op == "gotoj") {
            st.kind = op == "goto" ? Kind::Goto : Kind::GotoJump;
            if (!(in >> st.x >> st.z)) return fail("goto X Z [RADIUS] [MAX_FRAMES]");
            in >> st.radius >> st.max_frames;
        } else if (op == "dash") {
            st.kind = Kind::Dash;
            if (!(in >> st.x >> st.z >> st.frames)) return fail("dash X Z N [J1 R J2 D]");
            in >> st.j1 >> st.r >> st.j2;
            std::string d;
            if (in >> d) {
                if (d[0] == '@') { st.edge = true; if (std::sscanf(d.c_str(), "@%f,%f", &st.ex, &st.ez) != 2) return fail("dash edge is @EX,EZ"); }
                else st.d = unsigned(std::atoi(d.c_str()));
            }
            std::string pre;
            if (in >> pre && !padlog::ParseButtons(pre, st.pre)) return fail("unknown button");
        } else if (op == "jump") st.kind = Kind::Jump;
        else if (op == "spin") st.kind = Kind::Spin;
        else if (op == "stop") st.kind = Kind::Stop;
        else if (op == "instances") st.kind = Kind::Instances;
        else if (op == "tables") st.kind = Kind::Tables;
        else if (op == "until-air") { st.kind = Kind::UntilAir; in >> st.max_frames; }
        else if (op == "watch") { st.kind = Kind::Watch; if (!(in >> st.name)) return fail("watch NAME"); }
        else if (op == "hulls") { st.kind = Kind::Hulls; if (!(in >> st.name)) return fail("hulls NAME"); }
        else if (op == "until-graph") {
            st.kind = Kind::UntilGraph;
            if (!(in >> st.name >> st.graph)) return fail("until-graph NAME GRAPH [STATE] [MAX_FRAMES]");
            in >> st.graph_state >> st.max_frames;
        }
        else if (op == "teleport" || op == "message" || op == "counter") {
            st.kind = Kind::Test;
            std::string why;
            if (!test_requests::Parse(line, &st.request, &why)) { if (error) *error = path + ":" + std::to_string(n) + ": " + why; return false; }
        }
        else if (op == "until-flag") {
            st.kind = Kind::UntilFlag;
            std::string mask, want;
            if (!(in >> st.name >> mask >> want)) return fail("until-flag NAME MASK 0|1 [MAX_FRAMES]");
            st.mask = uint32_t(std::strtoul(mask.c_str(), nullptr, 0));
            st.less = want == "1";   // reused: true = wait for SET
            in >> st.max_frames;
        }
        else if (op == "until-inst") {
            st.kind = Kind::UntilInst;
            std::string op2;
            if (!(in >> st.name >> st.field >> op2 >> st.value)) return fail("until-inst NAME FIELD OP VALUE [MAX_FRAMES]");
            if (op2 != "<" && op2 != ">") return fail("until-inst OP is < or >");
            st.less = op2 == "<";
            static const char* fields[] = {"x", "y", "z", "xmin", "xmax", "ymin", "ymax", "zmin", "zmax", "dist"};
            bool ok = false; for (const char* f : fields) if (st.field == f) ok = true;
            if (!ok) return fail("until-inst FIELD is x y z xmin xmax ymin ymax zmin zmax dist");
            in >> st.max_frames;
        }
        else if (op == "wait") { st.kind = Kind::Wait; if (!(in >> st.frames)) return fail("wait N"); }
        else if (op == "idle") { st.kind = Kind::Idle; if (!(in >> st.frames)) return fail("idle N"); }
        else if (op == "at") { st.kind = Kind::At; if (!(in >> st.max_frames)) return fail("at N"); }
        else if (op == "hold") {
            st.kind = Kind::Hold;
            std::string buttons;
            if (!(in >> buttons >> st.frames)) return fail("hold BUTTONS N [LX LY RX RY]");
            if (!padlog::ParseButtons(buttons, st.buttons)) return fail("unknown button");
            int lx, ly, rx, ry;
            if (in >> lx >> ly >> rx >> ry) { st.lx = uint8_t(lx); st.ly = uint8_t(ly); st.rx = uint8_t(rx); st.ry = uint8_t(ry); }
        } else if (op == "look") {
            st.kind = Kind::Look;
            if (!(in >> st.x >> st.z)) return fail("look DX DZ [TOL] [N] [MAX_FRAMES]");
            st.radius = 4.f;
            in >> st.radius >> st.frames >> st.max_frames;
        } else if (op == "until-chunk") {
            st.kind = Kind::UntilChunk;
            if (!(in >> st.name)) return fail("until-chunk NAME [MAX_FRAMES]");
            in >> st.max_frames;
        } else if (op == "until-gstate") {
            st.kind = Kind::UntilGState;
            if (!(in >> st.frames)) return fail("until-gstate N [MAX_FRAMES]");
            in >> st.max_frames;
        } else return fail("unknown step");
        g_plan.push_back(st);
    }
    Services::Get().pads().SetOverrideSampler([](unsigned port, unsigned slot) {
        if (port != 0 || slot != 0) { Pads::Sample none; none.connected = false; return none; }
        return Compute(Services::Get().clock().BoundaryCount());
    });
    std::fprintf(stderr, "[autopilot] plan %s: %u steps\n", path.c_str(), unsigned(g_plan.size()));
    return true;
}

bool Done() { return g_done; }

}  // namespace te::autopilot
