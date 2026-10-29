#!/usr/bin/env python3
"""prelink.py -- the Vita's native build of the game code as ONE relocatable object (CMake TE_VITA_NATIVE).

Every game TU (tools/te_cxx_arm.py), the retail function table and the guest block (tools/gen_retail_image.py --arm) are
linked with `ld -r` into one object, then every symbol it defines is made LOCAL except the few the runner uses (objcopy
-G): the decomp's own C library (memmove, printf, puts, strlcpy, its malloc and break, operator new if it has one), its
thousands of retail-named functions and the retail data symbols can then never bind a reference of newlib, libstdc++, the
engine or SDL -- and nothing of theirs can bind one of the game's (references inside the object are resolved to its own
definitions before the symbols go local).

The object's remaining UNDEFINED symbols are checked: each must be a seam body (ps2e_*: platform/nativeee/host), the
runner's trampolines (CallEabiTrampoline / CallEabiTrampolineF: runner/te_native.cpp), the profiler's section array
(te_prof_section: a TE_PROF_SEAM build's forwarders mark it) or a C runtime function of the
allowed list (newlib's string functions and sprintf family, libgcc's __aeabi_* helpers). Anything else -- a retail data
symbol the guest block does not define, a game function with no body -- fails the build with its name.

    prelink.py --binutils PREFIX --out game.o [--report report.json] [--keep SYM ...] -- OBJ ...
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

KEEP = ["entry", "te_retail_functions", "te_retail_function_count", "te_guest_block"]
RUNTIME = re.compile(r"^(__aeabi_\w+|__gnu_\w+|memset|memcpy|memmove|memcmp|memchr|strlen|strcmp|strncmp|strcpy|strncpy|strcat|"
                     r"strncat|strchr|strrchr|strstr|sprintf|snprintf|vsprintf|vsnprintf|abort|__cxa_pure_virtual|__stack_chk_fail)$")
SEAM = re.compile(r"^(ps2e_\w+|CallEabiTrampoline|CallEabiTrampolineF|te_prof_section)$")   # te_prof_section: TE_PROF_SEAM's markers


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"prelink: failed: {' '.join(cmd[:4])} ...")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binutils", required=True, help="the toolchain's prefix, e.g. /usr/local/vitasdk/bin/arm-vita-eabi-")
    ap.add_argument("--out", required=True)
    ap.add_argument("--report", default=None)
    ap.add_argument("--keep", action="append", default=[])
    ap.add_argument("objects", nargs="+")
    a = ap.parse_args()
    objs = [o for o in a.objects if o != "--"]
    bu = a.binutils
    raw = a.out + ".raw.o"
    run([bu + "ld", "-r", "-o", raw, *objs])
    keep = KEEP + a.keep
    run([bu + "objcopy", *[f"-G{k}" for k in keep], raw, a.out])

    defined_global = []
    for line in run([bu + "nm", "-g", "--defined-only", a.out]).splitlines():
        p = line.split()
        if len(p) == 3:
            defined_global.append(p[2])
    missing_keep = [k for k in keep if k not in defined_global]
    undefined = sorted({l.split()[-1] for l in run([bu + "nm", "-u", a.out]).splitlines() if l.strip()})
    seam = [u for u in undefined if SEAM.match(u)]
    runtime = [u for u in undefined if RUNTIME.match(u)]
    other = [u for u in undefined if not SEAM.match(u) and not RUNTIME.match(u)]
    sizes = {}
    for line in run([bu + "size", "-A", a.out]).splitlines():
        p = line.split()
        if len(p) >= 2 and p[1].isdigit() and p[0].startswith("."):
            key = ".text" if p[0].startswith(".text") else ".rodata" if p[0].startswith(".rodata") else \
                  ".data" if p[0].startswith(".data") else ".bss" if p[0].startswith(".bss") else None
            if key:
                sizes[key] = sizes.get(key, 0) + int(p[1])
    rep = {"objects": len(objs), "kept_global": defined_global, "undefined_seam": len(seam), "undefined_runtime": runtime,
           "undefined_other": other, "section_bytes": sizes}
    if a.report:
        Path(a.report).write_text(json.dumps(rep, indent=1))
    if missing_keep or other:
        demangled = run([bu + "c++filt"], input="\n".join(other)).splitlines() if other else []
        raise SystemExit(f"prelink: the native game object is incomplete: entry points missing {missing_keep}; "
                         f"unresolvable references {demangled}")
    print(f"prelink: {len(objs)} objects -> {a.out}: global {defined_global}; needs {len(seam)} seam bodies + "
          f"runtime {runtime}; .text {sizes.get('.text', 0):,} B, .rodata {sizes.get('.rodata', 0):,} B, "
          f".data {sizes.get('.data', 0):,} B, .bss {sizes.get('.bss', 0):,} B (the guest block's 32 MB included)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
