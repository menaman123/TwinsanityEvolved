// platform/nativeee (wasm side) -- Font::Begin / Draw / End (include/game/font.h: methods of a game class whose bodies were
// the PS2 renderer's text.cpp: glyph bytes for VU1 to expand into textured quads). The game lays the text out itself
// (RenderText, FontScale: src/game/font.cpp); here the VU1 program's expansion is done in C++ over that layout and the
// quads cross to the host typed (ps2e_gfx2_text_quads with te_gfx_text_quad), one call per font page used.
//
// The expansion (the text VU1 program, as the engine's text HLE pins it -- games/<title>/hle/graphics/text/text_draw.cpp):
// each line starts at (line.x + corner, line.y + corner) where corner = 2048*16 - frame*16/2 (the GS coordinates with the
// screen's middle at 2048, which StartFrame's XYOFFSET takes away again); each glyph is a quad from the cursor to
// (cursor + width*scale, y + height*scale) showing texels (u, v)..(u + width, v - height) of its page, and the cursor
// advances by width*scale whatever the glyph's page. Glyph quadwords: x = u (its page in the low two bits of the float's
// bits), y = v, z = width, w = height, all in 16ths of a pixel.
#include "common.h"
#include "gcc2.h"
#include "game/font.h"
#include "game/graphicstables.h"
#include "game/memory.h"
#include "game/overlay.h"
#include "game/renderer.h"
#include "game/stream.h"
#include "platform/graphics.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_text_shape.h"

#include <bit>

