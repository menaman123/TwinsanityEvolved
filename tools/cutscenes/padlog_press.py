#!/usr/bin/env python3
"""tools/cutscenes/padlog_press.py -- a pad log with one more button press: BUTTONS held from boundary B for N boundaries (the
neutral pad after), the log's own lines around it kept (inputs only, as the fixtures' format). The skip legs: the watched leg's
pad log with CROSS pressed at the cutscene's first Watching boundary.

  tools/cutscenes/padlog_press.py IN OUT --at B [--buttons 4000] [--frames 3]
"""
import argparse


def press(lines, at, buttons, frames):
    head = [l for l in lines if l.startswith("#") and not l.startswith("# end")]
    tail = [l for l in lines if l.startswith("# end")]
    events = []
    for l in lines:
        if l.startswith("#") or not l.strip():
            continue
        b = int(l.split()[0])
        events.append((b, l.rstrip("\n")))
    # the held state at the press's end: the last line before it (the press replaces what was held over its frames)
    held = None
    for b, l in events:
        if b <= at + frames:
            held = l
    kept = [(b, l) for b, l in events if not (at <= b < at + frames)]
    kept.append((at, "%d %s 128 128 128 128  # PRESS" % (at, buttons)))
    after = "%d 0000 128 128 128 128  # NONE" % (at + frames)
    if held is not None and int(held.split()[0]) < at:
        after = "%d %s" % (at + frames, " ".join(held.split()[1:]))
    if not any(b == at + frames for b, _ in kept):
        kept.append((at + frames, after))
    kept.sort(key=lambda e: e[0])
    return [h.rstrip("\n") for h in head] + [l for _, l in kept] + [t.rstrip("\n") for t in tail]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--at", type=int, required=True)
    ap.add_argument("--buttons", default="4000")
    ap.add_argument("--frames", type=int, default=3)
    a = ap.parse_args()
    lines = open(a.inp).readlines()
    with open(a.out, "w") as f:
        f.write("\n".join(press(lines, a.at, a.buttons, a.frames)) + "\n")


if __name__ == "__main__":
    main()
