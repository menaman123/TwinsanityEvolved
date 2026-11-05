#!/usr/bin/env python3
"""level_select_pairing_judge.py -- a level select warp made from inside a level starts the new level as a warp from the menus
does: the pairing and the second character of the level left are not carried into it.

The report: warp to a level where Crash and Cortex are paired, then warp to another level, and Cortex comes along everywhere --
no double jump, no slide. The mechanism: GameController::WarpToChunk takes the save controller's summary from the game played
(saveController.Take), and wrote the pairing, the character and the second only for a row naming a character. Every row but
one names none, so the summary kept the pairing of the level left (Cavern Catastrophe's Rollerbrawl: pairing 3, Cortex the
second) and StartingPlay put the Rollerbrawl's vehicle on the new level's player (SetPlayerVehicle, gamecontroller.cpp).

For each case of fixtures/judges/level_select_pairing_pal.json two deterministic headless runs with the cutscene census's state
lines (TE_CUTSCENE_STATE, runner/te_cutscenes.cpp: the play word, the player's place and chunk): the REFERENCE (the title's level
select straight to row B: a menu warp, whose summary is a new game's) and the WARP (the title's level select to row A, play there
until its pairing is not alone, then the pause menu's level select to row B). GREEN for a case iff
  - the WARP run's state line at the pause (the last one before the second row is chosen) shows level A's pairing not alone and
    a second character named (the case reproduces the report's precondition),
  - both runs choose their rows, reach Playing in B's chunk, exit 0, no trap, no GameOver,
  - at every state line from B's start (the last StartingPlay) to +span, the WARP run's pairing, character and second equal the
    REFERENCE's at the same offset, and the player's place is within --tolerance of the REFERENCE's.
GREEN overall iff every case is. The unfixed warp is RED on both cases: the hub starts with pairing 3 + Cortex and Crash in the
Rollerbrawl's vehicle (pulled 15 units from the start), the rooftop with Nina in it and the Cortex of Cavern Catastrophe at her
place; he dies there, the restart keeps the pairing (the start checkpoint was set with it) and the deaths repeat.

Usage: level_select_pairing_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F] [--tolerance 0.05]
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
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "level_select_pairing_pal.json")
CHARACTERS = {0: "crash", 1: "cortex", 2: "tallcrash", 3: "nina", 4: "none", 5: "mecha", 6: "-"}
GAME_OVER = 18


def run(binary, iso, out, padlog, stop_at, every, tag):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    mc = os.path.join(d, "mc")
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--config", "none",
            "--trace-state", "--trace-player", "0", "--play-pad", os.path.join(ROOT, padlog), "--stop-at", str(stop_at),
            "--mc-dir", mc, "--run-seconds", "600"]
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1", TE_CUTSCENE_STATE=str(every))
    print("level_select_pairing_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=ROOT)
    return rc, log


def read(log):
    """(the state lines, the rows chosen with the boundary of the first state line after each, traps, the log's text)"""
    states, chosen = [], []
    text = open(log, errors="replace").read()
    for line in text.splitlines():
        if line.startswith("[cs.state] {"):
            j = json.loads(line[11:])
            w = j.get("play", 0)
            p = j.get("player", {})
            states.append({"b": j["b"], "gs": j["gs"], "pairing": (w >> 4) & 15, "character": (w >> 8) & 15, "second": (w >> 12) & 15,
                           "p": p.get("p"), "chunk": p.get("chunk", "")})
        m = re.search(r"mods: level_select row (\d+) chosen -> action 1 warp to '([^']*)'", line)
        if m:
            chosen.append((int(m.group(1)), m.group(2), states[-1]["b"] if states else 0))
    traps = len([l for l in text.splitlines() if re.search(r"wasm trap|trap:|Invalid call_indirect|SIGBUS|SIGSEGV", l)
                 and "0 traps" not in l])
    return states, chosen, traps, text


def from_start(states, span):
    """the state lines of the last start of play (the last StartingPlay line) to +span, as {offset: line}"""
    starts = [i for i, s in enumerate(states) if s["gs"] == 11]
    if not starts:
        return {}
    b0 = states[starts[-1]]["b"]
    return {s["b"] - b0: s for s in states[starts[-1]:] if s["b"] - b0 <= span}


def describe(s):
    p = s["p"] or [0, 0, 0]
    return "pairing %d %s+%s at (%.2f %.2f %.2f) %s" % (s["pairing"], CHARACTERS.get(s["character"], "?"), CHARACTERS.get(s["second"], "?"),
                                                        p[0], p[1], p[2], s["chunk"].split("\\")[-1])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "level_select_pairing_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--tolerance", type=float, default=0.05)
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    os.makedirs(a.out, exist_ok=True)
    every, span = fx["state_every"], fx["span"]
    green = True
    for case in fx["cases"]:
        name = case["name"]
        rc_r, log_r = run(a.bin, a.iso, a.out, case["reference_padlog"], case["reference_stop_at"], every, name + "_reference")
        rc_w, log_w = run(a.bin, a.iso, a.out, case["warp_padlog"], case["warp_stop_at"], every, name + "_warp")
        if rc_r not in (0,) or rc_w not in (0,):
            print("level_select_pairing_judge: %s: a run failed (exit %d / %d)" % (name, rc_r, rc_w))
            return 2
        st_r, ch_r, traps_r, text_r = read(log_r)
        st_w, ch_w, traps_w, text_w = read(log_w)
        to = case["chunk_to"]
        chose_r = [c for c in ch_r if c[0] == case["row_to"] and c[1] == to]
        chose_w = [c[0] for c in ch_w] == [case["row_from"], case["row_to"]]
        # the precondition: the pairing of level A when the second row is chosen
        before = [s for s in st_w if ch_w and len(ch_w) > 1 and s["b"] <= ch_w[1][2] and s["gs"] in (12, 15)]
        pre = before[-1] if before else None
        paired = pre is not None and pre["pairing"] != 1 and pre["second"] != 6
        over = any(s["gs"] == GAME_OVER for s in st_r + st_w) or "GameOver" in text_r or "GameOver" in text_w
        ref, war = from_start(st_r, span), from_start(st_w, span)
        reached = bool(ref) and bool(war) and all(s["chunk"] == to for s in list(ref.values())[1:] + list(war.values())[1:])
        playing = any(s["gs"] == 12 for s in ref.values()) and any(s["gs"] == 12 for s in war.values())
        same, first_diff = True, None
        for off in sorted(ref):
            r, w = ref[off], war.get(off)
            if w is None:
                same, first_diff = False, "+%d: no state line in the warp run" % off
                break
            fields = (r["pairing"], r["character"], r["second"]) == (w["pairing"], w["character"], w["second"])
            place = r["p"] is not None and w["p"] is not None and max(abs(x - y) for x, y in zip(r["p"], w["p"])) <= a.tolerance
            if not (fields and place):
                same, first_diff = False, "+%d: warp %s vs reference %s" % (off, describe(w), describe(r))
                break
        ok = bool(chose_r) and chose_w and paired and reached and playing and same and traps_r == 0 and traps_w == 0 and not over
        print("level_select_pairing_judge: %s: level A at the pause %s (paired %s); rows chosen %s / reference %s; B's start in %s: "
              "reached %s, Playing %s, %d state lines to +%d compared -> %s; traps %d/%d, GameOver %s -> %s"
              % (name, describe(pre) if pre else "-", paired, [c[0] for c in ch_w], [c[0] for c in ch_r], to.split("\\")[-1], reached,
                 playing, len(ref), span, "the same as the menu warp" if same else "DIFFERENT (%s)" % first_diff, traps_r, traps_w,
                 over, "OK" if ok else "FAIL"))
        if war:
            first = war[min(o for o in war if o > 0)] if any(o > 0 for o in war) else war[0]
            print("level_select_pairing_judge: %s: B's first frame of play after the warp: %s; the menu warp's: %s"
                  % (name, describe(first), describe(ref[min(o for o in ref if o > 0)]) if any(o > 0 for o in ref) else "-"))
        green = green and ok
    if green:
        print("level_select_pairing_judge: GREEN -- a warp from inside a level starts the new level with the pairing, the characters "
              "and the place a warp from the menus gives it: nothing of the level left rides along")
        return 0
    print("level_select_pairing_judge: RED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
