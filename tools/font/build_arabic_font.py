#!/usr/bin/env python3
"""build_arabic_font.py -- OUR Arabic glyphs in the game's lettering style, packed as a font in the game's own format.

    build_arabic_font.py [--iso IMAGE] [--style tools/font/style_pal.json] [--cache DIR] [--weight auto|400..800]
                         [--out mods/arabic/data/arabic.font] [--preview DIR] [--check]

1 THE TYPEFACE: Baloo Bhaijaan 2 (Ek Type, SIL Open Font License 1.1: tools/font/OFL-BalooBhaijaan2.txt), fetched from
  google/fonts at a pinned commit and checked against its sha256 (TYPEFACE below); cached in --cache (default build/font-cache,
  outside the scanned tree). A rounded, playful display face like the game's own cartoon capitals.
2 THE FORMS: every letter's isolated / final / initial / medial form, the lam-alef ligatures, tatweel, the Arabic-Indic digits
  and the Arabic comma / semicolon / question mark (tools/text/arabic_forms.py: 139 glyphs) -- the glyph of each form found
  through the typeface's OWN shaping tables (its GSUB init / medi / fina features and its rlig lam-alef ligatures).
3 THE STYLE (measured from the game's Latin glyphs: measure_style.py with --iso, else the committed numbers in --style):
  the glyph cell's baseline, the alef sized to the Latin cap height, the weight chosen so the stroke matches the Latin stroke,
  the same 1-texel light outline, the fill's vertical gradient from the cap height to the baseline, the same soft drop shadow.
  Joined sides of a form are drawn with the joining stroke continuing past the cell (the typeface's tatweel), so neither the
  outline nor the shadow closes where two letters connect.
4 THE FILE: the glyph cells packed into 8-bit pages with a 256-colour palette (gs.py: the disc font's own page format),
  a page material with the game font's render settings, the glyph quadwords (sixteenths of a texel), and OUR trailer naming
  each glyph's codepoint (psf.py). The text layer of the game draws these glyphs beside the game's own Latin glyphs.

No pixel of the disc is read except by measure_style.py to MEASURE (numbers); nothing of the disc is written.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import tempfile
import urllib.request

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "text"))
import arabic_forms as AF  # noqa: E402
import gs  # noqa: E402
import psf  # noqa: E402
import raster  # noqa: E402

TYPEFACE = {
    "name": "Baloo Bhaijaan 2",
    "url": "https://raw.githubusercontent.com/google/fonts/c13390277dfb1fcc7415d84ef7ef9cfc1e52f8c1/ofl/baloobhaijaan2/BalooBhaijaan2%5Bwght%5D.ttf",
    "file": "BalooBhaijaan2[wght].ttf",
    "sha256": "3e9f07fbc796c0ddcb3e6e0aa26f9c86741d9f5b7f5cb72f4ed3c06e55a19336",
    "licence": "OFL-BalooBhaijaan2.txt",
}
DEFAULT_STYLE = os.path.join(HERE, "style_pal.json")
DEFAULT_OUT = os.path.join(ROOT, "mods", "arabic", "data", "arabic.font")
# Our resources' IDs in the game's texture / material tables (the disc's are hashes; these are ours and fixed)
TEXTURE_ID = 0x7E41A100
MATERIAL_ID = 0x7E41B100
PAGE_W, PAGE_H_MAX = 256, 512
ALEF_TO_CAP = 0.85          # the alef's height: 0.85 of the Latin cap height (the menus' row pitch is set for Latin capitals)


def DRAWN():
    """The codepoints that have a picture (the word space is an empty cell)."""
    return [c for c in AF.required_codepoints() if c != 0x20]


# ---- the typeface ------------------------------------------------------------------------------------------------------
def fetch_typeface(cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, TYPEFACE["file"])
    if not os.path.exists(path):
        print("build_arabic_font: fetching %s" % TYPEFACE["url"], flush=True)
        with urllib.request.urlopen(TYPEFACE["url"], timeout=60) as r:
            data = r.read()
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
    digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
    if digest != TYPEFACE["sha256"]:
        sys.exit("build_arabic_font: %s has sha256 %s, the pinned typeface is %s" % (path, digest, TYPEFACE["sha256"]))
    return path


def instance(path, weight):
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer
    t = TTFont(path)
    if "fvar" in t:
        t = instancer.instantiateVariableFont(t, {"wght": weight})
    return t


def gsub_forms(t):
    """{codepoint: glyph name} of every required codepoint through the typeface's own GSUB (init/medi/fina single
    substitutions, rlig/liga lam-alef ligatures); the isolated form is the letter's own glyph."""
    cmap = t.getBestCmap()
    table = t["GSUB"].table

    def subtables(tag):
        for fr in table.FeatureList.FeatureRecord:
            if fr.FeatureTag != tag:
                continue
            for li in fr.Feature.LookupListIndex:
                lk = table.LookupList.Lookup[li]
                for st in lk.SubTable:
                    yield st.ExtSubTable if lk.LookupType == 7 else st

    single = {tag: {} for tag in ("init", "medi", "fina", "isol")}
    for tag in single:
        for st in subtables(tag):
            if hasattr(st, "mapping"):
                single[tag].update(st.mapping)
    ligatures = {}
    for tag in ("rlig", "liga"):
        for st in subtables(tag):
            if hasattr(st, "ligatures"):
                for first, lst in st.ligatures.items():
                    for lig in lst:
                        ligatures.setdefault((first,) + tuple(lig.Component), lig.LigGlyph)
    out = {}
    for letter in AF.LETTERS:
        base = cmap[letter]
        for form, cp in AF.forms_of(letter):
            tag = AF.FORM_NAMES[form]
            out[cp] = single["isol"].get(base, base) if tag == "isol" else single[tag].get(base)
    lam = cmap[AF.LAM]
    for alef, (iso, fin) in AF.LAM_ALEF.items():
        a = cmap[alef]
        afin = single["fina"].get(a, a)
        out[iso] = ligatures.get((single["init"].get(lam, lam), afin)) or ligatures.get((lam, a))
        out[fin] = ligatures.get((single["medi"].get(lam, lam), afin)) or ligatures.get((lam, a))
    for cp in [AF.TATWEEL] + AF.DIGITS + AF.PUNCTUATION:
        out[cp] = cmap.get(cp)
    missing = [hex(c) for c in DRAWN() if not out.get(c)]
    if missing:
        sys.exit("build_arabic_font: the typeface has no glyph for %s" % missing)
    return out


