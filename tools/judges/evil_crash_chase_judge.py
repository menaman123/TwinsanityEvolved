#!/usr/bin/env python3
"""evil_crash_chase_judge.py -- Bandicoot Pursuit: does Evil Crash chase Cortex past the first step of his route?

The report: "during Evil Crash chasing Cortex, Evil Crash gets glitched out and doesn't fully chase Cortex". The level (the
Doc Amok of Twinsanity Island, levels\\altearth\\hub\\altdoc): the player is Crash; Cortex runs a route of his own and Evil
Crash follows him (COM_ALTEARTH_DOACMOK_EVILCRASH_MOVE: a ground chase to each step of a route to the phase's key, the next
step once his collision box is inside the step's circle). That graph's SetFocusPosition carries the straight-ahead option in an
option word of what the authoring tool left in memory: the target is the step's distance along his facing, which misses the
first step (13.9 away, 6.3 degrees off his placement's facing) by 1.53 against the step's 1.0 radius; his chase packets have no
tolerance, so he stands at the missed point for good (retail behaviour: the same code and data on the console). The fix,
Platform::System::RouteStepsReached (game.route_steps = reached, the default): a step missed by more than half its circle is
aimed at itself.

Two deterministic headless runs from the same card (the hub card hub_entry.padlog writes, warped to altdoc at story 21 with
tools/warp_save.py) and the same pad log (LOAD GAME), with the test requests of the fixture (TE_TEST_ACTIONS: Crash put on the
chase's checkpoint crate, which starts the Doc Amok, then onto the pressure pad that holds the piston up so Cortex crosses the
first gap) and the cutscene census's state lines every boundary of a window (TE_CUTSCENE_STATE): RETAIL (a ps2r.ini with
game.route_steps = retail) and FIXED (= reached). GREEN iff
  - RETAIL reproduces the stop: Evil Crash's x never goes below `retail_min_x` while Cortex runs on, and he moves less than
    `still` over the last `still_window` boundaries before Cortex's run ends,
  - FIXED chases: Evil Crash's x goes below `fixed_max_x`, he runs his JUMP graph (5507: the rant's jump to the next key) at
    least once, and he is within `follow` of Cortex when Cortex's run ends,
  - Cortex's own run is the same in both: it ends within `cortex_end_slack` boundaries of the other's and his place stays within
    `cortex_tolerance` at every state line until then (Evil Crash running a few units behind him moves him by a tenth of a unit
    at most), and each run's log says what the game asked ([te.route] ... reached / retail).
On the binary before the fix both runs stop (FIXED fails); a binary that no longer reproduces the retail stop is RED too.

Usage: evil_crash_chase_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = a run failed.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "evil_crash_chase_pal.json")


def hub_card(binary, iso, out):
    card = os.path.join(out, "mc_hub")
    if os.path.isdir(card):
        return card
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1")
    with open(os.path.join(out, "mc_hub.log"), "w") as f:
        subprocess.check_call([binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
                               "--play-pad", os.path.join(ROOT, "fixtures", "padlogs", "hub_entry.padlog"), "--mc-dir", card,
                               "--stop-at", "2700"], stdout=f, stderr=subprocess.STDOUT, env=env, cwd=ROOT)
    return card


def run(binary, iso, out, fx, card, mode):
    d = os.path.join(out, mode)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    mc = os.path.join(d, "mc")
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "warp_save.py"), "--card", card, "--chunk", fx["chunk"], "--out", mc,
                           "--place", "0xFFFF", "--story", str(fx["story"])], stdout=subprocess.DEVNULL)
    ini = os.path.join(d, "ps2r.ini")
    with open(ini, "w") as f:
        f.write("game.route_steps = %s\n" % ("retail" if mode == "retail" else "reached"))
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--config", ini, "--trace-state",
            "--trace-player", "0", "--play-pad", os.path.join(ROOT, fx["padlog"]), "--mc-dir", mc, "--stop-at", str(fx["stop_at"]),
            "--run-seconds", "600"]
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1", TE_TEST_ACTIONS=fx["test"],
               TE_CUTSCENE_STATE="%d@%d-%d" % (fx["every"], fx["window"][0], fx["window"][1]))
    print("evil_crash_chase_judge: run %s: TE_TEST_ACTIONS='%s' %s" % (mode, fx["test"], " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=ROOT)
    return rc, log


def track(log, chunk, evil, cortex):
    """per state line: (boundary, Evil Crash's place, his graphs, Cortex's place, his graphs) in the chunk"""
    rows, routes = [], []
    for line in open(log, errors="replace"):
        if line.startswith("[te.route]"):
            routes.append(line.strip())
        if not line.startswith("[cs.state] {"):
            continue
        j = json.loads(line[11:])
        e = c = None
        for ch in j.get("chunks", []):
            if ch["c"].lower() != chunk.lower():
                continue
            for i in ch["i"]:
                graphs = i["r"][0] if isinstance(i.get("r", [0])[0], list) else []
                if i["n"].endswith(evil):
                    e = (i["p"], [g[0] for g in graphs])
                elif i["n"].endswith(cortex):
                    c = (i["p"], [g[0] for g in graphs])
        if e and c:
            rows.append((j["b"], e[0], e[1], c[0], c[1]))
    return rows, routes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "evil_crash_chase_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    os.makedirs(a.out, exist_ok=True)
    card = hub_card(a.bin, a.iso, a.out)
    tracks = {}
    for mode in ("retail", "fixed"):
        rc, log = run(a.bin, a.iso, a.out, fx, card, mode)
        if rc != 0:
            print("evil_crash_chase_judge: the %s run exited %d" % (mode, rc))
            return 2
        tracks[mode] = track(log, fx["chunk"], fx["evil_crash"], fx["cortex"])
    green = True
    ends = {}
    for mode in ("retail", "fixed"):
        rows, routes = tracks[mode]
        # Cortex's run ends where his route behaviour (5179) is no longer his (his death, the gap or a hazard)
        running = [r for r in rows if fx["cortex_graph"] in r[4]]
        ends[mode] = running[-1][0] if running else None
        chase = [r for r in rows if fx["phase_graph"] in r[2] and (ends[mode] is None or r[0] <= ends[mode])]
        if not chase or ends[mode] is None:
            print("evil_crash_chase_judge: %s: no chase in the window (Evil Crash's %d lines, Cortex's run %s)" % (mode, len(chase), ends[mode]))
            return 2
        min_x = min(r[1][0] for r in chase)
        last = [r for r in chase if r[0] > ends[mode] - fx["still_window"]]
        moved = max(abs(p - q) for r in last for s in last for p, q in zip(r[1], s[1])) if last else 0.0
        jumped = any(fx["jump_graph"] in r[2] for r in chase)
        at_end = [r for r in rows if r[0] == ends[mode]][0]
        gap = ((at_end[1][0] - at_end[3][0]) ** 2 + (at_end[1][2] - at_end[3][2]) ** 2) ** 0.5
        said = [r for r in routes if ("= retail" if mode == "retail" else "= reached") in r]
        if mode == "retail":
            ok = min_x >= fx["retail_min_x"] and moved < fx["still"] and bool(said)
            verdict = "stops: his x never below %.2f (%.1f), moved %.3f over his last %d boundaries" % (fx["retail_min_x"], min_x, moved, fx["still_window"])
        else:
            ok = min_x <= fx["fixed_max_x"] and jumped and gap <= fx["follow"] and bool(said)
            verdict = "chases: his x down to %.1f (<= %.1f), the jump graph %s, %.2f from Cortex when Cortex's run ends" % (min_x, fx["fixed_max_x"], "run" if jumped else "NOT run", gap)
        print("evil_crash_chase_judge: %s: Cortex's run b%d..b%d; Evil Crash %s; the game asked %d times (%s) -> %s"
              % (mode, running[0][0], ends[mode], verdict, len(routes), routes[0].split("-> ")[-1] if routes else "never", "OK" if ok else "FAIL"))
        green = green and ok
    # Cortex's run the same in both
    r_rows = {r[0]: r[3] for r in tracks["retail"][0] if r[0] <= ends["retail"]}
    f_rows = {r[0]: r[3] for r in tracks["fixed"][0] if r[0] <= ends["fixed"]}
    common = sorted(set(r_rows) & set(f_rows))
    worst = max((max(abs(p - q) for p, q in zip(r_rows[b], f_rows[b])) for b in common), default=99.0)
    same = abs(ends["retail"] - ends["fixed"]) <= fx["cortex_end_slack"] and worst <= fx["cortex_tolerance"] and len(common) > 100
    print("evil_crash_chase_judge: Cortex's run: ends b%s / b%s, %d state lines compared, worst difference %.4f -> %s"
          % (ends["retail"], ends["fixed"], len(common), worst, "the same" if same else "DIFFERENT"))
    green = green and same
    if green:
        print("evil_crash_chase_judge: GREEN -- with game.route_steps = retail Evil Crash stands at the first step's missed point as "
              "on the console; with the fix he follows Cortex, rants and jumps on, and Cortex's own run is the same")
        return 0
    print("evil_crash_chase_judge: RED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
