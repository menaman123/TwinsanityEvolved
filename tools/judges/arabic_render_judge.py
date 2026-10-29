#!/usr/bin/env python3
"""arabic_render_judge.py -- is Arabic CHOSEN in the game's options and DRAWN right: shaped, right to left, inside its box, with
no glyph missing?

The Arabic language pack is in the mods folder (mods/arabic: a language pack is not a mod -- present = offered), and the
player picks it: fixtures/judges/arabic_render_pal.json names the runs, each deterministic headless with a settings file the
judge makes, the text census on (TE_GFX_DIAG_TEXT=1: a [te.text.shape] line per shaped text -- its glyphs with their font and
codepoint, each line's span, the text's box -- beside the [te.gfx.diag] line of every glyph-quad call) and frames dumped:

  pick        English at the start; the route (fixtures/padlogs/language_pick_arabic.padlog) goes title -> OPTIONS -> GAME
              OPTIONS and steps TEXT LANGUAGE once LEFT (english wraps to the last text language: arabic): the page redraws in
              Arabic (its rows titled by the pack's own words), OPTIONS too after TRIANGLE; the settings file then holds
              game.text_language = arabic (and no mods.arabic.enabled);
  next_start  the same settings file, no language named: the boot is in Arabic -- the memory card notice, the intro's bottom
              line "three years ago...", OPTIONS and GAME OPTIONS on the language-table route stopped before its first step.

On each fixture frame GREEN iff:

  1 FORMS     every shaped text's glyphs are exactly the contextual forms, in display order, that the language's rules give its
              string (tools/text/shape.py: the same rules the unit test te_text_shape_test pins), Arabic glyphs from the Arabic
              font and everything else from the game's;
  2 ORDER     the fixture's texts are drawn on the frame (by their bank number), and the language row reads "arabic (text)";
  3 ALIGNED   each line sits where its alignment puts it inside the text's box: a left-aligned (start) text's lines end at the
              box's right edge, a right-aligned one's start at its left edge, a centred one's are centred on the place;
  4 INSIDE    every glyph is inside its line and the box, every glyph-quad call of the text inside the box;
  5 GLYPHS    no glyph is missing (neither font has it) in any shaped text of the whole run;
and the frame shows the text (lettering-coloured pixels inside the boxes). The frames go to --out as PNG for the report.

RED on the base runner (no language mods: the run cannot even name the mod, nothing is shaped), GREEN on the Arabic change's runner.
2026-10-27: RED on 43e9989 (the pack is a mod there: without its switch the LEFT step reaches
"spanish (english text)", nothing is shaped), GREEN on its change.

Usage: arabic_render_judge.py --bin BIN --iso ISO [--fixture F] [--out DIR]
Exit 0 = GREEN, 1 = RED, 2 = the run failed for another reason.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "arabic_render_pal.json")
sys.path.insert(0, os.path.join(ROOT, "tools", "text"))

SHAPE = re.compile(rb"^\[te\.text\.shape\] present (\d+) rtl (\d) align (0x[0-9a-f]+) anchor \(([-0-9.]+) ([-0-9.]+)\) box \(([-0-9.]+) ([-0-9.]+)\)-\(([-0-9.]+) ([-0-9.]+)\) "
                   rb"lines (\d+) glyphs (\d+) missing (\d+)(.*) \| frame (\d+)x(\d+) string '(.*)'$")
LINE = re.compile(rb" \| line (\d+) \(([-0-9.]+) ([-0-9.]+)\)-\(([-0-9.]+) ([-0-9.]+)\):((?: [ag]\d+:[0-9A-F]{4}@[-0-9.]+-[-0-9.]+)*)")
GLYPH = re.compile(rb"([ag])(\d+):([0-9A-F]{4})@([-0-9.]+)-([-0-9.]+)")
QUADS = re.compile(rb"^\[te\.gfx\.diag\] present (\d+) text (\d+) quads .* box \(([-0-9.]+) ([-0-9.]+)\)-\(([-0-9.]+) ([-0-9.]+)\) frame \d+x\d+ at .* string '(.*)'$")


def supports(binary, flag):
    """Does the runner's usage name `flag`? (headless: a runner without a disc image would ask for one otherwise)"""
    try:
        r = subprocess.run([binary, "--help"], capture_output=True, text=True, timeout=60, stdin=subprocess.DEVNULL,
                           env=dict(os.environ, TE_GFX_HEADLESS="1"))
    except Exception:
        return False
    return flag in (r.stderr + r.stdout)


