// render/backends/gl/gl_shader_library.cpp -- GlShaderLibrary implementation.
#include "engine/render/backends/gl/gl_shader_library.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#if PS2R_HOST_VITA
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <regex>
#include <utility>
#include <vector>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "engine/render/backend.h"   // MeshVertex (the raw mesh draws' stream)
#endif

#include "engine/render/backends/gl/gl_shader_sources.h"
#include "engine/render/backends/gl/gl_uniform_cache.h"
#include "engine/render/backends/gl/gl_census.h"
#include "engine/render/backends/gl/gl_fill_census.h"
#if PS2R_HOST_VITA
#include "engine/render/backends/gl/gl_state_cache.h"
#endif

namespace ps2r {
namespace render {

const char* GlShaderLibrary::VertexSource() { return GlShaderSources::Ps2Vertex(); }
const char* GlShaderLibrary::FragmentSource() { return GlShaderSources::Ps2Fragment(); }
const char* GlShaderLibrary::ScanoutVertexSource() { return GlShaderSources::ScanoutVertex(); }
const char* GlShaderLibrary::ScanoutFragmentSource() { return GlShaderSources::ScanoutFragment(); }
const char* GlShaderLibrary::ScanoutDitherFragmentSource() {
#if PS2R_HOST_VITA
    // the CT16 dither's matrix is a dynamically indexed uniform array, which Sony's compiler does not finish: the plain
    // scanout stands in for the retail-look present on the Vita
    return GlShaderSources::ScanoutFragment();
#else
    return GlShaderSources::ScanoutDitherFragment();
#endif
}

#if PS2R_HOST_VITA
namespace {
// vitaGL translates GLSL of the 1.x shape (attribute / varying / texture2D / gl_FragColor) into the Cg Sony's compiler
// takes; the library's sources are GLSL 4.1 core. The spellings that differ are rewritten here; the attribute
// locations the `layout` qualifiers named are bound by name before the link (LinkProgram).
// GLSL 4.1 built-ins with no 1.x spelling, as Cg's (Sony's compiler takes them through the translation), and overloads
// for the calls Cg will not resolve by itself: a vector clamped to scalars, integer min / max / clamp. Cg has no unsigned
// integers; the library's uint arithmetic stays within 31 bits, so int carries it.
const char* const kVitaPrelude = R"CG(
#define TE_INT_DIVIDE_IN_FLOAT 1
#define uint int
#define uvec2 ivec2
#define uvec3 ivec3
#define uvec4 ivec4
#define textureLod(s, c, l) tex2Dlod(s, float4(c, 0.0, l))
float te_clamp(float x, float a, float b) { return clamp(x, a, b); }
vec2 te_clamp(vec2 x, float a, float b) { return clamp(x, vec2(a), vec2(b)); }
vec3 te_clamp(vec3 x, float a, float b) { return clamp(x, vec3(a), vec3(b)); }
vec4 te_clamp(vec4 x, float a, float b) { return clamp(x, vec4(a), vec4(b)); }
vec2 te_clamp(vec2 x, vec2 a, vec2 b) { return clamp(x, a, b); }
vec3 te_clamp(vec3 x, vec3 a, vec3 b) { return clamp(x, a, b); }
vec4 te_clamp(vec4 x, vec4 a, vec4 b) { return clamp(x, a, b); }
int te_clamp(int x, int a, int b) { return x < a ? a : (x > b ? b : x); }
ivec2 te_clamp(ivec2 x, ivec2 a, ivec2 b) { return ivec2(te_clamp(x.x, a.x, b.x), te_clamp(x.y, a.y, b.y)); }
float te_min(float a, float b) { return min(a, b); }
vec2 te_min(vec2 a, float b) { return min(a, vec2(b)); }
vec3 te_min(vec3 a, float b) { return min(a, vec3(b)); }
vec4 te_min(vec4 a, float b) { return min(a, vec4(b)); }
vec2 te_min(vec2 a, vec2 b) { return min(a, b); }
vec3 te_min(vec3 a, vec3 b) { return min(a, b); }
vec4 te_min(vec4 a, vec4 b) { return min(a, b); }
int te_min(int a, int b) { return a < b ? a : b; }
float te_max(float a, float b) { return max(a, b); }
vec2 te_max(vec2 a, float b) { return max(a, vec2(b)); }
vec3 te_max(vec3 a, float b) { return max(a, vec3(b)); }
vec4 te_max(vec4 a, float b) { return max(a, vec4(b)); }
vec2 te_max(vec2 a, vec2 b) { return max(a, b); }
vec3 te_max(vec3 a, vec3 b) { return max(a, b); }
vec4 te_max(vec4 a, vec4 b) { return max(a, b); }
int te_max(int a, int b) { return a > b ? a : b; }
#define clamp te_clamp
#define min te_min
#define max te_max
)CG";

// The fragment profile has no texel fetch and no texture size query (tex2Dfetch / tex2Dsize): texelFetch(s, p, l)
// becomes a sample at the texel's centre and textureSize(s, l) the size the backend sets in te_size_<s> before each draw
// (GlDevice: the size of the texture bound on the sampler's unit). Only the PS2 program's samplers are read this way.
const char* const kVitaSamplerSizes[] = {"u_texture", "u_frame", "u_rt_texture", "u_clut", "u_idx_tex2", "u_zsrc_tex"};

// Every call `name(a, b, ...)` in s given to `make` as its argument texts (nested parentheses and commas respected).
std::string RewriteCalls(const std::string& s, const char* name, std::string (*make)(const std::vector<std::string>&)) {
    std::string out;
    const std::regex call(std::string("\\b") + name + "\\s*\\(");
    std::smatch m;
    std::string rest = s;
    while (std::regex_search(rest, m, call)) {
        out += rest.substr(0, static_cast<size_t>(m.position(0)));
        size_t i = static_cast<size_t>(m.position(0) + m.length(0));
        int depth = 1;
        std::vector<std::string> args(1);
        for (; i < rest.size() && depth > 0; ++i) {
            const char c = rest[i];
            if (c == '(') ++depth;
            if (c == ')' && --depth == 0) break;
            if (c == ',' && depth == 1) { args.emplace_back(); continue; }
            args.back() += c;
        }
        out += make(args);
        rest = rest.substr(i + 1);
    }
    return out + rest;
}

std::string Trim(const std::string& t) {
    const size_t b = t.find_first_not_of(" \t\n"), e = t.find_last_not_of(" \t\n");
    return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
}

std::string VitaGlsl(uint32_t type, const char* source) {
    std::string s = source;
    s = std::regex_replace(s, std::regex(R"(#version\s+\d+\s*\w*)"), "");
    s = std::regex_replace(s, std::regex(R"(layout\s*\(\s*location\s*=\s*\d+\s*\)\s*in\s+)"), "attribute ");
    s = std::regex_replace(s, std::regex(R"(\bnoperspective\s+)"), "");
    if (type == GL_VERTEX_SHADER) {
        s = std::regex_replace(s, std::regex(R"((^|\n)(\s*)out\s+)"), "$1$2varying ");
    } else {
        s = std::regex_replace(s, std::regex(R"((^|\n)(\s*)in\s+)"), "$1$2varying ");
        s = std::regex_replace(s, std::regex(R"(\bout\s+vec4\s+frag_color\s*;)"), "");
        s = "#define frag_color gl_FragColor\n" + s;
    }
    s = std::regex_replace(s, std::regex(R"(\btexture\s*\()"), "texture2D(");
    s = std::regex_replace(s, std::regex(R"(\b((0x[0-9A-Fa-f]+)|([0-9]+))[uU]\b)"), "$1");   // no unsigned literals in Cg
    if (type == GL_FRAGMENT_SHADER) {
        s = RewriteCalls(s, "texelFetch", [](const std::vector<std::string>& a) {
            const std::string sampler = Trim(a[0]);
            return "texture2D(" + sampler + ", (vec2(" + Trim(a.size() > 1 ? a[1] : "") + ") + vec2(0.5)) / te_size_" + sampler + ")";
        });
        s = RewriteCalls(s, "textureSize", [](const std::vector<std::string>& a) {
            return "ivec2(te_size_" + Trim(a[0]) + ")";
        });
        // a fragment shader that writes no colour (the stencil passes, drawn with colour writes off) still gets one:
        // Sony's compiler does not finish a fragment program without an output
        if (s.find("frag_color") == std::string::npos && s.find("gl_FragColor") == std::string::npos)
            s = std::regex_replace(s, std::regex(R"(void\s+main\s*\(\s*\)\s*\{)"), "void main() { gl_FragColor = vec4(0.0);");
        std::string sizes;
        for (const char* name : kVitaSamplerSizes)
            if (s.find(std::string("te_size_") + name) != std::string::npos) sizes += std::string("uniform vec2 te_size_") + name + ";\n";
        s = sizes + s;
    }
    return std::string(kVitaPrelude) + s;
}

// The PS2 fragment program as the Vita's draws use it. vitaGL sets EVERY sampler of a program on every draw
// (sceGxmSetFragmentTexture per sampler, each a texture bind for the GPU) and the PS2 program declares five; four of
// them -- the destination copy (u_rt_texture), the CLUT (u_clut), the indexed partner (u_idx_tex2) and the colour Z
// source (u_zsrc_tex) -- are read only by arms no ps2e_draw state turns on (its textures are decoded, never a render
// target: no shader blend, indexed or depth serve; its frame is 32-bit with the colour clamp on: no exact 16-bit mask,
// no wrap blend; its destination-alpha test is the stencil's, not u_date; no Z source is armed), so on the Vita they are
// the same sampler as u_texture: the program's code is unchanged (the dead arms read u_texture instead), its one sampler
// one bind a draw. TE_GFX_LEAN_SAMPLERS=0 compiles the five.
std::string VitaLeanPs2Fragment(const char* source) {
    std::string s = source;
    static const char* const kDead[] = {"u_rt_texture", "u_clut", "u_idx_tex2", "u_zsrc_tex"};
    std::string defines;
    for (const char* name : kDead) {
        s = std::regex_replace(s, std::regex(std::string(R"(uniform\s+sampler2D\s+)") + name + R"(\s*;)"), "");
        defines += std::string("#define ") + name + " u_texture\n";
    }
    return defines + s;
}

// The FAST PS2 fragment program (TE_GFX_VITA_PS2_FAST, default on): the program as above with every uniform of
// kVitaFastConstants replaced by the value the Vita's draws give it -- its declaration removed, `#define u_x value` in
// front -- so Sony's compiler drops the arms they gate: the depth-attachment and indexed CLUT serves (u_depth_fmt,
// u_idx_fmt, u_uv_native), the shader-side blend (u_sw_blend), DATE (u_date), the colour-authored Z source (u_zsrc_en),
// the 16-bit frame conventions (u_fb16, u_ct16q, u_fbmsk16_en), SCANMSK (u_scanmsk), the V flip (u_flip_v) and the
// colour wrap (u_colclamp 1). The compiled full program (the run4 device / Vita3K GXP: 2475 USSE instructions, 185
// branches, 24 temporaries) carries all of them, and the GPU pays its temporaries for every fragment (fewer threads in
// flight). A draw takes the fast program only while each of these uniforms holds exactly that value
// (GlDrawExecutor::SelectPs2Variant): the same source over the same values -- the same arithmetic.
struct VitaFastConstant { const char* name; int value; int GlUniformLocations::*field; };
const VitaFastConstant kVitaFastConstants[] = {
    {"u_sw_blend", 0, &GlUniformLocations::sw_blend},     {"u_depth_fmt", 0, &GlUniformLocations::depth_fmt},
    {"u_idx_fmt", 0, &GlUniformLocations::idx_fmt},       {"u_uv_native", 0, &GlUniformLocations::uv_native},
    {"u_flip_v", 0, &GlUniformLocations::flip_v},         {"u_scanmsk", 0, &GlUniformLocations::scanmsk},
    {"u_date", 0, &GlUniformLocations::date},             {"u_zsrc_en", 0, &GlUniformLocations::zsrc_en},
    {"u_fbmsk16_en", 0, &GlUniformLocations::fbmsk16_en}, {"u_fb16", 0, &GlUniformLocations::fb16},
    {"u_ct16q", 0, &GlUniformLocations::ct16q},           {"u_colclamp", 1, &GlUniformLocations::colclamp},
};

// "" when the source does not have the expected declarations (the fast program is then not built)
std::string VitaFastPs2Fragment(const std::string& source) {
    std::string s = source;
    std::string defines;
    for (const VitaFastConstant& c : kVitaFastConstants) {
        const std::regex decl(std::string(R"(uniform\s+int\s+)") + c.name + R"(\s*;)");
        if (!std::regex_search(s, decl)) return std::string();
        s = std::regex_replace(s, decl, "");
        defines += std::string("#define ") + c.name + " " + std::to_string(c.value) + "\n";
    }
    return defines + s;
}
}  // namespace

const GlShaderLibrary::FastConstant* GlShaderLibrary::FastConstants(int* count) {
    static FastConstant table[sizeof kVitaFastConstants / sizeof kVitaFastConstants[0]];
    static bool made = false;
    if (!made) {
        for (size_t i = 0; i < sizeof kVitaFastConstants / sizeof kVitaFastConstants[0]; ++i)
            table[i] = FastConstant{kVitaFastConstants[i].field, kVitaFastConstants[i].value};
        made = true;
    }
    *count = int(sizeof kVitaFastConstants / sizeof kVitaFastConstants[0]);
    return table;
}
namespace {
}  // namespace
#endif

uint32_t GlShaderLibrary::CompileShader(uint32_t type, const char* source) {
    GLuint shader = glCreateShader(type);
#if PS2R_HOST_VITA
    const std::string vita = VitaGlsl(type, source);
    source = vita.c_str();
#endif
    glShaderSource(shader, 1, &source, nullptr);
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    if (!ok) {
        char log[1024];
        log[0] = '\0';
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gl] shader compile error: %s\n", log);
#if PS2R_HOST_VITA
        // the source as handed to vitaGL, numbered, to match the compiler's line numbers
        int line_no = 1;
        for (const char* p = source; *p && line_no <= 12; ++line_no) {
            const char* e = std::strchr(p, '\n');
            const int n = e ? int(e - p) : int(std::strlen(p));
            std::fprintf(stderr, "[gl]   %2d| %.*s\n", line_no, n, p);
            p = e ? e + 1 : p + n;
        }
        std::fflush(stderr);
#endif
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

uint32_t GlShaderLibrary::LinkProgram(uint32_t vert, uint32_t frag) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
#if PS2R_HOST_VITA
    // the locations the GLSL 4.1 sources gave with layout(location = N) (VitaGlsl turns those into attributes)
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glBindAttribLocation(program, 2, "a_color");
    glBindAttribLocation(program, 3, "a_fog");
    glBindAttribLocation(program, 4, "a_normal");
    glBindAttribLocation(program, 5, "a_litcolor");
    glBindAttribLocation(program, 6, "a_q");
    glBindAttribLocation(program, 7, "a_weights");   // the GPU-skinning vertex program's (Ps2SkinVertex)
    glBindAttribLocation(program, 8, "a_joints");
    glBindAttribLocation(program, 9, "a_index");
    // the instanced mesh vertex program's rows (GlShaderSources::LeanPs2InstancedVertex)
    glBindAttribLocation(program, 10, "a_mesh0");
    glBindAttribLocation(program, 11, "a_mesh1");
    glBindAttribLocation(program, 12, "a_mesh2");
    glBindAttribLocation(program, 13, "a_mesh3");
    std::fflush(stderr);
#endif
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gl] program link error: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

#if PS2R_HOST_VITA
namespace {
// The movie pictures' YUV program (GlDevice::DrawMoviePicture; the copy program's vertex shader: its full-screen triangle
// over the picture's viewport, u_src = the picture's texture extent): the decoder's 4:2:0 planes (u_plane_y / u / v on
// units 0-2, R8, limited range as MPEG-2 codes them) converted with the BT.601 matrix swscale's yuv420p -> RGBA conversion
// used for the RGBA pictures (the movies' streams are BT.470BG = BT.601, TV range). The RGBA path drew swscale's image
// bilinearly, each chroma sample repeated over its 2x2 pixels; here the luma is filtered as that image was and the chroma
// sampled at the SAME footprint: of the two luma texels a bilinear tap blends (i and i + 1), only an odd i crosses into the
// next chroma texel, so the chroma weight is the luma's fraction there and 0 elsewhere (u_luma_size: the luma plane's
// size, then the chroma's). The arithmetic is the GPU's float where swscale rounded each texel to 8 bits and clamped it
// before the filter: a pixel may differ from the RGBA path's by a level (the development disc, simulated: TTIDENT 1:1 at most
// 1 level, VIVENDI scaled 640x480 -> 512x512 99.7 % of the pixels within 2 levels; TE_MOVIE_PICTURES=rgba keeps
// swscale's pictures).
const char* const kMovieYuvFragSrc = R"(
#version 410 core
in vec2 v_texcoord;
uniform sampler2D u_plane_y;
uniform sampler2D u_plane_u;
uniform sampler2D u_plane_v;
uniform vec4 u_luma_size;
out vec4 frag_color;
void main() {
    vec2 tap = v_texcoord * u_luma_size.xy - 0.5;
    vec2 first = floor(tap);
    vec2 c0 = floor(first * 0.5);
    vec2 cuv = (c0 + 0.5 + (floor((first + 1.0) * 0.5) - c0) * (tap - first)) / u_luma_size.zw;
    float luma = texture(u_plane_y, v_texcoord).r - 0.0627451;
    float cb = texture(u_plane_u, cuv).r - 0.5019608;
    float cr = texture(u_plane_v, cuv).r - 0.5019608;
    vec3 rgb = vec3(1.1643836 * luma + 1.5960268 * cr,
                    1.1643836 * luma - 0.3917623 * cb - 0.8129676 * cr,
                    1.1643836 * luma + 2.0172321 * cb);
    frag_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";
// ---- the compiled-program cache (the Vita) ------------------------------------------------------------------------------
// Sony's runtime compiler (libshacccg, through vitaGL's GLSL translation) runs for every shader at every launch: 16
// compiles before the first picture (the device's run4: a 3.1 s gap at boot). A pair's binaries -- vitaGL's own
// serialization of each compiled shader (custom_shaders.c serialize_shader: the matrix-uniform table + the GXP), read
// back with glGetProgramBinary right after the link -- go to <TE_GL_SHADER_CACHE>/<key>.tegxp; the next launch hands
// them to glShaderBinary (unserialize_shader: the GXP registered with the shader patcher, exactly what a compile leaves
// in the shader object) and links them as a compiled pair is linked (the same attach + glBindAttribLocation + link,
// over the same GXPs: the same program). Both shaders of a pair come from the cache or neither does: vitaGL's
// VGL_MODE_SHADER_PAIR translator carries the vertex shader's varying bindings into the fragment shader's translation
// (glsl_utils.c: glsl_bindings_map, glsl_is_first_shader flips per translation), so a half-cached pair would translate
// its other half against the wrong state; a pair loaded whole leaves that state as a compiled pair does (back at
// "first shader", the map cleared). The key: FNV-1a 64 over the pair's GLSL as handed to the translation, the
// translation's own text (kVitaPrelude) and kCacheFormat -- bump kCacheFormat when VitaGlsl's rewrites or the vitaGL
// library change. A file that does not parse (size, magic, a GXP's own header) is ignored and rewritten.
constexpr const char* kCacheFormat = "TEGXP 1; vitaGL r1488 2bdbe89; VGL_MODE_SHADER_PAIR; VitaGlsl 1";
constexpr uint32_t kCacheMagic = 0x50584754u;   // "TGXP"

struct CacheHeader {
    uint32_t magic;
    uint32_t header_size;
    uint64_t key;
    uint32_t vs_size;
    uint32_t fs_size;
};

uint64_t Fnv1a(uint64_t h, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

const char* CacheDir() {
    const char* d = std::getenv("TE_GL_SHADER_CACHE");
    if (!d || !*d || (d[0] == '0' && !d[1])) return nullptr;
    return d;
}

// THE SHIPPED CACHE: the package's own set of cache files (app0:shaders/<key>.tegxp; tools/vita/shader_cache_seed.sh makes
// it by running the package once in Vita3K -- the same libshacccg, so the same bytes the first launch would compile and
// store -- and packs it into the .vpk), read when the memory card's cache has no file for a key. The key covers the pair's
// GLSL, the translation's prelude and kCacheFormat, so a shader changed since the set was made has another key: not
// found there, compiled and stored on the card as before. Nothing is copied to the card (a shipped file is read in
// place at every launch, as fast as the card's own). TE_GL_SHADER_SEED=<dir>: another directory; =0: none.
const char* SeedDir() {
    static const char* const dir = [] {
        const char* d = std::getenv("TE_GL_SHADER_SEED");
        if (d && d[0] == '0' && !d[1]) return static_cast<const char*>(nullptr);
        return d && *d ? d : "app0:shaders";
    }();
    return dir;
}
// TE_GL_SHADER_SEED_BUILD=1 (the seed run): every program variant built at start-up, those otherwise built on first use
// mid-game included, so the cache the run stores holds them all
bool BuildEverything() {
    static const bool on = [] { const char* v = std::getenv("TE_GL_SHADER_SEED_BUILD"); return v && v[0] == '1'; }();
    return on;
}

// A serialized shader (u32 matrix count, the matrices' parameter indices, the GXP): the GXP, checked against its own
// header (magic, its size within the blob); null when it does not parse.
const uint8_t* GxpOf(const uint8_t* blob, size_t size, size_t* gxp_size) {
    if (size < 8) return nullptr;
    uint32_t mats = 0;
    std::memcpy(&mats, blob, 4);
    if (mats > 256 || 4 + 4 * size_t(mats) + 96 > size) return nullptr;
    const uint8_t* g = blob + 4 + 4 * size_t(mats);
    const size_t room = size - 4 - 4 * size_t(mats);
    uint32_t gsize = 0;
    std::memcpy(&gsize, g + 8, 4);
    if (std::memcmp(g, "GXP\0", 4) != 0 || gsize < 96 || gsize > room) return nullptr;
    *gxp_size = room;
    return g;
}

// What one compiled shader costs the GPU per vertex / fragment, from its GXP header (the layout Vita3K reads,
// gxm/types.h SceGxmProgram) and its code: the primary program's USSE instructions (64-bit each) and branches (Vita3K's
// usse_program_analyzer is_branch), the secondary (per-draw uniform setup) program's, the temporaries.
std::string GxpStats(const uint8_t* g, size_t size) {
    auto u16 = [g](size_t o) { uint16_t v; std::memcpy(&v, g + o, 2); return v; };
    auto u32 = [g](size_t o) { uint32_t v; std::memcpy(&v, g + o, 4); return v; };
    const uint32_t pri_n = u32(60), pri_off = u32(64), sec_n = u32(68);
    const uint32_t temps = u32(52), pas = u16(48), sas = u16(50);
    uint32_t branches = 0;
    const size_t code = 64 + size_t(pri_off);
    if (code + size_t(pri_n) * 8 <= size) {
        for (uint32_t i = 0; i < pri_n; ++i) {
            uint64_t inst;
            std::memcpy(&inst, g + code + size_t(i) * 8, 8);
            const uint32_t high = uint32_t(inst >> 32);
            const bool br = ((high & ~0x07FFFFFFu) >> 27 == 0x1Fu) && (((high & ~0xFFCFFFFFu) >> 20) == 0) &&
                            !(high & 0x00400000u) && ((((high & ~0xFFFFFE3Fu) >> 6) == 0) || ((high & ~0xFFFFFE3Fu) >> 6) == 1);
            branches += br ? 1 : 0;
        }
    }
    char line[160];
    std::snprintf(line, sizeof line, "%u instructions (%u branches), setup %u, temps %u, PA %u SA %u, %u bytes", pri_n,
                  branches, sec_n, temps, pas, sas, u32(8));
    return line;
}

// The two serialized shaders inside vitaGL's program binary (custom_shaders.c glGetProgramBinary: the attribute table
// -- u32 + VERTEX_ATTRIBS_NUM (16) SceGxmVertexAttribute -- then u32 size + the vertex shader, u32 size + the fragment)
bool SplitProgramBinary(const std::vector<uint8_t>& bin, std::vector<uint8_t>* vs, std::vector<uint8_t>* fs) {
    const size_t head = 4 + 16 * sizeof(SceGxmVertexAttribute);
    if (bin.size() < head + 8) return false;
    uint32_t vs_n = 0, fs_n = 0;
    std::memcpy(&vs_n, bin.data() + head, 4);
    if (head + 4 + size_t(vs_n) + 4 > bin.size()) return false;
    std::memcpy(&fs_n, bin.data() + head + 4 + vs_n, 4);
    if (head + 8 + size_t(vs_n) + size_t(fs_n) != bin.size()) return false;
    vs->assign(bin.begin() + long(head + 4), bin.begin() + long(head + 4 + vs_n));
    fs->assign(bin.begin() + long(head + 8 + vs_n), bin.end());
    size_t n = 0;
    return GxpOf(vs->data(), vs->size(), &n) && GxpOf(fs->data(), fs->size(), &n);
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    SceIoStat st{};
    bool ok = sceIoGetstatByFd(fd, &st) >= 0 && st.st_size > 0 && st.st_size < (8 << 20);
    if (ok) {
        out->resize(size_t(st.st_size));
        ok = sceIoRead(fd, out->data(), SceSize(out->size())) == int(out->size());
    }
    sceIoClose(fd);
    return ok;
}

bool WriteFileAtomic(const std::string& path, const std::vector<uint8_t>& data) {
    const std::string tmp = path + ".tmp";
    const SceUID fd = sceIoOpen(tmp.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return false;
    const bool ok = sceIoWrite(fd, data.data(), SceSize(data.size())) == int(data.size());
    sceIoClose(fd);
    if (!ok) { sceIoRemove(tmp.c_str()); return false; }
    sceIoRemove(path.c_str());
    return sceIoRename(tmp.c_str(), path.c_str()) >= 0;
}

uint32_t g_cache_loaded = 0, g_cache_compiled = 0, g_cache_stored = 0, g_cache_shipped = 0;
double g_cache_ms = 0;

// The compiled programs' GXPs, kept for the raw mesh draws (GlShaderLibrary::RawTwin) while g_capture_gxps is set
// (BuildLeanPrograms' vertex paths): program id -> its vertex and fragment GXP, each exactly as long as its header says
bool g_capture_gxps = false;
struct ProgramGxps { std::vector<uint8_t> vertex, fragment; };
std::vector<std::pair<uint32_t, ProgramGxps>> g_gxps;
void CaptureGxps(uint32_t program, const uint8_t* vg, size_t vn, const uint8_t* fg, size_t fn) {
    if (!program || !vg || !fg) return;
    uint32_t vsize = 0, fsize = 0;
    std::memcpy(&vsize, vg + 8, 4);
    std::memcpy(&fsize, fg + 8, 4);
    if (vsize > vn || fsize > fn) return;
    ProgramGxps p;
    p.vertex.assign(vg, vg + vsize);
    p.fragment.assign(fg, fg + fsize);
    g_gxps.emplace_back(program, std::move(p));
}
const ProgramGxps* GxpsOf(uint32_t program) {
    for (const auto& e : g_gxps)
        if (e.first == program) return &e.second;
    return nullptr;
}
}  // namespace
#endif

uint32_t GlShaderLibrary::BuildProgram(const char* vertex, const char* fragment, const char* label, uint32_t* keep_vs,
                                       uint32_t* keep_fs, uint32_t link_vs, uint32_t link_fs) {
#if !PS2R_HOST_VITA
    (void)label;
#endif
#if PS2R_HOST_VITA
    const auto t0 = std::chrono::steady_clock::now();
    const char* dir = CacheDir();
    const char* seed = SeedDir();
    std::string path;
    uint64_t key = 0xCBF29CE484222325ull;
    if (dir || seed) {
        key = Fnv1a(key, kCacheFormat, std::strlen(kCacheFormat) + 1);
        key = Fnv1a(key, kVitaPrelude, std::strlen(kVitaPrelude) + 1);
        key = Fnv1a(key, vertex, std::strlen(vertex) + 1);
        key = Fnv1a(key, fragment, std::strlen(fragment) + 1);
        char name[40];
        std::snprintf(name, sizeof name, "/%016llX.tegxp", (unsigned long long)key);
        if (dir) path = std::string(dir) + name;
        // the pair from a cache file (the memory card's, else the one shipped in the package: the same bytes the first
        // launch would compile and store -- libshacccg's output for the same sources)
        const auto load = [&](const std::string& from, bool shipped) -> uint32_t {
            std::vector<uint8_t> file;
            CacheHeader h{};
            if (!ReadFile(from, &file) || file.size() <= sizeof h) return 0;
            std::memcpy(&h, file.data(), sizeof h);
            size_t n = 0;
            const uint8_t* vs_blob = file.data() + sizeof h;
            const uint8_t* fs_blob = vs_blob + h.vs_size;
            if (!(h.magic == kCacheMagic && h.header_size == sizeof h && h.key == key &&
                  sizeof h + size_t(h.vs_size) + size_t(h.fs_size) == file.size() && GxpOf(vs_blob, h.vs_size, &n) &&
                  GxpOf(fs_blob, h.fs_size, &n))) {
                std::fprintf(stderr, "[gl] program %s: %s does not parse: %s\n", label, from.c_str(),
                             shipped ? "not used" : "compiled again");
                return 0;
            }
            GLuint vs = glCreateShader(GL_VERTEX_SHADER);
            glShaderBinary(1, &vs, 0, vs_blob, GLsizei(h.vs_size));
            GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderBinary(1, &fs, 0, fs_blob, GLsizei(h.fs_size));
            GLint vok = 0, fok = 0;
            glGetShaderiv(vs, GL_COMPILE_STATUS, &vok);
            glGetShaderiv(fs, GL_COMPILE_STATUS, &fok);
            const uint32_t program = (vok && fok) ? LinkProgram(vs, fs) : 0;
            if (program && keep_vs) *keep_vs = vs;
            else glDeleteShader(vs);
            if (program && keep_fs) *keep_fs = fs;
            else glDeleteShader(fs);
            if (!program) {
                std::fprintf(stderr, "[gl] program %s: the cached binaries %s did not link: %s\n", label, from.c_str(),
                             shipped ? "not used" : "compiled again");
                return 0;
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++g_cache_loaded;
            if (shipped) ++g_cache_shipped;
            g_cache_ms += ms;
            size_t vn = 0, fn = 0;
            const uint8_t* vg = GxpOf(vs_blob, h.vs_size, &vn);
            const uint8_t* fg = GxpOf(fs_blob, h.fs_size, &fn);
            if (g_capture_gxps) CaptureGxps(program, vg, vn, fg, fn);
            std::fprintf(stderr, "[gl] program %s: loaded from the %s in %.1f ms (%016llX); vertex %s; fragment %s\n", label,
                         shipped ? "shipped shader cache" : "shader cache", ms, (unsigned long long)key,
                         GxpStats(vg, vn).c_str(), GxpStats(fg, fn).c_str());
            return program;
        };
        if (dir) {
            if (const uint32_t program = load(path, false)) return program;
        }
        if (seed) {
            if (const uint32_t program = load(std::string(seed) + name, true)) return program;
        }
    }
#endif
    // the given compiled objects linked as they are (the caller keeps them), else the pair compiled here
    const bool given = link_vs && link_fs;
    GLuint vs = given ? link_vs : CompileShader(GL_VERTEX_SHADER, vertex);
    if (!vs) return 0;
    GLuint fs = given ? link_fs : CompileShader(GL_FRAGMENT_SHADER, fragment);
    if (!fs) { glDeleteShader(vs); return 0; }
    const uint32_t program = LinkProgram(vs, fs);
#if PS2R_HOST_VITA
    if (program) {
        ++g_cache_compiled;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        g_cache_ms += ms;
        // the pair's binaries, as vitaGL serializes them (for the cache, and the GPU cost line either way)
        GLint length = 0;
        glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
        std::vector<uint8_t> bin(length > 0 ? size_t(length) : 0);
        GLsizei got = 0;
        GLenum format = 0;
        std::vector<uint8_t> vbin, fbin;
        bool split = false;
        if (!bin.empty()) {
            glGetProgramBinary(program, length, &got, &format, bin.data());
            bin.resize(size_t(std::max<GLsizei>(0, std::min<GLsizei>(got, length))));
            split = SplitProgramBinary(bin, &vbin, &fbin);
        }
        std::string stats = "(no binary)";
        if (split) {
            size_t vn = 0, fn = 0;
            const uint8_t* vg = GxpOf(vbin.data(), vbin.size(), &vn);
            const uint8_t* fg = GxpOf(fbin.data(), fbin.size(), &fn);
            if (g_capture_gxps) CaptureGxps(program, vg, vn, fg, fn);
            stats = "vertex " + GxpStats(vg, vn) + "; fragment " + GxpStats(fg, fn);
        }
        bool stored = false;
        if (dir && split) {
            sceIoMkdir(dir, 0777);
            CacheHeader h{kCacheMagic, uint32_t(sizeof(CacheHeader)), key, uint32_t(vbin.size()), uint32_t(fbin.size())};
            std::vector<uint8_t> file(sizeof h);
            std::memcpy(file.data(), &h, sizeof h);
            file.insert(file.end(), vbin.begin(), vbin.end());
            file.insert(file.end(), fbin.begin(), fbin.end());
            stored = WriteFileAtomic(path, file);
            g_cache_stored += stored ? 1 : 0;
        }
        std::fprintf(stderr, "[gl] program %s: %s in %.1f ms%s; %s\n", label, given ? "linked from compiled shaders" : "compiled", ms,
                     !dir ? " (no shader cache: TE_GL_SHADER_CACHE unset or 0)" : stored ? ", stored in the shader cache" :
                     ", NOT stored in the shader cache", stats.c_str());
    }
#endif
    if (given) return program;   // (the objects stay the caller's)
    if (program && keep_vs) *keep_vs = vs;
    else glDeleteShader(vs);
    if (program && keep_fs) *keep_fs = fs;
    else glDeleteShader(fs);
    return program;
}

#if PS2R_HOST_VITA
// ---- the LEAN PS2 programs (the Vita) ---------------------------------------------------------------------------------
// The device's GPU (an SGX543MP4+, tile-based and deferred) spent ~40 ms a frame on the PS2 fragment program: the uber
// program emulates the whole GS pixel pipeline under ~45 uniform-driven branches (2475 USSE instructions; the fast
// specialisation 229 with 44 branches), every fragment executes a couple of hundred of them, and its alpha test's discard
// makes EVERY draw a punch-through object, so the hidden-surface removal that shades an opaque pixel once never applies.
// Each lean program is GlShaderSources::LeanPs2Fragment for one draw state (its #defines: kLeanVariants), the PS2
// program's arithmetic with the state's arms only: no branches on uniforms, a discard only where an alpha test can fail
// with AFAIL KEEP. SelectPs2 reads the state from the full program's uniforms as the uniform cache last sent them, so a
// lean program takes a draw only when the full program would have run exactly that state's arms; anything else draws
// with the fast program (or the full one) as before, counted by reason.
namespace {
struct LeanVariant {
    const char* label;
    int tex, tfx, tcc, mip, atest, nocolor;
};
// The family: the states the Vita's draws take (the census over the title, the hub walk, the Mech arena and the crystal
// cave -- every draw of those four was one of these; TFX DECAL / HIGHLIGHT(2), TCC 0 with colour and AFAIL ZB_ONLY /
// RGB_ONLY never occurred: the source has their arms, a line here adds a variant). Sony's compiler, fragment USSE
// instructions (Vita3K's libshacccg): 2 / 8 / 20 / 30 / 43 / 41 / 54, no branches (the fast program: 229, 44 branches).
const LeanVariant kLeanVariants[] = {
    {"no colour", 0, 0, 0, 0, 0, 1},
    {"untextured", 0, 0, 0, 0, 0, 0},
    {"untextured, alpha test", 0, 0, 0, 0, 1, 0},
    {"modulate", 1, 0, 1, 0, 0, 0},
    {"modulate mip", 1, 0, 1, 1, 0, 0},
    {"modulate, alpha test", 1, 0, 1, 0, 1, 0},
    {"modulate mip, alpha test", 1, 0, 1, 1, 1, 0},
};
constexpr int kLeanMax = int(sizeof kLeanVariants / sizeof kLeanVariants[0]);

std::string LeanSource(const LeanVariant& v) {
    std::string s = GlShaderSources::LeanPs2Fragment();
    if (v.mip) {
        // TE_GFX_VITA_MIP_FORM (the mip variants' LOD; diagnosis -- 0 / unset = the PS2 program's, the text unchanged): the
        // other forms are the same source with one line replaced, so the default programs keep their text (and their
        // shader-cache keys) and every form keeps the rest of the arithmetic.
        //   1 = the retail LOD alone (no footprint floor: GSMIP_AAF off -- the GS's own rule)
        //   2 = the HARDWARE LOD (texture(): the sampler's footprint from the UV derivatives, the retail term unused)
        //   3 = the same LOD with fewer operations: -log2|Q| for log2(1/max(|Q|, 1e-30)) and log2(r2) without its 1e-30
        //       floor (equal values; the rounding of the reciprocal is the only difference)
        static const int form = [] {
            const char* f = std::getenv("TE_GFX_VITA_MIP_FORM");
            const int n = f && *f ? std::atoi(f) : 0;
            if (n) std::fprintf(stderr, "[gl] lean mip variants: LOD form %d (TE_GFX_VITA_MIP_FORM; diagnosis)\n", n);
            return n;
        }();
        auto replace = [&s](const char* from, const char* to) {
            const size_t at = s.find(from);
            if (at != std::string::npos) s.replace(at, std::strlen(from), to);
        };
        if (form == 1) {
            replace("lod = max(min(lod, float(u_mip_ctl.x)), gsm_lam);", "lod = min(lod, float(u_mip_ctl.x));");
        } else if (form == 2) {
            replace("textureLod(u_texture, uv, lod)", "texture(u_texture, uv)");
        } else if (form == 3) {
            replace("float lod = log2(1.0 / max(q_abs, 1e-30)) * u_mip_lodp.y + u_mip_lodp.x;",
                    "float lod = log2(q_abs) * (-u_mip_lodp.y) + u_mip_lodp.x;");
            replace("float gsm_lam = 0.5 * log2(max(gsm_r2, 1e-30));", "float gsm_lam = 0.5 * log2(gsm_r2);");
        }
    }
    char defs[200];
    std::snprintf(defs, sizeof defs, "#define LEAN_TEX %d\n#define LEAN_TFX %d\n#define LEAN_TCC %d\n#define LEAN_MIP %d\n"
                  "#define LEAN_ATEST %d\n#define LEAN_NOCOLOR %d\n", v.tex, v.tfx, v.tcc, v.mip, v.atest, v.nocolor);
    const size_t version = s.find("#version");
    const size_t line = version == std::string::npos ? std::string::npos : s.find('\n', version);
    if (line == std::string::npos) return defs + s;
    s.insert(line + 1, defs);
    return s;
}

// the full program's uniforms a lean program mirrors (each lean program has the ones its code reads; -1 elsewhere)
struct MirrorSpec { const char* name; int GlUniformLocations::*field; };
const MirrorSpec kLeanMirrors[] = {
    {"u_screen_size", &GlUniformLocations::screen_size}, {"u_uv_scale", &GlUniformLocations::uv_scale},
    {"u_uv_mode", &GlUniformLocations::uv_mode},         {"u_native", &GlUniformLocations::native},
    {"u_mesh_lit", &GlUniformLocations::mesh_lit},       {"u_tex_size", &GlUniformLocations::tex_size},
    {"u_mip_ctl", &GlUniformLocations::mip_ctl},         {"u_mip_lodp", &GlUniformLocations::mip_lodp},
};
constexpr int kMirrors = int(sizeof kLeanMirrors / sizeof kLeanMirrors[0]);
const MirrorSpec kLeanArrayMirrors[] = {
    {"u_mesh", &GlUniformLocations::mesh}, {"u_mesh_light", &GlUniformLocations::mesh_light},
};
constexpr int kArrayMirrors = int(sizeof kLeanArrayMirrors / sizeof kLeanArrayMirrors[0]);

// the full program's uniforms that select a lean program (read from the uniform cache's shadows at every draw)
enum KeyField { kUseTexture, kTfx, kTcc, kMipCtl, kLodHw, kAte, kAtst, kAref, kAfail, kFba, kFog, kBlend, kKeyFields };
const MirrorSpec kKeySpecs[kKeyFields] = {
    {"u_use_texture", &GlUniformLocations::use_texture}, {"u_tfx", &GlUniformLocations::tfx},
    {"u_tcc", &GlUniformLocations::tcc},                 {"u_mip_ctl", &GlUniformLocations::mip_ctl},
    {"u_tex_lod_hw", &GlUniformLocations::tex_lod_hw},   {"u_ate", &GlUniformLocations::ate},
    {"u_atst", &GlUniformLocations::atst},               {"u_aref", &GlUniformLocations::aref},
    {"u_afail", &GlUniformLocations::afail},             {"u_fba", &GlUniformLocations::fba},
    {"u_fog_enabled", &GlUniformLocations::fog_enabled}, {"u_blend_enabled", &GlUniformLocations::blend_enabled},
};

bool SameShadow(const GlUniformCache::Shadow& a, const GlUniformCache::Shadow& b) {
    return a.kind == b.kind && a.i[0] == b.i[0] && a.i[1] == b.i[1] && a.f[0] == b.f[0] && a.f[1] == b.f[1] &&
           a.f[2] == b.f[2] && a.f[3] == b.f[3];
}

void SendShadow(int loc, const GlUniformCache::Shadow& s) {
    switch (s.kind) {
        case 1: glUniform1i(loc, s.i[0]); break;
        case 2: glUniform1f(loc, s.f[0]); break;
        case 3: glUniform2f(loc, s.f[0], s.f[1]); break;
        case 4: glUniform3f(loc, s.f[0], s.f[1], s.f[2]); break;
        case 5: glUniform2i(loc, s.i[0], s.i[1]); break;
        case 6: glUniform4f(loc, s.f[0], s.f[1], s.f[2], s.f[3]); break;
        default: return;
    }
    Census().lean_uniform_calls++;
}

// fallback reasons (the census)
enum Fallback { kFbNotFast, kFbUnknown, kFbFog, kFbMipMode, kFbNoVariant, kFbOff, kFallbacks };
const char* const kFallbackNames[kFallbacks] = {"a fast constant does not hold", "a state uniform never set", "fog",
                                               "LCM / hardware LOD", "no lean variant", "lean programs off"};
}  // namespace

// ---- the RAW MESH DRAWS (the Vita; GlShaderLibrary::RawMesh) ------------------------------------------------------------
// vitaGL r1488's globals and helpers they use (all extern in libvitaGL.a: nm): the context and shader patcher it draws
// with, its current blend state (blend_config: the SceGxmBlendInfo its next draw patches the fragment program for),
// multisampling and the frame target's float format (a byte each: the toolchain's short enums / GLboolean), its texture
// records (shared.h struct texture: last_frame at 0, the SceGxmTexture at 12 -- checked against a texture's known size
// before the first raw draw that samples one), its frame count, its progressive index buffer, its uniform dirty flags,
// the scene start of its draws and its uniform ring.
extern "C" {
extern SceGxmContext* gxm_context;
extern SceGxmShaderPatcher* gxm_shader_patcher;
extern SceGxmBlendInfo blend_info;
extern uint8_t msaa_mode;
extern uint8_t is_fbo_float;
extern uint8_t texture_slots[];
extern uint32_t vgl_framecount;
extern uint16_t* default_idx_ptr;
extern uint8_t dirty_shader_vert_unifs, dirty_shader_frag_unifs;
void scene_reset(void);
void* vglReserveUniformCircularPoolBuffer(uint32_t size);
// the platform layer's record of what the GXM context was last given (platform/nativeee/host/vita_gpu_census.cpp: every
// GXM call goes through its wrappers, vitaGL's too; forgotten at a scene's begin / end). Weak: without it every raw draw
// sets its programs and fragment uniforms.
extern const void* te_gxm_last_vertex_program __attribute__((weak));
extern const void* te_gxm_last_fragment_program __attribute__((weak));
extern const void* te_gxm_last_fragment_uniforms __attribute__((weak));
// ... and the fragment texture unit 0 (its SceGxmTexture pointer and words as handed over) and the vertex stream
// 0's data pointer
extern const void* te_gxm_last_texture0 __attribute__((weak));
extern uint32_t te_gxm_last_texture0_words[4] __attribute__((weak));
extern const void* te_gxm_last_stream0 __attribute__((weak));
// ... and the vertex default uniform buffer (forgotten also at a real vertex program change and when GXM
// reserves one)
extern const void* te_gxm_last_vertex_uniforms __attribute__((weak));
}

namespace {
// memcmp's equality answer for the raw draws' small images and records without the library call (newlib's
// memcmp: a call and a word loop with two branches a word) -- four words at a time ORed, the first difference ending it;
// the bytes past the last whole word one by one. The same answer as memcmp(a, b, bytes) == 0.
inline bool SameBytes(const void* a, const void* b, uint32_t bytes) {
    const uint8_t* const x = static_cast<const uint8_t*>(a);
    const uint8_t* const y = static_cast<const uint8_t*>(b);
    uint32_t i = 0;
    for (; i + 16 <= bytes; i += 16) {
        uint32_t p[4], q[4];
        std::memcpy(p, x + i, 16);
        std::memcpy(q, y + i, 16);
        if ((p[0] ^ q[0]) | (p[1] ^ q[1]) | (p[2] ^ q[2]) | (p[3] ^ q[3])) return false;
    }
    for (; i + 4 <= bytes; i += 4) {
        uint32_t p, q;
        std::memcpy(&p, x + i, 4);
        std::memcpy(&q, y + i, 4);
        if (p != q) return false;
    }
    for (; i < bytes; ++i)
        if (x[i] != y[i]) return false;
    return true;
}

// THE RAW DRAWS' REDUNDANT SETS (TE_GFX_VITA_SKIP_SETS=0: every raw draw sets its texture and stream as before).
// A raw draw (RawSubmit, a packet's replay, the skins') hands GXM its unit-0 texture and its stream 0 at every draw; the
// device's census: 782 texture sets and 861 stream sets a hub frame for 789 draws. A set the context already holds -- the
// same SceGxmTexture with the same four words as last handed over for unit 0 (the census wrappers' record, forgotten at
// every scene begin / end), the same data pointer for stream 0 -- is skipped: GXM keeps both as context state between
// draws, so the draw reads exactly what the set would have given it, and its own state emission has nothing new to write.
int g_skip_sets = -1;   // -1 not read, 0 off, 1 on
bool SkipSetsOn() {
    if (g_skip_sets < 0) {
        const char* e = std::getenv("TE_GFX_VITA_SKIP_SETS");
        g_skip_sets = (e && e[0] == '0') ? 0 : 1;
        const bool known = &te_gxm_last_texture0 && &te_gxm_last_texture0_words && &te_gxm_last_stream0;
        if (!known) g_skip_sets = 0;
        std::fprintf(stderr, "[gl] raw draws' redundant GXM sets (TE_GFX_VITA_SKIP_SETS): %s\n",
                     g_skip_sets ? "skipped (a texture / stream the context already holds is not set again)"
                                 : known ? "off (every raw draw sets its texture and stream)" : "off (the GXM call census is not linked)");
    }
    return g_skip_sets != 0;
}
inline void RawSetTexture0(const SceGxmTexture* t) {
    Census().raw_texture_sets++;
    if (SkipSetsOn() && te_gxm_last_texture0 == t && SameBytes(te_gxm_last_texture0_words, t, 16)) {
        Census().raw_texture_skipped++;
        return;
    }
    sceGxmSetFragmentTexture(gxm_context, 0, t);
}
inline void RawSetStream0(const void* p) {
    Census().raw_stream_sets++;
    if (SkipSetsOn() && te_gxm_last_stream0 == p) {
        Census().raw_stream_skipped++;
        return;
    }
    sceGxmSetVertexStream(gxm_context, 0, p);
}

// THE RAW MESH DRAWS' VERTEX UNIFORMS RE-USED (TE_GFX_VITA_GXM_CACHE=0 or =wrappers: every raw mesh draw
// reserves and sets its own, as before the cache). A raw mesh draw's vertex default uniform buffer holds the screen size, the
// UV prescale, its u_mesh rows (the screen matrix, the GS origin, the UV scroll) and, lit, its u_mesh_light rows -- the same
// bytes for consecutive draws of one transform (the parts of one model). When the composed contents equal the buffer the
// last raw mesh draw set, and the context still holds that buffer for the same vertex program (te_gxm_last_vertex_uniforms:
// nothing set or reserved since and no real program change -- the census wrappers forget it then and at every scene begin /
// end), the draw reads exactly what a fresh buffer would give it: none is reserved, none set. (GXM's default uniform buffer
// is context state that every draw reads until another is set: vitaGL's own draws rely on it -- they bind one only when
// their uniforms are dirty, the draws in between read the one bound.) The contents are composed in full (the padding
// between the parameters zero; no program reads it: every uniform of a twin's programs is one the raw draw writes,
// BuildRawTwin) and copied into the reserved buffer whole.
int g_vertex_reuse = -1;   // -1 not read, 0 off, 1 on
bool VertexReuseOn() {
    if (g_vertex_reuse < 0) {
        const char* e = std::getenv("TE_GFX_VITA_GXM_CACHE");
        const bool known = &te_gxm_last_vertex_uniforms && &te_gxm_last_vertex_program;
        g_vertex_reuse = (!known || (e && (e[0] == '0' || e[0] == 'w'))) ? 0 : 1;
        std::fprintf(stderr, "[gl] raw mesh draws' vertex uniforms (TE_GFX_VITA_GXM_CACHE): %s\n",
                     g_vertex_reuse ? "re-used when the context holds the same contents for the same program"
                                    : known ? "a buffer per draw (off)" : "a buffer per draw (the GXM call census is not linked)");
    }
    return g_vertex_reuse != 0;
}
constexpr uint32_t kRawVertexImage = 512;   // the largest vertex default uniform buffer composed here (BuildRawTwin's are ~100-220 B)
// two images: the one the context was last given (g_vert_last) and a spare a packet replay composes into
// (RawReplayVertexImage); a replay whose image is set swaps them -- the image it composed IS the last one, no copy of it
alignas(8) uint8_t g_vert_images[2][kRawVertexImage];
uint8_t* g_vert_last = g_vert_images[0];
uint8_t* g_vert_spare = g_vert_images[1];
const void* g_vert_spare_prog = nullptr;   // the layout (a vertex program, its buffer's size) whose padding the spare holds
uint32_t g_vert_spare_size = 0;            //  zeroed: a twin of that layout rewrites its fields whole at every replay
const void* g_vert_last_buf = nullptr;
const void* g_vert_last_prog = nullptr;
uint32_t g_vert_last_size = 0;
// the composed vertex uniforms `img` (`size` bytes, the twin's vsize) of a draw of vertex program `vprog` (already set)
inline bool RawVertexUniformsKept(const uint8_t* img, uint32_t size, const void* vprog) {
    return VertexReuseOn() && g_vert_last_buf && te_gxm_last_vertex_uniforms == g_vert_last_buf && g_vert_last_prog == vprog &&
           te_gxm_last_vertex_program == vprog && g_vert_last_size == size && SameBytes(g_vert_last, img, size);
}
inline void RawSetVertexUniforms(const uint8_t* img, uint32_t size, const void* vprog) {
    Census().raw_vertex_uniform_sets++;
    if (RawVertexUniformsKept(img, size, vprog)) {
        Census().raw_vertex_uniform_kept++;
        return;
    }
    uint8_t* vb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(size));
    std::memcpy(vb, img, size);
    sceGxmSetVertexDefaultUniformBuffer(gxm_context, vb);
    std::memcpy(g_vert_last, img, size);   // (the spare and its layout untouched)
    g_vert_last_buf = vb;
    g_vert_last_prog = vprog;
    g_vert_last_size = size;
}
// the spare image made ready for a twin of layout (vprog, size): its padding zeroed when it last held another's
inline uint8_t* RawVertexSpare(const void* vprog, uint32_t size) {
    if (g_vert_spare_prog != vprog || g_vert_spare_size != size) {
        std::memset(g_vert_spare, 0, size);
        g_vert_spare_prog = vprog;
        g_vert_spare_size = size;
    }
    return g_vert_spare;
}
// ... and set: kept when the context holds these contents for this program, else a buffer reserved, copied and set, the
// spare becoming the last image (and the old last the spare, its layout with it)
inline void RawSetVertexSpare(uint32_t size, const void* vprog) {
    Census().raw_vertex_uniform_sets++;
    if (RawVertexUniformsKept(g_vert_spare, size, vprog)) {
        Census().raw_vertex_uniform_kept++;
        return;
    }
    uint8_t* vb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(size));
    std::memcpy(vb, g_vert_spare, size);
    sceGxmSetVertexDefaultUniformBuffer(gxm_context, vb);
    std::swap(g_vert_last, g_vert_spare);
    g_vert_spare_prog = g_vert_last_prog;   // (the old last image: composed zero-padded for its own layout)
    g_vert_spare_size = g_vert_last_size;
    g_vert_last_buf = vb;
    g_vert_last_prog = vprog;
    g_vert_last_size = size;
}
}  // namespace

namespace {
constexpr size_t kVglTextureBytes = 56;      // sizeof(texture) in r1488 (texture_slots: 0xE0000 bytes / 16384)
constexpr size_t kVglTextureGxmOffset = 12;  // offsetof(texture, gxm_tex)
constexpr uint32_t kVglTextures = 16384;
constexpr uint32_t kRawMaxVerts = 0xC000;    // vitaGL's progressive index buffer (MAX_IDX_NUMBER)
int g_raw_layout = 0;                        // vitaGL's texture record checked: 0 not yet, 1 as expected, -1 not (raw off)

// One lean mesh-path program's twin: its GXPs registered with the patcher, the vertex program for the native mesh's
// MeshVertex stream, a fragment program per blend state met, the default uniform buffers' layouts
struct RawTwin {
    std::vector<uint8_t> vgxp, fgxp;
    SceGxmShaderPatcherId vid = nullptr, fid = nullptr;
    SceGxmVertexProgram* vprog = nullptr;
    struct Frag { uint32_t blend; uint8_t fbo_float; SceGxmFragmentProgram* fp; };
    std::vector<Frag> frags;
    struct U { int offset = -1; int type = 0; };   // byte offset in the default buffer (-1 absent), SCE_GXM_PARAMETER_TYPE_*
    uint32_t vsize = 0, fsize = 0;
    U v_screen, v_uvscale, v_mesh, v_light;
    U v_shift;              // (the instanced path) u_mesh_shift: u_mesh[4]'s place
    U f_texsize, f_mipctl, f_miplodp, f_at, f_fail, f_fba;
    bool sampler = false;   // the fragment program samples u_texture (unit 0)
    bool instanced = false; // the instanced mesh path's twin: the matrix rows from stream 1, one element per instance
    bool frag_as_mesh = false;   // (instanced) its fragment uniforms laid out as the mesh path twin's of its variant
    // its vid / vprog are another twin's of the same vertex path (BuildRawTwin's `share`: the same vertex GXP
    // bytes, the same attributes) -- registered and released by that twin, never by this one
    bool shares_vertex = false;
    bool ok = false;
};
// the twins of the mesh paths' programs: [0] the mesh path, [1] the lit mesh path, by lean variant
RawTwin g_raw_twins[2][kLeanMax];
// the twins of the INSTANCED mesh path's programs (GlShaderSources::LeanPs2InstancedVertex), by lean variant: a
// run of the mesh path's draws of one packet and one mesh range drawn as one instanced draw (DrawRawPacketInstances)
RawTwin g_inst_twins[kLeanMax];
constexpr uint32_t kInstanceRowBytes = 64;   // a_mesh0..a_mesh3: the matrix rows (u_mesh[0..3]; the shift is u_mesh_shift)
constexpr int kMaxInstances = 64;            // instances per instanced draw (the rows: 4 KB of the uniform ring)

void ReleaseRawTwin(RawTwin& t) {
    if (gxm_shader_patcher) {
        for (const RawTwin::Frag& f : t.frags) sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher, f.fp);
        if (t.vprog && !t.shares_vertex) sceGxmShaderPatcherReleaseVertexProgram(gxm_shader_patcher, t.vprog);
        if (t.vid && !t.shares_vertex) sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher, t.vid);
        if (t.fid) sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher, t.fid);
    }
    t = RawTwin();
}

// The twin of a program from its GXPs; `lit`: the lit mesh path's attributes. False (the program draws through vitaGL)
// when anything is not as the raw draw writes it: an attribute or default uniform it does not know, a type it does not
// convert, a sampler other than u_texture on unit 0.
// `share`: a built twin of the same vertex path (the same `lit`) whose vertex program this one may take
bool BuildRawTwin(RawTwin& t, const ProgramGxps& g, bool lit, bool instanced = false, const RawTwin* share = nullptr) {
    t = RawTwin();
    t.instanced = instanced;
    t.vgxp = g.vertex;
    t.fgxp = g.fragment;
    const SceGxmProgram* vp = reinterpret_cast<const SceGxmProgram*>(t.vgxp.data());
    const SceGxmProgram* fp = reinterpret_cast<const SceGxmProgram*>(t.fgxp.data());
    if (!gxm_shader_patcher || sceGxmProgramCheck(vp) < 0 || sceGxmProgramCheck(fp) < 0) return false;
    struct A { const char* name; uint16_t offset; SceGxmAttributeFormat format; uint8_t count; };
    const A unlit_attrs[] = {{"a_position", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3}, {"a_texcoord", 12, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3},
                             {"a_color", 24, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4}};
    const A lit_attrs[] = {{"a_position", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3}, {"a_texcoord", 12, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3},
                           {"a_normal", 32, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3}, {"a_litcolor", 44, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4}};
    static_assert(sizeof(MeshVertex) == 48, "the raw mesh stream is MeshVertex");
    static_assert(offsetof(MeshVertex, s) == 12 && offsetof(MeshVertex, r) == 24 && offsetof(MeshVertex, nx) == 32 &&
                  offsetof(MeshVertex, lr) == 44, "the raw mesh attributes' offsets");
    // (the instanced path: the mesh path's attributes, then its four matrix rows on stream 1 -- one 64-byte element per
    // instance)
    const A inst_attrs[] = {{"a_position", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3}, {"a_texcoord", 12, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3},
                            {"a_color", 24, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4}, {"a_mesh0", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 4},
                            {"a_mesh1", 16, SCE_GXM_ATTRIBUTE_FORMAT_F32, 4}, {"a_mesh2", 32, SCE_GXM_ATTRIBUTE_FORMAT_F32, 4},
                            {"a_mesh3", 48, SCE_GXM_ATTRIBUTE_FORMAT_F32, 4}};
    const A* table = instanced ? inst_attrs : lit ? lit_attrs : unlit_attrs;
    const int table_n = instanced ? 7 : lit ? 4 : 3;
    SceGxmVertexAttribute attrs[8];
    int n = 0;
    for (int k = 0; k < table_n; ++k) {
        const SceGxmProgramParameter* p = sceGxmProgramFindParameterByName(vp, table[k].name);
        if (!p) {
            if (instanced && k >= 3) return false;   // (every row is read: the transform)
            continue;   // not read by this program
        }
        if (sceGxmProgramParameterGetCategory(p) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) return false;
        attrs[n].streamIndex = instanced && k >= 3 ? 1 : 0;
        attrs[n].offset = table[k].offset;
        attrs[n].format = table[k].format;
        attrs[n].componentCount = table[k].count;
        attrs[n].regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(p));
        ++n;
    }
    // every parameter of both programs must be one the raw draw feeds
    auto layout = [](const SceGxmProgram* prog, const char* const* names, RawTwin::U* const* outs, int names_n, int* attrs_seen,
                     bool* sampler) -> bool {
        const unsigned cnt = sceGxmProgramGetParameterCount(prog);
        for (unsigned i = 0; i < cnt; ++i) {
            const SceGxmProgramParameter* p = sceGxmProgramGetParameter(prog, i);
            const SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(p);
            const char* name = sceGxmProgramParameterGetName(p);
            if (cat == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) { ++*attrs_seen; continue; }
            if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
                if (!sampler || std::strcmp(name, "u_texture") != 0 || sceGxmProgramParameterGetResourceIndex(p) != 0) return false;
                *sampler = true;
                continue;
            }
            if (cat != SCE_GXM_PARAMETER_CATEGORY_UNIFORM || sceGxmProgramParameterGetContainerIndex(p) != 14) return false;
            int k = 0;
            while (k < names_n && std::strcmp(name, names[k]) != 0) ++k;
            if (k == names_n) return false;
            const SceGxmParameterType type = sceGxmProgramParameterGetType(p);
            if (type != SCE_GXM_PARAMETER_TYPE_F32 && !(std::strcmp(name, "u_mip_ctl") == 0 &&
                                                       (type == SCE_GXM_PARAMETER_TYPE_S32 || type == SCE_GXM_PARAMETER_TYPE_U32)))
                return false;
            outs[k]->offset = int(sceGxmProgramParameterGetResourceIndex(p) * 4);
            outs[k]->type = int(type);
        }
        return true;
    };
    const char* const vnames[] = {"u_screen_size", "u_uv_scale", "u_mesh", "u_mesh_light"};
    RawTwin::U* const vouts[] = {&t.v_screen, &t.v_uvscale, &t.v_mesh, &t.v_light};
    const char* const inames[] = {"u_screen_size", "u_uv_scale", "u_mesh_shift"};   // (the instanced path's)
    RawTwin::U* const iouts[] = {&t.v_screen, &t.v_uvscale, &t.v_shift};
    const char* const fnames[] = {"u_tex_size", "u_mip_ctl", "u_mip_lodp", "u_lean_at", "u_lean_fail", "u_lean_fba"};
    RawTwin::U* const fouts[] = {&t.f_texsize, &t.f_mipctl, &t.f_miplodp, &t.f_at, &t.f_fail, &t.f_fba};
    int vattrs = 0, fattrs = 0;
    // (the instanced path's vertex uniforms: the screen size, the UV prescale and the shift -- its matrix is the rows)
    if (!(instanced ? layout(vp, inames, iouts, 3, &vattrs, nullptr) : layout(vp, vnames, vouts, 4, &vattrs, nullptr)) || vattrs != n)
        return false;
    if (instanced && t.v_shift.offset < 0) return false;
    if (!layout(fp, fnames, fouts, 6, &fattrs, &t.sampler) || fattrs != 0) return false;
    t.vsize = sceGxmProgramGetDefaultUniformBufferSize(vp);
    t.fsize = sceGxmProgramGetDefaultUniformBufferSize(fp);
    // THE VARIANTS' ONE VERTEX PROGRAM: each vertex path's shader is compiled once and linked with every lean
    // variant's fragment shader (BuildLeanPrograms), so its variants' vertex GXPs are the same bytes -- seven registrations of
    // one program made seven vertex programs of one code, and every switch of variant between two raw draws a vertex
    // program change (a GXM call, and the end of the vertex uniforms' re-use). A twin whose vertex GXP and attributes are
    // `share`'s (the same path's first twin) takes its vertex program: the same code with the same attributes and stream --
    // what the patcher would make again -- so a draw reads and computes exactly what its own would.
    if (share && share->ok && !share->instanced && !instanced && !share->shares_vertex && share->vid && share->vprog &&
        share->vgxp == t.vgxp && share->vsize == t.vsize && share->v_screen.offset == t.v_screen.offset &&
        share->v_uvscale.offset == t.v_uvscale.offset && share->v_mesh.offset == t.v_mesh.offset &&
        share->v_light.offset == t.v_light.offset) {
        if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, fp, &t.fid) < 0) return false;
        t.vid = share->vid;
        t.vprog = share->vprog;
        t.shares_vertex = true;
        t.ok = true;
        return true;
    }
    if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, vp, &t.vid) < 0) return false;
    if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, fp, &t.fid) < 0) return false;
    const SceGxmVertexStream streams[2] = {{uint16_t(sizeof(MeshVertex)), uint16_t(SCE_GXM_INDEX_SOURCE_INDEX_16BIT)},
                                           {uint16_t(kInstanceRowBytes), uint16_t(SCE_GXM_INDEX_SOURCE_INSTANCE_16BIT)}};
    if (sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher, t.vid, attrs, unsigned(n), streams, instanced ? 2 : 1,
                                               &t.vprog) < 0)
        return false;
    t.ok = true;
    return true;
}
// an instanced twin's fragment side is the mesh twin's of its variant when their fragment uniforms lie alike (the
// same fragment source compiled: a packet's composed image then serves both)
bool SameFragmentLayout(const RawTwin& a, const RawTwin& b, const char** why = nullptr) {
    auto same = [](const RawTwin::U& x, const RawTwin::U& y) { return x.offset == y.offset && x.type == y.type; };
    const char* w = !a.ok || !b.ok ? "a twin not built" : a.fsize != b.fsize ? "the uniform buffer's size"
                    : a.sampler != b.sampler ? "the sampler" : !same(a.f_texsize, b.f_texsize) ? "u_tex_size"
                    : !same(a.f_mipctl, b.f_mipctl) ? "u_mip_ctl" : !same(a.f_miplodp, b.f_miplodp) ? "u_mip_lodp"
                    : !same(a.f_at, b.f_at) ? "u_lean_at" : !same(a.f_fail, b.f_fail) ? "u_lean_fail"
                    : !same(a.f_fba, b.f_fba) ? "u_lean_fba" : a.fgxp != b.fgxp ? "the fragment GXP's bytes" : nullptr;
    if (why) *why = w;
    return w == nullptr;
}

