// platform/nativeee (wasm side) -- the graphics RESOURCES of Platform::Graphics that must run inside the module: they read
// the game's virtual Stream, allocate from the game's heap and disk manager and take references through the game's
// graphics tables (the "reverse leak": the PS2 renderer was compiled against the game's
// headers; so is this file). The host never calls game code. What crosses to the host is typed: a texture's image bytes
// (ps2e_gfx2_texture_loaded) and the frame's reset (ps2e_gfx2_frame_reset). The objects' layouts are
// platform/nativeee/te_gfx_layout.h (shared with the host, which reads materials from guest memory when it draws).
//
// Listed in tools/wasm_side_functions.txt (gen_seam.py emits no import or forwarder for these):
//   NewTexture DeleteTexture ReadTexture NewMaterial DeleteMaterial ReadMaterial ConstructMaterial DestroyMaterial
//   MakeFlatMaterial FlatMaterial SetFontPage MakeParticleMaterial MakeDistortionMaterial MakeSkidMaterial
//   AllocFrameMemory ResetBuckets SubmitBuckets
#include "common.h"
#include "gcc2.h"
#include "game/clock.h"
#include "game/disk.h"
#include "game/math.h"
#include "game/graphicstables.h"
#include "game/memory.h"
#include "game/particles.h"
#include "game/resources.h"
#include "game/shaderanimation.h"
#include "game/stream.h"
#include "platform/graphics.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"

static_assert(sizeof(te_gfx_material) <= Platform::Graphics::MaterialStorage, "a material fits OLEG's storage");
static_assert(sizeof(te_gfx_texture) == 0x6C, "GameTexture keeps the PS2's size (the game sizes nothing by it, but the tables do)");
static_assert(sizeof(te_gfx_texture_header) == 0x60, "the file's Texture header is 0x60 bytes");