def bounds(glyphset, name):
    from fontTools.pens.boundsPen import BoundsPen
    bp = BoundsPen(glyphset)
    glyphset[name].draw(bp)
    return bp.bounds or (0, 0, 0, 0)


# ---- the style ---------------------------------------------------------------------------------------------------------
def dilate(a, r):
    """Grey-scale dilation by a disk of radius r texels (the outline's coverage)."""
    out = a.copy()
    ir = int(np.ceil(r))
    h, w = a.shape
    for dy in range(-ir, ir + 1):
        for dx in range(-ir, ir + 1):
            d = (dx * dx + dy * dy) ** 0.5
            if d > r + 0.5:
                continue
            weight = 1.0 if d <= r - 0.5 else (r + 0.5 - d)
            s = np.zeros_like(a)
            ys, ye = max(0, dy), h + min(0, dy)
            xs, xe = max(0, dx), w + min(0, dx)
            s[ys:ye, xs:xe] = a[ys - dy:ye - dy, xs - dx:xe - dx]
            out = np.maximum(out, s * weight)
    return out


def blur(a, sigma):
    if sigma <= 0:
        return a
    r = max(1, int(3 * sigma + 0.5))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    p = np.pad(a, r)
    p = np.apply_along_axis(lambda v: np.convolve(v, k, mode="valid"), 1, p)
    return np.apply_along_axis(lambda v: np.convolve(v, k, mode="valid"), 0, p)


