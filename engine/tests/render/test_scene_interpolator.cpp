// tests/render/test_scene_interpolator.cpp -- SceneInterpolator (engine/platform/ps2e/scene_interpolator.h): the TRS
// decomposition round trip, the slerp, the camera between two poses (and the cut thresholds), a rigid object translated /
// rotated between two pictures landing midway at fraction 0.5 (re-projected from its drawn vertices), a vertex run lerped,
// unmatched / untagged / behind-camera draws passed through as the newest, the producer's cut, the displacement check,
// the fraction clamped. A simple perspective camera in the title's row-vector convention. No window, no GL.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "engine/platform/ps2e/scene_interpolator.h"

using namespace ps2r::ps2e;

static int g_fail = 0;
#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (!(cond)) { std::printf("FAIL: %s\n", name); g_fail = 1; }       \
        else std::printf("ok: %s\n", name);                                \
    } while (0)

namespace {
constexpr float kOffX = 2048.f - 256.f, kOffY = 2048.f - 256.f;   // a 512x512 frame's GS origin

// row-vector matrices: p' = p . M
void Identity(float m[16]) { for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.f : 0.f; }
void Translation(float m[16], float x, float y, float z) { Identity(m); m[12] = x; m[13] = y; m[14] = z; }
void RotationY(float m[16], double deg) {
    Identity(m);
    const double c = std::cos(deg * 3.14159265358979 / 180), s = std::sin(deg * 3.14159265358979 / 180);
    m[0] = float(c); m[2] = float(-s); m[8] = float(s); m[10] = float(c);
}
void Mul(const float a[16], const float b[16], float out[16]) {
    double da[16], db[16], dout[16];
    for (int i = 0; i < 16; ++i) { da[i] = a[i]; db[i] = b[i]; }
    SceneInterpolator::Multiply4(da, db, dout);
    for (int i = 0; i < 16; ++i) out[i] = float(dout[i]);
}
// camera at `eye` looking down +z (world->camera = a translation); camera->screen: x_gs = 2048 + 256 * x/z, y_gs likewise,
// z_gs = 2^20 / z (a GS-like depth), w = z
void Camera(InterpCamera& cam, float ex, float ey, float ez) {
    cam.valid = true;
    Translation(cam.to_camera, -ex, -ey, -ez);
    // (x, y, z, 1) . P = (256 x + 2048 z, 256 y + 2048 z, 2^20, z); divided by w = z: x_gs = 2048 + 256 x / z, z_gs = 2^20 / z
    float* p = cam.camera_to_screen;
    for (int i = 0; i < 16; ++i) p[i] = 0;
    p[0] = 256.f;        // row x: x -> column 0
    p[5] = 256.f;        // row y: y -> column 1
    p[8] = 2048.f;       // row z: the GS centre scales with w on columns 0 and 1, and z -> w (column 3)
    p[9] = 2048.f;
    p[11] = 1.f;
    p[14] = 1048576.f;   // row 1: the constant 2^20 on column 2 (z_gs = 2^20 / w)
}
// project one model-space point through world then camera to a frame-pixel vertex
ps2e_draw_vertex Project(const float world[16], const InterpCamera& cam, float x, float y, float z, float s, float t) {
    float ws[16], a[16];
    Mul(cam.to_camera, cam.camera_to_screen, ws);
    Mul(world, ws, a);
    const double hx = x * a[0] + y * a[4] + z * a[8] + a[12];
    const double hy = x * a[1] + y * a[5] + z * a[9] + a[13];
    const double hz = x * a[2] + y * a[6] + z * a[10] + a[14];
    const double hw = x * a[3] + y * a[7] + z * a[11] + a[15];
    ps2e_draw_vertex v{};
    v.x = float(hx / hw - kOffX);
    v.y = float(hy / hw - kOffY);
    v.z = float(hz / hw);
    v.q = float(1.0 / hw);
    v.u = s * v.q;
    v.v = t * v.q;
    v.r = v.g = v.b = 0x80; v.a = 0x80;
    return v;
}
struct Quad {
    std::vector<ps2e_draw_vertex> verts;
    InterpTransform transform;
    ps2e_draw_state state{};
    InterpItem item;
};
// a unit quad (two triangles) at the model origin, under `world`, as the producer records it
void MakeQuad(Quad& q, const float world[16], const InterpCamera& cam, uint64_t id, InterpKind kind = InterpKind::Rigid) {
    q.verts.clear();
    const float c[6][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {-0.5f, 0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    for (auto& k : c) q.verts.push_back(Project(world, cam, k[0], k[1], 0.f, k[0] + 0.5f, k[1] + 0.5f));
    for (int i = 0; i < 16; ++i) q.transform.world[i] = world[i];
    float ws[16];
    Mul(cam.to_camera, cam.camera_to_screen, ws);
    Mul(world, ws, q.transform.screen);
    q.state.texture = 7;
    q.item = InterpItem{&q.state, q.verts.data(), uint32_t(q.verts.size()), false, InterpTag{id, 0, kind}, &q.transform};
}
double Cx(const InterpOutput& o) { double s = 0; for (uint32_t i = 0; i < o.count; ++i) s += o.verts[i].x; return s / o.count; }
double Cx(const Quad& q) { double s = 0; for (auto& v : q.verts) s += v.x; return s / double(q.verts.size()); }
}  // namespace

int main() {
    // TRS round trip and slerp
    {
        double t[3] = {1, 2, 3}, q[4] = {0, std::sin(0.3), 0, std::cos(0.3)}, s[3] = {2, 3, 4}, m[16], t2[3], q2[4], s2[3];
        SceneInterpolator::ComposeTrs(t, q, s, m);
        CHECK(SceneInterpolator::DecomposeTrs(m, t2, q2, s2), "a TRS matrix decomposes");
        bool same = true;
        for (int i = 0; i < 3; ++i) same = same && std::fabs(t[i] - t2[i]) < 1e-9 && std::fabs(s[i] - s2[i]) < 1e-9;
        const double dq = std::fabs(q[0] * q2[0] + q[1] * q2[1] + q[2] * q2[2] + q[3] * q2[3]);
        CHECK(same && std::fabs(dq - 1.0) < 1e-9, "translation, scale and rotation come back");
        double a[4] = {0, 0, 0, 1}, b[4] = {0, std::sin(0.5), 0, std::cos(0.5)}, h[4];
        SceneInterpolator::Slerp(a, b, 0.5, h);
        CHECK(std::fabs(h[1] - std::sin(0.25)) < 1e-9 && std::fabs(h[3] - std::cos(0.25)) < 1e-9, "slerp halves the angle");
        CHECK(std::fabs(SceneInterpolator::AngleDegrees(a, b) - 1.0 * 180 / 3.14159265358979) < 1e-6, "the angle between two quaternions");
        double id[16], inv[16];
        SceneInterpolator::ComposeTrs(t, q, s, m);
        CHECK(SceneInterpolator::Invert4(m, inv), "a TRS matrix inverts");
        SceneInterpolator::Multiply4(m, inv, id);
        bool isid = true;
        for (int i = 0; i < 16; ++i) isid = isid && std::fabs(id[i] - ((i % 5 == 0) ? 1.0 : 0.0)) < 1e-9;
        CHECK(isid, "M . M^-1 = I");
    }

    // the camera between two poses; the cut thresholds
    {
        InterpCamera a, b;
        Camera(a, 0, 0, 0);
        Camera(b, 1, 0, 0);
        double v[16], p[16];
        bool cut = true;
        SceneInterpolator::Options opt;
        CHECK(SceneInterpolator::InterpolateCamera(a, b, 0.5, opt, v, p, &cut) && !cut, "a one-unit camera step interpolates");
        CHECK(std::fabs(v[12] + 0.5) < 1e-9, "the camera's position at 0.5 is halfway (world->camera translation -0.5)");
        Camera(b, 5, 0, 0);
        CHECK(SceneInterpolator::InterpolateCamera(a, b, 0.5, opt, v, p, &cut) && cut, "a five-unit step is a cut");
    }

    // a rigid quad translated one unit in x between two pictures: at 0.5 its centroid is midway (re-projected)
    {
        InterpCamera cam;
        Camera(cam, 0, 0, 0);
        float w0[16], w1[16];
        Translation(w0, 0, 0, 10);
        Translation(w1, 1, 0, 10);
        Quad q0, q1;
        MakeQuad(q0, w0, cam, 42);
        MakeQuad(q1, w1, cam, 42);
        InterpScene prev{&q0.item, 1, cam, kOffX, kOffY, 1.f, false}, cur{&q1.item, 1, cam, kOffX, kOffY, 1.f, false};
        SceneInterpolator si;
        std::vector<InterpOutput> out;
        CHECK(si.Interpolate(prev, cur, 0.5, out) && out.size() == 1 && out[0].count == 6, "one interpolated draw");
        const double mid = 0.5 * (Cx(q0) + Cx(q1));
        CHECK(std::fabs(Cx(out[0]) - mid) < 0.05, "the translated quad's centroid lands midway at 0.5");
        CHECK(out[0].verts != q1.verts.data() && out[0].state == &q1.state, "the vertices are the interpolator's, the state the newest's");
        std::vector<InterpOutput> o0, o1;
        si.Interpolate(prev, cur, 0.0, o0);
        const double cx0 = Cx(o0[0]);   // (the scratch is this call's: read before the next)
        si.Interpolate(prev, cur, 1.0, o1);
        CHECK(std::fabs(cx0 - Cx(q0)) < 0.05 && std::fabs(Cx(o1[0]) - Cx(q1)) < 0.05, "fraction 0 is the previous picture, 1 the newest");
        // S/T/Q rescaled: at fraction 1 the re-projected q equals the recorded q
        CHECK(std::fabs(o1[0].verts[0].q - q1.verts[0].q) < 1e-6f && std::fabs(o1[0].verts[0].u - q1.verts[0].u) < 1e-5f, "S, T and Q rescale by the depth ratio");
        CHECK(si.Stats().rigid_items == 3 && si.Stats().newest_items == 0, "census: three rigid re-projections");
        CHECK(si.ScratchBytes() >= 6 * sizeof(ps2e_draw_vertex), "the scratch holds the frame's vertices");
        // the fraction is clamped: 1.5 reads as 1
        std::vector<InterpOutput> o2;
        si.Interpolate(prev, cur, 1.5, o2);
        CHECK(std::fabs(Cx(o2[0]) - Cx(q1)) < 0.05, "never extrapolated: 1.5 is the newest picture");
    }

    // the 12.4 raster: matrices whose x/y land in 1/16 pixel (the title's toScreen) with screen_scale 16 give the same frame
    {
        InterpCamera cam16;
        Camera(cam16, 0, 0, 0);
        for (int c = 0; c < 2; ++c) { cam16.camera_to_screen[c] *= 16.f; }         // columns 0 and 1 of row 0 / row 1
        cam16.camera_to_screen[4] *= 16.f; cam16.camera_to_screen[5] *= 16.f;
        cam16.camera_to_screen[8] *= 16.f; cam16.camera_to_screen[9] *= 16.f;
        float w0[16], w1[16];
        Translation(w0, 0, 0, 10);
        Translation(w1, 1, 0, 10);
        Quad q0, q1;
        MakeQuad(q0, w0, cam16, 42);
        MakeQuad(q1, w1, cam16, 42);
        for (auto& v : q0.verts) { v.x = (v.x + kOffX) / 16.f - kOffX; v.y = (v.y + kOffY) / 16.f - kOffY; }   // the producer's /16
        for (auto& v : q1.verts) { v.x = (v.x + kOffX) / 16.f - kOffX; v.y = (v.y + kOffY) / 16.f - kOffY; }
        InterpScene prev{&q0.item, 1, cam16, kOffX, kOffY, 16.f, false}, cur{&q1.item, 1, cam16, kOffX, kOffY, 16.f, false};
        SceneInterpolator si;
        std::vector<InterpOutput> out;
        si.Interpolate(prev, cur, 0.5, out);
        CHECK(std::fabs(Cx(out[0]) - 0.5 * (Cx(q0) + Cx(q1))) < 0.05, "screen_scale 16: the 12.4-raster matrices re-project to the midway frame");
        si.Interpolate(prev, cur, 1.0, out);
        double err = 0;
        for (int i = 0; i < 6; ++i) err = std::max(err, double(std::fabs(out[0].verts[i].x - q1.verts[i].x)));
        CHECK(err < 0.01, "screen_scale 16: fraction 1 reproduces the drawn vertices");
    }

    // rotation: a quad turned 60 degrees about y between pictures -- the interpolated frame is the quad at 30 degrees (its
    // right edge's projected x matches the 30-degree quad's), not a lerp of the two drawings
    {
        InterpCamera cam;
        Camera(cam, 0, 0, 0);
        float r0[16], r1[16], rh[16], t[16], w0[16], w1[16], wh[16];
        RotationY(r0, 0); RotationY(r1, 60); RotationY(rh, 30);
        Translation(t, 0, 0, 10);
        Mul(r0, t, w0); Mul(r1, t, w1); Mul(rh, t, wh);
        Quad q0, q1, qh;
        MakeQuad(q0, w0, cam, 7); MakeQuad(q1, w1, cam, 7); MakeQuad(qh, wh, cam, 7);
        InterpScene prev{&q0.item, 1, cam, kOffX, kOffY, 1.f, false}, cur{&q1.item, 1, cam, kOffX, kOffY, 1.f, false};
        SceneInterpolator si;
        std::vector<InterpOutput> out;
        si.Interpolate(prev, cur, 0.5, out);
        double err = 0;
        for (int i = 0; i < 6; ++i) err = std::max(err, double(std::fabs(out[0].verts[i].x - qh.verts[i].x)) + std::fabs(out[0].verts[i].y - qh.verts[i].y));
        CHECK(err < 0.05, "the half-way frame is the quad drawn at the half angle (slerp, re-projected)");
        const double lerp_x = 0.5 * (q0.verts[1].x + q1.verts[1].x);
        CHECK(std::fabs(lerp_x - qh.verts[1].x) > 0.5, "(and a raw lerp of the drawings would not be: the test discriminates)");
    }

    // the camera moves, the object stands: the object's drawn vertices follow the interpolated camera
    {
        InterpCamera c0, c1;
        Camera(c0, 0, 0, 0);
        Camera(c1, 1, 0, 0);
        float w[16];
        Translation(w, 0, 0, 10);
        Quad q0, q1;
        MakeQuad(q0, w, c0, 9); MakeQuad(q1, w, c1, 9);
        InterpScene prev{&q0.item, 1, c0, kOffX, kOffY, 1.f, false}, cur{&q1.item, 1, c1, kOffX, kOffY, 1.f, false};
        SceneInterpolator si;
        std::vector<InterpOutput> out;
        si.Interpolate(prev, cur, 0.5, out);
        CHECK(std::fabs(Cx(out[0]) - 0.5 * (Cx(q0) + Cx(q1))) < 0.05, "a camera pan: the static object lands midway");
    }

    // a clipped instance: the previous picture's opaque draw of it is drawn too, re-projected, before the newest's; a
    // blended one is not (it would blend twice)
    {
        InterpCamera c0, c1;
        Camera(c0, 0, 0, 0);
        Camera(c1, 0, 0, 1);   // the camera moved forward
        float w[16];
        Translation(w, 0, 0, 10);
        Quad q0, q1;
        MakeQuad(q0, w, c0, 30); MakeQuad(q1, w, c1, 30);
        q0.state.depth_write = q1.state.depth_write = 1;
        q0.item.tag.clipped = q1.item.tag.clipped = true;
        InterpScene prev{&q0.item, 1, c0, kOffX, kOffY, 1.f, false}, cur{&q1.item, 1, c1, kOffX, kOffY, 1.f, false};
        SceneInterpolator si;
        std::vector<InterpOutput> out;
        si.Interpolate(prev, cur, 0.5, out);
        CHECK(out.size() == 2 && out[0].state == &q0.state && out[1].state == &q1.state && si.Stats().cover_items == 1 &&
                  std::fabs(Cx(out[0]) - Cx(out[1])) < 0.05,
              "a clipped instance: its previous draw re-projected first, landing where the newest's does");
        q0.state.blend = 1;
        SceneInterpolator blended;
        blended.Interpolate(prev, cur, 0.5, out);
        CHECK(out.size() == 1 && blended.Stats().cover_items == 0, "a clipped instance's blended draw is not drawn twice");
    }

    // a family: two standing instances named by their draw order (ids 21, 22); the first is culled, so the second is drawn
    // under the first's id -- it pairs with its own previous instance (the nearer one of its family), it does not slide
    {
        InterpCamera cam;
        Camera(cam, 0, 0, 0);
        float wa[16], wb[16];
        Translation(wa, 0, 0, 10); Translation(wb, 2, 0, 10);
        Quad a0, b0, b1;
        MakeQuad(a0, wa, cam, 21); MakeQuad(b0, wb, cam, 22); MakeQuad(b1, wb, cam, 21);
        a0.item.tag.part = b0.item.tag.part = b1.item.tag.part = 7;
        InterpItem prev_items[2] = {a0.item, b0.item};
        InterpScene prev{prev_items, 2, cam, kOffX, kOffY, 1.f, false}, cur{&b1.item, 1, cam, kOffX, kOffY, 1.f, false};
        SceneInterpolator si;
        std::vector<InterpOutput> out;
        si.Interpolate(prev, cur, 0.5, out);
        CHECK(out.size() == 1 && std::fabs(Cx(out[0]) - Cx(b1)) < 0.05 && si.Stats().family_matches == 1,
              "a family: the instance pairs with the nearer one of its family when the draw order shifted");
        // without a family the order's pairing stands (the instance slides from the first one's place)
        b1.item.tag.part = 0;
        SceneInterpolator plain;
        plain.Interpolate(prev, InterpScene{&b1.item, 1, cam, kOffX, kOffY, 1.f, false}, 0.5, out);
        CHECK(std::fabs(Cx(out[0]) - Cx(b1)) > 0.5, "(and the order alone would not: the test discriminates)");
    }

    // unmatched, untagged, a vertex run, the producer's cut, a cut by threshold, behind the camera
    {
        InterpCamera cam;
        Camera(cam, 0, 0, 0);
        float w0[16], w1[16];
        Translation(w0, 0, 0, 10); Translation(w1, 0.2f, 0, 10);
        Quad a0, a1, b1, u0, u1, v0, v1;
        MakeQuad(a0, w0, cam, 1); MakeQuad(a1, w1, cam, 1);
        MakeQuad(b1, w1, cam, 2);                              // spawned this tick
        MakeQuad(u0, w0, cam, 0, InterpKind::Newest); MakeQuad(u1, w1, cam, 0, InterpKind::Newest);
        MakeQuad(v0, w0, cam, 3, InterpKind::Vertices); MakeQuad(v1, w1, cam, 3, InterpKind::Vertices);
        InterpItem prev_items[3] = {a0.item, u0.item, v0.item};
        InterpItem cur_items[4] = {a1.item, b1.item, u1.item, v1.item};
        InterpScene prev{prev_items, 3, cam, kOffX, kOffY, 1.f, false}, cur{cur_items, 4, cam, kOffX, kOffY, 1.f, false};
        SceneInterpolator si;
        std::vector<InterpOutput> out;
        CHECK(si.Interpolate(prev, cur, 0.5, out) && out.size() == 4, "every newest draw has an output, in order");
        CHECK(out[1].verts == b1.verts.data() && out[2].verts == u1.verts.data(), "the spawned object and the untagged draw are the newest as they are");
        CHECK(std::fabs(Cx(out[3]) - 0.5 * (Cx(v0) + Cx(v1))) < 1e-3, "the vertex run is lerped per vertex");
        CHECK(si.Stats().unmatched == 1 && si.Stats().vertex_items == 1 && si.Stats().rigid_items == 1 && si.Stats().newest_items == 2, "census: one unmatched, one vertex run, one rigid, two newest");
        // the producer's cut: everything newest, the frame counted as snapped
        cur.cut = true;
        CHECK(!si.Interpolate(prev, cur, 0.5, out) && out[0].verts == a1.verts.data() && si.Stats().snapped_frames == 1, "the producer's cut snaps the frame to the newest");
        cur.cut = false;
        // a vertex run whose count changed: newest
        v1.verts.resize(3);
        v1.item.count = 3;
        cur_items[3] = v1.item;
        si.Interpolate(prev, cur, 0.5, out);
        CHECK(out[3].verts == v1.verts.data() && out[3].count == 3, "a vertex run with another count is the newest as it is");
        // an object that leapt: snapped
        float far_w[16];
        Translation(far_w, 50, 0, 10);
        MakeQuad(a1, far_w, cam, 1);
        cur_items[0] = a1.item;
        si.Interpolate(prev, cur, 0.5, out);
        CHECK(out[0].verts == a1.verts.data() && si.Stats().object_snaps == 1, "an object that leapt beyond the threshold is the newest as it is");
        // behind the camera: the object moved to z = -10 (its drawn w negative): the re-projection refuses, newest
        float behind[16];
        Translation(behind, 0, 0, 10);
        MakeQuad(a1, behind, cam, 1);
        cur_items[0] = a1.item;
        InterpCamera cam_b;
        Camera(cam_b, 0, 0, 25);   // the interpolated camera passes the quad: at 0.5 the camera is at z 12.5 > 10
        cur.camera = cam_b;
        SceneInterpolator::Options loose;
        loose.camera_cut_travel = 100;
        si.SetOptions(loose);
        si.Interpolate(prev, cur, 0.5, out);
        CHECK(out[0].verts == a1.verts.data() && si.Stats().unprojectable >= 1, "a vertex behind the interpolated camera leaves the draw as it is");
    }

    // the displacement check: a matched object's step between shown frames never exceeds the pictures' step; a wrong
    // identity (two different objects sharing an id) would
    {
        InterpCamera cam;
        Camera(cam, 0, 0, 0);
        float w0[16], w1[16], w2[16];
        Translation(w0, 0, 0, 10); Translation(w1, 1, 0, 10); Translation(w2, 2, 0, 10);
        Quad q0, q1, q2;
        MakeQuad(q0, w0, cam, 5); MakeQuad(q1, w1, cam, 5); MakeQuad(q2, w2, cam, 5);
        InterpScene s0{&q0.item, 1, cam, kOffX, kOffY, 1.f, false}, s1{&q1.item, 1, cam, kOffX, kOffY, 1.f, false}, s2{&q2.item, 1, cam, kOffX, kOffY, 1.f, false};
        SceneInterpolator si;
        std::vector<InterpOutput> out;
        si.NoteShownPicture(s0);
        si.Interpolate(s0, s1, 0.5, out);
        si.NoteShownPicture(s1);
        si.Interpolate(s1, s2, 0.5, out);
        si.NoteShownPicture(s2);
        CHECK(si.Stats().displacement_checks == 4 && si.Stats().displacement_violations == 0, "uniform motion: four checks, no violation");
        // a wrong match: the "previous" picture holds another object under the same id, far away
        float far_w[16];
        Translation(far_w, -3, 0, 10);
        Quad wrong;
        MakeQuad(wrong, far_w, cam, 5);
        InterpScene sw{&wrong.item, 1, cam, kOffX, kOffY, 1.f, false};
        si.ResetHistory();
        si.NoteShownPicture(s1);
        si.Interpolate(s1, s2, 0.5, out);          // the shown step: half a unit
        SceneInterpolator::Options loose;
        loose.object_snap_travel = 100;
        si.SetOptions(loose);
        si.Interpolate(sw, s2, 0.5, out);          // the interpolated frame sits between -3 and 2 (at -0.5): off the real path
        si.NoteShownPicture(s2);                   // the picture's step from that frame (2.5) exceeds its step from the previous picture (1)
        CHECK(si.Stats().displacement_violations == 1, "a wrong identity match is caught by the displacement check");
    }
    return g_fail;
}
