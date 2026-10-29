#!/usr/bin/env python3
"""translate.py -- a language for the game, from a translation TSV: extract, translate (optionally through an LLM), fit, check,
build the language mod's bank. Runnable for ANY language; the Arabic one is tools/text/arabic.tsv.

    translate.py --iso IMAGE --language arabic                  build mods/arabic/data/arabic.bank from tools/text/arabic.tsv
    translate.py --iso IMAGE --language arabic --check          report missing / stale / unfitting texts (exit 1 if any)
    translate.py --iso IMAGE --language arabic --fit            re-break every translation at spaces to fit its box (TSV rewritten)
    translate.py --iso IMAGE --language arabic --extract DIR    the masked English texts and the prompts, in batches, into DIR
                                                                (OUTSIDE the repository: the English is the game's text)
    translate.py --iso IMAGE --language arabic --replies DIR    merge the answers to those batches (rows: id, sha1, translation:
                                                                any translator's, an LLM's run elsewhere) into the TSV
    translate.py --iso IMAGE --language arabic --llm URL        post the batches with the prompts to an OpenAI-compatible chat
                 [--model NAME] [--api-key-env VAR]             endpoint, merge its rows into the TSV, then the review pass

The TSV (UTF-8): id (code:NNN / agentlab:NNN, the game's text number), sha1 of the English text it translates, the
translation with the control codes as tokens ({CROSS}, {SAVE}, {PARA} ... tools/text/bank.py) and '~' line breaks. The English
is read from your disc every time (the keys are checked against it) and never written into the repository. Building
from a complete TSV needs no LLM.
"""
import argparse
import json
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import bank as B  # noqa: E402
import fit as F  # noqa: E402
from twin_disc import Disc  # noqa: E402

BATCH = 40


def inside_repo(path):
    p = os.path.abspath(path)
    return p == ROOT or p.startswith(ROOT + os.sep)


def load(a):
    disc = Disc(a.iso)
    english, slot = B.english_texts(disc)
    m = F.Metrics(disc, a.font)
    rooms = F.rooms(m, english)
    return disc, english, slot, m, rooms


def rows_of(english):
    for f in B.BANK_FILES:
        for i, t in enumerate(english[f]):
            yield f, i, B.text_id(f, i), t


def check(a, english, m, rooms, tsv, quiet=False):
    """The report: every text's state (ok / missing / stale / tokens / encoding / unfitting)."""
    report = {"language": a.language, "texts": 0, "ok": 0, "missing": [], "stale": [], "tokens": [], "encoding": [], "unfitting": [],
              "fits": {}}
    for f, i, rid, t in rows_of(english):
        report["texts"] += 1
        if rid not in tsv:
            report["missing"].append(rid)
            continue
        sha, text = tsv[rid]
        if sha != B.key(t):
            report["stale"].append(rid)
            continue
        want = B.tokens_of(B.mask(t))
        got = B.tokens_of(text)
        if want != got:
            report["tokens"].append({"id": rid, "english": want, "translation": got})
            continue
        try:
            data = B.unmask(text)
        except UnicodeEncodeError as e:
            report["encoding"].append({"id": rid, "error": str(e)})
            continue
        c = F.context_of(f, i)
        room = rooms[c]
        ok, reasons, widths, lines = F.fit(m, F.as_drawn(c, t), F.as_drawn(c, data), room)
        report["fits"][rid] = {"context": c, "room": round(room, 1), "widths": widths, "lines": lines,
                               "english_lines": max(1, len(F.english_lines(F.as_drawn(c, t)))), "fits": ok}
        if not ok:
            report["unfitting"].append({"id": rid, "why": reasons})
            continue
        report["ok"] += 1
    extra = [rid for rid in tsv if rid not in {r[2] for r in rows_of(english)}]
    report["unknown_ids"] = extra
    if not quiet:
        print("translate: %s: %d texts, %d ok; missing %d, stale %d, token mismatches %d, not encodable %d, unfitting %d, unknown ids %d" % (
            a.language, report["texts"], report["ok"], len(report["missing"]), len(report["stale"]), len(report["tokens"]),
            len(report["encoding"]), len(report["unfitting"]), len(extra)))
        for k in ("missing", "stale", "tokens", "encoding", "unfitting"):
            for x in report[k][:40]:
                print("  %s: %s" % (k, x))
    return report


