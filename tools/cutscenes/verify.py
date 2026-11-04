#!/usr/bin/env python3
"""tools/cutscenes/verify.py -- the watch-vs-skip verification of in-game cutscenes, one leg pair per cutscene.

For each cutscene of a list (JSON: name, the chunk the hub card is warped to, the entry pad log (the LOAD GAME presses) or an
autopilot plan that walks to the cutscene's trigger): leg W plays it to its end, leg S presses CROSS two boundaries into the
Watching stretch that leads to the cutscene's end (the first boundary of Watching does not read the pad; a notice that pauses the
game in between -- the auto-save's -- takes the presses before it); both legs then keep the same neutral inputs. Both run the
deterministic headless runner with TE_CUTSCENE_STATE (runner/te_cutscenes.cpp) and are compared by tools/cutscenes/compare.py
(the progress, the counters, the chunks' flags, the player, the settled instances; the state trace from Playing on). Leg S
measures the skip itself: TE_CUTSCENE_SKIP_ALL=1, the table of the cutscenes the game lets the cross button skip not consulted
(platform/nativeee/host/te_cutscene_skip.h); --with-table runs the game as it ships (a refused cutscene: "refused").

  tools/cutscenes/verify.py --iso IMAGE --list LIST.json [--runner build/twinsanityevolved] [--out DIR] [--jobs 3] [--only a,b]
                            [--with-table]
  LIST.json: a list of entries, or the judge's fixture (fixtures/judges/cutscene_skip_pal.json: its cutscenes with a chunk).
  An entry: {"name": "...", "chunk": "levels\\\\earth\\\\cavern\\\\cavbridg", "padlog": "fixtures/padlogs/entries/....padlog"
             | "plan": "PLAN", "character": "crash", "stop": 4600, "span": 300, "story": N, "open": N, "area": N, "second": "cortex",
             "pairing": N (the card's story state: tools/warp_save.py), "nth": N (the cutscene is the run's Nth Watching stretch),
             "start": "act_DIRECTOR[:MESSAGE]@BOUNDARY"   (TE_CUTSCENE_START: the director given its trigger's behaviour),
             "test": "SPEC@BOUNDARY;..."}   (TE_TEST_ACTIONS: what the reach's recording asked of the game, tools/cutscenes/reach.py)
A plan's last step must leave the cutscene to run: verify.py appends `until-gstate 13 N` and then (S) `hold CROSS 3`, then
`until-gstate 12 N` and `wait SPAN` (W likewise without the press), and stops the run SPAN boundaries after play starts again.
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
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import padlog_press  # noqa: E402


def run(runner, iso, mc, args, log, every, start="", skip_all=False, test=""):
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1", TE_CUTSCENE_STATE=str(every))
    if start:
        env["TE_CUTSCENE_START"] = start
    # the test requests the reach's recording asked (platform/nativeee/host/te_test_requests.h), at their boundaries
    if test:
        env["TE_TEST_ACTIONS"] = test
    else:
        env.pop("TE_TEST_ACTIONS", None)
    # the skip without the table of the cutscenes it may skip (platform/nativeee/host/te_cutscene_skip.h): the mechanism measured
    if skip_all:
        env["TE_CUTSCENE_SKIP_ALL"] = "1"
    else:
        env.pop("TE_CUTSCENE_SKIP_ALL", None)
    with open(log, "w") as f:
        return subprocess.run([runner, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--config", "none",
                               "--trace-state", "--mc-dir", mc] + args + ["--run-seconds", "900"], stdout=f, stderr=subprocess.STDOUT,
                              env=env).returncode


def timeline(log):
    t = []
    for line in open(log, encoding="utf-8", errors="replace"):
        m = re.search(r"game state @ boundary (\d+): (\w+) \((\d+)\)", line)
        if m:
            t.append((int(m.group(1)), int(m.group(3))))
    return t


def card(out, hub, chunk, character, tag, item=None):
    mc = os.path.join(out, "mc_" + tag)
    shutil.rmtree(mc, ignore_errors=True)
    # the story's state a save made at the cutscene carries (tools/warp_save.py --story / --open / --area: the save's options; the
    # summary's second character and pairing)
    areas = []
    for key in ("story", "open", "area", "second", "pairing"):
        if item is not None and item.get(key) is not None:
            areas += ["--" + key, str(item[key])]
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "warp_save.py"), "--card", hub, "--chunk", chunk, "--out", mc, "--place",
                    "0xFFFF"] + (["--character", character] if character else []) + areas, check=True, stdout=subprocess.DEVNULL)
    return mc


def watching_stretches(t, after=0, chain=30):
    """The first boundaries of the timeline's Watching stretches after AFTER (a cutscene in parts -- Watching, a movie, a few
    boundaries of Playing, Watching again -- is one stretch, as tools/cutscenes/compare.py's after_cutscene takes it)."""
    out = []
    i = 0
    while i < len(t):
        b, s = t[i]
        if s != 13 or b <= after:
            i += 1
            continue
        out.append(b)
        # past this stretch: the first Playing that is not followed by Watching within CHAIN boundaries
        k = i + 1
        while k < len(t):
            if t[k][1] == 12:
                nxt = next(((bb, ss) for bb, ss in t[k + 1:] if ss != 12), None)
                if nxt is None or nxt[1] != 13 or nxt[0] - t[k][0] > chain:
                    break
            k += 1
        i = k + 1
    return out


