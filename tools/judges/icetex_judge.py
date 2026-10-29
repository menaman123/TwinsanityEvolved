#!/usr/bin/env python3
"""icetex_judge.py -- Ice Climb's cave walls and the ripple rings around the floating ice against the EXACT GS depth test.

The report: in Ice Climb's iceberg cave (levels\\ice\\iceclimb\\bergint) the walls' textures and the water ripple around
the floating ice "freak out" when the camera moves. Both are passes drawn over a surface they nearly touch, depth test
GEQUAL, no depth write: the far walls' scrolled caustics (bucket 9, ~50 GS Z units in front of the wall) and the ripple
rings lying on the water (bucket 8). The GS compares the integer Z exactly (the title's ZBUF is PSMZ32 and its screen
matrix maps the clip volume onto Z = 0 .. 2^24); the port mapped Z to the window depth as z / 2^32, which kept a vertex's Z
to 128 units without clip control (the float -1..1 detour) and the depth attachment to 256: wherever the rounding put a
pass's vertex behind its surface, whole triangles of it failed the test, and which ones changed as the camera moved.

The judge runs the fixture's pad log on a card warped to the cave (the hub card made as the crystal judge makes it), once
per judged present, with that present's draws logged (TE_GFX_DIAG_PRESENT + TE_GFX_DIAG_VERTS: every emitted vertex's
screen position, GS Z, depth test / write), the judged buckets painted opaque magenta (TE_GFX_DIAG_MARK_BUCKET: the
pixels their draws WIN), the buckets drawn after them and the shadow / decal composites left out. The REFERENCE is the
GS's own test over the logged triangles: rasterised at the pixel centres, Z interpolated in float64, the buckets in order
(the depth writers before the judged ones), a judged bucket's pixel visible where its triangle passes GEQUAL / GREATER /
ALWAYS against the depth so far. Per judged bucket: its reference-visible pixels (eroded 1 px, minus the other judged
bucket's, minus the logged particle blocks' boxes: the particles are drawn after and outside the vertex log) are the
judged pixels; a judged pixel the picture does not show magenta (additive light over the mark still counts as magenta) is
MISSING. GREEN when every judged (present, bucket) has at least `min_judged` pixels and a missing share at most
`max_missing_share`.

Usage: icetex_judge.py --bin BIN --iso IMAGE [--fixture F] [--out DIR] [--keep]
       icetex_judge.py --dir RUNDIR [--fixture F]      judge existing runs (RUNDIR/p<present>/run.log + dump/)
Exit 0 = GREEN, 1 = RED, 2 = the run failed / nothing to judge. Writes OUT/report.json.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "icetex_pal.json")
VERT = re.compile(r"\[te\.gfx\.vert\] b(\d+) tex \d+ v (\d+) xy \((\S+) (\S+)\) z (\S+) .* zt (\d+) zw (\d+) bl \d+")
PARTICLES = re.compile(r"\[te\.gfx\.diag\] present (\d+) particles\S* .*? box \((-?\d+) (-?\d+)\)-\((-?\d+) (-?\d+)\)")
DEPTH_NEVER, DEPTH_ALWAYS, DEPTH_GEQUAL, DEPTH_GREATER = 0, 1, 2, 3


# ----------------------------------------------------------------------------- the runs
def make_card(binary, iso, out, fx):
    """The hub card the hub pad log writes (drawing off), warped to the fixture's chunk."""
    hub, card = os.path.join(out, "mc_hub"), os.path.join(out, "mc_card")
    for d in (hub, card):
        shutil.rmtree(d, ignore_errors=True)
    os.makedirs(hub)
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1")
    with open(os.path.join(out, "hub_card.log"), "w") as log:
        subprocess.call([binary, "--iso", iso, "--config", "none", "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies",
                         "--play-pad", os.path.join(ROOT, "fixtures/padlogs/hub_entry.padlog"), "--mc-dir", hub, "--stop-at", "2700"],
                        stdout=subprocess.DEVNULL, stderr=log, env=env, cwd=ROOT)
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools/warp_save.py"), "--card", hub, "--chunk", fx["chunk"], "--out", card,
                           "--place", "0xFFFF"], stdout=subprocess.DEVNULL, cwd=ROOT)
    return card


