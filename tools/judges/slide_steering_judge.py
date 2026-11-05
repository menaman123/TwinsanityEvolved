#!/usr/bin/env python3
"""slide_steering_judge.py -- THE KNEE SLIDE'S STEERING: does a slide at 60 Hz keep to its direction, and does nothing else change?

A steering slide turns at the angle left to its direction over the steering's time left; in the steering's last frame retail
takes that time as only the part of the frame before the steering ends (at 60 Hz 9 of the frame's 149 clock units) and the
frame turns past the slide's direction (game/src/game/charactermoves.cpp CrouchController::Slide). game.slide_steering = fixed
(the default) takes the time left as no less than half the frame (Platform::System::SlideSteeringFixed); retail's own at the
PAL disc's 50 Hz leaves 97 of 179 units, so nothing changes there.

Deterministic headless runs of the slide pad logs (fixtures/padlogs/slide_steering.padlog at 60 Hz, slide_steering_50.padlog at
50 Hz: NEW GAME, then knee slides on N. Sanity Beach entered with the stick turned from the run), with --harness-out and the
probe's [move] lines (TE_TRACE_MOVES=1: every boundary of a slide, the facing's yaw and the slide's direction):
  A  60 Hz, the default (fixed)      -> GREEN needs `slides_60` slides, and in none the last frame of steering turning the facing
                                        past the slide's direction (its turn no more than the angle left before it)
  B  60 Hz, game.slide_steering = retail -> every slide's last frame of steering turns past its direction (the 60 Hz swing:
                                        the bug, reproduced with the fix off), by more than `min_swing_past` degrees in one
  C  60 Hz, the default + --mod speedrun -> B's slides exactly (the speedrun mod keeps retail's rules)
  A/B  the frames and the state trace identical up to the first slide's last frame of steering, different after it
  D/E  50 Hz, the default and retail -> at least `slides_50` slides, and the two runs identical: every [move] and probe line, every
                                        frame, the state trace (the fix changes nothing at the PAL disc's own rate)
A runner without the probe's slide lines (no TE_TRACE_MOVES) is RED.

Usage: slide_steering_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = a run failed.
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
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "slide_steering_pal.json")
SLIDE = re.compile(r"^\[move\] @(\d+) slide state (\d+) elapsed (-?\d+) ticks (-?\d+) mayJump (\d) yaw (\S+) direction (\S+) ")
SLIDING, STEERING_DONE = 5, 1


def run(binary, iso, out, fx, hz, settings, extra, tag):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    mc = os.path.join(d, "card", "mc")   # a mod's card lands beside mc, inside the run's folder
    os.makedirs(mc)
    config = "none"
    if settings:
        config = os.path.join(d, "ps2r.ini")
        with open(config, "w") as f:
            f.write("".join("%s = %s\n" % kv for kv in settings.items()))
    leg = fx["legs"][str(hz)]
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--config", config, "--hz", str(hz), "--deterministic", "--no-sdl-input", "--no-audio",
            "--skip-movies", "--trace-state", "--trace-player", str(fx["probe_every"]), "--play-pad", os.path.join(ROOT, leg["padlog"]),
            "--stop-at", str(leg["stop_at"]), "--mc-dir", mc, "--run-seconds", str(fx.get("run_seconds", 600)),
            "--harness-out", os.path.join(d, "harness")] + extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_TRACE_MOVES"] = "1"
    print("slide_steering_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("slide_steering_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return d, rc, log


def lines(log, prefixes):
    with open(log, errors="replace") as f:
        return [l.rstrip("\n") for l in f if l.startswith(prefixes)]


def turn(a, b):
    """the signed turn from yaw a to yaw b, degrees in [-180, 180)"""
    return (b - a + 180.0) % 360.0 - 180.0


def slides(log):
    """per slide (from a sliding line at elapsed 0): its boundary, its direction, the last frame of steering's turn and the angle
    left before it (the yaw of the line before against the slide's direction), and the steering frames' count"""
    rows = []
    for l in lines(log, ("[move] ",)):
        m = SLIDE.match(l)
        if m:
            rows.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(5)), float(m.group(6)), float(m.group(7))))
    out, current = [], None
    for r in rows:
        if r[1] == SLIDING and r[2] == 0:
            current = [r]
            out.append(current)
        elif current is not None:
            current.append(r)
            if r[1] != SLIDING and r[1] != 6:
                current = None
    result = []
    for s in out:
        steer = [k for k, r in enumerate(s) if r[1] == SLIDING and r[3] != STEERING_DONE]
        if len(steer) < 2:
            continue
        last = steer[-1]
        result.append({"at": s[0][0], "last_at": s[last][0], "direction": s[last][5], "frames": len(steer),
                       "turn": turn(s[last - 1][4], s[last][4]), "left": turn(s[last - 1][4], s[last][5])})
    return result


def keyed(path):
    out = {}
    with open(path) as f:
        for line in f:
            p = line.strip().split(",")
            if len(p) >= 2 and p[0].isdigit():
                out[int(p[0])] = ",".join(p[1:])
    return out


def first_difference(a, b):
    keys = sorted(set(a) & set(b))
    return next((k for k in keys if a[k] != b[k]), None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "slide_steering_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    retail = {"game.slide_steering": "retail"}
    da, rca, log_a = run(a.bin, a.iso, a.out, fx, 60, None, [], "A_60_fixed")
    db, rcb, log_b = run(a.bin, a.iso, a.out, fx, 60, retail, [], "B_60_retail")
    dc, rcc, log_c = run(a.bin, a.iso, a.out, fx, 60, None, ["--mod", "speedrun"], "C_60_speedrun")
    dd, rcd, log_d = run(a.bin, a.iso, a.out, fx, 50, None, [], "D_50_fixed")
    de, rce, log_e = run(a.bin, a.iso, a.out, fx, 50, retail, [], "E_50_retail")
    if any(rc != 0 for rc in (rca, rcb, rcc, rcd, rce)):
        print("slide_steering_judge: a run failed (A %d, B %d, C %d, D %d, E %d)" % (rca, rcb, rcc, rcd, rce))
        return 2
    reasons = []
    sa, sb, sc, sd, se = slides(log_a), slides(log_b), slides(log_c), slides(log_d), slides(log_e)
    for tag, s in (("A", sa), ("B", sb), ("C", sc), ("D", sd), ("E", se)):
        for k, x in enumerate(s):
            print("slide_steering_judge: %s slide %d @%d: direction %7.2f, %2d frames of steering, the last turns %8.2f with %7.2f left%s" % (
                tag, k + 1, x["at"], x["direction"], x["frames"], x["turn"], x["left"], "  PAST" if abs(x["turn"]) > abs(x["left"]) else ""))
    if not sa and not sb:
        reasons.append("no slide lines: the runner has no TE_TRACE_MOVES probe")

    # A: the default keeps every slide to its direction
    if len(sa) != fx["slides_60"]:
        reasons.append("A: %d slides, the pad log makes %d" % (len(sa), fx["slides_60"]))
    past_a = [x for x in sa if abs(x["turn"]) > abs(x["left"])]
    if past_a:
        reasons.append("A: %d slides turn past their direction in the last frame of steering (%s)" % (
            len(past_a), ", ".join("@%d %.1f of %.1f" % (x["last_at"], x["turn"], x["left"]) for x in past_a)))

    # B: retail's swing, the bug the fix is for
    if len(sb) != fx["slides_60"]:
        reasons.append("B: %d slides, the pad log makes %d" % (len(sb), fx["slides_60"]))
    calm_b = [x for x in sb if not abs(x["turn"]) > abs(x["left"]) + fx["min_swing_past"]]
    if calm_b:
        reasons.append("B: %d slides do not swing past their direction by more than %.0f degrees (retail's 60 Hz steering)" % (
            len(calm_b), fx["min_swing_past"]))

    # C: the speedrun mod keeps retail's
    if lines(log_c, ("[move] @",)) != lines(log_b, ("[move] @",)):
        reasons.append("C: the speedrun mod's slides are not retail's (B)")

    # A/B: nothing differs before the first slide's last frame of steering
    if sb:
        cut = sb[0]["last_at"]
        fa, fb = keyed(os.path.join(da, "harness", "frames.fhash")), keyed(os.path.join(db, "harness", "frames.fhash"))
        ta, tb = keyed(os.path.join(da, "harness", "state.trace")), keyed(os.path.join(db, "harness", "state.trace"))
        frame_diff = first_difference(fa, fb)
        if frame_diff is None or frame_diff < cut:
            reasons.append("A/B: the first differing frame is %s, the first slide's last frame of steering %d" % (frame_diff, cut))
        state_diff = first_difference(ta, tb)
        if state_diff is not None and state_diff < cut:
            reasons.append("A/B: the state traces part at %d, before the first slide's last frame of steering %d" % (state_diff, cut))
        print("slide_steering_judge: A/B the first differing frame %s (the first last frame of steering %d)" % (frame_diff, cut))

    # D/E: nothing changes at 50 Hz
    if len(sd) < fx["slides_50"]:
        reasons.append("D: %d slides at 50 Hz, the pad log makes at least %d" % (len(sd), fx["slides_50"]))
    if lines(log_d, ("[move] @", "[probe] @")) != lines(log_e, ("[move] @", "[probe] @")):
        reasons.append("D/E: the 50 Hz runs' probe lines differ: the fix changed the PAL rate's slides")
    for name in ("frames.fhash", "state.trace", "audio.ahash"):
        if keyed(os.path.join(dd, "harness", name)) != keyed(os.path.join(de, "harness", name)):
            reasons.append("D/E: the 50 Hz runs' %s differ" % name)

    if reasons:
        for r in reasons:
            print("slide_steering_judge: " + r)
        print("slide_steering_judge: RED (%d)" % len(reasons))
        return 1
    print("slide_steering_judge: GREEN -- 60 Hz: %d slides kept to their direction (retail's last frame of steering swings each "
          "%s degrees past it, the speedrun mod's too); 50 Hz: %d slides, the fixed and retail runs identical" % (
              len(sa), "/".join("%.0f" % (abs(x["turn"]) - abs(x["left"])) for x in sb), len(sd)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