// The raw draw's values (the full program's uniform shadows, as SelectPs2 sends them to a lean program)
struct RawValues {
    const GlUniformCache::Shadow* screen;
    const GlUniformCache::Shadow* uv_scale;
    const GlUniformCache::Shadow* tex_size;
    const GlUniformCache::Shadow* mip_ctl;
    const GlUniformCache::Shadow* mip_lodp;
    const GlUniformCache::ArrayShadow* mesh;
    const GlUniformCache::ArrayShadow* light;
    float at[3], fail[4], fba;
};

// The fragment default uniform buffer the GXM context was last given by a raw draw (RawSubmit's and a packet replay's):
// its contents and address -- a draw whose composed uniforms are these, while the context still holds that buffer
// (te_gxm_last_fragment_uniforms), sets none
alignas(8) uint8_t g_frag_last[256];
const void* g_frag_last_buf = nullptr;
uint32_t g_frag_last_size = 0;

// ---- the mesh draw PACKETS (the Vita; GlShaderLibrary::TakeRawPacket / DrawRawPacket) ----------------------------------
// A packet is what one raw mesh draw handed GXM apart from the draw's own transform: the twin (its vertex program and
// uniform layouts), the fragment program patched for the blend state it drew under, the texture record it sampled (and
// that record's SceGxmTexture as it was handed over: the sampler state, size, levels and data address), the composed
// fragment uniform image, the vertex constants (the screen size, the UV prescale). A later draw of the same state in the
// same GL context (ps2e_draw vouches for the state, the executor for the context) is that draw again with another
// transform: the replay makes RawSubmit's calls with these values and the draw's own u_mesh / u_mesh_light rows -- no GL
// call, no uniform shadow read, no lean selection. Recorded into a ring of slots; an id names a slot and its generation
// (a reused slot's old ids are stale), the twins' build epoch voids them all.
struct RawPacket {
    uint32_t id = 0;              // 0: empty
    uint32_t epoch = 0;           // g_packet_epoch when recorded
    RawTwin* twin = nullptr;
    SceGxmFragmentProgram* fprog = nullptr;
    uint32_t blend_raw = 0;       // vitaGL's blend_info when recorded (the fragment program's patch)
    uint8_t fbo_float = 0;
    uint8_t* slot = nullptr;      // vitaGL's texture record sampled on unit 0 (null: no sampler)
    uint32_t tex_words[4] = {0, 0, 0, 0};   // its SceGxmTexture as handed to GXM
    float screen[2] = {0.f, 0.f};
    float uv_scale[2] = {0.f, 0.f};
    uint32_t fsize = 0;
    uint8_t fimg[128];
    // the census of the draw replayed (SelectPs2's counted()): its lean program, vertex path, blend, discarding variant
    void* program = nullptr;      // GlShaderLibrary::LeanState::Program*
    int path = 0;
    int variant = 0;              // its lean variant (the fill census's program code: kFillLean + variant)
    bool blend = false;
    bool discards = false;
};
constexpr uint32_t kPackets = 1024;        // a power of two (an id's low bits are its slot)
constexpr uint32_t kPacketSlotBits = 10;
static_assert((1u << kPacketSlotBits) == kPackets, "packet slots");
RawPacket g_packets[kPackets];
uint32_t g_packet_next = 0;     // the next slot to fill (round robin)
uint32_t g_packet_gen = 0;      // bumped per recorded packet: an id = slot | gen << kPacketSlotBits (never 0)
uint32_t g_packet_epoch = 1;    // the twins' build (BuildLeanPrograms bumps it)
uint32_t g_packet_last = 0;     // the packet the last raw mesh draw recorded (TakeRawPacket hands it over once)
int g_packets_mode = -1;        // TE_GFX_VITA_PACKETS: -1 not read, 0 off, 1 on
bool g_packets_on = true;       // ... and on (the in-run A/B's arm)
bool g_instancing = false;      // the instanced packet draws (BuildLeanPrograms: TE_GFX_VITA_INSTANCING, twins built)
bool g_instancing_split = false;   // (diagnosis: TE_GFX_VITA_INSTANCING=2 -- the instanced twin, one plain draw per instance)

