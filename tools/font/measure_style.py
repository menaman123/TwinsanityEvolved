#!/usr/bin/env python3
"""measure_style.py -- MEASURE the lettering style of the game's own font (Startup\\Fonts\\Crash_Euro.psf on the disc).

Reads the disc at run time, decodes the font's pages (gs.py) and measures the Latin capitals and digits: the glyph cell, the
baseline and cap height, the fill's stroke width, the light outline (width, colour), the drop shadow (offset, softness, alpha)
and the fill's vertical gradient. Prints JSON (numbers only) -- the Arabic glyph pipeline (build_arabic_font.py) styles OUR
glyphs from these numbers. Nothing from the disc is written anywhere: no pixels, no glyphs. --preview DIR writes enlarged
pictures of the measured Latin glyphs for a report OUTSIDE the repository (refused inside it).

    measure_style.py --iso IMAGE [--out style.json] [--preview DIR]
"""
import argparse
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "text"))
import gs  # noqa: E402
import psf  # noqa: E402
from twin_disc import Disc  # noqa: E402

FONT = "Startup\\Fonts\\Crash_Euro.psf"
# capitals without descenders or accents, and the digits: the letters the measurements are taken over
LETTERS = "ABDEFHIKLMNPRTUVWXZ"
STEMS = "IHLTEF"


def classify(p):
    """A texel's role: fill (saturated: red full, blue none), light (the outline: every channel high), dark (the shadow),
    or empty. GS colour scale: 0x80 = full."""
    r, g, b, a = p
    if a < 8:
        return " "
    if r >= 0x60 and g >= 0x60 and b >= 0x50:
        return "o"
    if r >= 0x60 and b < 0x30:
        return "#"
    if r + g + b < 0x50:
        return "s"
    return "."


def glyph_grid(font, pages, ch):
    """Rows (top first, as the quad draws it: texel row v-1-r) of RGBA tuples of a character's glyph cell."""
    g = font["glyphs"][ord(ch) - font["first"]]
    page = pages[g["page"] - 1]
    u0, top = int(g["u"] / 16), int(g["v"] / 16 - 0.5)
    w, h = int(g["w"] / 16), int(g["h"] / 16)
    rows = []
    for r in range(h):
        y = top - r
        rows.append([tuple(page[((y * 128) + u0 + x) * 4:((y * 128) + u0 + x) * 4 + 4]) for x in range(w)])
    return rows


def runs(line, cls):
    out, n = [], 0
    for c in line:
        if c == cls:
            n += 1
        elif n:
            out.append(n)
            n = 0
    if n:
        out.append(n)
    return out


def page_material(font):
    """The last page's material settings (the pages after the first share them; the first differs only in the unused alpha
    test's method / reference): shader type, the 30 setting fields, TEX1 K/L, the colour, the bucket and the key."""
    m = font["pages"][-1]["material"]
    s = m["shaders"][0]
    names = ["blends", "preset", "alphaTest", "alphaMethod", "alphaReference", "alphaFail", "destinationTest", "destinationMode",
             "depthTest", "unused23", "gouraud", "textured", "stq", "fog", "secondContext", "uScroll", "vScroll", "ownAlpha",
             "alphaA", "alphaB", "alphaC", "alphaD", "alphaFix", "linear", "noFba", "unused57", "antiAliased", "noDepthWrites",
             "animatedColour", "hasAnimation"]
    return {"shader_type": s["type"], "settings": s["settings"], "settings_named": dict(zip(names, s["settings"])),
            "lod": list(s["lod"]), "colour": list(s["colour"]), "bucket": m["bucket"], "activated": m["activated"]}


def gaussian_blur(a, sigma):
    """Separable Gaussian blur of a 2-D float array (numpy), radius 3 sigma."""
    import numpy as np
    if sigma <= 0:
        return a
    r = max(1, int(3 * sigma + 0.5))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    pad = np.pad(a, r)
    tmp = np.apply_along_axis(lambda row: np.convolve(row, k, mode="valid"), 1, pad)
    return np.apply_along_axis(lambda col: np.convolve(col, k, mode="valid"), 0, tmp)


