// engine/platform/ps2e/scene_interpolator.h -- SceneInterpolator: the frame between two game pictures for a producer that
// presents faster than its game ticks (the typed-draw surface's consumer: a decompiled title's native arm, its game at
// 60 Hz, its presenter at 120 / 144 / 240 / uncapped on the PresentPacer).
//
// The producer keeps its last two pictures as it recorded them -- typed draws (ps2e_draw_state + frame-pixel vertices)
// in bucket order -- and TAGS each draw with what it is (InterpTag): a rigid object by a stable instance identity with
// its model->world and model->screen matrices as drawn (the camera's world->camera and camera->screen per picture), a
// run of vertices matched one to one when the same identity recurs with the same count (a skin's batch, a particle
// block, a decal block), or the newest picture's draw as it is (text, HUD, FMV pictures, menus, screen-space passes,
// anything untagged). Interpolate() produces the frame at `fraction` (0 = the previous picture, 1 = the newest):
//
//   camera    position lerp, orientation slerp, the camera->screen matrix lerped (the field of view: its scale terms vary
//             linearly with cot(fov/2)); a step beyond the cut thresholds (travel per tick, rotation per tick), or the
//             producer's cut flag (a level-chunk change), SNAPS the whole frame to the newest picture;
//   rigid     translation lerp, rotation slerp, scale lerp of the object's world matrix -- never a raw matrix lerp --
//             then every recorded vertex re-projected: a frame-pixel vertex (x, y, z) is taken back to the object's space
//             through the inverse of the model->screen matrix it was drawn with (the vertex's w comes out of that inverse)
//             and forward through the interpolated model->world, world->camera and camera->screen; S, T and Q are
//             rescaled by the depth ratio (perspective-correct); a step beyond the object thresholds, a singular matrix
//             or a vertex that lands behind the camera leaves the newest draw as it is (counted);
//   vertices  position lerp per vertex when the previous picture has the same identity and part with the same vertex
//             count, else the newest draw as it is;
//   newest    the newest draw as it is (also every object the previous picture does not have: spawned this tick).
//
// Row-vector convention throughout (the title's: p' = p . M, the translation in row 3). The frame's GS origin
// (off_x, off_y: the vertices are frame pixels = GS pixels minus it) is the producer's. The fraction is the producer's
// (its pacer's clock), clamped here to [0, 1]: nothing is ever extrapolated. One class; the plain records below carry
// no member functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"