def gradient_colour(style, row):
    """The fill colour (GS scale) at a cell row: the Latin fill's per-row colours from the cap top to the baseline,
    held at the ends."""
    rows = style["fill_gradient_rows"]
    t = (row - style["cap_top_row"]) / max(1.0, style["baseline_row"] - style["cap_top_row"])
    t = min(1.0, max(0.0, t))
    for a, b in zip(rows, rows[1:]):
        if a["t"] <= t <= b["t"]:
            f = (t - a["t"]) / max(1e-6, b["t"] - a["t"])
            return [a["rgb"][i] + (b["rgb"][i] - a["rgb"][i]) * f for i in range(3)]
    return rows[0]["rgb"] if t < rows[0]["t"] else rows[-1]["rgb"]


def compose(style, cov):
    """RGBA (GS scale floats) of a coverage map styled: shadow, then outline, then the fill's gradient (over)."""
    h, w = cov.shape
    outline = dilate(cov, style["outline_px"])
    dx, dy = style["shadow_offset_px"]
    sh = np.zeros_like(outline)
    sh[dy:, dx:] = outline[:h - dy, :w - dx] if dy or dx else outline
    shadow = np.clip(blur(sh, style["shadow_blur_px"]) * style["shadow_strength"], 0, 1)
    rgb = np.zeros((h, w, 3))
    alpha = np.zeros((h, w))
    # premultiplied "over", bottom to top
    for layer_alpha, colour in ((shadow, None), (outline, style["outline_rgb"]), (cov, "gradient")):
        if colour is None:
            c = np.zeros((h, w, 3))
        elif colour == "gradient":
            c = np.array([gradient_colour(style, r) for r in range(h)])[:, None, :].repeat(w, axis=1)
        else:
            c = np.array(colour, dtype=float)[None, None, :].repeat(h, axis=0).repeat(w, axis=1)
        a = layer_alpha[:, :, None]
        rgb = c * a + rgb * (1 - a)
        alpha = layer_alpha + alpha * (1 - layer_alpha)
    with np.errstate(invalid="ignore", divide="ignore"):
        straight = np.where(alpha[:, :, None] > 0, rgb / np.maximum(alpha[:, :, None], 1e-6), 0)
    return np.concatenate([straight, (alpha * 128.0)[:, :, None]], axis=2)


# ---- the glyph cells ---------------------------------------------------------------------------------------------------
def joins(cp):
    """(joins on its left: to the next letter, joins on its right: to the letter before) of a codepoint's form."""
    info = AF.presentation_info()
    if cp == AF.TATWEEL:
        return True, True
    if cp not in info:
        return False, False
    letter, form = info[cp]
    if cp in [x for pair in AF.LAM_ALEF.values() for x in pair]:
        return False, form == AF.FIN
    return form in (AF.INI, AF.MED), form in (AF.FIN, AF.MED)


def make_cells(t, forms, style, weight):
    gsset = t.getGlyphSet()
    hmtx = t["hmtx"]
    alef = bounds(gsset, forms[0xFE8D])
    scale = style["cap_height_px"] * ALEF_TO_CAP / alef[3]
    baseline = style["baseline_row"] + 0.5 + 0.5    # the Latin fill's last row is the baseline row: the ink sits on its bottom
    tatweel = forms[AF.TATWEEL]
    t_adv = hmtx[tatweel][0] * scale
    ink_low = min(bounds(gsset, forms[c])[1] for c in DRAWN())
    # an open side holds the outline and the shadow (its offset and softness), nothing more: the gaps between the letters of
    # a word are the typeface's own bearings
    pad_x = int(np.ceil(style["outline_px"] + style["shadow_offset_px"][0] + style["shadow_blur_px"]))
    below = -ink_low * scale + style["outline_px"] + style["shadow_offset_px"][1] + 2 * style["shadow_blur_px"]
    cell_h = int(np.ceil(baseline + below)) + 1
    cells = []
    for cp in DRAWN():
        name = forms[cp]
        adv = hmtx[name][0] * scale
        x0, y0, x1, y1 = bounds(gsset, name)
        jl, jr = joins(cp)
        # the cell in glyph-origin pixels: joined sides end where the typeface joins (0 / the advance), open sides hold the ink,
        # the outline and the shadow
        left = 0.0 if jl else min(0.0, x0 * scale - style["outline_px"])
        right = adv if jr else max(adv, x1 * scale + pad_x)
        width = int(round(right - left))
        if width <= 0:
            width = 1
        margin = 8
        canvas_w = width + 2 * margin + int(2 * t_adv) + 2
        origin = margin + int(np.ceil(t_adv)) - left
        placements = [(name, origin, baseline, scale)]
        if jl:
            placements.append((tatweel, origin - t_adv, baseline, scale))
            placements.append((tatweel, origin - 2 * t_adv, baseline, scale))
        if jr:
            placements.append((tatweel, origin + adv, baseline, scale))
            placements.append((tatweel, origin + adv + t_adv, baseline, scale))
        cov = raster.coverage(gsset, placements, canvas_w, cell_h)
        rgba = compose(style, cov)
        cx = int(round(origin + left))
        cells.append({"cp": cp, "name": name, "rgba": rgba[:, cx:cx + width], "joins": (jl, jr), "advance_px": adv})
    space = t.getBestCmap().get(0x20)
    space_w = max(1, int(round(hmtx[space][0] * scale))) if space else 8
    cells.append({"cp": 0x20, "name": space or "space", "rgba": np.zeros((cell_h, space_w, 4)), "joins": (False, False),
                  "advance_px": space_w, "empty": True})
    return cells, {"scale_px_per_unit": scale, "cell_height_px": cell_h, "weight": weight, "space_px": space_w}