def extract(a, english, m, rooms):
    if inside_repo(a.extract):
        sys.exit("translate: --extract writes the game's English text: give a folder OUTSIDE the repository")
    os.makedirs(a.extract, exist_ok=True)
    rows = []
    for f, i, rid, t in rows_of(english):
        c = F.context_of(f, i)
        rows.append({"id": rid, "sha1": B.key(t), "context": c, "room": round(rooms[c], 1), "lines": max(1, len(F.english_lines(t))),
                     "english": B.mask(t)})
    with open(os.path.join(a.extract, "english_masked.tsv"), "w", encoding="utf-8") as fh:
        fh.write("# id\tsha1\tcontext\troom\tlines\tenglish (masked)\n")
        for r in rows:
            fh.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (r["id"], r["sha1"], r["context"], r["room"], r["lines"], r["english"]))
    prompt = open(os.path.join(HERE, "prompts", "translate.txt"), encoding="utf-8").read()
    glossary = open(a.glossary, encoding="utf-8").read() if os.path.exists(a.glossary) else ""
    batches = [rows[k:k + BATCH] for k in range(0, len(rows), BATCH)]
    for n, batch in enumerate(batches):
        body = prompt.replace("{{LANGUAGE}}", a.language).replace("{{GLOSSARY}}", glossary)
        body += "\n\n## Batch %d of %d\n\n```\n" % (n + 1, len(batches))
        body += "\n".join("%s\t%s\t%s\t%s" % (r["id"], r["sha1"], r["context"], r["english"]) for r in batch)
        body += "\n```\n"
        with open(os.path.join(a.extract, "batch_%02d.md" % (n + 1)), "w", encoding="utf-8") as fh:
            fh.write(body)
    print("translate: %d texts in %d batches -> %s" % (len(rows), len(batches), a.extract))
    return batches


def ask(a, content):
    req = {"model": a.model, "temperature": 0, "messages": [{"role": "user", "content": content}]}
    headers = {"Content-Type": "application/json"}
    if a.api_key_env and os.environ.get(a.api_key_env):
        headers["Authorization"] = "Bearer " + os.environ[a.api_key_env]
    r = urllib.request.Request(a.llm.rstrip("/") + "/chat/completions", data=json.dumps(req).encode(), headers=headers)
    with urllib.request.urlopen(r, timeout=600) as resp:
        return json.load(resp)["choices"][0]["message"]["content"]


ROW = re.compile(r"^((?:code|agentlab):\d{3})\t([0-9a-f]{40})\t(.+)$")


def llm(a, english, m, rooms, tsv):
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        a.extract = tmp
        batches = extract(a, english, m, rooms)
        for n in range(len(batches)):
            content = open(os.path.join(tmp, "batch_%02d.md" % (n + 1)), encoding="utf-8").read()
            reply = ask(a, content)
            got = 0
            for line in reply.splitlines():
                mm = ROW.match(line.strip())
                if mm:
                    tsv[mm.group(1)] = (mm.group(2), mm.group(3))
                    got += 1
            print("translate: batch %d: %d rows" % (n + 1, got), flush=True)
    # the review pass: consistency with the glossary and the length report, for the rows that do not fit or disagree
    report = check(a, english, m, rooms, tsv, quiet=True)
    bad = [x["id"] for x in report["unfitting"]] + [x["id"] for x in report["tokens"]]
    if bad:
        review = open(os.path.join(HERE, "prompts", "review.txt"), encoding="utf-8").read().replace("{{LANGUAGE}}", a.language)
        glossary = open(a.glossary, encoding="utf-8").read() if os.path.exists(a.glossary) else ""
        review = review.replace("{{GLOSSARY}}", glossary)
        lines = []
        for f, i, rid, t in rows_of(english):
            if rid in bad and rid in tsv:
                fitinfo = report["fits"].get(rid, {})
                lines.append("%s\t%s\t%s\troom %s, lines %s\t%s\t%s" % (rid, tsv[rid][0], F.context_of(f, i), fitinfo.get("room"),
                                                                       fitinfo.get("english_lines"), B.mask(t), tsv[rid][1]))
        reply = ask(a, review + "\n\n```\n" + "\n".join(lines) + "\n```\n")
        for line in reply.splitlines():
            mm = ROW.match(line.strip())
            if mm:
                tsv[mm.group(1)] = (mm.group(2), mm.group(3))
    return tsv