namespace ps2r {
namespace ps2e {

enum class InterpKind : uint8_t { Newest = 0, Rigid = 1, Vertices = 2 };

struct InterpTag {
    uint64_t id = 0;        // the stable instance identity (0 = untagged: newest)
    uint32_t part = 0;      // a draw's ordinal within the object (Vertices: the batch / block to pair); Rigid: the FAMILY
                            // (0 = none) -- instances one producer names by their draw order, the model they share: an
                            // instance whose own previous instance is far is paired with a clearly nearer one of its family
                            // (the order shifts when an earlier instance is culled; the instances do not move)
    InterpKind kind = InterpKind::Newest;
    bool clipped = false;   // Rigid: the producer cut the instance's geometry to its view (clipped, parts culled): an
                            // in-between frame's camera sees past the cut, so the previous picture's opaque draws of the
                            // instance are drawn too, re-projected to the frame (they cover what the newest's cut left out)
};

struct InterpTransform {
    float world[16];        // model -> world as the object stood in this picture
    float screen[16];       // model -> screen as the draw was projected (world . world->camera . camera->screen)
};

struct InterpCamera {
    bool valid = false;
    float to_camera[16];        // world -> camera
    float camera_to_screen[16]; // camera -> screen (the GS pixel space, the viewport included)
};

struct InterpItem {
    const ps2e_draw_state* state = nullptr;
    const ps2e_draw_vertex* verts = nullptr;
    uint32_t count = 0;
    bool copy_frame = false;
    InterpTag tag;
    const InterpTransform* transform = nullptr;   // Rigid only (null: newest)
};

struct InterpScene {
    const InterpItem* items = nullptr;
    size_t count = 0;
    InterpCamera camera;
    float off_x = 0, off_y = 0;   // the GS origin the vertices had taken off (frame pixel = GS pixel - off)
    float screen_scale = 1;       // the model->screen matrices' x/y unit per frame pixel: 16 when they land on the GS's
                                  // 12.4 fixed-point raster (1/16 pixel), 1 when they give pixels
    bool cut = false;             // the producer's own cut (a level-chunk change): the frame snaps to the newest
    uint64_t ordinal = 0;         // the picture's ordinal as the producer counts them (the displacement check's pairs)
};

struct InterpOutput {
    const ps2e_draw_state* state = nullptr;
    const ps2e_draw_vertex* verts = nullptr;
    uint32_t count = 0;
    bool copy_frame = false;
};

struct InterpStats {
    uint64_t frames = 0;              // Interpolate calls
    uint64_t snapped_frames = 0;      // frames shown as the newest picture (a camera cut / the producer's cut)
    uint64_t rigid_items = 0;         // rigid draws re-projected
    uint64_t vertex_items = 0;        // vertex runs lerped
    uint64_t newest_items = 0;        // draws passed through as the newest picture's
    uint64_t object_snaps = 0;        // rigid draws left as they are: a step beyond the object thresholds
    uint64_t unmatched = 0;           // tagged draws without a counterpart in the previous picture
    uint64_t cover_items = 0;         // the previous picture's draws of a clipped instance drawn under the newest's
    uint64_t family_matches = 0;      // rigid instances paired with a nearer instance of their family than their own previous one
    uint64_t unprojectable = 0;       // rigid draws left as they are: a singular matrix or a vertex behind the camera
    uint64_t vertex_count_mismatch = 0;   // vertex runs found by identity but with another count (newest as they are)
    uint64_t vertex_run_jumps = 0;        // vertex runs refused because a vertex moved more than vertex_run_max_step_px
    uint64_t displacement_checks = 0; // matched objects (the same vertex count in both pictures) whose shown step was
                                      // checked against the real step between the pictures
    uint64_t displacement_violations = 0;   // ... and exceeded it by more than a pixel (a wrong identity match would)
};

class SceneInterpolator {
public:
    struct Options {
        double camera_cut_travel = 2.0;        // world units per tick
        double camera_cut_angle_degrees = 30;  // rotation per tick
        double object_snap_travel = 4.0;       // world units per tick
        double object_snap_angle_degrees = 60; // rotation per tick
        double displacement_tolerance_px = 1.0;
        double displacement_max_radius_px = 128.0;   // instances spread wider than this (RMS about the centroid) are not checked
        // A vertex run is lerped only when every vertex moved less than this between the two pictures (frame pixels): a
        // run whose vertices were re-ordered (a strip re-batched by the clipper, a particle block re-sorted) shows jumps of
        // the whole object's size and is shown as the newest instead
        double vertex_run_max_step_px = 64.0;
        bool trace_violations = false;         // each violation on stderr (a diagnosis run)
        bool check_displacement = true;        // the displacement check runs (two passes over every vertex per frame: a judge's run)
    };

    SceneInterpolator() = default;
    void SetOptions(const Options& options) { options_ = options; }
    const Options& GetOptions() const { return options_; }

    // The frame at `fraction` between `prev` and `cur`, in cur's draw order: `out` names cur's own draws or vertex runs
    // in this object's scratch storage (valid until the next call). Returns false when the frame snapped to the newest
    // picture (out = cur's draws as they are).
    bool Interpolate(const InterpScene& prev, const InterpScene& cur, double fraction, std::vector<InterpOutput>& out);
    // The census since construction, and the scratch storage's size (the one extra scene copy).
    const InterpStats& Stats() const { return stats_; }
    size_t ScratchBytes() const { return scratch_.capacity() * sizeof(ps2e_draw_vertex); }
    // The displacement check compares each matched object's step from the PREVIOUS SHOWN frame with the step between the
    // two pictures: a frame the producer shows as a picture itself (fraction 1, the draws as recorded) is noted here so
    // the next interpolated frame's step is measured from it, and the picture's own step from the previous shown frame
    // is checked against its step from the previous picture (a wrong identity match shows there: the interpolated frame
    // sat between two different objects). ResetHistory forgets the history (a new run).
    void NoteShownPicture(const InterpScene& cur);
    void ResetHistory();

    // The arithmetic (exposed for tests). Row vectors, 4x4 as 16 doubles row-major.
    static void Multiply4(const double a[16], const double b[16], double out[16]);
    static bool Invert4(const double m[16], double out[16]);
    // The world matrix's translation (row 3), rotation (a unit quaternion, x y z w) and per-axis scale; false when a
    // row is zero (no rotation to speak of).
    static bool DecomposeTrs(const double m[16], double t[3], double q[4], double s[3]);
    static void ComposeTrs(const double t[3], const double q[4], const double s[3], double out[16]);
    static void Slerp(const double a[4], const double b[4], double f, double out[4]);
    static double AngleDegrees(const double a[4], const double b[4]);
    // The world->camera and camera->screen matrices at `fraction` between two pictures' cameras; *cut when the step is
    // beyond the thresholds (the outputs are then b's).
    static bool InterpolateCamera(const InterpCamera& a, const InterpCamera& b, double fraction, const Options& options,
                                  double to_camera[16], double camera_to_screen[16], bool* cut);

private:
    // (identity, part) as one key: the part mixed in by a multiplicative hash so a producer whose identities fill the
    // 64 bits (two 32-bit addresses) never collides with another identity's part
    static uint64_t Key(const InterpTag& tag);
    // A rigid INSTANCE is its identity plus where it stands (the world matrix's translation): a producer that draws several
    // instances of one model through one scratch matrix cannot name them apart, so instances of one identity are matched
    // by the NEAREST previous translation (within the object threshold).
    static uint64_t InstanceKey(const InterpTag& tag, const float world[16]);
    bool ReprojectRigid(const InterpItem& item, const double K[16], float off_x, float off_y, float scale, size_t* begin);

