#!/usr/bin/env python3
"""ocean_seam_judge.py -- the intro scene's ocean has no straight seams.

The title chunk's "Three years ago..." scene (GameController::Title: the ice block drifting on the ocean) is water drawn
as placed tiles of two submodels each -- a blue base and a lighter MIX-blended sheen 0.19 units above it -- with a scrolled
texture at a grazing angle. The port divided every vertex's S and T by its Q and handed the engine plain u / v with q = 1,
so the GPU interpolated the texture AFFINELY across each triangle (flat in screen space) where the GS interpolates S, T
and Q linearly and divides PER PIXEL (perspective-correct). At a grazing angle the affine mapping bends the smooth sheen
texture differently in every triangle: a lighter/darker region ending on a straight triangle edge -- the "hard angular
seams" of the report. The judge measures, in fixed presented frames of the deterministic idle title run, the
longest THIN STRAIGHT luminance edge inside the water (the water mask from the frame's own blue range, the horizon's sheen
band and a margin excluded): Sobel on the luminance, pixels over `grad_threshold` luminance/px, 8-connected components,
each fitted by PCA -- a component is a straight seam when its extent along its major axis is >= N px and its RMS thickness
is under `max_width` px. GREEN when no judged frame has such a seam. N comes from the oracle frames at the matching
boundaries (the longest thin straight edge the oracle's own water shows, times `oracle_margin`), else the fixture's frozen
`max_seam_px`.

Usage: ocean_seam_judge.py --bin BIN --iso IMAGE [--fixture F] [--out DIR] [--oracle-dir DIR | --no-oracle]
       ocean_seam_judge.py --dir DUMPDIR [--fixture F] [...]            judge an existing dump
Exit 0 = GREEN, 1 = RED, 2 = the run failed / nothing to judge.
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "intro_ocean_pal.json")


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    mc, dump = os.path.join(out, "mc"), os.path.join(out, "dump")
    for d in (mc, dump):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 600))] + fx.get("args", [])
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_DUMP_DIR=dump, TE_GFX_DUMP_EVERY=str(fx["dump_every"]))
    log = os.path.join(out, "run.log")
    print("ocean_seam_judge: running", " ".join(args), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("ocean_seam_judge: exit", rc, "->", log, flush=True)
    wanted = {fr["present"] for fr in fx["frames"]}
    for n in os.listdir(dump):
        m = re.match(r"frame_(\d+)\.ppm$", n)
        if m and int(m.group(1)) not in wanted:
            os.remove(os.path.join(dump, n))
    return dump, rc


def read_image(path, flipped=False):
    """An RGB float array (H, W, 3) of a binary PPM (P6, 8-bit) or, with Pillow available, any image; `flipped` = the rows
    are bottom-up (the recomp oracle's --dump-frames) and are turned top-down."""
    import numpy as np
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if m:
        w, h = int(m.group(1)), int(m.group(2))
        a = np.frombuffer(data[m.end():m.end() + w * h * 3], dtype=np.uint8).reshape(h, w, 3)
    else:
        from PIL import Image  # a PNG / BMP from PCSX2
        im = Image.open(path).convert("RGB")
        a = np.asarray(im)
    a = a.astype(np.float32)
    return a[::-1].copy() if flipped else a


def water_mask(a, fx):
    """The frame's water: saturated blue (the HSV hue window of the fixture, blue well over red), as the oracle's water is."""
    import numpy as np
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    mx, mn = a.max(-1), a.min(-1)
    v = mx / 255.0
    s = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1.0), 0.0)
    d = np.maximum(mx - mn, 1e-6)
    h = np.where(mx == b, (4 + (r - g) / d) / 6, np.where(mx == g, (2 + (b - r) / d) / 6, ((g - b) / d / 6) % 1.0))
    w = fx["water"]
    return (s > w["min_s"]) & (v > w["min_v"]) & (h > w["hue"][0]) & (h < w["hue"][1]) & (b > r + w["blue_over_red"])


