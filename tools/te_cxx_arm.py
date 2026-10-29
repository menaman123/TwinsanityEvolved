#!/usr/bin/env python3
"""te_cxx_arm.py -- compile ONE game translation unit to a NATIVE armv7 (PlayStation Vita) object, zero edits to game code.

The ARM twin of tools/te_cxx.py (the wasm32 wrapper): the same five MIPS-only constructs of the decomp are rewritten in
the PREPROCESSED text (the patterns are te_cxx.py's own, imported, so the two wrappers can never drift apart), with the
replacement each construct needs on ARM instead of on wasm:

  R1 the float fence `asm("" : "+f"(x))`: "+f" is the MIPS FPU constraint and GCC's ARM backend REJECTS it ("impossible
     constraint in 'asm'": the FPA class is gone). "+t" is the VFP single-precision register class: the value stays in an
     s-register and the compiler must treat it as rewritten -- the fence's intent, at no cost (no memory round trip).
  R2 the file-scope EABI thunks `asm((Abi::Thunk<...>(Abi::Direction::Export, "NAME")))`: the consteval MIPS text moves
     registers between the EABI and n32. AAPCS-VFP (hard float) assigns integer and float arguments from SEPARATE register
     files in their order, exactly as GCC 2.9x's EABI does, so on ARM the retail name IS the C++ function: the thunk
     becomes an alias NAME -> NAME_n32 (the same alias record as the wasm build's).
  R3 the hand-written MIPS thunk blocks `asm(R"(...)")`: the same aliases; a block with no `_n32` target is a
     runtime-provided symbol (CallEabiTrampoline).
  R4 the `lb $0, 0(p)` cache touch: __builtin_prefetch(p) (a PLD).
  R5 the `lq/sq` quadword copy: a 16-byte __builtin_memcpy between the 16-byte-aligned addresses.
  (R6, te_cxx.py's rename of CallEabiFloat's asm label to CallEabiTrampolineF, is applied as for wasm: one symbol per
   return kind.)

Flags: te_cxx.py's (gnu++26, -D_EE, no exceptions/RTTI/thread-safe statics, -fno-strict-aliasing, -ffp-contract=off,
-fno-delete-null-pointer-checks, function/data sections) plus what the MIPS ABI the game was written for implies on this
target: -fno-short-enums (arm-vita-eabi's GCC defaults to SHORT enums: `enum {A, B}` is 1 byte there, 4 on the R5900
and on wasm32), -fsigned-char (plain char signed as on MIPS and wasm32), -fno-math-errno (sqrtf is the bare VSQRT, as
wasm's f32.sqrt), -Wno-psabi.

    te_cxx_arm.py [--gxx PATH] --game DIR --shim DIR [-I DIR ...] [-D X ...] [-O2] [--report out.json]
                  [--keep-ii out.ii] -o out.o file.cpp
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import te_cxx  # noqa: E402  (the patterns: one definition for both targets)

CXXFLAGS_ARM = [
    "-std=gnu++26", "-D_EE", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics",
    "-fno-asynchronous-unwind-tables", "-fno-common", "-fno-strict-aliasing", "-ffunction-sections",
    "-fdata-sections", "-ffp-contract=off", "-fno-delete-null-pointer-checks", "-fno-short-enums", "-fsigned-char",
    # -fwrapv: signed overflow wraps, as the R5900's integer unit and the wasm module's i32 arithmetic do (GCC would
    # otherwise optimise on the overflow being impossible, differently from the wasm build's clang)
    "-fwrapv",
    "-fno-math-errno", "-Wall", "-Wno-invalid-offsetof", "-Wno-psabi",
]

# R7 (the native build only): the update stagger of ObjectNode::Update (objectnode.cpp) and Ogi's (ogi.cpp),
# `(g_RenderedFrames + (reinterpret_cast<u32>(this) >> 8)) & mask`, reads the object's ADDRESS. In the native build the game's
# heap lives in the guest block at te_guest_block + (the wasm build's address), so the address is made block-relative: the
# same bits as the wasm build's (and the PS2's) for every object of the heap, whatever address the block was loaded at.
RE_STAGGER = re.compile(r"\(\s*reinterpret_cast\s*<\s*u32\s*>\s*\(\s*this\s*\)\s*>>\s*8\s*\)")


def rewrite_arm(text):
    log = {"fences": 0, "thunks_export": [], "thunks_import": [], "raw_blocks": [], "touch": 0, "lqsq": 0,
           "unrecognised_asm": 0, "trampoline_f_renamed": 0, "kept_portable_asm": 0, "stagger": 0}

    def stagger(m):
        log["stagger"] += 1
        return "((reinterpret_cast<u32>(this) - reinterpret_cast<u32>(te_guest_block)) >> 8)"
    text = RE_STAGGER.sub(stagger, text)
    if log["stagger"]:
        text = 'extern "C" unsigned char te_guest_block[];\n' + text

    def fence(m):
        log["fences"] += 1
        return '__asm__("" : "+t"(%s));' % m.group(1)   # __asm__: the blocker scan below matches only `asm`
    text = te_cxx.RE_FENCE.sub(fence, text)

    def thunk(m):
        direction, name = m.group(1), m.group(2)
        if direction == "Export":
            log["thunks_export"].append(name)
            return 'extern "C" void te_alias_%s(void) __asm__("%s") __attribute__((alias("%s_n32")));' % (name, name, name)
        log["thunks_import"].append(name)
        return ";"
    text = te_cxx.RE_THUNK.sub(thunk, text)

    def raw(m):
        body = m.group(1)
        globls = te_cxx.RE_RAW_GLOBL.findall(body)
        jumps = te_cxx.RE_RAW_JUMP.findall(body)
        aliases = [(g, g + "_n32") for g in globls if (g + "_n32") in jumps]
        log["raw_blocks"].append({"defines": globls, "jumps": jumps, "aliases": aliases,
                                  "runtime_provided": [g for g in globls if (g + "_n32") not in jumps]})
        return " ".join('extern "C" void te_alias_%s(void) __asm__("%s") __attribute__((alias("%s")));' % (a[0], a[0], a[1])
                        for a in aliases) or ";"
    text = te_cxx.RE_RAW.sub(raw, text)

    def touch(m):
        log["touch"] += 1
        return "__builtin_prefetch((const void*)(%s));" % m.group(1)
    text = te_cxx.RE_TOUCH.sub(touch, text)

    def lqsq(m):
        log["lqsq"] += 1
        to, frm = m.group(1), m.group(2)
        return ("__builtin_memcpy((void*)((unsigned int)(%s) & ~15u), (const void*)((unsigned int)(%s) & ~15u), 16);"
                % (to, frm))
    text = te_cxx.RE_LQSQ.sub(lqsq, text)

    text, n6 = te_cxx.RE_TRAMP_F.subn(r"\1F\2", text)
    log["trampoline_f_renamed"] = n6
    portable = re.compile(r'(?<![\w.])asm\s*(?:volatile\s*)?\(\s*""\s*:\s*"[=+]m"\s*\([^)]*\)\s*\)\s*;')
    log["kept_portable_asm"] = len(portable.findall(text))
    log["unrecognised_asm"] = len(te_cxx.RE_ANY_ASM_STMT.findall(text)) - log["kept_portable_asm"]
    return text, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gxx", default=os.path.join(os.environ.get("VITASDK", "/usr/local/vitasdk"), "bin", "arm-vita-eabi-g++"))
    ap.add_argument("--game", required=True)
    ap.add_argument("--shim", required=True)
    ap.add_argument("-O", dest="opt", default="2")
    ap.add_argument("-I", dest="incs", action="append", default=[])
    ap.add_argument("-D", dest="defs", action="append", default=[])
    ap.add_argument("-f", dest="fflags", action="append", default=[], help="extra -f flags (without the -f)")
    ap.add_argument("--report", default=None)
    ap.add_argument("--keep-ii", default=None)
    ap.add_argument("-o", dest="out", required=True)
    ap.add_argument("src")
    a = ap.parse_args()

    flags = [*CXXFLAGS_ARM, f"-O{a.opt}", *[f"-f{f}" for f in a.fflags]]
    inc = [f"-I{Path(a.game) / 'include'}", f"-I{a.shim}", *[f"-I{i}" for i in a.incs]]
    defs = [f"-D{d}" for d in a.defs]
    report = {"src": a.src, "out": a.out, "ok": False, "stage": None, "rewrites": None, "errors": [], "stderr": "",
              "target": "armv7 (arm-vita-eabi)"}
    r = subprocess.run([a.gxx, "-E", *flags, *inc, *defs, a.src], capture_output=True, text=True)
    if r.returncode != 0:
        report["stage"] = "preprocess"
        report["stderr"] = r.stderr
        report["errors"] = [l for l in r.stderr.splitlines() if "error" in l]
    else:
        text, log = rewrite_arm(r.stdout)
        report["rewrites"] = log
        if a.keep_ii:
            Path(a.keep_ii).write_text(text)
        c = subprocess.run([a.gxx, *flags, "-x", "c++", "-c", "-o", a.out, "-"], input=text, capture_output=True, text=True)
        report["stderr"] = c.stderr
        report["errors"] = [l for l in c.stderr.splitlines() if "error" in l]
        report["ok"] = c.returncode == 0
        report["stage"] = "compile"
    if a.report:
        Path(a.report).parent.mkdir(parents=True, exist_ok=True)
        Path(a.report).write_text(json.dumps(report, indent=1))
    if not report["ok"]:
        sys.stderr.write(report["stderr"])
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
