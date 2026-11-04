#!/usr/bin/env python3
"""tools/cutscenes/compare.py -- the state a skipped cutscene leaves against the state the watched one leaves.

Two run logs with TE_CUTSCENE_STATE=N (runner/te_cutscenes.cpp: "[cs.state] {...}" lines -- the game state, the progress, the
player's place / chunk / state word, the scripts' counters, each loaded chunk's saved and unsaved flags and its object
instances' places, flags and running behaviours): W watched the cutscene to its end, S pressed CROSS in it; both then play the
same inputs. The legs are lined up at the first Playing boundary after the cutscene (each leg's own: the skip ends sooner) and
compared there and every N boundaries after it, up to --span boundaries:

  - the progress (counts, play state, the levels' gems / crystals), the scripts' counters, the chunks' saved / unsaved flags,
    the music: equal;
  - the player: the chunk and his mode equal, the place within the floor (THE RULE below);
  - every instance (matched by chunk, object name and their order among that name's instances): the place within the
    tolerance, the flags and the behaviours running (graph and state per level). The CUTSCENE'S AGENTS (the game's own list:
    the "[te.cutscene] skipped, N agents: ..." line -- its director, the agents it ran behaviours on and messaged) that are
    SETTLED in the watched leg fail any difference; an agent that keeps changing when watched (a swinging log's cycle, a monitor's
    patrol it goes back to) is reported. For the others OF THE CHUNKS THE AGENTS ARE IN an instance SETTLED in the watched leg
    (the same
    at every sampled point of the span: what the cutscene left it as) fails when the skipped leg settles on another value (the
    last third of its samples alike and not the watched value); one that changes in either leg (the world's own time-driven agents: a bobbing pickup, a
    lift's cycle, a 0.1 s polling loop -- a skip cannot give them the cutscene's seconds, and a sampling stride can alias a loop
    into looking settled in one leg) is reported, not failed. --agents NAMES (comma, substrings) fails any difference of the
    named instances.
  THE VERDICT. The END STATE decides: from the first point after the cutscene (+0) to +SETTLE (120 boundaries, two seconds) the
  skipped leg must reach the watched state, and hold it for HOLD (60) boundaries from there: a pickup flying to the player, an
  actor's last frame, and what the watched cutscene's agents do a second after its end (a boss fight's start, a story area
  raised) -- which the skip, having no seconds of its own, does at its end -- may land that much apart. After that the skipped leg must PLAY ON (back to Playing, no trap, no stuck Watching, no GameOver in the
  span); a later difference (an enemy that fought during the watched cutscene's last seconds attacking a little earlier, an
  unseen agent updated on another frame) is reported with its offset, not failed -- a skip cannot give the world the
  cutscene's seconds. Instances of a chunk loaded in one leg only (streamed in during the watched seconds) are reported.

THE RULE (--rule FILE, JSON; the cutscene skip judge writes it from its fixture: the fixture's "rule" and the cutscene's
"allow"). What must be equal, what may be a little off and by how much, what runs on the world's time -- printed, never hidden:
  - EXACT: the progress, the counters, the chunks' saved / unsaved flags, the player's chunk and mode (the character's state
    word's low 16 bits: its modes, what it stands on, dead; game/agents.h CharacterState -- the high 16 are unused and keep
    whatever was in memory), the music (each slot's playing and prepared track) and, for the instances judged, their flags (less
    the renderer's 0x600) and running behaviours.
  - PLACES within the FLOOR ("floor": two watchings of one cutscene in play mode end the player that far apart: real play's own
    frame times); a CLASS of instances by name ("classes": [{"class", "names", "within", "why"}]: a pickup's bob, ...) within its
    own bound. A place within its bound is reported with its distance ("~"), not failed.
  - ALLOWED ("allow": [{"agent": NAME, "what": [place|flags|behaviours|presence|tracks], "why": REASON, "within": X, "settles": N,
    "from": N, "chunk": PATH}]): an instance whose name contains NAME (any name with "*"; only in a chunk whose path contains
    PATH with "chunk") may differ in WHAT -- what runs on the world's time and is not the cutscene's outcome (a missile in
    flight, a crate's hop, an idle's random pick, the next area streaming in) -- reported with the reason at every point and
    in the summary; "within" bounds its place; "settles": the skipped leg must come to the watched value by +N (checked over the
    span; with "within", any place before +N and within X from +N: a fall that ends after the skip); "from": the allowance
    applies from +N on (the end state itself judged by the floor). NAME "player" is the player's place; "music:SLOT" a music
    slot's tracks (a cutscene's speech track, which a skip does not play, ending after the watched cutscene).
  Without --rule: the floor is --tolerance (1e-3), no classes, nothing allowed (the strict comparison).

Exit 0 = the skipped state is the watched state (by the rules above), 1 = it differs, 2 = a leg did not get back to Playing.
  tools/cutscenes/compare.py W.log S.log [--span 300] [--tolerance 1e-3] [--rule RULE.json] [--agents A,B] [--json OUT]
"""
import argparse
import collections
import json
import sys


