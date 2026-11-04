#!/usr/bin/env python3
"""tools/cutscenes/census.py -- the census of the game's in-game cutscenes, from the game's own parsed scripts.

The runner's TE_CUTSCENE_CENSUS=1 (runner/te_cutscenes.cpp) logs every behaviour graph, starter and loaded chunk's instances the
game has read ([cs.graph] / [cs.starter] / [cs.chunk] JSON lines: ids, token types, counts and names, never the bytes). This
tool runs that over every chunk of the disc's archive (each one warped to with tools/warp_save.py, as tools/ci/level_entries.sh
does) or reads logs already made, names the commands by the decompilation's factory (a command keeps no id once read: its
vtable's retail address is mapped through game/src/game/commands.cpp BuildScriptCommand, game/include/game/commands.h's RETAIL
names and game/symbol_addrs.txt), and writes the census:

  (A) the cutscene FILES the scripts queue (QueueObjectVideo's / QueueVideo's sites) and
  (B) every CutsceneStart site: the chunk(s) whose instances run it, the graph, whether its graph (or a graph its start hands
      over to) asks the "cutscene skipped" condition (0x23C: a skip branch exists) and what that branch runs up to
      CutsceneEnd, the commands and conditions of the path from the start to its end by type, its stated waits (seconds).

  tools/cutscenes/census.py run --iso IMAGE [--runner build/twinsanityevolved] [--out DIR] [--jobs N] [--chunks a,b]
  tools/cutscenes/census.py report --logs DIR [--json OUT] [--markdown OUT]
"""
import argparse
import collections
import concurrent.futures
import glob
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "discs"))

CUTSCENE_START, CUTSCENE_END = 589, 590
SKIPPED_CONDITION = 0x23C


# ---- the decompilation's token tables -------------------------------------------------------------------------------------
def symbol_addresses():
    table = {}
    for line in open(os.path.join(ROOT, "game", "symbol_addrs.txt"), encoding="utf-8", errors="replace"):
        m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*0x([0-9A-Fa-f]+)", line)
        if m:
            table[m.group(1)] = int(m.group(2), 16)
    return table


def command_tables():
    """{vtable retail address: (class, [ids])} and {id: class} from the factory switch (BuildScriptCommand)."""
    src = open(os.path.join(ROOT, "game", "src", "game", "commands.cpp"), encoding="utf-8").read()
    body = src[src.index("void* BuildScriptCommand("):]
    body = body[:body.index("\n}\n")]
    ids_of = collections.defaultdict(list)
    pending = []
    for line in body.splitlines():
        for m in re.finditer(r"case (0x[0-9A-Fa-f]+|\d+):", line):
            pending.append(int(m.group(1), 0))
        m = re.search(r"NewScriptObject<([A-Za-z0-9_]+)>", line)
        if m and pending:
            for i in pending:
                ids_of[m.group(1)].append(i)
            pending = []
    retail = {}
    for header in glob.glob(os.path.join(ROOT, "game", "include", "game", "*.h")):
        for m in re.finditer(r"g_([A-Za-z0-9_]+)VTable\[\]\s*RETAIL\(([A-Za-z0-9_]+)\)", open(header, encoding="utf-8").read()):
            retail[m.group(1)] = m.group(2)
    symbols = symbol_addresses()
    by_vtable, by_id = {}, {}
    for cls, ids in ids_of.items():
        for i in ids:
            by_id[i] = cls
        sym = retail.get(cls)
        if sym and sym in symbols:
            by_vtable[symbols[sym]] = (cls, sorted(ids))
    return by_vtable, by_id


def condition_tables():
    src = open(os.path.join(ROOT, "game", "src", "game", "conditions.cpp"), encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"NewScriptObject<([A-Za-z0-9_]+)>\(\), (0x[0-9A-Fa-f]+|\d+)", src):
        out[int(m.group(2), 0)] = m.group(1)
    return out