def leg_args(entry, out, tag, skip_at=None, plan=None, span=300):
    if plan is not None:
        text = open(plan).read().rstrip("\n") + "\n"
        text += "until-gstate 13 20000\n" + ("hold CROSS 3\n" if skip_at else "") + "until-gstate 12 20000\nwait %d\nstop\n" % (span + 30)
        path = os.path.join(out, tag + ".plan")
        open(path, "w").write(text)
        return ["--autopilot", path]
    if skip_at is not None:
        path = os.path.join(out, tag + ".padlog")
        open(path, "w").write("\n".join(padlog_press.press(open(entry).readlines(), skip_at, "4000", 3)) + "\n")
        return ["--play-pad", path]
    return ["--play-pad", entry]


def watched(item, a, hub):
    """Leg W: the cutscene watched to its end, then SPAN boundaries of play; what the skipped legs need (the press boundary), or a
    result with status "not reached"."""
    name = item["name"]
    out = os.path.join(a.out, name)
    os.makedirs(out, exist_ok=True)
    span = item.get("span", a.span)
    stop = item.get("stop", 4800)
    plan = os.path.join(ROOT, item["plan"]) if item.get("plan") else None
    entry = os.path.join(ROOT, item.get("padlog", "fixtures/padlogs/entries/school_sch_hub_sch_hub.padlog"))
    character = item.get("character", "crash")
    w_log = os.path.join(out, "W.log")
    mc = card(out, hub, item["chunk"], character, "w", item)
    start = item.get("start", "")
    test = item.get("test", "")
    rc = run(a.runner, a.iso, mc, leg_args(entry, out, "W", None, plan, span) + ["--stop-at", str(stop)], w_log, a.every, start, test=test)
    t = timeline(w_log)
    # with a test start, the cutscene is the Watching that follows the boundary the start was given at (an earlier one is another's)
    given = [int(m.group(1)) for m in (re.match(r"\[cs\.start\] b (\d+): .* given starter", l) for l in open(w_log, errors="replace")) if m]
    # the cutscene is the run's first Watching stretch (after a test start's boundary), or its "nth" one (a route through an earlier
    # cutscene: the reach watches that one in both legs)
    stretches = watching_stretches(t, given[0] if given else 0)
    nth = item.get("nth", 1)
    if len(stretches) < nth:
        return {"name": name, "status": "not reached", "why": "%d Watching stretches in the watched leg, the cutscene is #%d (exit %d)" %
                (len(stretches), nth, rc)}
    w0 = stretches[nth - 1]
    if nth > 1:
        given = [w0 - 1]
    ends = [b for b, s in t if b > w0 and s == 12]
    if not ends:
        return {"name": name, "status": "not reached", "why": "the watched leg does not get back to Playing by %d (Watching @%d)" % (stop, w0)}
    # the Watching stretch that leads to the end: the last change to Watching before it
    press_at = max(b for b, s in t if s == 13 and b < ends[0]) + 2
    if not plan and ends[0] + span + 20 > stop:
        stop = ends[0] + span + 40
        rc = run(a.runner, a.iso, card(out, hub, item["chunk"], character, "w", item), leg_args(entry, out, "W", None, plan, span) +
                 ["--stop-at", str(stop)], w_log, a.every, start, test=test)
    shutil.rmtree(os.path.join(out, "mc_w"), ignore_errors=True)
    return {"name": name, "out": out, "item": item, "span": span, "stop": stop, "plan": plan, "entry": entry,
            "character": character, "start": start, "test": test, "w_log": w_log, "w_exit": rc, "watched": [w0, ends[0]], "pressed": press_at,
            "after": given[0] if given else 0}


