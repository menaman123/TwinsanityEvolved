#!/usr/bin/env python3
"""insn_compare.py NATIVE_ELF W2C_ELF [--hot NAME ...] [--json OUT] -- static instruction counts of the game's functions,
the Vita's native build against the wasm2c build (by function name: a retail-named function is FUN_xxx / its retail name in
both; a C++ function is its demangled signature in both -- the native ELF's labels demangled by objdump -C, wasm2c's
w2c_game_<the name section's demangled name with 0xNN escapes>). Counts instructions between a function's label and the
next label in `objdump -d` (literal pools excluded: objdump marks them .word)."""
import argparse
import json
import re
import subprocess
import sys
from collections import Counter

OBJDUMP = "/usr/local/vitasdk/bin/arm-vita-eabi-objdump"


def functions(elf, prefix=None, demangle=False):
    cmd = [OBJDUMP, "-d", "--no-show-raw-insn"] + (["-C"] if demangle else []) + [elf]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    funcs, cur, kinds = {}, None, None
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]{8} <(.+)>:$", line)
        if m:
            cur = m.group(1)
            if prefix is not None and not cur.startswith(prefix):
                cur = None
                continue
            funcs[cur] = Counter()
            continue
        if cur is None:
            continue
        p = line.split("\t")
        if len(p) >= 2 and re.match(r"^\s*[0-9a-f]+:$", p[0]):
            op = p[1].strip().split()[0] if p[1].strip() else ""
            if op in (".word", ".short", ".byte") or not op:
                continue
            funcs[cur]["insns"] += 1
            base = re.sub(r"(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(\.w|\.n)?$", "", op)
            if op.startswith("vmrs"):
                funcs[cur]["vmrs"] += 1
            if op.startswith(("ldr", "ldm", "vldr", "vldm", "pop")):
                funcs[cur]["loads"] += 1
            if op.startswith(("str", "stm", "vstr", "vstm", "push")):
                funcs[cur]["stores"] += 1
            if base in ("bl", "blx"):
                funcs[cur]["calls"] += 1
    return funcs


def unescape(name):
    return re.sub(r"0x([0-9A-Fa-f]{2})", lambda m: chr(int(m.group(1), 16)), name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("native")
    ap.add_argument("w2c")
    ap.add_argument("--hot", nargs="*", default=[])
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    nat = functions(a.native, demangle=True)
    w2c_raw = functions(a.w2c, prefix="w2c_game_")
    w2c = {}
    for k, v in w2c_raw.items():
        w2c.setdefault(unescape(k[len("w2c_game_"):]), v)
    common = sorted(set(nat) & set(w2c))
    tot_n = sum(nat[k]["insns"] for k in common)
    tot_w = sum(w2c[k]["insns"] for k in common)
    print(f"functions: native {len(nat)}, wasm2c {len(w2c)}, matched by name {len(common)}")
    print(f"matched set: native {tot_n:,} instructions, wasm2c {tot_w:,} (wasm2c / native = {tot_w / max(1, tot_n):.3f})")
    for key in ("loads", "stores", "calls", "vmrs"):
        n = sum(nat[k][key] for k in common)
        w = sum(w2c[k][key] for k in common)
        print(f"  {key:6}: native {n:,}  wasm2c {w:,}  (ratio {w / max(1, n):.3f})")
    rows = []
    for h in a.hot:
        if h in nat and h in w2c:
            rows.append((h, nat[h]["insns"], w2c[h]["insns"]))
            print(f"  {h:40} native {nat[h]['insns']:6}  wasm2c {w2c[h]['insns']:6}  ratio {w2c[h]['insns'] / max(1, nat[h]['insns']):.2f}"
                  f"  (loads {nat[h]['loads']}/{w2c[h]['loads']}, stores {nat[h]['stores']}/{w2c[h]['stores']}, calls {nat[h]['calls']}/{w2c[h]['calls']})")
        else:
            print(f"  {h:40} not matched (native {h in nat}, wasm2c {h in w2c})")
    if a.json:
        json.dump({"matched": len(common), "native_insns": tot_n, "w2c_insns": tot_w,
                   "per_function": {k: [nat[k]["insns"], w2c[k]["insns"]] for k in common}}, open(a.json, "w"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
