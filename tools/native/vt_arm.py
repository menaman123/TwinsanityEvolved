#!/usr/bin/env python3
"""vt_arm.py DRYRUN.txt GAME.wasm WASM_OBJDUMP -- the runner's thunked/pure-virtual vtable slots (--dry-run --verbose) with
their BODIES' signatures, classified for a native ARM (AAPCS-VFP, hard float) build where no thunk exists and the slot
is called with the declared signature straight into the body."""
import re
import subprocess
import sys
from collections import Counter

dry, wasm, objdump = sys.argv[1:4]
out = subprocess.run([objdump, "-x", wasm], capture_output=True, text=True).stdout
types, func_sig, func_name, elem = {}, {}, {}, {}
sec = None
for line in out.splitlines():
    m = re.match(r"^(\w+)\[\d+\]:", line)
    if m:
        sec = m.group(1)
        continue
    if sec == "Type":
        m = re.match(r"\s*- type\[(\d+)\] \((.*?)\) -> (.*)", line)
        if m:
            types[int(m.group(1))] = ([p.strip() for p in m.group(2).split(",") if p.strip()], m.group(3).strip())
    elif sec == "Function":
        m = re.match(r"\s*- func\[(\d+)\] sig=(\d+)(?: <(.*)>)?", line)
        if m:
            func_sig[int(m.group(1))] = int(m.group(2))
            func_name[int(m.group(1))] = m.group(3) or ""
    elif sec == "Elem":
        m = re.match(r"\s*- elem\[(\d+)\] = ref\.func:(\d+)(?: <(.*)>)?", line)
        if m:
            elem[int(m.group(1))] = int(m.group(2))
            if m.group(3):
                func_name.setdefault(int(m.group(2)), m.group(3))

def regs(params):
    """AAPCS core-register words of an integer argument list (i64 aligned to an even pair)."""
    n = 0
    for p in params:
        if p == "i64":
            n += n % 2
            n += 2
        elif p == "i32":
            n += 1
    return n

rows = []
for line in open(dry):
    m = re.match(r"\s*vtable 0x([0-9A-F]+) slot (\d+): body index (\d+) -> thunk (\d+) \((.*)\)( \[pure virtual\])?", line)
    if not m:
        continue
    vt, slot, body, sig, pure = m.group(1), int(m.group(2)), int(m.group(3)), m.group(5), bool(m.group(6))
    ms = re.fullmatch(r"\((.*?)\) -> (\w+)", sig)
    dparams = [p.strip() for p in ms.group(1).split(",") if p.strip()]
    dres = ms.group(2)
    f = elem.get(body)
    bparams, bres = types.get(func_sig.get(f, -1), ([], "?"))
    name = func_name.get(f, "?")
    dw, bw = regs(dparams), regs(bparams)
    if any(p in ("f32", "f64") for p in bparams):
        cls = "float parameter in the body: reads s-registers the caller never set (garbage; as on the PS2's $f12+)"
    elif "i64" in dparams or "i64" in bparams:
        same_layout = dparams[:len(bparams)] == bparams
        cls = "i64: AAPCS pairs it in r2:r3 -- " + ("prefix agrees, safe" if same_layout else "MISALIGNED (breaks)")
    elif bw <= dw:
        cls = "body takes a prefix of the arguments: safe"
    elif bw <= 4:
        cls = "body reads more registers (r0-r3) than passed: garbage values, as on the PS2's a-registers"
    else:
        cls = f"body reads {bw - max(dw, 4)} STACK word(s) the caller never pushed: risky (a callee may store to them)"
    if dres == bres or dres == "nil":
        rcls = "result ok"
    elif bres == "nil":
        rcls = f"caller reads a {dres} the body never set (r0/s0 garbage, as on the PS2)"
    else:
        rcls = f"result kind differs ({bres} body, {dres} slot): " + ("r0 vs s0" if "f32" in (bres, dres) else "width")
    rows.append((vt, slot, sig, f"({', '.join(bparams)}) -> {bres}", name, pure, cls, rcls))

print(f"{len(rows)} slots ({sum(1 for r in rows if not r[5])} thunked, {sum(1 for r in rows if r[5])} pure-virtual)")
for k, n in Counter(r[6] for r in rows).most_common():
    print(f"  {n:4d}  args: {k}")
for k, n in Counter(r[7] for r in rows).most_common():
    print(f"  {n:4d}  result: {k}")
print()
for r in rows:
    if "safe" not in r[6] or "ok" not in r[7]:
        print(f"  0x{r[0]} slot {r[1]:3d}: slot {r[2]:28s} body {r[3]:28s} {r[4][:60]:60s} {'[pure] ' if r[5] else ''}{r[6]}; {r[7]}")