def skip_started(lines):
    """Whether the game's lines say a skip started (game/src/game/cutsceneskip.cpp StartCutsceneSkip's)."""
    return any(re.match(r"\[te\.cutscene\] skipped: \d+ agents", l) for l in lines)


def differences(text, most=3):
    """compare.py's failing items of its last sample, short: the instance (its name's last part) and what differs (a place by its
    distance)."""
    block = []
    for line in text.splitlines():
        if re.match(r"\s+\+\d+: ", line):
            block = []
        elif line.startswith("      ! "):
            block.append(line[8:])
    out = []
    for line in block[:most]:
        who, _, what = line.partition(": ")
        m = re.match(r"(?:watched )?\[([-\d., ]+)\],? (?:vs|skipped) \[([-\d., ]+)\]", what) or \
            re.match(r"place \[([-\d., ]+)\] vs \[([-\d., ]+)\]", what)
        distance = ""
        if m:
            p, q = ([float(x) for x in g.split(",")] for g in m.groups())
            distance = " %.2f" % sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5
        if who.startswith("flags of "):
            out.append("flags of " + who.split("\\")[-1])
            continue
        if who.startswith(("player", "counts", "play", "counters")):
            out.append(who + distance)
            continue
        parts = who.split(" ", 1)
        name = parts[-1].split("|")[-1] if len(parts) > 1 else who
        out.append("%s %s%s" % (name, what.split(" ")[0], distance))
    return "; ".join(out)


def rule_file(item, fixture, out):
    """THE RULE for this cutscene (tools/cutscenes/compare.py --rule): the fixture's "rule" (the floor, the classes) and the
    cutscene's own "allow" list; written beside its legs. None without a fixture rule (the strict comparison)."""
    if not fixture or "rule" not in fixture:
        return None
    rule = dict(fixture["rule"])
    rule["allow"] = item.get("allow", [])
    path = os.path.join(out, "rule.json")
    with open(path, "w") as f:
        json.dump(rule, f, indent=1)
    return path


def allowed_of(path):
    """What compare.py's rule allowed (its --json report's "allowed"): one short line each, the instance by its name's last part."""
    try:
        report = json.load(open(path))
    except (OSError, ValueError):
        return []
    groups = {}
    for u in report.get("allowed", []):
        parts = u["who"].split(" ")
        name = parts[1].split("|")[-1] if len(parts) > 2 else u["who"]   # "chunk NAME #n" -> NAME; "player"
        g = groups.setdefault((name, u["why"]), {"who": set(), "kinds": set(), "max": 0.0, "after": None, "settles": u.get("settles")})
        g["who"].add(u["who"])
        g["kinds"].add(u["kind"])
        g["max"] = max(g["max"], u.get("max") or 0.0)
        if "after" in u:
            g["after"] = max(g["after"] or 0.0, u["after"])
    return ["%s%s %s%s: %s" % (name, (" x%d" % len(g["who"])) if len(g["who"]) > 1 else "", "/".join(sorted(g["kinds"])),
                              (" from +%d up to %.4f" % (g["settles"], g["after"])) if g["after"] is not None else
                              (" up to %.4f" % g["max"]) if g["max"] else "", why) for (name, why), g in sorted(groups.items())]


