#!/usr/bin/env python3
"""iceclimb_bergs_judge.py -- Ice Climb's floating ice (the bergs Crash crosses in levels\\ice\\iceclimb\\caveent) moves as
the recompiled reference moves it: a regression judge against a frozen reference trajectory.

The report: "you jump on these ice bergs to get across the water but then when i turn back ... theyre like partial peaking
out of the water and when trying to jump on them you die". The bergs are BODY motion blocks (game/src/game/trajectory.cpp
StepBodyTrajectory / PullBodySprings): four level springs at the corners of the box pull each toward its authored place,
the character presses the body it stands on (CharacterAgent::StandOnBody), and a body that rested 48 frames with the player
away is put back on its ride at the authored place (PutBackStuckBody). They are not buoyant (DynamicBody::buoyancy -1). So
a berg dips ~1.9 below its authored height after a landing and sits 1.12 lower while Crash stands on it -- the retail
program does the same (notes/icebergs.md).

The REFERENCE is the recompiled retail program (the engine's NTSC build, its maths' own generated bodies:
hle.math_helpers=off) on the fixture's pad log transposed to it: the first berg's height at twelve of the port's
boundaries (the landing, the dip, the bob, the sag under the standing character, the long stand, the jump off, the
rebound), frozen in the fixture with where it came from. The judge replays the fixture's pad log on a card warped to the
chunk (the hub card the hub pad log writes, as the crystal and cave judges make it) with --trace-bodies 1 and checks that
the berg's body exists (Crash landed on it) and that its height at each of the twelve boundaries is the reference's within
the fixture's `tolerance`. GREEN when all twelve hold.

Sensitivity (2026-10-28, evidence/icebergs/judges/p20): the port 12/12 within 4.7e-6; with VuTransformPoint / VuRotateVector
answered by the retail instructions on the engine's VU interpreter (--env TE_MATH_TRANSFORM_MICRO=1) within 9.1e-6 (GREEN);
a bias on the first berg's own matrix (--env TE_MATH_TRANSFORM_WATCH=0x00DFE2F0 --env TE_MATH_TRANSFORM_BIAS_Y=D: the body
0x00DFE240 + 0xB0 in this deterministic run; its level springs read their corner heights through it) settles the berg D
lower: D = 2e-4 GREEN, D = 0.01 RED (11 of 12, the route intact), D = 1.12 RED (the berg 1.12 lower all along: what a seam
error would have to be to sink the bergs as the report describes).

Usage: iceclimb_bergs_judge.py --bin BIN --iso IMAGE [--fixture F] [--out DIR] [--card DIR] [--env KEY=VALUE ...] [--keep]
       iceclimb_bergs_judge.py --log RUN_LOG [--fixture F] [--out DIR]       judge an existing run's log
  --card DIR    use this warped card instead of making one (a run of the hub pad log + tools/warp_save.py)
  --env K=V     an environment variable for the route's run only (a sensitivity check: a perturbed knob must turn it RED)
Exit 0 = GREEN, 1 = RED, 2 = the run failed / nothing to judge. Writes OUT/report.json.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "iceclimb_bergs_pal.json")
BODY = re.compile(r"\[bodies\] @(\d+) slot \d+ 0x[0-9A-F]+ \S+ '([^']*)' inst (0x[0-9A-F]+)( asleep)? pos \(([-\d.]+) ([-\d.]+) ([-\d.]+)\)"
                  r" y ([0-9A-F]{8})")


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


def run_route(binary, iso, out, card, fx, extra_env):
    mc = os.path.join(out, "mc")
    shutil.rmtree(mc, ignore_errors=True)
    shutil.copytree(card, mc)
    log = os.path.join(out, "run.log")
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_TRACE_BODIES_ALL="1")
    env.update(extra_env)
    with open(log, "w") as f:
        rc = subprocess.call([binary, "--iso", iso, "--config", "none", "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies",
                              "--trace-state", "--trace-bodies", "1", "--play-pad", os.path.join(ROOT, fx["padlog"]),
                              "--stop-at", str(fx["stop_at"]), "--mc-dir", mc],
                             stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    return rc, log


def read_bodies(log, name):
    """{instance: {boundary: (x, y, z, y bits)}} of the bodies whose object name contains `name` (y from its printed bits:
    the float exactly)."""
    bodies = {}
    with open(log, errors="replace") as f:
        for line in f:
            m = BODY.search(line)
            if m and name in m.group(2):
                y = struct.unpack("<f", struct.pack("<I", int(m.group(8), 16)))[0]
                bodies.setdefault(m.group(3), {})[int(m.group(1))] = (float(m.group(5)), y, float(m.group(7)), m.group(8))
    return bodies


def find_berg(bodies, at, near):
    """The instance whose first body sample lies within `near` (x, z) of the fixture's authored place."""
    best, dist = None, None
    for inst, rows in bodies.items():
        x, _, z, _ = rows[min(rows)]
        d = ((x - at[0]) ** 2 + (z - at[1]) ** 2) ** 0.5
        if d <= near and (dist is None or d < dist):
            best, dist = inst, d
    return best