namespace
{
constexpr s32 MaxQuadsPerCall = 256;
te_gfx_text_quad g_Quads[MaxQuadsPerCall];
// Which pages the frame's texts used (End sends nothing more: the quads went out in Draw, per page)
Material* g_TextMaterial;

u32 GlyphPage(const Vector4& glyph)
{
    return std::bit_cast<u32>(glyph.x) & 3;
}

f32 Corner(s32 frameSize)
{
    return 32768.0f - static_cast<f32>(frameSize * 16) * 0.5f;
}

// ---- a language mod's right-to-left text: shaped by te_text_shape.h, drawn with two fonts -------------
// Font::Begin asks the platform whether the language in force reads right to left; while it does, a text holding an Arabic
// letter is shaped and laid out here, its Arabic glyphs from the language's own font (read from the platform the first time
// one is drawn: nothing is loaded for any other language), everything else from the game's font. Any other text takes the
// game's own path below, unchanged.
bool g_RightToLeft;
s32 g_DiagOn = -1;

struct ArabicFont
{
    bool tried;
    Font* font;
    s32 count;
    u32 codepoints[256];
};
ArabicFont g_Arabic;
te::text::ShapedText g_Shaped;
te::text::LaidText g_Laid;
te::text::Diag g_Diag;

constexpr u32 FontTableMagic = 0x4D474554;   // "TEGM": the codepoint table after the glyphs

// The language's font: a PSF (its pages read into the game's texture and material tables like the game font's), then its
// codepoint table. Null when the platform has none or it does not read
const Font* LanguageFont()
{
    if (g_Arabic.tried)
    {
        return g_Arabic.font;
    }

    g_Arabic.tried = true;
    s32 size = ps2e_text_font_size();
    if (size <= 16)
    {
        return nullptr;
    }

    MemoryStream reader;
    MemoryStream::ConstructAllocated(&reader, static_cast<u32>(size), 0, MemoryStream::FileAlignment);
    if (reader.begin == nullptr || ps2e_text_font_read(reinterpret_cast<ps2e_u32>(reader.begin), size) != size)
    {
        reader.Destroy(DestroyOnly);
        return nullptr;
    }

    Stream* stream = &reader;
    Font* font = Font::Construct(static_cast<Font*>(MemoryAllocate(sizeof(Font))));
    u32 pages = 0;
    stream->ReadU32(&pages);
    font->pageCount = static_cast<s32>(pages <= Font::MaxPages ? pages : 0);
    for (s32 page = 0; page < font->pageCount; page++)
    {
        u32 textureId;
        u32 materialId;
        stream->ReadS32(reinterpret_cast<s32*>(&textureId));
        stream->ReadS32(reinterpret_cast<s32*>(&materialId));
        font->textures[page] = TextureFromStream(textureId, stream);
        MaterialResource* material = MaterialFromStream(materialId, stream);
        font->materials[page] = material;
        Platform::Graphics::SetFontPage(material->material, static_cast<u32>(page));
    }

    u32 glyphs = 0;
    stream->ReadU32(&glyphs);
    stream->ReadU32(reinterpret_cast<u32*>(&font->firstCharacter));
    font->glyphCount = static_cast<s32>(glyphs <= 256 ? glyphs : 256);
    font->glyphs = static_cast<Vector4*>(MemoryAllocate(static_cast<u32>(font->glyphCount) * sizeof(Vector4)));
    stream->Read(font->glyphs, static_cast<u32>(font->glyphCount) * sizeof(Vector4), 1);
    u32 magic = 0;
    u32 version = 0;
    u32 count = 0;
    stream->ReadU32(&magic);
    stream->ReadU32(&version);
    stream->ReadU32(&count);
    g_Arabic.count = 0;
    if (magic == FontTableMagic && count == glyphs)
    {
        for (u32 i = 0; i < count && i < 256; i++)
        {
            stream->ReadU32(&g_Arabic.codepoints[i]);
        }

        g_Arabic.count = static_cast<s32>(count < 256 ? count : 256);
    }

    reader.Destroy(DestroyOnly);
    g_Arabic.font = g_Arabic.count != 0 && font->pageCount != 0 ? font : nullptr;
    return g_Arabic.font;
}

const Font* g_MetricsFonts[2];

f32 GlyphWidth(void*, u8 font, u8 index)
{
    return g_MetricsFonts[font]->glyphs[index].z;
}

f32 GlyphHeight(void*, u8 font, u8 index)
{
    return g_MetricsFonts[font]->glyphs[index].w;
}

u32 GameGlyphIndex(const Font* font, u32 character)
{
    s32 index = static_cast<s32>(character & 0xFF) - font->firstCharacter;
    return index < 0 || index >= font->glyphCount ? 0 : static_cast<u32>(index);
}

// WriteTextPackets' placement over the shaped text: the glyph scale, the lines by the game's rules, the cursor by the
// alignment (mirrored inside the text's box for right to left), then the quads a page of a font at a time
void DrawShaped(Font* game, const Font* language, const TextItem* item, TextPackets* packets)
{
    Vector2 position;
    position.x = item->position.x * static_cast<f32>(g_RendererWidth) * 16.0f;
    position.y = item->position.y * static_cast<f32>(g_RendererHeight) * 16.0f;
    Vector2 scale;
    FontScale(game, &scale);
    Vector2 size;
    size.x = item->scale.x * scale.x;
    size.y = item->scale.y * scale.y;
    te::text::Shape(item->text, &g_Shaped);
    te::text::FontTables tables{g_Arabic.codepoints, g_Arabic.count, game->firstCharacter, game->glyphCount};
    te::text::Resolve(&g_Shaped, tables);
    g_MetricsFonts[te::text::kFontGame] = game;
    g_MetricsFonts[te::text::kFontArabic] = language;
    te::text::Metrics metrics{GlyphWidth, GlyphHeight, nullptr, game->glyphs[GameGlyphIndex(game, ' ')].w};
    te::text::Layout(g_Shaped, metrics, &g_Laid);
    f32 height = 0.0f;
    for (s32 line = 0; line < g_Laid.lineCount; line++)
    {
        height = height + g_Laid.lines[line].height * size.y;
    }

    // the empty lines' heights are in the line before them; one before the first line counts in the text's height only
    height = g_Laid.height * size.y > height ? g_Laid.height * size.y : height;
    f32 widest = g_Laid.widest * size.x;
    Vector2 cursor = position;
    TextAlignment alignment = item->alignment;
    if (alignment.top == 0)
    {
        cursor.y = alignment.bottom != 0 ? cursor.y - height : cursor.y - height * 0.5f;
    }

    for (s32 i = 0; i < g_Shaped.count; i++)
    {
        const te::text::ShapedGlyph& shaped = g_Shaped.glyphs[i];
        if (shaped.font == te::text::kFontGame && shaped.index != te::text::kNoGlyph)
        {
            packets->pages[GlyphPage(game->glyphs[shaped.index])] = 1;
        }
    }

    f32 cornerX = Corner(g_RendererWidth);
    f32 cornerY = Corner(g_RendererHeight);
    for (u8 which = 0; which < 2; which++)
    {
        const Font* font = which == te::text::kFontArabic ? language : game;
        for (s32 page = 0; page < font->pageCount; page++)
        {
            if (font->materials[page] == nullptr || font->materials[page]->material == nullptr)
            {
                continue;
            }

            Material* material = font->materials[page]->material;
            s32 quads = 0;
            f32 y = cursor.y;
            for (s32 line = 0; line < g_Laid.lineCount; line++)
            {
                const te::text::LaidLine& laid = g_Laid.lines[line];
                f32 x = cursor.x + te::text::LineStart(alignment.value, g_Shaped.rtl, laid.width * size.x, widest);
                f32 lineY = y + cornerY;
                f32 glyphX = x + cornerX;
                for (s32 i = laid.first; i < laid.first + laid.count; i++)
                {
                    const te::text::ShapedGlyph& shaped = g_Shaped.glyphs[i];
                    if (shaped.index == te::text::kNoGlyph)
                    {
                        continue;
                    }

                    const Vector4& glyph = g_MetricsFonts[shaped.font]->glyphs[shaped.index];
                    f32 right = glyphX + glyph.z * size.x;
                    f32 bottom = lineY + glyph.w * size.y;
                    if (shaped.font == which && static_cast<s32>(GlyphPage(glyph)) == page + 1)
                    {
                        te_gfx_text_quad& quad = g_Quads[quads++];
                        quad.x0 = glyphX;
                        quad.y0 = lineY;
                        quad.x1 = right;
                        quad.y1 = bottom;
                        quad.u0 = glyph.x;
                        quad.v0 = glyph.y;
                        quad.u1 = glyph.x + glyph.z;
                        quad.v1 = glyph.y - glyph.w;
                        quad.colour = item->colour;
                        if (quads == MaxQuadsPerCall)
                        {
                            ps2e_gfx2_text_quads(reinterpret_cast<ps2e_u32>(material), reinterpret_cast<ps2e_u32>(g_Quads), quads);
                            quads = 0;
                        }
                    }

                    glyphX = right;
                }

                y = y + laid.height * size.y;
            }

            if (quads != 0)
            {
                ps2e_gfx2_text_quads(reinterpret_cast<ps2e_u32>(material), reinterpret_cast<ps2e_u32>(g_Quads), quads);
            }
        }
    }

    if (g_DiagOn < 0)
    {
        g_DiagOn = ps2e_text_diag();
    }

    if (g_DiagOn != 0)
    {
        // the census: the text's box (the widest line's span at the anchor, the lines' heights), each line's, each glyph's
        te::text::Diag& d = g_Diag;
        d.text = reinterpret_cast<u32>(item->text);
        d.anchorX = position.x + cornerX;
        d.anchorY = position.y + cornerY;
        d.alignment = alignment.value;
        d.rtl = g_Shaped.rtl ? 1 : 0;
        d.missing = g_Shaped.missing;
        d.lineCount = g_Laid.lineCount;
        d.glyphCount = 0;
        f32 boxLeft = cursor.x + te::text::LineStart(alignment.value, false, widest, widest);
        d.boxX0 = boxLeft + cornerX;
        d.boxX1 = boxLeft + widest + cornerX;
        d.boxY0 = cursor.y + cornerY;
        d.boxY1 = cursor.y + height + cornerY;
        f32 y = cursor.y;
        for (s32 line = 0; line < g_Laid.lineCount; line++)
        {
            const te::text::LaidLine& laid = g_Laid.lines[line];
            f32 x = cursor.x + te::text::LineStart(alignment.value, g_Shaped.rtl, laid.width * size.x, widest) + cornerX;
            te::text::DiagLine& dl = d.lines[line];
            dl.x0 = x;
            dl.x1 = x + laid.width * size.x;
            dl.y0 = y + cornerY;
            dl.y1 = y + laid.height * size.y + cornerY;
            dl.first = d.glyphCount;
            dl.count = 0;
            for (s32 i = laid.first; i < laid.first + laid.count && d.glyphCount < te::text::kDiagGlyphs; i++)
            {
                const te::text::ShapedGlyph& shaped = g_Shaped.glyphs[i];
                f32 width = shaped.index == te::text::kNoGlyph ? 0.0f : g_MetricsFonts[shaped.font]->glyphs[shaped.index].z * size.x;
                te::text::DiagGlyph& dg = d.glyphs[d.glyphCount++];
                dg.codepoint = shaped.codepoint;
                dg.source = shaped.source;
                dg.font = shaped.font;
                dg.index = shaped.index;
                dg.x0 = x;
                dg.x1 = x + width;
                x = x + width;
                dl.count++;
            }

            y = y + laid.height * size.y;
        }

        ps2e_text_shaped(reinterpret_cast<ps2e_u32>(&g_Diag));
    }

    packets->count = 0;
}
}

