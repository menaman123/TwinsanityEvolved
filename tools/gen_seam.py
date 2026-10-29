#!/usr/bin/env python3
"""gen_seam.py -- the seam, generated from game/include/platform/*.h (the ONLY seam).

Reads the 14 platform headers (never edits them) and writes:
  ps2e_seam.h            C prototypes `ps2e_<ns>_<fn>` with scalar-only parameters (pointers/references/by-value
                         structs -> u32 guest address; bool/enum/small ints -> s32; f32; u64). Both sides include it.
  seam_forwarders.cpp    wasm side: `Platform::NS::Fn(...)` bodies that call the import (compiled to wasm32).
  seam_imports.txt       the module's complete function import list (wasm-ld --allow-undefined-file).
  seam_host_thunks.c     host side: `w2c_env_<import>(struct w2c_env*, ...)` -> `ps2e_<...>(...)` (wasm2c glue).
  seam_stubs.cpp         host side: WEAK default `ps2e_*` bodies (log once, return 0) so the runner links before an
                         owner's module defines the strong symbol in platform/nativeee/<header>.cpp.
  seam_manifest.json     machine-readable list (header -> functions -> import name, signature, owner).

    gen_seam.py --game GAME_ROOT --out OUT_DIR [--module game]
"""
import argparse
import json
import re
import sys
from pathlib import Path

OWNERS = {
    "disc": "services", "files": "services", "io": "services",
    "memory": "toolchain", "time": "services", "system": "services",
    "pads": "services", "saves": "services",
    "sound": "audio", "stream": "audio", "audio": "audio",
    "movie": "movie", "graphics": "graphics", "math": "graphics",
}

# Imports that are not Platform:: functions: the kernel.h shim's kputs and the EABI trampoline (abi.h).
EXTRA_IMPORTS = [
    # (import name, return wasm type, [(wasm type, name)], comment, runner_provided)
    ("ps2e_system_kputs", "void", [("u32", "text")], "kernel.h shim: kputs(const char*) -> host log", False),
    ("CallEabiTrampoline", "u64", [("u32", "function"), ("u32", "integers"), ("u32", "floats")],
     "abi.h CallEabiInteger: call table index `function` with 8 u64 integers + 8 f32 floats unpacked by its signature",
     True),
    ("CallEabiTrampolineF", "f32", [("u32", "function"), ("u32", "integers"), ("u32", "floats")],
     "abi.h CallEabiFloat (te_cxx.py rule R6 renames the second declaration's asm label)", True),
]

PRIMS_I32 = {"u32", "s32", "u16", "s16", "u8", "s8", "int", "unsigned", "char", "short", "bool", "size_t"}
WASM2C_TYPE = {"u32": "u32", "s32": "u32", "u64": "u64", "s64": "u64", "f32": "f32", "f64": "f64", "void": "void"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def snake(name):
    s = re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", name)
    s = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", s)
    return s.lower()


def find_namespace_blocks(text):
    """Yield (namespace, body) for every `namespace Platform::X { ... }` (depth-matched)."""
    for m in re.finditer(r"namespace\s+Platform::(\w+)\s*\{", text):
        depth = 1
        i = m.end()
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        yield m.group(1), text[m.end():i - 1]


def top_level_statements(body):
    """Split a namespace body into depth-0 statements (ending in ';' or a '{...}' block)."""
    out, depth, cur = [], 0, []
    for ch in body:
        cur.append(ch)
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                out.append("".join(cur))
                cur = []
        elif ch == ";" and depth == 0:
            out.append("".join(cur))
            cur = []
    return [" ".join(s.split()) for s in out if s.strip()]


def collect_types(text):
    enums = set(re.findall(r"\benum\s+(?:class\s+)?(\w+)", text))
    structs = set(re.findall(r"\bstruct\s+(\w+)\s*(?:\{|;)", text))
    return enums, structs


def split_params(s):
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur).strip())
    return [p for p in parts if p and p != "void"]