namespace
{
constexpr u32 TextureLeftoverSize = 0x20;
constexpr s32 NoHandle = -1;
// The PS2 renderer let a resource's packet go two rounds of releases later (renderer.h DeferRelease); kept
constexpr u32 DeferRelease = 1;
constexpr u32 NoTexture = 0;
constexpr u32 ShaderAnimationSize = sizeof(ShaderAnimation);

// The host told before the platform's own structures (materials, shaders, textures, models) change: the seam epoch moves
// (platform/nativeee/host/te_seam_epoch.h), so no answer the host verified against their old bytes is taken again unchecked
inline void ResourcesChanging() { ps2e_gfx2_resources_changed(); }

// The UI's flat material (MakeFlatMaterial), static like the PS2's g_FlatMaterial
te_gfx_material g_FlatMaterial;
te_gfx_shader g_FlatShader;

// AllocFrameMemory's arena. The PS2 took the frame's memory from its first frame chain's DMA buffer (buckets.cpp
// AllocDmaTags: FrameChainCapacity 0x7530 quadwords = 480,000 bytes, shared with the sky bucket's packets), carved from the
// renderer's 0x540000-byte block of the game's heap (graphics.cpp StartRenderer). The port has no DMA block, so the arena
// is ONE heap allocation of that chain's size, made at the first frame (HeapAlloc: the big-block part, where the PS2's
// block lived -- not the small pages the game's objects share), reset when the frame's buckets start over (ResetBuckets).
// Overflow -- the PS2 would have overrun its chain -- takes heap memory that the next reset frees: nothing leaks per
// frame. (The scene needed ~77 KB a frame; a 64 KB .bss arena with a leaking fallback exhausted the small pages in
// ~100 frames of the title's world: NULL pool arrays, then the instance traps.)
constexpr u32 FrameArenaSize = 0x7530 * 16;
u8* g_FrameArena;
u32 g_FrameArenaUsed;
u32 g_FrameArenaOverflows;
u32 g_FrameArenaPeak;
struct FrameOverflow
{
    void* memory;
    FrameOverflow* next;
};
FrameOverflow* g_FrameOverflows;

te_gfx_material* M(Material* material) { return reinterpret_cast<te_gfx_material*>(material); }
te_gfx_shader* S(u32 shader) { return reinterpret_cast<te_gfx_shader*>(shader); }
te_gfx_texture* T(GameTexture* texture) { return reinterpret_cast<te_gfx_texture*>(texture); }

u8 ReadSettingField(Stream* stream)
{
    s8 value;
    stream->ReadS8(&value);
    return static_cast<u8>(value);
}

void ConstructShader(te_gfx_shader* shader, u32 type)
{
    ResourcesChanging();
    *shader = te_gfx_shader{};
    shader->magic = TE_GFX_SHADER_MAGIC;
    shader->type = type;
    shader->colour[0] = shader->colour[1] = shader->colour[2] = shader->colour[3] = 0x80;
    for (f32& value : shader->shaderColour)
    {
        value = 1.0f;
    }
}

void SetColourBytes(te_gfx_shader* shader)
{
    // SetShaderColourByte: the colour fractions as bytes, 1.0 = 0x80
    for (u32 channel = 0; channel < 4; channel++)
    {
        f32 value = shader->shaderColour[channel] * 128.0f;
        s32 byte = static_cast<s32>(value);
        shader->colour[channel] = static_cast<u8>(byte < 0 ? 0 : byte > 255 ? 255 : byte);
    }
}

// ReadBasePrecompShader_: the settings a byte a field (the union's bit order), then TEX1's K and L, the leftover, the
// colour, the scrolls, the texture ID and the type, the animation when it has one, the texture taken from the table
void ReadShaderBase(te_gfx_shader* shader, Stream* stream)
{
    u64 v = 0;
    auto field = [&](u32 shift, u32 width)
    {
        u64 value = ReadSettingField(stream) & ((1ull << width) - 1);
        v |= value << shift;
    };
    field(0, 1);   // blends
    field(1, 4);   // preset
    field(5, 1);   // alphaTest
    field(6, 3);   // alphaMethod
    field(9, 8);   // alphaReference
    field(17, 2);  // alphaFail
    field(19, 1);  // destinationTest
    field(20, 1);  // destinationMode
    field(21, 2);  // depthTest
    field(23, 3);  // unused23
    field(26, 1);  // gouraud
    field(27, 1);  // textured
    field(28, 1);  // stq
    field(29, 1);  // fog
    field(30, 1);  // secondContext
    field(32, 3);  // uScroll
    field(35, 3);  // vScroll
    field(38, 1);  // ownAlpha
    field(39, 2);  // alphaA
    field(41, 2);  // alphaB
    field(43, 2);  // alphaC
    field(45, 2);  // alphaD
    field(47, 8);  // alphaFix
    field(55, 1);  // linear
    field(56, 1);  // noFba
    field(57, 1);  // unused57
    field(58, 1);  // antiAliased
    field(59, 1);  // noDepthWrites
    field(60, 1);  // animatedColour
    field(61, 1);  // hasAnimation
    shader->settings = v;
    stream->ReadU16(reinterpret_cast<u16*>(&shader->lodK));
    stream->ReadU16(reinterpret_cast<u16*>(&shader->lodL));
    // the tools' 16 leftover bytes: the first 8 are the host's scroll payload (zero until StepAnimations), the rest kept
    stream->Read(shader->scroll, sizeof(shader->scroll) + sizeof(shader->leftover), 1);
    shader->scroll[0] = shader->scroll[1] = 0.0f;
    stream->Read(shader->shaderColour, sizeof(shader->shaderColour), 1);
    stream->ReadF32(&shader->scrollPhases[0]);
    stream->ReadF32(&shader->scrollPhases[1]);
    stream->ReadF32(&shader->scrollSpeeds[0]);
    stream->ReadF32(&shader->scrollSpeeds[1]);
    stream->ReadS32(reinterpret_cast<s32*>(&shader->textureId));
    s32 type;
    stream->ReadS32(&type);
    shader->type = static_cast<u32>(type);
    if (TE_GFX_S_HAS_ANIMATION(shader->settings))
    {
        auto* animation = ConstructShaderAnimation(static_cast<ShaderAnimation*>(MemoryAllocate(ShaderAnimationSize)));
        shader->animation = reinterpret_cast<u32>(animation);
        ReadShaderAnimation(animation, stream);
    }

    u32 id = shader->textureId;
    if (id != NoTexture)
    {
        shader->texture = reinterpret_cast<u32>(g_TextureTable.Acquire(&id, nullptr));
    }

    SetColourBytes(shader);
}

// The type's reader: what each type reads BEFORE the base (shaderclasses.cpp ShaderTypeXXRead)
void ReadShaderOfType(te_gfx_shader* shader, u32 type, Stream* stream)
{
    switch (type)
    {
    case 0x10:
    case 0x11:
    case 0x18:
        stream->ReadF32(&shader->extra[0]);   // cornerValue
        break;
    case 0x17:
        stream->ReadS32(&shader->mode);
        stream->ReadF32(&shader->extra[0]);   // speed
        stream->ReadF32(&shader->extra[1]);   // amplitude
        break;
    case 0x1A:
        stream->ReadS32(&shader->mode);
        stream->ReadF32(&shader->extra[0]);   // speed
        stream->ReadF32(&shader->extra[1]);   // amplitudes[0..2]
        stream->ReadF32(&shader->extra[2]);
        stream->ReadF32(&shader->extra[3]);
        break;
    case 0x1C:
        stream->ReadF32(&shader->extra[0]);   // speed
        stream->ReadF32(&shader->extra[1]);   // amplitude
        break;
    default:
        break;
    }

    ReadShaderBase(shader, stream);
}

void DestroyShader(te_gfx_shader* shader)
{
    ResourcesChanging();
    if (shader->texture != 0)
    {
        ReleaseTexture(reinterpret_cast<GameTexture*>(shader->texture));
        shader->texture = 0;
    }

    if (shader->animation != 0)
    {
        // The animation's frames block and the animation (ShaderAnimation's destructor frees both)
        auto* animation = reinterpret_cast<ShaderAnimation*>(shader->animation);
        if (animation->frames != nullptr)
        {
            MemoryDeallocate2_(animation->frames);
        }

        MemoryDeallocate2_(animation);
        shader->animation = 0;
    }

    shader->magic = 0;
}

void ConstructMaterialAt(te_gfx_material* material, u32 kind, u32 bucket)
{
    ResourcesChanging();
    *material = te_gfx_material{};
    material->magic = TE_GFX_MATERIAL_MAGIC;
    material->kind = kind;
    material->bucket = bucket;
    material->ownsShaders = 1;
}

// The flat (UI) shader's settings: SetFlatSettings of the PS2 arm -- blended with the mix preset, no tests, Gouraud,
// untextured, depth not written (the depth test ALWAYS: the UI draws over the frame)
u64 FlatSettings()
{
    u64 v = 0;
    v |= 1ull << 0;                      // blends
    v |= u64{TE_GFX_PRESET_MIX} << 1;    // preset
    v |= 1ull << 21;                     // depthTest ALWAYS (ZTST 1)
    v |= 1ull << 26;                     // gouraud
    v |= 1ull << 59;                     // noDepthWrites
    return v;
}

te_gfx_shader* NewShader(u32 type)
{
    auto* shader = static_cast<te_gfx_shader*>(MemoryAllocate(sizeof(te_gfx_shader)));
    ConstructShader(shader, type);
    return shader;
}
}