// a raw draw's packet, recorded (the census half is SelectPs2's, below). CONTENT-ADDRESSED: a packet whose content is a
// current packet's (the run starts re-draw the same states every frame) takes that packet's slot and id instead of a new
// one -- the ids callers keep stay current, the ring does not wrap over live packets
constexpr uint32_t kPacketIndex = 4096;   // content hash -> slot + 1 (0: none)
uint16_t g_packet_index[kPacketIndex];
uint32_t PacketContentHash(const RawPacket& k) {
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t x) { h = (h ^ x) * 16777619u; };
    mix(uint32_t(uintptr_t(k.twin)));
    mix(uint32_t(uintptr_t(k.fprog)));
    mix(k.blend_raw);
    mix(k.fbo_float);
    mix(uint32_t(uintptr_t(k.slot)));
    for (uint32_t w : k.tex_words) mix(w);
    uint32_t f[4];
    std::memcpy(f, k.screen, 8);
    std::memcpy(f + 2, k.uv_scale, 8);
    for (uint32_t w : f) mix(w);
    mix(k.fsize);
    for (uint32_t i = 0; i + 4 <= k.fsize; i += 4) {
        uint32_t w;
        std::memcpy(&w, k.fimg + i, 4);
        mix(w);
    }
    return (h ^ (h >> 15)) & (kPacketIndex - 1);
}
bool SamePacketContent(const RawPacket& a, const RawPacket& b) {
    return a.twin == b.twin && a.fprog == b.fprog && a.blend_raw == b.blend_raw && a.fbo_float == b.fbo_float && a.slot == b.slot &&
           std::memcmp(a.tex_words, b.tex_words, sizeof a.tex_words) == 0 && std::memcmp(a.screen, b.screen, sizeof a.screen) == 0 &&
           std::memcmp(a.uv_scale, b.uv_scale, sizeof a.uv_scale) == 0 && a.fsize == b.fsize &&
           std::memcmp(a.fimg, b.fimg, a.fsize) == 0;
}
void RecordPacket(RawTwin& t, SceGxmFragmentProgram* fprog, uint32_t blend_raw, uint8_t* slot, const RawValues& v,
                  const uint8_t* img, uint32_t fsize, bool lit_twin) {
    if (!g_packets_on || g_packets_mode == 0 || fsize > sizeof(RawPacket::fimg)) return;
    // an unlit twin that reads the lights would take them from the shadow (whatever a lit draw left): not replayed
    if (!lit_twin && t.v_light.offset >= 0) return;
    if (t.v_mesh.offset < 0) return;
    if ((t.v_screen.offset >= 0 && (!v.screen || v.screen->kind != 3)) || (t.v_uvscale.offset >= 0 && (!v.uv_scale || v.uv_scale->kind != 3)))
        return;
    RawPacket n;
    n.epoch = g_packet_epoch;
    n.twin = &t;
    n.fprog = fprog;
    n.blend_raw = blend_raw;
    n.fbo_float = is_fbo_float;
    n.slot = slot;
    if (slot) std::memcpy(n.tex_words, slot + kVglTextureGxmOffset, sizeof n.tex_words);
    n.screen[0] = t.v_screen.offset >= 0 ? v.screen->f[0] : 0.f;
    n.screen[1] = t.v_screen.offset >= 0 ? v.screen->f[1] : 0.f;
    n.uv_scale[0] = t.v_uvscale.offset >= 0 ? v.uv_scale->f[0] : 0.f;
    n.uv_scale[1] = t.v_uvscale.offset >= 0 ? v.uv_scale->f[1] : 0.f;
    n.fsize = fsize;
    if (fsize) std::memcpy(n.fimg, img, fsize);
    const uint32_t h = PacketContentHash(n);
    if (const uint16_t at = g_packet_index[h]) {
        RawPacket& k = g_packets[at - 1];
        if (k.id && k.epoch == g_packet_epoch && SamePacketContent(k, n)) {
            g_packet_last = k.id;   // the same packet: its id stands (SelectPs2 fills the same census half again)
            return;
        }
    }
    const uint32_t at = g_packet_next;
    g_packet_next = (g_packet_next + 1) & (kPackets - 1);
    if (++g_packet_gen >= (1u << (32 - kPacketSlotBits))) g_packet_gen = 1;
    RawPacket& k = g_packets[at];
    k = n;
    k.id = at | (g_packet_gen << kPacketSlotBits);
    k.program = nullptr;
    k.path = 0;
    g_packet_index[h] = uint16_t(at + 1);
    g_packet_last = k.id;
    Census().packets_recorded++;
}

// -1 drawn; else the reason it was not (the census's raw_mesh_declined index): 2 no twin / no fragment program, 3 a value
// it reads not known, 5 the texture record's layout not as expected. `lit_twin`: the lit mesh path's twin. A drawn raw
// draw records its packet (RecordPacket).
int RawSubmit(RawTwin& t, const RawValues& v, const GlShaderLibrary::RawMesh& d, bool lit_twin) {
    if (!t.ok) return 2;
    if (t.fsize > 256) return 3;   // (checked before any GXM call: the composed image's buffer below)
    auto known2 = [](const RawTwin::U& u, const GlUniformCache::Shadow* s, int kind) { return u.offset < 0 || (s && s->kind == kind); };
    if (!known2(t.v_screen, v.screen, 3) || !known2(t.v_uvscale, v.uv_scale, 3) || !known2(t.f_texsize, v.tex_size, 3) ||
        !known2(t.f_mipctl, v.mip_ctl, 5) || !known2(t.f_miplodp, v.mip_lodp, 3))
        return 3;
    if ((t.v_mesh.offset >= 0 && (!v.mesh || v.mesh->count != 5)) || (t.v_light.offset >= 0 && (!v.light || v.light->count != 7)))
        return 3;
    uint8_t* slot = nullptr;
    if (t.sampler) {
        if (!d.texture || d.texture >= kVglTextures) return 3;
        slot = texture_slots + size_t(d.texture) * kVglTextureBytes;
        if (g_raw_layout <= 0) {
            // vitaGL's texture record as r1488 lays it out: the SceGxmTexture at 12 holds this texture's size
            if (g_raw_layout < 0 || !d.tex_w || !d.tex_h) return 5;
            const SceGxmTexture* gt = reinterpret_cast<const SceGxmTexture*>(slot + kVglTextureGxmOffset);
            const bool ok = sceGxmTextureGetWidth(gt) == d.tex_w && sceGxmTextureGetHeight(gt) == d.tex_h;
            g_raw_layout = ok ? 1 : -1;
            std::fprintf(stderr, "[gl] raw mesh draws: vitaGL's texture record %s (texture %u: %ux%u, the record says %ux%u)%s\n",
                         ok ? "as expected" : "NOT as expected", d.texture, d.tex_w, d.tex_h, sceGxmTextureGetWidth(gt),
                         sceGxmTextureGetHeight(gt), ok ? "" : ": the raw mesh draws are off");
            if (!ok) return 5;
        }
    }
    void* const base = *reinterpret_cast<void* const*>(uintptr_t(d.vbo));
    if (!base) return 3;
    // (vitaGL moves the streams by `first` too: every draw's indices start at 0) -- an indexed mesh: the stream at its first
    // distinct vertex, the indices from `first` on (vitaGL's record of the index buffer: its data pointer at 0)
    const uint16_t* indices = default_idx_ptr;
    const void* stream = static_cast<const uint8_t*>(base) + size_t(d.first) * sizeof(MeshVertex);
    if (d.ibo) {
        const uint16_t* const ibase = *reinterpret_cast<const uint16_t* const*>(uintptr_t(d.ibo));
        if (!ibase) return 3;
        indices = ibase + d.first;
        stream = base;
    }
    // the fragment program for vitaGL's current blend state (and the frame target's format)
    uint32_t blend_raw;
    std::memcpy(&blend_raw, &blend_info, sizeof blend_raw);
    SceGxmFragmentProgram* fprog = nullptr;
    for (const RawTwin::Frag& f : t.frags)
        if (f.blend == blend_raw && f.fbo_float == is_fbo_float) { fprog = f.fp; break; }
    if (!fprog) {
        if (sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher, t.fid, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4
                                                     : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SceGxmMultisampleMode(msaa_mode),
                                                     &blend_info, reinterpret_cast<const SceGxmProgram*>(t.vgxp.data()), &fprog) < 0 ||
            !fprog)
            return 2;
        t.frags.push_back(RawTwin::Frag{blend_raw, is_fbo_float, fprog});
    }
    // vitaGL's draw begins its scene for the bound framebuffer and applies a changed scissor first: so does this one
    scene_reset();
    const bool known = &te_gxm_last_vertex_program && &te_gxm_last_fragment_program && &te_gxm_last_fragment_uniforms;
    if (!known || te_gxm_last_vertex_program != t.vprog) sceGxmSetVertexProgram(gxm_context, t.vprog);
    if (!known || te_gxm_last_fragment_program != fprog) sceGxmSetFragmentProgram(gxm_context, fprog);
    if (slot) {
        std::memcpy(slot, &vgl_framecount, 4);   // its last use (vitaGL's garbage collector and copy-on-write read it)
        RawSetTexture0(reinterpret_cast<const SceGxmTexture*>(slot + kVglTextureGxmOffset));
    }
    auto put = [](uint8_t* buf, const RawTwin::U& u, const float* f, int n) {
        if (u.offset >= 0) std::memcpy(buf + u.offset, f, size_t(n) * 4);
    };
    uint8_t img[256];   // the composed fragment uniforms (kept by the packet)
    if (t.vsize) {
        if (t.vsize <= kRawVertexImage) {
            // composed whole, then set -- or not: the buffer the context holds has these contents for this program
            uint8_t vimg[kRawVertexImage];
            std::memset(vimg, 0, t.vsize);
            if (t.v_screen.offset >= 0) put(vimg, t.v_screen, v.screen->f, 2);
            if (t.v_uvscale.offset >= 0) put(vimg, t.v_uvscale, v.uv_scale->f, 2);
            if (t.v_mesh.offset >= 0) put(vimg, t.v_mesh, v.mesh->v, 20);
            if (t.v_light.offset >= 0) put(vimg, t.v_light, v.light->v, 28);
            RawSetVertexUniforms(vimg, t.vsize, t.vprog);
        } else {
            uint8_t* vb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(t.vsize));
            if (t.v_screen.offset >= 0) put(vb, t.v_screen, v.screen->f, 2);
            if (t.v_uvscale.offset >= 0) put(vb, t.v_uvscale, v.uv_scale->f, 2);
            if (t.v_mesh.offset >= 0) put(vb, t.v_mesh, v.mesh->v, 20);
            if (t.v_light.offset >= 0) put(vb, t.v_light, v.light->v, 28);
            sceGxmSetVertexDefaultUniformBuffer(gxm_context, vb);
        }
    }
    if (t.fsize) {
        // the fragment uniforms composed here; a fresh buffer only when they differ from the buffer the context holds (the
        // last raw draw's, still set: nothing set another since)
        if (t.fsize > sizeof img) return 3;
        std::memset(img, 0, t.fsize);
        if (t.f_texsize.offset >= 0) put(img, t.f_texsize, v.tex_size->f, 2);
        if (t.f_mipctl.offset >= 0) {
            if (t.f_mipctl.type == SCE_GXM_PARAMETER_TYPE_F32) {
                const float m[2] = {float(v.mip_ctl->i[0]), float(v.mip_ctl->i[1])};   // vitaGL's int -> float conversion
                put(img, t.f_mipctl, m, 2);
            } else {
                std::memcpy(img + t.f_mipctl.offset, v.mip_ctl->i, 8);
            }
        }
        if (t.f_miplodp.offset >= 0) put(img, t.f_miplodp, v.mip_lodp->f, 2);
        put(img, t.f_at, v.at, 3);
        put(img, t.f_fail, v.fail, 4);
        put(img, t.f_fba, &v.fba, 1);
        if (!known || !g_frag_last_buf || te_gxm_last_fragment_uniforms != g_frag_last_buf || g_frag_last_size != t.fsize ||
            !SameBytes(g_frag_last, img, t.fsize)) {
            uint8_t* fb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(t.fsize));
            std::memcpy(fb, img, t.fsize);
            sceGxmSetFragmentDefaultUniformBuffer(gxm_context, fb);
            std::memcpy(g_frag_last, img, t.fsize);
            g_frag_last_buf = fb;
            g_frag_last_size = t.fsize;
        }
    }
    RawSetStream0(stream);
    std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(d.vbo)) + 12, &vgl_framecount, 4);   // vbo.last_frame (its deferred free)
    if (d.ibo) std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(d.ibo)) + 12, &vgl_framecount, 4);   // (the index buffer's too)
    sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, d.count);
    // vitaGL's next draw sets its own programs, textures and streams (it does at every draw) and, with these, its uniform
    // buffers again
    dirty_shader_vert_unifs = 1;
    dirty_shader_frag_unifs = 1;
    // the packet a later draw of this state replays (the values just handed over, the draw's own transform aside)
    RecordPacket(t, fprog, blend_raw, slot, v, img, t.fsize, lit_twin);
    return -1;
}

// ---- THE RAW VERTEX DRAWS (the Vita; GlShaderLibrary::RawMesh::vertices) -------------------------------------
// A vertex draw (GlDrawExecutor::DrawRaw: the producer's RawVertex records in the vertex ring) whose lean program is the
// raw vertex path's (LeanPs2Vertex 4: u_native 2) drawn without vitaGL's glDrawArrays: the program's own two GXPs (its
// twin, registered once more with vitaGL's shader patcher), the vertex program patched for the ring's ONE 28-byte stream --
// the raw arrays' layout (GlVertexStream::Init: position 3 floats at 0, S / T 2 floats at 12, the colour 4 normalised bytes
// at 20, Q 1 float at 24: what vitaGL's draw gives the program as one stream per attribute, each at the same address), the
// fragment program patched for vitaGL's current blend state, the same uniform values (the full program's shadows, as
// SelectPs2 would send them to the lean program) in default uniform buffers, vitaGL's bound texture on unit 0, the stream
// at the draw's first vertex and sceGxmDraw over vitaGL's progressive index buffer -- the calls vitaGL's draw makes, with
// the same values: the same pictures. A draw past the index buffer (0xC000 vertices) is drawn in runs as DrawTriangles
// draws it (each from its own first vertex).
RawTwin g_vert_twins[kLeanMax];   // by lean variant (the raw vertex path's programs)
struct RawVertsCensus {
    uint64_t drawn = 0, runs = 0;   // raw vertex draws; the extra sceGxmDraw runs of the ones past the index buffer
    uint64_t declined[6] = {0, 0, 0, 0, 0, 0};   // 0 off, 1 not the raw vertex path, 2 no twin, 3 a value, 4 the ring, 5 texture
};
RawVertsCensus g_rv;

