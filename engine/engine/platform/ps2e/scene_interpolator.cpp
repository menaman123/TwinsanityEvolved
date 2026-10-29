// engine/platform/ps2e/scene_interpolator.cpp -- SceneInterpolator. See the header.
#include "engine/platform/ps2e/scene_interpolator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ps2r {
namespace ps2e {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinW = 1e-6;   // a re-projected vertex's w below this is behind (or on) the camera

void ToDouble(const float in[16], double out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = double(in[i]);
}

double Dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Normalize4(double q[4]) {
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0) for (int i = 0; i < 4; ++i) q[i] /= n;
    else { q[0] = q[1] = q[2] = 0; q[3] = 1; }
}

// A unit quaternion (x y z w) from the rows of a rotation matrix (row-vector convention: e_i . R = row i)
void QuatFromRows(const double r[3][3], double q[4]) {
    // the column form C = R^T is the usual v' = C v matrix; Shepperd's method on it
    const double c00 = r[0][0], c11 = r[1][1], c22 = r[2][2];
    const double c01 = r[1][0], c02 = r[2][0], c10 = r[0][1], c12 = r[2][1], c20 = r[0][2], c21 = r[1][2];
    const double trace = c00 + c11 + c22;
    if (trace > 0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q[3] = 0.25 * s;
        q[0] = (c21 - c12) / s;
        q[1] = (c02 - c20) / s;
        q[2] = (c10 - c01) / s;
    } else if (c00 > c11 && c00 > c22) {
        const double s = std::sqrt(1.0 + c00 - c11 - c22) * 2.0;
        q[3] = (c21 - c12) / s;
        q[0] = 0.25 * s;
        q[1] = (c01 + c10) / s;
        q[2] = (c02 + c20) / s;
    } else if (c11 > c22) {
        const double s = std::sqrt(1.0 + c11 - c00 - c22) * 2.0;
        q[3] = (c02 - c20) / s;
        q[0] = (c01 + c10) / s;
        q[1] = 0.25 * s;
        q[2] = (c12 + c21) / s;
    } else {
        const double s = std::sqrt(1.0 + c22 - c00 - c11) * 2.0;
        q[3] = (c10 - c01) / s;
        q[0] = (c02 + c20) / s;
        q[1] = (c12 + c21) / s;
        q[2] = 0.25 * s;
    }
    Normalize4(q);
}

// The rows of the rotation matrix of a unit quaternion (the transpose of the column form)
void RowsFromQuat(const double q[4], double r[3][3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double c[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = c[j][i];
}

double Lerp(double a, double b, double f) { return a + (b - a) * f; }

}  // namespace

void SceneInterpolator::Multiply4(const double a[16], const double b[16], double out[16]) {
    double t[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
            t[i * 4 + j] = s;
        }
    for (int i = 0; i < 16; ++i) out[i] = t[i];
}

bool SceneInterpolator::Invert4(const double m[16], double out[16]) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-30) return false;
    const double r = 1.0 / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * r;
    return true;
}

bool SceneInterpolator::DecomposeTrs(const double m[16], double t[3], double q[4], double s[3]) {
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    double r[3][3];
    for (int i = 0; i < 3; ++i) {
        const double* row = m + i * 4;
        s[i] = std::sqrt(Dot3(row, row));
        if (!(s[i] > 1e-12) || !std::isfinite(s[i])) return false;
        for (int j = 0; j < 3; ++j) r[i][j] = row[j] / s[i];
    }
    // a mirrored basis (a negative determinant): the mirror goes into the last scale
    const double det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                       r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    if (det < 0) {
        s[2] = -s[2];
        for (int j = 0; j < 3; ++j) r[2][j] = -r[2][j];
    }
    QuatFromRows(r, q);
    return true;
}

void SceneInterpolator::ComposeTrs(const double t[3], const double q[4], const double s[3], double out[16]) {
    double r[3][3];
    RowsFromQuat(q, r);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i * 4 + j] = r[i][j] * s[i];
        out[i * 4 + 3] = 0;
    }
    out[12] = t[0]; out[13] = t[1]; out[14] = t[2]; out[15] = 1;
}

