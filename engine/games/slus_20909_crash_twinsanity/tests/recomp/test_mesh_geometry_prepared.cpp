// MeshGeometry::Prepare + BuildPrepared against MeshGeometry::Build: the same draws (count, order, fan/body) with
// BIT-IDENTICAL vertices over randomized meshes and views -- clipped and unclipped instances, guard bands that cut
// triangles into fans, denormal and huge coordinates, restarts, degenerate (coincident) screen points, scrolls,
// colour overrides -- and the same Result on every rejected mesh and state. No assets, no captured data: the two
// paths are the same source arithmetic reordered, and this is the proof they stayed so.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace {
using Geometry = title::MeshGeometry;
using Draw = ps2r::gs::DrawIr;
using Rec = ps2r::gs::VertexRec;
size_t cases = 0, draws_compared = 0, vertices_compared = 0, fans_seen = 0, clipped_views = 0;
[[noreturn]] void Fail(const char* expression, int line) {
    std::fprintf(stderr, "FAIL case %zu line %d: %s\n", cases, line, expression);
    std::exit(1);
}
#define REQUIRE(value) do { if (!(value)) Fail(#value, __LINE__); } while (false)

std::mt19937 rng(0x7e5e);
float Uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
int Pick(int n) { return int(std::uniform_int_distribution<int>(0, n - 1)(rng)); }
float Bits(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
// Mostly ordinary coordinates; sometimes a denormal, a signed zero, a huge value, an exact integer
float Coordinate(float scale) {
    switch (Pick(24)) {
        case 0: return Bits(uint32_t(Pick(0x7fffff)) | (Pick(2) ? 0x80000000u : 0));   // denormal
        case 1: return Pick(2) ? -0.f : 0.f;
        case 2: return Uniform(-1e30f, 1e30f);
        case 3: return float(Pick(2001) - 1000);
        default: return Uniform(-scale, scale);
    }
}

Geometry::Mesh RandomMesh(size_t strips) {
    Geometry::Mesh mesh;
    for (size_t s = 0; s < strips; ++s) {
        Geometry::Strip strip;
        const int count = 3 + Pick(36);
        strip.vertices.resize(size_t(count));
        for (auto& v : strip.vertices) {
            for (auto& p : v.position) p = Coordinate(200.f);
            v.stq = {Uniform(-2.f, 2.f), Uniform(-2.f, 2.f), Pick(5) ? 1.f : Uniform(0.f, 3.f)};
            if (Pick(16) == 0) v.stq[0] = Coordinate(1.f);
            for (auto& c : v.colour) c = uint8_t(Pick(256));
            v.restart = Pick(9) == 0;
        }
        // a few coincident screen points: duplicate a neighbour's position
        if (count > 4 && Pick(3) == 0) strip.vertices[2].position = strip.vertices[1].position;
        mesh.strips.push_back(std::move(strip));
    }
    return mesh;
}

Geometry::Matrix RandomMatrix(float scale, bool projective) {
    Geometry::Matrix m;
    for (auto& row : m) for (auto& v : row) v = Uniform(-scale, scale);
    if (projective) {   // a perspective-like w row so clip space is exercised
        m[0][3] = Uniform(-0.01f, 0.01f);
        m[1][3] = Uniform(-0.01f, 0.01f);
        m[2][3] = Uniform(0.5f, 2.f);
        m[3][3] = Uniform(50.f, 400.f);
    }
    if (Pick(20) == 0) m[Pick(4)][Pick(4)] = Coordinate(1.f);
    return m;
}

Geometry::View RandomView() {
    Geometry::View view;
    view.clip = RandomMatrix(Pick(2) ? 1.f : 4.f, true);
    view.screen = RandomMatrix(Pick(2) ? 30.f : 3000.f, true);
    view.guard = {Uniform(0.5f, 1.5f), Uniform(0.5f, 1.5f), Uniform(0.5f, 1.5f), Uniform(0.5f, 1.5f)};
    if (Pick(4) == 0) view.guard = {1.f, 1.f, 1.f, 1.f};
    view.scroll = Pick(3) ? std::array<float, 2>{0.f, 0.f} : std::array<float, 2>{Uniform(-1.f, 1.f), Uniform(-1.f, 1.f)};
    view.clipped = Pick(3) != 0;
    return view;
}

Draw State() {
    Draw st{};
    st.prim_type = 4;
    st.tex_enabled = true;
    st.off_x = float(Pick(3) ? 1792 : 2048 - 256);
    st.off_y = float(Pick(3) ? 1792 : 2048 - 224);
    return st;
}

struct Published { std::vector<Rec> verts; bool fan; };
void Collect(void* user, const Rec* verts, size_t count, bool fan) {
    auto& out = *static_cast<std::vector<Published>*>(user);
    out.push_back({std::vector<Rec>(verts, verts + count), fan});
}

uint32_t FloatBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
void RequireSameVertex(const Rec& a, const Rec& b) {
    REQUIRE(FloatBits(a.x) == FloatBits(b.x));
    REQUIRE(FloatBits(a.y) == FloatBits(b.y));
    REQUIRE(FloatBits(a.z) == FloatBits(b.z));
    REQUIRE(FloatBits(a.s) == FloatBits(b.s));
    REQUIRE(FloatBits(a.t) == FloatBits(b.t));
    REQUIRE(FloatBits(a.q) == FloatBits(b.q));
    REQUIRE(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a);
    REQUIRE(a.draw_kick == b.draw_kick);
    REQUIRE(a.uv_u == b.uv_u && a.uv_v == b.uv_v && a.fog == b.fog);
}

// One mesh, one view, one state: Build vs Prepare + BuildPrepared (optionally through a colour override, in
// which case Build sees a copy of the mesh with those colours)
void Compare(const Geometry::Mesh& mesh, const Geometry::View& view, const Draw& state, bool override_colours) {
    ++cases;
    Geometry::Prepared prepared;
    REQUIRE(Geometry::Prepare(mesh, prepared) == Geometry::Result::Ok);
    size_t total = 0;
    for (const auto& strip : mesh.strips) total += strip.vertices.size();
    REQUIRE(prepared.vertex_count == total);
    REQUIRE(prepared.strips.size() == mesh.strips.size());

    Geometry::Mesh reference = mesh;
    std::vector<std::array<uint8_t, 4>> colours;
    if (override_colours) {
        colours.resize(total);
        size_t at = 0;
        for (auto& strip : reference.strips)
            for (auto& v : strip.vertices) {
                for (auto& c : colours[at]) c = uint8_t(Pick(256));
                v.colour = colours[at++];
            }
    }
    std::vector<Draw> built;
    const auto expected = Geometry::Build(reference, {view}, state, built);
    std::vector<Published> published;
    const auto actual = Geometry::BuildPrepared(prepared, view, state, override_colours ? colours.data() : nullptr,
                                                &Collect, &published);
    REQUIRE(expected == actual);
    if (expected != Geometry::Result::Ok) return;
    if (view.clipped) ++clipped_views;
    REQUIRE(built.size() == published.size());
    for (size_t d = 0; d < built.size(); ++d) {
        REQUIRE((built[d].prim_type == 5) == published[d].fan);
        if (published[d].fan) ++fans_seen;
        REQUIRE(built[d].verts.size() == published[d].verts.size());
        REQUIRE(built[d].vertex_count == published[d].verts.size());
        for (size_t i = 0; i < built[d].verts.size(); ++i) RequireSameVertex(built[d].verts[i], published[d].verts[i]);
        vertices_compared += published[d].verts.size();
    }
    draws_compared += built.size();
}

// View::classes / classed: the same vertices in the same order as the build without them, the sink's calls cut where the
// class changes, `classed` called before the first run and at each change only, and every body triangle under the class
// of the vertex that ends it (each vertex's colour its index: the triangle's third vertex is the strip vertex `at`)
size_t classed_cases = 0, classed_runs = 0;
void CompareClasses(const Geometry::Mesh& mesh, const Geometry::View& view, const Draw& state) {
    Geometry::Prepared prepared;
    REQUIRE(Geometry::Prepare(mesh, prepared) == Geometry::Result::Ok);
    std::vector<std::array<uint8_t, 4>> colours(prepared.vertex_count);
    for (size_t at = 0; at < colours.size(); ++at) colours[at] = {uint8_t(at), uint8_t(at >> 8), uint8_t(at >> 16), 0x80};
    std::vector<uint8_t> classes(prepared.vertex_count);
    const int mode = Pick(3);   // random classes, all 1, runs of a few vertices
    for (size_t at = 0; at < classes.size(); ++at) classes[at] = uint8_t(mode == 0 ? Pick(2) : mode == 1 ? 1 : (at / 5) & 1);
    std::vector<Published> plain;
    if (Geometry::BuildPrepared(prepared, view, state, colours.data(), &Collect, &plain) != Geometry::Result::Ok) return;
    ++classed_cases;
    struct Out {
        const std::vector<uint8_t>* classes;
        std::vector<Published> published;
        std::vector<int> calls;   // the class `classed` reported before each sink call
        int current = -1, changes = 0;
    } out{&classes, {}, {}, -1, 0};
    Geometry::View cv = view;
    cv.classes = classes.data();
    cv.classed = [](void* user, uint8_t c) {
        auto* o = static_cast<Out*>(user);
        REQUIRE(int(c) != o->current);   // only at a change
        o->current = c;
        o->changes++;
    };
    const auto sink = [](void* user, const Rec* verts, size_t count, bool fan) {
        auto* o = static_cast<Out*>(user);
        REQUIRE(o->current >= 0);   // a class reported before the first triangles
        o->published.push_back({std::vector<Rec>(verts, verts + count), fan});
        o->calls.push_back(o->current);
        if (!fan)
            for (size_t t = 0; t + 3 <= count; t += 3) {
                const Rec& c = verts[t + 2];
                const size_t at = size_t(c.r) | size_t(c.g) << 8 | size_t(c.b) << 16;
                REQUIRE(at < o->classes->size() && (*o->classes)[at] == o->current);
            }
    };
    REQUIRE(Geometry::BuildPrepared(prepared, cv, state, colours.data(), sink, &out) == Geometry::Result::Ok);
    std::vector<Rec> a, b;
    for (const auto& p : plain) a.insert(a.end(), p.verts.begin(), p.verts.end());
    for (const auto& p : out.published) b.insert(b.end(), p.verts.begin(), p.verts.end());
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) RequireSameVertex(a[i], b[i]);
    if (mode == 1) REQUIRE(out.published.size() == plain.size() && out.changes == (plain.empty() ? 0 : 1));
    classed_runs += size_t(out.changes);
}