def states(path):
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("[cs.state] {"):
            out.append(json.loads(line[11:]))
    return out


def timeline(path):
    t = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if "] game state @ boundary " in line:
            b = int(line.split("boundary ")[1].split(":")[0])
            name = line.split(": ")[1].split(" (")[0]
            t.append((b, name))
    return t


def after_cutscene(lines, after=0, chain=30):
    """The lines from the first Playing after the first cutscene (its first Watching line after boundary AFTER) on, keyed by the
    boundaries since that Playing line (a later cutscene of the run, or a death's restart, ends the span: the lines up to it).
    A cutscene in parts (a movie between them: Watching, Movie, a few boundaries of Playing, Watching again) is one cutscene:
    a Playing stretch that gives way to Watching within CHAIN boundaries is not its end."""
    first_watch = next((i for i, l in enumerate(lines) if l["gs"] == 13 and l["b"] > after), None)
    if first_watch is None:
        return None, {}
    i = first_watch
    while True:
        start = next((k for k in range(i + 1, len(lines)) if lines[k]["gs"] == 12), None)
        if start is None:
            return None, {}
        again = next((k for k in range(start + 1, len(lines)) if lines[k]["gs"] != 12), None)
        if again is not None and lines[again]["gs"] == 13 and lines[again]["b"] - lines[start]["b"] <= chain:
            i = again
            continue
        break
    b0 = lines[start]["b"]
    out = {}
    for l in lines[start:]:
        if l["gs"] != 12:
            break
        out[l["b"] - b0] = l
    return b0, out


def instances(line):
    """Every instance by chunk, object name and its agent's id (its persistent flag slot: what the chunk's layout gave it); the
    instances of a name without one by their order of place (the list's order changes as a chunk's instances wake)."""
    out = {}
    for c in line.get("chunks", []):
        groups = collections.defaultdict(list)
        for i in c["i"]:
            groups[i["n"]].append(i)
        for name, items in groups.items():
            plain = []
            for i in items:
                a = i.get("a", -1)
                if a not in (-1, 0xFFFF) and (c["c"], name, "a%d" % a) not in out:
                    out[(c["c"], name, "a%d" % a)] = i
                else:
                    plain.append(i)
            for n, i in enumerate(sorted(plain, key=lambda x: (round(x["p"][0], 1), round(x["p"][2], 1), round(x["p"][1], 1)))):
                out[(c["c"], name, n)] = i
    return out


# ReferencedObjectFlags the renderer sets every frame from what the camera draws (inDrawnCell, visible): not the cutscene's
RENDER_FLAGS = 0x200 | 0x400


def far(a, b, tol):
    return max(abs(x - y) for x, y in zip(a, b)) > tol