void SceneInterpolator::Slerp(const double a[4], const double b[4], double f, double out[4]) {
    double bb[4] = {b[0], b[1], b[2], b[3]};
    double cosom = a[0] * bb[0] + a[1] * bb[1] + a[2] * bb[2] + a[3] * bb[3];
    if (cosom < 0) {   // the shorter arc
        cosom = -cosom;
        for (int i = 0; i < 4; ++i) bb[i] = -bb[i];
    }
    double k0, k1;
    if (cosom > 0.9995) {   // nearly the same: lerp, renormalised
        k0 = 1.0 - f;
        k1 = f;
    } else {
        const double omega = std::acos(std::min(1.0, cosom));
        const double sinom = std::sin(omega);
        k0 = std::sin((1.0 - f) * omega) / sinom;
        k1 = std::sin(f * omega) / sinom;
    }
    for (int i = 0; i < 4; ++i) out[i] = a[i] * k0 + bb[i] * k1;
    Normalize4(out);
}

double SceneInterpolator::AngleDegrees(const double a[4], const double b[4]) {
    const double d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2.0 * std::acos(std::min(1.0, d)) * 180.0 / kPi;
}

bool SceneInterpolator::InterpolateCamera(const InterpCamera& a, const InterpCamera& b, double fraction, const Options& options,
                                          double to_camera[16], double camera_to_screen[16], bool* cut) {
    if (cut) *cut = false;
    if (!a.valid || !b.valid) return false;
    double va[16], vb[16], pa[16], pb[16];
    ToDouble(a.to_camera, va);
    ToDouble(b.to_camera, vb);
    ToDouble(a.camera_to_screen, pa);
    ToDouble(b.camera_to_screen, pb);
    // the camera's pose = the inverse of world->camera: its position is row 3, its orientation the 3x3
    double ia[16], ib[16];
    if (!Invert4(va, ia) || !Invert4(vb, ib)) return false;
    double ta[3], tb[3], qa[4], qb[4], sa[3], sb[3];
    if (!DecomposeTrs(ia, ta, qa, sa) || !DecomposeTrs(ib, tb, qb, sb)) return false;
    const double travel = std::sqrt((tb[0] - ta[0]) * (tb[0] - ta[0]) + (tb[1] - ta[1]) * (tb[1] - ta[1]) + (tb[2] - ta[2]) * (tb[2] - ta[2]));
    const double angle = AngleDegrees(qa, qb);
    if (travel > options.camera_cut_travel || angle > options.camera_cut_angle_degrees) {
        if (cut) *cut = true;
        for (int i = 0; i < 16; ++i) { to_camera[i] = vb[i]; camera_to_screen[i] = pb[i]; }
        return true;
    }
    double t[3], q[4], s[3], pose[16];
    for (int i = 0; i < 3; ++i) { t[i] = Lerp(ta[i], tb[i], fraction); s[i] = Lerp(sa[i], sb[i], fraction); }
    Slerp(qa, qb, fraction, q);
    ComposeTrs(t, q, s, pose);
    if (!Invert4(pose, to_camera)) return false;
    for (int i = 0; i < 16; ++i) camera_to_screen[i] = Lerp(pa[i], pb[i], fraction);
    return true;
}

void SceneInterpolator::ResetHistory() {
    shown_.clear();
    shown_next_.clear();
    picture_.clear();
    picture_next_.clear();
}