def choose_weight(path, style):
    """The weight whose joining stroke (tatweel thickness) at the cap-height scale is the Latin stroke (measured)."""
    best = None
    for w in range(400, 801, 25):
        t = instance(path, w)
        forms = gsub_forms(t)
        g = t.getGlyphSet()
        alef = bounds(g, forms[0xFE8D])
        tat = bounds(g, forms[AF.TATWEEL])
        stroke = (tat[3] - tat[1]) * style["cap_height_px"] * ALEF_TO_CAP / alef[3]
        d = abs(stroke - style["stroke_px"])
        if best is None or d < best[0]:
            best = (d, w, stroke)
    return best[1], best[2]


# ---- the pages ---------------------------------------------------------------------------------------------------------
def pack(cells, cell_h):
    """Shelf packing into PAGE_W x PAGE_H_MAX pages (rows of cells, a texel apart): [(page, x, y)] per cell, and each page's
    height (the smallest power of two that holds its rows: a texture's sides are powers of two)."""
    places, page, x, y = [], 0, 0, 0
    used = [0]
    for c in cells:
        if c.get("empty"):
            places.append((0, 0, 0))
            continue
        w = c["rgba"].shape[1]
        if x + w > PAGE_W:
            x, y = 0, y + cell_h + 1
        if y + cell_h > PAGE_H_MAX:
            page, x, y = page + 1, 0, 0
            used.append(0)
        places.append((page, x, y))
        used[page] = max(used[page], y + cell_h)
        x += w + 1
    heights = []
    for u in used:
        h = 64
        while h < u:
            h *= 2
        heights.append(h)
    return places, heights


