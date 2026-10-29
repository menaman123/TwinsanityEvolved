#!/usr/bin/env python3
"""flicker_judge.py -- whole-frame luminance FLICKER on one pad log (the cryscave dark flicker).

A deterministic headless run of the fixture's pad log with the presented frames dumped every `dump_every` presents; over the
fixture's present window the mean luminance of each frame (below the HUD rows) is compared with its neighbour's: a DIP is a
frame whose mean is under `dip_ratio` x the previous dumped frame's mean AND whose draw count is within `draws_tolerance`
of it (the scene did not change: the same draws, a darker frame). RED when more than `max_dips` dips occur in the window.
Found on bin/twinsanityevolved.966caf5 in Cavern Catastrophe's cryscave: the 3D scene (not the HUD) darkens on one frame
in ~10-15 at a Cd-33 scale with identical draw lists (diag_4315 vs diag_4325); bin/twinsanityevolved.1952747
(before the stencil shadows and the frozen heap layout) shows no dip in the same room.

Usage: flicker_judge.py --bin BIN --iso IMAGE [--fixture F] [--out DIR]     run + judge
       flicker_judge.py --dir DUMPDIR --log run.log [--fixture F]            judge an existing dump
Exit 0 = GREEN, 1 = RED, 2 = the run failed / nothing to judge.
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
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "cavern_cryscave_flicker_pal.json")
FRAME = re.compile(r"\[te\.gfx\] frame (\d+) -> \S+ \(draws (\d+), triangles (\d+)")
STATE = re.compile(r"game state @ boundary (\d+): (\w+) \((\d+)\)")


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    mc, dump = os.path.join(out, "mc"), os.path.join(out, "dump")
    for d in (mc, dump):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    if fx.get("card", "fresh").startswith("warp:"):
        hub = os.path.join(out, "mc_hub")
        shutil.rmtree(hub, ignore_errors=True)
        os.makedirs(hub)
        env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1")
        subprocess.call([binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
                         "--play-pad", os.path.join(ROOT, "fixtures/padlogs/hub_entry.padlog"), "--mc-dir", hub, "--stop-at", "2700"],
                        stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "hub_card.log"), "w"), env=env, cwd=ROOT)
        shutil.rmtree(mc, ignore_errors=True)
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools/warp_save.py"), "--card", hub, "--chunk", fx["card"][5:],
                               "--out", mc, "--place", "0xFFFF"], stdout=subprocess.DEVNULL, cwd=ROOT)
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 900))]
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_DUMP_DIR=dump, TE_GFX_DUMP_EVERY=str(fx["dump_every"]))
    log = os.path.join(out, "run.log")
    print("flicker_judge: running", " ".join(args), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("flicker_judge: exit", rc, "->", log, flush=True)
    # the dumps outside the window are not needed (disk)
    for n in os.listdir(dump):
        m = re.match(r"frame_(\d+)\.ppm$", n)
        if m and not (fx["presents_from"] - fx["dump_every"] <= int(m.group(1)) <= fx["presents_to"]):
            os.remove(os.path.join(dump, n))
    return dump, log, rc


def frames_info(log_path):
    info, states, state = {}, {}, "?"
    with open(log_path, errors="replace") as f:
        for line in f:
            m = STATE.search(line)
            if m:
                state = m.group(2)
                continue
            m = FRAME.search(line)
            if m:
                info[int(m.group(1))] = (int(m.group(2)), state)
    return info


def mean_luminance(path, skip_rows):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():m.end() + w * h * 3]
    start = skip_rows * w * 3
    body = px[start:]
    n = len(body) // 3
    # integer luminance over the body: 0.299 R + 0.587 G + 0.114 B
    r = sum(body[0::3]); g = sum(body[1::3]); b = sum(body[2::3])
    return (0.299 * r + 0.587 * g + 0.114 * b) / n if n else 0.0


def judge(dump, log, fx):
    info = frames_info(log)
    rows = []
    for n in sorted(os.listdir(dump)):
        m = re.match(r"frame_(\d+)\.ppm$", n)
        if not m:
            continue
        k = int(m.group(1))
        if not (fx["presents_from"] - fx["dump_every"] <= k <= fx["presents_to"]):
            continue
        draws, state = info.get(k, (-1, "?"))
        rows.append((k, mean_luminance(os.path.join(dump, n), fx.get("skip_rows", 120)), draws, state))
    if len(rows) < fx.get("min_frames", 20):
        print("flicker_judge: only %d frames in the window -- nothing to judge" % len(rows))
        return 2
    dips = []
    for (k0, l0, d0, s0), (k1, l1, d1, s1) in zip(rows, rows[1:]):
        if s1 != fx.get("require_state", "Playing") or k1 < fx["presents_from"]:
            continue
        if d0 > 0 and abs(d1 - d0) <= fx["draws_tolerance"] and l0 > fx["min_luminance"] and l1 < fx["dip_ratio"] * l0:
            dips.append({"present": k1, "lum": round(l1, 1), "prev": round(l0, 1), "ratio": round(l1 / l0, 2), "draws": d1, "prev_draws": d0})
    lums = [round(l, 1) for _, l, _, _ in rows]
    result = {"frames": len(rows), "dips": dips, "dip_count": len(dips), "luminance_min": min(lums), "luminance_max": max(lums),
              "presents": [rows[0][0], rows[-1][0]]}
    print("flicker_judge:", json.dumps(result))
    if len(dips) > fx["max_dips"]:
        print("  RED: %d whole-frame luminance dips (ratio < %.2f with the same draw count) in presents %d..%d (max %d); worst present %d (%.1f from %.1f)"
              % (len(dips), fx["dip_ratio"], fx["presents_from"], fx["presents_to"], fx["max_dips"],
                 min(dips, key=lambda d: d["ratio"])["present"], min(dips, key=lambda d: d["ratio"])["lum"], min(dips, key=lambda d: d["ratio"])["prev"]))
        print("flicker_judge: RED")
        return 1
    print("flicker_judge: GREEN (%d dips <= %d over %d frames)" % (len(dips), fx["max_dips"], len(rows)))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="an existing dump directory (with --log)")
    ap.add_argument("--log")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the run's directory (default: build/flicker_judge)")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    if a.dir and a.log:
        return judge(a.dir, a.log, fx)
    if not (a.bin and a.iso):
        ap.error("--bin and --iso (run) or --dir and --log (judge an existing dump)")
    if subprocess.call("pgrep -f '(^|/)(twinsanityevolved|ps2recomp_release[.0-9a-f]*)( |$)' > /dev/null", shell=True) == 0:
        print("flicker_judge: a game process is running (one at a time)")
        return 2
    out = a.out or os.path.join(ROOT, "build", "flicker_judge")
    dump, log, rc = run(a.bin, a.iso, out, fx)
    if rc != 0:
        print("flicker_judge: the run failed (exit %d)" % rc)
        return 2
    return judge(dump, log, fx)


if __name__ == "__main__":
    sys.exit(main())