def run_present(binary, iso, out, card, fx, present):
    d = os.path.join(out, "p%d" % present)
    shutil.rmtree(d, ignore_errors=True)
    mc, dump = os.path.join(d, "mc"), os.path.join(d, "dump")
    shutil.copytree(card, mc)
    os.makedirs(dump)
    later = ",".join(str(b) for b in range(max(fx["buckets"]) + 1, fx["bucket_count"]))
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_DIAG_PRESENT=str(present), TE_GFX_DIAG_VERTS="1",
               TE_GFX_DIAG_MARK_BUCKET=",".join(str(b) for b in fx["buckets"]), TE_GFX_DIAG_SKIP_BUCKET=later,
               TE_GFX_NO_SHADOWS="1", TE_GFX_NO_DECALS="1", TE_GFX_DUMP_DIR=dump, TE_GFX_DUMP_AT=str(present))
    args = [binary, "--iso", iso, "--config", "none", "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(present + fx.get("stop_margin", 200)), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 900))]
    print("icetex_judge: present %d: %s" % (present, " ".join(args)), flush=True)
    with open(os.path.join(d, "run.log"), "w") as log:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=log, env=env, cwd=ROOT)
    print("icetex_judge: present %d: exit %d" % (present, rc), flush=True)
    return rc


# ----------------------------------------------------------------------------- the picture and the reference
def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise ValueError("%s: not a binary PPM" % path)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data, dtype=np.uint8, count=w * h * 3, offset=m.end()).reshape(h, w, 3).astype(np.int16)


def marked(img):
    """the judged buckets' magenta, additive light drawn over it (a particle, a glow) included"""
    r, g, b = img[..., 0], img[..., 1], img[..., 2]
    return (r > 200) & (b > 200) & (g < np.minimum(r, b) - 50)


def load_triangles(log, buckets_upto):
    """the logged present's triangles in emission order: (bucket, ztest, zwrite, [(x, y, z) x 3])"""
    tris, cur = [], []
    with open(log, errors="replace") as f:
        for line in f:
            m = VERT.search(line)
            if not m:
                continue
            b, i, x, y, z, zt, zw = m.groups()
            if int(i) == 0:
                cur = []
            cur.append((int(b), int(zt), int(zw), float(x), float(y), float(z)))
            if len(cur) % 3 == 0 and int(b) <= buckets_upto:
                t = cur[-3:]
                tris.append((t[0][0], t[0][1], t[0][2], [(v[3], v[4], v[5]) for v in t]))
    return tris


def raster(pts, w, h):
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = pts
    minx, maxx = max(0, int(np.floor(min(x0, x1, x2)))), min(w - 1, int(np.ceil(max(x0, x1, x2))))
    miny, maxy = max(0, int(np.floor(min(y0, y1, y2)))), min(h - 1, int(np.ceil(max(y0, y1, y2))))
    if minx > maxx or miny > maxy:
        return None
    area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if area == 0:
        return None
    ys, xs = np.mgrid[miny:maxy + 1, minx:maxx + 1]
    px, py = xs + 0.5, ys + 0.5
    w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area
    w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area
    w2 = 1.0 - w0 - w1
    inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
    if not inside.any():
        return None
    return ys[inside], xs[inside], (w0 * z0 + w1 * z1 + w2 * z2)[inside]


def reference(tris, buckets, w, h):
    """per judged bucket, where its draws pass the exact GS depth test (the buckets in order, each one's draws as logged)"""
    order = sorted(range(len(tris)), key=lambda k: (tris[k][0], k))
    depth = np.zeros((h, w), dtype=np.float64)   # the GS far plane: Z 0
    ref = {b: np.zeros((h, w), dtype=bool) for b in buckets}
    for k in order:
        b, zt, zw, pts = tris[k]
        r = raster(pts, w, h)
        if r is None:
            continue
        ys, xs, zs = r
        cur = depth[ys, xs]
        if zt == DEPTH_GEQUAL:
            passed = zs >= cur
        elif zt == DEPTH_GREATER:
            passed = zs > cur
        elif zt == DEPTH_ALWAYS:
            passed = np.ones_like(zs, dtype=bool)
        else:
            passed = np.zeros_like(zs, dtype=bool)
        if b in ref:
            ref[b][ys[passed], xs[passed]] = True
        if zw:
            depth[ys[passed], xs[passed]] = zs[passed]
    return ref


def erode(mask):
    m = mask.copy()
    m[1:, :] &= mask[:-1, :]
    m[:-1, :] &= mask[1:, :]
    m[:, 1:] &= mask[:, :-1]
    m[:, :-1] &= mask[:, 1:]
    return m