def judge(log, fx, out, extra_env):
    tolerance = fx["tolerance"]
    reference = sorted((int(k), v) for k, v in fx["reference_y"].items())
    bodies = read_bodies(log, fx["name"])
    inst = find_berg(bodies, fx["berg"]["at"], fx["near"])
    report = {"judge": "iceclimb_bergs", "log": log, "tolerance": tolerance, "env": extra_env, "berg": fx["berg"]["label"],
              "samples": []}
    green = inst is not None
    if inst is None:
        report["detail"] = "no body near the berg's authored place %s: Crash did not land on it" % fx["berg"]["at"]
        print("iceclimb_bergs: %s: no body (Crash did not land on it)" % fx["berg"]["label"])
    else:
        rows = bodies[inst]
        report["instance"], report["first_boundary"] = inst, min(rows)
        for b, ref in reference:
            row = rows.get(b)
            if row is None:
                sample = {"boundary": b, "reference": ref, "port": None, "dy": None, "ok": False, "what": fx["what"].get(str(b), "")}
            else:
                dy = row[1] - ref
                sample = {"boundary": b, "reference": ref, "port": row[1], "port_bits": row[3], "dy": round(dy, 7),
                          "ok": abs(dy) <= tolerance, "what": fx["what"].get(str(b), "")}
            green &= sample["ok"]
            report["samples"].append(sample)
            print("iceclimb_bergs: @%d %-46s reference %10.6f port %s %s" % (
                b, sample["what"][:46], ref, "%10.6f (dy %+.2e)" % (sample["port"], sample["dy"]) if sample["port"] is not None else "   (none)",
                "ok" if sample["ok"] else "FAIL"))
        worst = max((s for s in report["samples"] if s["dy"] is not None), key=lambda s: abs(s["dy"]), default=None)
        report["worst"] = worst
        report["failed"] = sum(1 for s in report["samples"] if not s["ok"])
    report["verdict"] = "GREEN" if green else "RED"
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    detail = report.get("detail") or "%d of %d heights within %g; worst dy %+.2e at %s" % (
        len(reference) - report.get("failed", len(reference)), len(reference), tolerance,
        report["worst"]["dy"] if report.get("worst") else 0.0, report["worst"]["boundary"] if report.get("worst") else "-")
    print("iceclimb_bergs: %s (%s) %s" % (report["verdict"], detail, os.path.relpath(fx["path"], ROOT) if fx.get("path") else ""))
    return 0 if green else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--log", help="judge an existing run log instead of running")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", default="/tmp/iceclimb_bergs_judge")
    ap.add_argument("--card", help="a card warped to the chunk (default: made from the hub pad log)")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for the route's run only")
    ap.add_argument("--keep", action="store_true", help="keep the cards and the run's card copy")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    fx["path"] = os.path.abspath(a.fixture)
    extra_env = dict(kv.split("=", 1) for kv in a.env)
    os.makedirs(a.out, exist_ok=True)
    if a.log:
        return judge(a.log, fx, a.out, extra_env)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --log)")
    card = os.path.abspath(a.card) if a.card else make_card(os.path.abspath(a.bin), a.iso, a.out, fx)
    rc, log = run_route(os.path.abspath(a.bin), a.iso, a.out, card, fx, extra_env)
    if rc != 0:
        print("iceclimb_bergs: the run exited %d (%s)" % (rc, log))
        return 2
    verdict = judge(log, fx, a.out, extra_env)
    if not a.keep:
        for d in ("mc_hub", "mc_card", "mc"):
            shutil.rmtree(os.path.join(a.out, d), ignore_errors=True)
    return verdict


if __name__ == "__main__":
    sys.exit(main())
