#!/usr/bin/env python3
"""tools/judges/cutscene_skip_judge.py -- every scripted cutscene of the disc: skipped with CROSS it leaves the state watching it
leaves, or the cross button does nothing in it and it plays as watched.

The fixture (fixtures/judges/cutscene_skip_pal.json) lists the disc's 51 scripted cutscenes (tools/cutscenes/census.py's sites),
each with its status: "pass" (it may be skipped) or "refused" (it cannot be, `why` says why). The game's table of the cutscenes
the cross button may skip is made of the same fixture when the game is built (tools/cutscenes/skip_table.py). For each cutscene a
test reaches (its chunk, and how it starts: at the level's entry, TE_CUTSCENE_START's trigger, a pad log or a plan -- see
tools/cutscenes/verify.py), two deterministic headless legs: W watches it to its end, S presses CROSS two boundaries into it;
both then keep the same neutral inputs.
  pass     compare.py's verdict under THE RULE (the fixture's "rule" and the cutscene's "allow": the progress, the counters, the
           chunks' flags, the music, the player's chunk and mode and the judged instances' flags and behaviours EXACT; places
           within the floor -- what two watchings in play mode differ by -- or their class's bound; what runs on the world's time
           allowed per agent with its reason; everything allowed is printed), reached by +settle and held, S plays on, the game's
           skip line, and S back in Playing within `most_skip_boundaries` of CROSS and before W is;
  refused  the game's refusal line, no skip, and S's state lines exactly W's: the cutscene played as watched.
A cutscene no test reaches yet is listed (refused: not verified) and not run. --mechanism also runs each refused cutscene's skip
without the table (TE_CUTSCENE_SKIP_ALL=1, leg M) and says which of them the skip now leaves as watched (the fixture may let it
be skipped).

  tools/judges/cutscene_skip_judge.py --bin BIN --iso IMAGE --out DIR [--fixture FIXTURE] [--jobs N] [--only a,b] [--mechanism]
                                      [--keep]
GREEN when every cutscene a test reaches meets its status; RED names the ones that do not and why. The legs' logs are removed
once judged (--keep keeps them; a cutscene that fails keeps its own).
"""
import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import time
import types

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "cutscenes"))
import skip_table  # noqa: E402
import verify  # noqa: E402


def trace(path):
    """The state lines and the game state's changes of a leg, in order."""
    return [l for l in open(path, encoding="utf-8", errors="replace") if l.startswith("[cs.state] {") or "] game state @ boundary " in l]


def first_difference(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b)) if len(a) != len(b) else -1


def boundary_of(line):
    if line.startswith("[cs.state] {"):
        return json.loads(line[11:]).get("b")
    return int(line.split("boundary ")[1].split(":")[0])


def judge_pass(item, r, most):
    if r["status"] != "pass":
        why = r.get("why") or r.get("compare") or r["status"]
        if r["status"] == "refused":
            why = "the game refused the skip (the build's table is not this fixture's: rebuild)"
        return why
    s_play = r["skipped"][1]
    if s_play - r["pressed"] > most:
        return "the skip took %d boundaries from CROSS (most %d)" % (s_play - r["pressed"], most)
    if s_play >= r["watched"][1]:
        return "the skip did not shorten the cutscene (play again at %d, watched to %d)" % (s_play, r["watched"][1])
    if not any("skip over" in line for line in r.get("skip_log", [])):
        return "no skip line"
    return ""