void Font::Begin()
{
    g_TextMaterial = materials[0] != nullptr ? materials[0]->material : nullptr;
    g_RightToLeft = ps2e_text_direction() != 0;
}

void Font::Draw(const TextItem* item, TextPackets* packets)
{
    // The host's text-draw census: the string this item draws (its quads follow, a page at a time)
    ps2e_gfx2_text_item(reinterpret_cast<ps2e_u32>(item->text), item->position.x, item->position.y,
                        static_cast<ps2e_s32>(item->alignment.value));
    // A right-to-left language's text (one holding its letters): shaped, drawn with its own font beside this one
    if (g_RightToLeft && te::text::HasArabic(item->text))
    {
        if (const Font* language = LanguageFont())
        {
            DrawShaped(this, language, item, packets);
            return;
        }
    }

    // WriteTextPackets' layout: the glyph scale, the text laid out, the cursor by the alignment
    Vector2 position;
    position.x = item->position.x * static_cast<f32>(g_RendererWidth) * 16.0f;
    position.y = item->position.y * static_cast<f32>(g_RendererHeight) * 16.0f;
    Vector2 scale;
    FontScale(this, &scale);
    Vector2 size;
    size.x = item->scale.x * scale.x;
    size.y = item->scale.y * scale.y;
    static TextLayout layout;
    layout.pages[0] = layout.pages[1] = layout.pages[2] = layout.pages[3] = 0;
    RenderText(this, item->text, &size, &layout);
    Vector2 cursor = position;
    TextAlignment alignment = item->alignment;
    if (alignment.top == 0)
    {
        cursor.y = alignment.bottom != 0 ? cursor.y - layout.height : cursor.y - layout.height * 0.5f;
    }

    for (u32 page = 0; page < 4; page++)
    {
        if (layout.pages[page] == 1)
        {
            packets->pages[page] = 1;
        }
    }

    // The VU1 expansion, a page at a time (each page's material draws its own glyphs; the cursor advances for every glyph)
    f32 cornerX = Corner(g_RendererWidth);
    f32 cornerY = Corner(g_RendererHeight);
    for (s32 page = 0; page < pageCount; page++)
    {
        if (layout.pages[page + 1] == 0 || materials[page] == nullptr || materials[page]->material == nullptr)
        {
            continue;
        }

        Material* material = materials[page]->material;
        s32 quads = 0;
        s32 taken = 0;
        f32 y = cursor.y;
        for (s32 line = 0; line < layout.lineCount; line++)
        {
            f32 x = cursor.x;
            f32 extent = layout.lines[line].x;
            if (alignment.left == 0)
            {
                x = alignment.right != 0 ? x - extent : x - extent * 0.5f;
            }

            f32 lineY = y + cornerY;
            f32 glyphX = x + cornerX;
            s32 count = layout.counts[line];
            for (s32 glyphIndex = 0; glyphIndex < count; glyphIndex++)
            {
                const Vector4& glyph = glyphs[layout.glyphs[taken + glyphIndex]];
                f32 right = glyphX + glyph.z * size.x;
                f32 bottom = lineY + glyph.w * size.y;
                if (static_cast<s32>(GlyphPage(glyph)) == page + 1)
                {
                    te_gfx_text_quad& quad = g_Quads[quads++];
                    quad.x0 = glyphX;
                    quad.y0 = lineY;
                    quad.x1 = right;
                    quad.y1 = bottom;
                    quad.u0 = glyph.x;
                    quad.v0 = glyph.y;
                    quad.u1 = glyph.x + glyph.z;
                    quad.v1 = glyph.y - glyph.w;
                    quad.colour = item->colour;
                    if (quads == MaxQuadsPerCall)
                    {
                        ps2e_gfx2_text_quads(reinterpret_cast<ps2e_u32>(material), reinterpret_cast<ps2e_u32>(g_Quads), quads);
                        quads = 0;
                    }
                }

                glyphX = right;
            }

            taken += count;
            y = y + layout.lines[line].y;
        }

        if (quads != 0)
        {
            ps2e_gfx2_text_quads(reinterpret_cast<ps2e_u32>(material), reinterpret_cast<ps2e_u32>(g_Quads), quads);
        }
    }

    packets->count = 0;
}

