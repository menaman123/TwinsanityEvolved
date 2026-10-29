#!/usr/bin/env python3
"""w2c_callgraph.py W2C_DIR [FUNC ...] -- the transitive call graph of wasm2c functions (game_*.c):
direct calls, indirect calls (CALL_INDIRECT / te_adapt), imports (w2c_env_*: the seam), per function and closure."""
import re
import sys
from pathlib import Path

RE_DEF = re.compile(r'^(?:static )?(?:u32|u64|f32|f64|void) (w2c_game_\w+)\(w2c_game\* instance[^)]*\) \{$')
RE_CALL = re.compile(r'\b(w2c_game_\w+)\(instance')
RE_IMP = re.compile(r'\b(w2c_env_\w+)\(')
RE_IND = re.compile(r'CALL_INDIRECT|te_adapt_\w+|func_types_eq')


def load(w2c):
    funcs = {}
    for part in sorted(Path(w2c).glob("game_*.c")):
        cur = None
        body = []
        for line in part.read_text(errors="replace").splitlines():
            if cur is None:
                m = RE_DEF.match(line)
                if m:
                    cur = m.group(1)
                    body = []
                continue
            if line == "}":
                text = "\n".join(body)
                funcs[cur] = {
                    "part": part.name,
                    "lines": len(body),
                    "calls": sorted(set(RE_CALL.findall(text)) - {cur}),
                    "self": cur in RE_CALL.findall(text),
                    "imports": sorted(set(RE_IMP.findall(text)) - {"w2c_env_memory"}),
                    "indirect": len(RE_IND.findall(text)),
                }
                cur = None
                continue
            body.append(line)
    return funcs


def closure(funcs, root):
    seen, stack = set(), [root]
    while stack:
        f = stack.pop()
        if f in seen or f not in funcs:
            continue
        seen.add(f)
        stack += funcs[f]["calls"]
    return seen


def main():
    w2c = sys.argv[1]
    funcs = load(w2c)
    names = sys.argv[2:]
    for n in names:
        full = n if n.startswith("w2c_game_") else "w2c_game_" + n
        if full not in funcs:
            cands = [k for k in funcs if n in k]
            print(f"== {n}: not found; candidates {cands[:8]}")
            continue
        c = closure(funcs, full)
        imps = sorted({i for f in c for i in funcs[f]["imports"]})
        ind = sum(funcs[f]["indirect"] for f in c)
        lines = sum(funcs[f]["lines"] for f in c)
        f = funcs[full]
        print(f"== {full} ({f['part']}, {f['lines']} lines, recursive={f['self']}): direct callees {len(f['calls'])}, "
              f"closure {len(c)} functions / {lines} lines, imports {len(imps)}, indirect sites {ind}")
        for g in sorted(c):
            if g == full:
                continue
            h = funcs[g]
            print(f"     {g[9:]:60s} {h['lines']:5d} lines  imports={','.join(i[8:] for i in h['imports'])[:90]} ind={h['indirect']}")
        if imps:
            print("   IMPORTS:", ", ".join(i[8:] for i in imps))


if __name__ == "__main__":
    main()