void Rejections() {
    const Draw state = State();
    Geometry::View view = RandomView();
    // the same Result for a rejected mesh
    {
        Geometry::Mesh mesh = RandomMesh(3);
        mesh.strips[1].vertices.resize(2);
        Geometry::Prepared prepared;
        std::vector<Draw> built;
        REQUIRE(Geometry::Prepare(mesh, prepared) == Geometry::Result::InvalidGeometry);
        REQUIRE(Geometry::Build(mesh, {view}, state, built) == Geometry::Result::InvalidGeometry);
    }
    {
        Geometry::Mesh mesh = RandomMesh(3);
        mesh.strips[2].vertices.resize(39);
        Geometry::Prepared prepared;
        std::vector<Draw> built;
        REQUIRE(Geometry::Prepare(mesh, prepared) == Geometry::Result::InvalidGeometry);
        REQUIRE(Geometry::Build(mesh, {view}, state, built) == Geometry::Result::InvalidGeometry);
    }
    {
        Geometry::Mesh mesh = RandomMesh(3);
        mesh.strips[0].vertices[1].position[2] = std::nanf("");
        Geometry::Prepared prepared;
        std::vector<Draw> built;
        REQUIRE(Geometry::Prepare(mesh, prepared) == Geometry::Result::InvalidGeometry);
        REQUIRE(Geometry::Build(mesh, {view}, state, built) == Geometry::Result::InvalidGeometry);
    }
    {
        Geometry::Mesh mesh = RandomMesh(3);
        mesh.strips[1].vertices[0].stq[1] = INFINITY;
        Geometry::Prepared prepared;
        std::vector<Draw> built;
        REQUIRE(Geometry::Prepare(mesh, prepared) == Geometry::Result::InvalidGeometry);
        REQUIRE(Geometry::Build(mesh, {view}, state, built) == Geometry::Result::InvalidGeometry);
    }
    {
        Geometry::Mesh mesh;
        for (size_t i = 0; i < Geometry::kMaxVertices / 38 + 1; ++i) {
            Geometry::Strip strip;
            strip.vertices.resize(38);
            mesh.strips.push_back(strip);
        }
        Geometry::Prepared prepared;
        std::vector<Draw> built;
        REQUIRE(Geometry::Prepare(mesh, prepared) == Geometry::Result::LimitExceeded);
        REQUIRE(Geometry::Build(mesh, {view}, state, built) == Geometry::Result::LimitExceeded);
    }
    // the same Result for a rejected state / view
    {
        Geometry::Mesh mesh = RandomMesh(2);
        Geometry::Prepared prepared;
        REQUIRE(Geometry::Prepare(mesh, prepared) == Geometry::Result::Ok);
        std::vector<Published> published;
        std::vector<Draw> built;
        Draw untextured = state;
        untextured.tex_enabled = false;
        REQUIRE(Geometry::BuildPrepared(prepared, view, untextured, nullptr, &Collect, &published) == Geometry::Result::UnsupportedState);
        REQUIRE(Geometry::Build(mesh, {view}, untextured, built) == Geometry::Result::UnsupportedState);
        Geometry::View bad = view;
        bad.clip[1][2] = std::nanf("");
        REQUIRE(Geometry::BuildPrepared(prepared, bad, state, nullptr, &Collect, &published) == Geometry::Result::InvalidGeometry);
        REQUIRE(Geometry::Build(mesh, {bad}, state, built) == Geometry::Result::InvalidGeometry);
        bad = view;
        bad.guard[3] = INFINITY;
        REQUIRE(Geometry::BuildPrepared(prepared, bad, state, nullptr, &Collect, &published) == Geometry::Result::InvalidGeometry);
        REQUIRE(Geometry::Build(mesh, {bad}, state, built) == Geometry::Result::InvalidGeometry);
        REQUIRE(published.empty());
    }
}
}  // namespace