def shift(a, dx, dy):
    import numpy as np
    out = np.zeros_like(a)
    h, w = a.shape
    out[dy:, dx:] = a[:h - dy, :w - dx]
    return out


def fit_shadow(font, pages):
    """The drop shadow as the game's artist made it, as a model: shadow alpha = strength * blur_sigma(glyph coverage shifted
    by (dx, dy)), fitted (least squares) at the texels outside the glyph over the measured letters."""
    import numpy as np
    samples = []
    for ch in LETTERS + "0123456789":
        grid = glyph_grid(font, pages, ch)
        h, w = len(grid), len(grid[0])
        cov = np.zeros((h + 8, w + 8))
        dark = np.full((h + 8, w + 8), np.nan)
        for r in range(h):
            for x in range(w):
                c = classify(grid[r][x])
                a = grid[r][x][3] / 128.0
                if c in "#o.":
                    cov[r + 4, x + 4] = a
                elif c in " s":
                    dark[r + 4, x + 4] = a
        samples.append((cov, dark))
    best = None
    for dx in range(0, 5):
        for dy in range(0, 6):
            for sigma in (0.0, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0):
                num = den = 0.0
                preds = []
                for cov, dark in samples:
                    p = gaussian_blur(shift(cov, dx, dy), sigma)
                    m = ~np.isnan(dark)
                    preds.append((p[m], dark[m]))
                    num += float((p[m] * dark[m]).sum())
                    den += float((p[m] * p[m]).sum())
                strength = min(1.0, num / den) if den else 0.0
                err = sum(float(((strength * p - d) ** 2).sum()) for p, d in preds)
                n = sum(len(d) for _, d in preds)
                rms = (err / n) ** 0.5 if n else 0.0
                if best is None or rms < best["rms"]:
                    best = {"dx": dx, "dy": dy, "sigma": sigma, "strength": round(strength, 3), "rms": round(rms, 4)}
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out")
    ap.add_argument("--preview")
    a = ap.parse_args()
    disc = Disc(a.iso)
    data = disc.archive_file(FONT)
    if data is None:
        sys.exit("measure_style: the disc has no %s" % FONT)
    font = psf.read_psf(data)
    pages = [gs.decode_page(p["header"], p["image"])[2] for p in font["pages"]]

    cell = int(font["glyphs"][ord("A") - font["first"]]["h"] / 16)
    tops, bottoms, stems, outline_runs, lights, dark_alpha = [], [], [], [], [], []
    rows_fill = {}
    offsets = []
    for ch in LETTERS + "0123456789":
        grid = glyph_grid(font, pages, ch)
        classes = ["".join(classify(p) for p in row) for row in grid]
        fill_rows = [r for r, line in enumerate(classes) if "#" in line]
        if not fill_rows:
            continue
        if ch in LETTERS:
            tops.append(fill_rows[0])
            bottoms.append(fill_rows[-1])
        if ch in STEMS:
            mid = fill_rows[0] + (fill_rows[-1] - fill_rows[0]) // 2
            for r in range(mid - 3, mid + 4):
                rs = runs(classes[r], "#")
                if rs:
                    stems.append(min(rs))
        for r, line in enumerate(classes):
            # the outline: the light run on each side of a fill run
            for side in (line, line[::-1]):
                i = side.find("#")
                if i > 0:
                    j = i - 1
                    n = 0
                    while j >= 0 and side[j] == "o":
                        n += 1
                        j -= 1
                    if n:
                        outline_runs.append(n)
            for x, c in enumerate(line):
                p = grid[r][x]
                if c == "o":
                    lights.append(p)
                if c == "#":
                    rows_fill.setdefault(r, []).append(p)
                if c == "s":
                    dark_alpha.append(p[3])
        # the shadow's offset: the shift (dx, dy) that best lays the glyph's own shape (fill + outline) over its dark texels
        shape = {(x, r) for r, line in enumerate(classes) for x, c in enumerate(line) if c in "#o"}
        dark = {(x, r) for r, line in enumerate(classes) for x, c in enumerate(line) if c == "s"}
        if dark:
            best = None
            for dy in range(0, 5):
                for dx in range(0, 5):
                    shifted = {(x + dx, y + dy) for x, y in shape}
                    score = len(shifted & dark) - 0.5 * len(dark - shifted - shape)
                    if best is None or score > best[0]:
                        best = (score, dx, dy)
            offsets.append((best[1], best[2]))

    shadow_fit = fit_shadow(font, pages)
    cap_top = statistics.median(tops)
    baseline = statistics.median(bottoms)
    # the fill's colour down the letter: per row from the cap top to the baseline, the median of each channel
    gradient = []
    for r in range(int(cap_top), int(baseline) + 1):
        ps = rows_fill.get(r, [])
        if len(ps) < 4:
            continue
        gradient.append({"t": round((r - cap_top) / max(1, baseline - cap_top), 3),
                         "rgb": [int(statistics.median(p[i] for p in ps)) for i in range(3)]})
    light = [int(statistics.median(p[i] for p in lights)) for i in range(3)] if lights else None
    light_pure = [sorted(p[i] for p in lights)[int(len(lights) * 0.9)] for i in range(3)] if lights else None
    dx = statistics.median(o[0] for o in offsets) if offsets else 0
    dy = statistics.median(o[1] for o in offsets) if offsets else 0
    style = {
        "font": FONT,
        "colour_scale": "GS (0x80 = full; the game's text is drawn MODULATE with the item's colour)",
        "cell_height_px": cell,
        "cap_top_row": cap_top,
        "baseline_row": baseline,
        "cap_height_px": baseline - cap_top + 1,
        "below_baseline_px": cell - 1 - baseline,
        "stroke_px": statistics.median(stems) if stems else None,
        "outline_px": statistics.median(outline_runs) if outline_runs else None,
        "outline_rgb": light_pure,
        "outline_rgb_median": light,
        "shadow_offset_px": [shadow_fit["dx"], shadow_fit["dy"]],
        "shadow_blur_px": shadow_fit["sigma"],
        "shadow_strength": shadow_fit["strength"],
        "shadow_fit_rms": shadow_fit["rms"],
        "shadow_offset_overlap_px": [dx, dy],
        "shadow_rgb": [0, 0, 0],
        "shadow_alpha_max": max(dark_alpha) if dark_alpha else 0,
        "shadow_alpha_median": int(statistics.median(dark_alpha)) if dark_alpha else 0,
        "fill_gradient": [gradient[0], gradient[len(gradient) // 2], gradient[-1]] if gradient else [],
        "fill_gradient_rows": gradient,
        "letters_measured": LETTERS + "0123456789",
        # the render settings of the font's pages (the material's one shader: blending, tests, filtering; no pixels): our page
        # takes the same so its glyphs blend exactly as the game's do
        "page_material": page_material(font),
    }
    text = json.dumps(style, indent=1)
    print(text)
    if a.out:
        with open(a.out, "w") as f:
            f.write(text + "\n")
    if a.preview:
        out = os.path.abspath(a.preview)
        if out == ROOT or out.startswith(ROOT + os.sep):
            sys.exit("measure_style: --preview must be outside the repository (no disc pixels in the tree)")
        os.makedirs(out, exist_ok=True)
        from PIL import Image
        for ch in "CRASH":
            grid = glyph_grid(font, pages, ch)
            im = Image.new("RGBA", (len(grid[0]), len(grid)))
            im.putdata([(min(255, r * 2), min(255, g * 2), min(255, b * 2), min(255, al * 2)) for row in grid for r, g, b, al in row])
            im.resize((im.width * 8, im.height * 8), Image.NEAREST).save(os.path.join(out, "latin_%s.png" % ch))
    return 0


if __name__ == "__main__":
    sys.exit(main())