bool SceneInterpolator::ReprojectRigid(const InterpItem& item, const double K[16], float off_x, float off_y, float scale, size_t* begin) {
    const size_t start = scratch_.size();
    scratch_.resize(start + item.count);
    // The matrix folded for the vertex loop in float: the scale and the GS origin on the input side go into K's rows and
    // translation, the inverse scale on the output side into the x / y columns -- one 4x4 per vertex, no divisions but the
    // perspective one. The entries are frame-pixel sized (K maps pixels to pixels), so float holds them.
    const double sc = scale > 0 ? double(scale) : 1.0;
    float k[16];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) k[r * 4 + c] = float(K[r * 4 + c] * (r < 2 ? sc : 1.0) * (c < 2 ? 1.0 / sc : 1.0));
    for (int c = 0; c < 4; ++c)
        k[12 + c] = float((K[12 + c] + double(off_x) * sc * K[c] + double(off_y) * sc * K[4 + c]) * (c < 2 ? 1.0 / sc : 1.0));
    const float kMinWf = float(kMinW);
    ps2e_draw_vertex* out = scratch_.data() + start;
    for (uint32_t i = 0; i < item.count; ++i) {
        const ps2e_draw_vertex& v = item.verts[i];
        const float x = v.x, y = v.y, z = v.z;
        const float hw = x * k[3] + y * k[7] + z * k[11] + k[15];
        if (!(hw > kMinWf) || !std::isfinite(hw)) {
            scratch_.resize(start);
            return false;
        }
        const float r = 1.0f / hw;
        const float hx = x * k[0] + y * k[4] + z * k[8] + k[12];
        const float hy = x * k[1] + y * k[5] + z * k[9] + k[13];
        const float hz = x * k[2] + y * k[6] + z * k[10] + k[14];
        ps2e_draw_vertex o = v;
        o.x = hx * r - off_x;
        o.y = hy * r - off_y;
        o.z = std::max(0.0f, hz * r);
        if (v.q != 0.f) {   // the GS's S, T, Q scale with 1/w: the depth ratio is hw
            o.u = v.u * r;
            o.v = v.v * r;
            o.q = v.q * r;
        }
        out[i] = o;
    }
    *begin = start;
    return true;
}

uint64_t SceneInterpolator::Key(const InterpTag& tag) {
    uint64_t h = tag.id ^ 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t(tag.part) + 1) * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
    return h;
}

uint64_t SceneInterpolator::InstanceKey(const InterpTag& tag, const float world[16]) {
    uint32_t bits[3];
    std::memcpy(bits, world + 12, sizeof bits);
    uint64_t h = 1469598103934665603ull;
    for (uint32_t b : bits) { h ^= b; h *= 1099511628211ull; }
    return tag.id ^ (h | 1);
}

SceneInterpolator::PrevInstance* SceneInterpolator::NearestPrev(uint64_t id, const double t[3], double* distance) {
    auto list = prev_rigid_.find(id);
    if (list == prev_rigid_.end()) return nullptr;
    PrevInstance* best = nullptr;
    double best_d = 0;
    for (PrevInstance& p : list->second) {
        if (p.taken) continue;
        const double d = std::sqrt((p.t[0] - t[0]) * (p.t[0] - t[0]) + (p.t[1] - t[1]) * (p.t[1] - t[1]) + (p.t[2] - t[2]) * (p.t[2] - t[2]));
        if (!best || d < best_d) { best = &p; best_d = d; }
    }
    if (distance) *distance = best_d;
    return best;
}

SceneInterpolator::PrevInstance* SceneInterpolator::NearestInFamily(uint32_t family, const double t[3], double* distance) {
    auto members = prev_family_.find(family);
    if (members == prev_family_.end()) return nullptr;
    PrevInstance* best = nullptr;
    double best_d = 0;
    for (const auto& [id, index] : members->second) {
        PrevInstance& p = prev_rigid_[id][index];
        if (p.taken) continue;
        const double d = std::sqrt((p.t[0] - t[0]) * (p.t[0] - t[0]) + (p.t[1] - t[1]) * (p.t[1] - t[1]) + (p.t[2] - t[2]) * (p.t[2] - t[2]));
        if (!best || d < best_d) { best = &p; best_d = d; }
    }
    if (distance) *distance = best_d;
    return best;
}

