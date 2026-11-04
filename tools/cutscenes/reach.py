#!/usr/bin/env python3
"""tools/cutscenes/reach.py -- the cutscenes the game reaches by playing: each one's reach recorded as a pad log fixture.

A cutscene of the story plays when the save is at its point of the story (the save's areas: tools/warp_save.py --story / --open /
--area) and the player walks into what starts it (a first-entry trigger volume that tells the director its message, an object
that sends it, a boss's defeat). The list (fixtures/padlogs/cutscenes/cutscenes.json) names, for each cutscene: its site (the
director graph), the chunk and the save's state the hub card is warped to, and the autopilot steps (runner/te_autopilot.cpp)
that walk from the chunk's start to the trigger. `record` runs LOAD GAME and those steps closed-loop with --record-pad and keeps
the pad samples up to the first boundary of the cutscene (the Watching state), neutral after it: the recording is the fixture
(fixtures/padlogs/cutscenes/<name>.padlog, the card recipe in its header), replayed by tools/cutscenes/verify.py's legs W (watch
it) and S (CROSS two boundaries into it) and by the cutscene skip judge. A step the autopilot asks of the game for a test (teleport,
message, counter: platform/nativeee/host/te_test_requests.h -- a boss fight's hits, a route the autopilot cannot walk) is not an
input: the recording logs each one with its boundary, and `record` writes them as the entry's "test" (TE_TEST_ACTIONS, which the
legs set) and into the pad log's header.

  tools/cutscenes/reach.py record --iso IMAGE [--runner build/twinsanityevolved] [--only a,b] [--out DIR] [--jobs 3]
  tools/cutscenes/verify.py --iso IMAGE --list fixtures/padlogs/cutscenes/cutscenes.json   (the W/S pairs of the recorded ones)

A list entry: {"name": "11_5385", "site": 5385, "director": "...", "chunk": "levels\\\\ice\\\\hub\\\\psycho", "story": 8,
"character": "crash", "steps": ["wait 30", "goto 44 0.4 1.0 400"], "stop": 6800, "how": "what starts it, in a sentence"}.
Inputs only (the fixtures' pad log format); no game data.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIST = os.path.join(ROOT, "fixtures", "padlogs", "cutscenes", "cutscenes.json")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify  # noqa: E402

# LOAD GAME on the warped card, anchored on the game's states (the title 7, the main menu 8, play 12): the entry pad logs' presses;
# then the auto-save notice the load shows (Paused 15) dismissed with CROSS, as the movement steps do, before the reach's steps
LOAD_GAME = """until-gstate 7 3000
idle 11
hold START 5
until-gstate 8 300
idle 78
hold DOWN 3
idle 47
hold CROSS 5
idle 395
hold DOWN 3
idle 97
hold CROSS 3
until-gstate 12 3000
until-gstate 15 90
idle 28
hold CROSS 3
until-gstate 12 600
"""


def card_args(item):
    out = ["--place", "0xFFFF"]
    if item.get("character"):
        out += ["--character", item["character"]]
    for key in ("story", "open", "area", "second", "pairing"):
        if item.get(key) is not None:
            out += ["--" + key, str(item[key])]
    return out


def header(item, test=""):
    recipe = "tools/warp_save.py --card <hub card> --chunk '%s' %s" % (item["chunk"], " ".join(card_args(item)))
    lines = ["# cutscene reach fixture (tools/cutscenes/reach.py): site %d %s" % (item["site"], item.get("director", "")),
             "# card: " + recipe, "# chunk=" + item["chunk"]]
    for key in ("story", "open", "area", "character", "second", "pairing"):
        if item.get(key) is not None:
            lines.append("# %s=%s" % (key, item[key]))
    if item.get("start"):
        lines.append("# start=" + item["start"] + "  (TE_CUTSCENE_START: the director given its trigger behaviour; the legs set it)")
    if test:
        lines.append("# test=" + test + "  (TE_TEST_ACTIONS: what the recording's plan asked of the game; the legs set it)")
    lines.append("# reach: " + item.get("how", ""))
    return lines


def record(item, a, hub):
    name = item["name"]
    out = os.path.join(a.out, name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    mc = os.path.join(out, "mc")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "warp_save.py"), "--card", hub, "--chunk", item["chunk"], "--out", mc] +
                   card_args(item), check=True, stdout=subprocess.DEVNULL)
    plan = os.path.join(out, "plan")
    with open(plan, "w") as f:
        f.write(LOAD_GAME + "\n".join(item["steps"]) + ("\n" if item["steps"] else "") + "until-gstate 13 %d\nstop\n" % item.get("reach_frames", 3000))
    rec = os.path.join(out, "rec.padlog")
    log = os.path.join(out, "run.log")
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1")
    env.pop("TE_TEST_ACTIONS", None)   # the plan's own steps ask them
    if item.get("start"):
        # a cutscene whose trigger the reach cannot walk to: its director given its trigger behaviour at the boundary (the
        # runner's TE_CUTSCENE_START, as verify.py's legs give it), the story state the save's
        env["TE_CUTSCENE_START"] = item["start"]
    with open(log, "w") as f:
        rc = subprocess.run([a.runner, "--iso", a.iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--config", "none",
                             "--trace-state", "--autopilot", plan, "--record-pad", rec, "--mc-dir", mc, "--stop-at", str(item.get("record_stop", 9000)),
                             "--run-seconds", "900"], stdout=f, stderr=subprocess.STDOUT, env=env).returncode
    states = [(int(m.group(1)), int(m.group(2))) for m in (re.search(r"game state @ boundary (\d+): \w+ \((\d+)\) <-", l)
                                                           for l in open(log, errors="replace")) if m]
    nth = item.get("nth", 1)
    watch = verify.watching_stretches(states)[nth - 1:]
    if rc != 0 or not watch or not os.path.isfile(rec):
        return {"name": name, "ok": False, "why": "exit %d, %s" % (rc, "Watching @%d" % watch[0] if watch else "no Watching #%d" % nth)}
    # the test requests the plan asked before the cutscene, as TE_TEST_ACTIONS (SPEC@BOUNDARY;...): the legs replay them there
    asked = [(int(m.group(1)), m.group(2).strip()) for m in (re.match(r"\[te\.test\] b (\d+): (.+)$", l) for l in open(log, errors="replace")) if m]
    test = ";".join("%s@%d" % (spec, b) for b, spec in asked if b <= watch[0])
    # the samples up to the cutscene's first boundary, then the neutral pad (the legs' inputs after the trigger are the same)
    kept = []
    for line in open(rec):
        if line.startswith("#"):
            if not line.startswith("# end"):
                kept.append(line.rstrip("\n"))
            continue
        if int(line.split()[0]) <= watch[0]:
            kept.append(line.rstrip("\n"))
    kept.append("%d 0000 128 128 128 128  # NONE" % (watch[0] + 1))
    kept.append("# end at boundary %d" % item.get("stop", 7200))
    fixture = os.path.join(ROOT, "fixtures", "padlogs", "cutscenes", name + ".padlog")
    with open(fixture, "w") as f:
        f.write("\n".join(header(item, test) + kept) + "\n")
    shutil.rmtree(mc, ignore_errors=True)
    return {"name": name, "ok": True, "watching": watch[0], "fixture": os.path.relpath(fixture, ROOT), "test": test}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["record"])
    ap.add_argument("--iso", required=True)
    ap.add_argument("--runner", default=os.path.join(ROOT, "build", "twinsanityevolved"))
    ap.add_argument("--list", default=LIST)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "cutscene_reach"))
    ap.add_argument("--hub-card", default="")
    ap.add_argument("--only", default="")
    ap.add_argument("--jobs", type=int, default=3)
    a = ap.parse_args()
    a.runner = os.path.abspath(a.runner)
    os.makedirs(a.out, exist_ok=True)
    hub = a.hub_card or os.path.join(a.out, "mc_hub")
    if not os.path.isdir(hub):
        verify.make_hub_card(a.runner, a.iso, hub, os.path.join(a.out, "hub.log"))
    items = [i for i in json.load(open(a.list)) if i.get("steps") is not None]
    if a.only:
        items = [i for i in items if i["name"] in a.only.split(",")]
    status = 0
    tests = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for r in pool.map(lambda i: record(i, a, hub), items):
            print("reach: %-10s %s" % (r["name"], ("recorded %s (Watching @%d)%s" % (r["fixture"], r["watching"],
                                                                                      " test " + r["test"] if r["test"] else "")) if r["ok"] else
                                       "NOT REACHED: " + r["why"]), flush=True)
            status |= 0 if r["ok"] else 1
            if r["ok"]:
                tests[r["name"]] = r["test"]
    # the recorded test requests into the list (the entry's "test", gone when the plan asks none)
    entries = json.load(open(a.list))
    changed = False
    for e in entries:
        if e["name"] in tests and e.get("test", "") != tests[e["name"]]:
            if tests[e["name"]]:
                e["test"] = tests[e["name"]]
            else:
                e.pop("test", None)
            changed = True
    if changed:
        with open(a.list, "w") as f:
            f.write(json.dumps(entries, indent=1))
    return status


if __name__ == "__main__":
    sys.exit(main())
