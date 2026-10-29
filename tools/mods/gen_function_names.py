#!/usr/bin/env python3
"""gen_function_names.py -- the PAL executable's functions by address, for the mod importer's code-change report.

A mod's SLES_525.68 whose .text differs is listed BY FUNCTION ("FUN_001ac8d0 Font::ConstructBase"): the importer maps every
changed code address to the function that holds it. This script (run at BUILD time; nothing of it runs in the shipped
programs) writes that table as C++ from the decomp's own sources -- addresses and names only, no game data:

  * game/symbol_addrs.txt: every `NAME = 0xADDRESS; // type:func` row (the function starts of the PAL executable);
  * game/include/**/*.h and game/src/**/*.cpp: the decomp's C++ name for a retail function, from its `RETAIL(NAME)` /
    `RETAIL_N32(NAME)` asm label on the declaration -- `static Font* ConstructBase(Font*) RETAIL(FUN_001ac8d0);` inside
    `class Font` gives `Font::ConstructBase` (the enclosing class / struct / union / namespace scopes joined by `::`).

    gen_function_names.py --game GAME_ROOT --out function_names.cpp [--json report.json]

The C++ defines te::modimport::kFunctionNames[] = {address, symbol, C++ name or ""} sorted by address, and
kFunctionNameCount, and kCodeStart / kCodeEnd (the .text range the table covers: the first function's address and the
last data-image address the retail layout gives .text, tools/gen_retail_image.py).
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

SYMBOL = re.compile(r"(\w+)\s*=\s*0x([0-9A-Fa-f]+);\s*(?://\s*(.*))?")
RETAIL = re.compile(r"\bRETAIL(?:_N32)?\s*\(\s*([A-Za-z_]\w*)\s*\)")
SCOPE = re.compile(r"\b(class|struct|union|namespace)\s+([A-Za-z_]\w*)\b[^;{}()]*$", re.S)
ANON_NAMESPACE = re.compile(r"\bnamespace\s*$")
IDENT_BEFORE = re.compile(r"((?:[A-Za-z_]\w*\s*::\s*)*(?:operator\s*[^\s(]+|~?[A-Za-z_]\w*))\s*$")
TEXT_END = 0x2D9D88   # .text's end in the PAL executable (its section headers; tools/gen_retail_image.py's .vutext starts at 0x2D9D90)


def strip(text):
    """Comments and string / character literals blanked (newlines kept), so braces and parentheses are code's only."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == "R" and i + 2 < n and text[i + 1] == '"':
            # a raw string R"delim( ... )delim"
            k = text.find("(", i + 2)
            delim = text[i + 2:k] if k >= 0 else ""
            end = text.find(")" + delim + '"', k + 1) if k >= 0 else -1
            j = n if end < 0 else end + len(delim) + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def cxx_names(path):
    """{retail symbol: C++ qualified name} for the functions declared with RETAIL(...) in one file."""
    text = strip(path.read_text(errors="replace"))
    names = {}
    scopes = []          # a name per open brace (None = not a naming scope)
    stmt_start = 0       # where the current declaration began (after the last ; { or })
    i, n = 0, len(text)
    events = sorted([(m.start(), "retail", m) for m in RETAIL.finditer(text)] +
                    [(k, ch, None) for k, ch in enumerate(text) if ch in "{};"], key=lambda e: e[0])
    for pos, kind, m in events:
        if kind == "{":
            head = text[stmt_start:pos]
            sm = SCOPE.search(head)
            if sm and "=" not in head[sm.end(1):]:
                scopes.append(sm.group(2))
            elif ANON_NAMESPACE.search(head):
                scopes.append(None)
            else:
                scopes.append(None)
            stmt_start = pos + 1
        elif kind == "}":
            if scopes:
                scopes.pop()
            stmt_start = pos + 1
        elif kind == ";":
            stmt_start = pos + 1
        else:
            decl = text[stmt_start:pos]
            # a function: the declaration ends with its parameter list (and qualifiers) before the label
            body = decl.rstrip()
            body = re.sub(r"(\s*\b(const|volatile|noexcept|override|final|throw\s*\(\s*\))\b)*\s*$", "", body)
            if not body.endswith(")"):
                continue
            depth, k = 0, len(body) - 1
            while k >= 0:
                if body[k] == ")":
                    depth += 1
                elif body[k] == "(":
                    depth -= 1
                    if depth == 0:
                        break
                k -= 1
            if k <= 0:
                continue
            im = IDENT_BEFORE.search(body[:k])
            if not im:
                continue
            ident = re.sub(r"\s+", "", im.group(1))
            if ident in ("if", "while", "for", "switch", "return", "sizeof", "decltype", "alignof", "RETAIL"):
                continue
            prefix = "::".join(s for s in scopes if s)
            qualified = ident if "::" in ident or not prefix else prefix + "::" + ident
            names.setdefault(m.group(1), qualified)
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    game = Path(a.game)
    funcs = {}
    for line in (game / "symbol_addrs.txt").read_text().splitlines():
        m = SYMBOL.match(line.strip())
        if m and "type:func" in (m.group(3) or ""):
            funcs.setdefault(int(m.group(2), 16), m.group(1))
    cxx = {}
    files = sorted(list((game / "include").rglob("*.h")) + list((game / "src").rglob("*.cpp")) + list((game / "src").rglob("*.h")))
    for f in files:
        for sym, name in cxx_names(f).items():
            cxx.setdefault(sym, name)
    rows = sorted(funcs.items())
    named = 0
    lines = ["// GENERATED by tools/mods/gen_function_names.py from game/symbol_addrs.txt and the decomp's RETAIL() labels -- DO NOT EDIT.",
             "// The PAL executable's function starts (addresses and names only) for the mod importer's code-change report.",
             '#include "app/host/mod_import/function_names.h"', "", "namespace te::modimport {", "",
             "const FunctionName kFunctionNames[] = {"]
    for addr, sym in rows:
        name = cxx.get(sym, "")
        named += 1 if name else 0
        lines.append('    {0x%08X, "%s", "%s"},' % (addr, sym, name))
    lines += ["};", "const size_t kFunctionNameCount = sizeof(kFunctionNames) / sizeof(kFunctionNames[0]);",
              "const uint32_t kCodeStart = 0x%08X;" % (rows[0][0] if rows else 0),
              "const uint32_t kCodeEnd = 0x%08X;" % TEXT_END, "", "}  // namespace te::modimport", ""]
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines)
    if not out.exists() or out.read_text() != text:
        out.write_text(text)
    if a.json:
        Path(a.json).write_text(json.dumps({"functions": len(rows), "with_cxx_names": named, "files_scanned": len(files)}, indent=1))
    print("gen_function_names: %d functions, %d with the decomp's C++ name -> %s" % (len(rows), named, a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