int main(int argc, char** argv) {
    const size_t rounds = argc > 1 ? size_t(std::strtoul(argv[1], nullptr, 10)) : 1500;
    for (size_t round = 0; round < rounds; ++round) {
        const Geometry::Mesh mesh = RandomMesh(1 + size_t(Pick(6)));
        const Draw state = State();
        // several views of one prepared mesh (the port's placed instances) and a colour override now and then
        for (int v = 0; v < 3; ++v) Compare(mesh, RandomView(), state, Pick(4) == 0);
        CompareClasses(mesh, RandomView(), state);
    }
    // The strip's own vertex count bounds (3 and 38) and a one-strip mesh of all restarts
    {
        Geometry::Mesh mesh;
        Geometry::Strip three, full, restarts;
        three.vertices.resize(3);
        full.vertices.resize(38);
        restarts.vertices.resize(5);
        for (auto* strip : {&three, &full, &restarts})
            for (auto& v : strip->vertices) {
                for (auto& p : v.position) p = Uniform(-100.f, 100.f);
                v.stq = {Uniform(0.f, 1.f), Uniform(0.f, 1.f), 1.f};
                v.colour = {0x80, 0x80, 0x80, 0x80};
            }
        for (auto& v : restarts.vertices) v.restart = true;
        mesh.strips = {three, full, restarts};
        for (int v = 0; v < 20; ++v) Compare(mesh, RandomView(), State(), v % 2 == 0);
    }
    Rejections();
    REQUIRE(fans_seen > 0);
    REQUIRE(clipped_views > 0);
    REQUIRE(classed_cases > 0 && classed_runs > classed_cases);
    std::printf("test_mesh_geometry_prepared: %zu cases, %zu draws (%zu fans), %zu vertices bit-identical; %zu clipped views; rejections agree; "
                "%zu builds by classes (%zu runs) as the whole build\n",
                cases, draws_compared, fans_seen, vertices_compared, clipped_views, classed_cases, classed_runs);
    return 0;
}