# ---- the runs -------------------------------------------------------------------------------------------------------------
def archive_chunks(iso):
    """Every level chunk of the disc's archive index (names only), as a save names a chunk ("levels\\earth\\hub\\beach")."""
    import manifest  # tools/discs/manifest.py: the disc reader
    import archive_index
    img = manifest.Image(iso)
    files = manifest.describe(img, iso)[0]
    _, entries, _ = archive_index.read_index(img, files)
    out = []
    for name, _, _ in entries:
        if name.lower().endswith(".rm2") and name.lower().startswith("levels\\"):
            out.append(name[:-4].lower())
    return out


def run_chunk(runner, iso, card, chunk, out_dir, stop_at):
    name = chunk.replace("\\", "_")
    mc = os.path.join(out_dir, "mc_" + name)
    log = os.path.join(out_dir, name + ".log")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "warp_save.py"), "--card", card, "--chunk", chunk, "--out", mc,
                    "--place", "0xFFFF", "--character", "crash"], check=True, stdout=subprocess.DEVNULL)
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1", TE_CUTSCENE_CENSUS="1")
    padlog = os.path.join(ROOT, "fixtures", "padlogs", "entries", "school_sch_hub_sch_hub.padlog")
    with open(log, "w") as f:
        rc = subprocess.run([runner, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--config", "none",
                             "--trace-state", "--trace-player", "0", "--play-pad", padlog, "--mc-dir", mc, "--stop-at", str(stop_at),
                             "--run-seconds", "900"], stdout=f, stderr=subprocess.STDOUT, env=env).returncode
    shutil.rmtree(mc, ignore_errors=True)
    return chunk, rc, log


def cmd_run(args):
    runner = os.path.abspath(args.runner)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    card = os.path.join(out, "mc_hub")
    if not os.path.isdir(card):
        env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1", TE_CUTSCENE_CENSUS="1")
        with open(os.path.join(out, "hub.log"), "w") as f:
            subprocess.run([runner, "--iso", args.iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--config", "none",
                            "--trace-state", "--play-pad", os.path.join(ROOT, "fixtures", "padlogs", "hub_entry.padlog"), "--mc-dir", card,
                            "--stop-at", "4200"], stdout=f, stderr=subprocess.STDOUT, env=env, check=True)
    chunks = args.chunks.split(",") if args.chunks else archive_chunks(args.iso)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_chunk, runner, args.iso, card, c, out, args.stop_at) for c in chunks]
        for f in concurrent.futures.as_completed(futures):
            chunk, rc, log = f.result()
            print("census: %s exit %d (%s)" % (chunk, rc, os.path.basename(log)), flush=True)


# ---- the report -----------------------------------------------------------------------------------------------------------
def read_logs(paths):
    graphs, starters, chunks = {}, {}, collections.defaultdict(dict)
    variants = collections.defaultdict(set)
    for path in paths:
        run = os.path.basename(path)[:-4]
        for line in open(path, encoding="utf-8", errors="replace"):
            if line.startswith("[cs.graph] "):
                g = json.loads(line[11:])
                key = json.dumps(g["s"], sort_keys=True)
                variants[g["g"]].add(key)
                graphs.setdefault(g["g"], g)
            elif line.startswith("[cs.starter] "):
                s = json.loads(line[13:])
                starters.setdefault(s["st"], s)
            elif line.startswith("[cs.chunk] "):
                c = json.loads(line[11:])
                if c["loaded"]:
                    chunks[c["chunk"].lower()][run] = c
    return graphs, starters, chunks, variants


FLAG_COMPLETION, FLAG_INTERRUPTING, FLAG_CHILD_SLOT = 1 << 10, 1 << 11, 1 << 12


def children(g):
    """The graph ids a graph's states run as child behaviours (by id; a slot of the object's behaviours is named 'slot N')."""
    out = []
    for s in g["s"]:
        if s["child"] != 0xFFFF:
            out.append(("slot", s["child"]) if s["f"] & FLAG_CHILD_SLOT else ("graph", s["child"]))
    return out


def closure(gid, graphs, seen=None):
    """A graph and every graph its states run as children (by id), depth first."""
    seen = set() if seen is None else seen
    if gid in seen or gid not in graphs:
        return seen
    seen.add(gid)
    for kind, c in children(graphs[gid]):
        if kind == "graph":
            closure(c, graphs, seen)
    return seen