bool SceneInterpolator::Interpolate(const InterpScene& prev, const InterpScene& cur, double fraction, std::vector<InterpOutput>& out) {
    stats_.frames++;
    fraction = std::min(1.0, std::max(0.0, fraction));
    out.clear();
    out.reserve(cur.count);
    scratch_.clear();
    accum_.clear();
    auto newest_all = [&]() {
        for (size_t i = 0; i < cur.count; ++i) {
            const InterpItem& it = cur.items[i];
            out.push_back(InterpOutput{it.state, it.verts, it.count, it.copy_frame});
        }
        stats_.newest_items += cur.count;
    };
    // The camera between the two pictures; a cut snaps the frame
    double to_camera[16], camera_to_screen[16];
    bool cut = false;
    const bool have_camera = InterpolateCamera(prev.camera, cur.camera, fraction, options_, to_camera, camera_to_screen, &cut);
    if (cur.cut || cut) {
        stats_.snapped_frames++;
        newest_all();
        shown_.clear();   // the displacement history does not cross a cut
        picture_.clear();
        return false;
    }
    double world_to_screen[16];
    if (have_camera) Multiply4(to_camera, camera_to_screen, world_to_screen);
    // a new newest picture = a new pair (the frames of one pair share its ordinal in the displacement history)
    if (cur.ordinal) picture_ordinal_ = cur.ordinal;
    else if (reinterpret_cast<uint64_t>(cur.items) != last_cur_items_) {
        last_cur_items_ = reinterpret_cast<uint64_t>(cur.items);
        picture_ordinal_++;
    }
    // the previous picture's vertex runs by (id, part); its rigid instances by id (one per translation)
    prev_by_key_.clear();
    prev_rigid_.clear();
    prev_family_.clear();
    for (size_t i = 0; i < prev.count; ++i) {
        const InterpItem& it = prev.items[i];
        if (it.tag.id == 0 || it.tag.kind == InterpKind::Newest) continue;
        if (it.tag.kind == InterpKind::Vertices) prev_by_key_.emplace(Key(it.tag), i);
        else if (it.transform) {
            auto& list = prev_rigid_[it.tag.id];
            const uint64_t key = InstanceKey(it.tag, it.transform->world);
            bool seen = false;
            for (const PrevInstance& p : list) seen = seen || p.key == key;
            if (!seen) {
                list.push_back(PrevInstance{i, key, {double(it.transform->world[12]), double(it.transform->world[13]), double(it.transform->world[14])}});
                if (it.tag.part != 0) prev_family_[it.tag.part].emplace_back(it.tag.id, list.size() - 1);
            }
        }
    }
    // reserve the scratch for the whole frame once (the storage persists across frames: no steady-state allocation)
    size_t total = 0;
    for (size_t i = 0; i < cur.count; ++i) total += cur.items[i].count;
    if (scratch_.capacity() < total) scratch_.reserve(total);

    // per instance: the interpolated K (computed once per instance this frame) or the reason it has none
    struct Object { bool ok = false; std::array<double, 16> k; uint64_t prev_key = 0; size_t prev_item = 0; bool have_prev = false; double rot_rad = 0; };
    std::unordered_map<uint64_t, Object> objects;
    for (size_t i = 0; i < cur.count; ++i) {
        const InterpItem& it = cur.items[i];
        InterpOutput o{it.state, it.verts, it.count, it.copy_frame};
        bool matched = false;
        uint64_t accum_key = it.tag.kind == InterpKind::Vertices ? Key(it.tag) : it.tag.id;
        const Object* object = nullptr;
        if (it.tag.id != 0 && it.tag.kind == InterpKind::Rigid && it.transform && have_camera) {
            const uint64_t inst = InstanceKey(it.tag, it.transform->world);
            accum_key = inst;
            auto found = objects.find(inst);
            if (found == objects.end()) {
                Object ob;
                const double tb[3] = {double(it.transform->world[12]), double(it.transform->world[13]), double(it.transform->world[14])};
                double distance = 0;
                PrevInstance* p = NearestPrev(it.tag.id, tb, &distance);
                if (it.tag.part != 0) {
                    // a family member clearly nearer than the instance's own previous one: the producer's order shifted (an
                    // earlier instance culled or come into view), the instance itself did not move
                    double family_distance = 0;
                    PrevInstance* q = NearestInFamily(it.tag.part, tb, &family_distance);
                    if (q && q != p && (!p || family_distance < 0.5 * distance)) {
                        p = q;
                        distance = family_distance;
                        stats_.family_matches++;
                    }
                }
                if (!p) {
                    stats_.unmatched++;
                } else if (distance > options_.object_snap_travel) {
                    stats_.object_snaps++;
                } else {
                    p->taken = true;
                    ob.have_prev = true;
                    ob.prev_key = p->key;
                    ob.prev_item = p->item;
                    if (options_.trace_violations && distance > 0.05)
                        std::fprintf(stderr, "[ps2e.interp] instance %llx (id %llx) matched a previous instance %.3f units away (of %zu)\n",
                                     (unsigned long long)inst, (unsigned long long)it.tag.id, distance, prev_rigid_[it.tag.id].size());
                    // the object's world matrix then and now, decomposed, interpolated
                    double ma[16], mb[16], ta[3], tbb[3], qa[4], qb[4], sa[3], sb[3];
                    ToDouble(prev.items[p->item].transform->world, ma);
                    ToDouble(it.transform->world, mb);
                    bool ok = DecomposeTrs(ma, ta, qa, sa) && DecomposeTrs(mb, tbb, qb, sb);
                    if (!ok) {
                        stats_.unprojectable++;
                    } else if (AngleDegrees(qa, qb) > options_.object_snap_angle_degrees) {
                        stats_.object_snaps++;
                        ok = false;
                    } else {
                        ob.rot_rad = AngleDegrees(qa, qb) * kPi / 180.0;
                    }
                    if (ok) {
                        double t[3], q[4], sc[3], mf[16], screen_inv[16], screen_d[16], kmat[16];
                        for (int c = 0; c < 3; ++c) { t[c] = Lerp(ta[c], tbb[c], fraction); sc[c] = Lerp(sa[c], sb[c], fraction); }
                        Slerp(qa, qb, fraction, q);
                        ComposeTrs(t, q, sc, mf);
                        // a matrix that is not exactly translation . rotation . scale (a title's authored shear, a few parts
                        // per thousand) keeps the NEWEST picture's residual: M_f = TRS_f . (TRS_b^-1 . M_b), so fraction 1 is
                        // the drawn matrix exactly and the residual rides unchanged across the frame
                        {
                            double trs_b[16], trs_b_inv[16], residual[16];
                            ComposeTrs(tbb, qb, sb, trs_b);
                            if (Invert4(trs_b, trs_b_inv)) {
                                Multiply4(trs_b_inv, mb, residual);
                                Multiply4(mf, residual, mf);
                            }
                        }
                        ToDouble(it.transform->screen, screen_d);
                        if (!Invert4(screen_d, screen_inv)) {
                            stats_.unprojectable++;
                        } else {
                            // K = screen^-1 . M_f . world->screen_f: a drawn vertex back to the object, forward to the new frame
                            Multiply4(screen_inv, mf, kmat);
                            Multiply4(kmat, world_to_screen, kmat);
                            for (int c = 0; c < 16; ++c) ob.k[size_t(c)] = kmat[c];
                            ob.ok = true;
                        }
                    }
                }
                found = objects.emplace(inst, ob).first;
            }
            object = &found->second;
            if (object->ok) {
                size_t begin = 0;
                if (ReprojectRigid(it, object->k.data(), cur.off_x, cur.off_y, cur.screen_scale, &begin)) {
                    o.verts = scratch_.data() + begin;
                    o.count = it.count;
                    stats_.rigid_items++;
                    matched = true;
                } else {
                    stats_.unprojectable++;
                    stats_.newest_items++;
                }
            } else {
                stats_.newest_items++;
            }
        } else if (it.tag.id != 0 && it.tag.kind == InterpKind::Vertices) {
            auto p = prev_by_key_.find(Key(it.tag));
            bool steady = p != prev_by_key_.end() && prev.items[p->second].count == it.count && it.count > 0;
            if (steady) {
                // the run's vertices correspond one to one only when every vertex's step is within the bound AND within a
                // few times the run's centroid step (a re-ordered run -- a strip re-batched by the clipper -- moves single
                // vertices by the object's size while the centroid barely moves)
                const InterpItem& pi = prev.items[p->second];
                double cx = 0, cy = 0, worst = 0;
                for (uint32_t v = 0; v < it.count; ++v) {
                    const double dx = double(it.verts[v].x) - double(pi.verts[v].x), dy = double(it.verts[v].y) - double(pi.verts[v].y);
                    cx += dx; cy += dy;
                    worst = std::max(worst, std::max(std::fabs(dx), std::fabs(dy)));
                }
                const double centroid = std::hypot(cx / double(it.count), cy / double(it.count));
                steady = worst <= options_.vertex_run_max_step_px && worst <= 2.0 * centroid + 4.0;
                if (!steady) stats_.vertex_run_jumps++;
            }
            if (steady) {
                const InterpItem& pi = prev.items[p->second];
                const size_t start = scratch_.size();
                scratch_.resize(start + it.count);
                for (uint32_t v = 0; v < it.count; ++v) {
                    ps2e_draw_vertex ov = it.verts[v];
                    ov.x = float(Lerp(pi.verts[v].x, it.verts[v].x, fraction));
                    ov.y = float(Lerp(pi.verts[v].y, it.verts[v].y, fraction));
                    ov.z = float(Lerp(pi.verts[v].z, it.verts[v].z, fraction));
                    scratch_[start + v] = ov;
                }
                o.verts = scratch_.data() + start;
                o.count = it.count;
                stats_.vertex_items++;
                matched = true;
            } else if (p != prev_by_key_.end() && prev.items[p->second].count == it.count) {
                stats_.newest_items++;   // a jump: the newest as it is
            } else {
                if (p == prev_by_key_.end()) stats_.unmatched++;
                else {
                    stats_.vertex_count_mismatch++;
                    if (options_.trace_violations)
                        std::fprintf(stderr, "[ps2e.interp] vertex run %llx part %u: %u vertices now, %u in the previous picture (newest as it is)\n",
                                     (unsigned long long)it.tag.id, it.tag.part, it.count, prev.items[p->second].count);
                }
                stats_.newest_items++;
            }
        } else {
            stats_.newest_items++;
        }
        out.push_back(o);
        // The displacement check's accumulators: per instance, the shown vertices, the newest picture's and the matched
        // previous instance's (all its draws; compared only when both pictures hold the same vertex count of it)
        if (options_.check_displacement && it.tag.id != 0 && it.tag.kind != InterpKind::Newest) {
            Accum& a = accum_[accum_key];
            a.id = it.tag.id;
            for (uint32_t v = 0; v < o.count; ++v) { a.shown.x += o.verts[v].x; a.shown.y += o.verts[v].y; }
            a.shown.count += o.count;
            for (uint32_t v = 0; v < it.count; ++v) {
                a.cur.x += it.verts[v].x; a.cur.y += it.verts[v].y;
                a.sq += double(it.verts[v].x) * it.verts[v].x + double(it.verts[v].y) * it.verts[v].y;
            }
            a.cur.count += it.count;
            a.matched = a.matched || matched;
            if (it.transform) { a.cur.t[0] = it.transform->world[12]; a.cur.t[1] = it.transform->world[13]; a.cur.t[2] = it.transform->world[14]; }
            if (it.tag.kind == InterpKind::Vertices) {
                auto p = prev_by_key_.find(Key(it.tag));
                if (p != prev_by_key_.end()) {
                    const InterpItem& pi = prev.items[p->second];
                    for (uint32_t v = 0; v < pi.count; ++v) { a.prev.x += pi.verts[v].x; a.prev.y += pi.verts[v].y; }
                    a.prev.count += pi.count;
                    a.prev_key = accum_key;
                    a.shown_key = accum_key;
                    a.have_prev = true;
                }
            } else if (object && object->have_prev) {
                a.prev_key = object->prev_key;
                a.shown_key = object->prev_key;   // the previous shown frame was keyed by the previous picture's instance
                a.have_prev = true;
                a.rot_rad = object->rot_rad;
                const InterpTransform* pt = prev.items[object->prev_item].transform;
                if (pt) { a.prev.t[0] = pt->world[12]; a.prev.t[1] = pt->world[13]; a.prev.t[2] = pt->world[14]; }
            }
        }
    }
    // the matched previous rigid instances' centroids: every draw of each (by instance key)
    for (size_t i = 0; options_.check_displacement && i < prev.count; ++i) {
        const InterpItem& it = prev.items[i];
        if (it.tag.id == 0 || it.tag.kind != InterpKind::Rigid || !it.transform) continue;
        const uint64_t key = InstanceKey(it.tag, it.transform->world);
        for (auto& [k, a] : accum_) {
            if (!a.have_prev || a.prev_key != key) continue;
            for (uint32_t v = 0; v < it.count; ++v) { a.prev.x += it.verts[v].x; a.prev.y += it.verts[v].y; }
            a.prev.count += it.count;
        }
    }
    if (options_.check_displacement) CheckDisplacements(false, fraction);
    // (the scratch was reserved for the whole frame above: no output pointer moved)
    return true;
}