namespace Platform::Graphics
{
// ---- textures -------------------------------------------------------------------------------------------------------
GameTexture* NewTexture(u32 id)
{
    ResourcesChanging();
    auto* texture = static_cast<te_gfx_texture*>(MemoryAllocate(sizeof(te_gfx_texture)));
    ConstructResourceHeader(texture, id);
    texture->size = 0;
    texture->texture = te_gfx_texture_header{};
    texture->texture.unused02 = 2;
    texture->texture.transfer = NoHandle;
    texture->texture.hostHandle = 0;
    return reinterpret_cast<GameTexture*>(texture);
}

void DeleteTexture(GameTexture* texture)
{
    ResourcesChanging();
    te_gfx_texture* t = T(texture);
    ps2e_gfx2_texture_released(reinterpret_cast<ps2e_u32>(texture));
    t->texture.hostHandle = 0;
    if (t->texture.transfer >= 0)
    {
        DiskRelease(GetDiskManager(), &t->texture.transfer);
        t->texture.transfer = NoHandle;
    }

    MemoryDeallocate2_(texture);
}

// TextureRead of the PS2 arm: the size, the 0x60-byte header (its last 12 bytes the game's: set again), 0x20 bytes of the
// tools' memory, then the image packet into a block of the disk manager's; the host decodes the packet from there
void ReadTexture(GameTexture* texture, Stream* stream)
{
    ResourcesChanging();
    te_gfx_texture* t = T(texture);
    stream->ReadU32(&t->size);
    stream->Read(&t->texture, sizeof(te_gfx_texture_header), 1);
    t->texture.secondContext = 0;
    t->texture.hostHandle = 0;
    u8 leftover[TextureLeftoverSize];
    stream->Read(leftover, TextureLeftoverSize, 1);
    u32 imageSize = t->size - (sizeof(te_gfx_texture_header) + TextureLeftoverSize);
    s32 allocated;
    DiskAllocate(&allocated, GetDiskManager(), imageSize, false, DeferRelease);
    t->texture.transfer = allocated;
    u8* image = DiskLoadedMemory(GetDiskManager(), &t->texture.transfer);
    stream->Read(image, imageSize, 1);
    t->texture.hostHandle = static_cast<u32>(ps2e_gfx2_texture_loaded(reinterpret_cast<ps2e_u32>(texture),
                                                                      reinterpret_cast<ps2e_u32>(&t->texture),
                                                                      reinterpret_cast<ps2e_u32>(image),
                                                                      static_cast<ps2e_s32>(imageSize)));
}

// ---- materials ------------------------------------------------------------------------------------------------------
MaterialResource* NewMaterial(u32 id)
{
    ResourcesChanging();
    auto* resource = static_cast<MaterialResource*>(MemoryAllocate(sizeof(MaterialResource)));
    ConstructResourceHeader(resource, id);
    resource->material = nullptr;
    resource->unused0C = 0;
    return resource;
}

void DestroyMaterial(Material* material)
{
    ResourcesChanging();
    te_gfx_material* m = M(material);
    if (m->magic != TE_GFX_MATERIAL_MAGIC)
    {
        return;
    }

    for (u32 index = 0; index < m->shaderCount && index < TE_GFX_MAX_SHADERS; index++)
    {
        te_gfx_shader* shader = S(m->shaders[index]);
        if (shader != nullptr && shader->magic == TE_GFX_SHADER_MAGIC)
        {
            DestroyShader(shader);
            if (m->ownsShaders != 0 && shader != &g_FlatShader)
            {
                MemoryDeallocate2_(shader);
            }
        }

        m->shaders[index] = 0;
    }

    m->shaderCount = 0;
    m->magic = 0;
}

void DeleteMaterial(MaterialResource* resource)
{
    ResourcesChanging();
    if (resource->material != nullptr)
    {
        DestroyMaterial(resource->material);
        MemoryDeallocate2_(resource->material);
        resource->material = nullptr;
    }

    MemoryDeallocate2_(resource);
}

// ReadMaterialShader of the PS2 arm: the activated shaders' key, the bucket, a name (read and dropped), the shaders
void ReadMaterial(MaterialResource* resource, Stream* stream)
{
    ResourcesChanging();
    auto* material = static_cast<te_gfx_material*>(MemoryAllocate(Platform::Graphics::MaterialStorage));
    ConstructMaterialAt(material, TE_GFX_MATERIAL_RESOURCE, TE_GFX_BUCKET_OPAQUE);
    stream->ReadS64(reinterpret_cast<s64*>(&material->activatedShaders));
    s32 bucket;
    stream->ReadS32(&bucket);
    material->bucket = static_cast<u32>(bucket);
    s32 nameLength;
    stream->ReadS32(&nameLength);
    if (nameLength != 0)
    {
        void* name = MemoryAllocate2(static_cast<u32>(nameLength));
        stream->Read(name, static_cast<u32>(nameLength), 1);
        MemoryDeallocate2_(name);
    }

    s32 count;
    stream->ReadS32(&count);
    material->shaderCount = 0;
    for (s32 index = 0; index < count; index++)
    {
        s32 type;
        stream->ReadS32(&type);
        te_gfx_shader* shader = NewShader(static_cast<u32>(type));
        ReadShaderOfType(shader, static_cast<u32>(type), stream);
        if (material->shaderCount < TE_GFX_MAX_SHADERS)
        {
            material->shaders[material->shaderCount++] = reinterpret_cast<u32>(shader);
        }
        else
        {
            // The PS2 arm overran its four slots here (retail did too); the arm drops the extra shader
            DestroyShader(shader);
            MemoryDeallocate2_(shader);
        }
    }

    resource->material = reinterpret_cast<Material*>(material);
}

void ConstructMaterial(Material* material)
{
    ResourcesChanging();
    ConstructMaterialAt(M(material), TE_GFX_MATERIAL_RESOURCE, TE_GFX_BUCKET_OPAQUE);
}

Material* MakeFlatMaterial()
{
    ResourcesChanging();
    ConstructMaterialAt(&g_FlatMaterial, TE_GFX_MATERIAL_FLAT, TE_GFX_BUCKET_UI);
    ConstructShader(&g_FlatShader, 0x0E);
    g_FlatShader.settings = FlatSettings();
    g_FlatMaterial.shaderCount = 1;
    g_FlatMaterial.shaders[0] = reinterpret_cast<u32>(&g_FlatShader);
    g_FlatMaterial.ownsShaders = 0;
    g_FlatMaterial.activatedShaders = 1ull << 0x0E;
    return reinterpret_cast<Material*>(&g_FlatMaterial);
}

Material* FlatMaterial()
{
    return reinterpret_cast<Material*>(&g_FlatMaterial);
}

// The UI's 2D particles (OLEG's emitters: the HUD's crystal / gem / lives sparkles, the pickup sparkle effect): the flat
// settings, additive, textured with STQ coordinates -- the texture is the first particle page's additive shader's (materials.cpp
// MakeParticleMaterial: ParticlePageMaterial(0)->shaders[0]->texture, a reference taken from the texture table);
// without a page loaded (never on the PS2, which dereferenced it) the shader stays untextured
void* MakeParticleMaterial(Material* material)
{
    ResourcesChanging();
    te_gfx_material* m = M(material);
    ConstructMaterialAt(m, TE_GFX_MATERIAL_PARTICLE, TE_GFX_BUCKET_UI);
    te_gfx_shader* shader = NewShader(0x0E);
    u64 settings = FlatSettings();
    settings = (settings & ~(0xFull << 1)) | (u64{TE_GFX_PRESET_ADD} << 1);
    settings |= 1ull << 27;   // textured
    settings |= 1ull << 28;   // stq
    shader->settings = settings;
    const auto* page = reinterpret_cast<const te_gfx_material*>(ParticlePageMaterial(0));
    const te_gfx_shader* pageShader = page != nullptr && page->magic == TE_GFX_MATERIAL_MAGIC && page->shaderCount != 0
                                          ? reinterpret_cast<const te_gfx_shader*>(page->shaders[0]) : nullptr;
    if (pageShader != nullptr && pageShader->texture != 0)
    {
        u32 id = reinterpret_cast<const te_gfx_texture*>(pageShader->texture)->id;
        shader->textureId = id;
        shader->texture = reinterpret_cast<u32>(g_TextureTable.Acquire(&id, nullptr));
        shader->lodK = pageShader->lodK;
        shader->lodL = pageShader->lodL;
    }
    m->shaders[0] = reinterpret_cast<u32>(shader);
    m->shaderCount = 1;
    return shader;
}

void MakeDistortionMaterial(Material* material)
{
    ResourcesChanging();
    // the distorting hexagons' screen-copy shader (type 0x18). A plain blended material for now so nothing traps.
    te_gfx_material* m = M(material);
    ConstructMaterialAt(m, TE_GFX_MATERIAL_DISTORTION, 23);
    te_gfx_shader* shader = NewShader(0x18);
    shader->settings = FlatSettings();
    m->shaders[0] = reinterpret_cast<u32>(shader);
    m->shaderCount = 1;
}

Material* MakeSkidMaterial(bool subtracting)
{
    ResourcesChanging();
    // the skid marks' material (vertex colours added to or taken from the frame, depth not written, opaque bucket)
    auto* m = static_cast<te_gfx_material*>(MemoryAllocate(Platform::Graphics::MaterialStorage));
    ConstructMaterialAt(m, TE_GFX_MATERIAL_SKID, TE_GFX_BUCKET_OPAQUE);
    te_gfx_shader* shader = NewShader(0x0E);
    u64 settings = FlatSettings();
    settings = (settings & ~(0xFull << 1)) | (u64{subtracting ? TE_GFX_PRESET_SUBTRACT : TE_GFX_PRESET_ADD} << 1);
    settings = (settings & ~(3ull << 21)) | (2ull << 21);   // depth test GEQUAL
    shader->settings = settings;
    m->shaders[0] = reinterpret_cast<u32>(shader);
    m->shaderCount = 1;
    return reinterpret_cast<Material*>(m);
}

void SetFontPage(Material* material, u32 page)
{
    ResourcesChanging();
    te_gfx_material* m = M(material);
    if (m->magic == TE_GFX_MATERIAL_MAGIC && m->shaderCount != 0)
    {
        S(m->shaders[0])->fontPage = static_cast<u8>(page);
    }
}

// ---- the frame's memory and buckets ---------------------------------------------------------------------------------
void* AllocFrameMemory(u32 count, u32 size)
{
    u32 bytes = (count * size + 15) & ~15u;
    if (g_FrameArena == nullptr)
    {
        g_FrameArena = static_cast<u8*>(MemoryAllocate2(FrameArenaSize));
    }

    if (g_FrameArena != nullptr && g_FrameArenaUsed + bytes <= FrameArenaSize)
    {
        void* memory = g_FrameArena + g_FrameArenaUsed;
        g_FrameArenaUsed += bytes;
        if (g_FrameArenaUsed > g_FrameArenaPeak)
        {
            g_FrameArenaPeak = g_FrameArenaUsed;
        }

        return memory;
    }

    // Out of frame memory (the PS2 would have overrun its chain): the heap's, freed when the frame's buckets start over
    g_FrameArenaOverflows++;
    void* memory = MemoryAllocate(bytes);
    auto* entry = static_cast<FrameOverflow*>(MemoryAllocate(sizeof(FrameOverflow)));
    if (entry != nullptr)
    {
        entry->memory = memory;
        entry->next = g_FrameOverflows;
        g_FrameOverflows = entry;
    }

    return memory;
}

// The frame's memory starts over: at ResetBuckets (a movie had the screen) and at every SubmitBuckets, where the PS2's
// LinkRenderBuckets kicked the frame's DMA and StartFrameBuckets swapped the chains' buffers (buckets.cpp). The host
// copied every draw when it was recorded, so one buffer is enough where the PS2 double-buffered for its DMA.
static void FrameMemoryStartOver()
{
    g_FrameArenaUsed = 0;
    for (FrameOverflow* entry = g_FrameOverflows; entry != nullptr;)
    {
        FrameOverflow* next = entry->next;
        if (entry->memory != nullptr)
        {
            MemoryDeallocate_(entry->memory);
        }

        MemoryDeallocate_(entry);
        entry = next;
    }

    g_FrameOverflows = nullptr;
}

void ResetBuckets(bool movie)
{
    FrameMemoryStartOver();
    ps2e_gfx2_frame_reset(movie ? 1 : 0);
}

// The buckets are drawn at Present (host); here the frame's memory is recycled, as the PS2's chain swap did
void SubmitBuckets(bool)
{
    FrameMemoryStartOver();
}
}