def reachable_states(g):
    """The states a graph can be in: its start and every body's jump from a state it can be in."""
    if g["start"] < 0:
        return set()
    seen, todo = set(), [g["start"]]
    while todo:
        i = todo.pop()
        if i in seen or i >= len(g["s"]):
            continue
        seen.add(i)
        for b in g["s"][i]["b"]:
            if b["j"] >= 0:
                todo.append(b["j"])
    return seen


def commands_of(g, by_vtable, states=None):
    out = collections.Counter()
    for i, s in enumerate(g["s"]):
        if states is not None and i not in states:
            continue
        for b in s["b"]:
            for x in b["x"]:
                out[by_vtable.get(x, ("vt%06x" % x, []))[0]] += 1
    return out


def report(graphs, starters, chunks, by_vtable, conditions):
    names = {k: v["name"] for k, v in graphs.items()}
    vt_of = {cls: vt for vt, (cls, _) in by_vtable.items()}
    cs_vt, ce_vt = vt_of["CutsceneStartCommand"], vt_of["CutsceneEndCommand"]
    # kind A: a cutscene FILE (CutScene\\_<n>_<part>.cts) queued and started (VideoController's tracks); the music-only video
    # (QueueVideo / StartQueuedVideo) is a scripted cutscene's sound track
    file_vts = {vt_of[c] for c in ("QueueObjectVideoCommand", "StartObjectVideoCommand") if c in vt_of}
    track_vts = {vt_of[c] for c in ("QueueVideoCommand", "StartQueuedVideoCommand") if c in vt_of}

    def direct(g, vt):
        return any(vt in b["x"] for i in reachable_states(g) for b in g["s"][i]["b"])

    # graphs that run CutsceneStart themselves, or through a child they run
    starts = {gid for gid, g in graphs.items() if direct(g, cs_vt)}
    runs_start = {}
    for gid in graphs:
        cl = closure(gid, graphs)
        runs_start[gid] = bool(cl & starts)
    child_of = collections.defaultdict(set)
    for gid, g in graphs.items():
        for i in reachable_states(g):
            s = g["s"][i]
            if s["child"] != 0xFFFF and not s["f"] & FLAG_CHILD_SLOT:
                child_of[s["child"]].add(gid)
    # a SITE: a graph that runs CutsceneStart and is not a child of another graph that does (the helpers' parents are the sites)
    sites = sorted(gid for gid in graphs if runs_start[gid] and not any(runs_start.get(p) for p in child_of.get(gid, ())))
    # who runs which graph: starters' assigners, objects' behaviour slots and trigger behaviours, the chunks' instances
    graph_starters = collections.defaultdict(set)
    for sid, st in starters.items():
        for _, _, gid in st["a"]:
            if gid >= 0:
                graph_starters[gid].add(sid)
    starter_users = collections.defaultdict(set)   # starter -> {(chunk, object name)}
    for chunk, runs in chunks.items():
        for c in runs.values():
            for inst in c["i"]:
                for sid in inst["beh"] + [t[1] for t in inst["trig"]]:
                    if sid != 0xFFFF:
                        starter_users[sid].add((chunk, inst["n"]))
    rows = []
    for gid in sites:
        g = graphs[gid]
        cl = closure(gid, graphs)
        skip = []
        for member in sorted(cl):
            mg = graphs[member]
            live = reachable_states(mg)
            for i, s in enumerate(mg["s"]):
                for b in s["b"]:
                    if b.get("c") == SKIPPED_CONDITION:
                        target = b["j"]
                        branch = []
                        if target >= 0:
                            seen, todo = set(), [target]
                            while todo:
                                k = todo.pop(0)
                                if k in seen or k >= len(mg["s"]):
                                    continue
                                seen.add(k)
                                ts = mg["s"][k]
                                if ts["child"] != 0xFFFF and not ts["f"] & FLAG_CHILD_SLOT:
                                    branch.append("child:" + names.get(ts["child"], str(ts["child"])))
                                for tb in ts["b"]:
                                    branch += [by_vtable.get(x, ("vt%06x" % x, []))[0].replace("Command", "") for x in tb["x"]]
                                    if tb["j"] >= 0:
                                        todo.append(tb["j"])
                        skip.append({"graph": member, "name": mg["name"], "state": i, "live": i in live, "to": target,
                                     "runs": branch})
        cmds = collections.Counter()
        conds = collections.Counter()
        waits = []
        for member in cl:
            mg = graphs[member]
            live = reachable_states(mg)
            cmds += commands_of(mg, by_vtable, live)
            for i in live:
                s = mg["s"][i]
                pk = s.get("pk")
                if pk and ("dur" in pk or "delay" in pk):
                    waits.append(pk.get("dur", pk.get("delay")))
                for b in s["b"]:
                    if "c" in b:
                        conds[conditions.get(b["c"], hex(b["c"]))] += 1
        videos = sorted({v for member in cl for s in graphs[member]["s"] for b in s["b"] for v in b.get("v", [])})
        users = set()
        for sid in graph_starters.get(gid, ()):
            users |= starter_users.get(sid, set())
        rows.append({
            "graph": gid, "name": g["name"], "chunks": sorted({c for c, _ in users}), "objects": sorted({o for _, o in users}),
            "starters": sorted(graph_starters.get(gid, ())), "members": sorted(cl), "member_names": [names.get(m, "?") for m in sorted(cl)],
            "skip": skip, "skip_live": any(s["live"] for s in skip), "videos": videos,
            "kind": "A" if any(x in file_vts for member in cl for s in graphs[member]["s"] for b in s["b"] for x in b["x"]) else "B",
            "sound_track": any(x in track_vts for member in cl for s in graphs[member]["s"] for b in s["b"] for x in b["x"]),
            "commands": dict(cmds.most_common()), "conditions": dict(conds.most_common()),
            "waits": sorted(waits, reverse=True)[:12], "stated_seconds": round(sum(waits), 2),
            "ends": any(direct(graphs[m], ce_vt) for m in cl),
        })
    return rows