def judge_present(d, fx, present):
    log, frame = os.path.join(d, "run.log"), os.path.join(d, "dump", "frame_%06d.ppm" % present)
    if not os.path.exists(frame) or not os.path.exists(log):
        return None, ["present %d: no dump or log in %s" % (present, d)]
    img = read_ppm(frame)
    h, w = img.shape[:2]
    buckets = fx["buckets"]
    tris = load_triangles(log, max(buckets))
    if not tris:
        return None, ["present %d: no [te.gfx.vert] lines (a binary without TE_GFX_DIAG_VERTS?)" % present]
    ref = reference(tris, buckets, w, h)
    excl = np.zeros((h, w), dtype=bool)
    pad = fx.get("particle_pad", 4)
    with open(log, errors="replace") as f:
        for line in f:
            m = PARTICLES.search(line)
            if m and int(m.group(1)) == present:
                x0, y0, x1, y1 = (int(v) for v in m.groups()[1:])
                excl[max(0, y0 - pad):max(0, y1 + pad + 1), max(0, x0 - pad):max(0, x1 + pad + 1)] = True
    mag = marked(img)
    union = np.zeros((h, w), dtype=bool)
    for b in buckets:
        union |= ref[b]
    rows, reasons = [], []
    for b in buckets:
        others = np.zeros((h, w), dtype=bool)
        for o in buckets:
            if o != b:
                others |= ref[o]
        judged = erode(ref[b]) & ~excl & ~others
        n = int(judged.sum())
        missing = int((judged & ~mag).sum())
        share = missing / n if n else 0.0
        name = fx["names"].get(str(b), "bucket %d" % b)
        row = {"present": present, "bucket": b, "name": name, "triangles": len(tris), "judged": n, "missing": missing,
               "missing_share": round(share, 5)}
        rows.append(row)
        floor = fx["min_judged"].get(str(b), 1000)
        if n < floor:
            reasons.append("present %d %s: %d judged pixels < %d (not on screen)" % (present, name, n, floor))
        elif share > fx["max_missing_share"]:
            reasons.append("present %d %s: %d of %d judged pixels missing = %.2f %% > %.2f %% (the pass loses the depth test where "
                           "the GS's passes)" % (present, name, missing, n, 100 * share, 100 * fx["max_missing_share"]))
    extra = int((erode(mag) & ~union & ~excl).sum())
    rows.append({"present": present, "extra": extra, "note": "magenta (eroded) where no judged draw passes the exact test: reported"})
    return rows, reasons


def judge(rundir, fx, out):
    rows, reasons = [], []
    for p in fx["presents"]:
        r, why = judge_present(os.path.join(rundir, "p%d" % p), fx, p)
        reasons += why
        rows += r or []
    for r in rows:
        print("icetex_judge:", json.dumps(r))
    verdict = "nothing" if not any("judged" in r for r in rows) else "RED" if reasons else "GREEN"
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump({"verdict": verdict, "rows": rows, "reasons": reasons, "fixture": fx.get("name", "icetex_pal")}, f, indent=1)
    if verdict == "nothing":
        for r in reasons:
            print("  ", r)
        print("icetex_judge: nothing to judge")
        return 2
    if reasons:
        for r in reasons:
            print("  RED:", r)
        print("icetex_judge: RED")
        return 1
    judged = [r for r in rows if "judged" in r]
    print("icetex_judge: GREEN (%d present/bucket pairs, missing at most %.2f %%)"
          % (len(judged), 100 * max(r["missing_share"] for r in judged)))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="judge existing runs: DIR/p<present>/run.log and dump/")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the run's directory (default: build/icetex_judge)")
    ap.add_argument("--keep", action="store_true", help="keep the runs' logs and dumps (default: removed after judging)")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    if a.dir:
        return judge(a.dir, fx, a.dir)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir)")
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "icetex_judge"))
    os.makedirs(out, exist_ok=True)
    card = make_card(os.path.abspath(a.bin), a.iso, out, fx)
    for p in fx["presents"]:
        rc = run_present(os.path.abspath(a.bin), a.iso, out, card, fx, p)
        if rc != 0:
            print("icetex_judge: the run for present %d exited %d" % (p, rc))
    rc = judge(out, fx, out)
    if not a.keep:
        for p in fx["presents"]:
            shutil.rmtree(os.path.join(out, "p%d" % p, "mc"), ignore_errors=True)
            log = os.path.join(out, "p%d" % p, "run.log")
            if os.path.exists(log) and os.path.getsize(log) > 1 << 20:
                os.remove(log)   # the vertex log (tens of MB); the verdict and report.json stay
        for d in ("mc_hub", "mc_card"):
            shutil.rmtree(os.path.join(out, d), ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
