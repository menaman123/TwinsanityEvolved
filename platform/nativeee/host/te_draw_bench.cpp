// platform/nativeee (host side) -- TE_PROF_BENCH=2 (the Vita): what one draw costs the drawing thread here, measured at
// the device's creation (before the game draws anything). Every case runs a few hundred times inside one frame and is
// timed on the kernel clock (the best of three frames is printed): the engine's own paths (ps2e_draw_submit_mesh with an
// unchanged state = DrawMeshAgain, with a texture or another field changed = the full DrawMesh sequence, the raw CPU-vertex
// submit) and vitaGL's calls alone over the state a mesh draw left bound (glDrawArrays, + a vertex / fragment uniform
// changed, + a texture bound, glUniform* alone, glDrawArraysInstanced, glMultiDrawArrays). The difference between an engine
// path and the bare glDrawArrays is the engine's own code; the bare glDrawArrays is vitaGL + the emulator's GXM work.
// Each raw-GL case puts back what the backend's caches believe is bound, so the game starts from a consistent state.
#include "te_prof.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "engine/render/backends/gl/gl_census.h"

#if defined(__vita__)
#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <psp2/kernel/processmgr.h>
#endif

namespace te::prof {

#if defined(__vita__)
namespace {

int64_t BenchNowUs() { return int64_t(sceKernelGetProcessTimeWide()); }

template <typename F>
void Case(ps2e_draw* d, const char* name, int n, int ops_per_call, F&& fn) {
    std::fprintf(stderr, "[te.prof] draw bench: (running) %s\n", name);
    std::fflush(stderr);
    double best = 1e30;
    ps2r::render::GlCensus before{}, after{};
    for (int rep = 0; rep < 3; ++rep) {
        ps2e_draw_begin_frame(d, nullptr);
        fn(-1);   // the set-up call (not timed): the state the timed calls run under
        const ps2r::render::GlCensus c0 = ps2r::render::Census();
        const int64_t t0 = BenchNowUs();
        for (int i = 0; i < n; ++i) fn(i);
        const int64_t t1 = BenchNowUs();
        const ps2r::render::GlCensus c1 = ps2r::render::Census();
        ps2e_draw_present(d);
        const double us = double(t1 - t0) / double(n * ops_per_call);
        if (us < best) {
            best = us;
            before = c0;
            after = c1;
        }
    }
    const double per = double(n * ops_per_call);
    std::fprintf(stderr, "[te.prof] draw bench: %-62s %8.2f us/op  (per op: glDrawArrays %.2f, state calls %.2f, uniform calls %.2f)\n",
                 name, best, double(after.gl_draws - before.gl_draws) / per, double(after.state_calls - before.state_calls) / per,
                 double(after.uniform_calls - before.uniform_calls) / per);
    std::fflush(stderr);
}

}  // namespace
#endif

void DrawBench(::ps2e_draw* d) {
#if defined(__vita__)
    if (!d) return;
    std::fprintf(stderr, "[te.prof] draw bench: start (times are the drawing thread's per call; the best of 3 frames)\n");
    // two textures of one size, one of another
    std::vector<uint8_t> px(64 * 64 * 4, 0x80), px3(128 * 64 * 4, 0x40);
    const uint32_t t1 = ps2e_draw_texture_create(d, 64, 64, px.data());
    const uint32_t t2 = ps2e_draw_texture_create(d, 64, 64, px.data());
    const uint32_t t3 = ps2e_draw_texture_create(d, 128, 64, px3.data());
    // a mesh of one triangle and one of 1000, all inside a 16-pixel square
    auto make_mesh = [&](int tris) {
        std::vector<ps2e_draw_mesh_vertex> v(size_t(tris) * 3);
        for (int t = 0; t < tris; ++t)
            for (int k = 0; k < 3; ++k) {
                ps2e_draw_mesh_vertex& m = v[size_t(t) * 3 + size_t(k)];
                std::memset(&m, 0, sizeof m);
                m.x = k == 1 ? 1.f : 0.f;
                m.y = k == 2 ? 1.f : 0.f;
                m.s = m.x;
                m.t = m.y;
                m.q = 1.f;
                m.r = m.g = m.b = m.a = 128;
                m.lr = m.lg = m.lb = m.la = 128;
            }
        return ps2e_draw_mesh_create(d, v.data(), int(v.size()));
    };
    const uint32_t small = make_mesh(1), big = make_mesh(1000);
    if (!t1 || !t2 || !t3 || !small || !big) {
        std::fprintf(stderr, "[te.prof] draw bench: no textures / native meshes here (%u %u %u %u %u): skipped\n", t1, t2, t3, small, big);
        return;
    }
    ps2e_draw_state st{};
    st.texture = t1;
    st.gouraud = 1;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = 0;
    st.tfx = PS2E_DRAW_TFX_MODULATE;
    st.tcc = 1;
    st.wrap_u = st.wrap_v = PS2E_DRAW_WRAP_REPEAT;
    // model -> GS screen: 16 pixels a unit around the frame's middle (the GS origin at 2048 - 256)
    ps2e_draw_mesh_params mp{};
    const float screen[16] = {256.f, 0.f, 0.f, 0.f, 0.f, 256.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 2048.f * 16.f, 2048.f * 16.f, 1000.f, 1.f};
    std::memcpy(mp.screen, screen, sizeof screen);
    mp.offset[0] = mp.offset[1] = 2048.f - 256.f;
    mp.box[0] = mp.box[1] = 255.f;
    mp.box[2] = mp.box[3] = 273.f;
    auto moved = [&](int i) {   // another instance's matrix (as a frame's draws have)
        ps2e_draw_mesh_params m = mp;
        m.screen[12] += float(i & 7) * 16.f;
        return m;
    };
    const int n = 400;
    Case(d, "submit_mesh: same state + mesh (DrawMeshAgain)", n, 1, [&](int i) {
        const ps2e_draw_mesh_params m = moved(i);
        ps2e_draw_submit_mesh(d, &st, small, &m);
    });
    Case(d, "submit_mesh: 2 textures of one size alternating (texture swap)", n, 1, [&](int i) {
        ps2e_draw_state s = st;
        s.texture = (i & 1) ? t2 : t1;
        const ps2e_draw_mesh_params m = moved(i);
        ps2e_draw_submit_mesh(d, &s, small, &m);
    });
    Case(d, "submit_mesh: 2 textures of two sizes alternating (texture swap)", n, 1, [&](int i) {
        ps2e_draw_state s = st;
        s.texture = (i & 1) ? t3 : t1;
        const ps2e_draw_mesh_params m = moved(i);
        ps2e_draw_submit_mesh(d, &s, small, &m);
    });
    ps2e_draw_set_mesh_texture_swap(d, 0);
    Case(d, "submit_mesh: 2 textures of one size alternating (full path)", n, 1, [&](int i) {
        ps2e_draw_state s = st;
        s.texture = (i & 1) ? t2 : t1;
        const ps2e_draw_mesh_params m = moved(i);
        ps2e_draw_submit_mesh(d, &s, small, &m);
    });
    Case(d, "submit_mesh: 2 textures of two sizes alternating (full path)", n, 1, [&](int i) {
        ps2e_draw_state s = st;
        s.texture = (i & 1) ? t3 : t1;
        const ps2e_draw_mesh_params m = moved(i);
        ps2e_draw_submit_mesh(d, &s, small, &m);
    });
    ps2e_draw_set_mesh_texture_swap(d, 1);
    Case(d, "submit_mesh: gouraud flag alternating (full path, no GL change)", n, 1, [&](int i) {
        ps2e_draw_state s = st;
        s.gouraud = i & 1;
        const ps2e_draw_mesh_params m = moved(i);
        ps2e_draw_submit_mesh(d, &s, small, &m);
    });
    Case(d, "submit_mesh: 3000-vertex mesh, same state (DrawMeshAgain)", n, 1, [&](int i) {
        const ps2e_draw_mesh_params m = moved(i);
        ps2e_draw_submit_mesh(d, &st, big, &m);
    });
    Case(d, "submit_mesh: 2 meshes alternating, same state (DrawMeshAgain)", n, 1, [&](int i) {
        const ps2e_draw_mesh_params m = moved(i);
        ps2e_draw_submit_mesh(d, &st, (i & 1) ? big : small, &m);
    });
    // CPU vertices (the raw path)
    auto quads = [&](int count) {
        std::vector<ps2e_draw_vertex> v(static_cast<size_t>(count));
        for (int k = 0; k < count; ++k) {
            ps2e_draw_vertex& o = v[size_t(k)];
            std::memset(&o, 0, sizeof o);
            o.x = 256.f + float(k % 3 == 1 ? 8 : 0);
            o.y = 256.f + float(k % 3 == 2 ? 8 : 0);
            o.z = 1000.f;
            o.u = o.x / 512.f;
            o.v = o.y / 512.f;
            o.r = o.g = o.b = o.a = 128;
            o.q = 1.f;
        }
        return v;
    };
    // (300-vertex raw submits, 400 a frame in back-to-back short frames, wrapped the vertex ring every few frames; the
    // bench then crashed Vita3K a little later -- kept to 1/8 of the calls: ~0.4 MB a frame, like a game frame)
    const std::vector<ps2e_draw_vertex> v6 = quads(6), v300 = quads(300);
    Case(d, "submit: 6 CPU vertices, same state (raw path)", n, 1, [&](int) { ps2e_draw_submit(d, &st, v6.data(), 6); });
    Case(d, "submit: 300 CPU vertices, same state (raw path)", n / 8, 1, [&](int) { ps2e_draw_submit(d, &st, v300.data(), 300); });
    {
        ps2e_draw_state blended = st;
        blended.blend = 1;
        blended.blend_a = 0;
        blended.blend_b = 1;
        blended.blend_c = 0;
        blended.blend_d = 1;
        Case(d, "submit: 6 CPU vertices, blended, same state (raw path)", n, 1, [&](int) { ps2e_draw_submit(d, &blended, v6.data(), 6); });
    }
    // vitaGL alone over the state a mesh draw leaves bound (program, the mesh's arrays, the textures)
    // the program a mesh draw binds (a present unbinds it: asked inside a frame)
    GLint program = 0;
    ps2e_draw_begin_frame(d, nullptr);
    ps2e_draw_submit_mesh(d, &st, small, &mp);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    ps2e_draw_present(d);
    const GLint loc_mesh = program > 0 ? glGetUniformLocation(GLuint(program), "u_mesh") : -1;
    const GLint loc_tfx = program > 0 ? glGetUniformLocation(GLuint(program), "u_tfx") : -1;
    float mesh0[20], mesh1[20];
    std::memcpy(mesh0, mp.screen, sizeof mp.screen);
    mesh0[16] = mp.offset[0];
    mesh0[17] = mp.offset[1];
    mesh0[18] = mp.scroll[0];
    mesh0[19] = mp.scroll[1];
    std::memcpy(mesh1, mesh0, sizeof mesh0);
    mesh1[12] += 16.f;
    std::fprintf(stderr, "[te.prof] draw bench: the PS2 program %d, u_mesh %d, u_tfx %d\n", int(program), int(loc_mesh), int(loc_tfx));
    // each raw case's set-up draw is a mesh draw of the plain state with mesh0 (what the caches then believe)
    auto setup = [&](uint32_t mesh) { ps2e_draw_submit_mesh(d, &st, mesh, &mp); };
    Case(d, "glDrawArrays alone (3 vertices)", n, 1, [&](int i) {
        if (i < 0) { setup(small); return; }
        glDrawArrays(GL_TRIANGLES, 0, 3);
    });
    Case(d, "glDrawArrays alone (3000 vertices)", n, 1, [&](int i) {
        if (i < 0) { setup(big); return; }
        glDrawArrays(GL_TRIANGLES, 0, 3000);
    });
    if (loc_mesh >= 0) {
        Case(d, "glUniform4fv(u_mesh, 5) + glDrawArrays (vertex uniforms dirty)", n, 1, [&](int i) {
            if (i < 0) { setup(small); return; }
            glUniform4fv(loc_mesh, 5, (i & 1) ? mesh0 : mesh1);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        });
        Case(d, "glUniform4fv(u_mesh, 5) alone", n, 1, [&](int i) {
            if (i < 0) { setup(small); return; }
            glUniform4fv(loc_mesh, 5, (i & 1) ? mesh0 : mesh1);
        });
    }
    if (loc_tfx >= 0) {
        Case(d, "glUniform1i(u_tfx) + glDrawArrays (fragment uniforms dirty)", n, 1, [&](int i) {
            if (i < 0) { setup(small); return; }
            glUniform1i(loc_tfx, (i & 1) ? 0 : 1);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        });
        Case(d, "glUniform1i(u_tfx) alone", n, 1, [&](int i) {
            if (i < 0) { setup(small); return; }
            glUniform1i(loc_tfx, (i & 1) ? 0 : 1);
        });
    }
    if (loc_mesh >= 0 && loc_tfx >= 0) {
        Case(d, "glUniform4fv(u_mesh) + glUniform1i(u_tfx) + glDrawArrays", n, 1, [&](int i) {
            if (i < 0) { setup(small); return; }
            glUniform4fv(loc_mesh, 5, (i & 1) ? mesh0 : mesh1);
            glUniform1i(loc_tfx, (i & 1) ? 0 : 1);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        });
    }
    {
        GLint bound = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        GLuint mine[2] = {0, 0};
        glGenTextures(2, mine);
        for (GLuint t : mine) {
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        }
        glBindTexture(GL_TEXTURE_2D, GLuint(bound));
        Case(d, "glBindTexture (2 textures alternating) + glDrawArrays", n, 1, [&](int i) {
            if (i < 0) { setup(small); glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound); return; }
            glBindTexture(GL_TEXTURE_2D, mine[i & 1]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            if (i == n - 1) glBindTexture(GL_TEXTURE_2D, GLuint(bound));
        });
        glDeleteTextures(2, mine);
    }
    Case(d, "glDrawArraysInstanced(3 vertices, 8 instances), per instance", n / 8, 8, [&](int i) {
        if (i < 0) { setup(small); return; }
        glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 8);
    });
    {
        const GLint firsts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const GLsizei counts[8] = {3, 3, 3, 3, 3, 3, 3, 3};
        Case(d, "glMultiDrawArrays(8 x 3 vertices), per sub-draw", n / 8, 8, [&](int i) {
            if (i < 0) { setup(small); return; }
            glMultiDrawArrays(GL_TRIANGLES, firsts, counts, 8);
        });
    }
    {
        // the CPU-vertex path's per-vertex parts: the producer's copy, the copy into a GPU buffer, the submit's screen-box scan
        const size_t bytes = v300.size() * sizeof(ps2e_draw_vertex);
        std::vector<uint8_t> heap(bytes);
        Case(d, "memcpy 300 vertices heap -> heap", n, 1, [&](int i) {
            if (i < 0) return;
            std::memcpy(heap.data(), v300.data(), bytes);
        });
        volatile float sink = 0.f;
        Case(d, "screen box + finite-q scan of 300 vertices", n, 1, [&](int i) {
            if (i < 0) return;
            float box[4] = {v300[0].x, v300[0].y, v300[0].x, v300[0].y};
            bool finite = true;
            for (const ps2e_draw_vertex& v : v300) {
                box[0] = std::min(box[0], v.x);
                box[1] = std::min(box[1], v.y);
                box[2] = std::max(box[2], v.x);
                box[3] = std::max(box[3], v.y);
                if (!std::isfinite(v.q)) finite = false;
            }
            sink = box[0] + box[3] + (finite ? 1.f : 0.f);
        });
    }
    Case(d, "sceKernelGetProcessTimeWide (one HLE call)", n, 1, [&](int i) {
        if (i < 0) return;
        volatile int64_t t = int64_t(sceKernelGetProcessTimeWide());
        (void)t;
    });
    ps2e_draw_mesh_destroy(d, small);
    ps2e_draw_mesh_destroy(d, big);
    ps2e_draw_texture_destroy(d, t1);
    ps2e_draw_texture_destroy(d, t2);
    ps2e_draw_texture_destroy(d, t3);
    std::fprintf(stderr, "[te.prof] draw bench: done\n");
#else
    (void)d;
#endif
}

}  // namespace te::prof
