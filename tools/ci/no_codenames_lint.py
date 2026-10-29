#!/usr/bin/env python3
"""tools/ci/no_codenames_lint.py -- NO DEVELOPER VOCABULARY WHERE A PLAYER READS.

The text a player reads must not carry the words of the people who build the game: build tools (cmake, ninja,
vcpkg, the compilers), languages (python, wasm), the toolchain, building itself, and the engine's code names
(NativeEE, ps2r, PS2E, recomp, decomp, seam, HLE). Zero findings or the gate fails.

What it reads -- the SURFACES table below (a new user-facing surface is one more row):
  cpp       the string literals of C++ sources ("..." and raw strings; comments and #include lines are not text).
            A literal that is not something a player reads (an environment variable, a JSON key, a file the program
            opens, a process argument) carries `// lint: not-ui` on its line.
  json      the values a player reads in a JSON file: the strings under the keys in USER_KEYS (and arrays of them).
  markdown  the sections of a Markdown file whose `## ` headings are listed (to the next `## `); fenced code is not
            prose and is skipped.

  tools/ci/no_codenames_lint.py [--root DIR] [--list]

Exit 0 = clean, 1 = findings (one line each: file:line: the word, the text).
"""
import argparse
import glob
import json
import os
import re
import sys

WORDS = re.compile(
    r"\b(cmake|ninja|python3?|wasm\w*|wasi\w*|tool ?chains?|vcpkg|clang\w*|msvc|gcc|compil(?:e|er|ers|es|ed|ing)|"
    r"build|builds|building|rebuild\w*|nativeee|ps2r(?!\.ini)\w*|ps2e\w*|recomp\w*|decomp\w*|vitasdk|seams?|hle)\b",
    re.IGNORECASE)
NOT_UI = "lint: not-ui"
USER_KEYS = {"label", "short", "help", "description", "note", "name", "title", "plays_note", "product", "author", "text"}

# (kind, glob relative to the root, extra) -- extra = the README headings for markdown
SURFACES = [
    ("cpp", "engine/app/launcher/*.cpp", None),
    ("cpp", "engine/app/host/mod_import/*.cpp", None),   # the mod importer's messages (the launcher's Mods page, the mod tool)
    ("cpp", "platform/nativeee/host/mods_page.cpp", None),   # the game's Options > MODS page: the notes under a mod's row
    ("cpp", "platform/nativeee/host/language.cpp", None),    # the game's GAME OPTIONS language rows: their titles and words
    # the disc identity's and the region table's sentences: the launcher's cards and the game's refusals show them
    ("cpp", "engine/app/host/disc_identity.cpp", None),
    ("cpp", "engine/app/host/disc_regions.cpp", None),
    ("json", "launcher/title.json", None),
    ("markdown", "README.md", ["## What's new", "## Play: download the game", "## The launcher", "## Mods", "## Discs",
                               "## Arabic"]),
    # the release notes: the draft release's body (release.yml reads the file)
    ("markdown", "RELEASE-NOTES.txt", ["## Install", "## Play", "## Discs", "## Mods", "## Languages", "## PS Vita", "## Fixes"]),
]


def cpp_literals(text):
    """(line, literal) for every string literal outside comments and #include lines."""
    out = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            i = j
        elif c == "#" and (i == 0 or text[i - 1] == "\n" or text[:i].rsplit("\n", 1)[-1].strip() == ""):
            j = text.find("\n", i)   # a preprocessor line (#include "x.h", #pragma): not text
            i = n if j < 0 else j
        elif c == "R" and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            if not m:
                i += 1
                continue
            end = text.find(")" + m.group(1) + '"', i + m.end())
            end = n if end < 0 else end
            out.append((line, text[i + m.end():end]))
            line += text.count("\n", i, end)
            i = end + len(m.group(1)) + 2
        elif c == '"':
            j = i + 1
            buf = []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                if text[j] == "\n":
                    break
                buf.append(text[j])
                j += 1
            out.append((line, "".join(buf)))
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        else:
            i += 1
    return out


def check_cpp(path, rel, findings):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    lines = text.split("\n")
    for line, lit in cpp_literals(text):
        if NOT_UI in lines[line - 1]:
            continue
        for m in WORDS.finditer(lit):
            findings.append((rel, line, m.group(0), lit))


def json_strings(value, key=None, path="", out=None):
    if out is None:
        out = []
    if isinstance(value, dict):
        for k, v in value.items():
            json_strings(v, k, path + "/" + k, out)
    elif isinstance(value, list):
        for i, v in enumerate(value):
            json_strings(v, key, "%s[%d]" % (path, i), out)
    elif isinstance(value, str) and key in USER_KEYS:
        out.append((path, value))
    return out


def check_json(path, rel, findings):
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    raw = open(path, encoding="utf-8").read().split("\n")
    for where, text in json_strings(doc):
        line = next((i + 1 for i, l in enumerate(raw) if json.dumps(text)[1:-1][:40] in l), 0)
        for m in WORDS.finditer(text):
            findings.append((rel + " " + where, line, m.group(0), text))


def check_markdown(path, rel, headings, findings):
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    inside, fenced, seen = False, False, set()
    for i, l in enumerate(lines, 1):
        if l.startswith("## "):
            inside = any(l.startswith(h) for h in headings)
            if inside:
                seen.add(next(h for h in headings if l.startswith(h)))
            fenced = False
            continue
        if not inside:
            continue
        if l.strip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        for m in WORDS.finditer(l):
            findings.append((rel, i, m.group(0), l.strip()))
    for h in headings:
        if h not in seen:
            findings.append((rel, 0, "(missing section)", "the user section '%s' is not in the file" % h))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")))
    ap.add_argument("--list", action="store_true", help="list the files read and exit")
    a = ap.parse_args()
    findings, files = [], []
    for kind, pattern, extra in SURFACES:
        for path in sorted(glob.glob(os.path.join(a.root, pattern))):
            rel = os.path.relpath(path, a.root)
            files.append(rel)
            if a.list:
                continue
            if kind == "cpp":
                check_cpp(path, rel, findings)
            elif kind == "json":
                check_json(path, rel, findings)
            elif kind == "markdown":
                check_markdown(path, rel, extra, findings)
    if a.list:
        print("\n".join(files))
        return 0
    for rel, line, word, text in findings:
        print("%s:%d: '%s' in: %s" % (rel, line, word, text[:160]))
    print("no_codenames_lint: %d files, %d findings -> %s" % (len(files), len(findings), "FAIL" if findings else "PASS"))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
