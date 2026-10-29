#!/usr/bin/env python3
"""arabic_length_judge.py -- does EVERY translated text fit the box the game gives it?

The fixture (fixtures/judges/arabic_length_pal.json) names the translation (tools/text/arabic.tsv), the shipped bank and font,
and the thresholds. GREEN iff:

  A  FIT       the translation tools' check over the disc's English (tools/text/translate.py --check: the box of a text is the
               widest English text the game draws in the same place, capped by the screen; tools/text/fit.py) finds every
               text present, current (its sha1 is the disc's English's), with its placeholders, encodable, and fitting:
               unfitting <= the fixture's max_unfitting (0);
  B  BANK      the shipped bank is byte for byte what the TSV builds (nothing edited by hand, nothing stale);
  C  SHAPER    the game's own shaper (te_text_shape_test --dump, the text layer's C++ built natively) shapes every text of the
               bank exactly as the tools measured it (tools/text/shape.py): the widths the fit used are the widths drawn;
  D  ENGINE    the runner (--bin) offers the language: its language-packs line has the arabic row with all of the bank's texts
               ("made from this disc's English": the bank's sha256 keys match the disc).
2026-10-27: the bank carries the port's two row titles too (its Port section, tools/text/bank.py PORT_TEXTS): A fits them as
menu rows, B builds them, C shapes them; the pack is offered by its presence (no --mod).

RED before the fitting pass (the first translation pass's TSV: 28 texts wider than their boxes) and on a runner without
language mods; GREEN on the shipped translation and the Arabic change's runner.

Usage: arabic_length_judge.py --bin BIN --iso ISO [--tsv TSV] [--fixture F] [--out DIR]
Exit 0 = GREEN, 1 = RED, 2 = could not run.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "arabic_length_pal.json")
sys.path.insert(0, os.path.join(ROOT, "tools", "text"))
sys.path.insert(0, os.path.join(ROOT, "tools", "font"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--tsv", help="the translation to judge (default: the fixture's)")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "arabic_length_judge"))
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    os.makedirs(a.out, exist_ok=True)
    import bank as B
    import fit as F
    import shape as S
    import translate as T
    from twin_disc import Disc

    tsv_path = a.tsv or os.path.join(ROOT, fx["tsv"])
    bank_path = os.path.join(ROOT, fx["bank"])
    font_path = os.path.join(ROOT, fx["font"])
    fails = []
    disc = Disc(a.iso)
    english, slot = B.english_texts(disc)
    m = F.Metrics(disc, font_path)
    rooms = F.rooms(m, english)
    tsv = B.read_tsv(tsv_path)

    # A -- the fit
    args = argparse.Namespace(language=fx["language"])
    report = T.check(args, english, m, rooms, tsv, quiet=True)
    counts = {k: len(report[k]) for k in ("missing", "stale", "tokens", "encoding", "unfitting")}
    print("arabic_length_judge: A fit: %d texts, %d ok, %s; rooms %s" % (report["texts"], report["ok"], counts,
                                                                       {k: round(v) for k, v in rooms.items()}))
    if report["texts"] != fx["texts"]:
        fails.append("the disc's English has %d texts, the fixture %d" % (report["texts"], fx["texts"]))
    for k in ("missing", "stale", "tokens", "encoding"):
        if counts[k]:
            fails.append("FIT: %d %s text(s): %s" % (counts[k], k, report[k][:5]))
    if counts["unfitting"] > fx["max_unfitting"]:
        fails.append("FIT: %d unfitting text(s) > %d: %s" % (counts["unfitting"], fx["max_unfitting"],
                                                             [x["id"] + " " + "; ".join(x["why"]) for x in report["unfitting"][:8]]))
    with open(os.path.join(a.out, "fit_report.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=1)

    # B -- the bank is the TSV's build
    built_ok = False
    if not counts["missing"] and not counts["stale"] and not counts["tokens"] and not counts["encoding"]:
        with tempfile.TemporaryDirectory() as tmp:
            built = os.path.join(tmp, "bank")
            B.write_bank(built, B.bank_sections(english, tsv))   # the pipeline's sections: the game's two files and the port's rows
            built_ok = open(built, "rb").read() == open(bank_path, "rb").read()
    print("arabic_length_judge: B bank: %s" % ("the TSV's build" if built_ok else "NOT the TSV's build"))
    if not built_ok:
        fails.append("BANK: %s is not what %s builds" % (os.path.relpath(bank_path, ROOT), os.path.relpath(tsv_path, ROOT)))

    # C -- the game's shaper agrees with the tools'
    tool = os.path.join(os.path.dirname(os.path.abspath(a.bin)), fx["shaper_tool"])
    agree = 0
    total = 0
    if not os.path.exists(tool):
        fails.append("SHAPER: no %s beside the runner (a build without the text layer's shaper)" % fx["shaper_tool"])
    else:
        out = subprocess.run([tool, "--dump", bank_path], capture_output=True, text=True).stdout.splitlines()
        cpp = {}
        for line in out:
            head, _, body = line.partition("\t")
            cpp[head] = body
        data = open(bank_path, "rb").read()
        section, n = None, 0
        for raw in re.split(rb"\r\n|\r|\n", data):
            if not raw:
                continue
            if raw.startswith(b"@@ "):
                section, n = raw.split(b" ")[1].decode(), 0
                continue
            shaped = S.shape_text(raw)
            rows = []
            for k, line in enumerate(shaped["lines"]):
                rows.append("~" if k in shaped["empty"] else " ".join("%04X" % cp for cp, _, _ in line))
            mine = " | ".join(rows)
            key = "%s %d" % (section, n)
            total += 1
            if cpp.get(key) == mine:
                agree += 1
            elif len(fails) < 40:
                fails.append("SHAPER: %s: the game shapes %s, the tools %s" % (key, cpp.get(key), mine))
            n += 1
    print("arabic_length_judge: C shaper: %d of %d texts shaped alike by the game's shaper and the tools'" % (agree, total))

    # D -- the engine takes the bank on this disc
    mc = os.path.join(a.out, "mc")
    shutil.rmtree(mc, ignore_errors=True)
    os.makedirs(mc)
    log = os.path.join(a.out, "run.log")
    env = dict(os.environ, TE_GFX_HEADLESS="1")
    with open(log, "w") as fh:
        rc = subprocess.call([a.bin, "--iso", a.iso, "--config", "none", "--deterministic", "--no-sdl-input", "--no-audio",
                              "--skip-movies", "--mc-dir", mc, "--stop-at", str(fx["stop_at"]), "--run-seconds", "120"] + fx["runner_args"],
                             stdout=subprocess.DEVNULL, stderr=fh, env=env, cwd=ROOT)
    text = open(log, errors="replace").read()
    row = re.search(r"language (?:packs|mods): (\d+) row\(s\)(.*)", text)
    want = fx["engine_row"]
    if rc != 0 or not row or want not in row.group(2):
        fails.append("ENGINE: the runner (exit %d) did not offer the language: %s" % (rc, row.group(0)[:200] if row else "no language-mods line (a runner without language mods)"))
    print("arabic_length_judge: D engine: exit %d, %s" % (rc, row.group(0)[:160] if row else "no language-mods line"))

    verdict = "GREEN" if not fails else "RED"
    result = {"verdict": verdict, "bin": a.bin, "tsv": os.path.relpath(tsv_path, ROOT), "texts": report["texts"], "ok": report["ok"],
              "counts": counts, "bank_is_build": built_ok, "shaper_agree": [agree, total], "reasons": fails}
    with open(os.path.join(a.out, "report.json"), "w", encoding="utf-8") as fh:
        json.dump(result, fh, ensure_ascii=False, indent=1)
    print(json.dumps({k: v for k, v in result.items() if k != "reasons"}, ensure_ascii=False))
    for f in fails[:20]:
        print("  RED: %s" % f)
    print("arabic_length_judge: %s" % verdict)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