def erode(m, n):
    import numpy as np
    out = m.copy()
    for _ in range(n):
        p = np.pad(out, 1)
        out = p[1:-1, 1:-1] & p[:-2, 1:-1] & p[2:, 1:-1] & p[1:-1, :-2] & p[1:-1, 2:]
    return out


def sobel(lum):
    import numpy as np
    p = np.pad(lum, 1, mode="edge")
    gx = (p[:-2, 2:] + 2 * p[1:-1, 2:] + p[2:, 2:]) - (p[:-2, :-2] + 2 * p[1:-1, :-2] + p[2:, :-2])
    gy = (p[2:, :-2] + 2 * p[2:, 1:-1] + p[2:, 2:]) - (p[:-2, :-2] + 2 * p[:-2, 1:-1] + p[:-2, 2:])
    return np.hypot(gx, gy) / 8.0


def components(mask):
    """8-connected components of a boolean image: a label array and the count (scipy when present, else a flood fill)."""
    import numpy as np
    try:
        from scipy import ndimage
        lab, n = ndimage.label(mask, structure=np.ones((3, 3), dtype=int))
        return lab, int(n)
    except ImportError:
        h, w = mask.shape
        lab = np.zeros(mask.shape, dtype=np.int32)
        n = 0
        ys, xs = np.nonzero(mask)
        for sy, sx in zip(ys.tolist(), xs.tolist()):
            if lab[sy, sx]:
                continue
            n += 1
            stack = [(sy, sx)]
            lab[sy, sx] = n
            while stack:
                y, x = stack.pop()
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        yy, xx = y + dy, x + dx
                        if 0 <= yy < h and 0 <= xx < w and mask[yy, xx] and not lab[yy, xx]:
                            lab[yy, xx] = n
                            stack.append((yy, xx))
        return lab, n


def straight_edges(a, fx):
    """The frame's thin straight luminance edges inside the water: [(length_px, width_px, pixels, cx, cy)] longest first,
    plus the mask's facts (horizon row, bottom row, mask pixels, the gradient's maximum inside the mask)."""
    import numpy as np
    lum = 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]
    m = water_mask(a, fx)
    rows = np.nonzero(m.mean(1) > 0.5)[0]
    if len(rows) < 20:
        return [], {"error": "no water band (%d rows over half water)" % len(rows)}
    horizon, bottom = int(rows[0]), int(rows[-1])
    m[: horizon + int((bottom - horizon) * fx["horizon_skip"])] = False   # the horizon's own sheen band
    m = erode(m, fx.get("erode", 3))
    g = sobel(lum)
    edges = (g > fx["grad_threshold"]) & m
    lab, n = components(edges)
    out = []
    for i in range(1, n + 1):
        ys, xs = np.nonzero(lab == i)
        if len(xs) < fx.get("min_pixels", 6):
            continue
        pts = np.stack([xs, ys], 1).astype(np.float64)
        c = pts.mean(0)
        q = pts - c
        cov = q.T @ q / len(q)
        w, vec = np.linalg.eigh(cov)
        ext = q @ vec[:, 1]
        thick = q @ vec[:, 0]
        length = float(ext.max() - ext.min())
        width = float(math.sqrt(max(0.0, float(np.mean(thick ** 2)))) * 2.0 * 1.7)   # ~ the full width of a uniform band
        out.append((length, width, int(len(xs)), int(c[0]), int(c[1])))
    out.sort(reverse=True)
    facts = {"horizon": horizon, "bottom": bottom, "mask_px": int(m.sum()), "grad_max": round(float(g[m].max()), 2) if m.any() else 0.0}
    return out, facts


def longest_straight(edges, fx):
    thin = [e for e in edges if e[1] < fx["max_width"]]
    return thin[0] if thin else None


