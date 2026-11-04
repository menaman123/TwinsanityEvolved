#!/usr/bin/env python3
"""tools/vita/lanes_compare.py MODULE_LOG NATIVE_LOG -- two runs of one deterministic route on the PS Vita (Vita3K, or the
console), the module build (vita-module-release) against the native one (vita-release), compared from their app logs
(ux0:data/twinsanity/twinsanityevolved.log): README.md's PS Vita section has the recipe.

  the draw lists   TE_GFX_DRAWHASH=1's line per picture, "[te.drawhash] picture P: HASH (N draws)" -- every draw's state,
                   vertices, mesh parameters and skin palette hashed in submission order: the pictures both runs reached,
                   how many are identical, the first that differ
  the probes       --trace-player N's "[probe] @B ..." lines (the played character, the camera, the game state, the clock);
                   a line that differs only in 8-digit hex words is counted apart: there the module prints a guest offset
                   and the native build a pointer
  the game states  --trace-state's "game state @ boundary B ..." lines

Exit status 0 when every picture both reached is identical and the probes and the game states differ in pointer words at most
(--allow N: up to N differing pictures pass, for a known difference), 1 otherwise, 2 when a log has no draw-list lines.
"""
import argparse
import re
import sys

STAMP = re.compile(r"^\[\d+\.\d+\] ")
HEX = re.compile(r"0x[0-9A-Fa-f]{8}")
DRAWHASH = re.compile(r"\[te\.drawhash\] picture (\d+): ([0-9a-f]+) \((\d+) draws\)")


def load(path):
    draws, probes, states = {}, [], []
    with open(path, errors="replace") as f:
        for line in f:
            line = STAMP.sub("", line.rstrip("\n"))
            m = DRAWHASH.search(line)
            if m:
                draws[int(m.group(1))] = (m.group(2), int(m.group(3)))
            elif line.startswith("[probe] @"):
                probes.append(line)
            elif "game state @ boundary" in line:
                states.append(line)
    return draws, probes, states


def compare_lines(name, a, b):
    n = min(len(a), len(b))
    differ = [k for k in range(n) if a[k] != b[k]]
    pointer = [k for k in differ if HEX.sub("0x~", a[k]) == HEX.sub("0x~", b[k])]
    other = [k for k in differ if k not in pointer]
    print("%s: %d / %d lines, %d differ (%d only in a pointer word)" % (name, len(a), len(b), len(differ), len(pointer)))
    for k in other[:3]:
        print("  module %s\n  native %s" % (a[k][:220], b[k][:220]))
    return len(other) == 0 and len(a) == len(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("module_log")
    ap.add_argument("native_log")
    ap.add_argument("--allow", type=int, default=0, help="differing pictures that still pass (a known difference)")
    a = ap.parse_args()
    (dm, pm, sm), (dn, pn, sn) = load(a.module_log), load(a.native_log)
    if not dm or not dn:
        print("no [te.drawhash] lines in %s (TE_GFX_DRAWHASH=1 in args.txt)" % (a.module_log if not dm else a.native_log))
        return 2
    common = sorted(set(dm) & set(dn))
    differ = [p for p in common if dm[p] != dn[p]]
    print("draw lists: %d pictures in both (module %d, native %d), %d identical, %d differ%s" % (
        len(common), len(dm), len(dn), len(common) - len(differ), len(differ),
        (": " + ", ".join(str(p) for p in differ[:12]) + (" ..." if len(differ) > 12 else "")) if differ else ""))
    ok = len(differ) <= a.allow
    ok = compare_lines("probes", pm, pn) and ok
    ok = compare_lines("game states", sm, sn) and ok
    print("IDENTICAL" if ok and not differ else ("PASS (%d known)" % len(differ) if ok else "DIFFER"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
