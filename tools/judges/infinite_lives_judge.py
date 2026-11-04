#!/usr/bin/env python3
"""infinite_lives_judge.py -- INFINITE LIVES: does a death keep its life while the mod is on, and does nothing else change?

Two deterministic headless runs of the hub death pad log (fixtures/padlogs/hub_deaths.padlog: NEW GAME on an empty card, then
seven walks into a nitro crate on N. Sanity Beach, recorded with the mod on), each with --harness-out and the probe every 30
boundaries (the probe line's `dead` and `lives`: GameProgress::counts.lives, read from the game's memory):
  A  the mod off             -> the game's own count: StartLives (5) when play starts, one fewer after every death (4 3 2 1 0),
                                the fifth death the GAME OVER (state 18). The route's reference: it does die.
  B  --mod infinite_lives    -> GREEN needs: at least `deaths_on` deaths, the lives at StartLives on every probe line from the
                                start of play to the end, no GameOver, every death followed by FadingOut -> Restarting ->
                                Playing, one "a life kept" line per death, the run ending Playing.
  A/B  the mod changes nothing but the count: the state traces identical up to A's game over (where B fades out at the same
       boundary instead), every frame identical before A's first death, and at the judged present (the first death's lives
       panel) the frames differ only inside the panel's number (`hud_region`, at least `hud_min_pixels` pixels): A shows one
       fewer, B the number it showed before.
A runner refusing `--mod infinite_lives` (a build without the mod: exit 2) is RED.

Usage: infinite_lives_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
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
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "infinite_lives_pal.json")
PROBE = re.compile(r"^\[probe\] @(\d+) pos .*? dead (\d) .*? gstate (\d+) .*? lives (\d+)$")
KEPT = re.compile(r"infinite lives: a life kept @ boundary (\d+)")
GAME_OVER, PLAYING, FADING_OUT, RESTARTING = 18, 12, 21, 22


def run(binary, iso, out, fx, extra, tag):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    dump, mc = os.path.join(d, "dump"), os.path.join(d, "card", "mc")   # the -mods card lands beside mc, inside the run's folder
    os.makedirs(dump)
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--config", "none", "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies",
            "--trace-state", "--trace-player", str(fx["probe_every"]), "--play-pad", os.path.join(ROOT, fx["padlog"]),
            "--stop-at", str(fx["stop_at"]), "--mc-dir", mc, "--run-seconds", str(fx.get("run_seconds", 600)),
            "--harness-out", os.path.join(d, "harness")] + extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_AT"] = str(fx["present"])
    print("infinite_lives_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("infinite_lives_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return d, rc, log


def probes(log):
    """[(boundary, dead, gstate, lives)] in the log's order"""
    out = []
    with open(log, errors="replace") as f:
        for line in f:
            m = PROBE.match(line.rstrip("\n"))
            if m:
                out.append(tuple(int(v) for v in m.groups()))
    return out


def deaths(samples):
    """the boundaries of the probe lines where the character is newly dead (a rising edge of `dead`)"""
    out, was = [], 0
    for b, dead, _, _ in samples:
        if dead and not was:
            out.append(b)
        was = dead
    return out


def trace(d):
    rows = []
    with open(os.path.join(d, "harness", "state.trace")) as f:
        for line in f:
            p = line.strip().split(",")
            if len(p) >= 2 and p[0].isdigit():
                rows.append((int(p[0]), int(p[1])))
    return rows


def fhash(d):
    out = {}
    with open(os.path.join(d, "harness", "frames.fhash")) as f:
        for line in f:
            p = line.strip().split(",")
            if len(p) == 2 and p[0].isdigit():
                out[int(p[0])] = p[1]
    return out


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
    return w, h, data[pos:pos + 3 * w * h]


