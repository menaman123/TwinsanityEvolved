#!/usr/bin/env python3
"""gs.py -- the PS2 Graphics Synthesizer's local-memory layout for the two formats a font page uses.

A font page of the game's PSF (a texture resource) is an 8-bit indexed texture (PSMT8, 128x256) with a 256-colour CT32 palette
(CSM1), uploaded as ONE 32-bit rectangle (PSMCT32, buffer width 64): the texels were laid out by the game's tools so that the
32-bit write puts every texel byte where the 8-bit read finds it. The game's upload packet: a DMA RET tag, VIF DIRECT, a GIF
PACKED tag writing TRXPOS / TRXREG / TRXDIR, a GIF IMAGE tag and the rectangle's words.

decode_page(header, image) -> (w, h, rgba bytes)   the texture as the GS reads it (used ONLY to MEASURE the disc font's style;
                                                   nothing decoded is written into the repository)
encode_page(w, h, indices, palette) -> image bytes the same packet for OUR page (an 8-bit image + its palette), so the game's
                                                   own reader and the port's decoder take it like the disc's

Alpha is the GS's: 0x80 = opaque. The block / column tables are the GS's (as every PS2 graphics emulator writes them).
"""
import struct

BLOCK32 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
BLOCK8 = BLOCK32
COLUMN32 = [
    [0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31],
    [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47], [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63],
]
COLUMN8 = [
    [0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54],
    [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
    [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23],
    [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
    [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86],
    [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
    [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119],
    [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
    [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182],
    [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
    [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151],
    [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
    [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214],
    [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
    [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247],
    [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255],
]

PSMCT32 = 0x00
PSMT8 = 0x13
HEADER_SIZE = 0x60
LEFTOVER_SIZE = 0x20


def addr32(bp, bw, x, y):
    """Word address of a CT32 pixel: bp in blocks (64 words), bw in 64-pixel units."""
    page = (x >> 6) + (y >> 5) * bw
    return bp * 64 + page * 2048 + BLOCK32[(y >> 3) & 3][(x >> 3) & 7] * 64 + COLUMN32[y & 7][x & 7]


def addr8(bp, bw, x, y):
    """Byte address of a T8 texel: bp in blocks (256 bytes), bw in 64-pixel units (a T8 page is 128 pixels wide)."""
    page = (x >> 7) + (y >> 6) * (bw >> 1)
    return bp * 256 + page * 8192 + BLOCK8[(y >> 4) & 3][(x >> 4) & 7] * 256 + COLUMN8[y & 15][x & 15]


def clut_position(index):
    """CSM1 for an 8-bit texture: bits 3 and 4 of the index are swapped in the palette's 16x16 rectangle."""
    p = (index & 0xE7) | ((index & 0x08) << 1) | ((index & 0x10) >> 1)
    return p & 15, p >> 4


def parse_header(h):
    wp, hp = struct.unpack_from("<HH", h, 4)
    levels, psm, upload, tcc, _, tfx = struct.unpack_from("<6B", h, 8)
    offsets = struct.unpack_from("<7I", h, 0x10)
    widths = struct.unpack_from("<7I", h, 0x2C)
    palette = struct.unpack_from("<I", h, 0x48)[0]
    return {"width": 1 << wp, "height": 1 << hp, "levels": levels, "psm": psm, "upload": upload, "tcc": tcc, "tfx": tfx,
            "offset": offsets[0], "tbw": widths[0], "palette": palette}


def walk_upload(image):
    """The (trxpos, trxreg, words) of the packet's IMAGE transfers (CNT / RET / END tags, VIF DIRECT, GIF A+D + IMAGE)."""
    transfers, at = [], 0
    pos = reg = (0, 0)
    while at + 16 <= len(image):
        w0 = struct.unpack_from("<I", image, at)[0]
        qwc, tag = w0 & 0xFFFF, (w0 >> 28) & 7
        at += 16
        end = at + qwc * 16
        while at < end:
            lo, hi = struct.unpack_from("<QQ", image, at)
            at += 16
            nloop, flg, nreg = lo & 0x7FFF, (lo >> 58) & 3, (lo >> 60) & 0xF
            if flg == 0:
                for _ in range(nloop * (nreg or 16)):
                    data, addr = struct.unpack_from("<QQ", image, at)
                    at += 16
                    if addr & 0xFF == 0x51:
                        pos = (data >> 32 & 0x7FF, data >> 48 & 0x7FF)
                    elif addr & 0xFF == 0x52:
                        reg = (data & 0xFFF, data >> 32 & 0xFFF)
            elif flg >= 2:
                transfers.append((pos, reg, image[at:at + nloop * 16]))
                at += nloop * 16
        if tag in (6, 7):
            break
    return transfers


def decode_page(header, image):
    """(width, height, RGBA bytes with the GS's alpha) of a PSMT8 page uploaded as CT32, or of a CT32 page."""
    h = parse_header(header)
    vram = bytearray(4 * 1024 * 1024)
    upload_bw = 1 if h["psm"] != PSMCT32 else h["tbw"]
    for (px, py), (rw, rh), words in walk_upload(image):
        for i in range(min(len(words) // 4, rw * rh)):
            x, y = px + i % rw, py + i // rw
            a = addr32(0, upload_bw, x, y) * 4
            vram[a:a + 4] = words[i * 4:i * 4 + 4]
    w, ht = h["width"], h["height"]
    out = bytearray(w * ht * 4)
    if h["psm"] == PSMCT32:
        for y in range(ht):
            for x in range(w):
                a = addr32(h["offset"], h["tbw"], x, y) * 4
                out[(y * w + x) * 4:(y * w + x) * 4 + 4] = vram[a:a + 4]
        return w, ht, bytes(out)
    if h["psm"] != PSMT8:
        raise ValueError("pixel format %#x is not one this tool reads" % h["psm"])
    palette = []
    for i in range(256):
        cx, cy = clut_position(i)
        a = addr32(h["palette"], 1, cx, cy) * 4
        palette.append(bytes(vram[a:a + 4]))
    for y in range(ht):
        for x in range(w):
            index = vram[addr8(h["offset"] * 4 // 4, h["tbw"], x, y)]
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = palette[index]
    return w, ht, bytes(out)


def make_header(width, height, tcc=1, tfx=0):
    """The 0x60-byte texture header of a PSMT8 page uploaded as CT32 (levels 1, the palette right after the texels)."""
    wp, hp = width.bit_length() - 1, height.bit_length() - 1
    texel_blocks = width * height // 256           # 256-byte blocks of the 8-bit texels
    h = bytearray(HEADER_SIZE)
    struct.pack_into("<HH", h, 4, wp, hp)
    struct.pack_into("<6B", h, 8, 1, PSMT8, PSMCT32, tcc, 0, tfx)
    struct.pack_into("<I", h, 0x10, 0)              # level 0 at the slot base
    struct.pack_into("<I", h, 0x2C, width // 64)    # level 0's buffer width (64-pixel units)
    struct.pack_into("<I", h, 0x48, texel_blocks)   # the palette after the texels
    return bytes(h)


def encode_page(width, height, indices, palette):
    """The upload packet of an 8-bit page: `indices` (width*height bytes, row-major), `palette` (256 RGBA tuples, GS alpha)."""
    if width % 128 or height % 64 or len(indices) != width * height or len(palette) != 256:
        raise ValueError("a page is a multiple of 128x64 texels with a 256-colour palette")
    tbw = width // 64
    texel_words = width * height // 4
    palette_bp = texel_words // 64
    vram = bytearray((texel_words + 2048) * 4)       # the texels' pages + one CT32 page holding the 16x16 palette
    for y in range(height):
        for x in range(width):
            vram[addr8(0, tbw, x, y)] = indices[y * width + x]
    for i, (r, g, b, a) in enumerate(palette):
        cx, cy = clut_position(i)
        at = addr32(palette_bp, 1, cx, cy) * 4
        vram[at:at + 4] = bytes((r, g, b, a))
    # the 32-bit rectangle (64 wide) whose write lays those bytes out: read every CT32 pixel back from the image
    rows = len(vram) // 4 // 64
    rect = bytearray(64 * rows * 4)
    for y in range(rows):
        for x in range(64):
            a = addr32(0, 1, x, y) * 4
            rect[(y * 64 + x) * 4:(y * 64 + x) * 4 + 4] = vram[a:a + 4]
    qwords = len(rect) // 16
    gif_packed = struct.pack("<QQ", 3 | (1 << 60), 0xE)   # NLOOP 3, PACKED, NREG 1, A+D
    trxpos = struct.pack("<QQ", 0, 0x51)
    trxreg = struct.pack("<QQ", 64 | (rows << 32), 0x52)
    trxdir = struct.pack("<QQ", 0, 0x53)
    gif_image = struct.pack("<QQ", qwords | (1 << 15) | (2 << 58), 0)   # NLOOP, EOP, IMAGE
    body = gif_packed + trxpos + trxreg + trxdir + gif_image + bytes(rect)
    qwc = len(body) // 16
    dma = struct.pack("<IIII", qwc | (6 << 28), 0, 0, 0x50000000 | qwc)  # RET, VIF NOP + DIRECT qwc
    return dma + body
