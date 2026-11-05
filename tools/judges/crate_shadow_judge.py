#!/usr/bin/env python3
"""crate_shadow_judge.py -- does Crash's shadow fall on a crate's top, and nowhere else on it?

Every crate's material sets FBA (the frame alpha's top bit: never a receiver of the shadows), so on the PS2 no shadow falls on
a crate: the column cast straight down from the character passes through the whole crate, and a crate drawn without FBA
takes it on its top AND in a band down its side, and in a line along the bottom rail's ledge (an up-facing face under the
top's rim). With video.shadows_on_crates on (the default) the port draws an FBA pass's FLOOR triangles -- normal within 45
degrees of +y, nothing of the model above the centre -- without FBA, in the build's own order (graphics_scene.cpp
FloorSplitOf). Deterministic headless legs (fixtures/judges/crate_shadow_pal.json):

  each plan (fixtures/padlogs/crate_drop.plan: Crash dropped onto the middle of the beach's basic crate; crate_drop_edge.plan:
  onto its north edge, half of the column over the sand beside the side), off (a ps2r.ini with video.shadows_on_crates =
  off: the file applied), on, and on with the floors painted (TE_GFX_DIAG_FLOORS=paint: the pixels the floors win), the
  pictures of the window dumped:
    render only   the state trace and the audio hashes identical off and on
    the window    every picture whose hash differs off / on lies in the dumped window
    floors only   every pixel that differs is a floor pixel (the painted leg changes it) -- none on a side, the ledge or
                  anything else
    the side      no pixel of side_box changes (the crate's side face and its bottom rail's ledge in the landing pictures, frozen
                  apart from the build's own floors: a build that draws the whole crate without FBA, as the first attempt did,
                  changes 10..389 of them a picture)
    a shadow      every pixel that differs is darkened by one amount on its three channels (the composite: Cd - k, clamped at 0)
    taken         at least min_darkened such pixels in at least min_shadowed_pictures pictures (the top does take the shadow)
  the hub pad log (fixtures/padlogs/hub_entry.padlog to 4200): frames, audio and state identical off (--shadows-on-crates off)
  and on
  --base-bin BASE (the build before the change; optional): the off legs' and the hub leg's frames.fhash identical to BASE's

Usage: crate_shadow_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F] [--base-bin BASE]
Exit 0 = GREEN, 1 = RED, 2 = a run failed.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "crate_shadow_pal.json")


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    w, h = int(fields[1]), int(fields[2])
    return np.frombuffer(data[pos + 1:pos + 1 + w * h * 3], dtype=np.uint8).reshape(h, w, 3)


def run(binary, iso, out, name, args, env_extra, dump=None, ini=None):
    """one headless deterministic leg: its harness directory (frames.fhash, audio.ahash, state.trace) and its dumps; `ini`
    the text of its ps2r.ini (none: --config none)"""
    d = os.path.join(out, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "mc"))
    config = "none"
    if ini is not None:
        config = os.path.join(d, "ps2r.ini")
        with open(config, "w") as f:
            f.write(ini)
    cmd = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
           "--config", config, "--mc-dir", os.path.join(d, "mc"), "--harness-out", os.path.join(d, "leg")] + args
    env = dict(os.environ)
    for k in [k for k in env if k.startswith("TE_GFX_DUMP") or k.startswith("TE_GFX_DIAG")]:
        env.pop(k)
    env["TE_GFX_HEADLESS"] = "1"
    if dump:
        os.makedirs(os.path.join(d, "dump"))
        env.update({"TE_GFX_DUMP_DIR": os.path.join(d, "dump"), "TE_GFX_DUMP_EVERY": "1", "TE_GFX_DUMP_FROM": str(dump[0]),
                    "TE_GFX_DUMP_TO": str(dump[1])})
    env.update(env_extra)
    with open(os.path.join(d, "run.log"), "w") as log:
        rc = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT, env=env, cwd=ROOT)
    return rc, d


def hashes(leg_dir, name):
    with open(os.path.join(leg_dir, "leg", name)) as f:
        return f.read().split()


def judge_plan(a, fx, p, reasons, report):
    out = a.out
    plan = os.path.join(ROOT, p["plan"])
    common = ["--trace-player", str(fx["trace_player"]), "--autopilot", plan, "--stop-at", str(p["stop_at"]),
              "--run-seconds", str(fx["run_seconds"])]
    window = (p["dump_from"], p["dump_to"])
    legs = {}
    # the off leg through the settings file (video.shadows_on_crates = off: the key read, the file applied), the hub's off
    # leg below through --shadows-on-crates off
    for leg, args, env, ini in (("off", [], {}, "video.shadows_on_crates = off\n"), ("on", [], {}, None),
                                ("paint", [], {"TE_GFX_DIAG_FLOORS": "paint"}, None)):
        rc, d = run(a.bin, a.iso, out, "%s_%s" % (p["name"], leg), common + args, env, window, ini)
        if rc != 0:
            reasons.append("%s %s: the run exited %d" % (p["name"], leg, rc))
            return None
        legs[leg] = d
    with open(os.path.join(legs["off"], "run.log"), errors="replace") as f:
        log = f.read()
    if "not applied" in log or "shadows on crates off" not in log:
        reasons.append("%s: the settings file's video.shadows_on_crates = off was not applied" % p["name"])
    r = {"plan": p["plan"]}
    for name in ("state.trace", "audio.ahash"):
        if hashes(legs["off"], name) != hashes(legs["on"], name):
            reasons.append("%s: %s differs off / on (a render-only change)" % (p["name"], name))
    off_f, on_f = hashes(legs["off"], "frames.fhash"), hashes(legs["on"], "frames.fhash")
    # (a line per picture, in order: line k is picture k + 1, the dumps' numbering)
    differing = [k + 1 for k, (x, y) in enumerate(zip(off_f, on_f)) if x != y]
    differ_hashes = len(differing) + abs(len(off_f) - len(on_f))
    r["pictures_differing"] = differing
    pictures = sorted(n for n in os.listdir(os.path.join(legs["off"], "dump")) if n.endswith(".ppm"))
    differ_pictures = darkened = off_floor = not_shadow = shadowed = side = 0
    for n in pictures:
        paths = {leg: os.path.join(legs[leg], "dump", n) for leg in legs}
        if not all(os.path.exists(x) for x in paths.values()):
            reasons.append("%s: %s not dumped by every leg" % (p["name"], n))
            continue
        off, on, paint = (read_ppm(paths[leg]).astype(np.int16) for leg in ("off", "on", "paint"))
        if list(off.shape[1::-1]) != fx["frame"]:
            reasons.append("%s: %s is not %dx%d" % (p["name"], n, fx["frame"][0], fx["frame"][1]))
            continue
        differ = (off != on).any(axis=2)
        if not differ.any():
            continue
        differ_pictures += 1
        x0, y0, x1, y1 = p["side_box"]
        if p["side_pictures"][0] <= int(n[6:12]) <= p["side_pictures"][1]:
            side += int(differ[y0:y1, x0:x1].sum())
        floor = (paint != on).any(axis=2)
        # the composite's darkening: one amount K off every channel, clamped at 0 (on = max(0, off - K))
        K = (off - on).max(axis=2)
        dark = differ & (K > 0) & (on == np.maximum(0, off - K[..., None])).all(axis=2)
        off_floor += int((differ & ~floor).sum())
        not_shadow += int((differ & ~dark).sum())
        if (differ & floor & dark).any():
            shadowed += 1
        darkened += int((differ & floor & dark).sum())
        if (differ & ~floor).any() or (differ & ~dark).any():
            ys, xs = np.nonzero(differ & ~(floor & dark))
            report.setdefault("examples", []).append({"plan": p["name"], "picture": n, "pixels": [[int(x), int(y)] for x, y in zip(xs[:6], ys[:6])]})
    r.update({"pictures_differing_hashes": differ_hashes, "pictures_differing_dumped": differ_pictures, "darkened_floor_pixels": darkened,
              "shadowed_pictures": shadowed, "differing_off_the_floors": off_floor, "differing_not_a_darkening": not_shadow,
              "differing_in_the_side_box": side})
    for d in legs.values():   # (the dumps judged: ~180 MB a leg)
        shutil.rmtree(os.path.join(d, "dump"), ignore_errors=True)
    if side:
        reasons.append("%s: %d pixels of the crate's side and ledge (side_box %s) changed" % (p["name"], side, p["side_box"]))
    if differ_hashes != differ_pictures:
        reasons.append("%s: %d pictures differ off / on, %d of them in the window %d-%d" % (p["name"], differ_hashes, differ_pictures, *window))
    if off_floor:
        reasons.append("%s: %d differing pixels off the floors (a side, the ledge or another surface took the shadow)" % (p["name"], off_floor))
    if not_shadow:
        reasons.append("%s: %d differing pixels that are not the composite's darkening" % (p["name"], not_shadow))
    if darkened < p["min_darkened"] or shadowed < p["min_shadowed_pictures"]:
        reasons.append("%s: the top took %d darkened pixels in %d pictures (at least %d in %d)" % (p["name"], darkened, shadowed,
                                                                                                    p["min_darkened"], p["min_shadowed_pictures"]))
    if a.base_bin:
        rc, d = run(a.base_bin, a.iso, out, "%s_base" % p["name"], common, {})
        if rc != 0:
            reasons.append("%s base: the run exited %d" % (p["name"], rc))
        else:
            r["off_equals_base"] = hashes(d, "frames.fhash") == off_f
            if not r["off_equals_base"]:
                reasons.append("%s: the off leg's frames differ from the base build's" % p["name"])
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "crate_shadow_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--base-bin", help="the build before the change: the off legs must draw its frames")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    os.makedirs(a.out, exist_ok=True)
    reasons, report = [], {"plans": []}
    for p in fx["plans"]:
        r = judge_plan(a, fx, p, reasons, report)
        if r is None:
            print(json.dumps(report, indent=1))
            print("crate_shadow_judge: RED -- " + "; ".join(reasons))
            return 2
        report["plans"].append(r)
    hub = fx["hub"]
    args = ["--play-pad", os.path.join(ROOT, hub["padlog"]), "--stop-at", str(hub["stop_at"]), "--run-seconds", str(fx["run_seconds"])]
    legs = {}
    for leg, extra in (("off", ["--shadows-on-crates", "off"]), ("on", [])):
        rc, d = run(a.bin, a.iso, a.out, "hub_" + leg, args + extra, {})
        if rc != 0:
            print("crate_shadow_judge: RED -- the hub %s leg exited %d" % (leg, rc))
            return 2
        legs[leg] = d
    report["hub"] = {name: hashes(legs["off"], name) == hashes(legs["on"], name) for name in ("frames.fhash", "audio.ahash", "state.trace")}
    for name, same in report["hub"].items():
        if not same:
            reasons.append("the hub pad log's %s differs off / on" % name)
    if a.base_bin:
        rc, d = run(a.base_bin, a.iso, a.out, "hub_base", args, {})
        report["hub"]["off_equals_base"] = rc == 0 and hashes(d, "frames.fhash") == hashes(legs["off"], "frames.fhash")
        if not report["hub"]["off_equals_base"]:
            reasons.append("the hub pad log's off leg differs from the base build's")
    print(json.dumps(report, indent=1))
    if reasons:
        print("crate_shadow_judge: RED -- " + "; ".join(reasons))
        return 1
    print("crate_shadow_judge: GREEN (%s; the hub leg identical off and on%s)" % (
        "; ".join("%s: %d darkened floor pixels in %d pictures, 0 elsewhere" % (p["name"], r["darkened_floor_pixels"], r["shadowed_pictures"])
                  for p, r in zip(fx["plans"], report["plans"])), "; off = the base build" if a.base_bin else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