// The twin of a raw vertex path program from its GXPs (BuildRawTwin's checks for this path's attributes and uniforms);
// `share`: the path's first twin, whose vertex program this one takes when the vertex GXPs are the same bytes
bool BuildRawVertexTwin(RawTwin& t, const ProgramGxps& g, const RawTwin* share) {
    t = RawTwin();
    t.vgxp = g.vertex;
    t.fgxp = g.fragment;
    const SceGxmProgram* vp = reinterpret_cast<const SceGxmProgram*>(t.vgxp.data());
    const SceGxmProgram* fp = reinterpret_cast<const SceGxmProgram*>(t.fgxp.data());
    if (!gxm_shader_patcher || sceGxmProgramCheck(vp) < 0 || sceGxmProgramCheck(fp) < 0) return false;
    static_assert(sizeof(RawVertex) == 28 && offsetof(RawVertex, s) == 12 && offsetof(RawVertex, r) == 20 &&
                  offsetof(RawVertex, q) == 24, "the raw vertex stream is RawVertex (the raw arrays' layout)");
    struct A { const char* name; uint16_t offset; SceGxmAttributeFormat format; uint8_t count; };
    const A table[] = {{"a_position", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3}, {"a_texcoord", 12, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2},
                       {"a_color", 20, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4}, {"a_q", 24, SCE_GXM_ATTRIBUTE_FORMAT_F32, 1}};
    SceGxmVertexAttribute attrs[4];
    int n = 0;
    for (const A& a : table) {
        const SceGxmProgramParameter* p = sceGxmProgramFindParameterByName(vp, a.name);
        if (!p) continue;   // not read by this program
        if (sceGxmProgramParameterGetCategory(p) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) return false;
        attrs[n].streamIndex = 0;
        attrs[n].offset = a.offset;
        attrs[n].format = a.format;
        attrs[n].componentCount = a.count;
        attrs[n].regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(p));
        ++n;
    }
    // every parameter of both programs must be one the raw draw feeds: the attributes above, u_texture on unit 0, the
    // default uniforms below (F32; u_mip_ctl also as an integer)
    auto layout = [](const SceGxmProgram* prog, const char* const* names, RawTwin::U* const* outs, int names_n, int* attrs_seen,
                     bool* sampler) -> bool {
        const unsigned cnt = sceGxmProgramGetParameterCount(prog);
        for (unsigned i = 0; i < cnt; ++i) {
            const SceGxmProgramParameter* p = sceGxmProgramGetParameter(prog, i);
            const SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(p);
            const char* name = sceGxmProgramParameterGetName(p);
            if (cat == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) { ++*attrs_seen; continue; }
            if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
                if (!sampler || std::strcmp(name, "u_texture") != 0 || sceGxmProgramParameterGetResourceIndex(p) != 0) return false;
                *sampler = true;
                continue;
            }
            if (cat != SCE_GXM_PARAMETER_CATEGORY_UNIFORM || sceGxmProgramParameterGetContainerIndex(p) != 14) return false;
            int k = 0;
            while (k < names_n && std::strcmp(name, names[k]) != 0) ++k;
            if (k == names_n) return false;
            const SceGxmParameterType type = sceGxmProgramParameterGetType(p);
            if (type != SCE_GXM_PARAMETER_TYPE_F32 && !(std::strcmp(name, "u_mip_ctl") == 0 &&
                                                       (type == SCE_GXM_PARAMETER_TYPE_S32 || type == SCE_GXM_PARAMETER_TYPE_U32)))
                return false;
            outs[k]->offset = int(sceGxmProgramParameterGetResourceIndex(p) * 4);
            outs[k]->type = int(type);
        }
        return true;
    };
    const char* const vnames[] = {"u_screen_size", "u_uv_scale"};
    RawTwin::U* const vouts[] = {&t.v_screen, &t.v_uvscale};
    const char* const fnames[] = {"u_tex_size", "u_mip_ctl", "u_mip_lodp", "u_lean_at", "u_lean_fail", "u_lean_fba"};
    RawTwin::U* const fouts[] = {&t.f_texsize, &t.f_mipctl, &t.f_miplodp, &t.f_at, &t.f_fail, &t.f_fba};
    int vattrs = 0, fattrs = 0;
    if (!layout(vp, vnames, vouts, 2, &vattrs, nullptr) || vattrs != n) return false;
    if (!layout(fp, fnames, fouts, 6, &fattrs, &t.sampler) || fattrs != 0) return false;
    t.vsize = sceGxmProgramGetDefaultUniformBufferSize(vp);
    t.fsize = sceGxmProgramGetDefaultUniformBufferSize(fp);
    // the variants' one vertex program (BuildRawTwin's `share`): the path's first twin's, when the bytes are its
    if (share && share->ok && !share->shares_vertex && share->vid && share->vprog && share->vgxp == t.vgxp &&
        share->vsize == t.vsize && share->v_screen.offset == t.v_screen.offset && share->v_uvscale.offset == t.v_uvscale.offset) {
        if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, fp, &t.fid) < 0) return false;
        t.vid = share->vid;
        t.vprog = share->vprog;
        t.shares_vertex = true;
        t.ok = true;
        return true;
    }
    if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, vp, &t.vid) < 0) return false;
    if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, fp, &t.fid) < 0) return false;
    const SceGxmVertexStream stream = {uint16_t(sizeof(RawVertex)), uint16_t(SCE_GXM_INDEX_SOURCE_INDEX_16BIT)};
    if (sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher, t.vid, attrs, unsigned(n), &stream, 1, &t.vprog) < 0) return false;
    t.ok = true;
    return true;
}

// The raw vertex draw: RawSubmit's calls in RawSubmit's order for the ring's vertices (no packet: a vertex draw's vertices
// are its own). -1 drawn; else g_rv.declined's reason (checked before any GXM call: nothing handed over).
int RawVertexSubmit(RawTwin& t, const RawValues& v, const GlShaderLibrary::RawMesh& d) {
    if (!t.ok) return 2;
    if (t.fsize > 256 || t.vsize > kRawVertexImage) return 3;
    auto known2 = [](const RawTwin::U& u, const GlUniformCache::Shadow* s, int kind) { return u.offset < 0 || (s && s->kind == kind); };
    if (!known2(t.v_screen, v.screen, 3) || !known2(t.v_uvscale, v.uv_scale, 3) || !known2(t.f_texsize, v.tex_size, 3) ||
        !known2(t.f_mipctl, v.mip_ctl, 5) || !known2(t.f_miplodp, v.mip_lodp, 3))
        return 3;
    uint8_t* slot = nullptr;
    if (t.sampler) {
        if (!d.texture || d.texture >= kVglTextures) return 3;
        slot = texture_slots + size_t(d.texture) * kVglTextureBytes;
        if (g_raw_layout <= 0) {
            // (RawSubmit's check of vitaGL's texture record, made by whichever raw draw first samples a texture of known size)
            if (g_raw_layout < 0 || !d.tex_w || !d.tex_h) return 5;
            const SceGxmTexture* gt = reinterpret_cast<const SceGxmTexture*>(slot + kVglTextureGxmOffset);
            const bool ok = sceGxmTextureGetWidth(gt) == d.tex_w && sceGxmTextureGetHeight(gt) == d.tex_h;
            g_raw_layout = ok ? 1 : -1;
            std::fprintf(stderr, "[gl] raw vertex draws: vitaGL's texture record %s (texture %u: %ux%u, the record says %ux%u)%s\n",
                         ok ? "as expected" : "NOT as expected", d.texture, d.tex_w, d.tex_h, sceGxmTextureGetWidth(gt),
                         sceGxmTextureGetHeight(gt), ok ? "" : ": the raw draws are off");
            if (!ok) return 5;
        }
    }
    // the ring's storage (vitaGL's record of the buffer: its data pointer at 0, its size in bytes at 4) must hold the range
    if (!d.vbo) return 4;
    const uint8_t* const base = *reinterpret_cast<const uint8_t* const*>(uintptr_t(d.vbo));
    int32_t bytes;
    std::memcpy(&bytes, reinterpret_cast<const uint8_t*>(uintptr_t(d.vbo)) + 4, sizeof bytes);
    if (!base || bytes < 0 || (uint64_t(d.first) + d.count) * sizeof(RawVertex) > uint64_t(bytes)) return 4;
    // the fragment program for vitaGL's current blend state (and the frame target's format)
    uint32_t blend_raw;
    std::memcpy(&blend_raw, &blend_info, sizeof blend_raw);
    SceGxmFragmentProgram* fprog = nullptr;
    for (const RawTwin::Frag& f : t.frags)
        if (f.blend == blend_raw && f.fbo_float == is_fbo_float) { fprog = f.fp; break; }
    if (!fprog) {
        if (sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher, t.fid, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4
                                                     : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SceGxmMultisampleMode(msaa_mode),
                                                     &blend_info, reinterpret_cast<const SceGxmProgram*>(t.vgxp.data()), &fprog) < 0 ||
            !fprog)
            return 2;
        t.frags.push_back(RawTwin::Frag{blend_raw, is_fbo_float, fprog});
    }
    // vitaGL's draw begins its scene for the bound framebuffer and applies a changed scissor first: so does this one
    scene_reset();
    const bool known = &te_gxm_last_vertex_program && &te_gxm_last_fragment_program && &te_gxm_last_fragment_uniforms;
    if (!known || te_gxm_last_vertex_program != t.vprog) sceGxmSetVertexProgram(gxm_context, t.vprog);
    if (!known || te_gxm_last_fragment_program != fprog) sceGxmSetFragmentProgram(gxm_context, fprog);
    if (slot) {
        std::memcpy(slot, &vgl_framecount, 4);   // its last use (vitaGL's garbage collector and copy-on-write read it)
        RawSetTexture0(reinterpret_cast<const SceGxmTexture*>(slot + kVglTextureGxmOffset));
    }
    auto put = [](uint8_t* buf, const RawTwin::U& u, const float* f, int n) {
        if (u.offset >= 0) std::memcpy(buf + u.offset, f, size_t(n) * 4);
    };
    if (t.vsize) {
        // composed whole, then set -- or not: the buffer the context holds has these contents for this program
        uint8_t vimg[kRawVertexImage];
        std::memset(vimg, 0, t.vsize);
        if (t.v_screen.offset >= 0) put(vimg, t.v_screen, v.screen->f, 2);
        if (t.v_uvscale.offset >= 0) put(vimg, t.v_uvscale, v.uv_scale->f, 2);
        RawSetVertexUniforms(vimg, t.vsize, t.vprog);
    }
    if (t.fsize) {
        uint8_t img[256];
        std::memset(img, 0, t.fsize);
        if (t.f_texsize.offset >= 0) put(img, t.f_texsize, v.tex_size->f, 2);
        if (t.f_mipctl.offset >= 0) {
            if (t.f_mipctl.type == SCE_GXM_PARAMETER_TYPE_F32) {
                const float m[2] = {float(v.mip_ctl->i[0]), float(v.mip_ctl->i[1])};   // vitaGL's int -> float conversion
                put(img, t.f_mipctl, m, 2);
            } else {
                std::memcpy(img + t.f_mipctl.offset, v.mip_ctl->i, 8);
            }
        }
        if (t.f_miplodp.offset >= 0) put(img, t.f_miplodp, v.mip_lodp->f, 2);
        put(img, t.f_at, v.at, 3);
        put(img, t.f_fail, v.fail, 4);
        put(img, t.f_fba, &v.fba, 1);
        if (!known || !g_frag_last_buf || te_gxm_last_fragment_uniforms != g_frag_last_buf || g_frag_last_size != t.fsize ||
            !SameBytes(g_frag_last, img, t.fsize)) {
            uint8_t* fb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(t.fsize));
            std::memcpy(fb, img, t.fsize);
            sceGxmSetFragmentDefaultUniformBuffer(gxm_context, fb);
            std::memcpy(g_frag_last, img, t.fsize);
            g_frag_last_buf = fb;
            g_frag_last_size = t.fsize;
        }
    }
    std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(d.vbo)) + 12, &vgl_framecount, 4);   // the ring's last_frame (vitaGL's draw sets it)
    for (uint32_t at = 0; at < d.count; at += kRawMaxVerts) {
        const uint32_t n = std::min(d.count - at, kRawMaxVerts);
        RawSetStream0(base + (size_t(d.first) + at) * sizeof(RawVertex));
        sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, default_idx_ptr, n);
        if (at) g_rv.runs++;
    }
    // vitaGL's next draw sets its own programs, textures and streams (it does at every draw) and its uniform buffers again
    dirty_shader_vert_unifs = 1;
    dirty_shader_frag_unifs = 1;
    return -1;
}

// A packet's replay: RawSubmit's calls in RawSubmit's order with the packet's values and the draw's own u_mesh rows (the
// screen matrix, the GS origin, the UV scroll) and u_mesh_light rows (MeshUniforms' layout: three directions, three colours,
// the ambient, a vec4 each, w 0) -- what RawSubmit writes from the uniform shadows MeshUniforms just set from the same
// parameters. `ibo`: the mesh's index buffer when it is drawn indexed (GlDevice::GlMesh::list_ibo, the form RawSubmit's
// draw of it takes: the stream at the buffer's first vertex, the indices from `first` on), 0 = the buffer is the triangle
// list itself. `px`: the draw's own box share (the fill census's, when its producer handed no true box). -1 drawn; else the
// census's packet_declined reason (checked before any GXM call: nothing handed over).
int RawReplay(const RawPacket& k, uint32_t vbo, uint32_t ibo, uint32_t first, uint32_t count, const MeshDrawParams& mp, double px) {
    const RawTwin& t = *k.twin;
    if (!t.ok) return 3;
    if (count < 3 || count > kRawMaxVerts) return 7;
    uint32_t blend_raw;
    std::memcpy(&blend_raw, &blend_info, sizeof blend_raw);
    if (blend_raw != k.blend_raw || is_fbo_float != k.fbo_float) return 5;
    // the texture record as the recording draw handed it over: the same sampler state, size, levels and texels' address
    // (an upload, a re-specification, another draw's parameters: changed -- the slow path sets the texture again)
    if (k.slot && !SameBytes(k.slot + kVglTextureGxmOffset, k.tex_words, sizeof k.tex_words)) return 4;
    void* const base = *reinterpret_cast<void* const*>(uintptr_t(vbo));
    if (!base) return 7;
    const uint16_t* indices = default_idx_ptr;
    const void* stream = nullptr;
    if (ibo) {
        // an indexed mesh: the range is the index buffer's [first, first + count) over the distinct vertices (RawSubmit's
        // indexed form) -- the index buffer must hold them (vitaGL's record: its data pointer at 0, its size in bytes at 4)
        const uint16_t* const ibase = *reinterpret_cast<const uint16_t* const*>(uintptr_t(ibo));
        if (!ibase) return 7;
        int32_t bytes;
        std::memcpy(&bytes, reinterpret_cast<const uint8_t*>(uintptr_t(ibo)) + 4, sizeof bytes);
        if (bytes < 0 || (uint64_t(first) + count) * sizeof(uint16_t) > uint64_t(bytes)) return 7;
        indices = ibase + first;
        stream = base;
    } else {
        // the range drawn as a LIST of the buffer's vertices (as the earlier RawSubmit draws it: the progressive indices from
        // `first`): the buffer must hold them (vitaGL's record: its size in bytes at 4) -- a buffer holding fewer vertices
        // than the range is not replayed (the slow path draws it its own way)
        int32_t bytes;
        std::memcpy(&bytes, reinterpret_cast<const uint8_t*>(uintptr_t(vbo)) + 4, sizeof bytes);
        if (bytes < 0 || (uint64_t(first) + count) * sizeof(MeshVertex) > uint64_t(bytes)) return 7;
        stream = static_cast<const uint8_t*>(base) + size_t(first) * sizeof(MeshVertex);
    }
    // (the Vita's fill census, on a sampled frame: this draw's visibility counter, as SelectPs2 gives the draw it replays)
    FillCensusDraw(kFillLean + k.variant, px, k.path);
    scene_reset();
    const bool known = &te_gxm_last_vertex_program && &te_gxm_last_fragment_program && &te_gxm_last_fragment_uniforms;
    if (!known || te_gxm_last_vertex_program != t.vprog) sceGxmSetVertexProgram(gxm_context, t.vprog);
    if (!known || te_gxm_last_fragment_program != k.fprog) sceGxmSetFragmentProgram(gxm_context, k.fprog);
    if (k.slot) {
        std::memcpy(k.slot, &vgl_framecount, 4);   // its last use (vitaGL's garbage collector and copy-on-write read it)
        RawSetTexture0(reinterpret_cast<const SceGxmTexture*>(k.slot + kVglTextureGxmOffset));
    }
    if (t.vsize) {
        // composed whole (the larger twins' straight into the ring, as before), then set -- or not: the buffer
        // the context holds has these contents for this program. Composed in the spare image (RawVertexSpare:
        // its padding zeroed when it last held another layout; this twin's fields rewritten whole) -- the bytes a zeroed
        // image composed the same way holds -- with the field copies inlined (the light rows straight into place, their w
        // lanes 0), compared inline, and the spare made the last image when set (no copy of it: RawSetVertexSpare).
        const bool whole = t.vsize <= kRawVertexImage;
        uint8_t* vb = whole ? RawVertexSpare(t.vprog, t.vsize) : static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(t.vsize));
        if (t.v_screen.offset >= 0) std::memcpy(vb + t.v_screen.offset, k.screen, sizeof k.screen);
        if (t.v_uvscale.offset >= 0) std::memcpy(vb + t.v_uvscale.offset, k.uv_scale, sizeof k.uv_scale);
        uint8_t* const m = vb + t.v_mesh.offset;
        std::memcpy(m, mp.screen, sizeof mp.screen);
        std::memcpy(m + 64, mp.offset, sizeof mp.offset);
        std::memcpy(m + 72, mp.scroll, sizeof mp.scroll);
        if (t.v_light.offset >= 0) {
            // MeshUniforms' u_mesh_light rows: three directions, three colours, the ambient -- a vec4 each, w 0
            uint8_t* const L = vb + t.v_light.offset;
            const float zero = 0.f;
            for (int l = 0; l < 3; ++l) {
                std::memcpy(L + 16 * l, mp.light_dir[l], 12);
                std::memcpy(L + 16 * l + 12, &zero, 4);
                std::memcpy(L + 16 * (3 + l), mp.light_colour[l], 12);
                std::memcpy(L + 16 * (3 + l) + 12, &zero, 4);
            }
            std::memcpy(L + 96, mp.ambient, 12);
            std::memcpy(L + 108, &zero, 4);
        }
        if (whole) RawSetVertexSpare(t.vsize, t.vprog);
        else sceGxmSetVertexDefaultUniformBuffer(gxm_context, vb);
    }
    if (t.fsize) {
        if (!known || !g_frag_last_buf || te_gxm_last_fragment_uniforms != g_frag_last_buf || g_frag_last_size != t.fsize ||
            !SameBytes(g_frag_last, k.fimg, t.fsize)) {
            uint8_t* fb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(t.fsize));
            std::memcpy(fb, k.fimg, t.fsize);
            sceGxmSetFragmentDefaultUniformBuffer(gxm_context, fb);
            std::memcpy(g_frag_last, k.fimg, t.fsize);
            g_frag_last_buf = fb;
            g_frag_last_size = t.fsize;
        }
    }
    RawSetStream0(stream);
    std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(vbo)) + 12, &vgl_framecount, 4);   // vbo.last_frame (its deferred free)
    if (ibo) std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(ibo)) + 12, &vgl_framecount, 4);   // (the index buffer's too)
    sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, count);
    dirty_shader_vert_unifs = 1;
    dirty_shader_frag_unifs = 1;
    return -1;
}

// A run of `n` replays of packet `k` (the mesh path's, unlit) over one mesh range, their transforms in
// `inst`, as ONE instanced draw: RawReplay's calls in its order with the instanced twin `it` of the packet's lean variant --
// its vertex program (the mesh path's instruction sequence, the matrix rows read from stream 1, one 64-byte element per
// instance: each instance's u_mesh[0..3] as RawReplay writes them, u_mesh[4] the uniform u_mesh_shift), its fragment program for the blend state (patched against its vertex
// program; the same fragment GXP as the packet's: the packet's composed fragment image unchanged), the packet's texture
// and vertex constants -- then sceGxmDrawInstanced over the range (count indices per instance, the index pointer back at
// the range's start for each). The instances draw in their order, each its triangles as its own replay would. -1 drawn;
// else a packet_declined reason (checked before any GXM call).
int RawReplayInstances(const RawPacket& k, RawTwin& it, uint32_t vbo, uint32_t ibo, uint32_t first, uint32_t count,
                       const MeshInstance* inst, int n) {
    const RawTwin& t = *k.twin;
    if (!t.ok || !it.ok || !it.instanced || !it.frag_as_mesh || it.v_shift.offset < 0) return 3;
    if (n < 2 || n > kMaxInstances || count < 3 || count > kRawMaxVerts) return 7;
    for (int i = 1; i < n; ++i)   // (one shift for the run: u_mesh_shift)
        if (std::memcmp(inst[i].offset, inst[0].offset, 8) != 0 || std::memcmp(inst[i].scroll, inst[0].scroll, 8) != 0) return 7;
    uint32_t blend_raw;
    std::memcpy(&blend_raw, &blend_info, sizeof blend_raw);
    if (blend_raw != k.blend_raw || is_fbo_float != k.fbo_float) return 5;
    if (k.slot && std::memcmp(k.slot + kVglTextureGxmOffset, k.tex_words, sizeof k.tex_words) != 0) return 4;
    void* const base = *reinterpret_cast<void* const*>(uintptr_t(vbo));
    if (!base) return 7;
    const uint16_t* indices = default_idx_ptr;
    const void* stream = nullptr;
    if (ibo) {
        const uint16_t* const ibase = *reinterpret_cast<const uint16_t* const*>(uintptr_t(ibo));
        if (!ibase) return 7;
        int32_t bytes;
        std::memcpy(&bytes, reinterpret_cast<const uint8_t*>(uintptr_t(ibo)) + 4, sizeof bytes);
        if (bytes < 0 || (uint64_t(first) + count) * sizeof(uint16_t) > uint64_t(bytes)) return 7;
        indices = ibase + first;
        stream = base;
    } else {
        int32_t bytes;
        std::memcpy(&bytes, reinterpret_cast<const uint8_t*>(uintptr_t(vbo)) + 4, sizeof bytes);
        if (bytes < 0 || (uint64_t(first) + count) * sizeof(MeshVertex) > uint64_t(bytes)) return 7;
        stream = static_cast<const uint8_t*>(base) + size_t(first) * sizeof(MeshVertex);
    }
    // the instanced twin's fragment program for the packet's blend state (and the target's format), as RawSubmit makes them
    SceGxmFragmentProgram* fprog = nullptr;
    for (const RawTwin::Frag& f : it.frags)
        if (f.blend == k.blend_raw && f.fbo_float == k.fbo_float) { fprog = f.fp; break; }
    if (!fprog) {
        if (sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher, it.fid, k.fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4
                                                     : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SceGxmMultisampleMode(msaa_mode),
                                                     &blend_info, reinterpret_cast<const SceGxmProgram*>(it.vgxp.data()), &fprog) < 0 ||
            !fprog)
            return 2;
        it.frags.push_back(RawTwin::Frag{k.blend_raw, k.fbo_float, fprog});
    }
    scene_reset();
    const bool known = &te_gxm_last_vertex_program && &te_gxm_last_fragment_program && &te_gxm_last_fragment_uniforms;
    if (!known || te_gxm_last_vertex_program != it.vprog) sceGxmSetVertexProgram(gxm_context, it.vprog);
    if (!known || te_gxm_last_fragment_program != fprog) sceGxmSetFragmentProgram(gxm_context, fprog);
    if (k.slot) {
        std::memcpy(k.slot, &vgl_framecount, 4);   // its last use (vitaGL's garbage collector and copy-on-write read it)
        RawSetTexture0(reinterpret_cast<const SceGxmTexture*>(k.slot + kVglTextureGxmOffset));
    }
    if (it.vsize) {
        uint8_t* vb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(it.vsize));
        if (it.v_screen.offset >= 0) std::memcpy(vb + it.v_screen.offset, k.screen, sizeof k.screen);
        if (it.v_uvscale.offset >= 0) std::memcpy(vb + it.v_uvscale.offset, k.uv_scale, sizeof k.uv_scale);
        // u_mesh_shift: the run's shift (RawReplay's u_mesh[4]: the GS origin, then the UV scroll -- every instance's alike)
        std::memcpy(vb + it.v_shift.offset, inst[0].offset, 8);
        std::memcpy(vb + it.v_shift.offset + 8, inst[0].scroll, 8);
        sceGxmSetVertexDefaultUniformBuffer(gxm_context, vb);
    }
    if (it.fsize) {
        // (the packet's image, laid out alike: frag_as_mesh)
        if (!known || !g_frag_last_buf || te_gxm_last_fragment_uniforms != g_frag_last_buf || g_frag_last_size != it.fsize ||
            std::memcmp(g_frag_last, k.fimg, it.fsize) != 0) {
            uint8_t* fb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(it.fsize));
            std::memcpy(fb, k.fimg, it.fsize);
            sceGxmSetFragmentDefaultUniformBuffer(gxm_context, fb);
            std::memcpy(g_frag_last, k.fimg, it.fsize);
            g_frag_last_buf = fb;
            g_frag_last_size = it.fsize;
        }
    }
    // the instances' rows (RawReplay's u_mesh[0..3] image per instance: the matrix), in the uniform ring as every replay's
    // vertex uniforms are (GPU-mapped; the ring outlives the frame's GPU work), 16-aligned
    uint8_t* const raw_rows = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(uint32_t(n) * kInstanceRowBytes + 16));
    uint8_t* const rows = reinterpret_cast<uint8_t*>((uintptr_t(raw_rows) + 15) & ~uintptr_t(15));
    for (int i = 0; i < n; ++i) std::memcpy(rows + size_t(i) * kInstanceRowBytes, inst[i].screen, 64);
    RawSetStream0(stream);
    std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(vbo)) + 12, &vgl_framecount, 4);   // vbo.last_frame (its deferred free)
    if (ibo) std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(ibo)) + 12, &vgl_framecount, 4);
    if (g_instancing_split) {
        // (diagnosis, TE_GFX_VITA_INSTANCING=2: the instanced twin's programs, one plain draw per instance -- the draw count
        // of the replays one by one, the instanced draws' programs and rows)
        for (int i = 0; i < n; ++i) {
            sceGxmSetVertexStream(gxm_context, 1, rows + size_t(i) * kInstanceRowBytes);
            sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, count);
        }
    } else {
        sceGxmSetVertexStream(gxm_context, 1, rows);
        sceGxmDrawInstanced(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, uint32_t(n) * count, count);
    }
    dirty_shader_vert_unifs = 1;
    dirty_shader_frag_unifs = 1;
    return -1;
}

// ---- the RAW SKIN DRAWS and the SKIN PACKETS (the Vita; GlShaderLibrary::RawSkin, DrawSkinPacket) -------------------------
// A GPU-skinning program's twin (GlShaderSources::Ps2SkinVertex over a lean variant): RawTwin's fragment side and layouts,
// the vertex program patched for the skin mesh's one 72-byte SkinVertex stream (vitaGL's formats for the attribute arrays
// CreateSkinMesh sets: GL_FLOAT -> F32, the joints' GL_UNSIGNED_BYTE unnormalised -> U8), and the vertex uniforms the skin
// program adds: u_uv_mode, the palette u_bones and the displacements u_disp.
struct SkinTwin {
    RawTwin t;
    RawTwin::U v_uvmode, v_bones, v_disp;
    int bones_rows = 0, disp_rows = 0;   // the arrays' vec4 rows in the default buffer
};
SkinTwin g_skin_twins[kLeanMax];