def judge_refused(item, r):
    if r["status"] in ("not reached", "error"):
        return r.get("why", r["status"])
    if verify.skip_started(r.get("skip_log", [])):
        return "the cutscene was skipped (the build's table is not this fixture's: rebuild)"
    if not r.get("refused"):
        return "no refusal line"
    w, s = trace(r["logs"]["W"]), trace(r["logs"]["S"])
    at = first_difference(w, s)
    if at >= 0:
        line = w[at] if at < len(w) else s[at]
        return "the refused leg did not play as the watched one (first difference at boundary %s)" % boundary_of(line)
    if r["skipped"][1] != r["watched"][1]:
        return "play again at %s, watched to %s" % (r["skipped"][1], r["watched"][1])
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--fixture", default=os.path.join(ROOT, "fixtures", "judges", "cutscene_skip_pal.json"))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", default="")
    ap.add_argument("--mechanism", action="store_true")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    t0 = time.time()
    try:
        rows = {r["name"]: r for r in skip_table.load(a.fixture)}
    except SystemExit as e:
        print("cutscene_skip_judge: RED -- the fixture: %s" % e)
        return 1
    fixture = json.load(open(a.fixture))
    items = fixture["cutscenes"]
    if a.only:
        items = [i for i in items if i["name"] in a.only.split(",")]
    os.makedirs(a.out, exist_ok=True)
    hub = os.path.join(a.out, "mc_hub")
    try:
        verify.make_hub_card(os.path.abspath(a.bin), a.iso, hub, os.path.join(a.out, "hub.log"))
    except subprocess.CalledProcessError as e:
        print("cutscene_skip_judge: RED -- the hub card's run failed (exit %d)" % e.returncode)
        return 1
    args = types.SimpleNamespace(runner=os.path.abspath(a.bin), iso=a.iso, out=a.out, span=fixture.get("span", 300),
                                 every=fixture.get("every", 7))
    most = fixture.get("most_skip_boundaries", 240)
    reached = [i for i in items if i.get("chunk")]
    rule = fixture.get("rule", {})
    print("cutscene_skip_judge: the rule: places within the floor %g (%s)" % (rule.get("floor", 1e-3), rule.get("floor_why", "strict")))
    for c in rule.get("classes", []):
        print("cutscene_skip_judge:   %s (%s) within %g: %s" % (c["class"], ", ".join(c["names"]), c["within"], c["why"]))
    print("cutscene_skip_judge:   exact: the progress, the counters, the chunks' flags, the music, the player's chunk and mode, the "
          "judged instances' flags and behaviours; a cutscene's allowances are listed under it")

    def one(item):
        try:
            w = verify.watched(item, args, hub)
            if "status" in w:
                return w, None
            rule_path = verify.rule_file(item, fixture, w["out"])
            r = verify.skipped(w, args, hub, "S", skip_all=False, rule=rule_path)
            m = None
            if a.mechanism and item["status"] == "refused":
                m = verify.skipped(w, args, hub, "M", skip_all=True, rule=rule_path)
            return r, m
        except Exception as e:   # a leg that cannot run (a full disk, a missing fixture): this cutscene fails, the others go on
            return {"name": item["name"], "status": "error", "why": "%s: %s" % (type(e).__name__, e)}, None

    bad, passed, refused, could, results = [], 0, 0, [], []
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs))
    for item, (r, m) in zip(reached, pool.map(one, reached)):
        results.append((r, m))
        status = item["status"]
        why = judge_pass(item, r, most) if status == "pass" else judge_refused(item, r)
        w_watch, w_play = r.get("watched") or [0, 0]
        s_play = (r.get("skipped") or [0, 0])[1] or 0
        if status == "pass":
            what = "watched %s->%s (%s), CROSS at %s, play again at %s (%s)" % (w_watch, w_play, w_play - w_watch, r.get("pressed"),
                                                                              s_play, s_play - (r.get("pressed") or 0))
        else:
            what = "watched %s->%s (%s), CROSS at %s refused: played as watched" % (w_watch, w_play, w_play - w_watch, r.get("pressed"))
        if m is not None:
            what += "; the skip without the table: %s%s" % (m["status"], " (" + m["differs"] + ")" if m["status"] != "pass" and m.get("differs")
                                                          else "")
            if not judge_pass(item, m, most):
                could.append(item["name"])
        print("cutscene_skip_judge: %-28s %-7s %s%s" % (item["name"], "FAIL" if why else status, what if not why or r.get("watched") else "",
                                                      " -- " + why if why else ""), flush=True)
        for x in (r.get("allowed") or [])[:8] if status == "pass" else []:
            print("cutscene_skip_judge: %-28s   allowed: %s" % ("", x))
        if why:
            bad.append(item["name"])
        elif status == "pass":
            passed += 1
        else:
            refused += 1
        if not why and not a.keep:
            for log in list(r.get("logs", {}).values()) + list((m or {}).get("logs", {}).values()):
                if os.path.exists(log):
                    os.remove(log)
    pool.shutdown()
    unreached = [i for i in items if not i.get("chunk")]
    for item in unreached:
        print("cutscene_skip_judge: %-28s %-7s %s" % (item["name"], "not run", "(no test reaches it; refused: %s)" % rows[item["name"]]["why"]))
    with open(os.path.join(a.out, "results.json"), "w") as f:
        json.dump([{"item": i, "skip": r, "mechanism": m} for i, (r, m) in zip(reached, results)], f, indent=1)
    took = time.time() - t0
    summary = "%d cutscenes: %d skipped leaving the watched state, %d refused and played as watched, %d not reached by a test (refused); " \
              "%d run in %.0f s (%d jobs)" % (len(items), passed, refused, len(unreached), len(reached), took, a.jobs)
    if could:
        print("cutscene_skip_judge: the skip now leaves the watched state in %s: the fixture may let %s be skipped" % (
            ", ".join(could), "it" if len(could) == 1 else "them"))
    if bad:
        print("cutscene_skip_judge: RED -- %d of %d: %s -- %s" % (len(bad), len(reached), ", ".join(bad), summary))
        return 1
    print("cutscene_skip_judge: GREEN -- %s" % summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