// ---- shader animation: shadertypes.cpp UpdateShader for every shader of every material ------
// StepAnimations(clock) advanced each shader's UV scroll by its mode (wrapped: the fraction of the phase; sine / cosine of the
// phase's turn -- U's cosine lands in V's place as retail had it) and its ShaderAnimation (the game's own AdvanceShaderAnimation;
// the animated scrolls take its offset, the animated colour the shader's colour). Here the results go to te_gfx_shader::scroll
// (the host adds them to the vertices' ST) and the colour bytes. The particle wave shader (UpdateParticleWaves) is not animated yet.
namespace
{
constexpr u32 ScrollNone = 0, ScrollAnimated = 1, ScrollWrapped = 2, ScrollSine = 3, ScrollCosine = 4;
constexpr f32 ShaderTwoPi = 6.2831855f;
constexpr f32 AnimationColourScale = 256.0f, AnimationAlphaScale = 127.0f;

f32 Wrapped(f32* phase, f32 speed, f32 seconds)
{
    f32 value = *phase + speed * seconds;
    *phase = value;
    s32 whole = TruncateToInt(value);
    if (value < static_cast<f32>(whole))
    {
        whole--;
    }

    *phase -= static_cast<f32>(whole);
    return *phase;
}

f32 Waved(f32* phase, f32 speed, f32 seconds, bool cosine)
{
    *phase += speed * seconds;
    s32 angle;
    AngleFrom(&angle, *phase * ShaderTwoPi, AngleRadians);
    return cosine ? CosOfAngle(&angle) : SinOfAngle(&angle);
}

void UpdateShader(te_gfx_shader* shader, f32 seconds)
{
    const u32 uScroll = static_cast<u32>(TE_GFX_S_U_SCROLL(shader->settings));
    const u32 vScroll = static_cast<u32>(TE_GFX_S_V_SCROLL(shader->settings));
    switch (uScroll)
    {
    case ScrollWrapped: shader->scroll[0] = Wrapped(&shader->scrollPhases[0], shader->scrollSpeeds[0], seconds); break;
    case ScrollSine: shader->scroll[0] = Waved(&shader->scrollPhases[0], shader->scrollSpeeds[0], seconds, false); break;
    case ScrollCosine: shader->scroll[1] = Waved(&shader->scrollPhases[0], shader->scrollSpeeds[0], seconds, true); break;
    default: break;
    }

    switch (vScroll)
    {
    case ScrollWrapped: shader->scroll[1] = Wrapped(&shader->scrollPhases[1], shader->scrollSpeeds[1], seconds); break;
    case ScrollSine: shader->scroll[1] = Waved(&shader->scrollPhases[1], shader->scrollSpeeds[1], seconds, false); break;
    case ScrollCosine: shader->scroll[1] = Waved(&shader->scrollPhases[1], shader->scrollSpeeds[1], seconds, true); break;
    default: break;
    }

    if (shader->animation == 0)
    {
        return;
    }

    auto* animation = reinterpret_cast<ShaderAnimation*>(shader->animation);
    AdvanceShaderAnimation(animation, seconds);
    if (uScroll == ScrollAnimated)
    {
        shader->scroll[0] = animation->uvOffset.x;
    }

    if (vScroll == ScrollAnimated)
    {
        shader->scroll[1] = animation->uvOffset.y;
    }

    if (TE_GFX_S_ANIMATED_COLOUR(shader->settings))
    {
        shader->shaderColour[0] = animation->colour.x * AnimationColourScale;
        shader->shaderColour[1] = animation->colour.y * AnimationColourScale;
        shader->shaderColour[2] = animation->colour.z * AnimationColourScale;
        shader->shaderColour[3] = animation->colour.w * AnimationAlphaScale;
        SetColourBytes(shader);
    }
}
}

void Platform::Graphics::StepAnimations(const TimeClock* clock)
{
    ResourcesChanging();
    const f32 seconds = static_cast<f32>(static_cast<s32>(clock->advance)) * g_SecondsPerClockUnit;
    for (u32 index = 0; index < g_MaterialTable.count; index++)
    {
        MaterialResource* resource = g_MaterialTable.entries[index].item;
        if (resource == nullptr || resource->material == nullptr)
        {
            continue;
        }

        te_gfx_material* material = M(resource->material);
        if (material->magic != TE_GFX_MATERIAL_MAGIC)
        {
            continue;
        }

        for (u32 shaderIndex = 0; shaderIndex < material->shaderCount && shaderIndex < TE_GFX_MAX_SHADERS; shaderIndex++)
        {
            auto* shader = reinterpret_cast<te_gfx_shader*>(material->shaders[shaderIndex]);
            if (shader != nullptr && shader->magic == TE_GFX_SHADER_MAGIC)
            {
                UpdateShader(shader, seconds);
            }
        }
    }
}