bool BuildSkinTwin(SkinTwin& st, const ProgramGxps& g) {
    st = SkinTwin();
    RawTwin& t = st.t;
    t.vgxp = g.vertex;
    t.fgxp = g.fragment;
    const SceGxmProgram* vp = reinterpret_cast<const SceGxmProgram*>(t.vgxp.data());
    const SceGxmProgram* fp = reinterpret_cast<const SceGxmProgram*>(t.fgxp.data());
    if (!gxm_shader_patcher || sceGxmProgramCheck(vp) < 0 || sceGxmProgramCheck(fp) < 0) return false;
    struct A { const char* name; uint16_t offset; SceGxmAttributeFormat format; uint8_t count; };
    const A attrs_table[] = {
        {"a_position", uint16_t(offsetof(SkinVertex, x)), SCE_GXM_ATTRIBUTE_FORMAT_F32, 3},
        {"a_texcoord", uint16_t(offsetof(SkinVertex, s)), SCE_GXM_ATTRIBUTE_FORMAT_F32, 3},
        {"a_color", uint16_t(offsetof(SkinVertex, r)), SCE_GXM_ATTRIBUTE_FORMAT_F32, 4},
        {"a_normal", uint16_t(offsetof(SkinVertex, nx)), SCE_GXM_ATTRIBUTE_FORMAT_F32, 3},
        {"a_weights", uint16_t(offsetof(SkinVertex, w0)), SCE_GXM_ATTRIBUTE_FORMAT_F32, 3},
        {"a_joints", uint16_t(offsetof(SkinVertex, j0)), SCE_GXM_ATTRIBUTE_FORMAT_U8, 4},
        {"a_index", uint16_t(offsetof(SkinVertex, index)), SCE_GXM_ATTRIBUTE_FORMAT_F32, 1},
    };
    static_assert(sizeof(SkinVertex) == 72, "the raw skin stream is SkinVertex");
    SceGxmVertexAttribute attrs[7];
    int n = 0;
    for (const A& a : attrs_table) {
        const SceGxmProgramParameter* prm = sceGxmProgramFindParameterByName(vp, a.name);
        if (!prm) continue;   // not read by this program
        if (sceGxmProgramParameterGetCategory(prm) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) return false;
        attrs[n].streamIndex = 0;
        attrs[n].offset = a.offset;
        attrs[n].format = a.format;
        attrs[n].componentCount = a.count;
        attrs[n].regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(prm));
        ++n;
    }
    // every parameter of both programs must be one the raw draw feeds (an F32 uniform of the default buffer; u_uv_mode and
    // u_mip_ctl may be integers); `rows`: an array's vec4 rows
    auto layout = [](const SceGxmProgram* prog, const char* const* names, RawTwin::U* const* outs, int* const* rows, int names_n,
                     int* attrs_seen, bool* sampler) -> bool {
        const unsigned cnt = sceGxmProgramGetParameterCount(prog);
        for (unsigned i = 0; i < cnt; ++i) {
            const SceGxmProgramParameter* prm = sceGxmProgramGetParameter(prog, i);
            const SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(prm);
            const char* name = sceGxmProgramParameterGetName(prm);
            if (cat == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) { ++*attrs_seen; continue; }
            if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
                if (!sampler || std::strcmp(name, "u_texture") != 0 || sceGxmProgramParameterGetResourceIndex(prm) != 0) return false;
                *sampler = true;
                continue;
            }
            if (cat != SCE_GXM_PARAMETER_CATEGORY_UNIFORM || sceGxmProgramParameterGetContainerIndex(prm) != 14) return false;
            int k = 0;
            while (k < names_n && std::strcmp(name, names[k]) != 0) ++k;
            if (k == names_n) return false;
            const SceGxmParameterType type = sceGxmProgramParameterGetType(prm);
            const bool int_ok = std::strcmp(name, "u_mip_ctl") == 0 || std::strcmp(name, "u_uv_mode") == 0;
            if (type != SCE_GXM_PARAMETER_TYPE_F32 && !(int_ok && (type == SCE_GXM_PARAMETER_TYPE_S32 || type == SCE_GXM_PARAMETER_TYPE_U32)))
                return false;
            outs[k]->offset = int(sceGxmProgramParameterGetResourceIndex(prm) * 4);
            outs[k]->type = int(type);
            if (rows && rows[k]) *rows[k] = int(std::max(1u, sceGxmProgramParameterGetArraySize(prm)));
        }
        return true;
    };
    const char* const vnames[] = {"u_screen_size", "u_uv_scale", "u_uv_mode", "u_mesh", "u_mesh_light", "u_bones", "u_disp"};
    RawTwin::U* const vouts[] = {&t.v_screen, &t.v_uvscale, &st.v_uvmode, &t.v_mesh, &t.v_light, &st.v_bones, &st.v_disp};
    int* const vrows[] = {nullptr, nullptr, nullptr, nullptr, nullptr, &st.bones_rows, &st.disp_rows};
    const char* const fnames[] = {"u_tex_size", "u_mip_ctl", "u_mip_lodp", "u_lean_at", "u_lean_fail", "u_lean_fba"};
    RawTwin::U* const fouts[] = {&t.f_texsize, &t.f_mipctl, &t.f_miplodp, &t.f_at, &t.f_fail, &t.f_fba};
    int vattrs = 0, fattrs = 0;
    if (!layout(vp, vnames, vouts, vrows, 7, &vattrs, nullptr) || vattrs != n) return false;
    if (!layout(fp, fnames, fouts, nullptr, 6, &fattrs, &t.sampler) || fattrs != 0) return false;
    // the skin program reads its transform, its lights and its palette: without them it is not this program
    if (t.v_mesh.offset < 0 || t.v_light.offset < 0 || st.v_bones.offset < 0 || st.bones_rows < 3) return false;
    t.vsize = sceGxmProgramGetDefaultUniformBufferSize(vp);
    t.fsize = sceGxmProgramGetDefaultUniformBufferSize(fp);
    if (t.fsize > 128) return false;   // (the packet's fragment image)
    if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, vp, &t.vid) < 0) return false;
    if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, fp, &t.fid) < 0) return false;
    const SceGxmVertexStream stream = {uint16_t(sizeof(SkinVertex)), uint16_t(SCE_GXM_INDEX_SOURCE_INDEX_16BIT)};
    if (sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher, t.vid, attrs, unsigned(n), &stream, 1, &t.vprog) < 0) return false;
    t.ok = true;
    return true;
}

// A skin packet: RawPacket's values for a raw skin draw (its twin, fragment program, texture record, fragment image, the
// vertex constants -- the screen size, the UV prescale, u_uv_mode), replayed with the draw's own transform, lights, palette
// and displacements. A ring of slots (fewer than the meshes': a frame has tens of skin draws), content-addressed as theirs.
struct SkinPacket {
    uint32_t id = 0;
    uint32_t epoch = 0;
    SkinTwin* twin = nullptr;
    SceGxmFragmentProgram* fprog = nullptr;
    uint32_t blend_raw = 0;
    uint8_t fbo_float = 0;
    uint8_t* slot = nullptr;
    uint32_t tex_words[4] = {0, 0, 0, 0};
    float screen[2] = {0.f, 0.f};
    float uv_scale[2] = {0.f, 0.f};
    uint32_t uv_mode = 0;          // its bits as written (float or integer by the twin's type)
    uint32_t fsize = 0;
    uint8_t fimg[128];
    void* program = nullptr;       // GlShaderLibrary::SkinState::Program* (the census)
    int variant = 0;
};
constexpr uint32_t kSkinPackets = 256;
constexpr uint32_t kSkinPacketBits = 8;
static_assert((1u << kSkinPacketBits) == kSkinPackets, "skin packet slots");
SkinPacket g_skin_packets[kSkinPackets];
uint32_t g_skin_packet_next = 0, g_skin_packet_gen = 0, g_skin_packet_last = 0;
constexpr uint32_t kSkinPacketIndex = 1024;
uint16_t g_skin_packet_index[kSkinPacketIndex];
uint32_t SkinPacketHash(const SkinPacket& k) {
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t x) { h = (h ^ x) * 16777619u; };
    mix(uint32_t(uintptr_t(k.twin)));
    mix(uint32_t(uintptr_t(k.fprog)));
    mix(k.blend_raw);
    mix(k.fbo_float);
    mix(uint32_t(uintptr_t(k.slot)));
    for (uint32_t w : k.tex_words) mix(w);
    uint32_t f[4];
    std::memcpy(f, k.screen, 8);
    std::memcpy(f + 2, k.uv_scale, 8);
    for (uint32_t w : f) mix(w);
    mix(k.uv_mode);
    mix(k.fsize);
    for (uint32_t i = 0; i + 4 <= k.fsize; i += 4) {
        uint32_t w;
        std::memcpy(&w, k.fimg + i, 4);
        mix(w);
    }
    return (h ^ (h >> 15)) & (kSkinPacketIndex - 1);
}
bool SameSkinPacket(const SkinPacket& a, const SkinPacket& b) {
    return a.twin == b.twin && a.fprog == b.fprog && a.blend_raw == b.blend_raw && a.fbo_float == b.fbo_float && a.slot == b.slot &&
           std::memcmp(a.tex_words, b.tex_words, sizeof a.tex_words) == 0 && std::memcmp(a.screen, b.screen, sizeof a.screen) == 0 &&
           std::memcmp(a.uv_scale, b.uv_scale, sizeof a.uv_scale) == 0 && a.uv_mode == b.uv_mode && a.fsize == b.fsize &&
           std::memcmp(a.fimg, b.fimg, a.fsize) == 0;
}

// the u_uv_mode value as the twin's parameter holds it (vitaGL's glUniform1i into an F32 parameter: the int converted)
uint32_t UvModeBits(const RawTwin::U& u, int value) {
    uint32_t bits;
    if (u.type == SCE_GXM_PARAMETER_TYPE_F32) {
        const float f = float(value);
        std::memcpy(&bits, &f, 4);
    } else {
        std::memcpy(&bits, &value, 4);
    }
    return bits;
}

// The vertex uniforms both the raw skin draw and its replay write: the constants, the transform (u_mesh: the screen
// matrix, the GS origin, the UV scroll -- MeshUniforms' rows), the lights (u_mesh_light: three directions, three colours,
// the ambient), the palette's `joints` x 3 rows and the displacements (a plain skin: u_disp[0] = 0, the row its vertices
// read)
void PutSkinVertexUniforms(const SkinTwin& st, uint8_t* vb, const float* screen, const float* uv_scale, uint32_t uv_mode,
                           const float* mesh20, const float* light28, const float* bones, int joints, const float* disp,
                           int disp_count) {
    const RawTwin& t = st.t;
    if (t.v_screen.offset >= 0) std::memcpy(vb + t.v_screen.offset, screen, 8);
    if (t.v_uvscale.offset >= 0) std::memcpy(vb + t.v_uvscale.offset, uv_scale, 8);
    if (st.v_uvmode.offset >= 0) std::memcpy(vb + st.v_uvmode.offset, &uv_mode, 4);
    std::memcpy(vb + t.v_mesh.offset, mesh20, 20 * sizeof(float));
    std::memcpy(vb + t.v_light.offset, light28, 28 * sizeof(float));
    std::memcpy(vb + st.v_bones.offset, bones, size_t(joints) * 12 * sizeof(float));
    if (st.v_disp.offset >= 0) {
        if (disp_count > 0) {
            std::memcpy(vb + st.v_disp.offset, disp, size_t(disp_count) * 4 * sizeof(float));
        } else {
            std::memset(vb + st.v_disp.offset, 0, 4 * sizeof(float));
        }
    }
}

// -1 drawn; else the reason it was not (the census): 2 no twin / fragment program, 3 a value it reads not known / out of
// range, 5 the texture record's layout not as expected. A drawn raw skin draw records its packet.
int RawSkinSubmit(SkinTwin& st, const RawValues& v, const GlUniformCache::Shadow* uv_mode, const GlShaderLibrary::RawSkin& d,
                  int variant) {
    RawTwin& t = st.t;
    if (!t.ok) return 2;
    if (t.fsize > 128 || d.count < 3 || !d.bones || d.joints <= 0 || d.joints * 3 > st.bones_rows || d.disp_count < 0 ||
        (d.disp_count > 0 && (!d.disp || st.v_disp.offset < 0 || d.disp_count > st.disp_rows)))
        return 3;
    auto known2 = [](const RawTwin::U& u, const GlUniformCache::Shadow* s, int kind) { return u.offset < 0 || (s && s->kind == kind); };
    if (!known2(t.v_screen, v.screen, 3) || !known2(t.v_uvscale, v.uv_scale, 3) || !known2(t.f_texsize, v.tex_size, 3) ||
        !known2(t.f_mipctl, v.mip_ctl, 5) || !known2(t.f_miplodp, v.mip_lodp, 3) || !known2(st.v_uvmode, uv_mode, 1))
        return 3;
    if (!v.mesh || v.mesh->count != 5 || !v.light || v.light->count != 7) return 3;
    uint8_t* slot = nullptr;
    if (t.sampler) {
        if (!d.texture || d.texture >= kVglTextures) return 3;
        slot = texture_slots + size_t(d.texture) * kVglTextureBytes;
        if (g_raw_layout <= 0) {
            if (g_raw_layout < 0 || !d.tex_w || !d.tex_h) return 5;
            const SceGxmTexture* gt = reinterpret_cast<const SceGxmTexture*>(slot + kVglTextureGxmOffset);
            const bool ok = sceGxmTextureGetWidth(gt) == d.tex_w && sceGxmTextureGetHeight(gt) == d.tex_h;
            g_raw_layout = ok ? 1 : -1;
            std::fprintf(stderr, "[gl] raw skin draws: vitaGL's texture record %s (texture %u: %ux%u, the record says %ux%u)%s\n",
                         ok ? "as expected" : "NOT as expected", d.texture, d.tex_w, d.tex_h, sceGxmTextureGetWidth(gt),
                         sceGxmTextureGetHeight(gt), ok ? "" : ": the raw draws are off");
            if (!ok) return 5;
        }
    }
    void* const base = *reinterpret_cast<void* const*>(uintptr_t(d.vbo));
    const uint16_t* const indices = *reinterpret_cast<const uint16_t* const*>(uintptr_t(d.ibo));
    if (!base || !indices) return 3;
    int32_t ibytes;
    std::memcpy(&ibytes, reinterpret_cast<const uint8_t*>(uintptr_t(d.ibo)) + 4, sizeof ibytes);
    if (ibytes < 0 || uint64_t(d.count) * sizeof(uint16_t) > uint64_t(ibytes)) return 3;
    uint32_t blend_raw;
    std::memcpy(&blend_raw, &blend_info, sizeof blend_raw);
    SceGxmFragmentProgram* fprog = nullptr;
    for (const RawTwin::Frag& f : t.frags)
        if (f.blend == blend_raw && f.fbo_float == is_fbo_float) { fprog = f.fp; break; }
    if (!fprog) {
        if (sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher, t.fid, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4
                                                     : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SceGxmMultisampleMode(msaa_mode),
                                                     &blend_info, reinterpret_cast<const SceGxmProgram*>(t.vgxp.data()), &fprog) < 0 ||
            !fprog)
            return 2;
        t.frags.push_back(RawTwin::Frag{blend_raw, is_fbo_float, fprog});
    }
    // the fragment uniforms composed as RawSubmit composes them
    uint8_t img[128];
    auto put = [](uint8_t* buf, const RawTwin::U& u, const float* f, int n) {
        if (u.offset >= 0) std::memcpy(buf + u.offset, f, size_t(n) * 4);
    };
    if (t.fsize) {
        std::memset(img, 0, t.fsize);
        if (t.f_texsize.offset >= 0) put(img, t.f_texsize, v.tex_size->f, 2);
        if (t.f_mipctl.offset >= 0) {
            if (t.f_mipctl.type == SCE_GXM_PARAMETER_TYPE_F32) {
                const float m[2] = {float(v.mip_ctl->i[0]), float(v.mip_ctl->i[1])};
                put(img, t.f_mipctl, m, 2);
            } else {
                std::memcpy(img + t.f_mipctl.offset, v.mip_ctl->i, 8);
            }
        }
        if (t.f_miplodp.offset >= 0) put(img, t.f_miplodp, v.mip_lodp->f, 2);
        put(img, t.f_at, v.at, 3);
        put(img, t.f_fail, v.fail, 4);
        put(img, t.f_fba, &v.fba, 1);
    }
    const float screen[2] = {t.v_screen.offset >= 0 ? v.screen->f[0] : 0.f, t.v_screen.offset >= 0 ? v.screen->f[1] : 0.f};
    const float uvs[2] = {t.v_uvscale.offset >= 0 ? v.uv_scale->f[0] : 0.f, t.v_uvscale.offset >= 0 ? v.uv_scale->f[1] : 0.f};
    const uint32_t uvm = st.v_uvmode.offset >= 0 ? UvModeBits(st.v_uvmode, uv_mode->i[0]) : 0u;
    // (the fill census, on a sampled frame: this draw's visibility counter, as SelectSkin gives a vitaGL skin draw)
    FillCensusDraw(kFillSkin + variant, 512.0 * 512.0, 5);
    scene_reset();
    const bool known = &te_gxm_last_vertex_program && &te_gxm_last_fragment_program && &te_gxm_last_fragment_uniforms;
    if (!known || te_gxm_last_vertex_program != t.vprog) sceGxmSetVertexProgram(gxm_context, t.vprog);
    if (!known || te_gxm_last_fragment_program != fprog) sceGxmSetFragmentProgram(gxm_context, fprog);
    if (slot) {
        std::memcpy(slot, &vgl_framecount, 4);
        RawSetTexture0(reinterpret_cast<const SceGxmTexture*>(slot + kVglTextureGxmOffset));
    }
    if (t.vsize) {
        uint8_t* vb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(t.vsize));
        PutSkinVertexUniforms(st, vb, screen, uvs, uvm, v.mesh->v, v.light->v, d.bones, d.joints, d.disp, d.disp_count);
        sceGxmSetVertexDefaultUniformBuffer(gxm_context, vb);
    }
    if (t.fsize && (!known || !g_frag_last_buf || te_gxm_last_fragment_uniforms != g_frag_last_buf || g_frag_last_size != t.fsize ||
                    std::memcmp(g_frag_last, img, t.fsize) != 0)) {
        uint8_t* fb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(t.fsize));
        std::memcpy(fb, img, t.fsize);
        sceGxmSetFragmentDefaultUniformBuffer(gxm_context, fb);
        std::memcpy(g_frag_last, img, t.fsize);
        g_frag_last_buf = fb;
        g_frag_last_size = t.fsize;
    }
    RawSetStream0(base);
    std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(d.vbo)) + 12, &vgl_framecount, 4);   // the buffers' last use (vitaGL's
    std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(d.ibo)) + 12, &vgl_framecount, 4);   // deferred frees read them)
    sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, d.count);
    dirty_shader_vert_unifs = 1;
    dirty_shader_frag_unifs = 1;
    // the packet a later skin draw of this state replays
    if (g_packets_on && g_packets_mode != 0) {
        SkinPacket n;
        n.epoch = g_packet_epoch;
        n.twin = &st;
        n.fprog = fprog;
        n.blend_raw = blend_raw;
        n.fbo_float = is_fbo_float;
        n.slot = slot;
        if (slot) std::memcpy(n.tex_words, slot + kVglTextureGxmOffset, sizeof n.tex_words);
        std::memcpy(n.screen, screen, sizeof n.screen);
        std::memcpy(n.uv_scale, uvs, sizeof n.uv_scale);
        n.uv_mode = uvm;
        n.fsize = t.fsize;
        if (t.fsize) std::memcpy(n.fimg, img, t.fsize);
        const uint32_t h = SkinPacketHash(n);
        const uint16_t at = g_skin_packet_index[h];
        if (at && g_skin_packets[at - 1].id && g_skin_packets[at - 1].epoch == g_packet_epoch && SameSkinPacket(g_skin_packets[at - 1], n)) {
            g_skin_packet_last = g_skin_packets[at - 1].id;
        } else {
            const uint32_t slot_at = g_skin_packet_next;
            g_skin_packet_next = (g_skin_packet_next + 1) & (kSkinPackets - 1);
            if (++g_skin_packet_gen >= (1u << (32 - kSkinPacketBits))) g_skin_packet_gen = 1;
            SkinPacket& k = g_skin_packets[slot_at];
            k = n;
            k.id = slot_at | (g_skin_packet_gen << kSkinPacketBits);
            g_skin_packet_index[h] = uint16_t(slot_at + 1);
            g_skin_packet_last = k.id;
        }
    }
    return -1;
}

// A skin packet's replay: RawSkinSubmit's calls with the packet's values and the draw's own transform, lights, palette and
// displacements. -1 drawn; else a reason (checked before any GXM call): 3 stale, 4 the texture record changed, 5 the blend
// state / target format, 6 the screen size, 7 the buffers / a count out of range.
int SkinReplay(const SkinPacket& k, uint32_t vbo, uint32_t ibo, uint32_t count, const MeshDrawParams& mp, const float* bones,
               int joints, const float* disp, int disp_count, int frame_w, int frame_h) {
    if (!k.twin || k.epoch != g_packet_epoch) return 3;
    const SkinTwin& st = *k.twin;
    const RawTwin& t = st.t;
    if (!t.ok) return 3;
    if (t.v_screen.offset >= 0 && (k.screen[0] != float(frame_w) || k.screen[1] != float(frame_h))) return 6;
    if (count < 3 || !bones || joints <= 0 || joints * 3 > st.bones_rows || disp_count < 0 ||
        (disp_count > 0 && (!disp || st.v_disp.offset < 0 || disp_count > st.disp_rows)))
        return 7;
    uint32_t blend_raw;
    std::memcpy(&blend_raw, &blend_info, sizeof blend_raw);
    if (blend_raw != k.blend_raw || is_fbo_float != k.fbo_float) return 5;
    if (k.slot && std::memcmp(k.slot + kVglTextureGxmOffset, k.tex_words, sizeof k.tex_words) != 0) return 4;
    void* const base = *reinterpret_cast<void* const*>(uintptr_t(vbo));
    const uint16_t* const indices = *reinterpret_cast<const uint16_t* const*>(uintptr_t(ibo));
    if (!base || !indices) return 7;
    int32_t ibytes;
    std::memcpy(&ibytes, reinterpret_cast<const uint8_t*>(uintptr_t(ibo)) + 4, sizeof ibytes);
    if (ibytes < 0 || uint64_t(count) * sizeof(uint16_t) > uint64_t(ibytes)) return 7;
    // (the fill census: the replayed draw's counter, as SelectSkin gives the draw it replays)
    FillCensusDraw(kFillSkin + k.variant, 512.0 * 512.0, 5);
    scene_reset();
    const bool known = &te_gxm_last_vertex_program && &te_gxm_last_fragment_program && &te_gxm_last_fragment_uniforms;
    if (!known || te_gxm_last_vertex_program != t.vprog) sceGxmSetVertexProgram(gxm_context, t.vprog);
    if (!known || te_gxm_last_fragment_program != k.fprog) sceGxmSetFragmentProgram(gxm_context, k.fprog);
    if (k.slot) {
        std::memcpy(k.slot, &vgl_framecount, 4);
        RawSetTexture0(reinterpret_cast<const SceGxmTexture*>(k.slot + kVglTextureGxmOffset));
    }
    if (t.vsize) {
        // MeshUniforms' rows from the draw's parameters (u_mesh: the screen matrix, the GS origin, the UV scroll; u_mesh_light:
        // three directions, three colours, the ambient, a vec4 each with w 0)
        float mesh[20];
        std::memcpy(mesh, mp.screen, sizeof mp.screen);
        mesh[16] = mp.offset[0];
        mesh[17] = mp.offset[1];
        mesh[18] = mp.scroll[0];
        mesh[19] = mp.scroll[1];
        float light[28] = {};
        for (int l = 0; l < 3; ++l)
            for (int c = 0; c < 3; ++c) {
                light[l * 4 + c] = mp.light_dir[l][c];
                light[(3 + l) * 4 + c] = mp.light_colour[l][c];
            }
        for (int c = 0; c < 3; ++c) light[24 + c] = mp.ambient[c];
        uint8_t* vb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(t.vsize));
        PutSkinVertexUniforms(st, vb, k.screen, k.uv_scale, k.uv_mode, mesh, light, bones, joints, disp, disp_count);
        sceGxmSetVertexDefaultUniformBuffer(gxm_context, vb);
    }
    if (t.fsize && (!known || !g_frag_last_buf || te_gxm_last_fragment_uniforms != g_frag_last_buf || g_frag_last_size != t.fsize ||
                    std::memcmp(g_frag_last, k.fimg, t.fsize) != 0)) {
        uint8_t* fb = static_cast<uint8_t*>(vglReserveUniformCircularPoolBuffer(t.fsize));
        std::memcpy(fb, k.fimg, t.fsize);
        sceGxmSetFragmentDefaultUniformBuffer(gxm_context, fb);
        std::memcpy(g_frag_last, k.fimg, t.fsize);
        g_frag_last_buf = fb;
        g_frag_last_size = t.fsize;
    }
    RawSetStream0(base);
    std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(vbo)) + 12, &vgl_framecount, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(ibo)) + 12, &vgl_framecount, 4);
    sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, count);
    dirty_shader_vert_unifs = 1;
    dirty_shader_frag_unifs = 1;
    return -1;
}
}  // namespace

struct GlShaderLibrary::LeanState {
    struct Program {
        uint32_t id = 0;
        int mirror_loc[kMirrors];
        GlUniformCache::Shadow mirror_sent[kMirrors];
        int array_loc[kArrayMirrors];
        int array_count[kArrayMirrors];
        float array_sent[kArrayMirrors][GlUniformCache::kArrayFloats];
        int at_loc = -1, fail_loc = -1, fba_loc = -1;
        float at_sent[3] = {-1.f, -1.f, -1.f}, fail_sent[4] = {-1.f, -1.f, -1.f, -1.f}, fba_sent = -1.f;
        uint64_t draws = 0;
        double pixels = 0;
        uint64_t draws_opaque = 0;   // with no blend (an opaque object for the GPU unless the variant discards)
    };
    Program programs[kLeanMax];
    // the lean VERTEX paths (GlShaderSources::LeanPs2Vertex, LEAN_VS_PATH 1..kVsPaths: mesh, lit mesh, vertices, raw):
    // every fragment variant over every path's vertex program, id 0 where not built (the draw then takes programs[i],
    // over the generic PS2 vertex program); TE_GFX_VITA_LEAN_VS=0: none built
    static constexpr int kVsPaths = 4;
    Program paths[kVsPaths][kLeanMax];
    bool paths_built = false;
    const GlUniformCache::Shadow* vs_native = nullptr;
    const GlUniformCache::Shadow* vs_lit = nullptr;
    const GlUniformCache::Shadow* vs_uv_mode = nullptr;
    uint64_t path_draws[kVsPaths + 1] = {};   // the census: draws by vertex path (0 = the generic vertex program)
    uint64_t path_misses = 0;                 // draws whose path has no program (or whose uniforms are not a path's)
    int index[2][4][2][2][3];   // [tex][tfx][tcc][mip][atest] -> variant, -1 none
    int nocolor = -1;
    // the full program's shadows (Track): the fast constants, the key fields, the mirrors
    uint32_t tracked_gen = 0;
    bool tracked = false;
    const GlUniformCache::Shadow* fast[16] = {};
    int fast_value[16] = {};
    int fast_n = 0;
    const GlUniformCache::Shadow* key[kKeyFields] = {};
    const GlUniformCache::Shadow* mirror[kMirrors] = {};
    const GlUniformCache::ArrayShadow* array[kArrayMirrors] = {};
    // the census
    uint64_t fallback[kFallbacks] = {};
    double fallback_pixels[kFallbacks] = {};
    uint64_t fast_draws = 0, full_draws = 0;
    double fast_pixels = 0, full_pixels = 0;
    double all_pixels = 0;
    double opaque_pixels = 0;   // drawn by a lean program without a discard and without a blend
    uint64_t sync_draws = 0;    // draws whose lean program needed a uniform sent
    bool key_census = false;
    struct KeyCount { uint64_t key = 0; uint64_t draws = 0; double pixels = 0; int program = -1; };
    static constexpr int kKeySlots = 256;
    KeyCount keys[kKeySlots];
    uint32_t last_program = 0;
};

namespace {
GlShaderLibrary::LeanState* g_lean_census = nullptr;   // the one library's lean state, for Ps2ProgramCensusLines
}  // namespace

