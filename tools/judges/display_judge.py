#!/usr/bin/env python3
"""display_judge.py -- does a DISPLAY setting change show in the PRESENTED frame, and do the wide shapes draw for their shape?

THE SHAPES (fixtures/judges/display_pal.json; deterministic headless runs of the title to the main menu, frames dumped every
100 presents): A with the default shape (`auto` = the frame's own pixels when headless: the 512x512 PAL frame fills the
capture), B with `--aspect 16:9` (the frame letterboxed to 16:9: a 512x288 picture at y 112 with black bars above and below)
and C with `--aspect 32:9` (a 512x144 picture at y 184). GREEN iff B's and C's bars (the rows above and below the picture)
are black (mean luminance <= `bar_max_luminance`) while A's same rows are NOT (the title's sky and sea: mean >=
`picture_min_luminance`), and B's and C's picture rows still hold the scene. A runner that refuses the shape (a binary
without display settings, or without 32:9) is RED: the setting does not exist, so no change can show.

THE WIDE SHAPES DRAWN FOR THEIR SHAPE (`wide_legs`): LOAD GAME on the hub card whose save has the game's own widescreen option
on (the card made from hub_entry.padlog, unless --hub-card names one; tools/warp_save.py --widescreen on), then the hub in play
and its pause page (fixtures/padlogs/hub_widescreen.padlog), at 16:9 and at each wide shape (21:9, 32:9), the frame TARGET
dumped (the game's 512-wide frame before the picture is shaped) and the pause page's strings logged (TE_GFX_DIAG_PRESENT).
Drawn for its own width, a shape A shows the 16:9 view in the middle share s = (16/9) / A of the frame and more of the world
at the sides; only stretched (the fix off), A's frame is the 16:9 frame. GREEN iff for every shape and present
  1. more of the world: the middle share of A's frame differs from the 16:9 frame squeezed to that share by a mean of at most
     `max_mad` per channel, and by less than 1 / `control_ratio` of what a stretched frame differs (the 16:9 frame's own
     middle against its squeezed self: the stretch control);
  2. the menus and the HUD in the middle 16:9: every string drawn at 16:9 and at A (same text, same order; the HUD's counters
     in play, the pause page) has its box at A where the 16:9 box moves to, squeezed towards the frame's middle by s (within
     `text_tolerance` pixels); and the platform's own fps counter (--fps-counter on) has its right edge where the 16:9 one's
     moves to;
  3. the particles as on a 16:9 TV: their quads (the blocks drawn in play, TE_GFX_DIAG_PARTICLES) s times as wide as at 16:9
     and as high (the median within `particle_tolerance`).

RED on bin/twinsanityevolved.32e1174 (no --aspect), RED on fixes 7276bc0 (no 32:9; 21:9 only stretched), GREEN on the
display-settings build with 32:9 and the wide shapes drawn for their shape.

Usage: display_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F] [--hub-card DIR]
Exit 0 = GREEN, 1 = RED, 2 = the run failed.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "display_pal.json")


def run(binary, iso, out, fx, extra, tag, padlog=None, stop_at=None, mc_from=None, env_extra=None):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    dump, mc = os.path.join(d, "dump"), os.path.join(d, "mc")
    os.makedirs(dump)
    if mc_from:
        shutil.copytree(mc_from, mc)   # a copy: the game's auto-save writes into the card
    else:
        os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, padlog or fx["padlog"]), "--stop-at", str(stop_at or fx["stop_at"]),
            "--mc-dir", mc, "--run-seconds", str(fx.get("run_seconds", 600))] + extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_EVERY"] = str(fx["dump_every"])
    env.update(env_extra or {})
    print("display_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("display_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return d, rc


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, memoryview(data)[pos:pos + 3 * w * h]


def band_luminance(path, y0f, y1f):
    w, h, px = read_ppm(path)
    y0, y1 = int(round(y0f * h)), int(round(y1f * h))
    total, n = 0.0, 0
    for y in range(y0, y1):
        row = px[y * w * 3:(y + 1) * w * 3]
        for i in range(0, len(row), 3):
            total += 0.299 * row[i] + 0.587 * row[i + 1] + 0.114 * row[i + 2]
            n += 1
    return total / n if n else 0.0


def shape_case(fa, fb, bands, picture, fx, label):
    """The letterbox of one shape: its bars black where the default's are the scene, its picture rows the scene."""
    fails, result = [], {}
    for i, (y0, y1) in enumerate(bands):
        la, lb = band_luminance(fa, y0, y1), band_luminance(fb, y0, y1)
        result["bar%d" % i] = {"A": round(la, 1), label: round(lb, 1)}
        if lb > fx["bar_max_luminance"]:
            fails.append("%s's bar %d (rows %.2f..%.2f) is not black: %.1f > %.1f" % (label, i, y0, y1, lb, fx["bar_max_luminance"]))
        if la < fx["picture_min_luminance"]:
            fails.append("A's rows %.2f..%.2f show no picture: %.1f < %.1f (the default should fill the capture)" % (y0, y1, la, fx["picture_min_luminance"]))
    lp = band_luminance(fb, picture[0], picture[1])
    result["picture"] = {label: round(lp, 1)}
    if lp < fx["picture_min_luminance"]:
        fails.append("%s's picture rows %.2f..%.2f are dark: %.1f < %.1f" % (label, picture[0], picture[1], lp, fx["picture_min_luminance"]))
    return result, fails