def save_tsv(a, english, tsv):
    rows = [(rid, tsv[rid][0], tsv[rid][1]) for _, _, rid, _ in rows_of(english) if rid in tsv]
    B.write_tsv(a.tsv, rows, ["%s translation of the game's texts: id (the game's text number), sha1 of the English it translates, "
                              "the translation (tokens: tools/text/bank.py; '~' a line break). Made by tools/text/translate.py." % a.language,
                              "id\tsha1(english)\t%s" % a.language])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--language", required=True)
    ap.add_argument("--tsv")
    ap.add_argument("--out")
    ap.add_argument("--font")
    ap.add_argument("--glossary")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--fit", action="store_true")
    ap.add_argument("--extract")
    ap.add_argument("--replies", help="merge reply files (rows: id, sha1, translation) from this folder into the TSV")
    ap.add_argument("--llm")
    ap.add_argument("--model", default="gpt-4o")
    ap.add_argument("--api-key-env", default="OPENAI_API_KEY")
    ap.add_argument("--report", help="write the check's report (JSON) here")
    a = ap.parse_args()
    lang = a.language.lower()
    a.tsv = a.tsv or os.path.join(HERE, "%s.tsv" % lang)
    a.out = a.out or os.path.join(ROOT, "mods", lang, "data", "%s.bank" % lang)
    a.font = a.font or os.path.join(ROOT, "mods", lang, "data", "%s.font" % lang)
    a.glossary = a.glossary or os.path.join(HERE, "glossary_%s.tsv" % lang)
    disc, english, slot, m, rooms = load(a)
    print("translate: the disc's English text is the `%s` slot's (%d + %d texts); rooms %s" % (
        slot, len(english["Code"]), len(english["AgentLab"]), {k: round(v) for k, v in rooms.items()}))
    if a.extract and not a.llm:
        extract(a, english, m, rooms)
        return 0
    tsv = B.read_tsv(a.tsv) if os.path.exists(a.tsv) else {}
    if a.replies:
        # replies to the batches (an LLM's or a person's, made offline from --extract's batches): one row per text
        n = 0
        for name in sorted(os.listdir(a.replies)):
            with open(os.path.join(a.replies, name), encoding="utf-8") as fh:
                for line in fh:
                    mm = ROW.match(line.rstrip("\n").strip("\ufeff"))
                    if mm:
                        tsv[mm.group(1)] = (mm.group(2), mm.group(3))
                        n += 1
        save_tsv(a, english, tsv)
        print("translate: %d rows merged from %s -> %s" % (n, a.replies, a.tsv))
    if a.llm:
        tsv = llm(a, english, m, rooms, tsv)
        save_tsv(a, english, tsv)
    if a.fit:
        n = 0
        for f, i, rid, t in rows_of(english):
            if rid not in tsv or tsv[rid][0] != B.key(t):
                continue
            c = F.context_of(f, i)
            room = rooms[c]
            text = tsv[rid][1]
            if c in F.SPACES_BREAK or not B.unmask(text) or not F.shape.has_arabic(B.unmask(text)):
                continue   # (a save slot's name is broken by the game itself, at its spaces)
            new = F.wrap(m, text, room, max(1, len(F.english_lines(t))))
            if new != text:
                tsv[rid] = (tsv[rid][0], new)
                n += 1
        save_tsv(a, english, tsv)
        print("translate: %d translations re-broken to their boxes -> %s" % (n, a.tsv))
    report = check(a, english, m, rooms, tsv)
    if a.report:
        with open(a.report, "w", encoding="utf-8") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=1)
    problems = len(report["missing"]) + len(report["stale"]) + len(report["tokens"]) + len(report["encoding"]) + len(report["unfitting"])
    if a.check or a.fit:
        return 0 if problems == 0 else 1
    if problems:
        print("translate: not building %s: fix the texts above first" % a.out)
        return 1
    sections = B.bank_sections(english, tsv)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    size = B.write_bank(a.out, sections)
    print("translate: wrote %s (%d bytes, %d + %d texts, %d of the port's)" % (a.out, size, len(english["Code"]), len(english["AgentLab"]),
                                                                             len(english[B.PORT])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