bool GlShaderLibrary::BuildLeanPrograms(const char* vertex) {
    if (!lean_) lean_ = new LeanState();
    LeanState& ls = *lean_;
    ls = LeanState();
    std::memset(ls.index, -1, sizeof ls.index);
    lean_count_ = 0;
    const char* env = std::getenv("TE_GFX_VITA_PS2_LEAN");
    if (env && env[0] == '0') {
        std::fprintf(stderr, "[gl] lean PS2 programs: off (TE_GFX_VITA_PS2_LEAN=0)\n");
        return false;
    }
    const char* census = std::getenv("TE_GFX_VITA_PS2_CENSUS");
    ls.key_census = census && census[0] == '1';
    // diagnosis: TE_GFX_VITA_PS2_LEAN_ONLY=i,j,... -- only those variants take draws (the others' states draw as before)
    uint32_t only = 0xFFFFFFFFu;
    if (const char* o = std::getenv("TE_GFX_VITA_PS2_LEAN_ONLY")) {
        only = 0;
        for (const char* c = o; *c;) {
            const long v = std::strtol(c, nullptr, 10);
            if (v >= 0 && v < 32) only |= 1u << v;
            const char* comma = std::strchr(c, ',');
            if (!comma) break;
            c = comma + 1;
        }
        std::fprintf(stderr, "[gl] lean PS2 programs: only variants 0x%X take draws (TE_GFX_VITA_PS2_LEAN_ONLY)\n", only);
    }
    int built = 0;
    // a built program's uniform locations, its sampler on unit 0, and the check that it reads no texture size uniform
    auto setup = [&](LeanState::Program& p, uint32_t id, int i) -> bool {
        p = LeanState::Program();
        if (!id) return false;
        p.id = id;
        GlUniformLocations full_locs;
        LookupLocationsOf(id, full_locs);
        for (int m = 0; m < kMirrors; ++m) {
            p.mirror_loc[m] = full_locs.*(kLeanMirrors[m].field);
            p.mirror_sent[m] = GlUniformCache::Shadow();
        }
        for (int m = 0; m < kArrayMirrors; ++m) {
            p.array_loc[m] = full_locs.*(kLeanArrayMirrors[m].field);
            p.array_count[m] = 0;
        }
        p.at_loc = glGetUniformLocation(id, "u_lean_at");
        p.fail_loc = glGetUniformLocation(id, "u_lean_fail");
        p.fba_loc = glGetUniformLocation(id, "u_lean_fba");
        // the sampler on unit 0 (u_texture: the PS2 program's unit)
        glUseProgram(id);
        if (full_locs.tex_sampler >= 0) glUniform1i(full_locs.tex_sampler, 0);
        glUseProgram(0);
        // a lean program must not read a texture size the state cache would send (it is never handed them)
        bool sizes = false;
        for (const char* name : kVitaSamplerSizes)
            sizes = sizes || glGetUniformLocation(id, (std::string("te_size_") + name).c_str()) >= 0;
        if (sizes) {
            std::fprintf(stderr, "[gl] lean PS2 program %d reads a texture size uniform: not used\n", i);
            glDeleteProgram(id);
            p = LeanState::Program();
            return false;
        }
        return true;
    };
    uint32_t fs_obj[kLeanMax] = {};   // each variant's compiled fragment shader, linked again with the vertex paths below
    for (int i = 0; i < kLeanMax; ++i) {
        const LeanVariant& v = kLeanVariants[i];
        if (!(only & (1u << i))) continue;
        const std::string fs = LeanSource(v);
        char label[96];
        std::snprintf(label, sizeof label, "ps2 lean %d (%s)", i, v.label);
        const uint32_t id = BuildProgram(vertex, fs.c_str(), label, nullptr, &fs_obj[i]);
        std::fflush(stderr);
        LeanState::Program& p = ls.programs[i];
        if (!setup(p, id, i)) continue;
        if (v.nocolor) {
            ls.nocolor = i;
        } else if (v.tex) {
            ls.index[1][v.tfx][v.tcc][v.mip][v.atest] = i;
        } else {
            for (int tfx = 0; tfx < 4; ++tfx)
                for (int tcc = 0; tcc < 2; ++tcc)
                    for (int mip = 0; mip < 2; ++mip) ls.index[0][tfx][tcc][mip][v.atest] = i;
        }
        ++built;
    }
    // The vertex paths (TE_GFX_VITA_LEAN_VS=0: none): each path's vertex shader compiled ONCE -- with its first variant's
    // fragment shader, as a pair (vitaGL's pair translation) -- and linked with every other variant's fragment shader as
    // compiled above (the varyings bound alike: every PS2 vertex source declares v_texcoord, v_color, v_fog in that order),
    // each program cached as a pair; a launch after the first loads them all from the cache.
    const char* lean_vs_env = std::getenv("TE_GFX_VITA_LEAN_VS");
    const bool want_paths = built > 0 && !(lean_vs_env && lean_vs_env[0] == '0');
    int path_built = 0;
    for (auto& row : g_raw_twins)
        for (RawTwin& t : row) ReleaseRawTwin(t);
    raw_mesh_ = false;
    for (RawTwin& t : g_vert_twins) ReleaseRawTwin(t);
    raw_verts_ = false;
    // every packet names a twin (and a fragment program of it) released above: void them all
    ++g_packet_epoch;
    g_packet_last = 0;
    g_frag_last_buf = nullptr;
    g_vert_last_buf = nullptr;     // (the twins' vertex programs go: no image stands for one of them)
    g_vert_last_prog = nullptr;
    g_vert_spare_prog = nullptr;
    if (g_packets_mode < 0) {
        const char* pk = std::getenv("TE_GFX_VITA_PACKETS");
        g_packets_mode = (pk && pk[0] == '0') ? 0 : 1;
    }
    int twins = 0, shared_vertex = 0;
    int vert_twins = 0, vert_tried = 0;   // the raw vertex path's
    if (want_paths) {
        static const char* const kPathNames[LeanState::kVsPaths] = {"mesh", "lit mesh", "vertices", "raw"};
        // the raw mesh draws (TE_GFX_VITA_RAW_MESH=0: off) need the mesh paths' GXPs (BuildProgram keeps them meanwhile)
        const char* raw_env = std::getenv("TE_GFX_VITA_RAW_MESH");
        const bool want_raw = !(raw_env && raw_env[0] == '0');
        // ... and the raw vertex draws (TE_GFX_VITA_RAW_VERTS=0: off) the raw vertex path's (LEAN_VS_PATH 4)
        const char* rv_env = std::getenv("TE_GFX_VITA_RAW_VERTS");
        const bool want_rv = want_raw && !(rv_env && rv_env[0] == '0');
        for (int k = 0; k < LeanState::kVsPaths; ++k) {
            std::string vsrc = GlShaderSources::LeanPs2Vertex();
            char def[48];
            std::snprintf(def, sizeof def, "#define LEAN_VS_PATH %d\n", k + 1);
            const size_t version = vsrc.find("#version");
            const size_t line = version == std::string::npos ? std::string::npos : vsrc.find('\n', version);
            vsrc.insert(line == std::string::npos ? 0 : line + 1, def + DepthDefines());
            uint32_t vs_obj = 0;
            g_capture_gxps = want_raw && (k < 2 || (k == 3 && want_rv));
            // the path's first twin: the others take its vertex program (BuildRawTwin's `share`; with the raw
            // draws' re-use off -- TE_GFX_VITA_GXM_CACHE=0 / wrappers -- each its own, as before the cache)
            const RawTwin* path_first = nullptr;
            for (int i = 0; i < kLeanMax; ++i) {
                if (!ls.programs[i].id || !fs_obj[i]) continue;
                const std::string fs = LeanSource(kLeanVariants[i]);
                char label[112];
                std::snprintf(label, sizeof label, "ps2 lean %d (%s) over the %s vertex path", i, kLeanVariants[i].label, kPathNames[k]);
                const uint32_t id = vs_obj ? BuildProgram(vsrc.c_str(), fs.c_str(), label, nullptr, nullptr, vs_obj, fs_obj[i])
                                           : BuildProgram(vsrc.c_str(), fs.c_str(), label, &vs_obj, nullptr);
                std::fflush(stderr);
                if (!setup(ls.paths[k][i], id, i)) continue;
                ++path_built;
                if (g_capture_gxps && k == 3) {
                    // the raw vertex path's twin (its path's first twin's vertex program, as the mesh paths')
                    const ProgramGxps* g = GxpsOf(id);
                    ++vert_tried;
                    if (g && BuildRawVertexTwin(g_vert_twins[i], *g, VertexReuseOn() ? path_first : nullptr)) {
                        ++vert_twins;
                        if (!path_first) path_first = &g_vert_twins[i];
                    } else {
                        ReleaseRawTwin(g_vert_twins[i]);
                        std::fprintf(stderr, "[gl] raw vertex draws: no twin for %s (they draw through vitaGL)\n", label);
                    }
                } else if (g_capture_gxps) {
                    const ProgramGxps* g = GxpsOf(id);
                    if (g && BuildRawTwin(g_raw_twins[k][i], *g, k == 1, false, VertexReuseOn() ? path_first : nullptr)) {
                        ++twins;
                        if (!path_first) path_first = &g_raw_twins[k][i];
                        if (g_raw_twins[k][i].shares_vertex) ++shared_vertex;
                    } else {
                        std::fprintf(stderr, "[gl] raw mesh draws: no twin for %s (they draw through vitaGL)\n", label);
                    }
                }
            }
            g_capture_gxps = false;
            g_gxps.clear();
            if (vs_obj) glDeleteShader(vs_obj);   // (vitaGL keeps it while a program holds it)
        }
        ls.paths_built = path_built > 0;
        raw_mesh_ = twins > 0;
        raw_verts_ = vert_twins > 0;
        if (want_raw)
            std::fprintf(stderr, "[gl] raw vertex draws (TE_GFX_VITA_RAW_VERTS=0: off): %s -- %d of %d raw vertex path "
                         "programs have a GXM twin\n", !want_rv ? "off" : raw_verts_ ? "on" : "off (no twin)", vert_twins, vert_tried);
        if (want_raw)
            std::fprintf(stderr, "[gl] raw mesh draws: %d of %d mesh-path programs have a GXM twin (TE_GFX_VITA_RAW_MESH=0: off; %d of "
                         "them take their path's one vertex program); mesh draw packets %s\n", twins, 2 * built, shared_vertex,
                         !raw_mesh_ ? "off (no raw draws)" : g_packets_mode == 0 ? "off (TE_GFX_VITA_PACKETS=0)"
                                                                                  : "on (TE_GFX_VITA_PACKETS=0: off)");
        // THE INSTANCED MESH PATH -- OFF BY DEFAULT (TE_GFX_VITA_INSTANCING=1: on; =2 the diagnosis: the instanced
        // twin's programs with one plain draw per instance): in Vita3K the instanced twin's pictures differ from the mesh
        // path's by a few pixels of 1-4 levels where runs draw (the same with one plain draw per instance: the programs, not
        // the instanced draw; the same with two forms of different arithmetic) -- not picture-identical in the
        // emulator, so not on unless asked. Its vertex shader compiled once with the first variant's fragment shader, linked
        // with every other variant's (as the paths above), each program's GXPs taken for its GXM twin -- the GL programs
        // themselves never draw (deleted once their GXPs are taken). A variant's twin is used only when its fragment side
        // is its mesh twin's (the same GXP: a packet's composed fragment image serves both). Built in the shader seed's
        // run (TE_GL_SHADER_SEED_BUILD=1) too: the shipped cache holds them, an opt-in on the device compiles nothing.
        for (RawTwin& t : g_inst_twins) ReleaseRawTwin(t);
        const char* inst_env = std::getenv("TE_GFX_VITA_INSTANCING");
        // TE_GFX_VITA_INSTANCE=1 / abN (platform graphics_batch.cpp's plan, drawn instanced) asks for the twins too
        if (!(inst_env && *inst_env)) {
            const char* plan_env = std::getenv("TE_GFX_VITA_INSTANCE");
            if (plan_env && (plan_env[0] == '1' || (plan_env[0] == 'a' && plan_env[1] == 'b'))) inst_env = "1";
        }
        const bool inst_wanted = inst_env && (inst_env[0] == '1' || inst_env[0] == '2');
        const bool inst_build = raw_mesh_ && g_packets_mode != 0 && (inst_wanted || BuildEverything());
        g_instancing = inst_build && inst_wanted;
        g_instancing_split = inst_wanted && inst_env[0] == '2';
        if (inst_build) {
            std::string vsrc = GlShaderSources::LeanPs2InstancedVertex();
            {
                const std::string depth = DepthDefines();   // (the mesh path's own: LEAN_VS_PATH 1 resolves the same text)
                const size_t version = vsrc.find("#version");
                const size_t line = version == std::string::npos ? std::string::npos : vsrc.find('\n', version);
                if (!depth.empty()) vsrc.insert(line == std::string::npos ? 0 : line + 1, depth);
            }
            uint32_t vs_obj = 0;
            int inst_twins = 0, inst_tried = 0;
            g_capture_gxps = true;
            for (int i = 0; i < kLeanMax; ++i) {
                if (!ls.programs[i].id || !fs_obj[i] || !g_raw_twins[0][i].ok) continue;   // (a variant the mesh path draws raw)
                ++inst_tried;
                const std::string fs = LeanSource(kLeanVariants[i]);
                char label[112];
                std::snprintf(label, sizeof label, "ps2 lean %d (%s) over the instanced mesh vertex path", i, kLeanVariants[i].label);
                const uint32_t id = vs_obj ? BuildProgram(vsrc.c_str(), fs.c_str(), label, nullptr, nullptr, vs_obj, fs_obj[i])
                                           : BuildProgram(vsrc.c_str(), fs.c_str(), label, &vs_obj, nullptr);
                std::fflush(stderr);
                if (!id) continue;
                const ProgramGxps* g = GxpsOf(id);
                RawTwin& t = g_inst_twins[i];
                const bool built_twin = g && BuildRawTwin(t, *g, false, true);
                const char* differs = nullptr;
                t.frag_as_mesh = built_twin && SameFragmentLayout(t, g_raw_twins[0][i], &differs);
                if (t.frag_as_mesh) {
                    ++inst_twins;
                } else {
                    std::fprintf(stderr, "[gl] instanced packet draws: no twin for %s (%s%s): its runs draw one by one\n", label,
                                 built_twin ? "its fragment side differs from the mesh twin's: " : "not as the raw draw writes it",
                                 built_twin && differs ? differs : "");
                    ReleaseRawTwin(t);
                }
                // (the GL program goes; vitaGL hands its id to the next program made -- its captured GXPs go with it)
                glDeleteProgram(id);
                g_gxps.clear();
            }
            g_capture_gxps = false;
            g_gxps.clear();
            if (vs_obj) glDeleteShader(vs_obj);
            if (!inst_twins) g_instancing = false;
            std::fprintf(stderr, "[gl] instanced packet draws (TE_GFX_VITA_INSTANCING=1: on, off by default): %d of %d mesh-path "
                         "variants have an instanced GXM twin -- a run of draws of one packet, one mesh range and one shift is %s\n",
                         inst_twins, inst_tried,
                         g_instancing_split ? "the instanced twin's programs, one plain draw per instance (=2: the diagnosis)"
                         : g_instancing ? "ONE instanced draw (the matrices on an instance stream)"
                                        : "drawn one by one (the twins built for the shader seed's run)");
        } else if (raw_mesh_ && g_packets_mode != 0) {
            std::fprintf(stderr, "[gl] instanced packet draws: off (the default; TE_GFX_VITA_INSTANCING=1: on)\n");
        }
    }
    for (uint32_t obj : fs_obj)
        if (obj) glDeleteShader(obj);
    lean_count_ = built > 0 ? kLeanMax : 0;
    g_lean_census = &ls;
    std::fprintf(stderr, "[gl] lean PS2 programs: %d of %d built; over the vertex paths %d of %d%s%s\n", built, kLeanMax,
                 path_built, want_paths ? built * LeanState::kVsPaths : 0, want_paths ? "" : " (TE_GFX_VITA_LEAN_VS=0)",
                 ls.key_census ? "; state census on (TE_GFX_VITA_PS2_CENSUS=1)" : "");
    return built > 0;
}

uint32_t GlShaderLibrary::SelectPs2(GlStateCache& st, GlUniformCache& u, const float* box, int frame_w, int frame_h,
                                    const RawMesh* raw, bool* drew_raw) const {
    if (drew_raw) *drew_raw = false;
    // the draw's share of the frame (its screen box inside the frame)
    double px = 0;
    if (box) {
        const float x0 = std::max(0.f, std::min(box[0], float(frame_w))), x1 = std::max(0.f, std::min(box[2], float(frame_w)));
        const float y0 = std::max(0.f, std::min(box[1], float(frame_h))), y1 = std::max(0.f, std::min(box[3], float(frame_h)));
        if (x1 > x0 && y1 > y0) px = double(x1 - x0) * double(y1 - y0);
    }
    LeanState* lsp = lean_;
    if (lsp && lsp->tracked_gen != u.Generation()) {
        // the shadows' entries move at a Clear (a relink): tracked again
        LeanState& ls = *lsp;
        ls.tracked_gen = u.Generation();
        ls.tracked = true;
        int n = 0;
        const FastConstant* fc = FastConstants(&n);
        ls.fast_n = std::min(n, 16);
        for (int i = 0; i < ls.fast_n; ++i) {
            ls.fast[i] = u.Track(loc_.*(fc[i].field));
            ls.fast_value[i] = fc[i].value;
            ls.tracked = ls.tracked && ls.fast[i];
        }
        for (int k = 0; k < kKeyFields; ++k) {
            ls.key[k] = u.Track(loc_.*(kKeySpecs[k].field));
            ls.tracked = ls.tracked && (ls.key[k] || k == kBlend || k == kFog);
        }
        for (int m = 0; m < kMirrors; ++m) {
            const int loc = loc_.*(kLeanMirrors[m].field);
            ls.mirror[m] = loc >= 0 ? u.Track(loc) : nullptr;
            ls.tracked = ls.tracked && (loc < 0 || ls.mirror[m]);
        }
        for (int m = 0; m < kArrayMirrors; ++m) {
            const int loc = loc_.*(kLeanArrayMirrors[m].field);
            ls.array[m] = loc >= 0 ? u.TrackArray(loc) : nullptr;
            ls.tracked = ls.tracked && (loc < 0 || ls.array[m]);
        }
        // every lean program's copies are void: send everything again
        for (int i = 0; i < kLeanMax; ++i) {
            LeanState::Program& p = ls.programs[i];
            for (int m = 0; m < kMirrors; ++m) p.mirror_sent[m] = GlUniformCache::Shadow();
            for (int m = 0; m < kArrayMirrors; ++m) p.array_count[m] = 0;
            p.at_sent[0] = p.fail_sent[0] = p.fba_sent = -1.f;
            for (int k = 0; k < LeanState::kVsPaths; ++k) {
                LeanState::Program& q = ls.paths[k][i];
                for (int m = 0; m < kMirrors; ++m) q.mirror_sent[m] = GlUniformCache::Shadow();
                for (int m = 0; m < kArrayMirrors; ++m) q.array_count[m] = 0;
                q.at_sent[0] = q.fail_sent[0] = q.fba_sent = -1.f;
            }
        }
        // the uniforms that choose a vertex path (u_native, u_mesh_lit, u_uv_mode: the generic vertex program's branches)
        ls.vs_native = loc_.native >= 0 ? u.Track(loc_.native) : nullptr;
        ls.vs_lit = loc_.mesh_lit >= 0 ? u.Track(loc_.mesh_lit) : nullptr;
        ls.vs_uv_mode = loc_.uv_mode >= 0 ? u.Track(loc_.uv_mode) : nullptr;
        if (!ls.tracked) std::fprintf(stderr, "[gl] lean PS2 programs: the uniform cache cannot track the full program's uniforms: not used\n");
    }
    auto use = [&](uint32_t prog) {
        st.UseProgram(prog);
        // the full program reads the te_size_* uniforms (its dead arms' texel fetches): sent for the bound textures
        if (prog == ps2_program_) st.ApplyVitaSamplerSizes();
        return prog;
    };
    auto fallback = [&](int reason) -> uint32_t {
        bool fast = Ps2FastEnabled();
        if (fast) {
            int n = 0;
            const FastConstant* fc = FastConstants(&n);
            for (int i = 0; i < n && fast; ++i) fast = u.Holds1i(loc_.*(fc[i].field), fc[i].value);
        }
        if (lsp) {
            lsp->fallback[reason]++;
            lsp->fallback_pixels[reason] += px;
            lsp->all_pixels += px;
            if (fast) { lsp->fast_draws++; lsp->fast_pixels += px; }
            else { lsp->full_draws++; lsp->full_pixels += px; }
        }
        if (fast) Census().draws_ps2_fast++;
        else Census().draws_ps2_full++;
        FillCensusDraw(fast ? kFillFast : kFillFull, px, 0);
        return use(fast ? ps2_fast_program_ : ps2_program_);
    };
    if (!lsp || !lean_count_) return fallback(kFbOff);
    LeanState& ls = *lsp;
    if (!lean_enabled_ || !ls.tracked) return fallback(kFbOff);
    for (int i = 0; i < ls.fast_n; ++i)
        if (ls.fast[i]->kind != 1 || ls.fast[i]->i[0] != ls.fast_value[i]) return fallback(kFbNotFast);
    auto value = [&](int k, int* v) {
        const GlUniformCache::Shadow* s = ls.key[k];
        if (!s || s->kind != (k == kMipCtl ? 5 : 1)) return false;
        *v = s->i[0];
        return true;
    };
    int use_texture = 0, ate = 0, fba = 0;
    if (!value(kUseTexture, &use_texture) || !value(kAte, &ate) || !value(kFba, &fba)) return fallback(kFbUnknown);
    {
        int fog = 0;
        if (ls.key[kFog] && ls.key[kFog]->kind == 1) fog = ls.key[kFog]->i[0];
        if (fog != 0) return fallback(kFbFog);
    }
    int blend = 0;
    if (ls.key[kBlend] && ls.key[kBlend]->kind == 1) blend = ls.key[kBlend]->i[0];
    // the alpha test as the full program runs it: 0 = no fragment changes, 1 = discards where it fails, 2 = changes the
    // result where it fails (ZB_ONLY / RGB_ONLY); AFAIL FB_ONLY leaves a failing fragment as it is in the PS2 program
    int atest = 0, atst = 1, aref = 0, afail = 0;
    if (ate) {
        if (!value(kAtst, &atst) || !value(kAref, &aref) || !value(kAfail, &afail)) return fallback(kFbUnknown);
        const bool can_fail = !(atst == 1 || (atst == 5 && aref <= 0) || (atst == 3 && aref >= 255) ||
                                (atst == 7 && (aref < 0 || aref > 255)));
        if (afail != 1 && can_fail) atest = afail == 0 ? 1 : 2;
    }
    const int cmask = st.ColorMaskBits();
    int tfx = 0, tcc = 0, mip = 0;
    int variant = -1;
    if (cmask == 0 && atest != 1) {
        // nothing but depth / stencil is written, and no fragment is discarded
        if (ls.nocolor < 0) return fallback(kFbNoVariant);
        variant = ls.nocolor;
    } else {
        if (use_texture) {
            int lod_hw = 0;
            if (!value(kTfx, &tfx) || !value(kTcc, &tcc) || !value(kLodHw, &lod_hw) || !ls.key[kMipCtl] ||
                ls.key[kMipCtl]->kind != 5)
                return fallback(kFbUnknown);
            const int levels = ls.key[kMipCtl]->i[0], lcm = ls.key[kMipCtl]->i[1];
            mip = levels > 0 ? 1 : 0;
            if (mip && (lcm != 0 || lod_hw != 0)) return fallback(kFbMipMode);
            if (tfx < 0 || tfx > 3 || tcc < 0 || tcc > 1) return fallback(kFbNoVariant);
        }
        variant = ls.index[use_texture ? 1 : 0][tfx][tcc][mip][atest];
    }
    if (ls.key_census) {
        // the state key: texture, tfx, tcc, mips, the test (class, method, AFAIL), FBA, blend, the colour mask
        const uint64_t key = uint64_t(use_texture) | uint64_t(tfx) << 1 | uint64_t(tcc) << 3 | uint64_t(mip) << 4 |
                             uint64_t(atest) << 5 | uint64_t(ate ? atst : 15) << 7 | uint64_t(ate ? afail : 0) << 11 |
                             uint64_t(fba) << 13 | uint64_t(blend) << 14 | uint64_t(cmask & 15) << 15 |
                             uint64_t(ate && atest ? aref : 0) << 19 | uint64_t(1) << 40;
        uint32_t at = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 56) & (LeanState::kKeySlots - 1);
        for (int probe = 0; probe < LeanState::kKeySlots; ++probe, at = (at + 1) & (LeanState::kKeySlots - 1)) {
            LeanState::KeyCount& kc = ls.keys[at];
            if (kc.key == key || kc.key == 0) {
                kc.key = key;
                kc.draws++;
                kc.pixels += px;
                kc.program = variant;
                break;
            }
        }
    }
    if (variant < 0 || !ls.programs[variant].id) return fallback(kFbNoVariant);
    // The vertex path: the program of this variant over the lean vertex program of the draw's path, when the uniforms the
    // generic vertex program branches on hold that path's values (a variant that reads no texture coordinates takes the
    // path whatever u_uv_mode says: its v_texcoord is never read); else this variant over the generic vertex program
    LeanState::Program* chosen = &ls.programs[variant];
    int path = 0;
    if (ls.paths_built) {
        auto ival = [](const GlUniformCache::Shadow* s, int* v) {
            if (!s || s->kind != 1) return false;
            *v = s->i[0];
            return true;
        };
        int native = -1, lit = -1, uvm = -1;
        if (ival(ls.vs_native, &native) && ival(ls.vs_uv_mode, &uvm)) {
            const bool reads_uv = kLeanVariants[variant].tex != 0;
            if (native == 1 && ival(ls.vs_lit, &lit) && (lit == 0 || lit == 1) && (!reads_uv || uvm == 2)) path = lit ? 2 : 1;
            else if (native == 0 && (!reads_uv || uvm == 0)) path = 3;
            else if (native == 2 && (!reads_uv || uvm == 2)) path = 4;
        }
        if (path && ls.paths[path - 1][variant].id) {
            chosen = &ls.paths[path - 1][variant];
        } else {
            ls.path_misses++;
            path = 0;
        }
    }
    ls.path_draws[path]++;
    LeanState::Program& p = *chosen;
    // the alpha test's interval as the lean program takes it: pass where lo <= A <= hi, inverted for NOTEQUAL (A, AREF:
    // 0..255); the fail factor (RGB_ONLY keeps the colour, ZB_ONLY writes zero); FBA
    float at_lo = 0.f, at_hi = 255.f, at_inv = 0.f;
    switch (atst) {
        case 0: at_lo = 1.f; at_hi = 0.f; break;                     // NEVER
        case 2: at_hi = float(aref) - 1.f; break;                   // LESS
        case 3: at_hi = float(aref); break;                         // LEQUAL
        case 4: at_lo = at_hi = float(aref); break;                 // EQUAL
        case 5: at_lo = float(aref); break;                         // GEQUAL
        case 6: at_lo = float(aref) + 1.f; break;                   // GREATER
        case 7: at_lo = at_hi = float(aref); at_inv = 1.f; break;   // NOTEQUAL
        default: break;                                             // ALWAYS
    }
    const float fail_a = afail == 3 ? 1.f : 0.f;
    const float fba_f = fba ? 1.f : 0.f;
    auto counted = [&]() {
        p.draws++;
        p.pixels += px;
        ls.all_pixels += px;
        if (!blend) {
            p.draws_opaque++;
            if (kLeanVariants[variant].atest != 1) ls.opaque_pixels += px;
        }
        Census().draws_ps2_lean++;
    };
    FillCensusDraw(kFillLean + variant, px, path);   // (the Vita's fill census: this draw's visibility counter, on a sampled frame)
    if (raw && drew_raw && raw->vertices) {
        // the raw vertex draw: the raw vertex path's twin, the same values (declined: g_rv.declined -- 0 off, 1 not
        // the raw vertex path's program, 2 no twin / fragment program, 3 a value not known, 4 the ring's record, 5 vitaGL's
        // texture record not as expected); declined, the draw goes on below as the GL draw it always was
        int why;
        if (!RawVertsEnabled()) {
            why = 0;
        } else if (path != 4) {
            why = 1;
        } else if (raw->count < 3) {
            why = 4;
        } else {
            RawValues v{ls.mirror[0], ls.mirror[1], ls.mirror[5], ls.mirror[6], ls.mirror[7], ls.array[0], ls.array[1],
                        {at_lo, at_hi, at_inv}, {fail_a, fail_a, fail_a, 0.f}, fba_f};
            why = RawVertexSubmit(g_vert_twins[variant], v, *raw);
        }
        if (why < 0) {
            *drew_raw = true;
            counted();
            g_rv.drawn++;
            return p.id;
        }
        g_rv.declined[why >= 0 && why < 6 ? why : 0]++;
    } else if (raw && drew_raw) {
        // the raw mesh draw: a mesh-path program's twin, the same values (declined: the census's raw_mesh_declined index --
        // 0 off, 1 not a mesh-path program, 2 no twin / fragment program, 3 a value not known, 4 not 3..0xC000 vertices,
        // 5 vitaGL's texture record not as expected)
        int why;
        if (!RawMeshEnabled()) {
            why = 0;
        } else if (path != 1 && path != 2) {
            why = 1;
        } else if (raw->count > kRawMaxVerts || raw->count < 3) {
            why = 4;
        } else {
            RawValues v{ls.mirror[0], ls.mirror[1], ls.mirror[5], ls.mirror[6], ls.mirror[7], ls.array[0], ls.array[1],
                        {at_lo, at_hi, at_inv}, {fail_a, fail_a, fail_a, 0.f}, fba_f};
            why = RawSubmit(g_raw_twins[path - 1][variant], v, *raw, path == 2);
        }
        if (why < 0) {
            *drew_raw = true;
            counted();
            Census().draws_raw_mesh++;
            if (g_packet_last) {
                // the packet this draw recorded: its census half (what a replay of it counts, as counted() above)
                RawPacket& k = g_packets[g_packet_last & (kPackets - 1)];
                if (k.id == g_packet_last) {
                    k.program = &p;
                    k.path = path;
                    k.variant = variant;
                    k.blend = blend != 0;
                    k.discards = kLeanVariants[variant].atest == 1;
                }
            }
            return p.id;
        }
        Census().raw_mesh_declined[why < 6 ? why : 0]++;
    }
    // the program's uniforms brought to the full program's values (only what changed since this program last drew)
    bool sent = false;
    for (int m = 0; m < kMirrors; ++m) {
        const int loc = p.mirror_loc[m];
        const GlUniformCache::Shadow* s = ls.mirror[m];
        if (loc < 0 || !s || s->kind == 0 || SameShadow(*s, p.mirror_sent[m])) continue;
        SendShadow(loc, *s);
        p.mirror_sent[m] = *s;
        sent = true;
    }
    for (int m = 0; m < kArrayMirrors; ++m) {
        const int loc = p.array_loc[m];
        const GlUniformCache::ArrayShadow* a = ls.array[m];
        if (loc < 0 || !a || a->count <= 0 || a->count * 4 > GlUniformCache::kArrayFloats) continue;
        const size_t bytes = size_t(a->count) * 4 * sizeof(float);
        if (p.array_count[m] == a->count && std::memcmp(p.array_sent[m], a->v, bytes) == 0) continue;
        glUniform4fv(loc, a->count, a->v);
        Census().lean_uniform_calls++;
        p.array_count[m] = a->count;
        std::memcpy(p.array_sent[m], a->v, bytes);
        sent = true;
    }
    if (p.at_loc >= 0) {
        if (p.at_sent[0] != at_lo || p.at_sent[1] != at_hi || p.at_sent[2] != at_inv) {
            glUniform3f(p.at_loc, at_lo, at_hi, at_inv);
            Census().lean_uniform_calls++;
            p.at_sent[0] = at_lo; p.at_sent[1] = at_hi; p.at_sent[2] = at_inv;
            sent = true;
        }
    }
    if (p.fail_loc >= 0) {
        if (p.fail_sent[0] != fail_a || p.fail_sent[3] != 0.f) {
            glUniform4f(p.fail_loc, fail_a, fail_a, fail_a, 0.f);
            Census().lean_uniform_calls++;
            p.fail_sent[0] = p.fail_sent[1] = p.fail_sent[2] = fail_a;
            p.fail_sent[3] = 0.f;
            sent = true;
        }
    }
    if (p.fba_loc >= 0) {
        if (p.fba_sent != fba_f) {
            glUniform1f(p.fba_loc, fba_f);
            Census().lean_uniform_calls++;
            p.fba_sent = fba_f;
            sent = true;
        }
    }
    if (sent) ls.sync_draws++;
    counted();
    return use(p.id);
}