void Font::End(TextPackets*)
{
    g_TextMaterial = nullptr;
}

// the platform's own texts (the fps counter, a mod's timer) over the game's, once a frame, through the GAME's text system
// (QueueText with the game's font: the same quads the menus and the HUD are drawn with). The host decides what to show.
namespace
{
constexpr s32 MaxPlatformTexts = 8;
te_platform_text g_PlatformTexts[MaxPlatformTexts];
}

void Platform::Graphics::QueuePlatformTexts(Renderer* renderer, Font* font)
{
    if (renderer == nullptr || font == nullptr || font->glyphs == nullptr)
    {
        return;
    }

    s32 count = ps2e_gfx2_platform_texts(reinterpret_cast<ps2e_u32>(g_PlatformTexts), MaxPlatformTexts);
    if (count <= 0)
    {
        return;
    }

    if (count > MaxPlatformTexts)
    {
        count = MaxPlatformTexts;
    }

    for (s32 i = 0; i < count; i++)
    {
        const te_platform_text& line = g_PlatformTexts[i];
        if (line.text[0] == '\0')
        {
            continue;
        }

        renderer->font = font;
        renderer->colour = line.colour;
        renderer->textScale.x = line.scale;
        renderer->textScale.y = line.scale;
        renderer->textAlignment.value = line.alignment;
        QueueText(renderer, line.text, line.x, line.y);
    }
}