def distance(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


# game/agents.h CharacterState: previous mode, mode, standing, ground found, no fall events, dead, box only -- the low 16 bits;
# the high 16 are unused (they keep what the memory held: another value on another run of the same watching)
MODE_BITS = 0xFFFF
KINDS = ("place", "flags", "behaviours", "presence")


class Rule:
    """THE RULE: the floor for places, the classes' own bounds, the cutscene's allowances (see the module's description)."""

    def __init__(self, data=None, tolerance=1e-3):
        data = data or {}
        self.floor = float(data.get("floor", tolerance))
        self.floor_why = data.get("floor_why", "")
        self.classes = data.get("classes", [])
        self.allow = data.get("allow", [])
        for a in self.allow:
            bad = [k for k in a.get("what", []) if k not in KINDS + ("tracks",)]
            if bad or not a.get("agent") or not a.get("why"):
                raise SystemExit("compare: an allowance needs an agent, a reason and kinds among %s: %s" % (", ".join(KINDS), a))

    def bound(self, name):
        """The bound of an instance's place: its class's, else the floor; and the bound's name."""
        for c in self.classes:
            if any(n in name for n in c["names"]):
                return float(c["within"]), c["class"]
        return self.floor, "the floor"

    def allowance(self, name, kind, off=None, chunk=""):
        """The allowance for this kind of difference of the instance named NAME of CHUNK (or "player", "music:SLOT") at +OFF. An
        allowance's "chunk" limits it to the instances of a chunk whose path contains it; its agent "*" is any of them."""
        for a in self.allow:
            agent = a["agent"]
            if agent in ("player",) or agent.startswith("music:"):
                matches = agent == name
            else:
                matches = (agent == "*" or agent in name) and a.get("chunk", "") in chunk
            if matches and kind in a.get("what", []) and (off is None or off >= a.get("from", 0)):
                return a
        return None

    def describe(self):
        out = ["the rule: places within the floor %g%s" % (self.floor, (" (" + self.floor_why + ")") if self.floor_why else "")]
        for c in self.classes:
            out.append("  %s (%s) within %g: %s" % (c["class"], ", ".join(c["names"]), c["within"], c["why"]))
        for a in self.allow:
            out.append("  allowed: %s -- %s%s%s%s: %s" % (a["agent"], "/".join(a["what"]), (" within %g" % a["within"]) if "within" in a else "",
                                                       (", settles by +%d" % a["settles"]) if "settles" in a else "",
                                                       (", from +%d" % a["from"]) if "from" in a else "", a["why"]))
        out.append("  exact: the progress, the counters, the chunks' flags, the music, the player's chunk and mode, the judged "
                   "instances' flags and behaviours")
        return out


def same(a, b, tol):
    return (a is not None and b is not None and not far(a["p"], b["p"], tol) and (a["f"] & ~RENDER_FLAGS) == (b["f"] & ~RENDER_FLAGS)
            and a["r"] == b["r"])


def settled(series, tol):
    return all(x is not None for x in series) and all(same(series[0], x, tol) for x in series[1:])


def unseen(series):
    """Not seen over the whole span (its seen stamp never changes): the game updates it seldom, its loops frozen where they were"""
    stamps = {x.get("seen") for x in series if x is not None}
    return len(stamps) == 1 and None not in stamps


def compare_line(w, s, rule, agents, steady, w_steady, off=0):
    """One sampled point (OFF boundaries after play): the failing items, the notes (what is not judged), the allowed items (who,
    kind, the allowance or the bound, the place's distance). An allowance that settles by +N takes any difference before +N."""
    fails, notes, allowed = [], [], []
    unsettled_places = []   # a place an allowance lets settle by +N, still beyond its bound at or after +N
    for key in ("counts", "play", "levels", "counters"):
        if w.get(key) != s.get(key):
            fails.append("%s: watched %s, skipped %s" % (key, w.get(key), s.get(key)))
    if "music" in w and "music" in s and w["music"] != s["music"]:
        for slot, (x, y) in enumerate(zip(w["music"], s["music"])):
            if x == y:
                continue
            allow = rule.allowance("music:%d" % slot, "tracks", off)
            text = "music slot %d (playing, prepared track): watched %s, skipped %s" % (slot, x, y)
            if allow:
                allowed.append(("music:%d" % slot, "tracks", allow, text, None))
            else:
                fails.append(text)
    wf = {c["c"]: (c["saved"], c["unsaved"]) for c in w.get("chunks", [])}
    sf = {c["c"]: (c["saved"], c["unsaved"]) for c in s.get("chunks", [])}
    for c in sorted(set(wf) & set(sf)):
        if wf[c] != sf[c]:
            fails.append("flags of %s: watched %s, skipped %s" % (c, wf[c], sf[c]))
    if set(wf) != set(sf):
        notes.append("loaded chunks: only watched %s, only skipped %s" % (sorted(set(wf) - set(sf)), sorted(set(sf) - set(wf))))
    if w.get("camera") and s.get("camera") and far(w["camera"], s["camera"], 0.05):
        notes.append("camera: watched %s, skipped %s" % (w["camera"], s["camera"]))
    wp, sp = w.get("player"), s.get("player")
    if (wp is None) != (sp is None):
        fails.append("player: watched %s, skipped %s" % (wp, sp))
    elif wp:
        if wp["chunk"] != sp["chunk"]:
            fails.append("player chunk: watched %s, skipped %s" % (wp["chunk"], sp["chunk"]))
        d = distance(wp["p"], sp["p"])
        allow = rule.allowance("player", "place", off)
        if allow and d > rule.floor and d <= allow.get("within", float("inf")):
            allowed.append(("player", "place", allow, "place %.4f" % d, d))
        elif d > rule.floor:
            fails.append("player place: watched %s, skipped %s" % (wp["p"], sp["p"]))
        elif d > 0:
            allowed.append(("player", "place", "the floor", "place %.4f" % d, d))
        if (wp["state"] & MODE_BITS) != (sp["state"] & MODE_BITS):
            fails.append("player mode (state word's low 16 bits): watched %#x, skipped %#x" % (wp["state"], sp["state"]))
    wi, si = instances(w), instances(s)
    both = set(wf) & set(sf)
    for k in sorted(set(wi) | set(si), key=str):
        a, b = wi.get(k), si.get(k)
        if k[0] not in both:
            continue
        own = (k[1] in agents and k in w_steady) or k in steady
        who = "%s %s #%s" % (k[0].split("\\")[-1], k[1], k[2])
        what, ok = [], []
        if a is None or b is None:
            item = ("presence", "only in the %s leg" % ("skipped" if a is None else "watched"))
            (ok if rule.allowance(k[1], "presence", off, k[0]) else what).append(item)
        else:
            d = distance(a["p"], b["p"])
            if far(a["p"], b["p"], 1e-3):
                allow = rule.allowance(k[1], "place", off, k[0])
                bound, bound_name = (float(allow["within"]), "its allowance") if allow and "within" in allow else rule.bound(k[1])
                item = ("place", "place %s vs %s (%.4f)" % (a["p"], b["p"], d))
                if allow and ("within" not in allow or off < allow.get("settles", 0)):
                    ok.append(item + (allow, d))
                elif d <= bound:
                    ok.append(item + (allow or bound_name, d))
                else:
                    what.append(item)
                    if allow and "settles" in allow and own:
                        unsettled_places.append({"who": who, "kind": "place", "offset": off})
            if (a["f"] & ~RENDER_FLAGS) != (b["f"] & ~RENDER_FLAGS):
                item = ("flags", "flags %#x vs %#x" % (a["f"], b["f"]))
                (ok if rule.allowance(k[1], "flags", off, k[0]) else what).append(item)
            if a["r"] != b["r"]:
                item = ("behaviours", "behaviours %s vs %s" % (a["r"], b["r"]))
                (ok if rule.allowance(k[1], "behaviours", off, k[0]) else what).append(item)
        if what:
            (fails if own else notes).append("%s: %s" % (who, "; ".join(x[1] for x in what)))
        if own:
            for x in ok:
                kind = x[0]
                allow = x[2] if len(x) > 2 else rule.allowance(k[1], kind, off, k[0])
                allowed.append((who, kind, allow, x[1], x[3] if len(x) > 3 else None))
        elif ok:
            notes.append("%s: %s" % (who, "; ".join(x[1] for x in ok)))
    return fails, notes, allowed, unsettled_places


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("watched")
    ap.add_argument("skipped")
    ap.add_argument("--span", type=int, default=300)
    ap.add_argument("--tolerance", type=float, default=1e-3)
    ap.add_argument("--agents", default="")
    ap.add_argument("--settle", type=int, default=120)
    ap.add_argument("--hold", type=int, default=60)
    ap.add_argument("--json", default="")
    ap.add_argument("--after", type=int, default=0, help="the cutscene is the first Watching after this boundary (a test start's)")
    ap.add_argument("--rule", default="", help="THE RULE as JSON (the floor, the classes, the allowances); none: the strict comparison")
    a = ap.parse_args()
    rule = Rule(json.load(open(a.rule)) if a.rule else None, a.tolerance)
    agents = [x for x in a.agents.split(",") if x]
    report = {"watched_playing": None, "skipped_playing": None, "points": [], "rule": rule.describe()}
    for line in open(a.skipped, encoding="utf-8", errors="replace"):
        if line.startswith("[te.cutscene] skipped, ") and " agents:" in line:
            agents += [n for n in line.split(" agents:", 1)[1].split() if n != "?"]
            break
    report["agents"] = agents
    wl, sl = states(a.watched), states(a.skipped)
    wb, wt = after_cutscene(wl, a.after)
    sb, st = after_cutscene(sl, a.after)
    tw, ts = timeline(a.watched), timeline(a.skipped)
    report["watched_playing"], report["skipped_playing"] = wb, sb

    if wb is None or sb is None:
        print("compare: a leg does not get back to Playing after its cutscene (watched %s, skipped %s)" % (wb, sb))
        return 2
    w_watch = [b for b, n in tw if n == "Watching" and b > a.after]
    s_watch = [b for b, n in ts if n == "Watching" and b > a.after]
    print("compare: watched Watching @%s -> Playing @%d (%d boundaries); skipped Watching @%s -> Playing @%d (%d boundaries)" %
          (w_watch[0] if w_watch else "?", wb, wb - (w_watch[0] if w_watch else wb), s_watch[0] if s_watch else "?", sb,
           sb - (s_watch[0] if s_watch else sb)))
    for line in rule.describe():
        print("  " + line)
    status = 0
    offsets = [off for off in sorted(set(wt) & set(st)) if off <= a.span]
    keys = set()
    for off in offsets:
        keys |= set(instances(wt[off]))
    # the chunks the cutscene's agents are in (the watched leg's first point): the cutscene's own place
    agent_chunks = {k[0] for k in instances(wt[offsets[0]]) if k[1] in agents} if offsets else set()
    report["agent_chunks"] = sorted(agent_chunks)
    series = {k: [instances(wt[off]).get(k) for off in offsets] for k in keys}
    skipped_series = {k: [instances(st[off]).get(k) for off in offsets] for k in keys}
    steady = set()
    w_steady = set()
    w_settled = 0
    for k, v in series.items():
        if not settled(v, a.tolerance):
            continue
        w_settled += 1
        w_steady.add(k)
        if agent_chunks and k[0] not in agent_chunks:
            continue
        sv = skipped_series[k]
        if k[1] not in agents and unseen(v) and unseen(sv):
            # off screen in both legs: what it shows is the phase its loop was frozen at, not the cutscene's
            continue
        tail = sv[len(sv) - max(1, len(sv) // 3):]
        # the skipped leg settles on another value: a difference the cutscene's skip left; a skipped leg that keeps changing is a
        # time-driven agent (a loop the watched samples alias into one value)
        if settled(tail, a.tolerance) and not same(v[-1], tail[-1], a.tolerance):
            steady.add(k)
    report["settled_instances"] = w_settled
    report["settled_differing"] = len(steady)
    report["changing_instances"] = len(keys) - w_settled
    print("  instances: %d settled in the watched leg over +0..+%d (%d of them settled otherwise in the skipped leg), %d changing "
          "(time-driven: reported, not failed)" % (w_settled, offsets[-1] if offsets else 0, len(steady), len(keys) - w_settled))
    settle, hold = a.settle, a.hold
    reached = None
    later = None
    used = {}            # (who, kind, allowance) -> what was allowed: the allowance or bound, the largest distance, the offsets
    unsettled = []       # allowed to settle by +N and still different at or after +N
    early = []           # an allowance from +N whose item already differed before +N: the end state itself differs
    late_allowances = [x for x in rule.allow if "from" in x]
    for off in offsets:
        fails, notes, allowed, unsettled_places = compare_line(wt[off], st[off], rule, agents, steady, w_steady, off)
        unsettled += unsettled_places
        for x in late_allowances:
            if off >= x["from"]:
                continue
            for f in fails:
                who = f.split(":", 1)[0]
                if (x["agent"] == "player" and who == "player place") or (x["agent"] != "player" and x["agent"] in who):
                    early.append({"who": who, "offset": off, "from": x["from"]})
        kept = []
        for who, kind, allow, text, d in allowed:
            if isinstance(allow, dict) and "settles" in allow and off >= allow["settles"] and not (kind == "place" and "within" in allow):
                fails.append("%s: %s still differs at +%d (allowed to settle by +%d: %s)" % (who, kind, off, allow["settles"], allow["why"]))
                unsettled.append({"who": who, "kind": kind, "offset": off})
                continue
            kept.append((who, kind, allow, text, d))
            u = used.setdefault((who, kind, allow["why"] if isinstance(allow, dict) else str(allow)),
                                {"who": who, "kind": kind, "max": 0.0, "first": off, "last": off,
                                              "why": allow["why"] if isinstance(allow, dict) else
                                              "within %s %g" % (allow, rule.floor if allow == "the floor" else rule.bound(who.split(" ", 1)[-1])[0]),
                                              "settles": allow.get("settles") if isinstance(allow, dict) else None})
            u["last"] = off
            if d is not None:
                u["max"] = max(u["max"], d)
                if u["settles"] and off >= u["settles"]:
                    u["after"] = max(u.get("after", 0.0), d)   # once settled: the bound applies
        report["points"].append({"offset": off, "fails": fails, "notes": notes,
                                 "allowed": ["%s: %s" % (x[0], x[3]) for x in kept]})
        print("  +%d: %s%s%s" % (off, "EQUAL" if not fails else "DIFFERS (%d)" % len(fails),
                                (", %d allowed by the rule" % len(kept)) if kept else "", (", %d other differences" % len(notes)) if notes else ""))
        for f in fails[:12]:
            print("      ! " + f)
        for x in kept[:6]:
            print("      ~ %s: %s" % (x[0], x[3]))
        for n in notes[:4]:
            print("      . " + n)
        if not fails and reached is None and off <= settle:
            reached = off
        if fails and reached is not None and reached >= 0 and off <= reached + hold:
            reached = None if off <= settle else -1
        if fails and reached is not None and reached >= 0 and off > reached + hold and later is None:
            later = {"offset": off, "what": fails[:4]}
    # an allowance that settles by +N must be checked to +N: the span has to reach it
    short = sorted({u["settles"] for u in used.values() if u["settles"] and offsets and offsets[-1] < u["settles"]})
    end_state = reached is not None and reached >= 0 and not unsettled and not short and not early
    report["end_state"] = {"equal": end_state, "reached_at": reached}
    report["later_difference"] = later
    report["allowed"] = sorted(used.values(), key=lambda u: (u["who"], u["kind"]))
    report["unsettled"] = unsettled
    print("  END STATE: %s" % (("equal from +%d, held %d boundaries" % (reached, hold)) if end_state else "DIFFERS (not reached by +%d and held %d boundaries)" % (settle, hold)))
    if used:
        print("  allowed by the rule (%d):" % len(used))
        for u in report["allowed"]:
            short_who = " ".join(w.split("|")[-1] for w in u["who"].split(" "))
            print("    %s %s: %s%s+%d..+%d -- %s" % (short_who, u["kind"], ("up to %.4f, " % u["max"]) if u["max"] else "",
                                             ("from +%d up to %.4f, " % (u["settles"], u["after"])) if "after" in u else "", u["first"], u["last"],
                                             u["why"]))
    if unsettled:
        print("  NOT SETTLED: %s" % "; ".join("%s %s at +%d" % (u["who"], u["kind"], u["offset"]) for u in unsettled[:6]))
    if early:
        print("  THE END DIFFERS: %s (allowed only from +%d)" % ("; ".join("%s at +%d" % (e["who"], e["offset"]) for e in early[:4]), early[0]["from"]))
    report["early"] = early
    if short:
        print("  NOT CHECKED: the span ends at +%d, before the allowance's +%d" % (offsets[-1], short[0]))
    if later:
        print("  later: first difference at +%d (reported, not failed): %s" % (later["offset"], "; ".join(later["what"])))
    status = 0 if end_state else 1
    # the trace after the cutscene, over the span: the same states in the same order (boundaries relative to each leg's Playing)
    wtr = [(b - wb, n) for b, n in tw if wb <= b <= wb + a.span]
    s_tr = [(b - sb, n) for b, n in ts if sb <= b <= sb + a.span]
    report["trace_equal"] = wtr == s_tr
    print("  state trace over +0..+%d: %s" % (a.span, "equal" if report["trace_equal"] else "differs: watched %s, skipped %s" % (wtr[:8], s_tr[:8])))
    # plays on: no GameOver within the span, and the skipped leg's states over it the watched leg's (a death both legs come to at
    # the same offset, the cutscene that follows it), or the skipped leg back in play at the span's end
    s_states = [n for b, n in ts if sb <= b <= sb + a.span]
    plays_on = "GameOver" not in s_states and (report["trace_equal"] or not s_states or s_states[-1] in ("Playing", "Paused", "Waiting"))
    traps = sum(1 for line in open(a.skipped, encoding="utf-8", errors="replace") if "wasm trap" in line)
    report["plays_on"] = plays_on and traps == 0
    print("  plays on: %s" % ("yes" if report["plays_on"] else "NO (states %s, traps %d)" % (s_states[-4:], traps)))
    if not report["plays_on"]:
        status = 1
    report["status"] = ["equal", "differs", "no playing"][status]
    if a.json:
        with open(a.json, "w") as f:
            json.dump(report, f, indent=1)
    print("compare: %s" % report["status"].upper())
    return status


if __name__ == "__main__":
    sys.exit(main())