// The accumulated frame against the history: for every instance matched this frame whose two pictures hold the same
// vertex count (the same clipping outcome: the centroids move with the object, not with its clipped edge), the shown step
// from the previous shown frame must not exceed the real step between the two pictures by more than the tolerance. Then
// the history moves on: shown_ = this frame's centroids under the instance keys; picture_ likewise when this frame IS a
// picture (with the translation each instance stood at, for the next picture's nearest match).
void SceneInterpolator::CheckDisplacements(bool picture, double fraction) {
    shown_next_.clear();
    if (picture) picture_next_.clear();
    const uint64_t pair = picture_ordinal_;
    for (auto& [key, a] : accum_) {
        if (a.shown.count == 0) continue;
        Centroid now{a.shown.x / double(a.shown.count), a.shown.y / double(a.shown.count), a.shown.count, {a.cur.t[0], a.cur.t[1], a.cur.t[2]}, a.id, pair, 0.0, true};
        if (a.cur.count) {   // the RMS extent of the newest picture's vertices about their centroid (frame pixels)
            const double mx = a.cur.x / double(a.cur.count), my = a.cur.y / double(a.cur.count);
            a.radius_px = std::sqrt(std::max(0.0, a.sq / double(a.cur.count) - mx * mx - my * my));
        }
        // an instance whose drawn vertices spread over more than a quarter of the frame (an environment piece the camera
        // stands inside: its clipped vertex set changes with every camera move) has no centroid worth checking
        const bool measurable = a.radius_px < options_.displacement_max_radius_px;
        if (measurable && a.matched && a.have_prev && a.prev.count == a.cur.count && a.cur.count > 0) {
            const double real = std::hypot(a.cur.x / double(a.cur.count) - a.prev.x / double(a.prev.count),
                                           a.cur.y / double(a.cur.count) - a.prev.y / double(a.prev.count));
            now.carry = (1.0 - fraction) * real;
            auto prev_shown = shown_.find(a.shown_key);
            // compared with the previous shown frame of the same pair (the step between them is within this pair's step) or
            // of the pair before (the earlier frame's remaining part of ITS pair's step carries over: two shown frames may
            // straddle a picture)
            if (prev_shown != shown_.end() && prev_shown->second.valid && prev_shown->second.count == a.cur.count &&
                (prev_shown->second.pair == pair || prev_shown->second.pair + 1 == pair)) {
                const double shown = std::hypot(now.x - prev_shown->second.x, now.y - prev_shown->second.y);
                const double carried = prev_shown->second.pair + 1 == pair ? prev_shown->second.carry : 0.0;
                // a rotating rigid object's centroid follows a chord between the pictures: the extent of its drawn vertices
                // about the centroid times (1 - cos(angle / 2)) is how far off the straight line the half-way frame may sit
                const double arc = a.rot_rad > 0 ? 2.0 * a.radius_px * (1.0 - std::cos(a.rot_rad / 2.0)) : 0.0;
                stats_.displacement_checks++;
                if (shown > real + carried + arc + options_.displacement_tolerance_px) {
                    stats_.displacement_violations++;
                    if (options_.trace_violations)
                        std::fprintf(stderr, "[ps2e.interp] displacement: instance %llx (%llu vertices) %s step %.2f px, the pictures' step %.2f px (carried %.2f, arc %.2f) (prev %.1f,%.1f cur %.1f,%.1f shown-before %.1f,%.1f now %.1f,%.1f)\n",
                                     (unsigned long long)key, (unsigned long long)a.cur.count, picture ? "picture" : "interpolated", shown, real, carried, arc,
                                     a.prev.x / double(a.prev.count), a.prev.y / double(a.prev.count), a.cur.x / double(a.cur.count), a.cur.y / double(a.cur.count),
                                     prev_shown->second.x, prev_shown->second.y, now.x, now.y);
                }
            }
        }
        shown_next_[key] = now;
        if (picture) picture_next_[key] = now;
    }
    std::swap(shown_, shown_next_);
    if (picture) std::swap(picture_, picture_next_);
    accum_.clear();
}