uint32_t GlShaderLibrary::TakeRawPacket() const {
    const uint32_t id = g_packet_last;
    g_packet_last = 0;
    return id;
}
void GlShaderLibrary::DropRawPacket() const { g_packet_last = 0; }
bool GlShaderLibrary::PacketsEnabled() const { return g_packets_mode != 0 && g_packets_on; }
bool GlShaderLibrary::InstancingOn() const { return g_instancing && g_packets_mode != 0 && g_packets_on && RawMeshEnabled(); }
void GlShaderLibrary::SetPacketsOn(bool on) const { g_packets_on = on; }

int GlShaderLibrary::DrawRawPacket(uint32_t id, uint32_t vbo, uint32_t ibo, uint32_t first, uint32_t count, const MeshDrawParams& mp,
                                   int frame_w, int frame_h) const {
    // (the state census of TE_GFX_VITA_PS2_CENSUS=1 is SelectPs2's alone: no replays while it counts)
    if (g_packets_mode == 0 || !g_packets_on || !RawMeshEnabled() || !Ps2LeanEnabled() || !lean_ || lean_->key_census) return 0;
    const RawPacket& k = g_packets[id & (kPackets - 1)];
    if (!id || k.id != id || k.epoch != g_packet_epoch || !k.twin || !k.program) return 3;
    // the u_screen_size it was recorded with is the run's (DrawMesh sets it from the target for every run)
    if (k.twin->v_screen.offset >= 0 && (k.screen[0] != static_cast<float>(frame_w) || k.screen[1] != static_cast<float>(frame_h)))
        return 6;
    // the draw's share of the frame (as SelectPs2 counts the draw it replays; the fill census's stand-in box too)
    double px = 0;
    {
        const float x0 = std::max(0.f, std::min(mp.box[0], float(frame_w))), x1 = std::max(0.f, std::min(mp.box[2], float(frame_w)));
        const float y0 = std::max(0.f, std::min(mp.box[1], float(frame_h))), y1 = std::max(0.f, std::min(mp.box[3], float(frame_h)));
        if (x1 > x0 && y1 > y0) px = double(x1 - x0) * double(y1 - y0);
    }
    const int why = RawReplay(k, vbo, ibo, first, count, mp, px);
    if (why >= 0) return why;
    // the census, as SelectPs2 counts the draw it replays (its lean program, vertex path, the draw's share of the frame)
    LeanState& ls = *lean_;
    LeanState::Program& p = *static_cast<LeanState::Program*>(k.program);
    ls.path_draws[k.path]++;
    p.draws++;
    p.pixels += px;
    ls.all_pixels += px;
    if (!k.blend) {
        p.draws_opaque++;
        if (!k.discards) ls.opaque_pixels += px;
    }
    Census().draws_ps2_lean++;
    Census().draws_raw_mesh++;
    return -1;
}

int GlShaderLibrary::DrawRawPacketInstances(uint32_t id, uint32_t vbo, uint32_t ibo, uint32_t first, uint32_t count,
                                            const MeshInstance* inst, int n, int frame_w, int frame_h) const {
    // DrawRawPacket's conditions, the instanced twin of the packet's variant, and not on the fill census's sampled frames
    // (each draw's own visibility counter there: drawn one by one)
    if (!g_instancing || g_packets_mode == 0 || !g_packets_on || !RawMeshEnabled() || !Ps2LeanEnabled() || !lean_ ||
        lean_->key_census || FillCensusSampling())
        return 0;
    const RawPacket& k = g_packets[id & (kPackets - 1)];
    if (!id || k.id != id || k.epoch != g_packet_epoch || !k.twin || !k.program) return 3;
    if (k.variant < 0 || k.variant >= kLeanMax || k.twin != &g_raw_twins[0][k.variant]) return 3;   // (the unlit mesh path's)
    RawTwin& it = g_inst_twins[k.variant];
    if (k.twin->v_screen.offset >= 0 && (k.screen[0] != static_cast<float>(frame_w) || k.screen[1] != static_cast<float>(frame_h)))
        return 6;
    const int why = RawReplayInstances(k, it, vbo, ibo, first, count, inst, n);
    if (why >= 0) return why;
    // the census, as DrawRawPacket counts each draw it replays
    LeanState& ls = *lean_;
    LeanState::Program& p = *static_cast<LeanState::Program*>(k.program);
    for (int i = 0; i < n; ++i) {
        double px = 0;
        const float* b = inst[i].box;
        const float x0 = std::max(0.f, std::min(b[0], float(frame_w))), x1 = std::max(0.f, std::min(b[2], float(frame_w)));
        const float y0 = std::max(0.f, std::min(b[1], float(frame_h))), y1 = std::max(0.f, std::min(b[3], float(frame_h)));
        if (x1 > x0 && y1 > y0) px = double(x1 - x0) * double(y1 - y0);
        ls.path_draws[k.path]++;
        p.draws++;
        p.pixels += px;
        ls.all_pixels += px;
        if (!k.blend) {
            p.draws_opaque++;
            if (!k.discards) ls.opaque_pixels += px;
        }
    }
    Census().draws_ps2_lean += uint64_t(n);
    Census().draws_raw_mesh += uint64_t(n);
    return -1;
}

// ---- the GPU-SKINNING programs (the Vita) --------------------------------------------------------------------------------
// The skin vertex program (GlShaderSources::Ps2SkinVertex) over each lean fragment variant: the same family, the same
// selection (the lean key read from the full program's uniform values), the same mirrored uniforms -- only the vertex
// program differs. The textured MODULATE variants (3-6: what the skins' materials take) are built with the library; the
// others the first time a skinned draw takes their state (a compile on the drawing thread, once: cached afterwards).
struct GlShaderLibrary::SkinState {
    struct Program {
        SkinProgram sp;
        bool tried = false;
        int mirror_loc[kMirrors];
        GlUniformCache::Shadow mirror_sent[kMirrors];
        int array_loc[kArrayMirrors];
        int array_count[kArrayMirrors];
        float array_sent[kArrayMirrors][GlUniformCache::kArrayFloats];
        int at_loc = -1, fail_loc = -1, fba_loc = -1;
        float at_sent[3] = {-1.f, -1.f, -1.f}, fail_sent[4] = {-1.f, -1.f, -1.f, -1.f}, fba_sent = -1.f;
        uint64_t draws = 0;
    };
    Program programs[kLeanMax];
    int index[2][4][2][2][3];   // as LeanState::index
    int nocolor = -1;
    std::string vertex;         // the skin vertex source as compiled (the library's defines in)
    uint32_t tracked_gen = 0;
    bool tracked = false;
    const GlUniformCache::Shadow* fast[16] = {};
    int fast_value[16] = {};
    int fast_n = 0;
    const GlUniformCache::Shadow* key[kKeyFields] = {};
    const GlUniformCache::Shadow* mirror[kMirrors] = {};
    const GlUniformCache::ArrayShadow* array[kArrayMirrors] = {};
    uint64_t refused[kFallbacks] = {};
    uint64_t built_late = 0;
};

namespace {
GlShaderLibrary::SkinState* g_skin_census = nullptr;

// one skin program: (the skin vertex source, lean variant i) compiled, linked and its locations read; false on failure
// (`st`: the state cache the sampler's set-up binds through -- null at the library's build, which binds directly as the
// lean programs' build does)
// (`build`: GlShaderLibrary::BuildProgram -- the pair compiled, or loaded from the shader cache, as every program; no
// shader object kept or given: the skin programs are pairs of their own, not linked from the lean vertex paths' objects)
bool MakeSkinProgram(GlShaderLibrary::SkinState& ss, int i,
                     uint32_t (*build)(const char*, const char*, const char*, uint32_t*, uint32_t*, uint32_t, uint32_t),
                     void (*lookup)(uint32_t, GlUniformLocations&), GlStateCache* st) {
    GlShaderLibrary::SkinState::Program& p = ss.programs[i];
    p.tried = true;
    const LeanVariant& v = kLeanVariants[i];
    const std::string fs = LeanSource(v);
    char label[96];
    std::snprintf(label, sizeof label, "ps2 skin %d (%s)", i, v.label);
    // (its GXPs kept for the raw skin draw's twin, with the raw mesh draws: TE_GFX_VITA_RAW_MESH)
    const char* raw_env = std::getenv("TE_GFX_VITA_RAW_MESH");
    const bool want_raw = !(raw_env && raw_env[0] == '0');
    g_capture_gxps = want_raw;
    const uint32_t id = build(ss.vertex.c_str(), fs.c_str(), label, nullptr, nullptr, 0, 0);
    g_capture_gxps = false;
    std::fflush(stderr);
    if (want_raw && id) {
        ReleaseRawTwin(g_skin_twins[i].t);
        g_skin_twins[i] = SkinTwin();
        const ProgramGxps* g = GxpsOf(id);
        const bool twin = g && BuildSkinTwin(g_skin_twins[i], *g);
        if (!twin) {
            ReleaseRawTwin(g_skin_twins[i].t);
            g_skin_twins[i] = SkinTwin();
        }
        std::fprintf(stderr, "[gl] raw skin draws: program %s %s\n", label, twin ? "has a GXM twin" : "has no twin (drawn through vitaGL)");
    }
    g_gxps.clear();
    if (!id) return false;
    GlUniformLocations locs;
    lookup(id, locs);
    for (int m = 0; m < kMirrors; ++m) {
        p.mirror_loc[m] = locs.*(kLeanMirrors[m].field);
        p.mirror_sent[m] = GlUniformCache::Shadow();
    }
    for (int m = 0; m < kArrayMirrors; ++m) {
        p.array_loc[m] = locs.*(kLeanArrayMirrors[m].field);
        p.array_count[m] = 0;
    }
    p.at_loc = glGetUniformLocation(id, "u_lean_at");
    p.fail_loc = glGetUniformLocation(id, "u_lean_fail");
    p.fba_loc = glGetUniformLocation(id, "u_lean_fba");
    p.sp.bones = glGetUniformLocation(id, "u_bones");
    p.sp.disp = glGetUniformLocation(id, "u_disp");
    if (st) st->UseProgram(id);
    else glUseProgram(id);
    if (locs.tex_sampler >= 0) glUniform1i(locs.tex_sampler, 0);
    if (st) st->UseProgram(0);
    else glUseProgram(0);
    bool sizes = false;
    for (const char* name : kVitaSamplerSizes)
        sizes = sizes || glGetUniformLocation(id, (std::string("te_size_") + name).c_str()) >= 0;
    if (sizes || p.sp.bones < 0 || p.sp.disp < 0 || p.array_loc[0] < 0) {
        std::fprintf(stderr, "[gl] skin program %d: %s: not used\n", i,
                     sizes ? "it reads a texture size uniform" : "u_bones / u_mesh not found");
        glDeleteProgram(id);
        return false;
    }
    p.sp.id = id;
    return true;
}
}  // namespace

bool GlShaderLibrary::BuildSkinPrograms(const std::string& defines) {
    if (!skin_) skin_ = new SkinState();
    SkinState& ss = *skin_;
    ss = SkinState();
    // the skin twins of an earlier build released, every skin packet void (it names a twin)
    for (SkinTwin& t : g_skin_twins) {
        ReleaseRawTwin(t.t);
        t = SkinTwin();
    }
    ++g_packet_epoch;
    g_skin_packet_last = 0;
    std::memset(ss.index, -1, sizeof ss.index);
    for (int i = 0; i < kLeanMax; ++i) {
        const LeanVariant& v = kLeanVariants[i];
        if (v.nocolor) {
            ss.nocolor = i;
        } else if (v.tex) {
            ss.index[1][v.tfx][v.tcc][v.mip][v.atest] = i;
        } else {
            for (int tfx = 0; tfx < 4; ++tfx)
                for (int tcc = 0; tcc < 2; ++tcc)
                    for (int mip = 0; mip < 2; ++mip) ss.index[0][tfx][tcc][mip][v.atest] = i;
        }
    }
    ss.vertex = GlShaderSources::Ps2SkinVertex();
    if (!defines.empty()) {
        const size_t version = ss.vertex.find("#version");
        const size_t line = version == std::string::npos ? std::string::npos : ss.vertex.find('\n', version);
        if (line != std::string::npos) ss.vertex.insert(line + 1, defines);
    }
    int built = 0;
    for (int i = 0; i < kLeanMax; ++i) {
        const LeanVariant& v = kLeanVariants[i];
        // the textured MODULATE variants and the colourless one (the casters' re-stamp draws skins with colour writes off)
        // now; the untextured ones when a skinned draw first asks for them (all now in the seed run: BuildEverything)
        if (!(v.tex && v.tfx == 0 && v.tcc == 1) && !v.nocolor && !BuildEverything()) continue;
        built += MakeSkinProgram(ss, i, &GlShaderLibrary::BuildProgram, &GlShaderLibrary::LookupLocationsOf, nullptr) ? 1 : 0;
    }
    g_skin_census = &ss;
    std::fprintf(stderr, "[gl] GPU-skinning programs: %d built (the textured MODULATE and the no-colour lean variants; the "
                 "others on first use)\n", built);
    if (!built) {
        g_skin_census = nullptr;
        delete skin_;
        skin_ = nullptr;
        return false;
    }
    return true;
}

const GlShaderLibrary::SkinProgram* GlShaderLibrary::SelectSkin(GlStateCache& st, GlUniformCache& u, const float* box,
                                                               int frame_w, int frame_h, const RawSkin* raw, bool* drew_raw) const {
    (void)box;
    (void)frame_w;
    (void)frame_h;
    if (drew_raw) *drew_raw = false;
    SkinState* ssp = skin_;
    if (!ssp) return nullptr;
    SkinState& ss = *ssp;
    if (ss.tracked_gen != u.Generation()) {
        // the shadows' entries move at a Clear (a relink): tracked again, every program's copies void
        ss.tracked_gen = u.Generation();
        ss.tracked = true;
        int n = 0;
        const FastConstant* fc = FastConstants(&n);
        ss.fast_n = std::min(n, 16);
        for (int i = 0; i < ss.fast_n; ++i) {
            ss.fast[i] = u.Track(loc_.*(fc[i].field));
            ss.fast_value[i] = fc[i].value;
            ss.tracked = ss.tracked && ss.fast[i];
        }
        for (int k = 0; k < kKeyFields; ++k) {
            ss.key[k] = u.Track(loc_.*(kKeySpecs[k].field));
            ss.tracked = ss.tracked && (ss.key[k] || k == kBlend || k == kFog);
        }
        for (int m = 0; m < kMirrors; ++m) {
            const int loc = loc_.*(kLeanMirrors[m].field);
            ss.mirror[m] = loc >= 0 ? u.Track(loc) : nullptr;
            ss.tracked = ss.tracked && (loc < 0 || ss.mirror[m]);
        }
        for (int m = 0; m < kArrayMirrors; ++m) {
            const int loc = loc_.*(kLeanArrayMirrors[m].field);
            ss.array[m] = loc >= 0 ? u.TrackArray(loc) : nullptr;
            ss.tracked = ss.tracked && (loc < 0 || ss.array[m]);
        }
        for (int i = 0; i < kLeanMax; ++i) {
            SkinState::Program& p = ss.programs[i];
            for (int m = 0; m < kMirrors; ++m) p.mirror_sent[m] = GlUniformCache::Shadow();
            for (int m = 0; m < kArrayMirrors; ++m) p.array_count[m] = 0;
            p.at_sent[0] = p.fail_sent[0] = p.fba_sent = -1.f;
        }
        if (!ss.tracked) std::fprintf(stderr, "[gl] GPU-skinning programs: the uniform cache cannot track the full program's uniforms: not used\n");
    }
    auto refuse = [&](int reason) -> const SkinProgram* {
        ss.refused[reason]++;
        return nullptr;
    };
    if (!ss.tracked) return refuse(kFbOff);
    for (int i = 0; i < ss.fast_n; ++i)
        if (ss.fast[i]->kind != 1 || ss.fast[i]->i[0] != ss.fast_value[i]) return refuse(kFbNotFast);
    auto value = [&](int k, int* v) {
        const GlUniformCache::Shadow* s = ss.key[k];
        if (!s || s->kind != (k == kMipCtl ? 5 : 1)) return false;
        *v = s->i[0];
        return true;
    };
    int use_texture = 0, ate = 0, fba = 0;
    if (!value(kUseTexture, &use_texture) || !value(kAte, &ate) || !value(kFba, &fba)) return refuse(kFbUnknown);
    if (ss.key[kFog] && ss.key[kFog]->kind == 1 && ss.key[kFog]->i[0] != 0) return refuse(kFbFog);
    // the alpha test as SelectPs2 reads it
    int atest = 0, atst = 1, aref = 0, afail = 0;
    if (ate) {
        if (!value(kAtst, &atst) || !value(kAref, &aref) || !value(kAfail, &afail)) return refuse(kFbUnknown);
        const bool can_fail = !(atst == 1 || (atst == 5 && aref <= 0) || (atst == 3 && aref >= 255) ||
                                (atst == 7 && (aref < 0 || aref > 255)));
        if (afail != 1 && can_fail) atest = afail == 0 ? 1 : 2;
    }
    const int cmask = st.ColorMaskBits();
    int tfx = 0, tcc = 0, mip = 0;
    int variant = -1;
    if (cmask == 0 && atest != 1) {
        variant = ss.nocolor;
    } else {
        if (use_texture) {
            int lod_hw = 0;
            if (!value(kTfx, &tfx) || !value(kTcc, &tcc) || !value(kLodHw, &lod_hw) || !ss.key[kMipCtl] ||
                ss.key[kMipCtl]->kind != 5)
                return refuse(kFbUnknown);
            const int levels = ss.key[kMipCtl]->i[0], lcm = ss.key[kMipCtl]->i[1];
            mip = levels > 0 ? 1 : 0;
            if (mip && (lcm != 0 || lod_hw != 0)) return refuse(kFbMipMode);
            if (tfx < 0 || tfx > 3 || tcc < 0 || tcc > 1) return refuse(kFbNoVariant);
        }
        variant = ss.index[use_texture ? 1 : 0][tfx][tcc][mip][atest];
    }
    if (variant < 0) return refuse(kFbNoVariant);
    SkinState::Program& p = ss.programs[variant];
    if (!p.sp.id) {
        if (p.tried) return refuse(kFbNoVariant);
        ss.built_late++;
        if (!MakeSkinProgram(ss, variant, &GlShaderLibrary::BuildProgram, &GlShaderLibrary::LookupLocationsOf, &st))
            return refuse(kFbNoVariant);
    }
    if (raw && drew_raw && RawMeshEnabled() && g_skin_twins[variant].t.ok && lean_ && !lean_->key_census) {
        // THE RAW SKIN DRAW: the twin, the values SelectSkin would send the program (the mirrored shadows: u_screen_size,
        // u_uv_scale, u_uv_mode, u_tex_size, u_mip_ctl, u_mip_lodp, u_mesh, u_mesh_light; the alpha test, its fail factor,
        // FBA), the palette and the displacements -- no GL call; the program is not bound (vitaGL's next draw binds its own)
        float lo = 0.f, hi = 255.f, inv = 0.f;
        switch (atst) {
            case 0: lo = 1.f; hi = 0.f; break;
            case 2: hi = float(aref) - 1.f; break;
            case 3: hi = float(aref); break;
            case 4: lo = hi = float(aref); break;
            case 5: lo = float(aref); break;
            case 6: lo = float(aref) + 1.f; break;
            case 7: lo = hi = float(aref); inv = 1.f; break;
            default: break;
        }
        const float fail_a = afail == 3 ? 1.f : 0.f;
        const RawValues v{ss.mirror[0], ss.mirror[1], ss.mirror[5], ss.mirror[6], ss.mirror[7], ss.array[0], ss.array[1],
                          {lo, hi, inv}, {fail_a, fail_a, fail_a, 0.f}, fba ? 1.f : 0.f};
        const int why = RawSkinSubmit(g_skin_twins[variant], v, ss.mirror[2], *raw, variant);
        if (why < 0) {
            p.draws++;
            *drew_raw = true;
            Census().skin_raw++;
            if (g_skin_packet_last) {
                SkinPacket& k = g_skin_packets[g_skin_packet_last & (kSkinPackets - 1)];
                if (k.id == g_skin_packet_last) {
                    k.program = &p;
                    k.variant = variant;
                }
            }
            return &p.sp;
        }
        Census().skin_raw_declined++;
        g_skin_packet_last = 0;
    }
    for (int m = 0; m < kMirrors; ++m) {
        const int loc = p.mirror_loc[m];
        const GlUniformCache::Shadow* s = ss.mirror[m];
        if (loc < 0 || !s || s->kind == 0 || SameShadow(*s, p.mirror_sent[m])) continue;
        SendShadow(loc, *s);
        p.mirror_sent[m] = *s;
    }
    for (int m = 0; m < kArrayMirrors; ++m) {
        const int loc = p.array_loc[m];
        const GlUniformCache::ArrayShadow* a = ss.array[m];
        if (loc < 0 || !a || a->count <= 0 || a->count * 4 > GlUniformCache::kArrayFloats) continue;
        const size_t bytes = size_t(a->count) * 4 * sizeof(float);
        if (p.array_count[m] == a->count && std::memcmp(p.array_sent[m], a->v, bytes) == 0) continue;
        glUniform4fv(loc, a->count, a->v);
        Census().lean_uniform_calls++;
        p.array_count[m] = a->count;
        std::memcpy(p.array_sent[m], a->v, bytes);
    }
    if (p.at_loc >= 0) {
        float lo = 0.f, hi = 255.f, inv = 0.f;
        switch (atst) {
            case 0: lo = 1.f; hi = 0.f; break;
            case 2: hi = float(aref) - 1.f; break;
            case 3: hi = float(aref); break;
            case 4: lo = hi = float(aref); break;
            case 5: lo = float(aref); break;
            case 6: lo = float(aref) + 1.f; break;
            case 7: lo = hi = float(aref); inv = 1.f; break;
            default: break;
        }
        if (p.at_sent[0] != lo || p.at_sent[1] != hi || p.at_sent[2] != inv) {
            glUniform3f(p.at_loc, lo, hi, inv);
            Census().lean_uniform_calls++;
            p.at_sent[0] = lo; p.at_sent[1] = hi; p.at_sent[2] = inv;
        }
    }
    if (p.fail_loc >= 0) {
        const float a = afail == 3 ? 1.f : 0.f;
        if (p.fail_sent[0] != a || p.fail_sent[3] != 0.f) {
            glUniform4f(p.fail_loc, a, a, a, 0.f);
            Census().lean_uniform_calls++;
            p.fail_sent[0] = p.fail_sent[1] = p.fail_sent[2] = a;
            p.fail_sent[3] = 0.f;
        }
    }
    if (p.fba_loc >= 0) {
        const float f = fba ? 1.f : 0.f;
        if (p.fba_sent != f) {
            glUniform1f(p.fba_loc, f);
            Census().lean_uniform_calls++;
            p.fba_sent = f;
        }
    }
    p.draws++;
    FillCensusDraw(kFillSkin + variant, 512.0 * 512.0, 5);   // (a skin's box is not known here: the frame stands for it)
    st.UseProgram(p.sp.id);
    return &p.sp;
}

uint32_t GlShaderLibrary::TakeSkinPacket() const {
    const uint32_t id = g_skin_packet_last;
    g_skin_packet_last = 0;
    return id;
}
void GlShaderLibrary::DropSkinPacket() const { g_skin_packet_last = 0; }

