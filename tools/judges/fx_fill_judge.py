#!/usr/bin/env python3
"""fx_fill_judge.py -- the frozen census judge of the fill a 4x frame draws where the fog and the waterfall are: per leg, a
deterministic headless run at internal scale 4 with the GPU phase timer's fill census (TE_GFX_GPU_TIMERS=breaks: the
fragments that pass the depth / stencil tests per phase, counted by the GPU -- the same counts on any machine for the same
frames; no time is judged) and the renderer's own census lines, over the leg's PLAY windows.

    fx_fill_judge.py --bin BUILD/twinsanityevolved --iso <disc image> [--out DIR] [--fixture fixtures/judges/fx_fill_pal.json]
    fx_fill_judge.py --log LEG=RUN_LOG [--log ...] [--fixture ...]     # judge runs already made (the fixture's leg names)
    fx_fill_judge.py ... --legs a,b                                      # only those legs (run and judged)

Judged per leg (the fixture freezes the bounds):
  * sky_moved_min_share / sky_layers_min: the windows' frames whose sky waited for the opaque scene ("[te.gfx] the sky after
    the opaque scene: M of N frames ... per moved frame L sky layers") -- the 10th dimension's skies of 48-62 layers
  * sky_passed_max: the sky's fragments passed a frame (Mpx, the mean over the play windows)
  * blended_passed_max: the blended draws' fragments passed in the leg's WORST window (Mpx a frame): the fog sheets / the fall
  * fast_program: every draw of every window takes the PS2 fast program ("[te.gfx] PS2 fragment programs per present
    (TE_GFX_PS2_FAST): fast X, full 0.0")
  * copies_taken_max: the screen copies taken a frame in the worst window (0: the leg takes none)
The card: the hub card hub_entry.padlog writes by 2700 (an empty --mc-dir), warped by tools/warp_save.py to the leg's chunk
when the fixture names one; "card": "empty" for a NEW GAME pad log. A pad log's "# test=" line is the leg's TE_TEST_ACTIONS.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "fx_fill_pal.json")

PACE = re.compile(r"\[te\.gfx\] pace: presents (\d+)-(\d+) at")
FILL = re.compile(r"\[te\.gfx\] gpu fill per frame \([^)]*\):(.*)$")
SKY = re.compile(r"\[te\.gfx\] the sky after the opaque scene \(TE_GFX_SKY_LAST\): (\d+) of (\d+) frames .*?per moved frame ([\d.]+) sky layers")
FAST = re.compile(r"\[te\.gfx\] PS2 fragment programs per present \(TE_GFX_PS2_FAST\): fast ([\d.]+), full ([\d.]+) draws")
COPIES = re.compile(r"\[te\.gfx\] screen copies per frame \(presents (\d+)-(\d+);.*?copies TAKEN ([\d.]+)")
PLAYING = re.compile(r"game state @ boundary (\d+): Playing \(12\)")
STOP = re.compile(r"--stop-at (\d+) reached")


def parse(path):
    wins, cur = [], None
    playing, stop = None, None
    for line in open(path, errors="replace"):
        m = PLAYING.search(line)
        if m and playing is None:
            playing = int(m.group(1))
        m = STOP.search(line)
        if m:
            stop = int(m.group(1))
        m = PACE.search(line)
        if m:
            cur = {"from": int(m.group(1)), "to": int(m.group(2))}
            wins.append(cur)
            continue
        if cur is None:
            continue
        m = FILL.search(line)
        if m:
            for name, rest in re.findall(r"([a-z]+) ((?:[\d.]+ drawn )?[\d.]+ passed)", m.group(1)):
                cur["p:" + name] = float(re.match(r"(?:[\d.]+ drawn )?([\d.]+) passed", rest).group(1))
            continue
        m = SKY.search(line)
        if m:
            cur["sky_moved"], cur["sky_frames"], cur["sky_layers"] = int(m.group(1)), int(m.group(2)), float(m.group(3))
            continue
        m = FAST.search(line)
        if m:
            cur["fast"], cur["full"] = float(m.group(1)), float(m.group(2))
            continue
        m = COPIES.search(line)
        if m:
            for w in wins:
                if abs(w["to"] - int(m.group(2))) <= 2:
                    w["copies"] = float(m.group(3))
    # presents lag boundaries: the offset from the stop boundary and the last window's end
    offset = (stop - wins[-1]["to"]) if (stop and wins) else 175
    first = (playing - offset + 30) if playing is not None else None
    play = [w for w in wins if first is not None and w["from"] >= first]
    return play, playing, offset


def judge_leg(name, leg, path):
    play, playing, offset = parse(path)
    out = {"leg": name, "windows": len(play), "playing": playing, "checks": []}
    ok = True

    def check(what, good, detail):
        nonlocal ok
        ok = ok and good
        out["checks"].append(("ok  " if good else "FAIL") + f" {what}: {detail}")

    check("play windows", len(play) >= leg.get("min_windows", 5), f"{len(play)} (Playing at boundary {playing}, presents lag by {offset})")
    if not play:
        return False, out
    if "sky_moved_min_share" in leg:
        moved = sum(w.get("sky_moved", 0) for w in play)
        frames = sum(w.get("sky_frames", 0) for w in play)
        share = moved / frames if frames else 0.0
        layers = max((w.get("sky_layers", 0.0) for w in play), default=0.0)
        check("the sky after the opaque scene", share >= leg["sky_moved_min_share"] and layers >= leg.get("sky_layers_min", 0),
              f"{moved} of {frames} frames moved ({share:.3f}, at least {leg['sky_moved_min_share']}), {layers:.1f} layers "
              f"(at least {leg.get('sky_layers_min', 0)})")
    if "sky_passed_max" in leg:
        v = sum(w.get("p:sky", 0.0) for w in play) / len(play)
        check("sky fragments passed a frame", v <= leg["sky_passed_max"], f"{v:.2f} Mpx (at most {leg['sky_passed_max']})")
    if "blended_passed_max" in leg:
        worst = max(play, key=lambda w: w.get("p:blended", 0.0))
        v = worst.get("p:blended", 0.0)
        check("blended fragments passed a frame, the worst window", v <= leg["blended_passed_max"] and v >= leg.get("blended_passed_min", 0.0),
              f"{v:.2f} Mpx at presents {worst['from']}-{worst['to']} (between {leg.get('blended_passed_min', 0.0)} and "
              f"{leg['blended_passed_max']})")
    if leg.get("fast_program"):
        with_census = [w for w in play if "fast" in w]
        full = max((w.get("full", 0.0) for w in with_census), default=0.0)
        fast = min((w.get("fast", 0.0) for w in with_census), default=0.0)
        check("every draw takes the PS2 fast program", len(with_census) == len(play) and full == 0.0 and fast > 0.0,
              f"{len(with_census)} of {len(play)} windows with the census, full draws a present at most {full:.1f}, fast at "
              f"least {fast:.1f}")
    if "copies_taken_max" in leg:
        v = max((w.get("copies", 0.0) for w in play), default=0.0)
        check("screen copies taken a frame, the worst window", v <= leg["copies_taken_max"], f"{v:.2f} (at most {leg['copies_taken_max']})")
    return ok, out


def test_actions(padlog):
    for line in open(padlog, errors="replace"):
        if line.startswith("# test="):
            return line[len("# test="):].split("  (")[0].strip()
    return None


def run(binary, iso, out, fx, names):
    os.makedirs(out, exist_ok=True)
    hub = os.path.join(out, "mc_hub")
    shutil.rmtree(hub, ignore_errors=True)
    os.makedirs(hub)
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1")
    rc = subprocess.call([binary, "--iso", iso, "--config", "none", "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies",
                          "--trace-state", "--play-pad", os.path.join(ROOT, "fixtures", "padlogs", "hub_entry.padlog"), "--mc-dir", hub,
                          "--stop-at", "2700"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if rc != 0:
        raise SystemExit(f"fx_fill_judge: the hub card run failed (rc {rc})")
    ini = os.path.join(out, "ps2r.ini")
    with open(ini, "w") as f:
        f.write("[video]\nscale = %d\n" % fx.get("scale", 4))
    logs = {}
    for name, leg in fx["legs"].items():
        if name not in names:
            continue
        mc = os.path.join(out, "mc_" + name)
        shutil.rmtree(mc, ignore_errors=True)
        if leg.get("card") == "empty":
            os.makedirs(mc)
        elif leg.get("chunk"):
            subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "warp_save.py"), "--card", hub, "--chunk", leg["chunk"],
                                   "--out", mc, "--place", "0xFFFF"], stdout=subprocess.DEVNULL)
        else:
            shutil.copytree(hub, mc)
        padlog = os.path.join(ROOT, leg["padlog"])
        env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_GPU_TIMERS="breaks", TE_GFX_SUMMARY_EVERY=str(fx.get("summary_every", 60)))
        actions = test_actions(padlog)
        if actions:
            env["TE_TEST_ACTIONS"] = actions
        log = os.path.join(out, name + ".log")
        with open(log, "w") as f:
            rc = subprocess.call([binary, "--iso", iso, "--config", ini, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies",
                                  "--trace-state", "--play-pad", padlog, "--mc-dir", mc, "--stop-at", str(leg["stop_at"]),
                                  "--run-seconds", str(fx.get("run_seconds", 600))], env=env, stdout=f, stderr=subprocess.STDOUT)
        shutil.rmtree(mc, ignore_errors=True)
        if rc != 0:
            print(f"fx_fill_judge: {name}: the run exited {rc}")
        logs[name] = log
    shutil.rmtree(hub, ignore_errors=True)
    return logs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--out")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--log", action="append", default=[])
    ap.add_argument("--legs", help="comma-separated leg names (default: every leg of the fixture)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    names = a.legs.split(",") if a.legs else list(fx["legs"])
    if a.log:
        logs = dict(x.split("=", 1) for x in a.log)
    else:
        if not a.bin or not a.iso:
            ap.error("--bin and --iso (or --log LEG=FILE)")
        out = a.out or tempfile.mkdtemp(prefix="fx_fill_")
        logs = run(a.bin, a.iso, out, fx, names)
    all_ok = True
    for name, leg in fx["legs"].items():
        if name not in names:
            continue
        if name not in logs:
            print(f"fx_fill_judge: {name}: no log")
            all_ok = False
            continue
        ok, res = judge_leg(name, leg, logs[name])
        all_ok = all_ok and ok
        print(f"fx_fill_judge: {name} ({leg.get('what', '')}): {'GREEN' if ok else 'RED'}")
        for c in res["checks"]:
            print("    " + c)
    print("fx_fill_judge: verdict " + ("GREEN" if all_ok else "RED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