def parse(log):
    shaped, quads = [], []
    with open(log, "rb") as f:
        for raw in f:
            raw = raw.rstrip(b"\n")
            m = SHAPE.match(raw)
            if m:
                lines = []
                for lm in LINE.finditer(m.group(13)):
                    glyphs = [{"font": g.group(1).decode(), "index": int(g.group(2)), "cp": int(g.group(3), 16), "x0": float(g.group(4)),
                               "x1": float(g.group(5))} for g in GLYPH.finditer(lm.group(6))]
                    lines.append({"x0": float(lm.group(2)), "y0": float(lm.group(3)), "x1": float(lm.group(4)), "y1": float(lm.group(5)),
                                  "glyphs": glyphs})
                shaped.append({"present": int(m.group(1)), "rtl": int(m.group(2)), "align": int(m.group(3), 16),
                               "anchor": (float(m.group(4)), float(m.group(5))),
                               "box": (float(m.group(6)), float(m.group(7)), float(m.group(8)), float(m.group(9))),
                               "lines": lines, "line_count": int(m.group(10)), "glyph_count": int(m.group(11)),
                               "missing": int(m.group(12)), "string": m.group(16)})
                continue
            m = QUADS.match(raw)
            if m:
                quads.append({"present": int(m.group(1)), "box": tuple(float(m.group(i)) for i in range(3, 7)), "string": m.group(7)})
    return shaped, quads


def bank_ids(bank_path):
    """{text bytes: id} of the shipped bank (our strings)."""
    import bank as B
    out = {}
    data = open(bank_path, "rb").read()
    section, n = None, 0
    for raw in re.split(rb"\r\n|\r|\n", data):
        if not raw:
            continue
        if raw.startswith(b"@@ "):
            section, n = raw.split(b" ")[1].decode(), 0
            continue
        out.setdefault(raw, B.text_id(section, n))
        n += 1
    return out


def expected(string):
    import shape as S
    s = S.shape_text(string)
    return [[cp for cp, _, _ in line] for k, line in enumerate(s["lines"]) if k not in s["empty"]]