def skipped(w, a, hub, tag="S", skip_all=True, rule=None):
    """Leg `tag`: CROSS pressed at the watched leg's press boundary, the same inputs after; compared with the watched leg under
    RULE (a rule file, rule_file's). With skip_all the table of the cutscenes the skip may skip is not consulted (the mechanism);
    without, the game as it ships."""
    out, span, stop, after = w["out"], w["span"], w["stop"], w["after"]
    s_log = os.path.join(out, tag + ".log")
    mc = card(out, hub, w["item"]["chunk"], w["character"], tag.lower(), w["item"])
    rc_s = run(a.runner, a.iso, mc, leg_args(w["entry"], out, tag, w["pressed"], w["plan"], span) + ["--stop-at", str(stop)], s_log, a.every,
               w["start"], skip_all, w.get("test", ""))
    shutil.rmtree(mc, ignore_errors=True)
    cmp = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "cutscenes", "compare.py"), w["w_log"], s_log, "--span", str(span),
                          "--json", os.path.join(out, "compare_%s.json" % tag), "--after", str(after)] + (["--rule", rule] if rule else []),
                         capture_output=True, text=True)
    open(os.path.join(out, "compare_%s.txt" % tag), "w").write(cmp.stdout)
    skip_line = [l.strip() for l in open(s_log, encoding="utf-8", errors="replace") if l.startswith("[te.cutscene]")]
    st = timeline(s_log)
    s_watch = [b for b, s in st if s == 13 and b > after]
    s_end = [b for b, s in st if s_watch and b > s_watch[0] and s == 12]
    status = {0: "pass", 1: "differs", 2: "skipped leg stuck"}.get(cmp.returncode, "error")
    refused = any(l.startswith("[te.cutscene] not skipped:") for l in skip_line)
    if not skip_started(skip_line):
        status = "refused" if refused else "not skipped"
    traps = sum(1 for l in open(s_log, encoding="utf-8", errors="replace") if "wasm trap" in l)
    return {"name": w["name"], "status": status if not traps else "trap", "watched": w["watched"], "skipped": [s_watch[0] if s_watch else None,
            s_end[0] if s_end else None], "pressed": w["pressed"], "skip_log": skip_line, "refused": refused,
            "compare": cmp.stdout.strip().splitlines()[-1] if cmp.stdout else "", "differs": differences(cmp.stdout),
            "allowed": allowed_of(os.path.join(out, "compare_%s.json" % tag)),
            "exit": [w["w_exit"], rc_s],
            "logs": {"W": w["w_log"], tag: s_log}}


def verify(item, a, hub, skip_all=True, fixture=None):
    w = watched(item, a, hub)
    return w if "status" in w else skipped(w, a, hub, "S", skip_all, rule_file(item, fixture, w["out"]))


def make_hub_card(runner, iso, hub, log):
    """The hub card every warp derives from: the hub_entry pad log's new game (its auto-save in slot 1)."""
    with open(log, "w") as f:
        subprocess.run([runner, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--config", "none",
                        "--play-pad", os.path.join(ROOT, "fixtures", "padlogs", "hub_entry.padlog"), "--mc-dir", hub, "--stop-at", "2700"],
                       stdout=f, stderr=subprocess.STDOUT, env=dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1"), check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--list", required=True)
    ap.add_argument("--runner", default=os.path.join(ROOT, "build", "twinsanityevolved"))
    ap.add_argument("--hub-card", default="")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "cutscene_verify"))
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--span", type=int, default=300)
    ap.add_argument("--every", type=int, default=7)
    ap.add_argument("--only", default="")
    ap.add_argument("--with-table", action="store_true", help="the skipped legs consult the table of the cutscenes the skip may skip")
    a = ap.parse_args()
    a.runner = os.path.abspath(a.runner)
    os.makedirs(a.out, exist_ok=True)
    hub = a.hub_card or os.path.join(a.out, "mc_hub")
    if not os.path.isdir(hub):
        make_hub_card(a.runner, a.iso, hub, os.path.join(a.out, "hub.log"))
    items = json.load(open(a.list))
    fixture = items if isinstance(items, dict) else None
    if isinstance(items, dict):
        items = [i for i in items["cutscenes"] if i.get("chunk")]
    if a.only:
        items = [i for i in items if i["name"] in a.only.split(",")]
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for r in pool.map(lambda i: verify(i, a, hub, not a.with_table, fixture), items):
            results.append(r)
            print("verify: %-28s %-12s watched %s skipped %s %s %s" % (r["name"], r["status"], r.get("watched"), r.get("skipped"),
                                                                    r.get("why", ""), " | ".join(r.get("skip_log", []))), flush=True)
    with open(os.path.join(a.out, "results.json"), "w") as f:
        json.dump(results, f, indent=1)
    return 0 if all(r["status"] == "pass" for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
