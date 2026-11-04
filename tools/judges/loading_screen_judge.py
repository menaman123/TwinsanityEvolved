#!/usr/bin/env python3
"""loading_screen_judge.py -- THE LOADS AT THE DEVICE'S SPEED (disc.speed = device: platform/nativeee/host/te_loading.h): LOAD
GAME's two loads, the title's (LoadingTitle) and the hub's (LoadingLevel), take a few dozen boundaries and under a second, every
boundary paced, the loading picture shown, and the level they load is the one the console's pace loads.

    loading_screen_judge.py --bin BUILD/twinsanityevolved --iso <PAL disc> [--out DIR] [--fixture fixtures/judges/loading_screen_pal.json]

The route: the fixture's plan (fixtures/padlogs/load_game.plan: LOAD GAME's presses anchored on the game states, so it replays at
either speed) on the hub card the hub pad log writes. The legs:
  A  deterministic, headless, the console's pace (the profile's default), --harness-out, --trace-player: the reference -- each
     load's boundaries, the loading picture as the console shows it, the probe's dump of the level after the hub's load; it
     saves a state one boundary before each load
  B  deterministic, headless, --disc-speed device, --harness-out, --trace-player
  C  PLAY mode in a window (the default disc speed: the device's), vsync off (--config none), --trace-player
  Cv the same with vsync on (a settings file with video.vsync = on)
  S  PLAY mode headless (the synchronous presenter), --harness-out: the pictures a load shows in play (a window's drawing thread
     and the harness race at the run's exit -- te::harness::OnPresent after Finalize -- so the window legs have no harness)
  D  PLAY mode from each of A's states, --disc-speed console: the console's pace
GREEN iff
  * A's loads are the fixture's (the state, its first boundary, the boundary of the state after it);
  * in B each load lasts between fast_min and fast_max boundaries, and the census line "[te.load] <state> @<from>-<to>: <n>
    boundaries ... at the device's speed" names it; B's pictures through each load are A's: from the first present that differs
    from the load's first (the picture fading in) A's sequence frame for frame, the picture then held (A's held picture: its
    descriptor, the most common one in A's load) for at least picture_min presents;
  * the probe's dump at the hub's load's end (the player, the chunk, its links, every instance's name / kind / place / box /
    flags, the next samples; boundaries, clock and addresses blanked) is A's in B and in C;
  * in C, Cv and S each load takes at most fast_max boundaries, its census line says the device's speed, and every boundary of
    the run was paced (the clock's exit line: parks + late + timeouts = boundaries); in C and Cv at most max_wall_s of wall
    time (its stamped state lines); in S at least picture_min_play presents of the load carry A's held picture;
  * in D each load ends at A's boundary and takes at least paced_min_wall_s.
Exit 0 GREEN, 1 RED, 2 the setup failed. Writes OUT/report.json.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "loading_screen_pal.json")
STATE = re.compile(r"\[twinsanityevolved\] game state @ boundary (\d+): (\w+) \((\d+)\)")
CENSUS = re.compile(r"\[te\.load\] (\w+) @(\d+)-(\d+): (\d+) boundaries in ([\d.]+) s at the (device's speed|console's pace)")
CLOCK = re.compile(r"\[twinsanityevolved\] clock: (\d+) boundaries, (\d+) fields, (\d+) parks, (\d+) late, (\d+) timeouts")


def say(*a):
    print("loading_screen_judge:", *a, flush=True)


def make_card(binary, iso, out):
    """The hub card the hub pad log writes (drawing off)."""
    hub = os.path.join(out, "mc_hub")
    shutil.rmtree(hub, ignore_errors=True)
    os.makedirs(hub)
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1")
    with open(os.path.join(out, "hub_card.log"), "w") as log:
        rc = subprocess.call([binary, "--iso", iso, "--config", "none", "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies",
                              "--play-pad", os.path.join(ROOT, "fixtures/padlogs/hub_entry.padlog"), "--mc-dir", hub, "--stop-at", "2700"],
                             stdout=subprocess.DEVNULL, stderr=log, env=env, cwd=ROOT)
    return hub if rc == 0 else None


def run(args, log, env):
    """The runner with its stderr into `log`, each line stamped with time.monotonic()."""
    with open(log, "w") as f:
        p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env, cwd=ROOT, text=True, errors="replace")
        for line in p.stderr:
            f.write("%.4f %s" % (time.monotonic(), line))
        p.wait()
    return p.returncode


def lines_of(log):
    """[(t, text)]"""
    out = []
    with open(log, errors="replace") as f:
        for line in f:
            ts, _, text = line.partition(" ")
            try:
                out.append((float(ts), text))
            except ValueError:
                pass
    return out


def loads_of(lines, names):
    """Each load: its state, first boundary, the boundary of the state after it, the wall seconds between the two lines."""
    states = []
    for t, text in lines:
        m = STATE.search(text)
        if m and (not states or states[-1][2] != m.group(2)):
            states.append((t, int(m.group(1)), m.group(2)))
    out = []
    for k, (t, b, name) in enumerate(states):
        if name in names and k + 1 < len(states):
            t2, b2, _ = states[k + 1]
            out.append({"state": name, "from": b, "to": b2, "boundaries": b2 - b, "wall_s": round(t2 - t, 3)})
    return out


def census_of(lines):
    out = []
    for _, text in lines:
        m = CENSUS.search(text)
        if m:
            out.append({"state": m.group(1), "from": int(m.group(2)), "to": int(m.group(3)), "boundaries": int(m.group(4)),
                        "speed": "device" if m.group(6).startswith("device") else "console", "line": text.strip()})
    return out


def clock_of(lines):
    for _, text in lines:
        m = CLOCK.search(text)
        if m:
            return {k: int(v) for k, v in zip(("boundaries", "fields", "parks", "late", "timeouts"), m.groups())}
    return None


def pictures(path):
    """frames.pdesc: key -> (mid-band luminance, dhash)"""
    rows = {}
    if not os.path.isfile(path):
        return rows
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.strip().split(",")
            if len(p) >= 11 and p[0].isdigit():
                rows[int(p[0])] = (float(p[4]), p[10])
    return rows


def norm(line):
    line = re.sub(r"@\d+ ", "@N ", line)
    line = re.sub(r" clock \d+ frame_time \d+", "", line)
    return re.sub(r"(inst|asleep|linked|dynamicScenery|chunk|nodes) 0x[0-9A-F]+", r"\1 0x*", line).rstrip()


def level_after(lines, state, samples):
    """The probe's output after the load's census line: its dump at the load's end and the next samples, normalised."""
    out, after, seen = [], False, 0
    for _, text in lines:
        if text.startswith("[te.load] %s @" % state):
            after = True
            continue
        if not after or not text.startswith("[probe]"):
            continue
        if re.match(r"\[probe\] @\d+ pos ", text):
            seen += 1
            if seen > samples:
                break
        out.append(norm(text))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "loading_screen_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    binary = os.path.abspath(a.bin)
    out = os.path.abspath(a.out)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    report = {"judge": "loading_screen_judge", "result": "RED", "reasons": [], "legs": {}}
    reasons = report["reasons"]
    names = [l["state"] for l in fx["loads"]]
    plan = os.path.join(ROOT, fx["plan"])

    def finish(code):
        report["result"] = "GREEN" if code == 0 else ("RED" if code == 1 else "SETUP FAILED")
        with open(os.path.join(out, "report.json"), "w") as f:
            json.dump(report, f, indent=1)
        for r in reasons:
            say("  -", r)
        say("%s -- %s" % (report["result"], report.get("verdict", "; ".join(reasons[:3]))))
        for leg in ("A", "B", "C", "Cv", "S", "D-LoadingTitle", "D-LoadingLevel"):
            for d in ("mc", "states"):
                shutil.rmtree(os.path.join(out, leg, d), ignore_errors=True)
        shutil.rmtree(os.path.join(out, "mc_hub"), ignore_errors=True)
        return code

    hub = make_card(binary, a.iso, out)
    if not hub:
        reasons.append("the hub card failed (hub_card.log)")
        return finish(2)

    def leg(name, extra, play=False, vsync=False, saves=None, stop=None, harness=True, headless=None):
        d = os.path.join(out, name)
        os.makedirs(d, exist_ok=True)
        mc = os.path.join(d, "mc")
        shutil.copytree(hub, mc)
        config = "none"
        if vsync:
            config = os.path.join(d, "ps2r.ini")
            with open(config, "w") as f:
                f.write("video.vsync = on\n")
        args = [binary, "--iso", a.iso, "--config", config, "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
                "--trace-player", str(fx["probe_every"]), "--autopilot", plan, "--mc-dir", mc, "--stop-at", str(stop or fx["stop_at_fast"]),
                "--run-seconds", "300"]
        if harness:
            args += ["--harness-out", os.path.join(d, "leg")]
        if not play:
            args.append("--deterministic")
        for path_at in saves or []:
            args += ["--save-state", path_at]
        env = dict(os.environ)
        env.pop("TE_GFX_HEADLESS", None)
        if headless if headless is not None else not play:
            env["TE_GFX_HEADLESS"] = "1"
        rc = run(args + extra, os.path.join(d, "run.log"), env)
        lines = lines_of(os.path.join(d, "run.log"))
        return {"rc": rc, "lines": lines, "loads": loads_of(lines, names), "census": census_of(lines), "clock": clock_of(lines),
                "pictures": pictures(os.path.join(d, "leg", "frames.pdesc"))}

    # ---- A: the console's pace (and a state one boundary before each load)
    states_dir = os.path.join(out, "A", "states")
    saves = ["%s/%s.ps2st@%d" % (states_dir, l["state"], l["from"] - 1) for l in fx["loads"]]
    os.makedirs(states_dir)
    A = leg("A", [], saves=saves, stop=fx["stop_at"])
    report["legs"]["A"] = {"rc": A["rc"], "loads": A["loads"]}
    if A["rc"] != 0:
        reasons.append("A: the run failed (exit %d)" % A["rc"])
        return finish(2)
    ok = True
    want = {l["state"]: l for l in fx["loads"]}
    for l in A["loads"]:
        w = want.get(l["state"])
        if not w or l["from"] != w["from"] or l["to"] != w["to"]:
            reasons.append("A: %s @%d-%d, the fixture's @%s-%s" % (l["state"], l["from"], l["to"], w and w["from"], w and w["to"]))
            ok = False
    if len(A["loads"]) != len(fx["loads"]):
        reasons.append("A: %d loads, the fixture has %d" % (len(A["loads"]), len(fx["loads"])))
        return finish(1)
    # the loading picture as the console shows it: per load, the held picture (the most common descriptor in the load) and the
    # presents from the fade's start (the first present that differs from the load's first)
    held, fade = {}, {}
    for l in A["loads"]:
        keys = [k for k in sorted(A["pictures"]) if l["from"] <= k <= l["to"]]
        held[l["state"]] = Counter(A["pictures"][k][1] for k in keys).most_common(1)[0][0] if keys else None
        start = next((k for k in keys if A["pictures"][k] != A["pictures"][keys[0]]), None)
        fade[l["state"]] = [A["pictures"][k] for k in keys if start is not None and k >= start]
    level_a = level_after(A["lines"], "LoadingLevel", fx["probe_samples"])

    # ---- B: deterministic at the device's speed
    B = leg("B", ["--disc-speed", "device"])
    report["legs"]["B"] = {"rc": B["rc"], "loads": B["loads"], "census": [c["line"] for c in B["census"]]}
    if B["rc"] != 0:
        reasons.append("B: the run failed (exit %d)" % B["rc"])
        ok = False
    for l in B["loads"]:
        if not fx["fast_min"] <= l["boundaries"] <= fx["fast_max"]:
            reasons.append("B: %s @%d-%d is %d boundaries (fixture %d..%d)" % (l["state"], l["from"], l["to"], l["boundaries"], fx["fast_min"], fx["fast_max"]))
            ok = False
        if not any(c["state"] == l["state"] and c["from"] == l["from"] and c["to"] == l["to"] and c["speed"] == "device" for c in B["census"]):
            reasons.append("B: no census line at the device's speed for %s @%d-%d" % (l["state"], l["from"], l["to"]))
            ok = False
        keys = [k for k in sorted(B["pictures"]) if l["from"] <= k <= l["to"]]
        start = next((k for k in keys if B["pictures"][k] != B["pictures"][keys[0]]), None) if keys else None
        seq = [B["pictures"][k] for k in keys if start is not None and k >= start]
        n_held = sum(1 for p in seq if p[1] == held[l["state"]])
        a_seq = fade[l["state"]][:len(seq)]
        same = seq == a_seq
        l.update({"held_presents": n_held, "fade_presents": len(seq), "fade_is_the_console's": same})
        if not same:
            first = next((i for i, (x, y) in enumerate(zip(seq, a_seq)) if x != y), min(len(seq), len(a_seq)))
            reasons.append("B: %s: the pictures from the fade's start differ from A's at present %d of %d" % (l["state"], first, len(seq)))
            ok = False
        if n_held < fx["picture_min"]:
            reasons.append("B: %s: the loading picture held %d presents (fixture >= %d)" % (l["state"], n_held, fx["picture_min"]))
            ok = False
    if len(B["loads"]) != len(fx["loads"]):
        reasons.append("B: %d loads (fixture %d)" % (len(B["loads"]), len(fx["loads"])))
        ok = False
    level_b = level_after(B["lines"], "LoadingLevel", fx["probe_samples"])
    if not level_a or level_b != level_a:
        first = next((i for i, (x, y) in enumerate(zip(level_a, level_b)) if x != y), min(len(level_a), len(level_b)))
        reasons.append("B: the level after the hub's load differs from A's at probe line %d of %d (%s)" % (
            first, len(level_a), level_b[first][:120] if first < len(level_b) else "-"))
        ok = False
    report["legs"]["B"]["level_lines"] = len(level_b)

    # ---- C / Cv: play mode in a window; S: play mode headless with the harness
    for name, vsync, window in (("C", False, True), ("Cv", True, True), ("S", False, False)):
        L = leg(name, [], play=True, vsync=vsync, harness=not window, headless=not window)
        report["legs"][name] = {"rc": L["rc"], "loads": L["loads"], "census": [c["line"] for c in L["census"]], "clock": L["clock"]}
        if L["rc"] != 0:
            reasons.append("%s: the run failed (exit %d)" % (name, L["rc"]))
            ok = False
            continue
        for l in L["loads"]:
            n_held = sum(1 for k, p in L["pictures"].items() if l["from"] <= k <= l["to"] and p[1] == held[l["state"]])
            l["held_presents"] = n_held
            if (window and l["wall_s"] > fx["max_wall_s"]) or l["boundaries"] > fx["fast_max"]:
                reasons.append("%s: %s @%d-%d %d boundaries in %.3f s (fixture <= %d b, <= %.1f s)" % (
                    name, l["state"], l["from"], l["to"], l["boundaries"], l["wall_s"], fx["fast_max"], fx["max_wall_s"]))
                ok = False
            if not any(c["state"] == l["state"] and c["from"] == l["from"] and c["speed"] == "device" for c in L["census"]):
                reasons.append("%s: no census line at the device's speed for %s @%d" % (name, l["state"], l["from"]))
                ok = False
            if not window and n_held < fx["picture_min_play"]:
                reasons.append("%s: %s: the loading picture in %d presents (fixture >= %d)" % (name, l["state"], n_held, fx["picture_min_play"]))
                ok = False
        if len(L["loads"]) != len(fx["loads"]):
            reasons.append("%s: %d loads (fixture %d)" % (name, len(L["loads"]), len(fx["loads"])))
            ok = False
        c = L["clock"]
        if not c or c["parks"] + c["late"] + c["timeouts"] != c["boundaries"]:
            reasons.append("%s: not every boundary paced (%s)" % (name, c))
            ok = False
        if name == "C":
            level_c = level_after(L["lines"], "LoadingLevel", 1)
            if level_c != level_a[:len(level_c)] or not level_c:
                reasons.append("C: the level after the hub's load (play) differs from A's")
                ok = False
            report["legs"]["C"]["level_lines"] = len(level_c)

    # ---- D: the console's pace in play mode, from A's states
    for l in A["loads"]:
        name = "D-" + l["state"]
        d = os.path.join(out, name)
        os.makedirs(d)
        mc = os.path.join(d, "mc")
        shutil.copytree(hub, mc)
        env = dict(os.environ)
        env.pop("TE_GFX_HEADLESS", None)
        state = os.path.join(states_dir, "%s.ps2st" % l["state"])
        args = [binary, "--iso", a.iso, "--config", "none", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state", "--mc-dir", mc,
                "--load-state", state, "--stop-at", str(l["to"] + 5), "--disc-speed", "console", "--run-seconds", "120"]
        rc = run(args, os.path.join(d, "run.log"), env)
        got = [x for x in loads_of(lines_of(os.path.join(d, "run.log")), names) if x["state"] == l["state"]]
        report["legs"][name] = {"rc": rc, "loads": got}
        if rc != 0 or not got or got[0]["to"] != l["to"] or got[0]["wall_s"] < fx["paced_min_wall_s"]:
            reasons.append("%s: %s (A: @%d-%d; fixture >= %.1f s)" % (name, got or "no load", l["from"], l["to"], fx["paced_min_wall_s"]))
            ok = False

    if ok:
        b = {l["state"]: l for l in B["loads"]}
        c = {l["state"]: l for l in report["legs"]["C"]["loads"]}
        cv = {l["state"]: l for l in report["legs"]["Cv"]["loads"]}
        sp = {l["state"]: l for l in report["legs"]["S"]["loads"]}
        report["verdict"] = "; ".join("%s: console %d b, device %d b (picture held %d; play %d), play %.3f s / vsync %.3f s" % (
            s, want[s]["to"] - want[s]["from"], b[s]["boundaries"], b[s]["held_presents"], sp[s]["held_presents"], c[s]["wall_s"],
            cv[s]["wall_s"]) for s in names) + \
            "; the level after the load the console's (%d probe lines)" % len(level_a)
        return finish(0)
    return finish(1)


if __name__ == "__main__":
    sys.exit(main())
