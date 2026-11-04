#!/usr/bin/env python3
"""tools/cutscenes/show.py -- one graph of a census log written out by its states: the control packet's motion and stated
waits, each body's condition and threshold, its commands by the factory's names and its jump (ids, names and numbers only).

  tools/cutscenes/show.py LOG GRAPH_ID [GRAPH_ID ...]
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import census  # noqa: E402

FLAG_COMPLETION, FLAG_INTERRUPTING, FLAG_CHILD_SLOT, FLAG_PACKET = 1 << 10, 1 << 11, 1 << 12, 1 << 14


def describe(g, by_vtable, conditions, names):
    out = ["graph %d %r start s%d (%d states)" % (g["g"], g["name"], g["start"], len(g["s"]))]
    for i, s in enumerate(g["s"]):
        f = s["f"]
        bits = []
        if f & FLAG_COMPLETION:
            bits.append("completion")
        if f & FLAG_INTERRUPTING:
            bits.append("interrupting")
        child = s["child"]
        if child != 0xFFFF:
            bits.append(("child slot %d" % child) if f & FLAG_CHILD_SLOT else ("child %d %r" % (child, names.get(child, "?"))))
        pk = s.get("pk")
        if pk:
            bits.append("packet m%d%s%s%s" % (pk["m"], " delay %g" % pk["delay"] if "delay" in pk else "",
                                             " dur %g" % pk["dur"] if "dur" in pk else "", " stalls" if pk.get("st") else ""))
        out.append("  s%d: %s" % (i, ", ".join(bits) or "-"))
        for bi, b in enumerate(s["b"]):
            if "c" in b:
                cond = "%s%s(0x%X p%d) > %g" % ("NOT " if b["inv"] else "", conditions.get(b["c"], "?"), b["c"], b["p"], b["t"])
            else:
                cond = "(no condition)"
            cmds = [by_vtable.get(x, ("vt%06x" % x, []))[0].replace("Command", "") for x in b["x"]]
            # SendUserMessage with the census's argument words (runner/te_cutscenes.cpp "xa"): the message and whom it goes to, in
            # SendUserMessageCommand::Execute's order (game/src/game/commandsattach.cpp): every linked object, those a field picks,
            # the linked objects of an object id, the linked one of an index (the recipient byte; the current / last one), else a
            # designator (the starter's receiver below 0xDE, or itself / the player / the originator / the focus ...)
            designators = {0xF0: "itself", 0xF2: "player", 0xF4: "originator", 0xF5: "head target", 0xF7: "agentref2",
                           0xF8: "agentref1", 0xFB: "focus", 0xFD: "next key", 0xFE: "current key", 0xFF: "none"}
            for k, xa in enumerate(b.get("xa", [])):
                if k < len(cmds) and cmds[k] in ("SendUserMessage",):
                    target, fl = xa
                    recipient = target >> 16 & 0xFF
                    if fl >> 17 & 1:
                        how = "every linked"
                    elif fl >> 24 & 1:
                        how = "linked by field %d" % (fl >> 25 & 31)
                    elif fl & 0xFFFF != 0xFF:
                        how = "linked of object %d" % (fl & 0xFFFF)
                    elif fl >> 16 & 1:
                        how = "linked current" if fl >> 18 & 1 else "linked last" if fl >> 20 & 1 else "linked[%d]" % recipient
                    else:
                        how = ("receiver %d" % recipient) if recipient < 0xDE else designators.get(recipient, "designator 0x%X" % recipient)
                    cmds[k] += "<m%d to %s>" % (target & 0xFFFF, how)
            tag = "completion" if bi == 0 and f & FLAG_COMPLETION else "b%d" % bi
            out.append("    %s: %s -> %s%s  [%s]" % (tag, cond, ("s%d" % b["j"]) if b["j"] >= 0 else "end", " (restart)" if b["r"] else "",
                                                 ", ".join(cmds)))
    return "\n".join(out)


def main():
    log, ids = sys.argv[1], [int(x, 0) for x in sys.argv[2:]]
    by_vtable, _ = census.command_tables()
    conditions = census.condition_tables()
    graphs = {}
    for line in open(log, encoding="utf-8", errors="replace"):
        if line.startswith("[cs.graph] "):
            g = json.loads(line[11:])
            graphs.setdefault(g["g"], g)
    names = {k: v["name"] for k, v in graphs.items()}
    for i in ids:
        print(describe(graphs[i], by_vtable, conditions, names) if i in graphs else "graph %d: not in the log" % i)
        print()


if __name__ == "__main__":
    main()