def judge(dump, fx, oracle_dir):
    reasons, rows = [], []
    for fr in fx["frames"]:
        path = os.path.join(dump, "frame_%06d.ppm" % fr["present"])
        if not os.path.exists(path):
            reasons.append("present %d not dumped" % fr["present"])
            continue
        edges, facts = straight_edges(read_image(path), fx)
        if "error" in facts:
            reasons.append("present %d: %s" % (fr["present"], facts["error"]))
            continue
        limit = fx["max_seam_px"]
        row = {"present": fr["present"], "t_s": fr.get("t_s"), **facts}
        ofr = fr.get("oracle_frame")
        opath = os.path.join(oracle_dir, ofr) if (oracle_dir and ofr) else None
        if opath and os.path.exists(opath):
            oedges, ofacts = straight_edges(read_image(opath, fx.get("oracle", {}).get("flipped", False)), fx)
            olong = longest_straight(oedges, fx)
            row["oracle_longest_px"] = round(olong[0], 1) if olong else 0.0
            row["oracle_grad_max"] = ofacts.get("grad_max")
            limit = max(fx["min_seam_px"], math.ceil((olong[0] if olong else 0.0) * fx["oracle_margin"]))
        elif opath:
            print("ocean_seam_judge: oracle frame %s missing -- the frozen max_seam_px gates" % opath)
        longest = longest_straight(edges, fx)
        row["limit_px"] = limit
        row["longest_seam_px"] = round(longest[0], 1) if longest else 0.0
        if longest:
            row["seam_at"] = [longest[3], longest[4]]
            row["seam_width_px"] = round(longest[1], 1)
        row["thin_edges_over_20px"] = [[round(e[0]), round(e[1], 1), e[3], e[4]] for e in edges if e[1] < fx["max_width"] and e[0] >= 20][:6]
        if longest and longest[0] >= limit:
            reasons.append("present %d (t~%s s): a straight luminance edge %.0f px long (width %.1f) at (%d %d) inside the water, limit %d px" %
                           (fr["present"], fr.get("t_s"), longest[0], longest[1], longest[3], longest[4], limit))
        rows.append(row)
    for r in rows:
        print("ocean_seam_judge:", json.dumps(r))
    if not rows:
        print("ocean_seam_judge: nothing to judge")
        return 2
    if reasons:
        for r in reasons:
            print("  RED:", r)
        print("ocean_seam_judge: RED")
        return 1
    print("ocean_seam_judge: GREEN (%d frames)" % len(rows))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="an existing dump directory")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the run's directory (default: build/ocean_seam_judge)")
    ap.add_argument("--oracle-dir", help="the directory of the oracle frames the fixture's frames name (default: the fixture's; a PCSX2 dump dir replaces it with --oracle-top-down)")
    ap.add_argument("--oracle-top-down", action="store_true", help="the oracle frames are top-down (PCSX2 dumps; the recomp oracle's are bottom-up)")
    ap.add_argument("--no-oracle", action="store_true", help="gate by the fixture's frozen max_seam_px only")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    oracle_dir = None if a.no_oracle else (a.oracle_dir or fx.get("oracle", {}).get("dir"))
    if a.oracle_top_down:
        fx.setdefault("oracle", {})["flipped"] = False
    if oracle_dir and not os.path.isdir(oracle_dir):
        print("ocean_seam_judge: oracle dir %s missing -- the frozen max_seam_px gates" % oracle_dir)
        oracle_dir = None
    elif oracle_dir:
        print("ocean_seam_judge: oracle frames from", oracle_dir, "flipped", fx.get("oracle", {}).get("flipped", False))
    if a.dir:
        return judge(a.dir, fx, oracle_dir)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir)")
    out = a.out or os.path.join(ROOT, "build", "ocean_seam_judge")
    dump, rc = run(a.bin, a.iso, out, fx)
    if rc != 0:
        print("ocean_seam_judge: the run exited %d" % rc)
    return judge(dump, fx, oracle_dir)


if __name__ == "__main__":
    sys.exit(main())