def parse_param(p, index):
    p = re.sub(r"=\s*[^,]+$", "", p).strip()  # default value
    # function pointer: `void (*present)(...)`
    m = re.match(r"^(.*?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$", p)
    if m:
        return {"text": p, "name": m.group(2), "type": p.replace(m.group(2), "", 1), "kind": "ptr"}
    # array: `const char name[6]`
    m = re.match(r"^(.*?)\b(\w+)\s*\[[^\]]*\]$", p)
    if m:
        return {"text": p, "name": m.group(2), "type": m.group(1).strip(), "kind": "ptr"}
    m = re.match(r"^(.*?)\b(\w+)$", p)
    if not m:
        return {"text": p, "name": f"a{index}", "type": p, "kind": None}
    type_, name = m.group(1).strip(), m.group(2)
    if type_ == "" or name in PRIMS_I32 or name in ("f32", "u64", "s64", "f64", "float", "double"):
        # unnamed parameter: the whole thing is the type
        type_, name = p, f"a{index}"
    return {"text": p, "name": name, "type": type_, "kind": None}


def classify(type_, enums, structs):
    t = type_.replace("const", "").replace("volatile", "").strip()
    if "*" in t or "&" in t:
        return "ptr"
    base = t.split()[-1] if t.split() else t
    if base in ("f32", "float"):
        return "f32"
    if base in ("f64", "double"):
        return "f64"
    if base in ("u64", "s64", "long"):
        return "u64" if base == "u64" else "s64"
    if base in ("bool",):
        return "bool"
    if base in PRIMS_I32 or base in enums or re.match(r"^(u|s)(8|16|32)$", base):
        return "enum" if base in enums else "i32"
    if base in structs or base[0].isupper():
        return "byval"
    return "i32"


def wasm_type_of(kind):
    return {"ptr": "u32", "f32": "f32", "f64": "f64", "u64": "u64", "s64": "s64", "bool": "s32", "enum": "s32",
            "i32": "s32", "byval": "u32", "void": "void"}[kind]