int GlShaderLibrary::DrawSkinPacket(uint32_t id, uint32_t vbo, uint32_t ibo, uint32_t count, const MeshDrawParams& mp,
                                    const float* bones, int joints, const float* disp, int disp_count, int frame_w,
                                    int frame_h) const {
    if (g_packets_mode == 0 || !g_packets_on || !RawMeshEnabled() || !skin_ || !lean_ || lean_->key_census) return 0;
    const SkinPacket& k = g_skin_packets[id & (kSkinPackets - 1)];
    if (!id || k.id != id || !k.program) return 3;
    const int why = SkinReplay(k, vbo, ibo, count, mp, bones, joints, disp, disp_count, frame_w, frame_h);
    if (why >= 0) return why;
    // the census, as SelectSkin counts the draw it replays
    static_cast<SkinState::Program*>(k.program)->draws++;
    return -1;
}
#endif

std::string SkinProgramCensusLines(double frames) {
#if PS2R_HOST_VITA
    GlShaderLibrary::SkinState* ssp = g_skin_census;
    if (!ssp || frames <= 0) return std::string();
    GlShaderLibrary::SkinState& ss = *ssp;
    std::string out = "[te.gfx.vita] GPU-skinned draws per frame by program:";
    char b[160];
    uint64_t all = 0;
    for (int i = 0; i < kLeanMax; ++i) {
        all += ss.programs[i].draws;
        if (!ss.programs[i].draws) continue;
        std::snprintf(b, sizeof b, " %d %s %.1f", i, kLeanVariants[i].label, double(ss.programs[i].draws) / frames);
        out += b;
        ss.programs[i].draws = 0;
    }
    if (!all) out += " none";
    std::string fb;
    for (int r = 0; r < kFallbacks; ++r) {
        if (!ss.refused[r]) continue;
        std::snprintf(b, sizeof b, "%s %s %.1f", fb.empty() ? "" : ";", kFallbackNames[r], double(ss.refused[r]) / frames);
        fb += b;
        ss.refused[r] = 0;
    }
    if (!fb.empty()) out += "; REFUSED (no lean variant):" + fb;
    if (ss.built_late) {
        std::snprintf(b, sizeof b, "; %llu built on first use", (unsigned long long)ss.built_late);
        out += b;
        ss.built_late = 0;
    }
    return out + "\n";
#else
    (void)frames;
    return std::string();
#endif
}

std::string Ps2ProgramCensusLines(double frames) {
#if PS2R_HOST_VITA
    GlShaderLibrary::LeanState* lsp = g_lean_census;
    if (!lsp || frames <= 0) return std::string();
    GlShaderLibrary::LeanState& ls = *lsp;
    const double all = std::max(1.0, ls.all_pixels);
    std::string out = "[te.gfx.vita] PS2 programs per frame (draws, share of the drawn pixels by screen box):";
    char b[200];
    uint64_t lean_draws = 0;
    for (int i = 0; i < kLeanMax; ++i) {
        GlShaderLibrary::LeanState::Program& p = ls.programs[i];
        uint64_t draws = p.draws;
        double pixels = p.pixels;
        for (int k = 0; k < GlShaderLibrary::LeanState::kVsPaths; ++k) {
            draws += ls.paths[k][i].draws;
            pixels += ls.paths[k][i].pixels;
        }
        lean_draws += draws;
        if (!draws) continue;
        std::snprintf(b, sizeof b, " lean %d %s %.1f (%.1f%%)", i, kLeanVariants[i].label, double(draws) / frames,
                      100.0 * pixels / all);
        out += b;
    }
    std::snprintf(b, sizeof b, "; fast %.1f (%.1f%%), full %.1f (%.1f%%); pixels drawn %.2f frames' worth, opaque (no blend, "
                  "no discard) %.1f%%; uniforms sent to lean programs %.1f (%.1f draws)\n",
                  double(ls.fast_draws) / frames, 100.0 * ls.fast_pixels / all, double(ls.full_draws) / frames,
                  100.0 * ls.full_pixels / all, ls.all_pixels / frames / (512.0 * 512.0), 100.0 * ls.opaque_pixels / all,
                  double(Census().lean_uniform_calls) / frames, double(ls.sync_draws) / frames);
    out += b;
    Census().lean_uniform_calls = 0;
    std::string fb;
    for (int r = 0; r < kFallbacks; ++r) {
        if (!ls.fallback[r]) continue;
        std::snprintf(b, sizeof b, "%s %s %.1f (%.1f%%)", fb.empty() ? "" : ";", kFallbackNames[r],
                      double(ls.fallback[r]) / frames, 100.0 * ls.fallback_pixels[r] / all);
        fb += b;
    }
    if (!fb.empty()) out += "[te.gfx.vita] PS2 program fallbacks per frame:" + fb + "\n";
    if (ls.paths_built) {
        std::snprintf(b, sizeof b, "[te.gfx.vita] lean vertex paths per frame: mesh %.1f, lit mesh %.1f, vertices %.1f, raw %.1f, "
                      "the generic vertex program %.1f (of which no path program %.1f)\n",
                      double(ls.path_draws[1]) / frames, double(ls.path_draws[2]) / frames, double(ls.path_draws[3]) / frames,
                      double(ls.path_draws[4]) / frames, double(ls.path_draws[0]) / frames, double(ls.path_misses) / frames);
        out += b;
    }
    for (auto& n : ls.path_draws) n = 0;
    ls.path_misses = 0;
    {
        GlCensus& c = Census();
        uint64_t declined = 0;
        for (uint64_t d : c.raw_mesh_declined) declined += d;
        if (c.draws_raw_mesh || declined) {
            char rb[320];
            std::snprintf(rb, sizeof rb, "[te.gfx.vita] raw mesh draws per frame: %.1f (declined: off %.1f, not a mesh-path program "
                          "%.1f, no twin %.1f, a value not known %.1f, length %.1f, texture record %.1f)\n",
                          double(c.draws_raw_mesh) / frames, double(c.raw_mesh_declined[0]) / frames,
                          double(c.raw_mesh_declined[1]) / frames, double(c.raw_mesh_declined[2]) / frames,
                          double(c.raw_mesh_declined[3]) / frames, double(c.raw_mesh_declined[4]) / frames,
                          double(c.raw_mesh_declined[5]) / frames);
            out += rb;
        }
        c.draws_raw_mesh = 0;
        for (uint64_t& d : c.raw_mesh_declined) d = 0;
    }
    {
        // the raw vertex draws (RawVertexSubmit): vitaGL's glDrawArrays replaced, and why one was not
        uint64_t declined = 0;
        for (uint64_t d : g_rv.declined) declined += d;
        if (g_rv.drawn || declined) {
            char rb[360];
            std::snprintf(rb, sizeof rb, "[te.gfx.vita] raw vertex draws per frame (TE_GFX_VITA_RAW_VERTS): %.1f drawn without vitaGL's "
                          "glDrawArrays (+%.1f runs past its index buffer); declined (drawn through vitaGL): off %.1f, not the raw "
                          "vertex path %.1f, no twin %.1f, a value not known %.1f, the ring %.1f, texture record %.1f\n",
                          double(g_rv.drawn) / frames, double(g_rv.runs) / frames, double(g_rv.declined[0]) / frames,
                          double(g_rv.declined[1]) / frames, double(g_rv.declined[2]) / frames, double(g_rv.declined[3]) / frames,
                          double(g_rv.declined[4]) / frames, double(g_rv.declined[5]) / frames);
            out += rb;
        }
        g_rv = RawVertsCensus{};
    }
    if (ls.key_census) {
        // the most-drawn state keys by pixels
        std::vector<GlShaderLibrary::LeanState::KeyCount> keys;
        for (const auto& k : ls.keys)
            if (k.key) keys.push_back(k);
        std::sort(keys.begin(), keys.end(), [](const GlShaderLibrary::LeanState::KeyCount& a, const GlShaderLibrary::LeanState::KeyCount& b) { return a.pixels > b.pixels; });
        out += "[te.gfx.vita] PS2 state census (tex tfx tcc mip | test class/atst/afail/aref | fba blend cmask -> program: draws/frame, pixel share):";
        for (size_t i = 0; i < keys.size() && i < 24; ++i) {
            const uint64_t k = keys[i].key;
            std::snprintf(b, sizeof b, " [t%d f%d c%d m%d | a%d/%d/%d/%d | fba%d bl%d cm%X -> %d: %.1f %.1f%%]", int(k & 1),
                          int(k >> 1 & 3), int(k >> 3 & 1), int(k >> 4 & 1), int(k >> 5 & 3), int(k >> 7 & 15),
                          int(k >> 11 & 3), int(k >> 19 & 0x1FF), int(k >> 13 & 1), int(k >> 14 & 1), int(k >> 15 & 15),
                          keys[i].program, double(keys[i].draws) / frames, 100.0 * keys[i].pixels / all);
            out += b;
        }
        out += "\n";
        for (auto& k : ls.keys) k = GlShaderLibrary::LeanState::KeyCount();
    }
    for (int i = 0; i < kLeanMax; ++i) {
        ls.programs[i].draws = ls.programs[i].draws_opaque = 0;
        ls.programs[i].pixels = 0;
        for (int k = 0; k < GlShaderLibrary::LeanState::kVsPaths; ++k) {
            ls.paths[k][i].draws = ls.paths[k][i].draws_opaque = 0;
            ls.paths[k][i].pixels = 0;
        }
    }
    for (int r = 0; r < kFallbacks; ++r) { ls.fallback[r] = 0; ls.fallback_pixels[r] = 0; }
    ls.fast_draws = ls.full_draws = 0;
    ls.fast_pixels = ls.full_pixels = ls.all_pixels = ls.opaque_pixels = 0;
    ls.sync_draws = 0;
    return out;
#else
    (void)frames;
    return std::string();
#endif
}

std::string GlShaderLibrary::DepthDefines() const {
    std::string d;
    if (zero_to_one_depth_) d += "#define ZERO_TO_ONE_DEPTH 1\n";
    if (gs_z24_depth_) d += "#define GS_Z24_DEPTH 1\n";
    return d;
}

bool GlShaderLibrary::Build(GlUniformCache& uniforms) {
    Destroy();
    std::string vertex = VertexSource();
    std::string defines = DepthDefines();
#if PS2R_HOST_VITA
    // the models' transform on the GPU: the Vita's CPU cannot afford it per vertex
    native_mesh_ = true;
    defines += "#define NATIVE_MESH 1\n";
#endif
    if (!defines.empty()) {
        const size_t version = vertex.find("#version");
        const size_t line = version == std::string::npos ? std::string::npos : vertex.find('\n', version);
        if (line != std::string::npos) vertex.insert(line + 1, defines);
    }
#if PS2R_HOST_VITA
    const char* lean_env = std::getenv("TE_GFX_LEAN_SAMPLERS");
    const bool lean = !(lean_env && lean_env[0] == '0');
    const std::string fragment = lean ? VitaLeanPs2Fragment(FragmentSource()) : std::string(FragmentSource());
    std::fprintf(stderr, "[gl] PS2 fragment program: %s\n", lean ? "one sampler (the four unused aliased to u_texture)" : "five samplers");
    ps2_program_ = BuildProgram(vertex.c_str(), fragment.c_str(), "ps2");
    // the fast program (TE_GFX_VITA_PS2_FAST: unset / 1 on, 0 = not built, abN = built and switched on / off every N
    // presents: GlDevice::Present)
    {
        const char* fast_env = std::getenv("TE_GFX_VITA_PS2_FAST");
        const bool want_fast = ps2_program_ && !(fast_env && fast_env[0] == '0');
        const std::string fast = want_fast ? VitaFastPs2Fragment(fragment) : std::string();
        if (want_fast && fast.empty())
            std::fprintf(stderr, "[gl] the PS2 fast program: the fragment source lacks a uniform it specializes: not built\n");
        ps2_fast_program_ = fast.empty() ? 0 : BuildProgram(vertex.c_str(), fast.c_str(), "ps2 fast");
    }
    if (ps2_program_) BuildLeanPrograms(vertex.c_str());
    // the GPU-skinning programs over the lean fragment variants (TE_GFX_GPU_SKIN=0: not built, the skins CPU-built)
    if (ps2_program_ && lean_count_ > 0) {
        const char* skin_env = std::getenv("TE_GFX_GPU_SKIN");
        if (skin_env && skin_env[0] == '0') std::fprintf(stderr, "[gl] GPU-skinning programs: off (TE_GFX_GPU_SKIN=0)\n");
        else BuildSkinPrograms(DepthDefines());
    }
#else
    ps2_program_ = BuildProgram(vertex.c_str(), FragmentSource(), "ps2");
#endif
    if (!ps2_program_) return false;
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif

    scanout_program_ = BuildProgram(ScanoutVertexSource(), ScanoutFragmentSource(), "scanout");
    if (!scanout_program_) return false;
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    scanout_tex_loc_ = glGetUniformLocation(scanout_program_, "u_texture");
    scanout_dither_program_ = BuildProgram(ScanoutVertexSource(), ScanoutDitherFragmentSource(), "scanout dither");
    if (!scanout_dither_program_) return false;
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    scanout_dither_tex_loc_ = glGetUniformLocation(scanout_dither_program_, "u_texture");
    scanout_dither_size_loc_ = glGetUniformLocation(scanout_dither_program_, "u_source_size");
    scanout_dither_dimx_loc_ = glGetUniformLocation(scanout_dither_program_, "u_dimx");
    if (!BuildStencilPrograms()) return false;
#if PS2R_HOST_VITA
    std::fflush(stderr);
    {
        // the screen copy's program (GlDevice::CopyRect); a failure leaves 0 = the glCopyTexSubImage2D copy
        copy_program_ = BuildProgram(GlShaderSources::CopyVertex(), GlShaderSources::CopyFragment(), "screen copy");
        if (copy_program_) {
            copy_src_loc_ = glGetUniformLocation(copy_program_, "u_src");
            glUseProgram(copy_program_);
            const GLint t = glGetUniformLocation(copy_program_, "u_texture");
            if (t >= 0) glUniform1i(t, 0);
            glUseProgram(0);
        }
        if (!copy_program_ || copy_src_loc_ < 0)
            std::fprintf(stderr, "[gl] the screen-copy program failed to build: copies go through glCopyTexSubImage2D\n");
    }
    {
        // the movie pictures' YUV program (GlDevice::DrawMoviePicture; an RGBA picture draws with the copy program); a
        // failure leaves 0 = no YUV movie textures (the movie player keeps RGBA pictures)
        movie_yuv_program_ = BuildProgram(GlShaderSources::CopyVertex(), kMovieYuvFragSrc, "movie yuv");
        if (movie_yuv_program_) {
            movie_yuv_src_loc_ = glGetUniformLocation(movie_yuv_program_, "u_src");
            movie_yuv_size_loc_ = glGetUniformLocation(movie_yuv_program_, "u_luma_size");
            glUseProgram(movie_yuv_program_);
            const char* const planes[3] = {"u_plane_y", "u_plane_u", "u_plane_v"};
            for (int i = 0; i < 3; ++i) {
                const GLint t = glGetUniformLocation(movie_yuv_program_, planes[i]);
                if (t >= 0) glUniform1i(t, i);
            }
            glUseProgram(0);
        }
        if (!movie_yuv_program_ || movie_yuv_src_loc_ < 0 || movie_yuv_size_loc_ < 0) {
            std::fprintf(stderr, "[gl] the movie YUV program failed to build: movie pictures stay RGBA\n");
            if (movie_yuv_program_) glDeleteProgram(movie_yuv_program_);
            movie_yuv_program_ = 0;
        }
    }
#endif

    LookupLocations();
#if PS2R_HOST_VITA
    // the fast program's uniforms mirror the full program's: every value the uniform cache sends goes to both (vitaGL's
    // glUniform* writes the program the location belongs to, bound or not), so either program holds the draw's values
    uniforms.SetMirror({});
    LookupLocationsOf(ps2_fast_program_, loc_fast_);
    if (ps2_fast_program_) {
        // the fast program must not read the texture sizes the state cache sends per bound program (only the full one is
        // bound when they are sent): none of its live code does -- checked, else it is not used
        bool sizes = false;
        for (const char* name : kVitaSamplerSizes)
            sizes = sizes || glGetUniformLocation(ps2_fast_program_, (std::string("te_size_") + name).c_str()) >= 0;
        int n = 0;
        const FastConstant* fc = FastConstants(&n);
        bool constants_live = true;
        for (int i = 0; i < n; ++i) constants_live = constants_live && (loc_.*(fc[i].field)) >= 0;
        if (sizes || !constants_live) {
            std::fprintf(stderr, "[gl] the PS2 fast program is not used (%s)\n",
                         sizes ? "it reads a texture size uniform" : "a uniform it specializes is not in the full program");
            glDeleteProgram(ps2_fast_program_);
            ps2_fast_program_ = 0;
            loc_fast_ = GlUniformLocations{};
        } else {
            static_assert(sizeof(GlUniformLocations) % sizeof(int) == 0, "the location table is ints");
            constexpr size_t kInts = sizeof(GlUniformLocations) / sizeof(int);
            int full[kInts], fast[kInts];
            std::memcpy(full, &loc_, sizeof full);
            std::memcpy(fast, &loc_fast_, sizeof fast);
            std::vector<std::pair<int, int>> pairs;
            for (size_t i = 0; i < kInts; ++i)
                if (full[i] >= 0 && fast[i] >= 0) pairs.emplace_back(full[i], fast[i]);
            uniforms.SetMirror(pairs);
            std::fprintf(stderr, "[gl] the PS2 fast program: %u uniforms mirrored; used by a draw while %d uniforms hold "
                         "their constants (TE_GFX_VITA_PS2_FAST=0: off)\n", unsigned(pairs.size()), n);
        }
    }
#endif
    PrimeUniforms(uniforms);
#if PS2R_HOST_VITA
    std::fprintf(stderr, "[gl] shader cache (%s): %u programs loaded (%u of them from the shipped cache %s), %u compiled (%u stored), "
                 "%.0f ms for all\n", CacheDir() ? CacheDir() : "off", g_cache_loaded, g_cache_shipped,
                 SeedDir() ? SeedDir() : "(none)", g_cache_compiled, g_cache_stored, g_cache_ms);
    std::fflush(stderr);
#endif
    return true;
}

// The stencil-composite programs: strip-count and invert fragments over the PS2
// vertex program (their sampler units bound once), the constant quad pair, and the
// opaque-alpha stencil mark over the constant vertex (its frame sampler on unit 0).
bool GlShaderLibrary::BuildStencilPrograms() {
    struct Prog { const char* vs; const char* fs; uint32_t* out; const char* label; };
    const Prog progs[4] = {
        {VertexSource(), GlShaderSources::StencilStripFragment(), &strip_program_, "stencil strip"},
        {VertexSource(), GlShaderSources::StencilInvertFragment(), &invert_program_, "stencil invert"},
        {GlShaderSources::ConstVertex(), GlShaderSources::ConstFragment(), &const_program_, "constant"},
        {GlShaderSources::ConstVertex(), GlShaderSources::OpaqueMaskFragment(), &opaque_mask_program_, "opaque mask"},
    };
    for (const Prog& p : progs) {
        *p.out = BuildProgram(p.vs, p.fs, p.label);
        if (!*p.out) return false;
    }
    sloc_.strip_screen = glGetUniformLocation(strip_program_, "u_screen_size");
    sloc_.strip_zsrc_en = glGetUniformLocation(strip_program_, "u_zsrc_en");
    sloc_.strip_datm = glGetUniformLocation(strip_program_, "u_datm");
    sloc_.strip_zsrc_num = glGetUniformLocation(strip_program_, "u_zsrc_num");
    sloc_.strip_zsrc_den = glGetUniformLocation(strip_program_, "u_zsrc_den");
    glUseProgram(strip_program_);
    const GLint rt_s = glGetUniformLocation(strip_program_, "u_rt_texture");
    const GLint zs_s = glGetUniformLocation(strip_program_, "u_zsrc_tex");
    if (rt_s >= 0) glUniform1i(rt_s, 1);
    if (zs_s >= 0) glUniform1i(zs_s, 4);
    sloc_.invert_screen = glGetUniformLocation(invert_program_, "u_screen_size");
    sloc_.invert_fix = glGetUniformLocation(invert_program_, "u_fix_alpha");
    glUseProgram(invert_program_);
    const GLint rt_i = glGetUniformLocation(invert_program_, "u_rt_texture");
    if (rt_i >= 0) glUniform1i(rt_i, 1);
    sloc_.const_color = glGetUniformLocation(const_program_, "u_color");
    glUseProgram(opaque_mask_program_);
    const GLint frame_s = glGetUniformLocation(opaque_mask_program_, "u_frame");
    if (frame_s >= 0) glUniform1i(frame_s, 0);
    glUseProgram(0);
    return true;
}

void GlShaderLibrary::Destroy() {
    if (ps2_program_) { glDeleteProgram(ps2_program_); ps2_program_ = 0; }
    if (scanout_program_) { glDeleteProgram(scanout_program_); scanout_program_ = 0; }
    if (scanout_dither_program_) { glDeleteProgram(scanout_dither_program_); scanout_dither_program_ = 0; }
    if (strip_program_) { glDeleteProgram(strip_program_); strip_program_ = 0; }
    if (invert_program_) { glDeleteProgram(invert_program_); invert_program_ = 0; }
    if (const_program_) { glDeleteProgram(const_program_); const_program_ = 0; }
    if (opaque_mask_program_) { glDeleteProgram(opaque_mask_program_); opaque_mask_program_ = 0; }
    if (copy_program_) { glDeleteProgram(copy_program_); copy_program_ = 0; }
    if (movie_yuv_program_) { glDeleteProgram(movie_yuv_program_); movie_yuv_program_ = 0; }
    if (ps2_fast_program_) { glDeleteProgram(ps2_fast_program_); ps2_fast_program_ = 0; }
#if PS2R_HOST_VITA
    if (lean_) {
        for (auto& row : g_raw_twins)
            for (RawTwin& t : row) ReleaseRawTwin(t);
        raw_mesh_ = false;
        for (RawTwin& t : g_vert_twins) ReleaseRawTwin(t);
        raw_verts_ = false;
        ++g_packet_epoch;   // (the packets name the twins released above)
        g_packet_last = 0;
        g_frag_last_buf = nullptr;
        g_vert_last_buf = nullptr;
        g_vert_last_prog = nullptr;
        g_vert_spare_prog = nullptr;
        for (auto& p : lean_->programs)
            if (p.id) glDeleteProgram(p.id);
        for (auto& row : lean_->paths)
            for (auto& p : row)
                if (p.id) glDeleteProgram(p.id);
        if (g_lean_census == lean_) g_lean_census = nullptr;
        delete lean_;
        lean_ = nullptr;
    }
    if (skin_) {
        for (auto& p : skin_->programs)
            if (p.sp.id) glDeleteProgram(p.sp.id);
        if (g_skin_census == skin_) g_skin_census = nullptr;
        delete skin_;
        skin_ = nullptr;
    }
#endif
    lean_count_ = 0;
    loc_fast_ = GlUniformLocations{};
    copy_src_loc_ = -1;
    sloc_ = GlStencilLocations{};
    loc_ = GlUniformLocations{};
    scanout_tex_loc_ = -1;
    scanout_dither_tex_loc_ = scanout_dither_size_loc_ = scanout_dither_dimx_loc_ = -1;
}

void GlShaderLibrary::LookupLocations() { LookupLocationsOf(ps2_program_, loc_); }

void GlShaderLibrary::LookupLocationsOf(uint32_t program, GlUniformLocations& loc_) {
    loc_ = GlUniformLocations{};
    if (!program) return;
    const GLuint p = program;
    auto L = [p](const char* name) { return glGetUniformLocation(p, name); };
    loc_.screen_size = L("u_screen_size");
    loc_.use_texture = L("u_use_texture");
    loc_.tex_sampler = L("u_texture");
    loc_.blend_a = L("u_blend_a");
    loc_.blend_b = L("u_blend_b");
    loc_.blend_c = L("u_blend_c");
    loc_.blend_d = L("u_blend_d");
    loc_.fix_alpha = L("u_fix_alpha");
    loc_.blend_enabled = L("u_blend_enabled");
    loc_.tfx = L("u_tfx");
    loc_.tcc = L("u_tcc");
    loc_.fba = L("u_fba");
    loc_.colclamp = L("u_colclamp");
    loc_.scanmsk = L("u_scanmsk");
    loc_.zte = L("u_zte");
    loc_.ztst = L("u_ztst");
    loc_.ate = L("u_ate");
    loc_.atst = L("u_atst");
    loc_.aref = L("u_aref");
    loc_.afail = L("u_afail");
    loc_.date = L("u_date");
    loc_.datm = L("u_datm");
    loc_.fog_enabled = L("u_fog_enabled");
    loc_.fogcol = L("u_fogcol");
    loc_.tex_size = L("u_tex_size");
    loc_.uv_scale = L("u_uv_scale");
    loc_.uv_mode = L("u_uv_mode");
    loc_.native = L("u_native");
    loc_.mesh_lit = L("u_mesh_lit");
    loc_.mesh = L("u_mesh");
    loc_.mesh_light = L("u_mesh_light");
    loc_.sw_blend = L("u_sw_blend");
    loc_.rt_sampler = L("u_rt_texture");
    loc_.flip_v = L("u_flip_v");
    loc_.fbmsk16_en = L("u_fbmsk16_en");
    loc_.fbmsk16 = L("u_fbmsk16");
    loc_.depth_fmt = L("u_depth_fmt");
    loc_.depth_zoff = L("u_depth_zoff");
    loc_.idx_fmt = L("u_idx_fmt");
    loc_.idx_self = L("u_idx_self");
    loc_.draw_scale = L("u_draw_scale");
    loc_.clut_sampler = L("u_clut");
    loc_.idx_tex2_sampler = L("u_idx_tex2");
    loc_.fb16 = L("u_fb16");
    loc_.ct16q = L("u_ct16q");
    loc_.uv_native = L("u_uv_native");
    loc_.dthe = L("u_dthe");
    loc_.dimx_lo = L("u_dimx_lo");
    loc_.dimx_hi = L("u_dimx_hi");
    loc_.zsrc_en = L("u_zsrc_en");
    loc_.zsrc_sampler = L("u_zsrc_tex");
    loc_.zsrc_num = L("u_zsrc_num");
    loc_.zsrc_den = L("u_zsrc_den");
    loc_.clut_raw = L("u_clut_raw");
    loc_.mip_ctl = L("u_mip_ctl");
    loc_.mip_lodp = L("u_mip_lodp");
    loc_.tex_lod_hw = L("u_tex_lod_hw");
}

// A fresh program object has every uniform at its GL default; the value shadow is
// dropped BEFORE priming so it can never serve a pre-relink value. Units 2/3/4 must
// hold valid textures on every draw (macOS GL returns black for all fetches when a
// statically referenced sampler's unit is incomplete) -- the device binds its dummy
// texture there; the arms are primed OFF.
void GlShaderLibrary::PrimeUniforms(GlUniformCache& uniforms) {
    uniforms.Clear();
    glUseProgram(ps2_program_);
    uniforms.Set1i(loc_.tex_sampler, 0);
    uniforms.Set1i(loc_.rt_sampler, 1);
    uniforms.Set1i(loc_.clut_sampler, 2);
    uniforms.Set1i(loc_.idx_tex2_sampler, 3);
    uniforms.Set1i(loc_.zsrc_sampler, 4);
    uniforms.Set1i(loc_.zsrc_en, 0);
    uniforms.Set1i(loc_.zsrc_num, 1);
    uniforms.Set1i(loc_.zsrc_den, 1);
    uniforms.Set1i(loc_.fb16, 0);
    uniforms.Set1i(loc_.ct16q, 0);
    uniforms.Set1i(loc_.dthe, 0);
    uniforms.Set1i(loc_.clut_raw, 0);
    uniforms.Set1i(loc_.uv_mode, 0);
    if (loc_.native >= 0) uniforms.Set1i(loc_.native, 0);
    uniforms.Set1i(loc_.tex_lod_hw, 0);
    uniforms.Set2f(loc_.uv_scale, 1.0f, 1.0f);
    glUseProgram(0);
}

}  // namespace render
}  // namespace ps2r