def check_text(t, quads, tol):
    """[problems] of one shaped text: forms and order, alignment, glyphs and quads inside the box."""
    import shape as S
    p = []
    want = expected(t["string"])
    got = [[g["cp"] for g in line["glyphs"]] for line in t["lines"]]
    if got != want:
        p.append("FORMS: drawn %s, the rules give %s" % ([" ".join("%04X" % c for c in l) for l in got], [" ".join("%04X" % c for c in l) for l in want]))
    for line in t["lines"]:
        for g in line["glyphs"]:
            arabic = 0x0600 <= g["cp"] <= 0x06FF or 0xFE70 <= g["cp"] <= 0xFEFF
            if arabic and g["font"] != "a":
                p.append("FORMS: U+%04X drawn from the game's font (a Latin fallback)" % g["cp"])
    x0, y0, x1, y1 = t["box"]
    left, right = (t["align"] & 0x10) != 0, (t["align"] & 0x40) != 0
    for k, line in enumerate(t["lines"]):
        if t["rtl"]:
            if left and abs(line["x1"] - x1) > tol:
                p.append("ALIGNED: line %d ends at %.1f, the box's right edge is %.1f (a start-aligned right-to-left line)" % (k, line["x1"], x1))
            if right and not left and abs(line["x0"] - x0) > tol:
                p.append("ALIGNED: line %d starts at %.1f, the box's left edge is %.1f" % (k, line["x0"], x0))
        if not left and not right and abs((line["x0"] + line["x1"]) / 2 - t["anchor"][0]) > tol:
            p.append("ALIGNED: line %d is centred on %.1f, the place is %.1f" % (k, (line["x0"] + line["x1"]) / 2, t["anchor"][0]))
        if line["x0"] < x0 - tol or line["x1"] > x1 + tol or line["y0"] < y0 - tol or line["y1"] > y1 + tol:
            p.append("INSIDE: line %d (%.1f..%.1f) is outside the box (%.1f..%.1f)" % (k, line["x0"], line["x1"], x0, x1))
        prev = line["x0"]
        for g in line["glyphs"]:
            if abs(g["x0"] - prev) > tol or g["x1"] < g["x0"] - tol:
                p.append("INSIDE: a glyph of line %d at %.1f-%.1f does not follow the one before (%.1f)" % (k, g["x0"], g["x1"], prev))
            if g["x0"] < x0 - tol or g["x1"] > x1 + tol:
                p.append("INSIDE: a glyph of line %d (%.1f-%.1f) is outside the box" % (k, g["x0"], g["x1"]))
            prev = g["x1"]
    for q in quads:
        qx0, qy0, qx1, qy1 = q["box"]
        if qx0 < x0 - tol or qx1 > x1 + tol or qy0 < y0 - tol or qy1 > y1 + tol:
            p.append("INSIDE: glyph quads (%.1f %.1f)-(%.1f %.1f) outside the text's box (%.1f %.1f)-(%.1f %.1f)" % (qx0, qy0, qx1, qy1, x0, y0, x1, y1))
    if t["missing"]:
        p.append("GLYPHS: %d glyph(s) missing" % t["missing"])
    del S
    return p


def read_ppm(path):
    data = open(path, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos:pos + 3 * w * h]


def lettering_pixels(path, boxes):
    """Red-dominant pixels (the lettering's gradient) inside the boxes."""
    w, h, px = read_ppm(path)
    n = 0
    for x0, y0, x1, y1 in boxes:
        for y in range(max(0, int(y0)), min(h, int(y1) + 1)):
            for x in range(max(0, int(x0)), min(w, int(x1) + 1)):
                r, g, b = px[(y * w + x) * 3:(y * w + x) * 3 + 3]
                if r > 110 and r - b > 60:
                    n += 1
    return n