# ---- the wide shapes drawn for their shape -------------------------------------------------------------------------------------

def ratio(shape):
    w, h = shape.split(":")
    return float(w) / float(h)


def squeeze_weights(w, share):
    """Per output column of the frame squeezed to `share` of its width (area averaging): [(input column, weight), ...]."""
    out_w = int(round(w * share))
    scale = w / out_w
    cols = []
    for j in range(out_w):
        a, b = j * scale, (j + 1) * scale
        taps, x = [], int(a)
        while x < b and x < w:
            cover = min(b, x + 1) - max(a, x)
            if cover > 0:
                taps.append((x, cover / scale))
            x += 1
        cols.append(taps)
    return cols


def middle_mad(wide, ref, share):
    """The mean absolute difference per channel between the middle `share` of the frame `wide` and the frame `ref` squeezed
    to that share; and the stretch control: the same for `ref`'s own middle (what an only-stretched frame scores)."""
    w, h, pw = read_ppm(wide)
    w2, h2, pr = read_ppm(ref)
    if (w, h) != (w2, h2):
        raise SystemExit("display_judge: the frames differ in size: %dx%d and %dx%d" % (w, h, w2, h2))
    cols = squeeze_weights(w, share)
    x0 = (w - len(cols)) // 2
    total = control = 0.0
    n = 0
    for y in range(h):
        base = y * w * 3
        for j, taps in enumerate(cols):
            for c in range(3):
                sq = 0.0
                for x, wt in taps:
                    sq += pr[base + 3 * x + c] * wt
                total += abs(pw[base + 3 * (x0 + j) + c] - sq)
                control += abs(pr[base + 3 * (x0 + j) + c] - sq)
                n += 1
    return total / n, control / n


TEXT = re.compile(r"\[te\.gfx\.diag\] present (\d+) text \d+ quads .* box \(([-\d.]+) ([-\d.]+)\)-\(([-\d.]+) ([-\d.]+)\) "
                  r"frame (\d+)x(\d+) .* string '(.*)'$")


COUNTER = re.compile(r"\bfps~")
PARTICLES = re.compile(r"\[te\.gfx\.diag\] present (\d+) particles block (0x[0-9a-f]+) ")
TRIANGLE = re.compile(r"\(([-\d.]+) ([-\d.]+) [-\d.e+]+\)")


def particle_quads(log, present):
    """{block: [(width, height) of each quad]} of the particle blocks drawn at `present` (TE_GFX_DIAG_PARTICLES' triangles,
    two to a quad)."""
    out, tris = {}, None
    with open(log, errors="replace") as f:
        for line in f:
            m = PARTICLES.search(line)
            if m:
                tris = out.setdefault(m.group(2), []) if int(m.group(1)) == present else None
                if tris is not None:
                    tris.append([])
            elif tris is not None and line.startswith("[te.gfx.diag]   tri ("):
                tris[-1].append([(float(x), float(y)) for x, y in TRIANGLE.findall(line)])
            elif tris is not None and not line.startswith("[te.gfx.diag]   "):
                tris = None
    sizes = {}
    for block, draws in out.items():
        for draw in draws:
            for i in range(0, len(draw) - 1, 2):
                xs = [p[0] for p in draw[i] + draw[i + 1]]
                ys = [p[1] for p in draw[i] + draw[i + 1]]
                sizes.setdefault(block, []).append((max(xs) - min(xs), max(ys) - min(ys)))
    return sizes


def text_boxes(log, present):
    out = []
    with open(log, errors="replace") as f:
        for line in f:
            m = TEXT.search(line.rstrip("\n"))
            if m and int(m.group(1)) == present:
                out.append((m.group(8), float(m.group(2)), float(m.group(4)), int(m.group(6))))
    return out


