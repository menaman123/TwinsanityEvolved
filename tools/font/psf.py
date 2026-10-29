#!/usr/bin/env python3
"""psf.py -- the game's font file (a PSF: game/src/game/font.cpp Font::Read) read and written.

    u32 pageCount                                   (Font::MaxPages = 3)
    per page: u32 textureId, u32 materialId, the TEXTURE resource, the MATERIAL resource
        texture  (Platform::Graphics::ReadTexture): u32 size (= 0x60 + 0x20 + image bytes), the 0x60-byte header, 0x20
                 bytes of the tools' memory, the GS upload packet (gs.py)
        material (Platform::Graphics::ReadMaterial): s64 activated shaders' key, s32 bucket, s32 name length + the name,
                 s32 shader count, per shader: s32 type, the type's own fields (none for the font's type 0x0D), 30 setting
                 bytes, s16 lodK, s16 lodL, 16 leftover bytes, 4 f32 colour, 2 f32 scroll phases, 2 f32 scroll speeds,
                 s32 texture id, s32 type again
    u32 glyphCount, u32 firstCharacter
    glyphCount quadwords: f32 u (the page in the low two bits of its bits: font page index + 1; 0 = not drawn), f32 v (the
                 glyph's TOP row + 1 in sixteenths... the quad samples v down to v - height), f32 width, f32 height -- all in
                 sixteenths of a texel; a glyph's advance IS its width

OUR trailer (after the glyphs; the game's reader stops before it): b"TEGM", u32 version 1, u32 count, then per glyph a u32
Unicode codepoint -- what each glyph draws (a presentation form U+FE70..U+FEFC, a letter, a digit). The text layer maps its
shaped codepoints to glyph indexes through it.
"""
import struct

TRAILER_MAGIC = b"TEGM"
SHADER_SETTINGS = 30


class Reader:
    def __init__(self, data, at=0):
        self.d, self.p = data, at

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]
        self.p += 4
        return v

    def s32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def take(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v


TYPE_EXTRA = {0x10: 4, 0x11: 4, 0x18: 4, 0x17: 12, 0x1A: 20, 0x1C: 8}


def read_material(r):
    m = {"activated": struct.unpack_from("<q", r.take(8))[0], "bucket": r.s32()}
    n = r.s32()
    m["name"] = r.take(n)
    m["shaders"] = []
    for _ in range(r.s32()):
        s = {"type": r.s32()}
        s["extra"] = r.take(TYPE_EXTRA.get(s["type"], 0))
        s["settings"] = list(r.take(SHADER_SETTINGS))
        s["lod"] = struct.unpack("<hh", r.take(4))
        s["leftover"] = r.take(16)
        s["colour"] = struct.unpack("<4f", r.take(16))
        s["scroll"] = struct.unpack("<4f", r.take(16))
        s["texture"] = r.u32()
        s["type2"] = r.s32()
        if s["settings"][29]:
            raise ValueError("an animated font shader: not a font this tool reads")
        m["shaders"].append(s)
    return m


def write_material(m):
    out = struct.pack("<qi", m["activated"], m["bucket"]) + struct.pack("<i", len(m["name"])) + m["name"]
    out += struct.pack("<i", len(m["shaders"]))
    for s in m["shaders"]:
        out += struct.pack("<i", s["type"]) + s["extra"] + bytes(s["settings"]) + struct.pack("<hh", *s["lod"]) + s["leftover"]
        out += struct.pack("<4f", *s["colour"]) + struct.pack("<4f", *s["scroll"]) + struct.pack("<Ii", s["texture"], s["type2"])
    return out


def read_psf(data):
    r = Reader(data)
    font = {"pages": []}
    for _ in range(r.u32()):
        page = {"texture_id": r.u32(), "material_id": r.u32()}
        size = r.u32()
        page["header"] = r.take(0x60)
        page["leftover"] = r.take(0x20)
        page["image"] = r.take(size - 0x80)
        page["material"] = read_material(r)
        font["pages"].append(page)
    count = r.u32()
    font["first"] = r.u32()
    glyphs = []
    for _ in range(count):
        raw = r.take(16)
        u, v, w, h = struct.unpack("<4f", raw)
        glyphs.append({"page": struct.unpack_from("<I", raw)[0] & 3, "u": u, "v": v, "w": w, "h": h})
    font["glyphs"] = glyphs
    font["codepoints"] = None
    if r.d[r.p:r.p + 4] == TRAILER_MAGIC:
        r.take(4)
        r.u32()
        n = r.u32()
        font["codepoints"] = [r.u32() for _ in range(n)]
    return font


def glyph_word(page, u):
    """A glyph's first word: u in sixteenths with the page (font page index + 1) in its low two bits."""
    bits = struct.unpack("<I", struct.pack("<f", u))[0]
    return struct.unpack("<f", struct.pack("<I", (bits & ~3) | (page & 3)))[0]


def write_psf(pages, first, glyphs, codepoints=None):
    """pages: [{texture_id, material_id, header, leftover, image, material}]; glyphs: [{page, u, v, w, h}] (sixteenths)."""
    out = struct.pack("<I", len(pages))
    for p in pages:
        out += struct.pack("<II", p["texture_id"], p["material_id"])
        out += struct.pack("<I", 0x80 + len(p["image"])) + p["header"] + p["leftover"] + p["image"]
        out += write_material(p["material"])
    out += struct.pack("<II", len(glyphs), first)
    for g in glyphs:
        out += struct.pack("<4f", glyph_word(g["page"], g["u"]), g["v"], g["w"], g["h"])
    if codepoints is not None:
        out += TRAILER_MAGIC + struct.pack("<II", 1, len(codepoints)) + b"".join(struct.pack("<I", c) for c in codepoints)
    return out