def judge_frames(name, frames, shaped, quads, dump, ids, tol, out, report, fails):
    from PIL import Image
    for fname, frame in frames.items():
        present = frame["present"]
        texts = [t for t in shaped if t["present"] == present]
        problems = []
        drawn_ids = sorted({ids.get(t["string"], "?") for t in texts})
        for want in frame["texts"]:
            if want not in drawn_ids:
                problems.append("ORDER: %s is not drawn on present %d (drawn: %s)" % (want, present, drawn_ids))
        if frame.get("row"):
            rows = [t for t in texts if frame["row"].encode() in t["string"]]
            if not rows:
                problems.append("ORDER: no text on present %d reads %r" % (present, frame["row"]))
        # a row titled in the pack's own words ("<title> : <value>": the bank's Port texts)
        bank_text = {v: k for k, v in ids.items()}
        for want in frame.get("titled", []):
            title = bank_text.get(want)
            if title is None or not any(t["string"].startswith(title + b" : ") for t in texts):
                problems.append("TITLED: no row on present %d is titled %s" % (present, want))
        for t in texts:
            q = [x for x in quads if x["present"] == present and x["string"] == t["string"]]
            problems += ["%s: %s" % (ids.get(t["string"], t["string"][:24].decode("cp1256", "replace")), x) for x in check_text(t, q, tol)]
        path = os.path.join(dump, "frame_%06d.ppm" % present)
        lit = 0
        if os.path.exists(path):
            lit = lettering_pixels(path, [t["box"] for t in texts])
            Image.open(path).save(os.path.join(out, "%s_%s_%06d.png" % (name, fname, present)))
            if lit < frame["min_lettering_pixels"]:
                problems.append("the frame shows %d lettering pixels in the texts' boxes < %d" % (lit, frame["min_lettering_pixels"]))
        else:
            problems.append("no frame dumped at present %d" % present)
        report["frames"]["%s/%s" % (name, fname)] = {"present": present, "shaped_texts": len(texts), "ids": drawn_ids, "lettering_pixels": lit,
                                                    "verdict": "green" if not problems else "red", "problems": problems}
        print(json.dumps({"frame": "%s/%s" % (name, fname), **report["frames"]["%s/%s" % (name, fname)]}, ensure_ascii=False)[:1500])
        fails += ["%s/%s: %s" % (name, fname, x) for x in problems]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "arabic_render_judge"))
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    shutil.rmtree(a.out, ignore_errors=True)
    os.makedirs(a.out)
    fails = []
    if not supports(a.bin, "--mods-dir"):
        fails.append("the runner has no mods folder (--mods-dir): the language pack cannot be offered")
    ini = os.path.join(a.out, "ps2r.ini")
    with open(ini, "w", newline="\n") as f:
        f.write("# the arabic render judge's settings file\n" + "".join(x + "\n" for x in fx.get("settings", [])))
    ids = bank_ids(os.path.join(ROOT, fx["bank"]))
    report = {"bin": a.bin, "runs": {}, "frames": {}}
    total_shaped = 0
    for name, r in fx["runs"].items():
        dump, mc = os.path.join(a.out, name, "dump"), os.path.join(a.out, name, "mc")
        os.makedirs(dump)
        os.makedirs(mc)
        log = os.path.join(a.out, name, "run.log")
        args = [a.bin, "--iso", a.iso, "--config", ini, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
                "--play-pad", os.path.join(ROOT, r["padlog"]), "--stop-at", str(r["stop_at"]), "--mc-dir", mc,
                "--run-seconds", str(fx.get("run_seconds", 300))] + fx["runner_args"] + r.get("args", [])
        env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_DIAG_TEXT="1", TE_GFX_DUMP_DIR=dump, TE_GFX_DUMP_EVERY=str(fx["dump_every"]))
        print("arabic_render_judge: run %s: %s" % (name, " ".join(args[1:])), flush=True)
        with open(log, "w") as f:
            rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
        print("arabic_render_judge: run %s exit %d -> %s" % (name, rc, log), flush=True)
        if rc != 0:
            fails.append("%s: the run exited %d" % (name, rc))
        shaped, quads = parse(log)
        total_shaped += len(shaped)
        missing_runs = [t for t in shaped if t["missing"]]
        if missing_runs:
            fails.append("%s: GLYPHS: %d shaped text(s) in the run with a missing glyph (first: %r)" % (name, len(missing_runs), missing_runs[0]["string"][:40]))
        if not shaped:
            fails.append("%s: no text was shaped in the whole run (no Arabic drawn)" % name)
        with open(ini) as f:
            ini_text = f.read()
        run_report = {"exit": rc, "shaped_texts": len(shaped), "ini_after": ini_text}
        for line in r.get("ini_must_hold", []):
            if line not in ini_text.splitlines():
                fails.append("%s: the settings file does not hold %r after the run" % (name, line))
        for key in r.get("ini_must_not_name", []):
            if any(l.strip().startswith(key) for l in ini_text.splitlines()):
                fails.append("%s: the settings file still names %s" % (name, key))
        report["runs"][name] = run_report
        judge_frames(name, r["frames"], shaped, quads, dump, ids, fx["tolerance_px"], a.out, report, fails)
    report["verdict"] = "GREEN" if not fails else "RED"
    report["reasons"] = fails
    with open(os.path.join(a.out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    for f in fails[:20]:
        print("  RED: %s" % f)
    print("arabic_render_judge: %s (%d shaped texts in the runs)" % (report["verdict"], total_shaped))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
