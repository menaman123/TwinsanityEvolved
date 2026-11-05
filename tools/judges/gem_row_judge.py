#!/usr/bin/env python3
"""gem_row_judge.py -- does the pause screen's gem row show a gem the moment it is picked up, and does the game keep the gem
for good only from the next save point on, as the retail game does?

One deterministic headless run of the gem row pad log (fixtures/padlogs/gem_row.padlog: NEW GAME on an empty card; the
fixture's test line warps Crash onto the beach's red gem, back to the start, then onto five nitro crates), with frames dumped
at three pictures of the pause screen (fixtures/judges/gem_row_pal.json):

  before     paused before any gem                          -> every slot of the arc empty (the empty slot's grey)
  after      paused after the red gem, no save point passed -> the red gem's slot lit, the five others empty: the row is the
                                                               live progress (OLEG::UpdateHud reads GameController::progress,
                                                               the one AddGemCommand marks), not the save's
  continued  paused after the GAME OVER page's CONTINUE     -> every slot empty again: CONTINUE restarts from the save's
                                                               snapshot, which a gem joins only at a save point

A slot is lit when at least `lit_red_min` of its box is the red gem's colour and at most `lit_grey_max` the empty slot's grey;
empty when at least `empty_grey_min` is grey and at most `empty_red_max` red. Each judged picture must be in the state
`require_state` (the trace's state when it was presented), and the run must reach GameOver between `after` and `continued`.
Thresholds are FROZEN in the fixture.

Usage: gem_row_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
       gem_row_judge.py --dir DUMP --log run.log [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed / no frames.
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
sys.path.insert(0, HERE)
import skin_judge  # noqa: E402  (the frame-state map and the PPM reader are shared)

DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "gem_row_pal.json")
FRAME = re.compile(r"\[te\.gfx\] frame (\d+) -> \S+")
STATE = re.compile(r"game state @ boundary (\d+): (\w+) \((\d+)\)")


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    mc = os.path.join(out, "mc")
    dump = os.path.join(out, "dump")
    for d in (mc, dump):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    log = os.path.join(out, "run.log")
    args = [binary, "--iso", iso, "--config", "none", "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies",
            "--trace-state", "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]),
            "--mc-dir", mc, "--run-seconds", str(fx.get("run_seconds", 600))]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_EVERY"] = str(fx["dump_every"])
    env["TE_GFX_DUMP_FROM"] = str(fx["dump_from"])
    env["TE_GFX_DUMP_TO"] = str(fx["dump_to"])
    env["TE_GFX_DUMP_AT"] = str(fx["dump_at"])
    env["TE_TEST_ACTIONS"] = fx["test"]
    print("gem_row_judge: running", " ".join(args[1:]), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("gem_row_judge: exit", rc, "->", log, flush=True)
    return dump, log, rc


def log_order(log_path, pictures, game_over):
    """the line numbers of the dumped pictures' frame lines and of the first GameOver state line"""
    at = {}
    over = None
    with open(log_path, errors="replace") as f:
        for n, line in enumerate(f):
            m = FRAME.search(line)
            if m and int(m.group(1)) in pictures and int(m.group(1)) not in at:
                at[int(m.group(1))] = n
                continue
            m = STATE.search(line)
            if m and over is None and int(m.group(3)) == game_over:
                over = (n, int(m.group(1)))
    return at, over


def slot_shares(px, w, box):
    """the shares of the box's pixels in the red gem's colour and in the empty slot's grey"""
    x0, y0, x1, y1 = box
    red = grey = n = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            i = row + x * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            if r - g > 60 and r > 120:
                red += 1
            if abs(r - g) < 14 and abs(g - b) < 14 and 50 < r < 130:
                grey += 1
            n += 1
    return (red / n, grey / n) if n else (0.0, 0.0)


def judge(dump, log, fx):
    pics = fx["pictures"]
    states = skin_judge.frame_states(log)
    at, over = log_order(log, set(pics.values()), fx["game_over_state"])
    slots = fx["slots"]
    red_slot = fx["red_slot"]
    failures = []
    measured = {}
    for name in ("before", "after", "continued"):
        pic = pics[name]
        path = os.path.join(dump, "frame_%06d.ppm" % pic)
        if not os.path.exists(path):
            print("gem_row_judge: no frame for picture %d (%s)" % (pic, name))
            return 2
        state = states.get(pic, "?")
        if state != fx["require_state"]:
            failures.append("%s (picture %d) is in state %s, not %s" % (name, pic, state, fx["require_state"]))
        w, h, px = skin_judge.read_ppm(path)
        if [w, h] != fx["frame"]:
            failures.append("%s (picture %d) is %dx%d, not %dx%d" % (name, pic, w, h, fx["frame"][0], fx["frame"][1]))
            continue
        shares = [slot_shares(px, w, box) for box in slots]
        measured[name] = shares
        for k, (red, grey) in enumerate(shares, start=1):
            lit = red >= fx["lit_red_min"] and grey <= fx["lit_grey_max"]
            empty = grey >= fx["empty_grey_min"] and red <= fx["empty_red_max"]
            want_lit = name == "after" and k == red_slot
            if want_lit and not lit:
                failures.append("%s (picture %d): slot %d is not the red gem (red %.2f grey %.2f): the row does not show the "
                                "gem just picked up" % (name, pic, k, red, grey))
            if not want_lit and not empty:
                failures.append("%s (picture %d): slot %d is not empty (red %.2f grey %.2f)" % (name, pic, k, red, grey))
        print("gem_row_judge: %-9s picture %d state %s slots %s" % (
            name, pic, state, " ".join("%d:%.2f/%.2f" % (k, r, g) for k, (r, g) in enumerate(shares, start=1))))
    if over is None:
        failures.append("the run never reached GameOver (%d)" % fx["game_over_state"])
    elif not (at.get(pics["after"], -1) < over[0] < at.get(pics["continued"], -1)):
        failures.append("GameOver (boundary %d) is not between the pictures 'after' and 'continued'" % over[1])
    else:
        print("gem_row_judge: GameOver at boundary %d, between 'after' and 'continued'" % over[1])
    if failures:
        for f in failures:
            print("gem_row_judge: RED --", f)
        print("gem_row_judge: RED (%d findings)" % len(failures))
        return 1
    print("gem_row_judge: GREEN -- the row shows the red gem the moment it is picked up (slot %d red %.2f), and after the GAME "
          "OVER's CONTINUE it is gone with the save's snapshot (every slot empty), as the retail game does"
          % (red_slot, measured["after"][red_slot - 1][0]))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "judges", "gem_row"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--dir", help="judge an existing dump folder (with --log)")
    ap.add_argument("--log")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    if a.dir:
        if not a.log:
            ap.error("--dir needs --log")
        return judge(a.dir, a.log, fx)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir and --log)")
    dump, log, rc = run(a.bin, a.iso, a.out, fx)
    if rc != 0:
        print("gem_row_judge: the run failed (exit %d)" % rc)
        return 2
    return judge(dump, log, fx)


if __name__ == "__main__":
    sys.exit(main())