def quantise(pages_rgba):
    """One 256-colour palette per page (GS scale RGBA), the indices: PIL's octree over the 8-bit-scaled picture."""
    from PIL import Image
    out = []
    for rgba in pages_rgba:
        img8 = np.clip(np.rint(rgba * [2, 2, 2, 2]), 0, 255).astype(np.uint8)
        img = Image.fromarray(img8, "RGBA")
        q = img.quantize(colors=256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
        pal = q.getpalette(rawmode="RGBA")[:256 * 4]
        pal += [0] * (256 * 4 - len(pal))
        palette = [tuple(min(0x80, (v + 1) // 2) for v in pal[i * 4:i * 4 + 4]) for i in range(256)]
        idx = np.array(q, dtype=np.uint8)
        back = np.array([palette[i] for i in range(256)], dtype=float)[idx]
        want = np.clip(rgba, 0, 128)
        # the error as drawn: premultiplied colour (a transparent texel's colour is never seen) and alpha
        err = np.concatenate([np.abs(back[:, :, :3] * back[:, :, 3:] - want[:, :, :3] * want[:, :, 3:]) / 128.0,
                              np.abs(back[:, :, 3:] - want[:, :, 3:])], axis=2)
        out.append((idx, palette, float(err.max()), float(err.mean())))
    return out


def page_material(style, page):
    m = style["page_material"]
    shader = {"type": m["shader_type"], "extra": b"", "settings": list(m["settings"]), "lod": tuple(m["lod"]),
              "leftover": bytes(16), "colour": tuple(m["colour"]), "scroll": (0.0, 0.0, 0.0, 0.0),
              "texture": TEXTURE_ID + page, "type2": m["shader_type"]}
    return {"activated": m["activated"], "bucket": m["bucket"], "name": b"ArabicFont%d.tga\x00" % page, "shaders": [shader]}


def build(style, cells, meta):
    cell_h = meta["cell_height_px"]
    places, heights = pack(cells, cell_h)
    npages = len(heights)
    if npages > 3:
        sys.exit("build_arabic_font: %d pages of %dx%d: more than a font's three" % (npages, PAGE_W, PAGE_H_MAX))
    canvases = [np.zeros((h, PAGE_W, 4)) for h in heights]
    glyphs = []
    for c, (page, x, y) in zip(cells, places):
        rgba = c["rgba"]
        h, w = rgba.shape[:2]
        if c.get("empty"):
            glyphs.append({"page": 0, "u": 0.5 * 16.0, "v": 0.5 * 16.0, "w": w * 16.0, "h": h * 16.0})
            continue
        ph = heights[page]
        # stored upside down, as the disc's pages are: the quad samples v from the cell's top row down to v - height
        canvases[page][ph - y - h:ph - y, x:x + w] = rgba[::-1]
        top_storage_row = ph - 1 - y
        glyphs.append({"page": page + 1, "u": (x + 0.5) * 16.0, "v": (top_storage_row + 0.5) * 16.0, "w": w * 16.0, "h": h * 16.0})
    pages, report = [], []
    for i, (idx, palette, emax, emean) in enumerate(quantise(canvases)):
        image = gs.encode_page(PAGE_W, heights[i], idx.reshape(-1).tobytes(), palette)
        header = gs.make_header(PAGE_W, heights[i])
        pages.append({"texture_id": TEXTURE_ID + i, "material_id": MATERIAL_ID + i, "header": header,
                      "leftover": bytes(gs.LEFTOVER_SIZE), "image": image, "material": page_material(style, i)})
        report.append({"page": i, "size": [PAGE_W, heights[i]], "palette_error_max": round(emax, 2), "palette_error_mean": round(emean, 3)})
    data = psf.write_psf(pages, 0, glyphs, [c["cp"] for c in cells])
    return data, canvases, report


def preview(out_dir, canvases, cells, style, iso):
    """Pictures for the report (OUTSIDE the repository): the pages, and a line of Latin (the disc's glyphs, read to
    compare only) beside a line of ours on one baseline."""
    from PIL import Image
    out_dir = os.path.abspath(out_dir)
    if out_dir == ROOT or out_dir.startswith(ROOT + os.sep):
        sys.exit("build_arabic_font: --preview must be outside the repository")
    os.makedirs(out_dir, exist_ok=True)
    for i, cv in enumerate(canvases):
        img = np.clip(np.rint(cv[::-1] * 2), 0, 255).astype(np.uint8)
        Image.fromarray(img, "RGBA").resize((img.shape[1] * 3, img.shape[0] * 3), Image.NEAREST).save(os.path.join(out_dir, "arabic_page%d.png" % i))
    by_cp = {c["cp"]: c for c in cells}
    words = ["كراش بانديكوت", "خيارات اللعبة", "لعبة جديدة ١٢٣"]
    line_h = max(c["rgba"].shape[0] for c in cells)
    strip = []
    latin = None
    if iso:
        import measure_style
        from twin_disc import Disc
        disc = Disc(iso)
        font = psf.read_psf(disc.archive_file(measure_style.FONT))
        pages = [gs.decode_page(p["header"], p["image"])[2] for p in font["pages"]]
        cols = []
        for ch in "crash bandicoot":   # the game's texts are lower case (its font draws them as capitals)
            g = font["glyphs"][ord(ch) - font["first"]]
            if g["page"] == 0:
                cols.append(np.zeros((int(g["h"] / 16), int(g["w"] / 16), 4)))
                continue
            grid = measure_style.glyph_grid(font, pages, ch)
            cols.append(np.array(grid, dtype=float))
        latin = np.concatenate(cols, axis=1)
    for wd in words:
        shaped = AF.shape_word_forms([ord(ch) for ch in wd])
        # visual order: the line reversed, a run of digits kept left to right
        runs, cur = [], []
        for cp in shaped:
            digit = 0x0660 <= cp <= 0x0669 or 0x30 <= cp <= 0x39
            if cur and digit != cur[0][0]:
                runs.append(cur)
                cur = []
            cur.append((digit, cp))
        if cur:
            runs.append(cur)
        visual = []
        for run in reversed(runs):
            visual += [cp for _, cp in (run if run[0][0] else reversed(run))]
        parts = []
        for cp in visual:
            if cp == 0x20:
                parts.append(np.zeros((line_h, 16, 4)))
            elif 0x30 <= cp <= 0x39 or cp in by_cp:
                c = by_cp.get(cp) or by_cp[0x0660 + cp - 0x30]
                rgba = c["rgba"]
                pad = np.zeros((line_h - rgba.shape[0], rgba.shape[1], 4))
                parts.append(np.concatenate([rgba, pad], axis=0))
        strip.append(np.concatenate(parts, axis=1))
    if latin is not None:
        pad = np.zeros((line_h - latin.shape[0], latin.shape[1], 4))
        strip.insert(0, np.concatenate([latin, pad], axis=0))
    width = max(s.shape[1] for s in strip) + 20
    sheet = np.zeros((line_h * len(strip) + 10 * len(strip), width, 4))
    for i, s in enumerate(strip):
        sheet[i * (line_h + 10):i * (line_h + 10) + line_h, 10:10 + s.shape[1]] = s
    bg = np.zeros_like(sheet)
    bg[:, :] = [20, 30, 80, 128]
    a = sheet[:, :, 3:4] / 128.0
    comp = sheet[:, :, :3] * a + bg[:, :, :3] * (1 - a)
    img = np.clip(np.rint(comp * 2), 0, 255).astype(np.uint8)
    Image.fromarray(img, "RGB").resize((width * 4, sheet.shape[0] * 4), Image.NEAREST).save(os.path.join(out_dir, "side_by_side.png"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", help="measure the style from this disc (else --style)")
    ap.add_argument("--style", default=DEFAULT_STYLE)
    ap.add_argument("--cache", default=os.path.join(ROOT, "build", "font-cache"))
    ap.add_argument("--weight", default="auto")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--preview")
    ap.add_argument("--check", action="store_true", help="build in memory and compare with --out (exit 1 when it differs)")
    a = ap.parse_args()
    style = json.load(open(a.style))
    if a.iso:
        import subprocess
        with tempfile.TemporaryDirectory() as tmp:
            measured = os.path.join(tmp, "style.json")
            subprocess.run([sys.executable, os.path.join(HERE, "measure_style.py"), "--iso", a.iso, "--out", measured],
                           check=True, stdout=subprocess.DEVNULL)
            m = json.load(open(measured))
        for k, v in m.items():
            style[k] = v
    path = fetch_typeface(a.cache)
    if a.weight == "auto":
        weight, stroke = choose_weight(path, style)
    else:
        weight, stroke = int(a.weight), None
    t = instance(path, weight)
    forms = gsub_forms(t)
    cells, meta = make_cells(t, forms, style, weight)
    data, canvases, report = build(style, cells, meta)
    summary = {"typeface": TYPEFACE["name"], "weight": weight, "stroke_px": stroke, **meta, "glyphs": len(cells),
               "pages": len(canvases), "bytes": len(data), "palette": report,
               "sha256": hashlib.sha256(data).hexdigest()}
    print(json.dumps(summary, indent=1))
    if a.preview:
        preview(a.preview, canvases, cells, style, a.iso)
    if a.check:
        old = open(a.out, "rb").read() if os.path.exists(a.out) else b""
        if old != data:
            print("build_arabic_font: %s differs from the build" % a.out)
            return 1
        print("build_arabic_font: %s is the build's" % a.out)
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(data)
    print("build_arabic_font: wrote %s (%d bytes)" % (a.out, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