def cmd_report(args):
    logs = sorted(glob.glob(os.path.join(args.logs, "*.log")))
    graphs, starters, chunks, variants = read_logs(logs)
    by_vtable, _ = command_tables()
    conditions = condition_tables()
    rows = report(graphs, starters, chunks, by_vtable, conditions)
    differing = sorted(g for g, v in variants.items() if len(v) > 1)
    summary = {"logs": len(logs), "graphs": len(graphs), "starters": len(starters), "chunks": len(chunks), "sites": len(rows),
               "kind_A": sum(r["kind"] == "A" for r in rows), "kind_B": sum(r["kind"] == "B" for r in rows),
               "with_sound_track": sum(r["sound_track"] for r in rows),
               "with_live_skip": sum(r["skip_live"] for r in rows), "with_dead_skip_only": sum(bool(r["skip"]) and not r["skip_live"] for r in rows),
               "without_skip": sum(not r["skip"] for r in rows), "graphs_read_differently": differing}
    totals = collections.Counter()
    for r in rows:
        if not r["skip_live"]:
            totals.update(r["commands"])
    summary["commands_of_sites_without_a_live_skip"] = dict(totals.most_common())
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"summary": summary, "sites": rows}, f, indent=1)
    print(json.dumps(summary, indent=1))
    for r in rows:
        print("%5d %-55s kind %s skip %-5s chunks %s" % (r["graph"], r["name"][:55], r["kind"],
                                                        "live" if r["skip_live"] else ("dead" if r["skip"] else "no"),
                                                        ",".join(c.split("\\")[-1] for c in r["chunks"]) or "-"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--iso", required=True)
    r.add_argument("--runner", default=os.path.join(ROOT, "build", "twinsanityevolved"))
    r.add_argument("--out", default=os.path.join(ROOT, "build", "cutscene_census"))
    r.add_argument("--jobs", type=int, default=4)
    r.add_argument("--stop-at", type=int, default=4200)
    r.add_argument("--chunks", default="")
    p = sub.add_parser("report")
    p.add_argument("--logs", required=True)
    p.add_argument("--json", default="")
    a = ap.parse_args()
    return cmd_run(a) if a.cmd == "run" else cmd_report(a)


if __name__ == "__main__":
    sys.exit(main() or 0)