def wide_legs(a, fx, out):
    wl = fx["wide_legs"]
    fails, result = [], {}
    card = a.hub_card
    if not card:
        d, rc = run(a.bin, a.iso, out, fx, [], "hub_card", padlog=wl["hub_padlog"], stop_at=wl["hub_stop_at"],
                    env_extra={"TE_GFX_NO_DRAW": "1", "TE_GFX_DUMP_EVERY": "0"})
        if rc != 0:
            print("display_judge: the hub card's run exited %d" % rc, file=sys.stderr)
            return None, None
        card = os.path.join(d, "mc")
    ws_card = os.path.join(out, "card_widescreen")
    rc = subprocess.call([sys.executable, os.path.join(ROOT, "tools", "warp_save.py"), "--card", card, "--chunk", wl["chunk"],
                          "--out", ws_card, "--widescreen", "on"], cwd=ROOT)
    if rc != 0:
        print("display_judge: warp_save.py exited %d" % rc, file=sys.stderr)
        return None, None
    presents = wl["presents"]
    first, last = min(presents.values()), max(presents.values())
    env = {"TE_GFX_DUMP_TARGET": "1", "TE_GFX_DUMP_EVERY": "100", "TE_GFX_DUMP_FROM": str(first), "TE_GFX_DUMP_TO": str(last),
           "TE_GFX_DIAG_TEXT": "1", "TE_GFX_DIAG_PRESENT": str(wl["particle_present"]), "TE_GFX_DIAG_PARTICLES": "1"}
    dirs = {}
    for shape in [wl["reference"]] + wl["shapes"]:
        d, rc = run(a.bin, a.iso, out, fx, ["--aspect", shape, "--fps-counter", "on"], "wide_%s" % shape.replace(":", "x"),
                    padlog=wl["padlog"], stop_at=wl["stop_at"], mc_from=ws_card, env_extra=env)
        if rc != 0:
            if shape == wl["reference"]:
                print("display_judge: the %s leg exited %d" % (shape, rc), file=sys.stderr)
                return None, None
            fails.append("the runner refused --aspect %s (exit %d): the shape does not exist" % (shape, rc))
            continue
        dirs[shape] = d
    ref = dirs[wl["reference"]]
    ref_log = os.path.join(ref, "run.log")
    ref_texts = {present: text_boxes(ref_log, present) for present in wl["text_presents"]}
    for present, boxes in ref_texts.items():
        if not boxes:
            fails.append("the %s leg logged no string at present %d" % (wl["reference"], present))
    ref_quads = particle_quads(ref_log, wl["particle_present"])
    for shape in wl["shapes"]:
        if shape not in dirs:
            continue
        share = ratio(wl["reference"]) / ratio(shape)
        r = {"share": round(share, 4)}
        for name, present in sorted(presents.items(), key=lambda kv: kv[1]):
            fw = os.path.join(dirs[shape], "dump", "frame_%06d.ppm" % present)
            fr = os.path.join(ref, "dump", "frame_%06d.ppm" % present)
            if not os.path.exists(fw) or not os.path.exists(fr):
                print("display_judge: present %d not dumped (%s)" % (present, shape), file=sys.stderr)
                return None, None
            mad, control = middle_mad(fw, fr, share)
            r[name] = {"middle_mad": round(mad, 2), "stretch_control": round(control, 2)}
            if not (mad <= wl["max_mad"] and mad * wl["control_ratio"] < control):
                fails.append("%s at present %d (%s): the frame's middle is %.2f from the 16:9 view squeezed by %.3f (at most %.1f, "
                             "and under the stretch's %.2f / %.0f): not drawn for the shape" % (shape, present, name, mad, share,
                                                                                               wl["max_mad"], control,
                                                                                               wl["control_ratio"]))
        log = os.path.join(dirs[shape], "run.log")
        for present in wl["text_presents"]:
            ref_text, boxes = ref_texts[present], text_boxes(log, present)
            pairs = [(p, q) for p, q in zip(ref_text, boxes) if p[0] == q[0] and not COUNTER.search(p[0])]
            strings = [p for p in ref_text if not COUNTER.search(p[0])]
            worst, worst_text = 0.0, ""
            for (text, x0, x1, fw16), (_, y0, y1, _) in pairs:
                middle = fw16 / 2.0
                for got, was in ((y0, x0), (y1, x1)):
                    off = abs(got - (middle + (was - middle) * share))
                    if off > worst:
                        worst, worst_text = off, text
            r["strings_%d" % present] = {"matched": len(pairs), "of": len(strings), "worst_px": round(worst, 2), "worst": worst_text}
            if len(pairs) < max(1, len(strings) // 2):
                fails.append("%s: only %d of the %d strings of present %d drawn as at 16:9" % (shape, len(pairs), len(strings), present))
            elif worst > wl["text_tolerance"]:
                fails.append("%s: the string '%s' of present %d is %.1f px from where the 16:9 one squeezed to the middle puts it "
                             "(at most %.1f): the menus and the HUD not kept in the middle 16:9" % (shape, worst_text, present, worst,
                                                                                                     wl["text_tolerance"]))
            # the platform's own text, the fps counter (top right, its figures the run's own): its right edge, where it is
            # aligned, where the 16:9 one squeezed to the middle puts it
            counter = [b for b in boxes if COUNTER.search(b[0])]
            counter16 = [b for b in ref_text if COUNTER.search(b[0])]
            if not counter or not counter16:
                fails.append("%s: no fps counter drawn at present %d (--fps-counter on)" % (shape, present))
            else:
                right, right16, fw16 = max(b[2] for b in counter), max(b[2] for b in counter16), counter16[0][3]
                want = fw16 / 2.0 + (right16 - fw16 / 2.0) * share
                r["fps_counter_%d" % present] = {"right": round(right, 1), "want": round(want, 1), "at_16_9": round(right16, 1)}
                if abs(right - want) > wl["text_tolerance"]:
                    fails.append("%s: the fps counter's right edge %.1f at present %d, not %.1f (the 16:9 one's %.1f squeezed to the "
                                 "middle)" % (shape, right, present, want, right16))
        # the particles in the world: as wide, against the world's 16:9 share of the frame, as at 16:9 (their heights as at 16:9)
        quads = particle_quads(log, wl["particle_present"])
        widths, heights = [], []
        for block, sizes16 in ref_quads.items():
            for (w16, h16), (w, h) in zip(sizes16, quads.get(block, [])):
                if w16 >= wl["particle_min_px"] and h16 >= wl["particle_min_px"]:
                    widths.append(w / w16)
                    heights.append(h / h16)
        if not widths:
            fails.append("%s: no particle quad of %.0f px or more at present %d in both legs" % (shape, wl["particle_min_px"],
                                                                                                 wl["particle_present"]))
        else:
            widths.sort()
            heights.sort()
            wmid, hmid = widths[len(widths) // 2], heights[len(heights) // 2]
            r["particles"] = {"quads": len(widths), "width_ratio": round(wmid, 3), "height_ratio": round(hmid, 3)}
            if abs(wmid / share - 1.0) > wl["particle_tolerance"] or abs(hmid - 1.0) > wl["particle_tolerance"]:
                fails.append("%s: the particles' widths %.3f and heights %.3f of 16:9's (want %.3f and 1): not as on a 16:9 TV" %
                             (shape, wmid, hmid, share))
        result[shape] = r
    return result, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the runs' directory (default: build/display_judge)")
    ap.add_argument("--hub-card", help="a --mc-dir holding the hub save (default: made here from the fixture's hub pad log)")
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    fx = json.load(open(a.fixture))
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "display_judge"))
    os.makedirs(out, exist_ok=True)
    present = fx["present"]

    da, rca = run(a.bin, a.iso, out, fx, [], "A_auto")
    if rca != 0:
        print("display_judge: run A exited %d" % rca, file=sys.stderr)
        return 2
    fa = os.path.join(da, "dump", "frame_%06d.ppm" % present)
    fails = []
    result = {"present": present}
    cases = [("B", fx["aspect"], fx["bars"], fx["picture"])]
    if "super_wide" in fx:
        sw = fx["super_wide"]
        cases.append(("C", sw["aspect"], sw["bars"], sw["picture"]))
    for label, shape, bands, picture in cases:
        db, rcb = run(a.bin, a.iso, out, fx, ["--aspect", shape], "%s_aspect_%s" % (label, shape.replace(":", "x")))
        if rcb != 0:
            fails.append("the runner refused --aspect %s (exit %d): no such display setting -- nothing can show" % (shape, rcb))
            continue
        fb = os.path.join(db, "dump", "frame_%06d.ppm" % present)
        if not os.path.exists(fa) or not os.path.exists(fb):
            print("display_judge: present %d not dumped" % present, file=sys.stderr)
            return 2
        r, f = shape_case(fa, fb, bands, picture, fx, label)
        result[shape] = r
        fails += f
    if "wide_legs" in fx:
        r, f = wide_legs(a, fx, out)
        if r is None:
            return 2
        result["wide"] = r
        fails += f
    result["verdict"] = "GREEN" if not fails else "RED"
    result["why"] = fails
    print(json.dumps(result))
    print("display_judge: %s" % result["verdict"])
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