def parse_header(path):
    text = strip_comments(path.read_text())
    enums, structs = collect_types(text)
    fns = []
    for ns, body in find_namespace_blocks(text):
        for st in top_level_statements(body):
            if st.endswith("}"):
                # a struct/enum/inline body: harvest its types, skip
                continue
            if not st.endswith(";") or "(" not in st:
                continue
            head = st[:-1].strip()
            if re.match(r"^(enum|struct|class|constexpr|inline|template|using|typedef|static_assert|CHECK_|extern|friend)\b", head):
                continue
            if re.match(r"^[A-Z_]+\(", head):  # a macro statement (CHECK_SIZE(...))
                continue
            # trailing RETAIL(...) / RETAIL_N32(...) asm label
            label = None
            m = re.search(r"\s(RETAIL(?:_N32)?)\((\w+)\)$", head)
            if m:
                label = (m.group(1), m.group(2))
                head = head[:m.start()].strip()
            noreturn = "[[noreturn]]" in head
            head = head.replace("[[noreturn]]", "").strip()
            m = re.match(r"^(.*?)\b(\w+)\s*\((.*)\)\s*$", head, re.DOTALL)
            if not m:
                continue
            ret, name, params = m.group(1).strip(), m.group(2), m.group(3)
            if name in ("operator",):
                continue
            plist = [parse_param(p, i) for i, p in enumerate(split_params(params))]
            for p in plist:
                p["kind"] = p["kind"] or classify(p["type"], enums, structs)
            rkind = "void" if ret == "void" else classify(ret, enums, structs)
            fns.append({"ns": ns, "name": name, "ret": ret, "ret_kind": rkind, "params": plist, "label": label,
                        "noreturn": noreturn, "decl": st})
    return fns, enums, structs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--module", default="game")
    ap.add_argument("--wasm-side", default="memory",
                    help="comma list of headers implemented INSIDE the module (platform/nativeee/wasm/<h>.cpp): no imports")
    a = ap.parse_args()
    inc = Path(a.game) / "include" / "platform"
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    wasm_side = {x.strip() for x in a.wasm_side.split(",") if x.strip()}
    # tools/wasm_side_functions.txt: single Platform::X::Y functions of an imported header implemented INSIDE the module
    # (platform/nativeee/wasm/*.cpp) because their bodies must call game code (Stream, the heap, the disk manager):
    # no forwarder, no import, no stub for them.
    wasm_side_single = set()
    tsv_fns = Path(__file__).resolve().parent / "wasm_side_functions.txt"
    if tsv_fns.exists():
        for line in tsv_fns.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                wasm_side_single.add(line)
    # tools/platform_imports.tsv: host imports the wasm-side platform code calls (not Platform:: functions)
    tsv_imp = Path(__file__).resolve().parent / "platform_imports.tsv"
    if tsv_imp.exists():
        for line in tsv_imp.read_text().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            name, rt, params, comment = (line.split("\t") + ["", "", ""])[:4]
            plist = [tuple(x.split(":")) for x in params.split(",")] if params.strip() else []
            EXTRA_IMPORTS.append((name, rt, plist, comment, False))
    all_fns, headers = [], sorted(inc.glob("*.h"))
    seen = {}
    wasm_side_fns = []
    for h in headers:
        fns, _, _ = parse_header(h)
        if h.stem in wasm_side:
            for f in fns:
                f["header"] = h.name
            wasm_side_fns += fns
            continue
        for f in fns:
            f["header"] = h.name
            if f"{f['ns']}::{f['name']}" in wasm_side_single:
                wasm_side_fns.append(f)
                continue
            base = f"ps2e_{snake(f['ns'])}_{snake(f['name'])}"
            n = seen.get(base, 0) + 1
            seen[base] = n
            f["import"] = base if n == 1 else f"{base}_{n}"
            all_fns.append(f)

    # ---- the implicit second seam (tools/second_seam.tsv): plain symbols the game calls by retail name ----------------
    second = []
    tsv = Path(__file__).resolve().parent / "second_seam.tsv"
    for line in tsv.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        sym, imp, ret, params, owner, decl = line.split("\t")
        plist = [tuple(x.split(":")) for x in params.split(",")] if params.strip() else []
        second.append({"symbol": sym, "import": imp, "ret": ret, "params": plist, "owner": owner, "decl": decl})

    # ---- ps2e_seam.h -----------------------------------------------------------------------------------------------
    H = ["// GENERATED by tools/gen_seam.py from game/include/platform/*.h -- DO NOT EDIT.",
         "// The wasm module's import surface = the decomp's platform seam, scalarised: every pointer, reference and",
         "// by-value struct is a u32 GUEST ADDRESS (an offset into the engine's 32 MB MainMemory == wasm linear memory);",
         "// bool/enum/small ints are s32; f32 and u64 pass through. Struct returns become a leading u32 `out` address.",
         "#pragma once", "#include <stdint.h>",
         "#ifdef __cplusplus", "extern \"C\" {", "#endif",
         "typedef uint32_t ps2e_u32; typedef int32_t ps2e_s32; typedef uint64_t ps2e_u64; typedef int64_t ps2e_s64;",
         "typedef float ps2e_f32; typedef double ps2e_f64;", ""]
    cur = None
    for f in all_fns:
        if f["header"] != cur:
            cur = f["header"]
            H.append(f"// ---- {cur}  (group: {OWNERS.get(cur[:-2], '?')}) ----")
        ps = []
        if f["ret_kind"] == "byval":
            ps.append("ps2e_u32 out")
        for p in f["params"]:
            ps.append(f"ps2e_{wasm_type_of(p['kind'])} {p['name']}")
        rt = "void" if f["ret_kind"] in ("void", "byval") else f"ps2e_{wasm_type_of(f['ret_kind'])}"
        H.append(f"// Platform::{f['ns']}::{f['decl']}")
        H.append(f"{rt} {f['import']}({', '.join(ps) or 'void'});")
    H.append("")
    H.append("// ---- the implicit second seam (tools/second_seam.tsv): VU0 maths and renderer resources the game calls by retail name ----")
    for f in second:
        H.append(f"// {f['symbol']}: {f['decl']}  (group: {f['owner']})")
        rt = "void" if f["ret"] == "void" else "ps2e_u32" if f["ret"] in ("ptr", "u32") else "ps2e_" + f["ret"]
        H.append(f"{rt} {f['import']}({', '.join('ps2e_' + ('u32' if k == 'ptr' else k) + ' ' + n for k, n in f['params']) or 'void'});")
    H.append("")
    H.append("// ---- extra imports (kernel.h shim, abi.h trampoline) ----")
    for name, rt, ps, comment, runner in EXTRA_IMPORTS:
        H.append(f"// {comment}")
        H.append(f"{'void' if rt == 'void' else 'ps2e_' + rt} {name}({', '.join('ps2e_' + t + ' ' + n for t, n in ps) or 'void'});")
    H += ["#ifdef __cplusplus", "}", "#endif", ""]
    (out / "ps2e_seam.h").write_text("\n".join(H))

    # ---- seam_forwarders_<header>.cpp (wasm side), one per header so each sees ONLY its own header ---------------
    forwarder_files = []
    # TE_PROF_SEAM on the Vita's NATIVE lane (cmake --preset vita-native-release -DTE_PROF_SEAM=ON): there are no host
    # thunks there -- the forwarders call the ps2e_* bodies directly -- so the forwarders mark the game thread's profiler
    # section with the import's number, the number the wasm lane's thunks use (te_prof_seam_names' row, 1-based: the
    # Platform functions, the second seam, the thunked extras). Every other build: the marker is nothing.
    prof_ids = {}
    for i, name in enumerate([f["import"] for f in all_fns] + [f["import"] for f in second] +
                             [e[0] for e in EXTRA_IMPORTS if not e[4]]):
        prof_ids.setdefault(name, i + 1)
    PROF_PREAMBLE = [
        "#if defined(TE_NATIVE) && defined(TE_PROF_SEAM)",
        "extern \"C\" volatile unsigned short te_prof_section[];",
        "namespace {",
        "struct TeSeamSection {",
        "    unsigned short saved;",
        "    explicit TeSeamSection(unsigned short id) : saved(te_prof_section[0]) { te_prof_section[0] = id; }",
        "    ~TeSeamSection() { te_prof_section[0] = saved; }",
        "};",
        "}  // namespace",
        "#define TE_SEAM_SECTION(id) TeSeamSection te_seam_section_(id)",
        "#else",
        "#define TE_SEAM_SECTION(id) ((void)0)",
        "#endif", ""]
    for h in headers:
        if h.stem in wasm_side:
            continue
        F = ["// GENERATED by tools/gen_seam.py -- DO NOT EDIT. wasm side of the seam: Platform::* -> ps2e_* imports.",
             f"// One file per header ({h.name}); compiled to wasm32 and linked into the game module.",
             "#include \"common.h\"", f"#include \"platform/{h.name}\"", "#include \"ps2e_seam.h\"", ""] + PROF_PREAMBLE
        for f in [x for x in all_fns if x["header"] == h.name]:
            sig_params = ", ".join(p["text"] if re.search(r"\b%s\b" % re.escape(p["name"]), p["text"]) else f"{p['text']} {p['name']}" for p in f["params"])
            args, pre = [], []
            for p in f["params"]:
                k = p["kind"]
                if k == "ptr":
                    if "&" in p["text"] and "*" not in p["text"].split("&")[0]:
                        args.append(f"reinterpret_cast<ps2e_u32>(&{p['name']})")
                    else:
                        args.append(f"reinterpret_cast<ps2e_u32>({p['name']})")
                elif k == "byval":
                    pre.append(f"    auto te_copy_{p['name']} = {p['name']};")
                    args.append(f"reinterpret_cast<ps2e_u32>(&te_copy_{p['name']})")
                elif k in ("bool", "enum", "i32"):
                    args.append(f"static_cast<ps2e_s32>({p['name']})")
                elif k == "s64":
                    args.append(f"static_cast<ps2e_s64>({p['name']})")
                else:
                    args.append(p["name"])
            F.append(f"namespace Platform::{f['ns']}")
            F.append("{")
            F.append(f"{'[[noreturn]] ' if f['noreturn'] else ''}{f['ret']} {f['name']}({sig_params})")
            F.append("{")
            F.append(f"    TE_SEAM_SECTION({prof_ids[f['import']]});")
            F += pre
            rk = f["ret_kind"]
            call = f"{f['import']}({', '.join(args)})"
            if rk == "void":
                F.append(f"    {call};")
                if f["noreturn"]:
                    F.append("    __builtin_trap();")
            elif rk == "byval":
                F.append(f"    {f['ret']} te_out;")
                F.append(f"    {f['import']}({', '.join(['reinterpret_cast<ps2e_u32>(&te_out)'] + args)});")
                F.append("    return te_out;")
            elif rk == "ptr":
                F.append(f"    return reinterpret_cast<{f['ret']}>({call});")
            else:
                F.append(f"    return static_cast<{f['ret']}>({call});")
            F.append("}")
            F.append("}")
            F.append("")
        name = f"seam_forwarders_{h.stem}.cpp"
        (out / name).write_text("\n".join(F))
        forwarder_files.append(name)
    F = ["// GENERATED by tools/gen_seam.py from tools/second_seam.tsv -- DO NOT EDIT. wasm side of the implicit second seam:",
         "// each retail-named symbol the game calls is defined here with that asm label and forwards to its import.",
         "#include \"ps2e_seam.h\""] + PROF_PREAMBLE + ["extern \"C\" {"]
    for f in second:
        ct = {"ptr": "ps2e_u32", "u32": "ps2e_u32", "s32": "ps2e_s32", "f32": "ps2e_f32", "void": "void"}
        ps = ", ".join(f"{ct[k]} {n}" for k, n in f["params"])
        F.append(f"{ct[f['ret']]} te_second_{f['symbol']}({ps or 'void'}) asm(\"{f['symbol']}\");")
        F.append(f"{ct[f['ret']]} te_second_{f['symbol']}({ps or 'void'}) {{ TE_SEAM_SECTION({prof_ids[f['import']]}); "
                 f"{'return ' if f['ret'] != 'void' else ''}{f['import']}({', '.join(n for _, n in f['params'])}); }}")
    F += ["}", ""]
    (out / "seam_forwarders_second.cpp").write_text("\n".join(F))
    forwarder_files.append("seam_forwarders_second.cpp")
    (out / "seam_forwarders.txt").write_text("\n".join(forwarder_files) + "\n")

    # ---- seam_imports.txt ----------------------------------------------------------------------------------------
    imports = [f["import"] for f in all_fns] + [f["import"] for f in second] + [e[0] for e in EXTRA_IMPORTS]
    (out / "seam_imports.txt").write_text("\n".join(imports) + "\n")

    # ---- seam_host_thunks.c (host side, wasm2c glue) -------------------------------------------------------------
    mod = a.module
    T = ["// GENERATED by tools/gen_seam.py -- DO NOT EDIT. Host side: wasm2c import thunks -> ps2e_* (platform/nativeee).",
         f"#include \"{mod}.h\"", "#include \"ps2e_seam.h\"", "",
         "// TE_PROF_SEAM (a Vita profiling build: cmake -DTE_PROF_SEAM=ON): every thunk marks the game thread's section with",
         "// its import's number for the sampling profiler (platform/nativeee/host/te_prof.h, TE_PROF=1); otherwise the",
         "// markers compile to nothing (each thunk a plain branch).",
         "#if defined(TE_PROF_SEAM)",
         "extern volatile unsigned short te_prof_section[];",
         "#define TE_PROF_IN(id) const unsigned short te_prof_saved = te_prof_section[0]; te_prof_section[0] = (unsigned short)(id)",
         "#define TE_PROF_OUT() te_prof_section[0] = te_prof_saved",
         "#else",
         "#define TE_PROF_IN(id) (void)0",
         "#define TE_PROF_OUT() (void)0",
         "#endif", ""]
    def w2c_sig(rt_kind, params):
        ps = ["struct w2c_env* env"]
        for t, n in params:
            ps.append(f"{WASM2C_TYPE[t]} {n}")
        return ps
    prof_names = []
    def thunk_body(rt, call):
        # the import's number (1-based) is its row in te_prof_seam_names
        prof_names.append(call.split("(")[0])
        n = len(prof_names)
        if rt == "void":
            return f"    (void)env; TE_PROF_IN({n}); (void){call}; TE_PROF_OUT();"
        return f"    (void)env; TE_PROF_IN({n}); {rt} te_r = ({rt}){call}; TE_PROF_OUT(); return te_r;"
    for f in all_fns:
        params = []
        if f["ret_kind"] == "byval":
            params.append(("u32", "out"))
        params += [(wasm_type_of(p["kind"]), p["name"]) for p in f["params"]]
        rt = "void" if f["ret_kind"] in ("void", "byval") else WASM2C_TYPE[wasm_type_of(f["ret_kind"])]
        T.append(f"{rt} w2c_env_{f['import']}({', '.join(w2c_sig(f['ret_kind'], params))}) {{")
        T.append(thunk_body(rt, f"{f['import']}({', '.join(n for _, n in params)})"))
        T.append("}")
    for f in second:
        ps = [("u32" if k == "ptr" else k, n) for k, n in f["params"]]
        rt = "void" if f["ret"] == "void" else WASM2C_TYPE["u32" if f["ret"] == "ptr" else f["ret"]]
        T.append(f"{rt} w2c_env_{f['import']}({', '.join(w2c_sig(None, ps))}) {{")
        T.append(thunk_body(rt, f"{f['import']}({', '.join(n for _, n in ps)})"))
        T.append("}")
    T.append("")
    for name, rt, ps, comment, runner in EXTRA_IMPORTS:
        if runner:
            T.append(f"// {name}: provided by the runner (needs the instance/table), not thunked here.")
            continue
        T.append(f"{WASM2C_TYPE[rt] if rt != 'void' else 'void'} w2c_env_{name}({', '.join(w2c_sig(None, ps))}) {{")
        T.append(thunk_body(WASM2C_TYPE[rt] if rt != 'void' else 'void', f"{name}({', '.join(n for _, n in ps)})"))
        T.append("}")
    T.append("")
    # the names: the Vita's te_prof.cpp reads them for its TE_PROF report whether or not the markers are compiled in
    T.append("#if defined(TE_PROF_SEAM) || defined(__vita__)")
    T.append("const char* const te_prof_seam_names[] = {")
    T += [f"    \"{n}\"," for n in prof_names]
    T.append("};")
    T.append(f"const unsigned te_prof_seam_count = {len(prof_names)}u;")
    T.append("#endif")
    T.append("")
    (out / "seam_host_thunks.c").write_text("\n".join(T))
    # The same names alone, for a lane with no thunks (the Vita's native build of the game code, CMake TE_VITA_NATIVE: the
    # game calls the ps2e_* bodies directly; te_prof.cpp still names its seam sections from this table)
    N = ["// GENERATED by tools/gen_seam.py -- DO NOT EDIT. The seam's import names (te_prof.cpp), for the native lane.",
         "const char* const te_prof_seam_names[] = {"]
    N += [f"    \"{n}\"," for n in prof_names]
    N += ["};", f"const unsigned te_prof_seam_count = {len(prof_names)}u;", ""]
    (out / "seam_prof_names.c").write_text("\n".join(N))

    # ---- seam_stubs.cpp (host side, weak defaults) ---------------------------------------------------------------
    S = ["// GENERATED by tools/gen_seam.py -- DO NOT EDIT. WEAK default bodies for every ps2e_* import: an owner's",
         "// platform/nativeee/<header>.cpp defines the strong symbol and wins at link. A stub logs once and returns 0.",
         "#include \"ps2e_seam.h\"", "#include <cstdio>", "",
         "// COFF has no weak definitions (selectany is for data): a stub is `name_te_default` plus the linker's",
         "// /alternatename:name=name_te_default, used only while no strong `name` is linked (platform/nativeee/host/te_weak.h).",
         "#if defined(_MSC_VER)", "#define TE_WEAK", "#define TE_STUB(name) name##_te_default",
         "#define TE_STUB_END(name) __pragma(comment(linker, \"/alternatename:\" #name \"=\" #name \"_te_default\"))",
         "#else", "#define TE_WEAK __attribute__((weak))", "#define TE_STUB(name) name", "#define TE_STUB_END(name)", "#endif",
         "static void te_stub_once(const char* name, bool* said) { if (!*said) { *said = true; std::fprintf(stderr, \"[twinsanityevolved] UNIMPLEMENTED seam import: %s\\n\", name); } }",
         "extern \"C\" {"]
    for f in all_fns:
        ps = []
        if f["ret_kind"] == "byval":
            ps.append("ps2e_u32 out")
        ps += [f"ps2e_{wasm_type_of(p['kind'])} {p['name']}" for p in f["params"]]
        rt = "void" if f["ret_kind"] in ("void", "byval") else f"ps2e_{wasm_type_of(f['ret_kind'])}"
        S.append(f"TE_WEAK {rt} TE_STUB({f['import']})({', '.join(ps) or 'void'}) {{ static bool said; te_stub_once(\"{f['import']}\", &said);"
                 + (" " + " ".join(f"(void){p['name']};" for p in f["params"]) if f["params"] else "")
                 + (" (void)out;" if f["ret_kind"] == "byval" else "")
                 + (" return 0;" if rt != "void" else "") + f" }} TE_STUB_END({f['import']})")
    for f in second:
        ct = {"ptr": "ps2e_u32", "u32": "ps2e_u32", "s32": "ps2e_s32", "f32": "ps2e_f32", "void": "void"}
        ps = ", ".join(f"{ct[k]} {n}" for k, n in f["params"])
        S.append(f"TE_WEAK {ct[f['ret']]} TE_STUB({f['import']})({ps or 'void'}) {{ static bool said; te_stub_once(\"{f['import']}\", &said); "
                 + " ".join(f"(void){n};" for _, n in f["params"]) + (" return 0;" if f["ret"] != "void" else "") + f" }} TE_STUB_END({f['import']})")
    for name, rt, ps, comment, runner in EXTRA_IMPORTS:
        if runner:
            continue
        S.append(f"TE_WEAK {'void' if rt == 'void' else 'ps2e_' + rt} TE_STUB({name})({', '.join('ps2e_' + t + ' ' + n for t, n in ps)}) {{ static bool said; te_stub_once(\"{name}\", &said); "
                 + " ".join(f"(void){n};" for _, n in ps) + (" return 0;" if rt != "void" else "") + f" }} TE_STUB_END({name})")
    S += ["}", ""]
    (out / "seam_stubs.cpp").write_text("\n".join(S))

    # ---- manifest --------------------------------------------------------------------------------------------------
    man = {"headers": {}, "extra_imports": [e[0] for e in EXTRA_IMPORTS]}
    for f in all_fns:
        man["headers"].setdefault(f["header"], {"owner": OWNERS.get(f["header"][:-2], "?"), "functions": []})
        man["headers"][f["header"]]["functions"].append({
            "platform": f"Platform::{f['ns']}::{f['name']}", "import": f["import"], "decl": f["decl"],
            "wasm_signature": {"ret": wasm_type_of(f["ret_kind"]) if f["ret_kind"] != "byval" else "void(out u32)",
                               "params": [[wasm_type_of(p["kind"]), p["name"], p["type"]] for p in f["params"]]},
            "retail_label": f["label"]})
    man["second_seam"] = second
    man["wasm_side"] = {h: [f"Platform::{f['ns']}::{f['name']}" for f in wasm_side_fns if f["header"] == h]
                        for h in sorted({f["header"] for f in wasm_side_fns})}
    (out / "seam_manifest.json").write_text(json.dumps(man, indent=1))
    print(f"gen_seam: {len(all_fns)} Platform functions imported from {len(headers) - len(wasm_side)} headers "
          f"(+{len(wasm_side_fns)} wasm-side in {sorted(wasm_side)}) + {len(second)} second-seam + {len(EXTRA_IMPORTS)} extra imports -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