void SceneInterpolator::NoteShownPicture(const InterpScene& cur) {
    if (!options_.check_displacement) return;
    // the picture shown as it is: each instance's step from the previous shown frame against its step from the previous
    // PICTURE -- the same instance key (a static object, a vertex run) or, for a rigid instance that moved, the previous
    // picture's nearest instance of the same identity. The picture closes the pair the frames before it were interpolated
    // towards (the same items), else it opens a new ordinal
    if (cur.ordinal) picture_ordinal_ = cur.ordinal;
    else if (reinterpret_cast<uint64_t>(cur.items) != last_cur_items_) {
        last_cur_items_ = reinterpret_cast<uint64_t>(cur.items);
        picture_ordinal_++;
    }
    accum_.clear();
    for (size_t i = 0; i < cur.count; ++i) {
        const InterpItem& it = cur.items[i];
        if (it.tag.id == 0 || it.tag.kind == InterpKind::Newest || it.count == 0) continue;
        const uint64_t key = it.tag.kind == InterpKind::Rigid && it.transform ? InstanceKey(it.tag, it.transform->world)
                           : it.tag.kind == InterpKind::Vertices ? Key(it.tag) : it.tag.id;
        Accum& a = accum_[key];
        for (uint32_t v = 0; v < it.count; ++v) { a.shown.x += it.verts[v].x; a.shown.y += it.verts[v].y; }
        a.shown.count += it.count;
        a.cur = a.shown;
        a.matched = true;
        a.id = it.tag.id;
        if (it.transform) { a.cur.t[0] = it.transform->world[12]; a.cur.t[1] = it.transform->world[13]; a.cur.t[2] = it.transform->world[14]; }
    }
    for (auto& [key, a] : accum_) {
        auto last = picture_.find(key);
        const Centroid* c = nullptr;
        uint64_t prev_key = key;
        if (last != picture_.end() && last->second.valid) {
            c = &last->second;
        } else {
            double best = 0;
            for (const auto& [pk, pc] : picture_) {
                if (!pc.valid || pc.id != a.id) continue;
                const double d = std::sqrt((pc.t[0] - a.cur.t[0]) * (pc.t[0] - a.cur.t[0]) + (pc.t[1] - a.cur.t[1]) * (pc.t[1] - a.cur.t[1]) +
                                           (pc.t[2] - a.cur.t[2]) * (pc.t[2] - a.cur.t[2]));
                if (d <= options_.object_snap_travel && (!c || d < best)) { c = &pc; best = d; prev_key = pk; }
            }
        }
        if (c) {
            a.prev = Centroid{c->x * double(c->count), c->y * double(c->count), c->count, {c->t[0], c->t[1], c->t[2]}, c->id, c->pair, 0.0, true};
            a.prev_key = prev_key;
            a.shown_key = key;   // the frame before this picture was keyed by this picture's instance
            a.have_prev = true;
        }
    }
    CheckDisplacements(true, 1.0);
}

}  // namespace ps2e
}  // namespace ps2r