    Options options_;
    InterpStats stats_;
    std::vector<ps2e_draw_vertex> scratch_;
    std::unordered_map<uint64_t, size_t> prev_by_key_;    // (id, part) -> index into prev's items (vertex runs)
    struct PrevInstance { size_t item; uint64_t key; double t[3]; bool taken = false; };   // taken: matched this frame (1:1)
    std::unordered_map<uint64_t, std::vector<PrevInstance>> prev_rigid_;   // id -> prev's rigid instances (one per translation)
    std::unordered_map<uint32_t, std::vector<std::pair<uint64_t, size_t>>> prev_family_;   // family -> (id, index in prev_rigid_[id])
    std::unordered_map<uint64_t, std::vector<size_t>> prev_draws_;   // a previous instance (its key) -> its draws (prev's items)
    // sums until finished, then means; `carry` = the part of the pictures' step the shown frame had NOT yet travelled when
    // it was shown ((1 - fraction) * the pair's step): the next shown frame may legitimately move that far on top of its
    // own pair's step
    // `pair` = the picture ordinal the frame was interpolated TOWARDS (the newest picture of its pair; a shown picture is its
    // own ordinal): two shown frames are compared only when they belong to the same pair, or when the earlier one is that
    // pair's first picture -- the displacement between them is then bounded by the pair's own step
    // `carry` = what the frame had left of its own pair's step when shown ((1 - fraction) . that step): the next pair's first
    // frame may legitimately travel that far on top of its own fraction of its pair's step
    struct Centroid { double x = 0, y = 0; uint64_t count = 0; double t[3] = {0, 0, 0}; uint64_t id = 0; uint64_t pair = 0; double carry = 0; bool valid = false; };
    // the per-instance accumulators of one frame (sum of the shown vertices, of the newest picture's, of the matched
    // previous instance's), finished into shown_ / picture_ at the end of the frame
    // prev_key: the previous PICTURE's instance (the real step); shown_key: the key the previous SHOWN frame's centroid
    // of this instance was stored under (an interpolated frame is keyed by the newest picture's instance)
    // rot_rad: a rigid instance's rotation between the two pictures (its slerp sweeps an arc: a point off the pivot moves
    // along a chord, whose middle is off the straight line by up to radius . (1 - cos(angle / 2)); the bound allows it)
    struct Accum { Centroid shown, cur, prev; uint64_t id = 0, prev_key = 0, shown_key = 0; double rot_rad = 0, sq = 0, radius_px = 0; bool matched = false; bool have_prev = false; };
    std::unordered_map<uint64_t, Accum> accum_;
    void CheckDisplacements(bool picture, double fraction);
    uint64_t picture_ordinal_ = 0;   // the newest picture's ordinal (the producer's InterpScene::ordinal; counted here when it gives none)
    uint64_t last_cur_items_ = 0;    // the `cur` scene's identity the last Interpolate saw when no ordinal is given
    // the previous instance of `id` nearest to `t` (world translation), or null
    // the nearest NOT YET TAKEN previous instance of `id` (one previous instance matches one newest instance per frame;
    // the newest are visited in draw order, the same in both frames of a pair, so the assignment is the same too)
    PrevInstance* NearestPrev(uint64_t id, const double t[3], double* distance);
    // the nearest NOT YET TAKEN previous instance of any id of `family`, or null
    PrevInstance* NearestInFamily(uint32_t family, const double t[3], double* distance);
    std::unordered_map<uint64_t, Centroid> shown_;         // the previous shown frame's object centroids (by id)
    std::unordered_map<uint64_t, Centroid> shown_next_;
    std::unordered_map<uint64_t, Centroid> picture_;       // the previous shown PICTURE's instance centroids (by instance key)
    std::unordered_map<uint64_t, Centroid> picture_next_;
};

}  // namespace ps2e
}  // namespace ps2r