def collapse(values):
    out = []
    for v in values:
        if not out or out[-1] != v:
            out.append(v)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "infinite_lives_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    da, rca, log_a = run(a.bin, a.iso, a.out, fx, [], "A_mod_off")
    db, rcb, log_b = run(a.bin, a.iso, a.out, fx, ["--mod", "infinite_lives"], "B_infinite_lives")
    if rcb == 2:
        print("infinite_lives_judge: RED -- the runner refuses --mod infinite_lives (no infinite lives mod)")
        return 1
    if rca != 0 or rcb != 0:
        print("infinite_lives_judge: a run failed (A exit %d, B exit %d)" % (rca, rcb))
        return 2
    start = fx["start_lives"]
    reasons = []

    # A: the game's own count
    pa = probes(log_a)
    ta = trace(da)
    over_a = next((b for b, s in ta if s == GAME_OVER), None)
    deaths_a = [b for b in deaths(pa) if over_a is None or b <= over_a + fx["probe_every"]]
    lives_a = collapse([l for b, dead, gs, l in pa if (over_a is None or b <= over_a) and (gs == PLAYING or dead)])
    want_a = list(range(start, start - fx["deaths_off"] - 1, -1))
    if len(deaths_a) != fx["deaths_off"]:
        reasons.append("A: %d deaths before the game over, not %d" % (len(deaths_a), fx["deaths_off"]))
    if over_a is None:
        reasons.append("A: no game over (the route's fifth death should end the lives)")
    if lives_a[:len(want_a)] != want_a:
        reasons.append("A: the lives went %s, not %s" % (lives_a, want_a))

    # B: the mod on
    pb = probes(log_b)
    tb = trace(db)
    deaths_b = deaths(pb)
    play_b = [(b, l) for b, dead, gs, l in pb if gs == PLAYING or dead]
    first_play = next((b for b, s in tb if s == PLAYING), None)
    lives_b = sorted(set(l for b, l in play_b if first_play is not None and b >= first_play))
    with open(log_b, errors="replace") as f:
        kept = [int(m.group(1)) for m in KEPT.finditer(f.read())]
    over_b = [b for b, s in tb if s == GAME_OVER]
    if len(deaths_b) < fx["deaths_on"]:
        reasons.append("B: %d deaths, the route makes %d" % (len(deaths_b), fx["deaths_on"]))
    if lives_b != [start]:
        reasons.append("B: the lives seen in play %s, not only %d" % (lives_b, start))
    if over_b:
        reasons.append("B: a game over at %s" % over_b)
    if len(kept) != len(deaths_b):
        reasons.append("B: %d 'a life kept' lines for %d deaths" % (len(kept), len(deaths_b)))
    restarts = 0
    for i, dead_at in enumerate(deaths_b):
        until = deaths_b[i + 1] if i + 1 < len(deaths_b) else 1 << 62
        after = [s for b, s in tb if dead_at - fx["probe_every"] < b < until]
        if FADING_OUT in after and RESTARTING in after and PLAYING in after[after.index(RESTARTING):]:
            restarts += 1
        else:
            reasons.append("B: the death at ~%d is not followed by FadingOut -> Restarting -> Playing (%s)" % (dead_at, after))
    if not tb or tb[-1][1] != PLAYING:
        reasons.append("B: the run ends in state %s, not Playing" % (tb[-1][1] if tb else None))

    # A/B: nothing but the count
    common = 0
    while common < min(len(ta), len(tb)) and ta[common] == tb[common]:
        common += 1
    fork = "A %s / B %s" % (ta[common] if common < len(ta) else None, tb[common] if common < len(tb) else None)
    if over_a is None or common >= len(ta) or ta[common] != (over_a, GAME_OVER) or common >= len(tb) or tb[common] != (over_a, FADING_OUT):
        reasons.append("A/B: the state traces part at %s, not at A's game over against B's fade-out" % fork)
    ha, hb = fhash(da), fhash(db)
    keys = sorted(set(ha) & set(hb))
    first_diff = next((k for k in keys if ha[k] != hb[k]), None)
    first_death = deaths_a[0] if deaths_a else None
    if first_death is None or first_diff is None or first_diff < first_death - fx["probe_every"]:
        reasons.append("A/B: the first differing frame is at %s, A's first death at ~%s" % (first_diff, first_death))
    fa = os.path.join(da, "dump", "frame_%06d.ppm" % fx["present"])
    fb = os.path.join(db, "dump", "frame_%06d.ppm" % fx["present"])
    hud = "-"
    if not (os.path.isfile(fa) and os.path.isfile(fb)):
        reasons.append("A/B: no picture %d in both runs" % fx["present"])
    else:
        w, h, pxa = read_ppm(fa)
        _, _, pxb = read_ppm(fb)
        diff = [i // 3 for i in range(0, min(len(pxa), len(pxb)), 3) if pxa[i:i + 3] != pxb[i:i + 3]]
        x0, y0, x1, y1 = fx["hud_region"]
        inside = [p for p in diff if x0 * w <= p % w < x1 * w and y0 * h <= p // w < y1 * h]
        box = (min(p % w for p in diff), min(p // w for p in diff), max(p % w for p in diff), max(p // w for p in diff)) if diff else None
        hud = "%d pixels differ, %d inside the panel's box %s, their box %s" % (len(diff), len(inside), fx["hud_region"], box)
        if len(inside) < fx["hud_min_pixels"] or len(inside) != len(diff):
            reasons.append("A/B: at picture %d %s" % (fx["present"], hud))

    print("infinite_lives_judge: A (off): deaths at %s, lives %s, game over at %s. B (on): deaths at %s, lives in play %s, "
          "lives kept at %s, restarts %d/%d, game overs %d, last state %s. A/B: state traces identical for %d lines, parting at %s; "
          "frames identical to %s (first death ~%s); picture %d: %s" % (
              deaths_a, lives_a, over_a, deaths_b, lives_b, kept, restarts, len(deaths_b), len(over_b), tb[-1] if tb else None,
              common, fork, first_diff, first_death, fx["present"], hud))
    if not reasons:
        print("infinite_lives_judge: GREEN -- with the mod off the lives go %s and the fifth death is the game over; with it on "
              "%d deaths keep the lives at %d, each restarts at the checkpoint, no game over; nothing else differs before the "
              "first death and the lives panel's number is the only difference after it" % (
                  "-".join(str(v) for v in want_a), len(deaths_b), start))
        return 0
    for r in reasons:
        print("infinite_lives_judge:   " + r)
    print("infinite_lives_judge: RED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
