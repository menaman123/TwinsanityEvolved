#!/usr/bin/env python3
"""tools/cutscenes/who.py -- which behaviours run during a run's first cutscene: every instance whose running behaviour changes
between the last Playing line before it and the first Playing line after it (TE_CUTSCENE_STATE lines), with the graphs' names
from a census log (ids and names only).

  tools/cutscenes/who.py RUN.log CENSUS_GRAPHS.log
"""
import json
import sys


def main():
    run, census = sys.argv[1], sys.argv[2]
    names = {}
    for line in open(census, encoding="utf-8", errors="replace"):
        if line.startswith("[cs.graph] "):
            g = json.loads(line[11:])
            names[g["g"]] = g["name"]
    lines = [json.loads(l[11:]) for l in open(run, encoding="utf-8", errors="replace") if l.startswith("[cs.state] {")]
    first = next((i for i, l in enumerate(lines) if l["gs"] == 13), None)
    if first is None:
        print("who: no Watching")
        return 1
    window = [l for l in lines[first:] if l["gs"] in (13, 15, 17, 14, 11)]
    seen = {}
    for l in window:
        for c in l["chunks"]:
            for i in c["i"]:
                for slot, r in enumerate(i["r"]):
                    if not r:
                        continue
                    key = (c["c"].split("\\")[-1], i["n"], slot)
                    graphs = [names.get(g, str(g)) for g, _, _ in r]
                    seen.setdefault(key, [])
                    if not seen[key] or seen[key][-1] != graphs:
                        seen[key].append(graphs)
    for key, history in sorted(seen.items()):
        if any(any(w in "/".join(h) for w in ("CUTSCENE", "DIRECTOR", "_CS", "SKIP")) for h in history) or len(history) > 1:
            print("%s %s r%d: %s" % (key[0], key[1].split("|")[-1], key[2], " -> ".join("/".join(h) for h in history[:6])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
